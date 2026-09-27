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

#include "mcp/tools/UvCheck.h"

#include "mcp/JsonVm.h"
#include "mcp/ObjectIds.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/Map.h"
#include "mdl/NodeTree.h"
#include "mdl/WorldNode.h"

#include "kd/string_compare.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace tb::mcp
{
namespace
{

constexpr auto Issues = std::array{
  std::pair{UvIssue::AspectDistortion, std::string_view{"UV_ASPECT_DISTORTION"}},
  std::pair{UvIssue::FractionalRepeat, std::string_view{"UV_FRACTIONAL_REPEAT"}},
  std::pair{UvIssue::PanelNotAligned, std::string_view{"UV_PANEL_NOT_ALIGNED"}},
  std::pair{UvIssue::UnusualScale, std::string_view{"UV_UNUSUAL_SCALE"}},
  std::pair{UvIssue::TexelDensityMismatch, std::string_view{"UV_TEXEL_DENSITY_MISMATCH"}},
  std::pair{UvIssue::Seam, std::string_view{"UV_SEAM"}},
};

/** Distances in world units below this are zero (vertices, planes, shared edges). */
constexpr auto GeometryEpsilon = 0.01;
/** Normals whose dot product exceeds this are parallel. */
constexpr auto ParallelDot = 1.0 - 1e-6;
/** The distance from the shared edge at which seams compare the texture's direction. */
constexpr auto SeamProbeDistance = 64.0;

using FaceKey = std::pair<const mdl::BrushNode*, size_t>;

FaceKey keyOf(const mdl::BrushFaceHandle& handle)
{
  return {handle.node(), handle.faceIndex()};
}

double round3(const double value)
{
  return std::round(value * 1000.0) / 1000.0;
}

Json numbers(const vm::vec2d& v)
{
  return Json{round3(v.x()), round3(v.y())};
}

std::string format2(const vm::vec2d& v)
{
  return fmt::format("{:g} x {:g}", round3(v.x()), round3(v.y()));
}

std::string format3(const vm::vec3d& v)
{
  return fmt::format("[{:g}, {:g}, {:g}]", round3(v.x()), round3(v.y()), round3(v.z()));
}

/**
 * Describes what makes the texture of two coplanar faces diverge along their shared edge:
 * the scale and the rotation, or else the texture axes (e.g. Valve 220 axes that were
 * rotated or mirrored without changing the rotation value). Values that only differ
 * beyond 3 decimals are shown with more digits so that the message never reads "1 x 1 vs
 * 1 x 1".
 */
std::string linearDifference(const mdl::BrushFace& lhs, const mdl::BrushFace& rhs)
{
  const auto lhsUv = lhs.uvAttributes();
  const auto rhsUv = rhs.uvAttributes();
  const auto lhsScale = vm::vec2d{lhsUv.scale};
  const auto rhsScale = vm::vec2d{rhsUv.scale};
  const auto precise = [](const vm::vec2d& v) {
    return fmt::format("{:.7g} x {:.7g}", v.x(), v.y());
  };

  auto parts = std::vector<std::string>{};
  if (lhsScale != rhsScale)
  {
    parts.push_back(
      format2(lhsScale) != format2(rhsScale)
        ? fmt::format(
            "the scale differs ({} vs {})", format2(lhsScale), format2(rhsScale))
        : fmt::format(
            "the scale differs slightly ({} vs {})",
            precise(lhsScale),
            precise(rhsScale)));
  }
  if (lhsUv.rotation != rhsUv.rotation)
  {
    parts.push_back(fmt::format(
      "the rotation differs ({:.7g} vs {:.7g} degrees)",
      double(lhsUv.rotation),
      double(rhsUv.rotation)));
  }
  if (parts.empty())
  {
    parts.push_back(fmt::format(
      "the texture axes differ (U {} V {} vs U {} V {})",
      format3(lhs.uAxis()),
      format3(lhs.vAxis()),
      format3(rhs.uAxis()),
      format3(rhs.vAxis())));
  }

  auto result = std::string{};
  for (size_t i = 0; i < parts.size(); ++i)
  {
    result += (i == 0 ? "" : i + 1 == parts.size() ? " and " : ", ") + parts[i];
  }
  return result;
}

double maxAbs(const vm::vec2d& v)
{
  return std::max(std::abs(v.x()), std::abs(v.y()));
}

double sign(const double value)
{
  return value < 0.0 ? -1.0 : 1.0;
}

bool isSkippedKind(const MaterialKind kind)
{
  return kind == MaterialKind::Tool || kind == MaterialKind::Sky
         || kind == MaterialKind::Liquid;
}

bool isTileLike(const MaterialKind kind)
{
  return kind == MaterialKind::Tile || kind == MaterialKind::Unknown;
}

const char* axisName(const size_t axis)
{
  return axis == 0 ? "U" : "V";
}

/** The texture coordinate in texels of a point, as sampleFace computes it. */
vm::vec2d texelAt(const mdl::BrushFace& face, const vm::vec3d& point)
{
  const auto uv = face.uvAttributes();
  return vm::vec2d{
    vm::dot(point, face.uAxis()) / double(uv.scale.x()) + double(uv.offset.x()),
    vm::dot(point, face.vAxis()) / double(uv.scale.y()) + double(uv.offset.y()),
  };
}

/** The mean of the texel density along U and V. */
double meanDensity(const FaceSample& sample)
{
  return std::sqrt(sample.texelDensity.x() * sample.texelDensity.y());
}

/**
 * The segment that the edges of both polygons share (collinear and overlapping by more
 * than GeometryEpsilon), or nullopt.
 */
std::optional<std::pair<vm::vec3d, vm::vec3d>> sharedEdge(
  const std::vector<vm::vec3d>& lhs, const std::vector<vm::vec3d>& rhs)
{
  const auto onLine =
    [](const vm::vec3d& point, const vm::vec3d& origin, const vm::vec3d& direction) {
      const auto offset = point - origin;
      return vm::length(offset - direction * vm::dot(offset, direction))
             <= GeometryEpsilon;
    };

  for (size_t i = 0; i < lhs.size(); ++i)
  {
    const auto& a0 = lhs[i];
    const auto& a1 = lhs[(i + 1) % lhs.size()];
    const auto length = vm::length(a1 - a0);
    if (length <= GeometryEpsilon)
    {
      continue;
    }
    const auto direction = (a1 - a0) / length;
    for (size_t j = 0; j < rhs.size(); ++j)
    {
      const auto& b0 = rhs[j];
      const auto& b1 = rhs[(j + 1) % rhs.size()];
      if (!onLine(b0, a0, direction) || !onLine(b1, a0, direction))
      {
        continue;
      }
      const auto t0 = vm::dot(b0 - a0, direction);
      const auto t1 = vm::dot(b1 - a0, direction);
      const auto lo = std::max(0.0, std::min(t0, t1));
      const auto hi = std::min(length, std::max(t0, t1));
      if (hi - lo > GeometryEpsilon)
      {
        return std::pair{a0 + direction * lo, a0 + direction * hi};
      }
    }
  }
  return std::nullopt;
}

bool coplanar(const mdl::BrushFace& lhs, const mdl::BrushFace& rhs)
{
  return vm::dot(lhs.normal(), rhs.normal()) > ParallelDot
         && std::abs(lhs.boundary().distance - rhs.boundary().distance)
              <= GeometryEpsilon;
}

/** Wraps a texel difference into [-size / 2, size / 2]. */
double wrapped(const double difference, const double size)
{
  return difference - size * std::round(difference / size);
}

UvFix fix(std::string tool, Json arguments, std::string description)
{
  return UvFix{std::move(tool), std::move(arguments), false, std::move(description)};
}

struct FaceInfo
{
  mdl::BrushFaceHandle handle;
  std::optional<FaceSample> sample;
  const MaterialProfile* profile = nullptr;
  MaterialKind kind = MaterialKind::Unknown;
  std::vector<vm::vec3d> vertices;

  bool checked() const { return sample && profile && !isSkippedKind(kind); }
};

class Checker
{
private:
  const mdl::Map& m_map;
  const ProfileProvider& m_profiles;
  std::set<UvIssue> m_issues;
  std::set<FaceKey> m_targets;
  std::map<FaceKey, FaceInfo> m_infos;
  /** Pairs of faces already reported (seams). */
  std::set<std::pair<FaceKey, FaceKey>> m_reportedPairs;
  std::vector<UvFinding> m_findings;

public:
  Checker(
    const mdl::Map& map,
    const ProfileProvider& profiles,
    const std::vector<UvIssue>& only)
    : m_map{map}
    , m_profiles{profiles}
  {
    for (const auto& [issue, code] : Issues)
    {
      if (only.empty() || std::ranges::find(only, issue) != only.end())
      {
        m_issues.insert(issue);
      }
    }
  }

  std::vector<UvFinding> check(const std::vector<mdl::BrushFaceHandle>& faces)
  {
    auto unique = std::vector<mdl::BrushFaceHandle>{};
    for (const auto& handle : faces)
    {
      if (m_targets.insert(keyOf(handle)).second)
      {
        unique.push_back(handle);
      }
    }
    for (const auto& handle : unique)
    {
      checkFace(info(handle));
    }
    return std::move(m_findings);
  }

private:
  bool enabled(const UvIssue issue) const { return m_issues.contains(issue); }

  const FaceInfo& info(const mdl::BrushFaceHandle& handle)
  {
    const auto key = keyOf(handle);
    if (const auto it = m_infos.find(key); it != m_infos.end())
    {
      return it->second;
    }

    auto info = FaceInfo{handle, std::nullopt, nullptr, MaterialKind::Unknown, {}};
    const auto& face = handle.face();
    if (face.geometry())
    {
      info.vertices = face.vertexPositions();
      if (face.materialName() != mdl::BrushFace::NoMaterialName)
      {
        info.sample = sampleFace(face, textureSizeOf(face.material()));
        info.profile = &m_profiles(face.materialName());
        info.kind = checkedKind(*info.profile);
      }
    }
    return m_infos.emplace(key, std::move(info)).first->second;
  }

  void add(
    const UvIssue issue,
    const FaceInfo& face,
    const FaceInfo* neighbour,
    std::string message,
    Json measured,
    std::vector<UvFix> fixes)
  {
    m_findings.push_back(UvFinding{
      issue,
      face.handle,
      neighbour ? std::optional{neighbour->handle} : std::nullopt,
      face.handle.face().materialName(),
      std::move(message),
      std::move(measured),
      std::move(fixes),
    });
  }

  void checkFace(const FaceInfo& face)
  {
    if (!face.checked())
    {
      return;
    }
    if (enabled(UvIssue::AspectDistortion))
    {
      checkAspect(face);
    }
    if (enabled(UvIssue::FractionalRepeat))
    {
      checkRepeats(face);
    }
    if (enabled(UvIssue::PanelNotAligned))
    {
      checkAlignment(face);
    }
    if (enabled(UvIssue::UnusualScale))
    {
      checkScale(face);
    }
    if (enabled(UvIssue::TexelDensityMismatch) || enabled(UvIssue::Seam))
    {
      checkNeighbours(face);
    }
  }

  // UV_ASPECT_DISTORTION

  void checkAspect(const FaceInfo& face)
  {
    const auto& sample = *face.sample;
    const auto expected = expectedAspect(*face.profile);
    const auto aspect = sample.texelDensity.x() / sample.texelDensity.y();
    const auto deviation = aspect / expected.ratio;
    const auto off = std::max(deviation, 1.0 / deviation) - 1.0;
    if (off <= MaxAspectDeviation)
    {
      return;
    }

    const auto& uv = face.handle.face();
    const auto scale = vm::vec2d{uv.uvAttributes().scale};
    const auto uLength = vm::length(uv.uAxis());
    const auto vLength = vm::length(uv.vAxis());
    // keep U, give V the density that the expected ratio asks for
    const auto fixedV =
      sign(scale.y()) * (std::abs(scale.x()) / uLength) / expected.ratio * vLength;

    auto fixes = std::vector<UvFix>{};
    auto keepAspect = Json{{"operation", "fit"}, {"keepAspect", true}, {"round", true}};
    if (face.kind == MaterialKind::Panel && sample.repeats)
    {
      keepAspect["repeatU"] = std::max(1.0, std::round(sample.repeats->x()));
      fixes.push_back(fix(
        "uv_align",
        std::move(keepAspect),
        "Fit the panel a whole number of times along U; V follows with undistorted "
        "texels."));
      fixes.push_back(fix(
        "material_fit_geometry",
        Json::object(),
        "Or resize the face so that the panel fits at its typical scale."));
    }
    else
    {
      fixes.push_back(fix(
        "face_attributes_set",
        Json{{"scale", numbers(vm::vec2d{scale.x(), fixedV})}},
        "Give V the scale that makes the texels undistorted (U unchanged)."));
    }

    add(
      UvIssue::AspectDistortion,
      face,
      nullptr,
      fmt::format(
        "'{}' is stretched: {} world units per texel (U / V = {:g}, expected {:g} from "
        "{}), {:.0f}% off.",
        uv.materialName(),
        format2(sample.texelDensity),
        round3(aspect),
        round3(expected.ratio),
        expected.source,
        off * 100.0),
      Json{
        {"scale", numbers(scale)},
        {"texelDensity", numbers(sample.texelDensity)},
        {"aspect", round3(aspect)},
        {"expectedAspect", round3(expected.ratio)},
        {"expectedFrom", expected.source},
        {"deviation", round3(off)},
      },
      std::move(fixes));
  }

  // UV_FRACTIONAL_REPEAT

  void checkRepeats(const FaceInfo& face)
  {
    const auto& sample = *face.sample;
    if (!sample.repeats)
    {
      return;
    }

    auto axes = std::vector<size_t>{};
    if (face.kind == MaterialKind::Panel)
    {
      axes = {0, 1};
    }
    else if (face.kind == MaterialKind::Trim)
    {
      if (const auto across = trimAcrossAxis(*face.profile))
      {
        axes = {*across};
      }
    }

    auto fractional = Json::array();
    for (const auto axis : axes)
    {
      if (!sample.wholeRepeatAxes[axis])
      {
        fractional.push_back(axis == 0 ? "u" : "v");
      }
    }
    if (fractional.empty())
    {
      return;
    }

    const auto& repeats = *sample.repeats;
    const auto whole = vm::vec2d{
      std::max(1.0, std::round(repeats.x())), std::max(1.0, std::round(repeats.y()))};
    const auto geometry = fix(
      "material_fit_geometry",
      Json::object(),
      "Resize the face so that the texture fits whole repeats at its typical scale.");

    auto fixes = std::vector<UvFix>{};
    if (face.kind == MaterialKind::Panel)
    {
      // fitting both axes exactly distorts the texels if the face has other proportions
      const auto density = vm::vec2d{
        sample.worldSize.x() / (whole.x() * sample.textureSize->x()),
        sample.worldSize.y() / (whole.y() * sample.textureSize->y())};
      const auto expected = expectedAspect(*face.profile).ratio;
      const auto deviation = density.x() / density.y() / expected;
      const auto exactFit = fix(
        "uv_align",
        Json{{"operation", "fit"}, {"repeatU", whole.x()}, {"repeatV", whole.y()}},
        fmt::format("Fit the panel exactly {:g} x {:g} times.", whole.x(), whole.y()));
      if (std::max(deviation, 1.0 / deviation) - 1.0 <= MaxAspectDeviation)
      {
        fixes = {exactFit, geometry};
      }
      else
      {
        fixes = {
          geometry,
          fix(
            "uv_align",
            Json{
              {"operation", "fit"},
              {"repeatU", whole.x()},
              {"keepAspect", true},
              {"round", true}},
            "Or fit whole repeats keeping the texels undistorted."),
          exactFit,
        };
      }
    }
    else
    {
      const auto axis = fractional[0] == "u" ? size_t(0) : size_t(1);
      fixes = {
        fix(
          "uv_align",
          Json{
            {"operation", "fit"},
            {axis == 0 ? "repeatU" : "repeatV", whole[axis]},
            {"keepAspect", true}},
          fmt::format(
            "Fit the trim {:g} time(s) across ({}); the other axis follows.",
            whole[axis],
            axisName(axis))),
        geometry,
      };
    }

    add(
      UvIssue::FractionalRepeat,
      face,
      nullptr,
      fmt::format(
        "{} '{}' repeats {} times; it should repeat a whole number of times (at least "
        "once) along {}.",
        face.kind == MaterialKind::Panel ? "Panel" : "Trim",
        face.handle.face().materialName(),
        format2(repeats),
        fractional.size() == 2 ? std::string{"U and V"}
                               : std::string{fractional[0] == "u" ? "U" : "V"}),
      Json{
        {"kind", toString(face.kind)},
        {"repeats", numbers(repeats)},
        {"fractionalAxes", std::move(fractional)},
        {"textureSize", numbers(*sample.textureSize)},
        {"faceSize", numbers(sample.worldSize)},
      },
      std::move(fixes));
  }

  // UV_PANEL_NOT_ALIGNED

  void checkAlignment(const FaceInfo& face)
  {
    const auto& sample = *face.sample;
    if (face.kind != MaterialKind::Panel || !sample.textureSize || sample.aligned())
    {
      return;
    }

    const auto& size = *sample.textureSize;
    auto shift = vm::vec2d{0, 0};
    auto axes = Json::array();
    for (size_t axis = 0; axis < 2; ++axis)
    {
      if (!sample.alignedAxes[axis])
      {
        const auto start = sample.texelStart[axis];
        shift[axis] = start <= size[axis] / 2.0 ? -start : size[axis] - start;
        axes.push_back(axis == 0 ? "u" : "v");
      }
    }

    add(
      UvIssue::PanelNotAligned,
      face,
      nullptr,
      fmt::format(
        "Panel '{}' is not aligned to the face: its texture starts {} texels past the "
        "face edge.",
        face.handle.face().materialName(),
        format2(sample.texelStart)),
      Json{
        {"texelStart", numbers(sample.texelStart)},
        {"unalignedAxes", std::move(axes)},
        {"textureSize", numbers(size)},
      },
      {
        fix(
          "face_attributes_set",
          Json{{"offsetBy", numbers(shift)}},
          "Move the texture so that its edge lies on the face edge."),
        fix(
          "uv_align",
          Json{
            {"operation", "fit"}, {"repeatU", 1}, {"keepAspect", true}, {"round", true}},
          "Or fit and justify the panel."),
      });
  }

  // UV_UNUSUAL_SCALE

  void checkScale(const FaceInfo& face)
  {
    const auto& profile = *face.profile;
    auto low = vm::vec2d{};
    auto high = vm::vec2d{};
    auto source = std::string{};
    auto samples = uint64_t(0);
    if (profile.typicalScale && profile.typicalScale->source == "notes")
    {
      low = high = profile.typicalScale->value;
      source = "notes";
    }
    else if (
      profile.scaleRange
      && ((profile.scaleRange->source == "corpus" && profile.scaleRange->samples >= MinCorpusScaleSamples)
          || (profile.scaleRange->source == "map" && profile.scaleRange->samples >= MinMapSamples)))
    {
      low = profile.scaleRange->value.low;
      high = profile.scaleRange->value.high;
      source = profile.scaleRange->source;
      samples = profile.scaleRange->samples;
    }
    else
    {
      return;
    }

    const auto& scale = face.sample->scale;
    auto factor = 1.0;
    auto below = false;
    for (size_t axis = 0; axis < 2; ++axis)
    {
      if (low[axis] > 0.0 && low[axis] / scale[axis] > factor)
      {
        factor = low[axis] / scale[axis];
        below = true;
      }
      if (high[axis] > 0.0 && scale[axis] / high[axis] > factor)
      {
        factor = scale[axis] / high[axis];
        below = false;
      }
    }
    if (factor <= UnusualScaleFactor)
    {
      return;
    }

    const auto typical = profile.typicalScale ? profile.typicalScale->value : low;
    const auto signedScale = vm::vec2d{uvScale(face)};
    const auto range = low == high ? fmt::format("{:g}", round3(low.x()))
                                   : fmt::format(
                                       "{:g}-{:g} x {:g}-{:g}",
                                       round3(low.x()),
                                       round3(high.x()),
                                       round3(low.y()),
                                       round3(high.y()));
    add(
      UvIssue::UnusualScale,
      face,
      nullptr,
      fmt::format(
        "'{}' has scale {}, {:.1f}x {} the typical {} (from {}{}).",
        face.handle.face().materialName(),
        format2(scale),
        factor,
        below ? "below" : "above",
        low == high ? "scale " + format2(low) : "range " + range,
        source,
        samples > 0 ? fmt::format(", {} faces", samples) : std::string{}),
      Json{
        {"scale", numbers(scale)},
        {"typicalLow", numbers(low)},
        {"typicalHigh", numbers(high)},
        {"typicalScale", numbers(typical)},
        {"source", source},
        {"samples", samples},
        {"factor", round3(factor)},
        {"direction", below ? "below" : "above"},
      },
      {
        fix(
          "face_attributes_set",
          Json{
            {"scale",
             numbers(vm::vec2d{
               sign(signedScale.x()) * typical.x(),
               sign(signedScale.y()) * typical.y()})}},
          "Use the typical scale (offsets unchanged)."),
        fix(
          "uv_align",
          Json{{"operation", "typical"}},
          "Or apply the typical scale and justify the texture to the face."),
      });
  }

  static vm::vec2d uvScale(const FaceInfo& face)
  {
    return vm::vec2d{face.handle.face().uvAttributes().scale};
  }

  // neighbours: UV_TEXEL_DENSITY_MISMATCH and UV_SEAM

  /** Faces of the same brush sharing an edge and coplanar faces of other brushes. */
  std::vector<std::pair<const FaceInfo*, std::pair<vm::vec3d, vm::vec3d>>> neighbours(
    const FaceInfo& face)
  {
    auto result =
      std::vector<std::pair<const FaceInfo*, std::pair<vm::vec3d, vm::vec3d>>>{};
    const auto& brushFace = face.handle.face();
    auto* brushNode = face.handle.node();

    const auto& faces = brushNode->brush().faces();
    for (size_t i = 0; i < faces.size(); ++i)
    {
      if (i != face.handle.faceIndex() && faces[i].geometry())
      {
        const auto& other = info(mdl::BrushFaceHandle{brushNode, i});
        if (const auto edge = sharedEdge(face.vertices, other.vertices))
        {
          result.emplace_back(&other, *edge);
        }
      }
    }

    const auto bounds = brushFace.bounds().expand(GeometryEpsilon * 10.0);
    for (auto* node : m_map.worldNode().nodeTree().find_intersectors(bounds))
    {
      auto* other = dynamic_cast<mdl::BrushNode*>(node);
      if (!other || other == brushNode)
      {
        continue;
      }
      const auto& otherFaces = other->brush().faces();
      for (size_t i = 0; i < otherFaces.size(); ++i)
      {
        if (otherFaces[i].geometry() && coplanar(brushFace, otherFaces[i]))
        {
          const auto& otherFace = info(mdl::BrushFaceHandle{other, i});
          if (const auto edge = sharedEdge(face.vertices, otherFace.vertices))
          {
            result.emplace_back(&otherFace, *edge);
          }
        }
      }
    }
    return result;
  }

  /** How far the face's mean scale is from its material's typical scale (log ratio). */
  static double scaleDeviation(const FaceInfo& face)
  {
    const auto& profile = *face.profile;
    if (!profile.typicalScale)
    {
      return 0.0;
    }
    const auto typical =
      std::sqrt(profile.typicalScale->value.x() * profile.typicalScale->value.y());
    const auto actual = std::sqrt(face.sample->scale.x() * face.sample->scale.y());
    return typical > 0.0 ? std::abs(std::log(actual / typical)) : 0.0;
  }

  void checkNeighbours(const FaceInfo& face)
  {
    const auto key = keyOf(face.handle);
    const FaceInfo* worst = nullptr;
    auto worstRatio = DensityMismatchFactor;

    for (const auto& [neighbour, edge] : neighbours(face))
    {
      if (!neighbour->checked())
      {
        continue;
      }
      const auto neighbourKey = keyOf(neighbour->handle);
      const auto neighbourIsTarget = m_targets.contains(neighbourKey);

      if (
        enabled(UvIssue::TexelDensityMismatch) && isTileLike(face.kind)
        && isTileLike(neighbour->kind))
      {
        const auto lhs = meanDensity(*face.sample);
        const auto rhs = meanDensity(*neighbour->sample);
        const auto ratio = std::max(lhs, rhs) / std::min(lhs, rhs);
        // for two checked faces, the one further from its typical scale is reported
        const auto reportHere =
          !neighbourIsTarget || scaleDeviation(face) > scaleDeviation(*neighbour)
          || (scaleDeviation(face) == scaleDeviation(*neighbour) && key < neighbourKey);
        if (ratio > worstRatio && reportHere)
        {
          worst = neighbour;
          worstRatio = ratio;
        }
      }

      if (
        enabled(UvIssue::Seam) && neighbour->handle.node() != face.handle.node()
        && (isTileLike(face.kind) || face.kind == MaterialKind::Trim)
        && kdl::ci::str_is_equal(
          face.handle.face().materialName(), neighbour->handle.face().materialName())
        && face.sample->textureSize)
      {
        const auto pair = std::minmax(key, neighbourKey);
        if (!neighbourIsTarget || m_reportedPairs.insert(pair).second)
        {
          checkSeam(face, *neighbour, edge);
        }
      }
    }

    if (worst)
    {
      reportDensity(face, *worst, worstRatio);
    }
  }

  void reportDensity(const FaceInfo& face, const FaceInfo& neighbour, const double ratio)
  {
    const auto lhs = meanDensity(*face.sample);
    const auto rhs = meanDensity(*neighbour.sample);
    const auto scale = uvScale(face);
    const auto factor = rhs / lhs;

    auto fixes = std::vector<UvFix>{
      fix(
        "face_attributes_set",
        Json{{"scale", numbers(scale * factor)}},
        "Match the neighbour's texel density."),
    };
    if (kdl::ci::str_is_equal(
          face.handle.face().materialName(), neighbour.handle.face().materialName()))
    {
      auto copy = fix(
        "face_attributes_copy",
        Json{{"mode", "rotate"}},
        "Or copy the neighbour's alignment so that the texture wraps around the edge.");
      copy.neighbourAsSource = true;
      fixes.push_back(std::move(copy));
    }

    add(
      UvIssue::TexelDensityMismatch,
      face,
      &neighbour,
      fmt::format(
        "'{}' has {:.2f}x the texel size of its neighbour ('{}'): {:g} vs {:g} world "
        "units per texel.",
        face.handle.face().materialName(),
        lhs / rhs,
        neighbour.handle.face().materialName(),
        round3(lhs),
        round3(rhs)),
      Json{
        {"texelDensity", round3(lhs)},
        {"neighbourTexelDensity", round3(rhs)},
        {"ratio", round3(ratio)},
      },
      std::move(fixes));
  }

  void checkSeam(
    const FaceInfo& face,
    const FaceInfo& neighbour,
    const std::pair<vm::vec3d, vm::vec3d>& edge)
  {
    const auto& lhs = face.handle.face();
    const auto& rhs = neighbour.handle.face();
    const auto& size = *face.sample->textureSize;

    const auto [e0, e1] = edge;
    const auto along = vm::normalize(e1 - e0);
    const auto probe = e0 + vm::cross(lhs.normal(), along) * SeamProbeDistance;
    const auto difference = [&](const vm::vec3d& point) {
      return texelAt(lhs, point) - texelAt(rhs, point);
    };
    const auto d0 = difference(e0);
    const auto linearMismatch =
      std::max(maxAbs(difference(e1) - d0), maxAbs(difference(probe) - d0))
      > TexelTolerance;

    auto message = std::string{};
    auto measured = Json{
      {"scale", numbers(uvScale(face))},
      {"neighbourScale", numbers(uvScale(neighbour))},
      {"edge", Json{toJson(e0), toJson(e1)}},
    };
    auto fixes = std::vector<UvFix>{};
    auto copy = fix(
      "face_attributes_copy",
      Json{{"mode", "project"}},
      "Copy the neighbour's alignment so that the texture continues across the edge.");
    copy.neighbourAsSource = true;
    fixes.push_back(std::move(copy));

    if (linearMismatch)
    {
      message = fmt::format(
        "'{}' does not continue across the edge to its coplanar neighbour: {}.",
        lhs.materialName(),
        linearDifference(lhs, rhs));
      measured["mismatch"] = "scaleOrRotation";
      measured["rotation"] = round3(double(lhs.uvAttributes().rotation));
      measured["neighbourRotation"] = round3(double(rhs.uvAttributes().rotation));
    }
    else
    {
      const auto offset = vm::vec2d{wrapped(d0.x(), size.x()), wrapped(d0.y(), size.y())};
      if (
        std::abs(offset.x()) <= TexelTolerance && std::abs(offset.y()) <= TexelTolerance)
      {
        return;
      }
      message = fmt::format(
        "'{}' does not continue across the edge to its coplanar neighbour: the texture "
        "is "
        "shifted by {} texels.",
        lhs.materialName(),
        format2(offset));
      measured["mismatch"] = "offset";
      measured["offsetDifference"] = numbers(offset);
      fixes.push_back(fix(
        "face_attributes_set",
        Json{{"offsetBy", numbers(-offset)}},
        "Or shift this face's texture by the difference."));
    }

    add(
      UvIssue::Seam,
      face,
      &neighbour,
      std::move(message),
      std::move(measured),
      std::move(fixes));
  }
};

} // namespace

std::string_view toString(const UvIssue issue)
{
  for (const auto& [value, code] : Issues)
  {
    if (value == issue)
    {
      return code;
    }
  }
  return "UV_UNKNOWN";
}

std::optional<UvIssue> uvIssueFromString(const std::string_view code)
{
  for (const auto& [value, name] : Issues)
  {
    if (name == code)
    {
      return value;
    }
  }
  return std::nullopt;
}

std::vector<std::string> uvIssueCodes()
{
  auto result = std::vector<std::string>{};
  for (const auto& [value, code] : Issues)
  {
    result.emplace_back(code);
  }
  return result;
}

ExpectedAspect expectedAspect(const MaterialProfile& profile)
{
  if (
    profile.typicalScale
    && (profile.typicalScale->source == "notes" || profile.typicalScale->source == "corpus"))
  {
    const auto& scale = profile.typicalScale->value;
    if (scale.x() > 0.0 && scale.y() > 0.0)
    {
      return ExpectedAspect{scale.x() / scale.y(), profile.typicalScale->source};
    }
  }
  return ExpectedAspect{};
}

MaterialKind checkedKind(const MaterialProfile& profile)
{
  if (
    profile.kind.source == "map" && profile.kind.samples < MinMapSamples && profile.image
    && profile.image->suggestedKind != MaterialKind::Unknown)
  {
    return profile.image->suggestedKind;
  }
  return profile.kind.value;
}

std::optional<size_t> trimAcrossAxis(const MaterialProfile& profile)
{
  if (profile.textureSize && profile.textureSize->x() != profile.textureSize->y())
  {
    return profile.textureSize->x() < profile.textureSize->y() ? 0 : 1;
  }
  if (profile.image && profile.image->tilesU != profile.image->tilesV)
  {
    return profile.image->tilesU ? 1 : 0;
  }
  return std::nullopt;
}

Json toJson(const UvFinding& finding, const IdRegistry& ids)
{
  const auto faceId = ids.formatFace(*finding.face.node(), finding.face.faceIndex());
  const auto neighbourId =
    finding.neighbour ? std::optional{ids.formatFace(
                          *finding.neighbour->node(), finding.neighbour->faceIndex())}
                      : std::nullopt;

  const auto fixJson = [&](const UvFix& fix) {
    auto arguments = Json::object();
    if (fix.tool == "material_fit_geometry")
    {
      arguments["face"] = faceId;
    }
    else
    {
      arguments["ids"] = Json{faceId};
    }
    if (fix.neighbourAsSource && neighbourId)
    {
      arguments["source"] = *neighbourId;
    }
    for (const auto& [key, value] : fix.arguments.items())
    {
      arguments[key] = value;
    }
    return Json{
      {"tool", fix.tool},
      {"arguments", std::move(arguments)},
      {"description", fix.description},
    };
  };

  auto result = Json{
    {"code", toString(finding.issue)},
    {"face", faceId},
    {"brush", ids.format(*finding.face.node())},
    {"material", finding.material},
    {"message", finding.message},
    {"measured", finding.measured},
  };
  if (neighbourId)
  {
    result["neighbour"] = *neighbourId;
  }
  if (!finding.fixes.empty())
  {
    result["fix"] = fixJson(finding.fixes.front());
    auto alternatives = Json::array();
    for (size_t i = 1; i < finding.fixes.size(); ++i)
    {
      alternatives.push_back(fixJson(finding.fixes[i]));
    }
    result["alternatives"] = std::move(alternatives);
  }
  return result;
}

ProfileProvider profileProvider(MaterialKnowledge& knowledge)
{
  return [&knowledge](const std::string& name) -> const MaterialProfile& {
    const auto& profile = knowledge.profile(name);
    if (profile.kind.source == "map" && profile.kind.samples < MinMapSamples)
    {
      return knowledge.profile(name, true);
    }
    return profile;
  };
}

std::vector<UvFinding> checkUv(
  const std::vector<mdl::BrushFaceHandle>& faces,
  const mdl::Map& map,
  const ProfileProvider& profiles,
  const std::vector<UvIssue>& only)
{
  return Checker{map, profiles, only}.check(faces);
}

} // namespace tb::mcp
