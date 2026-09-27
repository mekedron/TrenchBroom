/*
 Copyright (C) 2026 Nikita Rabykin

 This file is part of TrenchBroom.

 TrenchBroom is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 TrenchBroom is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with TrenchBroom. If not, see <http://www.gnu.org/licenses/>.
 */

#include "mcp/tools/EntityModelTools.h"

#include "EntityUtils.h"
#include "ToolUtils.h"
#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/JsonVm.h"
#include "mcp/ObjectIds.h"
#include "mcp/Pagination.h"
#include "mcp/Schema.h"
#include "mcp/Targets.h"
#include "mcp/ToolRegistry.h"
#include "mcp/tools/EntityModelUtils.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityModel.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityPropertiesVariableStore.h"
#include "mdl/Map.h"
#include "mdl/Map_Entities.h"
#include "mdl/ModelDefinition.h"
#include "mdl/WorldNode.h"

#include <fmt/format.h>

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tb::mcp
{
namespace
{
using namespace schema;

/** At most this many animation names are listed in a hint. */
constexpr auto MaxHintNames = size_t(20);

std::string joined(const std::vector<std::string>& items)
{
  auto result = std::string{};
  for (const auto& item : items)
  {
    result += (result.empty() ? "" : ", ") + item;
  }
  return result;
}

Json nameJson(const mdl::EntityModelFrame& frame)
{
  return frame.name().empty() ? Json(nullptr) : Json(frame.name());
}

// entity_animation_set

/** How the animation of one entity is set. */
struct AnimationChange
{
  mdl::EntityNode* entityNode;
  std::string id;
  std::string property;
  std::string value;
  size_t index;
};

Result<AnimationChange, ToolError> planAnimationChange(
  mdl::EntityNodeBase& entityNodeBase,
  const Json& animation,
  const IdRegistry& ids,
  EntityModelLoader& loader)
{
  const auto id = ids.format(entityNodeBase);
  auto* entityNode = dynamic_cast<mdl::EntityNode*>(&entityNodeBase);
  if (!entityNode || entityNode->hasChildren())
  {
    return makeError(
      ErrorCode::WrongObjectKind,
      id + " is not a point entity; only point entities have models.",
      "Pass point entity ids; entity_model_info shows their models.",
      {id});
  }

  const auto& entity = entityNode->entity();
  const auto& classname = entity.classname();
  const auto spec = entity.modelSpecification();
  const auto state = resolveEntityModel(entity, loader);
  if (state.is_error())
  {
    if (spec.is_success() && !spec.value().path.empty())
    {
      return makeError(
        ErrorCode::OperationFailed,
        fmt::format(
          "The model {} of {} could not be loaded: {}",
          spec.value().path.generic_string(),
          id,
          errorMessage(state)),
        "Check the game path and mods with game_info; the model file must exist in "
        "the game's folders or packages.",
        {id});
    }
    return makeError(
      ErrorCode::InvalidArgument,
      fmt::format(
        "{} ({}) has no model whose animation could be set: {}",
        id,
        classname,
        errorMessage(state)),
      "Use entity_model_info to see which entities have models.",
      {id});
  }

  const auto property = findFrameProperty(entity);
  if (!property)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      fmt::format(
        "The class '{}' of {} always shows the same frame of {}: its model definition "
        "does not take the frame from a property.",
        classname,
        id,
        state.value().specification.path.generic_string()),
      "Only classes whose model definition takes the frame from a property (e.g. "
      "model({\"path\": ..., \"frame\": sequence})) can change their animation; use "
      "another class.",
      {id});
  }

  const auto& data = state.value().data();
  const auto index = findAnimation(data, animation);
  if (!index)
  {
    const auto names = animationNames(data, MaxHintNames);
    return makeError(
      ErrorCode::InvalidArgument,
      fmt::format(
        "The model {} of {} has no animation {}; it has {} animation{}.",
        state.value().specification.path.generic_string(),
        id,
        animation.dump(),
        data.frameCount(),
        data.frameCount() == 1 ? "" : "s"),
      fmt::format(
        "Valid animations: {}{} (names ignore case), or an index 0..{}. "
        "entity_model_info lists them with their bounds.",
        joined(names),
        data.frameCount() > names.size() ? ", ..." : "",
        data.frameCount() > 0 ? data.frameCount() - 1 : 0),
      {id});
  }

  const auto* pointDefinition = mdl::getPointEntityDefinition(entity.definition());
  const auto variables = mdl::EntityPropertiesVariableStore{entity};
  const auto value =
    framePropertyValue(pointDefinition->modelDefinition, variables, *property, *index);
  if (!value)
  {
    return makeError(
      ErrorCode::OperationFailed,
      fmt::format(
        "No value of property '{}' makes {} show animation {}.", *property, id, *index),
      "Set the property with entity_properties_set and check the result with "
      "entity_model_info.",
      {id});
  }

  return AnimationChange{entityNode, id, *property, *value, *index};
}

ToolResult entityAnimationSet(CallContext& context, const Args& args)
{
  auto& map = context.map();
  const auto& ids = context.ids();

  const auto entities = resolveEntities(map, ids, args);
  if (entities.is_error())
  {
    return errorOf(entities);
  }

  const auto animation = args.get<Json>("animation");
  auto loader = EntityModelLoader{map};

  // check all entities before changing any
  auto changes = std::vector<AnimationChange>{};
  auto previousValues = std::vector<Json>{};
  for (auto* entityNode : entities.value())
  {
    auto change = planAnimationChange(*entityNode, animation, ids, loader);
    if (change.is_error())
    {
      return errorOf(change);
    }
    const auto* previous = entityNode->entity().property(change.value().property);
    previousValues.push_back(previous ? Json(*previous) : Json(nullptr));
    changes.push_back(std::move(change).value());
  }

  // set each distinct property value on all its entities at once
  auto groups = std::map<std::pair<std::string, std::string>, std::vector<size_t>>{};
  for (size_t i = 0; i < changes.size(); ++i)
  {
    groups[{changes[i].property, changes[i].value}].push_back(i);
  }
  for (const auto& [propertyValue, indices] : groups)
  {
    const auto& [property, value] = propertyValue;
    auto groupEntities = std::vector<mdl::EntityNodeBase*>{};
    auto groupIds = std::vector<std::string>{};
    for (const auto i : indices)
    {
      groupEntities.push_back(changes[i].entityNode);
      groupIds.push_back(changes[i].id);
    }

    validateProperty(
      context,
      changes[indices.front()].entityNode->entity().definition(),
      property,
      value,
      groupIds);
    if (auto result = withEntities(
          context,
          groupEntities,
          [&]() -> ToolResult {
            if (!mdl::setEntityProperty(map, property, value))
            {
              return context.operationFailed(
                "Property '" + property + "' could not be set.");
            }
            return Json::object();
          });
        result.is_error())
    {
      return result;
    }
  }

  auto items = Json::array();
  auto nodes = std::vector<mdl::Node*>{};
  for (size_t i = 0; i < changes.size(); ++i)
  {
    const auto& change = changes[i];
    const auto& entity = change.entityNode->entity();
    const auto state = resolveEntityModel(entity, loader);
    const auto* frame = state.is_success() ? state.value().frame() : nullptr;
    const auto bounds = state.is_success() ? state.value().worldBounds() : std::nullopt;
    items.push_back(Json{
      {"id", change.id},
      {"classname", entity.classname()},
      {"property", change.property},
      {"value", change.value},
      {"previousValue", previousValues[i]},
      {"animation",
       Json{
         {"index", change.index},
         {"name", frame ? nameJson(*frame) : Json(nullptr)},
       }},
      {"modelBounds", bounds ? toJson(*bounds) : Json(nullptr)},
    });
    nodes.push_back(change.entityNode);
  }

  warnModelPlacement(context, nodes, loader);
  return Json{{"entities", std::move(items)}};
}

// entity_placement_check

Json placementItem(
  mdl::Map& map,
  const IdRegistry& ids,
  mdl::EntityNodeBase& entityNodeBase,
  EntityModelLoader& loader)
{
  const auto id = ids.format(entityNodeBase);
  const auto& entity = entityNodeBase.entity();
  const auto spec = entity.modelSpecification();
  auto result = Json{
    {"id", id},
    {"classname", entity.classname()},
    {"model",
     spec.is_success() && !spec.value().path.empty()
       ? Json(spec.value().path.generic_string())
       : Json(nullptr)},
    {"checked", false},
    {"animation", nullptr},
    {"modelBounds", nullptr},
    {"surface", nullptr},
    {"findings", Json::array()},
  };

  auto* entityNode = dynamic_cast<mdl::EntityNode*>(&entityNodeBase);
  if (!entityNode || entityNode->hasChildren())
  {
    result["reason"] = "Not a point entity.";
    return result;
  }

  const auto state = resolveEntityModel(entity, loader);
  if (state.is_error())
  {
    result["reason"] = errorMessage(state);
    return result;
  }

  const auto* frame = state.value().frame();
  if (!frame)
  {
    result["reason"] =
      fmt::format("The model has no frame {}.", state.value().specification.frameIndex);
    return result;
  }

  // the same rule as issues_list and map_check: lights, sounds, sprites etc. are skipped
  const auto rule = placementRule(map, entity);
  if (!checksModelPlacement(rule, state.value()))
  {
    result["reason"] =
      "Its model is not checked: the class does not stand on a floor, or the model is "
      "a sprite.";
    return result;
  }

  const auto bounds = state.value().worldBounds(*frame);
  auto check =
    checkModelPlacement(bounds, map, ids, id, placementSubject(id, state.value()));
  applyPlacementRule(check, rule);
  auto findings = Json::array();
  for (const auto& finding : check.findings)
  {
    findings.push_back(placementFindingJson(finding));
  }

  result["checked"] = true;
  result["animation"] = Json{{"index", frame->index()}, {"name", nameJson(*frame)}};
  result["modelBounds"] = toJson(bounds);
  result["surface"] = placementSurfaceJson(check.surface, ids);
  result["findings"] = std::move(findings);
  return result;
}

ToolResult entityPlacementCheck(CallContext& context, const Args& args)
{
  auto& map = context.map();
  const auto& ids = context.ids();
  const auto request = pageRequest(args, map.modificationCount());
  if (request.is_error())
  {
    return errorOf(request);
  }

  const auto scope = args.getOptional<std::string>("scope");
  auto entities = std::vector<mdl::EntityNodeBase*>{};
  if (scope == "map")
  {
    if (args.has("ids"))
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "Pass either ids or scope \"map\", not both.",
        "Omit ids to check all point entities with models.");
    }
    for (auto* entityNode : pointEntities({&map.worldNode()}))
    {
      const auto spec = entityNode->entity().modelSpecification();
      if (spec.is_success() && !spec.value().path.empty())
      {
        entities.push_back(entityNode);
      }
    }
  }
  else
  {
    auto resolved = resolveEntities(map, ids, args);
    if (resolved.is_error())
    {
      return errorOf(resolved);
    }
    entities = std::move(resolved).value();
  }

  const auto onlyProblems = args.get<bool>("onlyProblems");
  auto loader = EntityModelLoader{map};
  auto items = std::vector<Json>{};
  auto checked = size_t(0);
  auto withFindings = size_t(0);
  for (auto* entityNode : entities)
  {
    auto item = placementItem(map, ids, *entityNode, loader);
    const auto hasFindings = !item["findings"].empty();
    checked += item["checked"].get<bool>() ? size_t(1) : size_t(0);
    withFindings += hasFindings ? size_t(1) : size_t(0);
    if (!onlyProblems || hasFindings)
    {
      items.push_back(std::move(item));
    }
  }

  auto result = makePage(items, request.value(), map.modificationCount());
  result["summary"] = Json{
    {"entities", entities.size()},
    {"checked", checked},
    {"notChecked", entities.size() - checked},
    {"withFindings", withFindings},
  };
  return result;
}

} // namespace

void registerEntityModelTools(ToolRegistry& registry)
{
  registry.add(
    ToolDef{"entity_animation_set"}
      .title("Set Entity Animation")
      .description(
        "Sets the animation (model frame, e.g. a Half-Life sequence or a Quake frame) "
        "of point entities in one undo step, by setting the property their model "
        "definition takes the frame from (e.g. \"sequence\"; entity_model_info reports "
        "it as frameProperty and lists the animations with their bounds). Fails with "
        "INVALID_ARGUMENT for unknown animations (the hint lists valid names), for "
        "classes whose model shows a fixed frame, and for entities without model; with "
        "OPERATION_FAILED if a model cannot be loaded. Returns per entity the property, "
        "its new and previous value, the animation {index, name} and the resulting "
        "world model bounds. Placement problems of the new bounds are warnings "
        "(MODEL_BELOW_FLOOR, MODEL_FLOATING, MODEL_PENETRATES_BRUSHES, MODEL_NO_FLOOR; "
        "see entity_placement_check). "
        "Examples: {\"ids\": [\"entity:40\"], \"animation\": \"sitting2\"}; "
        "{\"ids\": [\"entity:40\"], \"animation\": 13}")
      .input(object({
        idsField(
          {ObjectKind::Entity},
          "Point entity ids. Default: the entities of the current selection"),
        field(
          "animation",
          oneOf({
            string().nonEmpty().describe("Animation name, ignoring case"),
            integer().min(0).describe("Animation index (0-based)"),
          }))
          .required()
          .describe("Animation name (ignoring case) or index, e.g. \"idle\" or 3"),
      }))
      .output(object({
        field("entities", array(any()))
          .required()
          .describe("[{id, classname, property, value, previousValue, animation {index, "
                    "name}, modelBounds}]"),
      }))
      .mutation(Mutation::Map)
      .handler(entityAnimationSet));

  registry.add(
    ToolDef{"entity_placement_check"}
      .title("Check Model Placement")
      .description(
        "Checks whether point entities stand correctly with the real bounds of their "
        "model in its current animation (not the class size); read-only. The surface "
        "below is found with vertical rays from the top of the model (brushes and "
        "patches, hidden ones included, triggers and layers omitted from export "
        "ignored). Findings: MODEL_BELOW_FLOOR (the model reaches more than 2 units "
        "below the floor; distance = depth, suggestedMove moves it up), MODEL_FLOATING "
        "(more than 1 unit above the floor; distance = gap), "
        "MODEL_PENETRATES_BRUSHES (the model intersects other solid or brush entity "
        "brushes such as furniture or walls), MODEL_NO_FLOOR. Targets: ids, the "
        "selected entities, or scope \"map\" for all point entities with models. "
        "Entities whose model cannot be loaded are listed with checked false and a "
        "reason. Each item has the model, animation, modelBounds, surface {z, object, "
        "face} and findings [{code, message, objectIds, distance, suggestedMove}]; "
        "summary counts all entities. Fix findings with objects_move (suggestedMove) or "
        "entity_animation_set. "
        "Examples: {\"ids\": [\"entity:40\"]}; {\"scope\": \"map\", "
        "\"onlyProblems\": true}")
      .input(object({
        idsField(
          {ObjectKind::Entity},
          "Point entity ids. Default: the entities of the current selection"),
        field("scope", enumOf({"map"}))
          .describe("\"map\": check all point entities with models instead of ids"),
        field("onlyProblems", boolean().defaultsTo(false))
          .describe("List only entities with findings; summary still counts all"),
      }))
      .output(object({
        field("items", array(any()))
          .required()
          .describe("[{id, classname, model, checked, reason?, animation, modelBounds, "
                    "surface, findings: [{code, message, objectIds, distance?, "
                    "suggestedMove?}]}]"),
        field("total", integer()).required(),
        field("nextCursor", any())
          .required()
          .describe("Cursor of the next page, or null"),
        field("summary", any())
          .required()
          .describe("{entities, checked, notChecked, withFindings}"),
      }))
      .paginated()
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(entityPlacementCheck));
}

} // namespace tb::mcp
