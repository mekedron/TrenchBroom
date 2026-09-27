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

#include "mcp/tools/EntityModelUtils.h"

#include "ToolUtils.h"
#include "base/Logger.h"
#include "el/Value.h"
#include "el/VariableStore.h"
#include "gl/Material.h"
#include "gl/Texture.h"
#include "gl/TextureResource.h"
#include "mcp/CallContext.h"
#include "mcp/JsonVm.h"
#include "mcp/ObjectIds.h"
#include "mcp/tools/AssetUtils.h"
#include "mcp/tools/GeometryUtils.h"
#include "mcp/tools/SpaceAnalysis.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityModel.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/EntityPropertiesVariableStore.h"
#include "mdl/GameConfig.h"
#include "mdl/GameFileSystem.h"
#include "mdl/GameInfo.h"
#include "mdl/LayerNode.h"
#include "mdl/LoadEntityModel.h"
#include "mdl/Map.h"
#include "mdl/ModelDefinition.h"
#include "mdl/Node.h"
#include "mdl/NodeTree.h"
#include "mdl/WorldNode.h"

#include "kd/result.h"
#include "kd/string_compare.h"
#include "kd/string_format.h"

#include "vm/ray.h"
#include "vm/vec.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <initializer_list>
#include <string_view>

namespace tb::mcp
{
namespace
{

/** Shrinks boxes for overlap tests and insets floor rays, like space_check. */
constexpr auto Epsilon = 0.01;

/** At most this many placement warnings are added by one call. */
constexpr auto MaxPlacementWarnings = size_t(20);

/** An origin lies in a liquid if it is at least this deep inside a liquid brush. */
constexpr auto LiquidDepth = 1.0;

bool isOneOf(const std::string_view str, std::initializer_list<std::string_view> list)
{
  return std::ranges::find(list, str) != list.end();
}

bool hasAnyPrefix(
  const std::string_view str, std::initializer_list<std::string_view> prefixes)
{
  return std::ranges::any_of(
    prefixes, [&](const auto prefix) { return str.starts_with(prefix); });
}

/** Classes whose position does not matter (targets of spotlights, compiler settings). */
bool positionIndependent(const std::string_view classname)
{
  return isOneOf(
    classname,
    {"info_null",
     "info_notnull",
     "info_target",
     "info_landmark",
     "info_compile_parameters",
     "info_texlights",
     "light_environment"});
}

/** Monsters that fly or swim. */
bool mayFly(const std::string_view classname)
{
  return isOneOf(
    classname,
    {"monster_wizard",
     "monster_fish",
     "monster_flyer",
     "monster_hover",
     "monster_alien_controller",
     "monster_nihilanth",
     "monster_apache",
     "monster_osprey",
     "monster_ichthyosaur",
     "monster_leech",
     "monster_flyer_flock"});
}

/** Classes that are often placed on walls, ceilings or in the air. */
bool mayHang(const std::string_view classname)
{
  return hasAnyPrefix(
    classname,
    {"light",
     "env_",
     "ambient_",
     "path_",
     "target_",
     "trigger_",
     "misc_",
     "func_",
     "info_",
     "speaker",
     "scripted_",
     "aiscripted_"});
}

bool hasModel(const mdl::Entity& entity)
{
  const auto spec = entity.modelSpecification();
  return spec.is_success() && !spec.value().path.empty();
}

bool standingClass(const mdl::Entity& entity)
{
  const auto& classname = entity.classname();
  if (hasAnyPrefix(classname, {"info_player_", "monster_", "item_", "weapon_", "ammo_"}))
  {
    return true;
  }
  return hasModel(entity) && !mayHang(classname);
}

bool inLiquid(const mdl::Map& map, const vm::vec3d& point)
{
  const auto box = vm::bbox3d{point - vm::vec3d{1, 1, 1}, point + vm::vec3d{1, 1, 1}};
  for (const auto* node : map.worldNode().nodeTree().find_intersectors(box))
  {
    const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(node);
    if (
      brushNode && brushRole(*brushNode).liquid
      && brushNode->brush().bounds().contains(point)
      && std::ranges::all_of(brushNode->brush().faces(), [&](const auto& face) {
           return face.boundary().point_distance(point) <= -LiquidDepth;
         }))
    {
      return true;
    }
  }
  return false;
}

/**
 * Wraps a variable store: records the names of the variables an evaluation reads and
 * optionally overrides one variable. Evaluation clones the store, so the clones share the
 * list of names.
 */
class FrameProbeStore : public el::VariableStore
{
private:
  const el::VariableStore& m_variables;
  std::shared_ptr<std::vector<std::string>> m_reads;
  std::optional<std::pair<std::string, el::Value>> m_override;

public:
  FrameProbeStore(
    const el::VariableStore& variables,
    std::shared_ptr<std::vector<std::string>> reads,
    std::optional<std::pair<std::string, el::Value>> override = std::nullopt)
    : m_variables{variables}
    , m_reads{std::move(reads)}
    , m_override{std::move(override)}
  {
  }

  el::VariableStore* clone() const override { return new FrameProbeStore{*this}; }

  size_t size() const override { return m_variables.size(); }

  el::Value value(const std::string& name) const override
  {
    if (std::ranges::find(*m_reads, name) == m_reads->end())
    {
      m_reads->push_back(name);
    }
    if (m_override && m_override->first == name)
    {
      return m_override->second;
    }
    return m_variables.value(name);
  }

  std::vector<std::string> names() const override { return m_variables.names(); }

  void set(std::string, el::Value) override {}
};

/** The model specification with the given variable set to the given value. */
Result<mdl::ModelSpecification> evaluateWith(
  const mdl::ModelDefinition& definition,
  const el::VariableStore& variables,
  const std::string& name,
  const std::string& value)
{
  const auto probe = FrameProbeStore{
    variables,
    std::make_shared<std::vector<std::string>>(),
    std::pair{name, el::Value{value}}};
  return definition.modelSpecification(probe);
}

bool selectsFrameDirectly(
  const mdl::ModelDefinition& definition,
  const el::VariableStore& variables,
  const std::string& name,
  const std::filesystem::path& path)
{
  return std::ranges::all_of(std::array{1, 2, 5}, [&](const auto frame) {
    const auto spec = evaluateWith(definition, variables, name, std::to_string(frame));
    return spec.is_success() && spec.value().path == path
           && spec.value().frameIndex == size_t(frame);
  });
}

std::string formatNumber(const double value)
{
  return fmt::format("{}", roundForOutput(value));
}

/** Point entities among the nodes and their descendants, without duplicates. */
void collectPointEntities(mdl::Node& node, std::vector<mdl::EntityNode*>& result)
{
  if (auto* entityNode = dynamic_cast<mdl::EntityNode*>(&node))
  {
    if (
      !entityNode->hasChildren() && std::ranges::find(result, entityNode) == result.end())
    {
      result.push_back(entityNode);
    }
    return;
  }
  for (auto* child : node.children())
  {
    collectPointEntities(*child, result);
  }
}

vm::bbox3d definitionBounds(const mdl::EntityDefinition* definition)
{
  return definition && definition->pointEntityDefinition
           ? definition->pointEntityDefinition->bounds
           : mdl::EntityNode::DefaultBounds;
}

} // namespace

std::vector<mdl::EntityNode*> pointEntities(const std::vector<mdl::Node*>& nodes)
{
  auto result = std::vector<mdl::EntityNode*>{};
  for (auto* node : nodes)
  {
    collectPointEntities(*node, result);
  }
  return result;
}

// EntityModelLoader

EntityModelLoader::EntityModelLoader(const mdl::Map& map)
  : m_map{map}
  , m_logger{std::make_unique<NullLogger>()}
{
}

EntityModelLoader::~EntityModelLoader() = default;

const mdl::Map& EntityModelLoader::map() const
{
  return m_map;
}

Result<const mdl::EntityModel*> EntityModelLoader::load(const std::filesystem::path& path)
{
  auto it = m_models.find(path);
  if (it == m_models.end())
  {
    if (!m_fileSystem)
    {
      m_fileSystem = createGameFileSystem(m_map, *m_logger);
    }

    // only the geometry is needed, so skins and shaders are not loaded
    const auto loadMaterial = [](const std::filesystem::path& materialPath) {
      return gl::Material{
        materialPath.string(), gl::createTextureResource(gl::Texture{1, 1})};
    };

    auto entry = Entry{};
    mdl::loadEntityModelSync(
      *m_fileSystem,
      m_map.gameInfo().gameConfig.materialConfig,
      path,
      loadMaterial,
      *m_logger)
      | kdl::transform([&](auto model) {
          entry.model = std::make_unique<mdl::EntityModel>(std::move(model));
        })
      | kdl::transform_error([&](const auto& error) { entry.error = error.msg; });
    if (entry.model && !entry.model->data())
    {
      entry.model.reset();
      entry.error = "The model data could not be loaded.";
    }
    it = m_models.emplace(path, std::move(entry)).first;
  }

  if (it->second.model)
  {
    return static_cast<const mdl::EntityModel*>(it->second.model.get());
  }
  return Error{it->second.error};
}

// EntityModelState

const mdl::EntityModelData& EntityModelState::data() const
{
  return *model->data();
}

const mdl::EntityModelFrame* EntityModelState::frame() const
{
  return data().frame(specification.frameIndex);
}

vm::bbox3d EntityModelState::worldBounds(const mdl::EntityModelFrame& frame) const
{
  return vm::bbox3d(frame.bounds()).transform(transformation);
}

std::optional<vm::bbox3d> EntityModelState::worldBounds() const
{
  const auto* currentFrame = frame();
  return currentFrame ? std::optional{worldBounds(*currentFrame)} : std::nullopt;
}

Result<EntityModelState> resolveEntityModel(
  const mdl::Entity& entity, EntityModelLoader& loader)
{
  if (!mdl::getPointEntityDefinition(entity.definition()))
  {
    return Error{"The entity has no point entity definition, so it has no model."};
  }

  const auto spec = entity.modelSpecification();
  if (spec.is_error())
  {
    return Error{"The model expression could not be evaluated: " + errorMessage(spec)};
  }
  if (spec.value().path.empty())
  {
    return Error{"The entity's class has no model."};
  }

  const auto& scaleExpression =
    loader.map().gameInfo().gameConfig.entityConfig.scaleExpression;
  if (const auto* model = entity.model();
      model && model->dataResource().isLoaded() && model->data())
  {
    return EntityModelState{
      spec.value(), model, true, entity.modelTransformation(scaleExpression)};
  }

  return loader.load(spec.value().path) | kdl::transform([&](const auto* model) {
           // the rotation depends on the model's pitch type
           auto withModel = entity;
           withModel.setModel(model);
           return EntityModelState{
             spec.value(), model, false, withModel.modelTransformation(scaleExpression)};
         });
}

// frame property

std::optional<std::string> findFrameProperty(
  const mdl::ModelDefinition& definition, const el::VariableStore& variables)
{
  auto reads = std::make_shared<std::vector<std::string>>();
  const auto probe = FrameProbeStore{variables, reads};
  const auto spec = definition.modelSpecification(probe);
  if (spec.is_error() || spec.value().path.empty())
  {
    return std::nullopt;
  }

  const auto names = *reads;
  for (const auto& name : names)
  {
    if (selectsFrameDirectly(definition, variables, name, spec.value().path))
    {
      return name;
    }
  }
  return std::nullopt;
}

std::optional<std::string> findFrameProperty(const mdl::Entity& entity)
{
  const auto* pointDefinition = mdl::getPointEntityDefinition(entity.definition());
  if (!pointDefinition)
  {
    return std::nullopt;
  }
  const auto variables = mdl::EntityPropertiesVariableStore{entity};
  return findFrameProperty(pointDefinition->modelDefinition, variables);
}

std::optional<std::string> framePropertyValue(
  const mdl::ModelDefinition& definition,
  const el::VariableStore& variables,
  const std::string& property,
  const size_t frameIndex)
{
  const auto selects = [&](const size_t value) {
    const auto spec =
      evaluateWith(definition, variables, property, std::to_string(value));
    return spec.is_success() && spec.value().frameIndex == frameIndex;
  };

  if (selects(frameIndex))
  {
    return std::to_string(frameIndex);
  }
  for (size_t value = 0; value < 1024; ++value)
  {
    if (selects(value))
    {
      return std::to_string(value);
    }
  }
  return std::nullopt;
}

// animations

std::vector<ModelAnimation> modelAnimations(
  const EntityModelState& state, const size_t limit)
{
  auto result = std::vector<ModelAnimation>{};
  for (const auto& frame : state.data().frames())
  {
    if (result.size() >= limit)
    {
      break;
    }
    result.push_back(ModelAnimation{
      frame.index(),
      frame.name().empty() ? std::nullopt : std::optional{frame.name()},
      vm::bbox3d(frame.bounds()),
      state.worldBounds(frame),
    });
  }
  return result;
}

Json animationJson(const ModelAnimation& animation)
{
  return Json{
    {"index", animation.index},
    {"name", animation.name ? Json(*animation.name) : Json(nullptr)},
    {"bounds", toJson(animation.bounds)},
    {"worldBounds", toJson(animation.worldBounds)},
  };
}

std::optional<size_t> findAnimation(
  const mdl::EntityModelData& data, const Json& animation)
{
  const auto& frames = data.frames();
  if (animation.is_number_integer())
  {
    const auto index = animation.get<int64_t>();
    return index >= 0 && size_t(index) < frames.size() ? std::optional{size_t(index)}
                                                       : std::nullopt;
  }
  if (!animation.is_string())
  {
    return std::nullopt;
  }

  const auto name = animation.get<std::string>();
  const auto it = std::ranges::find_if(
    frames, [&](const auto& frame) { return kdl::ci::str_is_equal(frame.name(), name); });
  if (it != frames.end())
  {
    return it->index();
  }

  if (!name.empty() && name.size() < 10 && std::ranges::all_of(name, [](const auto c) {
        return std::isdigit(static_cast<unsigned char>(c));
      }))
  {
    const auto index = size_t(std::stoul(name));
    return index < frames.size() ? std::optional{index} : std::nullopt;
  }
  return std::nullopt;
}

std::vector<std::string> animationNames(
  const mdl::EntityModelData& data, const size_t limit)
{
  auto result = std::vector<std::string>{};
  for (const auto& frame : data.frames())
  {
    if (result.size() >= limit)
    {
      break;
    }
    result.push_back(frame.name().empty() ? std::to_string(frame.index()) : frame.name());
  }
  return result;
}

Json entityModelAnimationsJson(
  const mdl::Entity& entity, EntityModelLoader& loader, const size_t maxAnimations)
{
  const auto frameProperty = findFrameProperty(entity);
  auto result = Json{
    {"modelLoaded", false},
    {"modelBounds", nullptr},
    {"frameProperty", frameProperty ? Json(*frameProperty) : Json(nullptr)},
    {"currentAnimation", nullptr},
    {"animationCount", nullptr},
    {"animations", Json::array()},
    {"animationsTruncated", false},
  };

  const auto spec = entity.modelSpecification();
  if (spec.is_error() || spec.value().path.empty())
  {
    return result;
  }

  const auto state = resolveEntityModel(entity, loader);
  if (state.is_error())
  {
    result["modelLoadError"] = errorMessage(state);
    return result;
  }

  const auto& model = state.value();
  result["modelLoaded"] = true;
  if (const auto* frame = model.frame())
  {
    result["modelBounds"] = toJson(model.worldBounds(*frame));
    result["currentAnimation"] = animationJson(ModelAnimation{
      frame->index(),
      frame->name().empty() ? std::nullopt : std::optional{frame->name()},
      vm::bbox3d(frame->bounds()),
      model.worldBounds(*frame),
    });
  }

  const auto frameCount = model.data().frameCount();
  auto animations = Json::array();
  for (const auto& animation : modelAnimations(model, maxAnimations))
  {
    animations.push_back(animationJson(animation));
  }
  result["animationCount"] = frameCount;
  result["animations"] = std::move(animations);
  result["animationsTruncated"] = frameCount > maxAnimations;
  return result;
}

// placement checks

PlacementCheck checkModelPlacement(
  const vm::bbox3d& modelBounds,
  mdl::Map& map,
  const IdRegistry& ids,
  const std::string& entityId,
  const std::string& subject,
  const double belowFloorTolerance)
{
  const auto isObstacle = [&](const mdl::Node& node) {
    if (isPointEntity(node) || inOmittedLayer(node))
    {
      return false;
    }
    const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node);
    return !brushNode || classifyBrush(*brushNode) != BrushClass::Trigger;
  };

  auto result = PlacementCheck{modelBounds, std::nullopt, {}};

  // the surface below: rays from the top of the model at its center and inset corners
  const auto& bounds = modelBounds;
  const auto inset = std::min(
    {Epsilon,
     (bounds.max.x() - bounds.min.x()) / 4.0,
     (bounds.max.y() - bounds.min.y()) / 4.0});
  const auto top = std::max(bounds.max.z() - Epsilon, bounds.min.z());
  const auto center = bounds.center();
  const auto samples = std::array{
    vm::vec3d{center.x(), center.y(), top},
    vm::vec3d{bounds.min.x() + inset, bounds.min.y() + inset, top},
    vm::vec3d{bounds.max.x() - inset, bounds.min.y() + inset, top},
    vm::vec3d{bounds.min.x() + inset, bounds.max.y() - inset, top},
    vm::vec3d{bounds.max.x() - inset, bounds.max.y() - inset, top},
  };
  // a surface at the bottom of the model supports it, even if other surfaces lie above
  // it (e.g. a chair seat above the floor a sitting model's feet rest on); otherwise the
  // highest surface below is the floor, as for dropToFloor
  auto support = std::optional<PlacementSurface>{};
  auto highest = std::optional<PlacementSurface>{};
  for (const auto& sample : samples)
  {
    const auto hits = castRay(map, vm::ray3d{sample, vm::vec3d{0, 0, -1}}, isObstacle);
    if (hits.empty())
    {
      continue;
    }
    if (const auto& hit = hits.front(); !highest || hit.point.z() > highest->z)
    {
      highest = PlacementSurface{hit.point.z(), hit.node, hit.faceIndex};
    }
    for (const auto& hit : hits)
    {
      const auto depth = hit.point.z() - bounds.min.z();
      const auto distance = std::abs(depth);
      if (
        depth >= -PlacementTolerance && depth <= belowFloorTolerance
        && (!support || distance < std::abs(support->z - bounds.min.z())))
      {
        support = PlacementSurface{hit.point.z(), hit.node, hit.faceIndex};
      }
    }
  }
  result.surface = support ? support : highest;

  // brushes whose interior intersects the model, except the floor; overlaps up to the
  // tolerance are fine
  auto inner = bounds;
  for (size_t i = 0; i < 3; ++i)
  {
    const auto shrink =
      std::min(PlacementTolerance, (bounds.max[i] - bounds.min[i]) / 4.0);
    inner.min[i] += shrink;
    inner.max[i] -= shrink;
  }
  auto penetrated = std::vector<std::string>{};
  for (auto* node : map.worldNode().nodeTree().find_intersectors(inner))
  {
    const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(node);
    if (
      brushNode && (!result.surface || result.surface->node != brushNode)
      && isObstacle(*brushNode) && intersectsInterior(brushNode->brush(), inner))
    {
      penetrated.push_back(ids.format(*brushNode));
    }
  }
  std::ranges::sort(penetrated);

  if (result.surface)
  {
    const auto floorId = ids.format(*result.surface->node);
    const auto floorZ = formatNumber(result.surface->z);
    const auto depth = result.surface->z - bounds.min.z();
    if (depth > belowFloorTolerance)
    {
      const auto amount = formatNumber(depth);
      result.findings.push_back(PlacementFinding{
        "MODEL_BELOW_FLOOR",
        fmt::format(
          "{} reaches {} units below the floor at z={} ({}). Move it up by {} "
          "(objects_move with vector [0, 0, {}]), drop it onto the floor "
          "(entity_create_point with dropToFloor), or choose another animation "
          "(entity_animation_set).",
          subject,
          amount,
          floorZ,
          floorId,
          amount,
          amount),
        {entityId, floorId},
        roundForOutput(depth),
        vm::vec3d{0, 0, roundForOutput(depth)},
      });
    }
    else if (-depth > PlacementTolerance)
    {
      const auto amount = formatNumber(-depth);
      result.findings.push_back(PlacementFinding{
        "MODEL_FLOATING",
        fmt::format(
          "{} floats {} units above the floor at z={} ({}). Unless it is meant to fly "
          "or hang, move it down by {} (objects_move with vector [0, 0, -{}]) or drop "
          "it onto the floor (entity_create_point with dropToFloor).",
          subject,
          amount,
          floorZ,
          floorId,
          amount,
          amount),
        {entityId, floorId},
        roundForOutput(-depth),
        vm::vec3d{0, 0, -roundForOutput(-depth)},
      });
    }
  }
  else
  {
    result.findings.push_back(PlacementFinding{
      "MODEL_NO_FLOOR",
      subject
        + " has no floor below it. Unless it is meant to fly, move it above a floor "
          "(find one with ray_pick, direction [0, 0, -1]).",
      {entityId},
      std::nullopt,
      std::nullopt,
    });
  }

  if (!penetrated.empty())
  {
    auto list = std::string{};
    for (const auto& id : penetrated)
    {
      list += (list.empty() ? "" : ", ") + id;
    }
    auto objectIds = penetrated;
    objectIds.insert(objectIds.begin(), entityId);
    result.findings.push_back(PlacementFinding{
      "MODEL_PENETRATES_BRUSHES",
      subject + " intersects the brushes " + list
        + " (e.g. furniture, walls or a ceiling). If it is meant to sit on or lean "
          "against them, check it in a snapshot (view_snapshot); otherwise move it out "
          "of them, or choose another animation whose model fits "
          "(entity_animation_set).",
      std::move(objectIds),
      std::nullopt,
      std::nullopt,
    });
  }

  return result;
}

PlacementRule placementRule(const mdl::Map& map, const mdl::Entity& entity)
{
  const auto& classname = entity.classname();
  auto rule = PlacementRule{};
  rule.positionIndependent = positionIndependent(classname);
  rule.standing = !rule.positionIndependent && standingClass(entity);
  rule.mayFloat = rule.standing && (mayFly(classname) || inLiquid(map, entity.origin()));
  return rule;
}

bool checksModelPlacement(const PlacementRule& rule, const EntityModelState& state)
{
  if (!rule.standing)
  {
    return false;
  }
  const auto extension = kdl::str_to_lower(state.specification.path.extension().string());
  return extension != ".spr" && extension != ".sp2";
}

void applyPlacementRule(PlacementCheck& check, const PlacementRule& rule)
{
  if (rule.mayFloat)
  {
    std::erase_if(check.findings, [](const auto& finding) {
      return finding.code == "MODEL_FLOATING" || finding.code == "MODEL_NO_FLOOR";
    });
  }
}

std::string placementSubject(const std::string& entityId, const EntityModelState& state)
{
  const auto* frame = state.frame();
  if (!frame)
  {
    return "The model of " + entityId;
  }
  return frame->name().empty()
           ? fmt::format("The model of {} (frame {})", entityId, frame->index())
           : fmt::format("The model of {} (animation '{}')", entityId, frame->name());
}

Json placementFindingJson(const PlacementFinding& finding)
{
  auto result = Json{
    {"code", finding.code},
    {"message", finding.message},
    {"objectIds", finding.objectIds},
  };
  if (finding.distance)
  {
    result["distance"] = *finding.distance;
  }
  if (finding.suggestedMove)
  {
    result["suggestedMove"] = toJson(*finding.suggestedMove);
  }
  return result;
}

Json placementSurfaceJson(
  const std::optional<PlacementSurface>& surface, const IdRegistry& ids)
{
  if (!surface)
  {
    return nullptr;
  }
  auto result = Json{
    {"z", roundForOutput(surface->z)},
    {"object", ids.format(*surface->node)},
    {"face", nullptr},
  };
  if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(surface->node);
      brushNode && surface->faceIndex)
  {
    result["face"] = ids.formatFace(*brushNode, *surface->faceIndex);
  }
  return result;
}

void warnModelPlacement(
  CallContext& context, const std::vector<mdl::Node*>& nodes, EntityModelLoader& loader)
{
  const auto entityNodes = pointEntities(nodes);
  auto& map = context.map();
  const auto& ids = context.ids();
  auto warned = size_t(0);
  auto omitted = size_t(0);
  for (const auto* entityNode : entityNodes)
  {
    const auto rule = placementRule(map, entityNode->entity());
    if (!rule.standing)
    {
      continue;
    }
    const auto state = resolveEntityModel(entityNode->entity(), loader);
    if (state.is_error() || !checksModelPlacement(rule, state.value()))
    {
      continue;
    }
    const auto bounds = state.value().worldBounds();
    if (!bounds)
    {
      continue;
    }

    const auto id = ids.format(*entityNode);
    auto check =
      checkModelPlacement(*bounds, map, ids, id, placementSubject(id, state.value()));
    applyPlacementRule(check, rule);
    for (const auto& finding : check.findings)
    {
      if (warned < MaxPlacementWarnings)
      {
        context.warn(finding.code, finding.message, finding.objectIds);
        ++warned;
      }
      else
      {
        ++omitted;
      }
    }
  }

  if (omitted > 0)
  {
    context.warn(
      "MORE_PLACEMENT_FINDINGS",
      fmt::format(
        "{} more model placement findings are not listed; use entity_placement_check "
        "to list them all.",
        omitted));
  }
}

void warnModelPlacement(CallContext& context, const std::vector<mdl::Node*>& nodes)
{
  auto loader = EntityModelLoader{context.map()};
  warnModelPlacement(context, nodes, loader);
}

Result<DropBounds, ToolError> dropToFloorBounds(
  const mdl::EntityDefinition* definition,
  const std::string& classname,
  const std::vector<std::pair<std::string, std::string>>& properties,
  const bool applyDefaults,
  const std::string_view dropUsing,
  EntityModelLoader& loader)
{
  const auto fallback = DropBounds{definitionBounds(definition), false};
  if (dropUsing == "definition")
  {
    return fallback;
  }

  auto entity = mdl::Entity{};
  entity.addOrUpdateProperty(mdl::EntityPropertyKeys::Classname, classname);
  for (const auto& [key, value] : properties)
  {
    entity.addOrUpdateProperty(key, value);
  }
  if (definition)
  {
    if (applyDefaults)
    {
      mdl::setDefaultProperties(
        *definition, entity, mdl::SetDefaultPropertyMode::SetMissing);
    }
    entity.setDefinition(definition);
  }
  // the bounds relative to the origin
  entity.removeProperty(mdl::EntityPropertyKeys::Origin);

  const auto state = resolveEntityModel(entity, loader);
  auto reason = std::string{};
  if (state.is_success())
  {
    if (const auto bounds = state.value().worldBounds())
    {
      return DropBounds{*bounds, true};
    }
    reason =
      fmt::format("its model has no frame {}", state.value().specification.frameIndex);
  }
  else
  {
    reason = errorMessage(state);
  }

  if (dropUsing == "model")
  {
    return makeError(
      ErrorCode::InvalidArgument,
      fmt::format(
        "dropUsing \"model\" needs the model of '{}', but it cannot be used: {}",
        classname,
        reason),
      "Use dropUsing \"auto\" or \"definition\", or check the game path and mods "
      "(game_info) if the model file should exist.");
  }
  return fallback;
}

} // namespace tb::mcp
