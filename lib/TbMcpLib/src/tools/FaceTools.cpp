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

#include "mcp/tools/FaceTools.h"

#include "NodeJson.h"
#include "base/Color.h"
#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/JsonVm.h"
#include "mcp/ObjectIds.h"
#include "mcp/Pagination.h"
#include "mcp/Schema.h"
#include "mcp/Targets.h"
#include "mcp/ToolRegistry.h"
#include "mcp/tools/GeometryUtils.h"
#include "mcp/tools/UvTools.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/Grid.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/Map_Brushes.h"
#include "mdl/Map_Selection.h"
#include "mdl/NodeQueries.h"
#include "mdl/UpdateBrushFaceAttributes.h"
#include "mdl/UvAlignment.h"
#include "mdl/UvAttributes.h"
#include "mdl/UvCoordSystem.h"
#include "mdl/WorldNode.h"

#include "kd/string_compare.h"

#include "vm/vec.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tb::mcp
{
namespace
{

using namespace schema;

/** At most this many faces are described in the result of a modifying face tool. */
constexpr size_t MaxFacesInResult = 50;

/** Rotations that differ by less than this many degrees are equal. */
constexpr double RotationEpsilon = 0.001;

const mdl::FaceAttribsConfig& faceAttribsConfig(const mdl::Map& map)
{
  return map.gameInfo().gameConfig.faceAttribsConfig;
}

mdl::MapFormat mapFormat(const mdl::Map& map)
{
  return map.worldNode().mapFormat();
}

/** Whether the map format writes surface flags, content flags and the surface value. */
bool storesSurfaceAttributes(const mdl::MapFormat format)
{
  switch (format)
  {
  case mdl::MapFormat::Quake2:
  case mdl::MapFormat::Quake2_Valve:
  case mdl::MapFormat::Quake3:
  case mdl::MapFormat::Quake3_Legacy:
  case mdl::MapFormat::Quake3_Valve:
  case mdl::MapFormat::Daikatana:
    return true;
  case mdl::MapFormat::Unknown:
  case mdl::MapFormat::Standard:
  case mdl::MapFormat::Valve:
  case mdl::MapFormat::Hexen2:
    return false;
  }
  return false;
}

/** Whether the map format writes face colors. */
bool storesColor(const mdl::MapFormat format)
{
  return format == mdl::MapFormat::Daikatana;
}

std::string uvFormatName(const mdl::MapFormat format)
{
  return mdl::isParallelUvCoordSystem(format) ? "valve220" : "standard";
}

std::string joined(const std::vector<std::string>& strings)
{
  auto result = std::string{};
  for (const auto& string : strings)
  {
    result += (result.empty() ? "" : ", ") + string;
  }
  return result;
}

std::vector<std::string> flagNamesOf(const mdl::FlagsConfig& config)
{
  auto result = std::vector<std::string>{};
  for (const auto& flag : config.flags)
  {
    result.push_back(flag.name);
  }
  return result;
}

std::string faceId(const IdRegistry& ids, const mdl::BrushFaceHandle& handle)
{
  return ids.formatFace(*handle.node(), handle.faceIndex());
}

std::vector<std::string> faceIds(
  const IdRegistry& ids, const std::vector<mdl::BrushFaceHandle>& faces)
{
  auto result = std::vector<std::string>{};
  result.reserve(faces.size());
  for (const auto& handle : faces)
  {
    result.push_back(faceId(ids, handle));
  }
  return result;
}

/**
 * `{"bits", "names", "unknownBits", "fromMaterial"}`: the set flags, the names the game
 * config gives them, set bits the game config does not name, and whether the value is
 * the material's default (not set on the face).
 */
Json flagsJson(const mdl::FlagsConfig& config, const int bits, const bool fromMaterial)
{
  auto names = Json::array();
  auto known = 0;
  for (const auto& flag : config.flags)
  {
    known |= flag.value;
    if (flag.value != 0 && (bits & flag.value) == flag.value)
    {
      names.push_back(flag.name);
    }
  }

  auto unknownBits = Json::array();
  for (int bit = 0; bit < 31; ++bit)
  {
    const auto value = 1 << bit;
    if ((bits & value) != 0 && (known & value) == 0)
    {
      unknownBits.push_back(value);
    }
  }
  if (bits < 0 && (known & std::numeric_limits<int>::min()) == 0)
  {
    unknownBits.push_back(std::numeric_limits<int>::min());
  }

  return Json{
    {"bits", bits},
    {"names", std::move(names)},
    {"unknownBits", std::move(unknownBits)},
    {"fromMaterial", fromMaterial},
  };
}

Json colorJson(const Color& color)
{
  const auto rgb = color.to<RgbB>();
  return Json{
    int(rgb.get<ColorChannel::r>()),
    int(rgb.get<ColorChannel::g>()),
    int(rgb.get<ColorChannel::b>()),
  };
}

/** Describes the attributes of a face (the items of face_attributes_get). */
Json faceAttributesJson(
  const mdl::Map& map, const mdl::BrushFaceHandle& handle, const IdRegistry& ids)
{
  const auto& face = handle.face();
  const auto uv = face.uvAttributes();
  const auto& surface = face.surfaceAttributes();
  const auto& config = faceAttribsConfig(map);

  auto result = Json{
    {"id", faceId(ids, handle)},
    {"brush", ids.format(*handle.node())},
    {"material", face.materialName()},
    {"materialSize",
     face.material() ? toJson(vm::vec2d{face.textureSize()}) : Json(nullptr)},
    {"offset", toJson(vm::vec2d{uv.offset})},
    {"scale", toJson(vm::vec2d{uv.scale})},
    {"rotation", roundForOutput(faceRotation(face))},
    {"uAxis", toJson(face.uAxis())},
    {"vAxis", toJson(face.vAxis())},
    {"tags", faceTagNames(map, face)},
  };

  if (storesSurfaceAttributes(mapFormat(map)) || !surface.empty())
  {
    result["surfaceFlags"] = flagsJson(
      config.surfaceFlags, face.resolvedSurfaceFlags(), !surface.flags.has_value());
    result["contentFlags"] = flagsJson(
      config.contentFlags, face.resolvedSurfaceContents(), !surface.contents.has_value());
    result["surfaceValue"] = roundForOutput(double(face.resolvedSurfaceValue()));
    if (!surface.value)
    {
      result["surfaceValueFromMaterial"] = true;
    }
  }
  if (surface.color)
  {
    result["color"] = colorJson(*surface.color);
  }
  return result;
}

Json flagDefinitionsJson(const mdl::FlagsConfig& config)
{
  auto result = Json::array();
  for (const auto& flag : config.flags)
  {
    result.push_back(Json{{"name", flag.name}, {"value", flag.value}});
  }
  return result;
}

/** What the map format stores, and the flags the game defines. */
Json formatJson(const mdl::Map& map)
{
  const auto format = mapFormat(map);
  const auto& config = faceAttribsConfig(map);
  return Json{
    {"mapFormat", mdl::formatName(format)},
    {"uvFormat", uvFormatName(format)},
    {"storesSurfaceAttributes", storesSurfaceAttributes(format)},
    {"storesColor", storesColor(format)},
    {"surfaceFlags", flagDefinitionsJson(config.surfaceFlags)},
    {"contentFlags", flagDefinitionsJson(config.contentFlags)},
  };
}

/** The result of a modifying face tool: the count and the first faces' attributes. */
Json facesResult(CallContext& context, const std::vector<mdl::BrushFaceHandle>& faces)
{
  auto items = Json::array();
  for (size_t i = 0; i < faces.size() && i < MaxFacesInResult; ++i)
  {
    items.push_back(faceAttributesJson(context.map(), faces[i], context.ids()));
  }
  return Json{
    {"count", faces.size()},
    {"faces", std::move(items)},
    {"truncated", faces.size() > MaxFacesInResult},
  };
}

Schema facesOutput(std::vector<Field> fields = {})
{
  fields.push_back(field("count", integer()).required().describe("Faces changed"));
  fields.push_back(
    field("faces", array(any()))
      .required()
      .describe("Attributes of the first 50 faces afterwards (the items of "
                "face_attributes_get)"));
  fields.push_back(field("truncated", boolean())
                     .required()
                     .describe("More than 50 faces changed; faces lists the first 50"));
  return object(std::move(fields));
}

ToolResult setAttributes(
  CallContext& context,
  const std::vector<mdl::BrushFaceHandle>& faces,
  const std::vector<mdl::UpdateBrushFaceAttributes>& updates)
{
  return withFaces(context, faces, [&]() -> ToolResult {
    for (const auto& update : updates)
    {
      if (!mdl::setBrushFaceAttributes(context.map(), update))
      {
        return context.operationFailed(
          "The face attributes could not be changed.",
          "Check the values, e.g. a scale must not be 0.");
      }
    }
    return Json::object();
  });
}

/**
 * Applies an update computed per face. Faces with equal updates are changed together;
 * faces for which the function returns nothing are left unchanged.
 */
ToolResult applyPerFace(
  CallContext& context,
  const std::vector<mdl::BrushFaceHandle>& faces,
  const std::function<
    std::optional<mdl::UpdateBrushFaceAttributes>(const mdl::BrushFace&)>& updateFor)
{
  auto groups = std::vector<
    std::pair<mdl::UpdateBrushFaceAttributes, std::vector<mdl::BrushFaceHandle>>>{};
  for (const auto& handle : faces)
  {
    if (auto update = updateFor(handle.face()))
    {
      auto it = std::ranges::find_if(
        groups, [&](const auto& group) { return group.first == *update; });
      if (it == groups.end())
      {
        groups.emplace_back(std::move(*update), std::vector{handle});
      }
      else
      {
        it->second.push_back(handle);
      }
    }
  }

  for (const auto& [update, groupFaces] : groups)
  {
    if (auto result = setAttributes(context, groupFaces, {update}); result.is_error())
    {
      return result;
    }
  }
  return Json::object();
}

/** Warns with IGNORED_ARGUMENT about arguments that do not apply. */
void warnIgnored(
  CallContext& context,
  const Args& args,
  const std::vector<std::string_view>& keys,
  const std::string& reason)
{
  for (const auto key : keys)
  {
    if (args.has(key))
    {
      context.warn(
        "IGNORED_ARGUMENT", "'" + std::string{key} + "' has no effect " + reason + ".");
    }
  }
}

ToolError invalidArgument(std::string message, std::string hint = {})
{
  return makeError(ErrorCode::InvalidArgument, std::move(message), std::move(hint));
}

/** The minimum and maximum UV coordinates of the face's vertices at offset 0. */
std::pair<vm::vec2f, vm::vec2f> uvExtent(
  const mdl::BrushFace& face, const vm::vec2f& scale)
{
  const auto toUv = face.toUvCoordSystemMatrix(vm::vec2f{0, 0}, scale);
  auto min = vm::vec2f::fill(std::numeric_limits<float>::max());
  auto max = vm::vec2f::fill(std::numeric_limits<float>::lowest());
  for (const auto* vertex : face.vertices())
  {
    const auto uv = vm::vec2f{toUv * vertex->position()};
    min = vm::min(min, uv);
    max = vm::max(max, uv);
  }
  return {min, max};
}

/** Faces whose material is not loaded; their texture size is unknown. */
std::vector<mdl::BrushFaceHandle> removeFacesWithoutMaterial(
  CallContext& context, std::vector<mdl::BrushFaceHandle>& faces, const std::string& what)
{
  auto missing = std::vector<mdl::BrushFaceHandle>{};
  std::erase_if(faces, [&](const auto& handle) {
    if (handle.face().material())
    {
      return false;
    }
    missing.push_back(handle);
    return true;
  });

  if (!missing.empty())
  {
    context.warn(
      "MATERIAL_NOT_LOADED",
      std::to_string(missing.size()) + " face(s) were skipped: " + what
        + " needs the texture size, but their material is not loaded.",
      faceIds(context.ids(), missing));
  }
  return missing;
}

// face_attributes_get

/**
 * The faces named by explicit ids, including hidden and locked ones (reading does not
 * need editable faces); without ids, the faces of the selection.
 */
Result<std::vector<mdl::BrushFaceHandle>, ToolError> resolveFacesToRead(
  CallContext& context, const Args& args)
{
  const auto explicitIds = args.getOptional<std::vector<std::string>>("ids");
  if (!explicitIds)
  {
    return resolveFaceTargets(context, args);
  }

  auto faces = std::vector<mdl::BrushFaceHandle>{};
  const auto addFace = [&](const mdl::BrushFaceHandle& handle) {
    if (std::ranges::find(faces, handle) == faces.end())
    {
      faces.push_back(handle);
    }
  };

  for (const auto& id : *explicitIds)
  {
    const auto ref = parseObjectRef(id);
    if (!ref)
    {
      return invalidArgument(
        "'" + id + "' is not a valid id.",
        "Use face ids such as 'brush:12/face:3' or object ids such as 'brush:12', "
        "'group:3' or 'entity:7'.");
    }
    if (ref->faceIndex)
    {
      auto handle = resolveFace(context, id);
      if (handle.is_error())
      {
        return errorOf(handle);
      }
      addFace(handle.value());
      continue;
    }

    auto node = context.ids().resolve(*ref);
    if (node.is_error())
    {
      return errorOf(node);
    }
    const auto nodeFaces = mdl::collectBrushFaces(std::vector{node.value()});
    if (nodeFaces.empty())
    {
      return makeError(
        ErrorCode::WrongObjectKind,
        "Object " + id + " contains no brushes.",
        "Pass face ids, brush ids, or groups and entities that contain brushes.",
        {id});
    }
    for (const auto& handle : nodeFaces)
    {
      addFace(handle);
    }
  }
  return faces;
}

ToolResult faceAttributesGet(CallContext& context, const Args& args)
{
  auto& map = context.map();
  const auto request = pageRequest(args, map.modificationCount());
  if (request.is_error())
  {
    return errorOf(request);
  }

  auto faces = resolveFacesToRead(context, args);
  if (faces.is_error())
  {
    return errorOf(faces);
  }

  const auto detail = request.value().detail;
  auto items = std::vector<Json>{};
  items.reserve(faces.value().size());
  for (const auto& handle : faces.value())
  {
    auto item = faceAttributesJson(map, handle, context.ids());
    if (detail == Detail::Full)
    {
      const auto full =
        faceJson(map, *handle.node(), handle.faceIndex(), context.ids(), Detail::Full);
      for (const auto* key : {"normal", "center", "area", "vertices"})
      {
        item[key] = full[key];
      }
    }
    items.push_back(std::move(item));
  }

  auto result = makePage(items, request.value(), map.modificationCount());
  result["format"] = formatJson(map);
  return result;
}

// face_attributes_set

/** A flag given by name (looked up in the game config) or as a raw bit value. */
Schema flagItemSchema()
{
  return oneOf({string().nonEmpty(), integer().min(0)})
    .describe("A flag name from the game config (case-insensitive) or raw bits");
}

Schema flagsChangeSchema(const std::string& what)
{
  return object({
           field("set", array(flagItemSchema()))
             .describe("Replace all " + what + " with these ([] clears them)"),
           field("add", array(flagItemSchema())).describe("Set these flags"),
           field("remove", array(flagItemSchema())).describe("Clear these flags"),
         })
    .describe(
      "Changes the " + what
      + ": either 'set', or 'add' and/or 'remove'. Use 'unset' to go back to the "
        "material's defaults");
}

/**
 * Resolves flag names and raw bits to a mask. Unknown names are skipped with an
 * UNKNOWN_FLAG warning.
 */
int flagMask(
  CallContext& context,
  const Json& items,
  const mdl::FlagsConfig& config,
  const std::string& what)
{
  auto mask = 0;
  auto unknown = std::vector<std::string>{};
  for (const auto& item : items)
  {
    if (item.is_number_integer())
    {
      mask |= item.get<int>();
      continue;
    }

    const auto name = item.get<std::string>();
    const auto it = std::ranges::find_if(config.flags, [&](const auto& flag) {
      return kdl::ci::str_is_equal(flag.name, name);
    });
    if (it == config.flags.end())
    {
      unknown.push_back(name);
    }
    else
    {
      mask |= it->value;
    }
  }

  if (!unknown.empty())
  {
    const auto known = flagNamesOf(config);
    context.warn(
      "UNKNOWN_FLAG",
      "Unknown " + what + " " + joined(unknown) + " skipped; "
        + (known.empty() ? "the game defines no " + what + ", pass raw bits instead." : "the game defines: " + joined(known) + "."));
  }
  return mask;
}

/**
 * Converts a flags change argument to flag operations (at most two: add, then remove).
 */
Result<std::vector<mdl::FlagOp>, ToolError> flagOps(
  CallContext& context,
  const Json& change,
  const mdl::FlagsConfig& config,
  const std::string& key,
  const std::string& what)
{
  const auto has = [&](const char* name) { return change.contains(name); };
  if (has("set") && (has("add") || has("remove")))
  {
    return invalidArgument(
      "'" + key + "' combines 'set' with 'add' or 'remove'.",
      "Use 'set' to replace all flags, or 'add' / 'remove' to change some.");
  }
  if (!has("set") && !has("add") && !has("remove"))
  {
    return invalidArgument(
      "'" + key + "' names no change.",
      "Pass e.g. {\"add\": [\"light\"]} or {\"set\": []}.");
  }

  auto result = std::vector<mdl::FlagOp>{};
  if (has("set"))
  {
    result.emplace_back(mdl::SetFlags{flagMask(context, change["set"], config, what)});
    return result;
  }

  const auto add = has("add") ? flagMask(context, change["add"], config, what) : 0;
  const auto remove =
    has("remove") ? flagMask(context, change["remove"], config, what) : 0;
  if ((add & remove) != 0)
  {
    return invalidArgument(
      "'" + key + "' both adds and removes the same flag.",
      "Name each flag in either 'add' or 'remove'.");
  }
  if (add != 0)
  {
    result.emplace_back(mdl::SetFlagBits{add});
  }
  if (remove != 0)
  {
    result.emplace_back(mdl::ClearFlagBits{remove});
  }
  return result;
}

bool contains(const std::vector<std::string>& strings, const std::string_view string)
{
  return std::ranges::find(strings, string) != strings.end();
}

ToolResult faceAttributesSet(CallContext& context, const Args& args)
{
  auto& map = context.map();
  const auto& config = faceAttribsConfig(map);
  const auto format = mapFormat(map);
  const auto unset = args.getOr<std::vector<std::string>>("unset", {});

  for (const auto& [absolute, relative] :
       {std::pair{"offset", "offsetBy"},
        std::pair{"scale", "scaleBy"},
        std::pair{"rotation", "rotateBy"}})
  {
    if (args.has(absolute) && args.has(relative))
    {
      return invalidArgument(
        "'" + std::string{absolute} + "' and '" + relative + "' cannot be combined.",
        "Pass the absolute value or the relative change.");
    }
  }
  for (const auto* key : {"surfaceFlags", "contentFlags", "surfaceValue", "color"})
  {
    if (args.has(key) && contains(unset, key))
    {
      return invalidArgument(
        "'" + std::string{key} + "' is both set and unset.",
        "Remove it from 'unset' or omit the value.");
    }
  }
  for (const auto* key : {"scale", "scaleBy"})
  {
    if (const auto value = args.getOptional<vm::vec2d>(key))
    {
      if (value->x() == 0.0 || value->y() == 0.0)
      {
        return invalidArgument(
          "'" + std::string{key} + "' must not contain 0.",
          "Use a negative value to flip the texture.");
      }
    }
  }

  auto update = mdl::UpdateBrushFaceAttributes{};
  auto updates = std::vector<mdl::UpdateBrushFaceAttributes>{};
  auto hasChange = false;

  if (args.has("material"))
  {
    update.materialName = materialArgument(context, args);
    hasChange = true;
  }
  if (const auto offset = args.getOptional<vm::vec2d>("offset"))
  {
    update.xOffset = mdl::SetValue{float(offset->x())};
    update.yOffset = mdl::SetValue{float(offset->y())};
    hasChange = true;
  }
  if (const auto offset = args.getOptional<vm::vec2d>("offsetBy"))
  {
    update.xOffset = mdl::AddValue{float(offset->x())};
    update.yOffset = mdl::AddValue{float(offset->y())};
    hasChange = true;
  }
  if (const auto scale = args.getOptional<vm::vec2d>("scale"))
  {
    update.xScale = mdl::SetValue{float(scale->x())};
    update.yScale = mdl::SetValue{float(scale->y())};
    hasChange = true;
  }
  if (const auto factors = args.getOptional<vm::vec2d>("scaleBy"))
  {
    update.xScale = mdl::MultiplyValue{float(factors->x())};
    update.yScale = mdl::MultiplyValue{float(factors->y())};
    hasChange = true;
  }
  // Valve faces rotate by the difference to their stored rotation, which can differ
  // from the rotation they show (see faceRotation), so they are rotated per face.
  auto parallelRotation = std::optional<double>{};
  if (const auto rotation = args.getOptional<double>("rotation"))
  {
    if (mdl::isParallelUvCoordSystem(format))
    {
      parallelRotation = *rotation;
    }
    else
    {
      update.rotation = mdl::SetValue{float(*rotation)};
    }
    hasChange = true;
  }
  if (const auto angle = args.getOptional<double>("rotateBy"))
  {
    update.rotation = mdl::AddValue{float(*angle)};
    hasChange = true;
  }
  if (const auto value = args.getOptional<double>("surfaceValue"))
  {
    update.surfaceValue = mdl::SetValue{float(*value)};
    hasChange = true;
  }
  if (const auto color = args.getOptional<std::vector<int>>("color"))
  {
    update.color = std::optional<Color>{
      RgbB{uint8_t((*color)[0]), uint8_t((*color)[1]), uint8_t((*color)[2])}};
    hasChange = true;
  }

  // flag changes: the first operation of each kind goes into the main update, a second
  // one (remove after add) into an extra update
  auto extra = mdl::UpdateBrushFaceAttributes{};
  auto hasExtra = false;
  for (const auto& [key, flagsConfig, what, member] :
       {std::tuple{
          "surfaceFlags",
          &config.surfaceFlags,
          "surface flags",
          &mdl::UpdateBrushFaceAttributes::surfaceFlags},
        std::tuple{
          "contentFlags",
          &config.contentFlags,
          "content flags",
          &mdl::UpdateBrushFaceAttributes::surfaceContents}})
  {
    if (const auto change = args.getOptional<Json>(key))
    {
      auto ops = flagOps(context, *change, *flagsConfig, key, what);
      if (ops.is_error())
      {
        return errorOf(ops);
      }
      if (!ops.value().empty())
      {
        update.*member = ops.value()[0];
      }
      if (ops.value().size() > 1)
      {
        extra.*member = ops.value()[1];
        hasExtra = true;
      }
      hasChange = true;
    }
  }

  if (contains(unset, "surfaceFlags"))
  {
    update.surfaceFlags = mdl::SetFlags{std::nullopt};
  }
  if (contains(unset, "contentFlags"))
  {
    update.surfaceContents = mdl::SetFlags{std::nullopt};
  }
  if (contains(unset, "surfaceValue"))
  {
    update.surfaceValue = mdl::SetValue{std::nullopt};
  }
  if (contains(unset, "color"))
  {
    update.color = std::optional<Color>{std::nullopt};
  }
  hasChange = hasChange || !unset.empty();

  if (!hasChange)
  {
    return invalidArgument(
      "No attribute to change was given.",
      "Pass e.g. offset, scale, rotation, material, surfaceFlags or unset.");
  }

  const auto setsSurfaceAttributes =
    args.has("surfaceFlags") || args.has("contentFlags") || args.has("surfaceValue");
  if (setsSurfaceAttributes && !storesSurfaceAttributes(format))
  {
    context.warn(
      "ATTRIBUTE_NOT_SAVED",
      "The " + mdl::formatName(format)
        + " map format does not store surface flags, content flags or surface values; "
          "they are kept in the editor but lost when the map is saved.");
  }
  if (args.has("color") && !storesColor(format))
  {
    context.warn(
      "ATTRIBUTE_NOT_SAVED",
      "Only the Daikatana map format stores face colors; the " + mdl::formatName(format)
        + " format loses them when the map is saved.");
  }

  auto faces = resolveFaceTargets(context, args);
  if (faces.is_error())
  {
    return errorOf(faces);
  }

  if (update != mdl::UpdateBrushFaceAttributes{})
  {
    updates.push_back(std::move(update));
  }
  if (hasExtra)
  {
    updates.push_back(std::move(extra));
  }
  if (auto result = setAttributes(context, faces.value(), updates); result.is_error())
  {
    return result;
  }
  if (parallelRotation)
  {
    if (auto result = applyPerFace(
          context,
          faces.value(),
          [&](
            const mdl::BrushFace& face) -> std::optional<mdl::UpdateBrushFaceAttributes> {
            const auto delta =
              vm::normalize_degrees(*parallelRotation - faceRotation(face));
            if (delta < RotationEpsilon || delta > 360.0 - RotationEpsilon)
            {
              return std::nullopt;
            }
            return mdl::UpdateBrushFaceAttributes{
              .rotation = mdl::AddValue{float(delta)}};
          });
        result.is_error())
    {
      return result;
    }
  }
  warnUvFindings(context, faces.value());
  return facesResult(context, faces.value());
}

// face_attributes_copy

ToolResult faceAttributesCopy(CallContext& context, const Args& args)
{
  auto& map = context.map();
  const auto sourceId = args.get<std::string>("source");
  auto source = resolveFace(context, sourceId);
  if (source.is_error())
  {
    return errorOf(source);
  }

  auto targets = resolveFaceTargets(context, args);
  if (targets.is_error())
  {
    return errorOf(targets);
  }
  auto faces = targets.value();
  std::erase(faces, source.value());
  if (faces.empty())
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "There are no target faces besides the source face " + sourceId + ".",
      "Pass the target faces in ids, or select them.",
      {sourceId});
  }

  const auto mode = args.get<std::string>("mode");
  const auto includeContentFlags = args.get<bool>("contentFlags");
  if (mode == "material" && args.has("contentFlags"))
  {
    warnIgnored(context, args, {"contentFlags"}, "in mode 'material'");
  }
  if (mode == "rotate" && !mdl::isParallelUvCoordSystem(mapFormat(map)))
  {
    context.warn(
      "ROTATION_NEEDS_VALVE_FORMAT",
      "The " + mdl::formatName(mapFormat(map))
        + " format has no UV axes to rotate, so 'rotate' copies offset, scale and "
          "rotation like 'project'. Use a Valve 220 format for textures that wrap "
          "around edges.");
  }

  // copy everything needed from the source first: changing faces of the source brush
  // replaces its faces
  const auto& sourceFace = source.value().face();
  const auto materialName = sourceFace.materialName();
  const auto attributes = includeContentFlags
                            ? mdl::copyAll(sourceFace)
                            : mdl::copyAllExceptContentFlags(sourceFace);
  const auto snapshot = sourceFace.takeUvCoordSystemSnapshot();
  const auto uvAttributes = sourceFace.uvAttributes();
  const auto sourcePlane = sourceFace.boundary();
  const auto wrapStyle =
    mode == "rotate" ? mdl::WrapStyle::Rotation : mdl::WrapStyle::Projection;

  auto result = withFaces(context, faces, [&]() -> ToolResult {
    if (mode == "material")
    {
      if (!mdl::setBrushFaceAttributes(map, {.materialName = materialName}))
      {
        return context.operationFailed("The material could not be copied.");
      }
      return Json::object();
    }

    if (!mdl::setBrushFaceAttributes(map, attributes))
    {
      return context.operationFailed("The face attributes could not be copied.");
    }
    if (snapshot && !mdl::copyUv(map, *snapshot, uvAttributes, sourcePlane, wrapStyle))
    {
      return context.operationFailed("The UV alignment could not be copied.");
    }
    return Json::object();
  });
  if (result.is_error())
  {
    return result;
  }

  auto json = facesResult(context, faces);
  json["source"] = sourceId;
  json["mode"] = mode;
  return json;
}

// uv_align

mdl::UvPolicy policyArgument(const Args& args)
{
  const auto policy = args.getOr<std::string>("policy", "best");
  return policy == "next"   ? mdl::UvPolicy::next
         : policy == "prev" ? mdl::UvPolicy::prev
                            : mdl::UvPolicy::best;
}

std::optional<mdl::UpdateBrushFaceAttributes> centerUpdate(const mdl::BrushFace& face)
{
  const auto [min, max] = uvExtent(face, face.uvAttributes().scale);
  const auto size = face.textureSize();
  const auto offset = mdl::modOffset(size / 2.0f - (min + max) / 2.0f, size);
  return mdl::UpdateBrushFaceAttributes{
    .xOffset = mdl::SetValue{offset.x()},
    .yOffset = mdl::SetValue{offset.y()},
  };
}

/**
 * Scales the texture so that it repeats the given number of times along U and / or V
 * across the face, and moves it so that a repetition starts at the face's edge.
 */
std::optional<mdl::UpdateBrushFaceAttributes> fitUpdate(
  const mdl::BrushFace& face,
  const std::optional<double> repeatU,
  const std::optional<double> repeatV)
{
  const auto [unitMin, unitMax] = uvExtent(face, vm::vec2f{1, 1});
  const auto faceSize = unitMax - unitMin;
  const auto textureSize = face.textureSize();
  auto scale = face.uvAttributes().scale;

  const auto fitted = [&](const size_t axis, const double repeat) {
    const auto sign = scale[axis] < 0.0f ? -1.0f : 1.0f;
    return sign * faceSize[axis] / (float(repeat) * textureSize[axis]);
  };
  if (repeatU)
  {
    scale[0] = fitted(0, *repeatU);
  }
  if (repeatV)
  {
    scale[1] = fitted(1, *repeatV);
  }
  if (scale[0] == 0.0f || scale[1] == 0.0f)
  {
    // a face without extent along an axis cannot be fitted
    return std::nullopt;
  }

  const auto [min, max] = uvExtent(face, scale);
  const auto offset = mdl::modOffset(-min, textureSize);

  auto update = mdl::UpdateBrushFaceAttributes{};
  if (repeatU)
  {
    update.xScale = mdl::SetValue{scale.x()};
    update.xOffset = mdl::SetValue{offset.x()};
  }
  if (repeatV)
  {
    update.yScale = mdl::SetValue{scale.y()};
    update.yOffset = mdl::SetValue{offset.y()};
  }
  return update;
}

const auto AlignParameterKeys = std::vector<std::string_view>{
  "edge",
  "policy",
  "repeatU",
  "repeatV",
  "keepAspect",
  "round",
  "trimSheet",
  "axis",
  "direction"};

std::vector<std::string_view> relevantAlignParameters(
  const std::string& operation, const Args& args)
{
  if (operation == "justify")
  {
    return args.getOr<std::string>("edge", "") == "center"
             ? std::vector<std::string_view>{"edge"}
             : std::vector<std::string_view>{"edge", "policy"};
  }
  if (operation == "align")
  {
    return {"policy"};
  }
  if (operation == "fit")
  {
    return args.getOr<bool>("trimSheet", false)
             ? std::vector<std::string_view>{"trimSheet", "policy"}
             : std::vector<std::string_view>{
                 "trimSheet", "repeatU", "repeatV", "keepAspect", "round"};
  }
  if (operation == "flip")
  {
    return {"axis"};
  }
  if (operation == "rotate90")
  {
    return {"direction"};
  }
  return {};
}

ToolResult uvAlign(CallContext& context, const Args& args)
{
  auto& map = context.map();
  const auto operation = args.get<std::string>("operation");

  const auto required =
    [&](const char* key, const char* example) -> std::optional<ToolError> {
    if (!args.has(key))
    {
      return invalidArgument(
        "Operation '" + operation + "' needs '" + key + "'.",
        "Pass e.g. {\"operation\": \"" + operation + "\", " + example + "}.");
    }
    return std::nullopt;
  };
  if (operation == "justify")
  {
    if (auto error = required("edge", "\"edge\": \"left\""))
    {
      return *error;
    }
  }
  else if (operation == "flip")
  {
    if (auto error = required("axis", "\"axis\": \"u\""))
    {
      return *error;
    }
  }
  else if (operation == "rotate90")
  {
    if (auto error = required("direction", "\"direction\": \"cw\""))
    {
      return *error;
    }
  }

  const auto relevant = relevantAlignParameters(operation, args);
  for (const auto key : AlignParameterKeys)
  {
    if (args.has(key) && std::ranges::find(relevant, key) == relevant.end())
    {
      context.warn(
        "IGNORED_ARGUMENT",
        "'" + std::string{key} + "' has no effect on operation '" + operation
          + "' with these arguments.");
    }
  }

  auto resolved = resolveFaceTargets(context, args);
  if (resolved.is_error())
  {
    return errorOf(resolved);
  }
  auto faces = resolved.value();
  const auto policy = policyArgument(args);

  const auto editorOperation = [&](const std::function<void()>& function) {
    return withFaces(context, faces, [&]() -> ToolResult {
      function();
      return Json::object();
    });
  };
  const auto update = [&](const mdl::UpdateBrushFaceAttributes& attributes) {
    return setAttributes(context, faces, {attributes});
  };

  auto result = ToolResult{Json::object()};
  auto reportFits = false;
  auto fitProfiles = std::optional<UvFitProfiles>{};
  if (operation == "justify")
  {
    const auto edge = args.get<std::string>("edge");
    if (edge == "center")
    {
      removeFacesWithoutMaterial(context, faces, "centering");
      result = applyPerFace(context, faces, centerUpdate);
    }
    else
    {
      const auto direction = edge == "left"    ? mdl::UvJustifyDirection::Left
                             : edge == "right" ? mdl::UvJustifyDirection::Right
                             : edge == "up"    ? mdl::UvJustifyDirection::Up
                                               : mdl::UvJustifyDirection::Down;
      result = editorOperation([&]() { mdl::justifyUv(map, direction, policy); });
    }
  }
  else if (operation == "align")
  {
    result = editorOperation([&]() { mdl::alignUv(map, policy); });
  }
  else if (operation == "fit")
  {
    if (args.getOr<bool>("trimSheet", false))
    {
      result = editorOperation([&]() {
        mdl::fitUv(
          map, mdl::UvFitDirection::Horizontal, policy, mdl::UvFitMode::trimSheet);
        mdl::fitUv(map, mdl::UvFitDirection::Vertical, policy, mdl::UvFitMode::trimSheet);
      });
    }
    else
    {
      const auto request = uvFitRequest(args);
      if (request.is_error())
      {
        return errorOf(request);
      }
      removeFacesWithoutMaterial(context, faces, "fitting");
      if (request.value().keepAspect)
      {
        fitProfiles = uvFitProfiles(context, faces, false);
        result = applyPerFace(context, faces, [&](const auto& face) {
          return aspectFitUpdate(face, request.value(), *fitProfiles);
        });
      }
      else
      {
        result = applyPerFace(context, faces, [&](const auto& face) {
          return fitUpdate(face, request.value().repeatU, request.value().repeatV);
        });
      }
      reportFits = true;
    }
  }
  else if (operation == "typical")
  {
    removeFacesWithoutMaterial(context, faces, "the typical scale");
    fitProfiles = uvFitProfiles(context, faces, true);
    result = applyPerFace(context, faces, [&](const auto& face) {
      return typicalFitUpdate(face, *fitProfiles);
    });
    reportFits = true;
  }
  else if (operation == "autoFit")
  {
    result = editorOperation([&]() { mdl::autoFitUv(map); });
  }
  else if (operation == "reset")
  {
    result = update(mdl::resetAll(faceAttribsConfig(map).defaultUvAttributes));
  }
  else if (operation == "resetToWorld")
  {
    result = update(mdl::resetAllToParaxial(faceAttribsConfig(map).defaultUvAttributes));
  }
  else if (operation == "flip")
  {
    result = args.get<std::string>("axis") == "u"
               ? update({.xScale = mdl::MultiplyValue{-1.0f}})
               : update({.yScale = mdl::MultiplyValue{-1.0f}});
  }
  else if (operation == "rotate90")
  {
    const auto angle = args.get<std::string>("direction") == "ccw" ? 90.0f : -90.0f;
    result = update({.rotation = mdl::AddValue{angle}});
  }

  if (result.is_error())
  {
    return result;
  }

  warnUvFindings(context, faces);
  auto json = facesResult(context, faces);
  json["operation"] = operation;
  if (reportFits)
  {
    json["fits"] =
      uvFitReport(faces, context.ids(), operation == "typical" ? &*fitProfiles : nullptr);
  }
  return json;
}

// uv_nudge

ToolResult uvNudge(CallContext& context, const Args& args)
{
  auto& map = context.map();
  const auto direction = args.getOptional<std::string>("direction");
  const auto offsetBy = args.getOptional<vm::vec2d>("offsetBy");
  const auto rotate = args.getOptional<std::string>("rotate");

  if (direction && offsetBy)
  {
    return invalidArgument(
      "'direction' and 'offsetBy' cannot be combined.",
      "Move by steps with direction (and distance), or by an exact offset change with "
      "offsetBy.");
  }
  if (!direction && !offsetBy && !rotate)
  {
    return invalidArgument(
      "Nothing to do: pass 'direction', 'offsetBy' or 'rotate'.",
      "E.g. {\"direction\": \"left\"} moves the textures by one grid step.");
  }
  if (!direction)
  {
    warnIgnored(context, args, {"distance"}, "without 'direction'");
  }
  if (!rotate)
  {
    warnIgnored(context, args, {"angle"}, "without 'rotate'");
  }

  auto faces = resolveFaceTargets(context, args);
  if (faces.is_error())
  {
    return errorOf(faces);
  }

  const auto distance = float(args.getOr<double>("distance", map.grid().actualSize()));
  const auto angle =
    float(args.getOr<double>("angle", vm::to_degrees(map.grid().angle())));

  // the image moves along +u when the offset decreases and the scale is positive
  const auto move = [&]() -> vm::vec2f {
    if (!direction)
    {
      return vm::vec2f{0, 0};
    }
    if (*direction == "right")
    {
      return vm::vec2f{distance, 0};
    }
    if (*direction == "left")
    {
      return vm::vec2f{-distance, 0};
    }
    if (*direction == "down")
    {
      return vm::vec2f{0, distance};
    }
    return vm::vec2f{0, -distance};
  }();

  auto result = applyPerFace(context, faces.value(), [&](const auto& face) {
    auto update = mdl::UpdateBrushFaceAttributes{};
    if (direction)
    {
      const auto scale = face.uvAttributes().scale;
      const auto delta = vm::vec2f{
        scale.x() < 0.0f ? move.x() : -move.x(),
        scale.y() < 0.0f ? move.y() : -move.y(),
      };
      if (delta.x() != 0.0f)
      {
        update.xOffset = mdl::AddValue{delta.x()};
      }
      if (delta.y() != 0.0f)
      {
        update.yOffset = mdl::AddValue{delta.y()};
      }
    }
    else if (offsetBy)
    {
      update.xOffset = mdl::AddValue{float(offsetBy->x())};
      update.yOffset = mdl::AddValue{float(offsetBy->y())};
    }
    if (rotate)
    {
      update.rotation = mdl::AddValue{*rotate == "ccw" ? angle : -angle};
    }
    return std::optional{update};
  });
  if (result.is_error())
  {
    return result;
  }
  return facesResult(context, faces.value());
}

} // namespace

void registerFaceTools(ToolRegistry& registry)
{
  registry.add(
    ToolDef{"face_attributes_get"}
      .title("Get Face Attributes")
      .description(
        "Returns the texture attributes of faces (read-only): material, "
        "materialSize ([width, height] in texels, null if not loaded), offset [u, "
        "v] (texels), scale [u, v] (map units per texel), rotation (degrees; for "
        "valve220 derived from the UV axes, since the stored value drifts on "
        "transforms with alignment lock), uAxis / vAxis and the face's smart tags. "
        "surfaceFlags / contentFlags {bits, names, unknownBits, fromMaterial} and "
        "surfaceValue appear in formats that store them "
        "(format.storesSurfaceAttributes, e.g. Quake 2) or when set; color [r, g, "
        "b] (0-255) if set. 'format' describes the map format (mapFormat, uvFormat "
        "\"valve220\" or \"standard\", storesSurfaceAttributes, storesColor) and "
        "the flag names the game defines. Targets: face ids ('brush:12/face:3'), "
        "or brush, group and entity ids for all their faces (hidden and locked "
        "ones too); default: the current selection. detail \"full\" adds normal, "
        "center, area and vertices. Change them with face_attributes_set, uv_align "
        "or uv_nudge. Example: {\"ids\": [\"brush:12/face:3\", \"brush:14\"]}")
      .input(object({
        faceTargetsField(
          "Face ids ('brush:1042/face:3') and brush, group or entity ids (all faces of "
          "their brushes). Default: the selected faces, or all faces of the selected "
          "objects"),
      }))
      .output(object({
        field("items", array(any()))
          .required()
          .describe(
            "Per face: {id, brush, material, materialSize, offset, scale, rotation, "
            "uAxis, vAxis, tags, surfaceFlags?, contentFlags?, surfaceValue?, color?}"),
        field("total", integer()).required(),
        field("nextCursor", any()).required(),
        field("format", any())
          .required()
          .describe(
            "{mapFormat, uvFormat, storesSurfaceAttributes, storesColor, surfaceFlags, "
            "contentFlags: [{name, value}]}"),
      }))
      .paginated()
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(faceAttributesGet));

  registry.add(
    ToolDef{"face_attributes_set"}
      .title("Set Face Attributes")
      .description(
        "Sets texture attributes of faces, like the editor's face inspector; one "
        "undo step. Absolute: offset [u, v] (texels), scale [u, v], rotation "
        "(degrees); relative: offsetBy (added), scaleBy (multiplied), rotateBy "
        "(added); an absolute value excludes its relative one. material "
        "(UNKNOWN_MATERIAL warning if not loaded). surfaceFlags / contentFlags: "
        "{\"set\": [...]} replaces all flags, {\"add\": [...], \"remove\": [...]} "
        "changes some; items are flag names (face_attributes_get lists them under "
        "format) or raw bits; unknown names are skipped with UNKNOWN_FLAG. "
        "surfaceValue, color [r, g, b] 0-255 (Daikatana). unset returns "
        "surfaceFlags, contentFlags or surfaceValue to the material's defaults, or "
        "removes color. Values a format does not store (e.g. flags in Standard or "
        "Valve maps) warn with ATTRIBUTE_NOT_SAVED. Targets: face ids, or brush, "
        "group and entity ids; default: the current selection. Returns count and "
        "the attributes of the first 50 faces; uv_check findings (UV_* codes) are "
        "added as warnings. Examples: {\"ids\": [\"brush:12/face:3\"], \"scale\": "
        "[0.5, 0.5], \"offsetBy\": [16, 0]}; {\"ids\": [\"brush:12/face:3\"], "
        "\"surfaceFlags\": {\"add\": [\"light\"]}, \"surfaceValue\": 300}")
      .input(object({
        faceTargetsField(),
        field("material", string().nonEmpty()).describe("Material name"),
        field("offset", vec2()).describe("Offset [u, v] in texels"),
        field("offsetBy", vec2()).describe("[du, dv] in texels, added to the offset"),
        field("scale", vec2())
          .describe("Scale [u, v] in map units per texel; negative flips, 0 is invalid"),
        field("scaleBy", vec2()).describe("[fu, fv], multiplies the scale"),
        field("rotation", angle())
          .describe("Rotation in degrees, as face_attributes_get reports it"),
        field("rotateBy", angle()).describe("Added to the rotation (degrees)"),
        field("surfaceFlags", flagsChangeSchema("surface flags")),
        field("contentFlags", flagsChangeSchema("content flags")),
        field("surfaceValue", number()).describe("Surface value (e.g. light intensity)"),
        field("color", array(integer().min(0).max(255)).minSize(3).maxSize(3))
          .describe("Face color [r, g, b], 0-255 (Daikatana)"),
        field(
          "unset",
          array(enumOf({"surfaceFlags", "contentFlags", "surfaceValue", "color"})))
          .describe("Return surfaceFlags, contentFlags or surfaceValue to the material's "
                    "defaults; color: remove it"),
      }))
      .output(facesOutput())
      .mutation(Mutation::Map)
      .handler(faceAttributesSet));

  registry.add(
    ToolDef{"face_attributes_copy"}
      .title("Copy Face Attributes")
      .description(
        "Copies the texture attributes of the source face to the target faces, "
        "like the editor's Alt+click; one undo step. mode \"project\" (Alt+click): "
        "material, offset, scale, rotation, surface flags, value and color, with "
        "the texture projected from the source plane so that it continues "
        "seamlessly across coplanar or angled faces; \"rotate\" (Alt+Shift+click): "
        "the same, but the UV axes are rotated around the shared edge so that the "
        "texture wraps around corners (Valve 220 only; in Standard maps it works "
        "like project and warns with ROTATION_NEEDS_VALVE_FORMAT); \"material\" "
        "(Alt+Ctrl+click): only the material. Content flags are copied only with "
        "contentFlags: true. Targets: face ids, or brush, group and entity ids; "
        "default: the current selection; the source face is never a target. "
        "Returns count and the attributes of the first 50 faces. Example: "
        "{\"source\": \"brush:12/face:3\", \"ids\": [\"brush:14\"], \"mode\": "
        "\"rotate\"}")
      .input(object({
        field("source", objectId({ObjectKind::Brush}))
          .required()
          .describe("The face to copy from, e.g. 'brush:12/face:3'"),
        faceTargetsField(
          "Target face ids and brush, group or entity ids (all faces of their brushes). "
          "Default: the selected faces, or all faces of the selected objects"),
        field("mode", enumOf({"project", "rotate", "material"}).defaultsTo("project"))
          .describe(
            "project: all attributes, projected from the source plane; rotate: the same, "
            "wrapped around the shared edge (Valve 220); material: only the material"),
        field("contentFlags", boolean().defaultsTo(false))
          .describe("Also copy the content flags (project and rotate)"),
      }))
      .output(facesOutput({
        field("source", string()).required().describe("The source face id"),
        field("mode", string()).required().describe("The mode used"),
      }))
      .mutation(Mutation::Map)
      .handler(faceAttributesCopy));

  registry.add(
    ToolDef{"uv_align"}
      .title("Align UV")
      .description(
        "Aligns textures on faces like the buttons of the editor's face inspector; "
        "one undo step. operation: \"justify\" (edge \"left\", \"right\", \"up\", "
        "\"down\" as seen looking at the face, or \"center\"); \"align\" (rotate "
        "the texture to the face edge closest to the U axis); \"fit\" (scale so "
        "the texture repeats repeatU x repeatV times across the face from its "
        "edge; fractional repeats allowed, an omitted one leaves that axis "
        "unchanged, both omitted = 1 x 1; keepAspect with only one repeat lets the "
        "other axis follow with undistorted texels; round rounds the repeats to "
        "whole numbers >= 1; trimSheet uses the editor's trim sheet fit instead); "
        "\"typical\" (the material's typical scale from material_usage, justified "
        "to the face edge; TYPICAL_SCALE_DEFAULT if only the game's default is "
        "known); \"autoFit\" (align, justify and fit to whole repeats); \"reset\" "
        "(offset 0, rotation 0, default scale and UV axes); \"resetToWorld\" (the "
        "same with world-aligned axes); \"flip\" (axis \"u\" or \"v\"); "
        "\"rotate90\" (direction \"cw\" = -90 or \"ccw\" = +90). policy (\"best\", "
        "\"next\", \"prev\") cycles through the choices of justify, align and trim "
        "sheet fit like repeated clicks. Arguments that do not apply warn with "
        "IGNORED_ARGUMENT. center and fit skip faces whose material is not loaded "
        "(MATERIAL_NOT_LOADED). Targets: face ids, or brush, group and entity ids; "
        "default: the current selection. Returns count, the first 50 faces and, "
        "for fit and typical, 'fits' (scale and repeats per face); uv_check "
        "findings (UV_* codes) are added as warnings. Examples: {\"ids\": "
        "[\"brush:12\"], \"operation\": \"fit\", \"repeatU\": 2, \"keepAspect\": "
        "true, \"round\": true}; {\"ids\": [\"brush:12/face:3\"], \"operation\": "
        "\"justify\", \"edge\": \"center\"}")
      .input(object({
        faceTargetsField(),
        field(
          "operation",
          enumOf(
            {"justify",
             "align",
             "fit",
             "autoFit",
             "reset",
             "resetToWorld",
             "flip",
             "rotate90",
             "typical"}))
          .required()
          .describe(
            "The alignment operation; see the tool description for what each one does "
            "and which arguments it takes"),
        field("edge", enumOf({"left", "right", "up", "down", "center"}))
          .describe("justify: the edge to justify to, or center"),
        field("policy", enumOf({"best", "next", "prev"}))
          .describe(
            "justify, align, fit with trimSheet: which choice to take. Default: best"),
        field("repeatU", number().min(0.001))
          .describe("fit: number of texture repeats along U across the face"),
        field("repeatV", number().min(0.001))
          .describe("fit: number of texture repeats along V across the face"),
        field("keepAspect", boolean())
          .describe(
            "fit: with only repeatU or only repeatV, the other axis keeps the texels "
            "undistorted. Default: false"),
        field("round", boolean())
          .describe("fit: round the repeats to whole numbers (>= 1). Default: false"),
        field("trimSheet", boolean())
          .describe("fit: use the editor's trim sheet fit. Default: false"),
        field("axis", enumOf({"u", "v"})).describe("flip: the axis to flip"),
        field("direction", enumOf({"cw", "ccw"}))
          .describe("rotate90: clockwise (-90) or counterclockwise (+90)"),
      }))
      .output(facesOutput({
        field("operation", string()).required().describe("The operation applied"),
        field("fits", array(any()))
          .describe(
            "fit and typical: [{id, material, scale, repeats, typicalScale?: {value, "
            "source, samples}}] for the first 50 faces"),
      }))
      .mutation(Mutation::Map)
      .handler(uvAlign));

  registry.add(
    ToolDef{"uv_nudge"}
      .title("Nudge UV")
      .description(
        "Moves or rotates textures on faces in steps, relative to each face's "
        "texture axes (not the camera); one undo step. direction \"right\" / "
        "\"left\" moves the texture image along +U / -U, \"down\" / \"up\" along "
        "+V / -V (V points down in the image) by distance texels (default: the "
        "grid size; a negative scale is taken into account). offsetBy [du, dv] "
        "instead adds exactly that to the offset. rotate \"ccw\" / \"cw\" adds / "
        "subtracts angle degrees (default: the grid angle, like the editor's "
        "rotate keys). Targets: face ids, or brush, group and entity ids; default: "
        "the current selection. Returns count and the attributes of the first 50 "
        "faces. Example: {\"ids\": [\"brush:12/face:3\"], \"direction\": \"left\", "
        "\"distance\": 8, \"rotate\": \"ccw\", \"angle\": 15}")
      .input(object({
        faceTargetsField(),
        field("direction", enumOf({"left", "right", "up", "down"}))
          .describe(
            "Direction to move the texture image in its own axes: right +U, left -U, "
            "down +V, up -V"),
        field("distance", number().min(0))
          .describe("Texels to move. Default: the grid size"),
        field("offsetBy", vec2()).describe("Exact offset change [du, dv] in texels"),
        field("rotate", enumOf({"cw", "ccw"}))
          .describe("Rotate the texture clockwise or counterclockwise by angle"),
        field("angle", number().min(0))
          .describe("Degrees to rotate. Default: the grid angle"),
      }))
      .output(facesOutput())
      .mutation(Mutation::Map)
      .handler(uvNudge));
}

} // namespace tb::mcp
