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

#include "mcp/tools/UvTools.h"

#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/JsonVm.h"
#include "mcp/ObjectIds.h"
#include "mcp/Pagination.h"
#include "mcp/Schema.h"
#include "mcp/Targets.h"
#include "mcp/ToolRegistry.h"
#include "mcp/tools/MaterialKnowledgeTools.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/Map.h"
#include "mdl/NodeQueries.h"
#include "mdl/Selection.h"
#include "mdl/UpdateBrushFaceAttributes.h"
#include "mdl/UvAttributes.h"
#include "mdl/WorldNode.h"

#include "kd/string_format.h"
#include "kd/string_utils.h"

#include "vm/vec.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <string>
#include <utility>

namespace tb::mcp
{
namespace
{

using namespace schema;

/** At most this many faces are listed in a fit report. */
constexpr auto MaxFacesInReport = size_t(50);

std::string lower(const std::string& name)
{
  return kdl::str_to_lower(name);
}

std::string faceId(const IdRegistry& ids, const mdl::BrushFaceHandle& handle)
{
  return ids.formatFace(*handle.node(), handle.faceIndex());
}

double sign(const double value)
{
  return value < 0.0 ? -1.0 : 1.0;
}

/** The minimum and maximum texture coordinates of the face's vertices at offset 0. */
std::pair<vm::vec2d, vm::vec2d> texelExtent(
  const mdl::BrushFace& face, const vm::vec2d& scale)
{
  constexpr auto inf = std::numeric_limits<double>::infinity();
  auto min = vm::vec2d{inf, inf};
  auto max = vm::vec2d{-inf, -inf};
  for (const auto& vertex : face.vertexPositions())
  {
    const auto texel = vm::vec2d{
      vm::dot(vertex, face.uAxis()) / scale.x(),
      vm::dot(vertex, face.vAxis()) / scale.y()};
    min = vm::min(min, texel);
    max = vm::max(max, texel);
  }
  return {min, max};
}

/** Sets the scale and moves the texture so that a repetition starts at the face edge. */
mdl::UpdateBrushFaceAttributes justifiedUpdate(
  const mdl::BrushFace& face, const vm::vec2d& scale)
{
  const auto [min, max] = texelExtent(face, scale);
  const auto offset = mdl::modOffset(vm::vec2f{-min}, face.textureSize());
  return mdl::UpdateBrushFaceAttributes{
    .xOffset = mdl::SetValue{offset.x()},
    .yOffset = mdl::SetValue{offset.y()},
    .xScale = mdl::SetValue{float(scale.x())},
    .yScale = mdl::SetValue{float(scale.y())},
  };
}

// uv_check

/**
 * The faces named by explicit ids, including hidden and locked ones (checking does not
 * change them); brush, group and entity ids stand for all faces of their brushes.
 */
Result<std::vector<mdl::BrushFaceHandle>, ToolError> facesOfIds(
  CallContext& context, const std::vector<std::string>& ids)
{
  auto faces = std::vector<mdl::BrushFaceHandle>{};
  auto seen = std::set<std::pair<const mdl::BrushNode*, size_t>>{};
  const auto addFace = [&](const mdl::BrushFaceHandle& handle) {
    if (seen.emplace(handle.node(), handle.faceIndex()).second)
    {
      faces.push_back(handle);
    }
  };

  for (const auto& id : ids)
  {
    const auto ref = parseObjectRef(id);
    if (!ref)
    {
      return makeError(ErrorCode::InvalidArgument, "'" + id + "' is not a valid id.");
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

ToolResult uvCheck(CallContext& context, const Args& args)
{
  auto& map = context.map();
  const auto request = pageRequest(args, map.modificationCount());
  if (request.is_error())
  {
    return errorOf(request);
  }

  auto only = std::vector<UvIssue>{};
  for (const auto& code : args.getOr<std::vector<std::string>>("codes", {}))
  {
    if (const auto issue = uvIssueFromString(code))
    {
      only.push_back(*issue);
    }
  }

  const auto scope = args.getOptional<std::string>("scope");
  const auto explicitIds = args.getOptional<std::vector<std::string>>("ids");
  if (scope && explicitIds)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Pass either 'ids' or 'scope', not both.",
      "E.g. {\"ids\": [\"brush:12\"]} or {\"scope\": \"map\"}.");
  }

  auto faces = std::vector<mdl::BrushFaceHandle>{};
  auto checked = std::string{};
  if (scope == "map")
  {
    checked = "map";
    faces = mdl::collectBrushFaces(std::vector<mdl::Node*>{&map.worldNode()});
  }
  else if (explicitIds)
  {
    checked = "ids";
    auto resolved = facesOfIds(context, *explicitIds);
    if (resolved.is_error())
    {
      return errorOf(resolved);
    }
    faces = std::move(resolved.value());
  }
  else
  {
    checked = "selection";
    faces = map.selection().allBrushFaces();
    if (faces.empty())
    {
      return makeError(
        ErrorCode::NoSelection,
        "No brushes or faces are selected.",
        "Pass face or brush ids, or {\"scope\": \"map\"} to check the whole map.");
    }
  }

  auto knowledge = materialKnowledge(context);
  warnKnowledgeProblems(context, knowledge);
  const auto findings = checkUv(faces, map, profileProvider(knowledge), only);

  auto counts = Json::object();
  for (const auto& code : uvIssueCodes())
  {
    const auto issue = *uvIssueFromString(code);
    if (only.empty() || std::ranges::find(only, issue) != only.end())
    {
      counts[code] = 0;
    }
  }
  auto items = std::vector<Json>{};
  items.reserve(findings.size());
  for (const auto& finding : findings)
  {
    counts[std::string{toString(finding.issue)}] =
      counts[std::string{toString(finding.issue)}].get<size_t>() + 1;
    items.push_back(toJson(finding, context.ids()));
  }

  auto result = makePage(items, request.value(), map.modificationCount());
  result["counts"] = std::move(counts);
  result["facesChecked"] = faces.size();
  result["scope"] = checked;
  return result;
}

// material_fit_geometry

Result<mdl::BrushFaceHandle, ToolError> fitGeometryFace(
  CallContext& context, const Args& args)
{
  if (const auto id = args.getOptional<std::string>("face"))
  {
    auto handle = resolveFace(context, *id);
    if (handle.is_error())
    {
      return errorOf(handle);
    }
    if (handle.value().faceIndex() >= handle.value().node()->brush().faceCount())
    {
      return makeError(
        ErrorCode::ObjectNotFound, "There is no face " + *id + ".", {}, {*id});
    }
    return handle.value();
  }

  const auto& selected = context.map().selection().brushFaces;
  if (selected.size() == 1)
  {
    return selected.front();
  }
  if (selected.empty())
  {
    return makeError(
      ErrorCode::NoSelection,
      "No face is selected.",
      "Pass 'face', e.g. {\"face\": \"brush:12/face:3\"}, or select one face.");
  }
  return makeError(
    ErrorCode::InvalidArgument,
    fmt::format("{} faces are selected; the tool measures one face.", selected.size()),
    "Pass 'face', e.g. {\"face\": \"brush:12/face:3\"}, or select one face.");
}

/** The faces of the same brush that share an edge (two vertices) with the face. */
std::vector<size_t> adjacentFaces(const mdl::BrushFaceHandle& handle)
{
  constexpr auto epsilon = 0.01;
  const auto vertices = handle.face().vertexPositions();
  const auto& faces = handle.node()->brush().faces();
  auto result = std::vector<size_t>{};
  for (size_t i = 0; i < faces.size(); ++i)
  {
    if (i == handle.faceIndex() || !faces[i].geometry())
    {
      continue;
    }
    auto shared = 0;
    for (const auto& vertex : faces[i].vertexPositions())
    {
      if (std::ranges::any_of(vertices, [&](const auto& other) {
            return vm::length(vertex - other) <= epsilon;
          }))
      {
        ++shared;
      }
    }
    if (shared >= 2)
    {
      result.push_back(i);
    }
  }
  return result;
}

/**
 * Which adjacent face to move, and how far along its normal, to change the face's extent
 * along the (in-plane) direction by delta: side +1 moves the face at the maximum edge,
 * -1 the face at the minimum edge.
 */
Json resizeCall(
  CallContext& context,
  const mdl::BrushFaceHandle& handle,
  const vm::vec3d& axisDirection,
  const double delta,
  const double side)
{
  const auto& face = handle.face();
  const auto normal = face.normal();
  const auto inPlane = axisDirection - normal * vm::dot(axisDirection, normal);
  const auto inPlaneLength = vm::length(inPlane);
  if (inPlaneLength < 1e-6)
  {
    return nullptr;
  }
  const auto direction = inPlane / inPlaneLength * side;
  const auto cosine = std::abs(vm::dot(direction, axisDirection));

  auto best = std::optional<size_t>{};
  auto bestDot = 0.1;
  const auto& faces = handle.node()->brush().faces();
  for (const auto index : adjacentFaces(handle))
  {
    const auto dot = vm::dot(faces[index].normal(), direction);
    if (dot > bestDot)
    {
      best = index;
      bestDot = dot;
    }
  }
  if (!best || cosine < 1e-6)
  {
    return nullptr;
  }

  const auto distance = roundForOutput(delta * bestDot / cosine);
  const auto id = context.ids().formatFace(*handle.node(), *best);
  return Json{
    {"face", id},
    {"distance", distance},
    {"call",
     Json{
       {"tool", "face_extrude"},
       {"arguments", Json{{"faces", Json{id}}, {"distance", distance}}},
     }},
  };
}

Json sizeAt(const double repeats, const double panelSize)
{
  return Json{
    {"size", roundForOutput(repeats * panelSize)},
    {"repeats", repeats},
  };
}

ToolResult materialFitGeometry(CallContext& context, const Args& args)
{
  auto handle = fitGeometryFace(context, args);
  if (handle.is_error())
  {
    return errorOf(handle);
  }
  const auto& face = handle.value().face();
  const auto id = faceId(context.ids(), handle.value());
  const auto materialName = args.getOr<std::string>("material", face.materialName());

  auto knowledge = materialKnowledge(context);
  warnKnowledgeProblems(context, knowledge);
  const auto& profile = profileProvider(knowledge)(materialName);
  if (!profile.loaded || !profile.textureSize)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      fmt::format("Material '{}' is not loaded, so its size is unknown.", materialName),
      "Load its texture collection (materials_collections_set) or pass a loaded "
      "material.");
  }
  const auto kind = checkedKind(profile);
  if (kind == MaterialKind::Tile)
  {
    context.warn(
      "MATERIAL_IS_TILE",
      fmt::format(
        "'{}' is a seamless tile ({}): it may repeat any number of times, so fitting the "
        "geometry to it is optional.",
        profile.name,
        profile.kind.source));
  }
  const auto& typical = *profile.typicalScale;
  if (typical.source == "config")
  {
    context.warn(
      "TYPICAL_SCALE_DEFAULT",
      fmt::format(
        "There are no notes, corpus or map statistics for '{}'; the game's default "
        "scale {} is used. Set the scale with material_notes_set if it is known.",
        profile.name,
        toJson(typical.value).dump()));
  }

  const auto sample = sampleFace(face, profile.textureSize);
  if (!sample)
  {
    return makeError(
      ErrorCode::InvalidGeometry,
      "The face has no measurable extent along its texture axes.",
      "Check the face's geometry and UV axes (uv_align reset).",
      {id});
  }

  const auto& textureSize = *profile.textureSize;
  const auto across = kind == MaterialKind::Trim ? trimAcrossAxis(profile) : std::nullopt;
  const auto axisVectors = std::array{face.uAxis(), face.vAxis()};
  auto axes = Json::array();
  auto panelSize = vm::vec2d{};
  auto allFit = true;
  for (size_t axis = 0; axis < 2; ++axis)
  {
    const auto axisLength = vm::length(axisVectors[axis]);
    const auto direction = axisVectors[axis] / axisLength;
    const auto density = typical.value[axis] / axisLength;
    const auto panel = textureSize[axis] * density;
    panelSize[axis] = panel;
    const auto faceSize = sample->worldSize[axis];
    const auto repeats = faceSize / panel;
    const auto tolerance = TexelTolerance / textureSize[axis];
    const auto relevant = !across || *across == axis;

    const auto lowerCount = std::floor(repeats + tolerance);
    const auto upperCount = std::max(1.0, std::ceil(repeats - tolerance));
    const auto fits =
      lowerCount >= 1.0
      && std::abs(faceSize - lowerCount * panel) <= density * TexelTolerance;
    const auto target = lowerCount < 1.0 || (upperCount - repeats < repeats - lowerCount)
                          ? upperCount
                          : lowerCount;
    const auto delta = fits ? 0.0 : target * panel - faceSize;
    allFit = allFit && (fits || !relevant);

    auto item = Json{
      {"axis", axis == 0 ? "u" : "v"},
      {"direction", toJson(direction)},
      {"relevant", relevant},
      {"faceSize", roundForOutput(faceSize)},
      {"panelSize", roundForOutput(panel)},
      {"repeats", roundForOutput(repeats)},
      {"fits", fits},
      {"smaller", lowerCount >= 1.0 ? sizeAt(lowerCount, panel) : Json(nullptr)},
      {"larger", sizeAt(upperCount, panel)},
      {"target", sizeAt(target, panel)},
      {"delta", roundForOutput(delta)},
      {"resize", nullptr},
    };
    if (!fits)
    {
      auto resize = resizeCall(context, handle.value(), direction, delta, 1.0);
      if (!resize.is_null())
      {
        resize["opposite"] = resizeCall(context, handle.value(), direction, delta, -1.0);
      }
      item["resize"] = std::move(resize);
    }
    axes.push_back(std::move(item));
  }

  auto then = Json{
    {"tool", "uv_align"},
    {"arguments", Json{{"ids", Json{id}}, {"operation", "typical"}}},
  };
  if (materialName != face.materialName())
  {
    then = Json{
      {"tool", "material_apply"},
      {"arguments", Json{{"ids", Json{id}}, {"material", profile.name}}},
      {"next",
       Json{
         {"tool", "uv_align"},
         {"arguments", Json{{"ids", Json{id}}, {"operation", "typical"}}},
       }},
    };
  }

  return Json{
    {"face", id},
    {"material", profile.name},
    {"kind", Json{{"value", toString(kind)}, {"source", profile.kind.source}}},
    {"textureSize", toJson(textureSize)},
    {"typicalScale",
     Json{
       {"value", toJson(typical.value)},
       {"source", typical.source},
       {"samples", typical.samples},
     }},
    {"typicalFaceSize",
     profile.typicalFaceSize ? toJson(profile.typicalFaceSize->value) : Json(nullptr)},
    {"panelSize", toJson(panelSize)},
    {"faceSize", toJson(sample->worldSize)},
    {"fits", allFit},
    {"axes", std::move(axes)},
    {"then", std::move(then)},
  };
}

} // namespace

// warnings

void warnUvFindings(CallContext& context, const std::vector<mdl::BrushFaceHandle>& faces)
{
  if (faces.empty())
  {
    return;
  }

  auto knowledge = materialKnowledge(context);
  warnKnowledgeProblems(context, knowledge);
  const auto findings = checkUv(faces, context.map(), profileProvider(knowledge));

  auto& ids = context.ids();
  auto rest = std::map<std::string, size_t>{};
  for (size_t i = 0; i < findings.size(); ++i)
  {
    const auto& finding = findings[i];
    const auto code = std::string{toString(finding.issue)};
    if (i >= MaxUvWarnings)
    {
      rest[code] += 1;
      continue;
    }

    const auto json = toJson(finding, ids);
    auto message = finding.message;
    if (json.contains("fix"))
    {
      message += fmt::format(
        " Fix: {} {}",
        json["fix"]["tool"].get<std::string>(),
        json["fix"]["arguments"].dump());
    }
    auto objectIds = std::vector<std::string>{json["face"].get<std::string>()};
    if (json.contains("neighbour"))
    {
      objectIds.push_back(json["neighbour"].get<std::string>());
    }
    context.warn(code, std::move(message), std::move(objectIds));
  }

  if (!rest.empty())
  {
    auto parts = std::vector<std::string>{};
    for (const auto& [code, count] : rest)
    {
      parts.push_back(fmt::format("{} {}", count, code));
    }
    context.warn(
      "UV_CHECK_MORE",
      fmt::format(
        "{} more UV finding(s) on these faces ({}); list them with uv_check and the face "
        "ids.",
        findings.size() - MaxUvWarnings,
        kdl::str_join(parts, ", ")));
  }
}

// uv_align fit

Result<UvFitRequest, ToolError> uvFitRequest(const Args& args)
{
  auto request = UvFitRequest{
    args.getOptional<double>("repeatU"),
    args.getOptional<double>("repeatV"),
    args.getOr<bool>("keepAspect", false),
    args.getOr<bool>("round", false),
  };

  if (request.keepAspect && request.repeatU && request.repeatV)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "keepAspect takes only one of 'repeatU' and 'repeatV'; the other axis follows the "
      "texture's aspect ratio.",
      "E.g. {\"operation\": \"fit\", \"repeatU\": 2, \"keepAspect\": true}.");
  }
  if (!request.repeatU && !request.repeatV)
  {
    request.repeatU = 1.0;
    if (!request.keepAspect)
    {
      request.repeatV = 1.0;
    }
  }
  if (request.round)
  {
    for (auto* repeat : {&request.repeatU, &request.repeatV})
    {
      if (*repeat)
      {
        **repeat = std::max(1.0, std::round(**repeat));
      }
    }
  }
  return request;
}

UvFitProfiles uvFitProfiles(
  CallContext& context,
  const std::vector<mdl::BrushFaceHandle>& faces,
  const bool warnDefaultScales)
{
  auto knowledge = materialKnowledge(context);
  warnKnowledgeProblems(context, knowledge);

  auto counts = std::map<std::string, uint64_t>{};
  auto names = std::map<std::string, std::string>{};
  for (const auto& handle : faces)
  {
    const auto& name = handle.face().materialName();
    counts[lower(name)] += 1;
    names.try_emplace(lower(name), name);
  }

  auto result = UvFitProfiles{};
  auto defaults = std::vector<std::string>{};
  for (const auto& [key, name] : names)
  {
    const auto& profile = knowledge.profile(name);
    result.aspects[key] = expectedAspect(profile);

    auto scale = *profile.typicalScale;
    if (scale.source == "map" && scale.samples <= counts[key])
    {
      // only the fitted faces use the material: fall back to the game's default
      const auto& config = context.map().gameInfo().gameConfig;
      scale = Sourced<vm::vec2d>{
        vm::abs(vm::vec2d{config.faceAttribsConfig.defaultUvAttributes.scale}),
        "config",
        0};
    }
    if (scale.source == "config")
    {
      defaults.push_back(profile.name);
    }
    result.typicalScales[key] = std::move(scale);
  }

  if (warnDefaultScales && !defaults.empty())
  {
    context.warn(
      "TYPICAL_SCALE_DEFAULT",
      fmt::format(
        "No notes, corpus or map statistics for {}; the game's default scale was used. "
        "Scan reference maps (material_corpus_scan) or set the scale "
        "(material_notes_set).",
        kdl::str_join(defaults, ", ")));
  }
  return result;
}

std::optional<mdl::UpdateBrushFaceAttributes> aspectFitUpdate(
  const mdl::BrushFace& face, const UvFitRequest& request, const UvFitProfiles& profiles)
{
  if (!face.material() || !face.geometry())
  {
    return std::nullopt;
  }

  const auto textureSize = vm::vec2d{face.textureSize()};
  const auto current = vm::vec2d{face.uvAttributes().scale};
  const auto [unitMin, unitMax] = texelExtent(face, vm::vec2d{1, 1});
  const auto extent = unitMax - unitMin;
  const auto axisLength = vm::vec2d{vm::length(face.uAxis()), vm::length(face.vAxis())};

  const auto given = request.repeatU ? size_t(0) : size_t(1);
  const auto free = 1 - given;
  const auto repeat = request.repeatU ? *request.repeatU : *request.repeatV;
  if (extent[given] <= 0.0 || extent[free] <= 0.0 || repeat <= 0.0)
  {
    return std::nullopt;
  }

  const auto it = profiles.aspects.find(lower(face.materialName()));
  const auto ratio = it != profiles.aspects.end() ? it->second.ratio : 1.0;

  auto scale = vm::vec2d{};
  scale[given] = extent[given] / (repeat * textureSize[given]);
  // texel density = scale / axis length; ratio = density U / density V
  const auto givenDensity = scale[given] / axisLength[given];
  const auto freeDensity = given == 0 ? givenDensity / ratio : givenDensity * ratio;
  scale[free] = freeDensity * axisLength[free];
  if (request.round)
  {
    const auto repeats = extent[free] / (scale[free] * textureSize[free]);
    scale[free] = extent[free] / (std::max(1.0, std::round(repeats)) * textureSize[free]);
  }
  scale = vm::vec2d{sign(current.x()) * scale.x(), sign(current.y()) * scale.y()};
  return justifiedUpdate(face, scale);
}

std::optional<mdl::UpdateBrushFaceAttributes> typicalFitUpdate(
  const mdl::BrushFace& face, const UvFitProfiles& profiles)
{
  const auto it = profiles.typicalScales.find(lower(face.materialName()));
  if (!face.material() || !face.geometry() || it == profiles.typicalScales.end())
  {
    return std::nullopt;
  }
  const auto current = vm::vec2d{face.uvAttributes().scale};
  const auto& typical = it->second.value;
  return justifiedUpdate(
    face, vm::vec2d{sign(current.x()) * typical.x(), sign(current.y()) * typical.y()});
}

Json uvFitReport(
  const std::vector<mdl::BrushFaceHandle>& faces,
  const IdRegistry& ids,
  const UvFitProfiles* typicalProfiles)
{
  auto result = Json::array();
  for (size_t i = 0; i < faces.size() && i < MaxFacesInReport; ++i)
  {
    const auto& face = faces[i].face();
    const auto sample = sampleFace(face, textureSizeOf(face.material()));
    auto item = Json{
      {"id", faceId(ids, faces[i])},
      {"material", face.materialName()},
      {"scale", toJson(vm::vec2d{face.uvAttributes().scale})},
      {"repeats", sample && sample->repeats ? toJson(*sample->repeats) : Json(nullptr)},
    };
    if (typicalProfiles)
    {
      const auto it = typicalProfiles->typicalScales.find(lower(face.materialName()));
      if (it != typicalProfiles->typicalScales.end())
      {
        item["typicalScale"] = Json{
          {"value", toJson(it->second.value)},
          {"source", it->second.source},
          {"samples", it->second.samples},
        };
      }
    }
    result.push_back(std::move(item));
  }
  return result;
}

void registerUvTools(ToolRegistry& registry)
{
  auto codes = uvIssueCodes();

  registry.add(
    ToolDef{"uv_check"}
      .title("Check UV Quality")
      .description(
        "Finds texturing problems, measured against each material's profile "
        "(material_usage: notes > corpus > map > image): UV_ASPECT_DISTORTION (texel "
        "aspect U/V more than 10% off the material's typical ratio, else square "
        "texels), UV_FRACTIONAL_REPEAT (a panel, or a trim across its short axis, does "
        "not repeat a whole number of times, at least once, within 1 texel), "
        "UV_PANEL_NOT_ALIGNED (a panel's texture edge is not on the face edge), "
        "UV_UNUSUAL_SCALE (scale more than 1.25x outside the typical range from notes, "
        "the corpus (5+ faces) or the map (20+ faces)), UV_TEXEL_DENSITY_MISMATCH "
        "(tiles whose texel size differs more than 1.5x from a neighbouring tile face: "
        "same brush sharing an edge, or coplanar and touching), UV_SEAM (a coplanar "
        "touching face with the same material does not continue the texture: other "
        "scale or rotation, or offset more than 1 texel off modulo the texture size). "
        "Tool, sky and liquid materials are skipped; repeats, alignment and seams need "
        "the material loaded. Each finding has the face, the measured values and a "
        "suggested 'fix' (a tool call with arguments) plus 'alternatives'. Targets: "
        "'ids' (faces, brushes, groups, entities; hidden and locked ones too), "
        "scope \"map\", or the selection (default). 'codes' limits the checks. "
        "Example: {\"ids\": [\"brush:12\"]} -> {\"items\": [{\"code\": "
        "\"UV_FRACTIONAL_REPEAT\", \"face\": \"brush:12/face:1\", \"brush\": "
        "\"brush:12\", \"material\": \"LAB1_GAD2\", \"message\": \"Panel 'LAB1_GAD2' "
        "repeats 1.5 x 1 times; ...\", \"measured\": {\"repeats\": [1.5, 1], ...}, "
        "\"fix\": {\"tool\": \"uv_align\", \"arguments\": {\"ids\": "
        "[\"brush:12/face:1\"], \"operation\": \"fit\", \"repeatU\": 2, \"repeatV\": "
        "1}, \"description\": \"...\"}, \"alternatives\": [...]}], \"total\": 1, "
        "\"nextCursor\": null, \"counts\": {\"UV_FRACTIONAL_REPEAT\": 1, ...}, "
        "\"facesChecked\": 6, \"scope\": \"ids\"}")
      .input(object({
        faceTargetsField(
          "Face ids ('brush:1042/face:3') and brush, group or entity ids (all faces of "
          "their brushes), including hidden and locked ones. Default: the selected "
          "faces, or all faces of the selected objects"),
        field("scope", enumOf({"selection", "map"}))
          .describe("\"map\" checks every brush face of the map; excludes 'ids'"),
        field("codes", array(enumOf(codes)).nonEmpty())
          .describe("Only these checks. Default: all"),
      }))
      .output(object({
        field("items", array(any()))
          .required()
          .describe(
            "Findings: {code, face, brush, material, message, measured, neighbour?, fix: "
            "{tool, arguments, description}, alternatives: [...]}"),
        field("total", integer()).required(),
        field("nextCursor", any()).required(),
        field("counts", any()).required().describe("Findings per checked code"),
        field("facesChecked", integer()).required(),
        field("scope", enumOf({"ids", "selection", "map"})).required(),
      }))
      .paginated()
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(uvCheck));

  registry.add(
    ToolDef{"material_fit_geometry"}
      .title("Fit Geometry to Material")
      .description(
        "Adjusts geometry to the texture instead of stretching the texture: for a face "
        "and a material (default: the face's), the texture size, the material's typical "
        "scale with its source (notes > corpus > map > the game's default), the panel "
        "size in world units at that scale (texture size x scale), the face's size "
        "along its texture axes, and per axis the nearest sizes that fit whole repeats "
        "(smaller, larger, target: the nearest one, at least 1 repeat), the change "
        "needed (delta, world units) and how to make it: resize.call extrudes the "
        "adjacent face at the edge the texture axis points to (resize.opposite: the "
        "other edge). Afterwards apply the scale with 'then' (uv_align operation "
        "\"typical\"). For trims only the axis across the strip is relevant. Warns with "
        "MATERIAL_IS_TILE for seamless tiles (fitting is optional) and "
        "TYPICAL_SCALE_DEFAULT without data. Read-only. Example: {\"face\": "
        "\"brush:12/face:1\"} -> {\"face\": \"brush:12/face:1\", \"material\": "
        "\"LAB1_GAD2\", \"kind\": {\"value\": \"panel\", \"source\": \"corpus\"}, "
        "\"textureSize\": [64, 64], \"typicalScale\": {\"value\": [0.5, 0.5], "
        "\"source\": \"corpus\", \"samples\": 58}, \"panelSize\": [32, 32], "
        "\"faceSize\": [48, 32], \"fits\": false, \"axes\": [{\"axis\": \"u\", "
        "\"faceSize\": 48, \"panelSize\": 32, \"repeats\": 1.5, \"fits\": false, "
        "\"smaller\": {\"size\": 32, \"repeats\": 1}, \"larger\": {\"size\": 64, "
        "\"repeats\": 2}, \"target\": {\"size\": 64, \"repeats\": 2}, \"delta\": 16, "
        "\"resize\": {\"face\": \"brush:12/face:4\", \"distance\": 16, \"call\": "
        "{\"tool\": \"face_extrude\", \"arguments\": {\"faces\": "
        "[\"brush:12/face:4\"], \"distance\": 16}}, \"opposite\": {...}}}, {\"axis\": "
        "\"v\", ...}], \"then\": {\"tool\": \"uv_align\", \"arguments\": {...}}}")
      .input(object({
        field("face", objectId({ObjectKind::Brush}))
          .describe("The face, e.g. 'brush:12/face:3'. Default: the one selected face"),
        field("material", string().nonEmpty())
          .describe("The material to fit. Default: the face's material"),
      }))
      .output(object({
        field("face", string()).required(),
        field("material", string()).required(),
        field("kind", any()).required().describe("{value, source}"),
        field("textureSize", vec2()).required(),
        field("typicalScale", any()).required().describe("{value, source, samples}"),
        field("typicalFaceSize", any())
          .required()
          .describe("The face size the material is typically used on, or null"),
        field("panelSize", vec2())
          .required()
          .describe("World units covered by one texture repeat at the typical scale"),
        field("faceSize", vec2())
          .required()
          .describe("The face's extent along its U and V texture axes"),
        field("fits", boolean())
          .required()
          .describe("Whether the face already fits whole repeats on the relevant axes"),
        field("axes", array(any()))
          .required()
          .describe(
            "Per axis: {axis, direction, relevant, faceSize, panelSize, repeats, fits, "
            "smaller, larger, target: {size, repeats}, delta, resize: {face, distance, "
            "call, opposite} or null}"),
        field("then", any())
          .required()
          .describe("The call that applies the typical scale afterwards"),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(materialFitGeometry));
}

} // namespace tb::mcp
