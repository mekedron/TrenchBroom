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

#include "mcp/tools/PickTools.h"

#include "NodeJson.h"
#include "ToolUtils.h"
#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/CameraProjection.h"
#include "mcp/JsonVm.h"
#include "mcp/ObjectIds.h"
#include "mcp/Session.h"
#include "mcp/ToolRegistry.h"
#include "mcp/tools/GeometryUtils.h"
#include "mcp/tools/SnapshotTools.h"
#include "mcp/tools/SpatialTools.h"
#include "mdl/BrushNode.h"
#include "mdl/Map.h"

#include "kd/result.h"

#include "vm/bbox.h"
#include "vm/ray.h"
#include "vm/vec.h"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace tb::mcp
{
namespace
{
using namespace schema;

/** The most pixels one view_pick call picks. */
constexpr auto MaxPixels = size_t(256);

/** The camera and visibility of a snapshot image. */
struct PickSource
{
  std::string documentId;
  AgentCamera camera;
  size_t width = 0;
  size_t height = 0;
  Json view;
};

std::optional<PickSource> findSource(const Session& session, const std::string& name)
{
  if (const auto* record = session.findSnapshotRecord(name))
  {
    return PickSource{
      record->documentId, record->camera, record->width, record->height, record->view};
  }
  const auto it = std::ranges::find_if(
    session.keptSnapshots, [&](const auto& kept) { return kept.name == name; });
  if (it != session.keptSnapshots.end())
  {
    return PickSource{it->documentId, it->camera, it->width, it->height, it->view};
  }
  return std::nullopt;
}

ToolError unknownSnapshotError(const Session& session, const std::string& name)
{
  auto known = std::string{};
  const auto& records = session.snapshotRecords;
  // the most recent ones
  for (auto it = records.rbegin(); it != records.rend() && it - records.rbegin() < 5;
       ++it)
  {
    known += (known.empty() ? "" : ", ") + it->id;
  }
  for (const auto& kept : session.keptSnapshots)
  {
    known += (known.empty() ? "" : ", ") + kept.name;
  }
  return makeError(
    ErrorCode::InvalidArgument,
    fmt::format("There is no snapshot '{}' in this session.", name),
    fmt::format(
      "Pass the snapshotId of a recent snapshot (the last {} are remembered) or a keepAs "
      "name; take a new one with view_snapshot. Known: {}.",
      Session::MaxSnapshotRecords,
      known.empty() ? "none" : known));
}

/** The normal of the box face that contains the point. */
vm::vec3d boxNormal(const vm::bbox3d& box, const vm::vec3d& point)
{
  auto best = std::numeric_limits<double>::max();
  auto normal = vm::vec3d{0, 0, 1};
  for (size_t axis = 0; axis < 3; ++axis)
  {
    for (const auto sign : {-1.0, 1.0})
    {
      const auto distance =
        std::abs(point[axis] - (sign < 0 ? box.min[axis] : box.max[axis]));
      if (distance < best)
      {
        best = distance;
        normal = vm::vec3d{0, 0, 0};
        normal[axis] = sign;
      }
    }
  }
  return normal;
}

Json pickHitJson(
  const RayHit& hit, const ImageProjection& projection, const IdRegistry& ids)
{
  auto result = rayHitJson(hit, ids);
  if (hit.faceIndex)
  {
    result["faceIndex"] = *hit.faceIndex;
  }
  else if (isPointEntity(*hit.node))
  {
    result["normal"] = toJson(boxNormal(hit.node->logicalBounds(), hit.point));
  }
  result["depth"] = roundForOutput(projection.depth(hit.point));
  const auto summary = nodeSummary(*hit.node, ids);
  result["bounds"] = summary.value("bounds", Json(nullptr));
  result["layer"] = summary.value("layer", Json(nullptr));
  if (!result.contains("group"))
  {
    result["group"] = nullptr;
  }
  return result;
}

ToolResult viewPick(CallContext& context, const Args& args)
{
  const auto name = args.get<std::string>("snapshot");
  const auto source = findSource(context.session(), name);
  if (!source)
  {
    return unknownSnapshotError(context.session(), name);
  }
  if (source->documentId != context.documentInfo().id)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      fmt::format(
        "The snapshot '{}' shows the document {}, not {}.",
        name,
        source->documentId,
        context.documentInfo().id),
      fmt::format("Pass document: \"{}\".", source->documentId));
  }

  if (args.has("pixel") == args.has("pixels"))
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Pass either pixel or pixels.",
      "Example: {\"snapshot\": \"snap:3\", \"pixel\": {\"x\": 512, \"y\": 384}}");
  }
  auto pixels = std::vector<Json>{};
  if (args.has("pixel"))
  {
    pixels.push_back(args.get<Json>("pixel"));
  }
  else
  {
    for (const auto& pixel : args.get<Json>("pixels"))
    {
      pixels.push_back(pixel);
    }
  }
  for (const auto& pixel : pixels)
  {
    const auto x = pixel["x"].get<int64_t>();
    const auto y = pixel["y"].get<int64_t>();
    if (x < 0 || y < 0 || x >= int64_t(source->width) || y >= int64_t(source->height))
    {
      return makeError(
        ErrorCode::InvalidArgument,
        fmt::format(
          "The pixel ({}, {}) is outside the {}x{} image.",
          x,
          y,
          source->width,
          source->height),
        "Pixel coordinates start at (0, 0) in the top left corner; x < width, y < "
        "height.");
    }
  }

  auto projection =
    ImageProjection::create(source->camera, source->width, source->height);
  if (projection.is_error())
  {
    return makeError(
      ErrorCode::InternalError,
      fmt::format("The snapshot's camera is invalid: {}", errorMessage(projection)));
  }

  auto& map = context.map();
  const auto includeHidden = args.get<bool>("includeHidden");
  const auto visibility = includeHidden
                            ? SnapshotVisibility{}
                            : snapshotVisibility(map, context.ids(), source->view);

  auto ignored = std::vector<const mdl::Node*>{};
  for (const auto& id : args.getOr<std::vector<std::string>>("ignore", {}))
  {
    auto node = context.ids().resolve(id);
    if (node.is_error())
    {
      return errorOf(node);
    }
    ignored.push_back(node.value());
  }
  const auto kinds = args.getOr<std::vector<std::string>>("kinds", {});
  const auto ignoreTriggers = args.get<bool>("ignoreTriggers");
  const auto ignorePointEntities = args.get<bool>("ignorePointEntities");
  const auto maxDistance = args.getOptional<double>("maxDistance");

  const auto accept = [&](const mdl::Node& node) {
    if (includeHidden ? false : !visibility.drawsNode(node))
    {
      return false;
    }
    if (std::ranges::any_of(ignored, [&](const auto* ignoredNode) {
          return &node == ignoredNode || node.isDescendantOf(*ignoredNode);
        }))
    {
      return false;
    }
    if (ignorePointEntities && isPointEntity(node))
    {
      return false;
    }
    if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node);
        brushNode && ignoreTriggers && classifyBrush(*brushNode) == BrushClass::Trigger)
    {
      return false;
    }
    return kinds.empty()
           || std::ranges::find(kinds, toString(objectKindOf(node))) != kinds.end();
  };

  auto picks = Json::array();
  for (const auto& pixel : pixels)
  {
    const auto x = size_t(pixel["x"].get<int64_t>());
    const auto y = size_t(pixel["y"].get<int64_t>());
    const auto ray = projection.value().pixelRay(x, y);
    const auto hits = castRay(map, ray, accept, maxDistance);

    auto hitJson = Json(nullptr);
    for (const auto& hit : hits)
    {
      // what the image cannot show: in front of the near or behind the far plane, and
      // faces the snapshot did not draw
      if (!projection.value().withinClipRange(projection.value().depth(hit.point)))
      {
        continue;
      }
      if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(hit.node);
          brushNode && hit.faceIndex && !includeHidden
          && !visibility.drawsFace(*brushNode, *hit.faceIndex))
      {
        continue;
      }
      hitJson = pickHitJson(hit, projection.value(), context.ids());
      break;
    }
    picks.push_back(Json{
      {"pixel", Json{{"x", x}, {"y", y}}},
      {"ray", Json{{"origin", toJson(ray.origin)}, {"direction", toJson(ray.direction)}}},
      {"hit", std::move(hitJson)},
    });
  }

  auto result = Json{
    {"snapshot", name},
    {"width", source->width},
    {"height", source->height},
    {"camera", toJson(source->camera)},
  };
  if (args.has("pixel"))
  {
    result["hit"] = picks.front()["hit"];
  }
  result["picks"] = std::move(picks);
  return result;
}

} // namespace

void registerPickTools(ToolRegistry& registry)
{
  const auto pixelSchema = object({
    field("x", integer().min(0)).required().describe("Column, 0 = left"),
    field("y", integer().min(0)).required().describe("Row, 0 = top"),
  });

  registry.add(
    ToolDef{"view_pick"}
      .title("Pick in Snapshot")
      .description(
        "Returns what pixels of a snapshot show (read-only): the object, face id "
        "('brush:12/face:3') and index, hit point, surface normal, distance and depth "
        "from the camera, classname or group name, group, layer and bounds. Arguments: "
        "'snapshot' is the snapshotId string returned by view_snapshot, "
        "view_snapshots_around, map_plan_view (image), view_snapshot_compare or "
        "view_snapshot_user (the last 32 per session), or a keepAs name; pass either "
        "'pixel' as one {x, y} object or 'pixels' as a list of {x, y} objects (at most "
        "256; not [x, y] pairs). Pixels count from (0, 0) at the top left corner. Picks "
        "only what the snapshot drew (its hidden objects, hideTags, hideClassnames and "
        "isolate apply; includeHidden picks anything) in the current map, so objects "
        "changed since the snapshot are picked as they are now. hit is null where the "
        "image shows only the background. Examples: {\"snapshot\": \"snap:3\", "
        "\"pixel\": {\"x\": 512, \"y\": 400}}; {\"snapshot\": \"snap:3\", \"pixels\": "
        "[{\"x\": 100, \"y\": 200}, {\"x\": 640, \"y\": 380}], \"kinds\": [\"brush\"]}")
      .input(object({
        field("snapshot", string().nonEmpty())
          .required()
          .describe(
            "The snapshotId of a snapshot (e.g. \"snap:3\") or a view_snapshot keepAs "
            "name"),
        field("pixel", pixelSchema)
          .describe("One pixel as {x, y}; excludes pixels. The result has hit"),
        field(
          "pixels",
          array(pixelSchema.describe("A pixel as {x, y}")).nonEmpty().maxSize(MaxPixels))
          .describe(fmt::format(
            "Several pixels as [{{x, y}}, ...] (at most {}); excludes pixel. The result "
            "has picks",
            MaxPixels)),
        field("includeHidden", boolean().defaultsTo(false))
          .describe("Also pick objects that the snapshot did not draw"),
        field("ignoreTriggers", boolean().defaultsTo(false))
          .describe("Look through trigger brushes"),
        field("ignorePointEntities", boolean().defaultsTo(false))
          .describe("Look through point entities"),
        field("kinds", array(enumOf({"brush", "entity", "patch"})))
          .describe("Only pick these kinds of objects (entity: point entities)"),
        field("ignore", array(objectId()))
          .describe(
            "Look through these objects (and the members of entities and groups)"),
        field("maxDistance", number().min(0))
          .describe("Ignore hits farther from the camera (map units)"),
      }))
      .output(object({
        field("snapshot", string()).describe("The snapshot argument"),
        field("width", integer()).describe("Image width in pixels"),
        field("height", integer()).describe("Image height in pixels"),
        field("camera", any()).describe("The snapshot's camera"),
        field("hit", any()).describe("With pixel: the hit of that pixel, or null"),
        field("picks", array(any()))
          .describe(
            "One per pixel, in order: {pixel: {x, y}, ray: {origin, direction}, hit: "
            "{object, kind, label, face, faceIndex, material, normal, point, distance, "
            "depth, entity, classname, group, layer, bounds} | null}"),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(viewPick));
}

} // namespace tb::mcp
