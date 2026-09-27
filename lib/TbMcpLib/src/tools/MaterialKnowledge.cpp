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

#include "mcp/tools/MaterialKnowledge.h"

#include "NodeJson.h"
#include "gl/Material.h"
#include "gl/MaterialManager.h"
#include "gl/Texture.h"
#include "mcp/JsonVm.h"
#include "mcp/tools/AssetUtils.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/Map_World.h"
#include "mdl/Node.h"
#include "mdl/Tag.h"
#include "mdl/TagAttribute.h"
#include "mdl/TagManager.h"
#include "mdl/UvAttributes.h"
#include "mdl/WorldNode.h"

#include "kd/string_compare.h"
#include "kd/string_format.h"

#include "vm/vec.h"

#include <fmt/format.h>
#include <fmt/std.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <system_error>
#include <utility>

namespace tb::mcp
{
namespace
{

double roundTo(const double value, const double step)
{
  return roundForOutput(std::round(value / step) * step);
}

std::string lower(const std::string_view str)
{
  return kdl::str_to_lower(str);
}

bool containsAny(const std::string& str, const std::vector<std::string_view>& words)
{
  return std::ranges::any_of(
    words, [&](const auto word) { return str.find(word) != std::string::npos; });
}

Json vec2Json(const vm::vec2d& v)
{
  return Json::array({roundForOutput(v.x()), roundForOutput(v.y())});
}

Json optionalVec2Json(const std::optional<vm::vec2d>& v)
{
  return v ? vec2Json(*v) : Json(nullptr);
}

Json optionalNumberJson(const std::optional<double>& value)
{
  return value ? Json(roundForOutput(*value)) : Json(nullptr);
}

std::optional<vm::vec2d> vec2Member(const Json& json, const std::string_view key)
{
  const auto* value = findMember(json, key);
  return value ? vec2FromJson(*value) : std::nullopt;
}

template <typename T>
std::optional<T> member(const Json& json, const std::string_view key)
{
  const auto* value = findMember(json, key);
  if (!value)
  {
    return std::nullopt;
  }
  try
  {
    return value->get<T>();
  }
  catch (const Json::exception&)
  {
    return std::nullopt;
  }
}

} // namespace

// Material kinds

namespace
{

constexpr auto KindNames = std::array<std::pair<MaterialKind, std::string_view>, 8>{{
  {MaterialKind::Unknown, "unknown"},
  {MaterialKind::Panel, "panel"},
  {MaterialKind::Tile, "tile"},
  {MaterialKind::Trim, "trim"},
  {MaterialKind::Decal, "decal"},
  {MaterialKind::Sky, "sky"},
  {MaterialKind::Liquid, "liquid"},
  {MaterialKind::Tool, "tool"},
}};

const auto LiquidWords =
  std::vector<std::string_view>{"liquid", "water", "lava", "slime"};

const auto ToolWords = std::vector<std::string_view>{
  "clip",
  "skip",
  "hint",
  "origin",
  "null",
  "nodraw",
  "caulk",
  "trigger",
  "areaportal",
  "portal",
  "bevel",
  "nodrop",
  "donotenter",
};

} // namespace

std::string_view toString(const MaterialKind kind)
{
  for (const auto& [value, name] : KindNames)
  {
    if (value == kind)
    {
      return name;
    }
  }
  return "unknown";
}

std::optional<MaterialKind> materialKindFromString(const std::string_view name)
{
  for (const auto& [value, kindName] : KindNames)
  {
    if (kdl::ci::str_is_equal(kindName, name))
    {
      return value;
    }
  }
  return std::nullopt;
}

std::vector<std::string> materialKindNames()
{
  auto result = std::vector<std::string>{};
  for (const auto& [value, name] : KindNames)
  {
    if (value != MaterialKind::Unknown)
    {
      result.emplace_back(name);
    }
  }
  return result;
}

std::optional<MaterialKind> kindFromName(const std::string_view materialName)
{
  const auto name = lower(materialName);
  if (name.find("sky") != std::string::npos)
  {
    return MaterialKind::Sky;
  }
  if (
    (!name.empty() && (name.front() == '*' || name.front() == '!'))
    || containsAny(name, {"water", "lava", "slime"}))
  {
    return MaterialKind::Liquid;
  }
  return std::nullopt;
}

std::optional<ConfigKind> kindFromConfig(
  const mdl::Map& map, const std::string_view materialName)
{
  auto probe = mdl::BrushFace::create(
    vm::vec3d{0, 0, 0},
    vm::vec3d{0, 1, 0},
    vm::vec3d{1, 0, 0},
    std::string{materialName},
    mdl::UvAttributes{},
    mdl::SurfaceAttributes{},
    mdl::MapFormat::Standard);
  if (probe.is_error())
  {
    return std::nullopt;
  }
  auto face = std::move(probe).value();
  if (const auto* material = map.materialManager().material(std::string{materialName}))
  {
    // the face only references the material (a usage count that the probe face releases
    // again), so that surfaceparm and flag matchers see the material's defaults
    face.setMaterial(const_cast<gl::Material*>(material));
  }

  for (const auto& tag : map.tagManager().smartTags())
  {
    if (!tag.matches(face))
    {
      continue;
    }
    const auto name = lower(tag.name());
    if (name.find("sky") != std::string::npos)
    {
      return ConfigKind{MaterialKind::Sky, tag.name()};
    }
    if (containsAny(name, LiquidWords))
    {
      return ConfigKind{MaterialKind::Liquid, tag.name()};
    }
    const auto transparent = std::ranges::any_of(tag.attributes(), [](const auto& attr) {
      return attr == mdl::TagAttributes::Transparency;
    });
    if (
      containsAny(name, ToolWords)
      || (transparent && name.find("trans") == std::string::npos))
    {
      return ConfigKind{MaterialKind::Tool, tag.name()};
    }
  }
  return std::nullopt;
}

// Face sampling

std::optional<vm::vec2d> textureSizeOf(const gl::Material* material)
{
  if (const auto* texture = gl::getTexture(material))
  {
    if (texture->width() > 0 && texture->height() > 0)
    {
      return vm::vec2d{double(texture->width()), double(texture->height())};
    }
  }
  return std::nullopt;
}

namespace
{

/** Whether the coordinate is on a texture edge, i.e. a multiple of the size. */
bool onTextureEdge(const double coordinate, const double size)
{
  const auto remainder = coordinate - std::floor(coordinate / size) * size;
  return remainder <= TexelTolerance || size - remainder <= TexelTolerance;
}

} // namespace

std::optional<FaceSample> sampleFace(
  const mdl::BrushFace& face, const std::optional<vm::vec2d> textureSize)
{
  if (!face.geometry())
  {
    return std::nullopt;
  }

  const auto uv = face.uvAttributes();
  const auto scale = vm::vec2d{uv.scale};
  const auto offset = vm::vec2d{uv.offset};
  const auto uAxis = face.uAxis();
  const auto vAxis = face.vAxis();
  const auto uLength = vm::length(uAxis);
  const auto vLength = vm::length(vAxis);
  constexpr auto epsilon = 1e-6;
  if (
    std::abs(scale.x()) < epsilon || std::abs(scale.y()) < epsilon || uLength < epsilon
    || vLength < epsilon)
  {
    return std::nullopt;
  }

  const auto vertices = face.vertexPositions();
  if (vertices.size() < 3)
  {
    return std::nullopt;
  }

  constexpr auto inf = std::numeric_limits<double>::infinity();
  auto texelMin = vm::vec2d{inf, inf};
  auto texelMax = vm::vec2d{-inf, -inf};
  auto worldMin = vm::vec2d{inf, inf};
  auto worldMax = vm::vec2d{-inf, -inf};
  for (const auto& vertex : vertices)
  {
    // the texture coordinates in texels, like mdl::computeUvCoords plus the offset
    const auto u = vm::dot(vertex, uAxis);
    const auto v = vm::dot(vertex, vAxis);
    const auto texel = vm::vec2d{u / scale.x() + offset.x(), v / scale.y() + offset.y()};
    const auto world = vm::vec2d{u / uLength, v / vLength};
    texelMin = vm::min(texelMin, texel);
    texelMax = vm::max(texelMax, texel);
    worldMin = vm::min(worldMin, world);
    worldMax = vm::max(worldMax, world);
  }

  auto sample = FaceSample{};
  sample.scale = vm::abs(scale);
  sample.flipped = {scale.x() < 0.0, scale.y() < 0.0};
  sample.rotation = faceRotation(face);
  sample.texelDensity = vm::vec2d{sample.scale.x() / uLength, sample.scale.y() / vLength};
  sample.worldSize = worldMax - worldMin;
  sample.texelSize = texelMax - texelMin;
  sample.texelStart = texelMin;

  if (textureSize && textureSize->x() > 0.0 && textureSize->y() > 0.0)
  {
    const auto& size = *textureSize;
    sample.textureSize = size;
    sample.repeats =
      vm::vec2d{sample.texelSize.x() / size.x(), sample.texelSize.y() / size.y()};
    for (size_t axis = 0; axis < 2; ++axis)
    {
      const auto s = size[axis];
      sample.texelStart[axis] = texelMin[axis] - std::floor(texelMin[axis] / s) * s;
      sample.alignedAxes[axis] =
        onTextureEdge(texelMin[axis], s) || onTextureEdge(texelMax[axis], s);
      const auto count = std::round(sample.texelSize[axis] / s);
      sample.wholeRepeatAxes[axis] =
        count >= 1.0 && std::abs(sample.texelSize[axis] - count * s) <= TexelTolerance;
    }
  }
  return sample;
}

// Histogram

Histogram::Histogram(const size_t dimensions)
  : m_dimensions{dimensions}
{
}

size_t Histogram::dimensions() const
{
  return m_dimensions;
}

const std::map<Histogram::Key, uint64_t>& Histogram::entries() const
{
  return m_entries;
}

uint64_t Histogram::total() const
{
  return m_total;
}

bool Histogram::empty() const
{
  return m_total == 0;
}

void Histogram::add(Key key, const uint64_t count)
{
  if (count == 0)
  {
    return;
  }
  if (m_dimensions < 2)
  {
    key[1] = 0.0;
  }
  if (m_total == 0)
  {
    m_min = key;
    m_max = key;
  }
  else
  {
    for (size_t axis = 0; axis < 2; ++axis)
    {
      m_min[axis] = std::min(m_min[axis], key[axis]);
      m_max[axis] = std::max(m_max[axis], key[axis]);
    }
  }
  m_entries[key] += count;
  m_total += count;
}

void Histogram::merge(const Histogram& other)
{
  if (other.m_total == 0)
  {
    return;
  }
  const auto hadValues = m_total > 0;
  for (const auto& [key, count] : other.m_entries)
  {
    m_entries[key] += count;
  }
  m_total += other.m_total;
  for (size_t axis = 0; axis < 2; ++axis)
  {
    m_min[axis] =
      hadValues ? std::min(m_min[axis], other.m_min[axis]) : other.m_min[axis];
    m_max[axis] =
      hadValues ? std::max(m_max[axis], other.m_max[axis]) : other.m_max[axis];
  }
}

void Histogram::cap(const size_t maxEntries)
{
  if (m_entries.size() <= maxEntries || maxEntries == 0)
  {
    return;
  }

  auto sorted = std::vector<std::pair<Key, uint64_t>>{m_entries.begin(), m_entries.end()};
  std::ranges::stable_sort(
    sorted, [](const auto& lhs, const auto& rhs) { return lhs.second > rhs.second; });

  auto kept = std::map<Key, uint64_t>{sorted.begin(), sorted.begin() + long(maxEntries)};
  for (auto it = sorted.begin() + long(maxEntries); it != sorted.end(); ++it)
  {
    const auto& key = it->first;
    auto nearest = kept.begin();
    auto nearestDistance = std::numeric_limits<double>::infinity();
    for (auto candidate = kept.begin(); candidate != kept.end(); ++candidate)
    {
      auto distance = 0.0;
      for (size_t axis = 0; axis < m_dimensions; ++axis)
      {
        const auto d = candidate->first[axis] - key[axis];
        distance += d * d;
      }
      if (distance < nearestDistance)
      {
        nearestDistance = distance;
        nearest = candidate;
      }
    }
    nearest->second += it->second;
  }
  m_entries = std::move(kept);
}

std::optional<Histogram::Key> Histogram::mode() const
{
  auto result = std::optional<Key>{};
  auto best = uint64_t(0);
  for (const auto& [key, count] : m_entries)
  {
    if (count > best)
    {
      best = count;
      result = key;
    }
  }
  return result;
}

std::optional<double> Histogram::percentile(
  const size_t axis, const double fraction) const
{
  if (m_total == 0 || axis >= m_dimensions)
  {
    return std::nullopt;
  }
  auto marginal = std::map<double, uint64_t>{};
  for (const auto& [key, count] : m_entries)
  {
    marginal[key[axis]] += count;
  }
  const auto rank = std::max(
    uint64_t(1),
    uint64_t(std::ceil(std::clamp(fraction, 0.0, 1.0) * double(m_total) - 1e-9)));
  auto cumulative = uint64_t(0);
  for (const auto& [value, count] : marginal)
  {
    cumulative += count;
    if (cumulative >= rank)
    {
      return value;
    }
  }
  return marginal.rbegin()->first;
}

std::optional<double> Histogram::min(const size_t axis) const
{
  return m_total > 0 && axis < m_dimensions ? std::optional{m_min[axis]} : std::nullopt;
}

std::optional<double> Histogram::max(const size_t axis) const
{
  return m_total > 0 && axis < m_dimensions ? std::optional{m_max[axis]} : std::nullopt;
}

Json Histogram::toJson() const
{
  auto entries = Json::array();
  for (const auto& [key, count] : m_entries)
  {
    auto entry = Json::array();
    for (size_t axis = 0; axis < m_dimensions; ++axis)
    {
      entry.push_back(roundForOutput(key[axis]));
    }
    entry.push_back(count);
    entries.push_back(std::move(entry));
  }
  auto lo = Json::array();
  auto hi = Json::array();
  for (size_t axis = 0; axis < m_dimensions; ++axis)
  {
    lo.push_back(roundForOutput(m_min[axis]));
    hi.push_back(roundForOutput(m_max[axis]));
  }
  return Json{{"e", std::move(entries)}, {"lo", std::move(lo)}, {"hi", std::move(hi)}};
}

std::optional<Histogram> Histogram::fromJson(const Json& json, const size_t dimensions)
{
  const auto* entries = findMember(json, "e");
  if (!entries || !entries->is_array())
  {
    return std::nullopt;
  }

  auto result = Histogram{dimensions};
  for (const auto& entry : *entries)
  {
    if (!entry.is_array() || entry.size() != dimensions + 1)
    {
      return std::nullopt;
    }
    for (const auto& value : entry)
    {
      if (!value.is_number())
      {
        return std::nullopt;
      }
    }
    auto key = Key{0, 0};
    for (size_t axis = 0; axis < dimensions; ++axis)
    {
      key[axis] = entry[axis].get<double>();
    }
    const auto count = entry[dimensions].get<double>();
    if (count < 0)
    {
      return std::nullopt;
    }
    result.add(key, uint64_t(count));
  }

  // restore the exact extremes, which capping does not lose
  const auto* lo = findMember(json, "lo");
  const auto* hi = findMember(json, "hi");
  if (
    result.m_total > 0 && lo && hi && lo->is_array() && hi->is_array()
    && lo->size() == dimensions && hi->size() == dimensions)
  {
    for (size_t axis = 0; axis < dimensions; ++axis)
    {
      if ((*lo)[axis].is_number() && (*hi)[axis].is_number())
      {
        result.m_min[axis] = std::min(result.m_min[axis], (*lo)[axis].get<double>());
        result.m_max[axis] = std::max(result.m_max[axis], (*hi)[axis].get<double>());
      }
    }
  }
  return result;
}

// MaterialStats

void MaterialStats::add(const FaceSample& sample)
{
  samples += 1;
  scale.add({roundTo(sample.scale.x(), 0.001), roundTo(sample.scale.y(), 0.001)});
  worldSize.add({roundTo(sample.worldSize.x(), 1.0), roundTo(sample.worldSize.y(), 1.0)});
  texelSize.add({roundTo(sample.texelSize.x(), 1.0), roundTo(sample.texelSize.y(), 1.0)});
  const auto degrees = roundTo(sample.rotation, 1.0);
  rotation.add({degrees >= 360.0 ? degrees - 360.0 : degrees, 0.0});

  if (sample.repeats)
  {
    sizedSamples += 1;
    repeats.add({roundTo(sample.repeats->x(), 0.01), roundTo(sample.repeats->y(), 0.01)});
    wholeRepeats[0] += sample.wholeRepeatAxes[0] ? 1u : 0u;
    wholeRepeats[1] += sample.wholeRepeatAxes[1] ? 1u : 0u;
    wholeRepeats[2] += sample.wholeRepeats() ? 1u : 0u;
    aligned[0] += sample.alignedAxes[0] ? 1u : 0u;
    aligned[1] += sample.alignedAxes[1] ? 1u : 0u;
    aligned[2] += sample.aligned() ? 1u : 0u;
  }
}

void MaterialStats::merge(const MaterialStats& other)
{
  samples += other.samples;
  sizedSamples += other.sizedSamples;
  for (size_t i = 0; i < 3; ++i)
  {
    wholeRepeats[i] += other.wholeRepeats[i];
    aligned[i] += other.aligned[i];
  }
  scale.merge(other.scale);
  worldSize.merge(other.worldSize);
  texelSize.merge(other.texelSize);
  repeats.merge(other.repeats);
  rotation.merge(other.rotation);
}

void MaterialStats::cap()
{
  scale.cap(MaxHistogramEntries);
  worldSize.cap(MaxHistogramEntries);
  texelSize.cap(MaxHistogramEntries);
  repeats.cap(MaxHistogramEntries);
  rotation.cap(MaxHistogramEntries / 2);
}

Json toJson(const MaterialStats& stats)
{
  return Json{
    {"n", stats.samples},
    {"sized", stats.sizedSamples},
    {"whole", stats.wholeRepeats},
    {"aligned", stats.aligned},
    {"scale", stats.scale.toJson()},
    {"size", stats.worldSize.toJson()},
    {"texels", stats.texelSize.toJson()},
    {"repeats", stats.repeats.toJson()},
    {"rotation", stats.rotation.toJson()},
  };
}

std::optional<MaterialStats> materialStatsFromJson(const Json& json)
{
  if (!json.is_object())
  {
    return std::nullopt;
  }
  const auto samples = member<uint64_t>(json, "n");
  const auto sized = member<uint64_t>(json, "sized");
  const auto whole = member<std::array<uint64_t, 3>>(json, "whole");
  const auto aligned = member<std::array<uint64_t, 3>>(json, "aligned");
  const auto histogram = [&](const std::string_view key, const size_t dimensions) {
    const auto* value = findMember(json, key);
    return value ? Histogram::fromJson(*value, dimensions) : std::nullopt;
  };
  auto scale = histogram("scale", 2);
  auto size = histogram("size", 2);
  auto texels = histogram("texels", 2);
  auto repeats = histogram("repeats", 2);
  auto rotation = histogram("rotation", 1);
  if (
    !samples || !sized || !whole || !aligned || !scale || !size || !texels || !repeats
    || !rotation)
  {
    return std::nullopt;
  }

  auto result = MaterialStats{};
  result.samples = *samples;
  result.sizedSamples = *sized;
  result.wholeRepeats = *whole;
  result.aligned = *aligned;
  result.scale = std::move(*scale);
  result.worldSize = std::move(*size);
  result.texelSize = std::move(*texels);
  result.repeats = std::move(*repeats);
  result.rotation = std::move(*rotation);
  return result;
}

Json toJson(const ScaleRange& range)
{
  return Json{
    {"min", vec2Json(range.min)},
    {"low", vec2Json(range.low)},
    {"high", vec2Json(range.high)},
    {"max", vec2Json(range.max)},
  };
}

// Summaries

namespace
{

std::optional<vm::vec2d> modeOf(const Histogram& histogram)
{
  if (const auto mode = histogram.mode())
  {
    return vm::vec2d{(*mode)[0], (*mode)[1]};
  }
  return std::nullopt;
}

std::optional<vm::vec2d> percentileOf(const Histogram& histogram, const double fraction)
{
  const auto u = histogram.percentile(0, fraction);
  const auto v = histogram.percentile(1, fraction);
  return u && v ? std::optional{vm::vec2d{*u, *v}} : std::nullopt;
}

bool isWholeTexelCount(const double texels, const double size)
{
  const auto count = std::round(texels / size);
  return count >= 1.0 && std::abs(texels - count * size) <= TexelTolerance;
}

} // namespace

StatsSummary summarize(
  const MaterialStats& stats, const std::optional<vm::vec2d> textureSize)
{
  auto result = StatsSummary{};
  result.samples = stats.samples;
  if (stats.samples == 0)
  {
    return result;
  }

  result.typicalScale = modeOf(stats.scale);
  if (const auto min = stats.scale.min(0),
      max = stats.scale.max(0),
      minV = stats.scale.min(1),
      maxV = stats.scale.max(1);
      min && max && minV && maxV)
  {
    result.scaleRange = ScaleRange{
      vm::vec2d{*min, *minV},
      *percentileOf(stats.scale, 0.1),
      *percentileOf(stats.scale, 0.9),
      vm::vec2d{*max, *maxV},
    };
  }
  result.typicalFaceSize = modeOf(stats.worldSize);
  if (const auto rotation = stats.rotation.mode())
  {
    result.typicalRotation = (*rotation)[0];
  }

  auto repeats = std::optional<Histogram>{};
  if (stats.sizedSamples > 0)
  {
    const auto sized = double(stats.sizedSamples);
    result.sizedSamples = stats.sizedSamples;
    repeats = stats.repeats;
    result.wholeRepeatFraction = double(stats.wholeRepeats[2]) / sized;
    result.wholeRepeatFractionPerAxis = vm::vec2d{
      double(stats.wholeRepeats[0]) / sized, double(stats.wholeRepeats[1]) / sized};
    result.alignedFraction = double(stats.aligned[2]) / sized;
    result.alignedFractionPerAxis =
      vm::vec2d{double(stats.aligned[0]) / sized, double(stats.aligned[1]) / sized};
  }
  else if (textureSize && textureSize->x() > 0.0 && textureSize->y() > 0.0)
  {
    // derive the repeats from the texel extents; the alignment stays unknown
    const auto& size = *textureSize;
    auto derived = Histogram{2};
    auto whole = std::array<uint64_t, 3>{0, 0, 0};
    for (const auto& [key, count] : stats.texelSize.entries())
    {
      derived.add(
        {roundTo(key[0] / size.x(), 0.01), roundTo(key[1] / size.y(), 0.01)}, count);
      const auto wholeU = isWholeTexelCount(key[0], size.x());
      const auto wholeV = isWholeTexelCount(key[1], size.y());
      whole[0] += wholeU ? count : 0;
      whole[1] += wholeV ? count : 0;
      whole[2] += wholeU && wholeV ? count : 0;
    }
    const auto sized = double(stats.texelSize.total());
    result.sizedSamples = stats.texelSize.total();
    repeats = std::move(derived);
    result.wholeRepeatFraction = double(whole[2]) / sized;
    result.wholeRepeatFractionPerAxis =
      vm::vec2d{double(whole[0]) / sized, double(whole[1]) / sized};
  }

  if (repeats && !repeats->empty())
  {
    result.typicalRepeats = modeOf(*repeats);
    result.medianRepeats = percentileOf(*repeats, 0.5);
    const auto single = [](const double value) { return value <= 1.05; };
    result.singleRepeatFractionPerAxis =
      vm::vec2d{repeats->fraction(0, single), repeats->fraction(1, single)};
  }
  return result;
}

Json toJson(const StatsSummary& summary)
{
  const auto kind = kindFromStats(summary);
  return Json{
    {"samples", summary.samples},
    {"sizedSamples", summary.sizedSamples},
    {"kind", kind ? Json(toString(*kind)) : Json(nullptr)},
    {"typicalScale", optionalVec2Json(summary.typicalScale)},
    {"scaleRange", summary.scaleRange ? toJson(*summary.scaleRange) : Json(nullptr)},
    {"typicalFaceSize", optionalVec2Json(summary.typicalFaceSize)},
    {"typicalRepeats", optionalVec2Json(summary.typicalRepeats)},
    {"medianRepeats", optionalVec2Json(summary.medianRepeats)},
    {"wholeRepeatFraction", optionalNumberJson(summary.wholeRepeatFraction)},
    {"alignedFraction", optionalNumberJson(summary.alignedFraction)},
    {"typicalRotation", optionalNumberJson(summary.typicalRotation)},
  };
}

std::optional<MaterialKind> kindFromStats(const StatsSummary& summary)
{
  if (
    summary.sizedSamples < MinKindSamples || !summary.wholeRepeatFraction
    || !summary.medianRepeats || !summary.singleRepeatFractionPerAxis)
  {
    return std::nullopt;
  }

  const auto whole = *summary.wholeRepeatFraction;
  const auto& median = *summary.medianRepeats;
  const auto& single = *summary.singleRepeatFractionPerAxis;
  const auto alignedAxis = [&](const size_t axis) {
    return !summary.alignedFractionPerAxis
           || (*summary.alignedFractionPerAxis)[axis] >= 0.75;
  };

  if (
    whole >= 0.75 && median.x() <= 2.0 && median.y() <= 2.0
    && (!summary.alignedFraction || *summary.alignedFraction >= 0.75))
  {
    return MaterialKind::Panel;
  }
  for (size_t axis = 0; axis < 2; ++axis)
  {
    if (single[axis] >= 0.75 && alignedAxis(axis) && single[1 - axis] < 0.5)
    {
      return MaterialKind::Trim;
    }
  }
  if (whole < 0.6 || median.x() > 2.0 || median.y() > 2.0)
  {
    return MaterialKind::Tile;
  }
  return std::nullopt;
}

// Image analysis

namespace
{

double colorDifference(const unsigned char* lhs, const unsigned char* rhs)
{
  const auto lhsTransparent = lhs[3] < 128;
  const auto rhsTransparent = rhs[3] < 128;
  if (lhsTransparent || rhsTransparent)
  {
    return lhsTransparent == rhsTransparent ? 0.0 : 1.0;
  }
  return double(
           std::abs(int(lhs[0]) - int(rhs[0])) + std::abs(int(lhs[1]) - int(rhs[1]))
           + std::abs(int(lhs[2]) - int(rhs[2])))
         / (3.0 * 255.0);
}

} // namespace

ImageAnalysis analyzeImage(const RgbaImage& image)
{
  auto result = ImageAnalysis{};
  result.width = image.width;
  result.height = image.height;
  if (
    image.width == 0 || image.height == 0
    || image.pixels.size() < image.width * image.height * 4)
  {
    return result;
  }

  const auto w = image.width;
  const auto h = image.height;
  const auto pixel = [&](const size_t x, const size_t y) {
    return image.pixels.data() + (y * w + x) * 4;
  };
  result.aspectRatio = double(w) / double(h);

  auto transparent = size_t(0);
  for (size_t i = 0; i < w * h; ++i)
  {
    transparent += image.pixels[i * 4 + 3] < 128 ? 1u : 0u;
  }
  result.transparentFraction = double(transparent) / double(w * h);

  // across the seam when repeating along U: the right column next to the left column
  auto edgeU = 0.0;
  for (size_t y = 0; y < h; ++y)
  {
    edgeU += colorDifference(pixel(w - 1, y), pixel(0, y));
  }
  result.edgeDifference[0] = edgeU / double(h);

  auto innerU = 0.0;
  for (size_t y = 0; y < h; ++y)
  {
    for (size_t x = 0; x + 1 < w; ++x)
    {
      innerU += colorDifference(pixel(x, y), pixel(x + 1, y));
    }
  }
  result.innerDifference[0] = w > 1 ? innerU / double((w - 1) * h) : 0.0;

  auto edgeV = 0.0;
  for (size_t x = 0; x < w; ++x)
  {
    edgeV += colorDifference(pixel(x, h - 1), pixel(x, 0));
  }
  result.edgeDifference[1] = edgeV / double(w);

  auto innerV = 0.0;
  for (size_t y = 0; y + 1 < h; ++y)
  {
    for (size_t x = 0; x < w; ++x)
    {
      innerV += colorDifference(pixel(x, y), pixel(x, y + 1));
    }
  }
  result.innerDifference[1] = h > 1 ? innerV / double(w * (h - 1)) : 0.0;

  const auto tiles = [](const double edge, const double inner) {
    return edge <= 1.5 * inner + 0.03;
  };
  result.tilesU = tiles(result.edgeDifference[0], result.innerDifference[0]);
  result.tilesV = tiles(result.edgeDifference[1], result.innerDifference[1]);

  const auto elongation = double(std::max(w, h)) / double(std::min(w, h));
  if (result.transparentFraction >= 0.25)
  {
    result.suggestedKind = MaterialKind::Decal;
  }
  else if (result.tilesU && result.tilesV)
  {
    result.suggestedKind = elongation >= 4.0 ? MaterialKind::Trim : MaterialKind::Tile;
  }
  else if (result.tilesU || result.tilesV || elongation >= 4.0)
  {
    result.suggestedKind = MaterialKind::Trim;
  }
  else
  {
    result.suggestedKind = MaterialKind::Panel;
  }
  return result;
}

Json toJson(const ImageAnalysis& analysis)
{
  return Json{
    {"width", analysis.width},
    {"height", analysis.height},
    {"aspectRatio", roundForOutput(analysis.aspectRatio)},
    {"edgeDifference", vec2Json(analysis.edgeDifference)},
    {"innerDifference", vec2Json(analysis.innerDifference)},
    {"tilesU", analysis.tilesU},
    {"tilesV", analysis.tilesV},
    {"transparentFraction", roundForOutput(analysis.transparentFraction)},
    {"suggestedKind", toString(analysis.suggestedKind)},
  };
}

// Notes

Json toJson(const MaterialNote& note)
{
  auto result = Json{{"material", note.name}};
  if (note.kind)
  {
    result["kind"] = toString(*note.kind);
  }
  if (note.scale)
  {
    result["scale"] = vec2Json(*note.scale);
  }
  if (note.faceSize)
  {
    result["faceSize"] = vec2Json(*note.faceSize);
  }
  if (note.text)
  {
    result["text"] = *note.text;
  }
  result["updated"] = note.updated;
  return result;
}

std::optional<MaterialNote> materialNoteFromJson(const Json& json)
{
  const auto name = member<std::string>(json, "material");
  if (!name || name->empty())
  {
    return std::nullopt;
  }

  auto note = MaterialNote{};
  note.name = *name;
  if (const auto kind = member<std::string>(json, "kind"))
  {
    note.kind = materialKindFromString(*kind);
    if (!note.kind)
    {
      return std::nullopt;
    }
  }
  if (const auto* scale = findMember(json, "scale"))
  {
    note.scale = scale->is_number()
                   ? std::optional{vm::vec2d{scale->get<double>(), scale->get<double>()}}
                   : vec2FromJson(*scale);
    if (!note.scale)
    {
      return std::nullopt;
    }
  }
  if (findMember(json, "faceSize"))
  {
    note.faceSize = vec2Member(json, "faceSize");
    if (!note.faceSize)
    {
      return std::nullopt;
    }
  }
  note.text = member<std::string>(json, "text");
  note.updated = member<std::string>(json, "updated").value_or("");
  return note;
}

Json toJson(const NotesFile& notes)
{
  auto items = Json::array();
  for (const auto& [key, note] : notes.notes)
  {
    items.push_back(toJson(note));
  }
  return Json{{"version", 1}, {"notes", std::move(items)}};
}

Result<NotesFile, std::string> notesFileFromJson(const Json& json)
{
  const auto* items = findMember(json, "notes");
  if (!items || !items->is_array())
  {
    return std::string{"'notes' is missing or not an array"};
  }
  auto result = NotesFile{};
  for (const auto& item : *items)
  {
    auto note = materialNoteFromJson(item);
    if (!note)
    {
      return fmt::format("invalid note: {}", dumpJson(item));
    }
    result.notes[lower(note->name)] = std::move(*note);
  }
  return result;
}

// Corpus

void CorpusFile::merge(const CorpusFile& other)
{
  const auto addUnique = [](auto& list, const auto& values) {
    for (const auto& value : values)
    {
      if (std::ranges::find(list, value) == list.end())
      {
        list.push_back(value);
      }
    }
  };
  addUnique(folders, other.folders);
  addUnique(files, other.files);
  if (!other.scannedAt.empty())
  {
    scannedAt = other.scannedAt;
  }
  faces += other.faces;
  for (const auto& [key, entry] : other.materials)
  {
    auto it = materials.find(key);
    if (it == materials.end())
    {
      materials.emplace(key, entry);
      continue;
    }
    if (entry.textureSize)
    {
      it->second.textureSize = entry.textureSize;
    }
    it->second.stats.merge(entry.stats);
    it->second.stats.cap();
  }
}

Json toJson(const CorpusFile& corpus)
{
  auto materials = Json::object();
  for (const auto& [key, entry] : corpus.materials)
  {
    auto item = Json{{"name", entry.name}};
    if (entry.textureSize)
    {
      item["size"] = vec2Json(*entry.textureSize);
    }
    item["stats"] = toJson(entry.stats);
    materials[key] = std::move(item);
  }
  return Json{
    {"version", 1},
    {"game", corpus.game},
    {"mod", corpus.mod ? Json(*corpus.mod) : Json(nullptr)},
    {"folders", corpus.folders},
    {"files", corpus.files},
    {"scannedAt", corpus.scannedAt},
    {"faces", corpus.faces},
    {"materials", std::move(materials)},
  };
}

Result<CorpusFile, std::string> corpusFileFromJson(const Json& json)
{
  const auto* materials = findMember(json, "materials");
  if (!json.is_object() || !materials || !materials->is_object())
  {
    return std::string{"'materials' is missing or not an object"};
  }

  auto result = CorpusFile{};
  result.game = member<std::string>(json, "game").value_or("");
  result.mod = member<std::string>(json, "mod");
  result.folders = member<std::vector<std::string>>(json, "folders")
                     .value_or(std::vector<std::string>{});
  result.files =
    member<std::vector<std::string>>(json, "files").value_or(std::vector<std::string>{});
  result.scannedAt = member<std::string>(json, "scannedAt").value_or("");
  result.faces = member<uint64_t>(json, "faces").value_or(0);
  for (const auto& [key, item] : materials->items())
  {
    const auto* stats = findMember(item, "stats");
    auto parsed = stats ? materialStatsFromJson(*stats) : std::nullopt;
    if (!parsed)
    {
      return fmt::format("invalid statistics of material '{}'", key);
    }
    result.materials[lower(key)] = CorpusFile::Entry{
      member<std::string>(item, "name").value_or(key),
      vec2Member(item, "size"),
      std::move(*parsed),
    };
  }
  return result;
}

// Knowledge store

std::filesystem::path KnowledgeScope::corpusPath() const
{
  return directory / "corpus.json";
}

std::filesystem::path KnowledgeScope::notesPath() const
{
  return directory / "notes.json";
}

std::filesystem::path KnowledgeScope::gameCorpusPath() const
{
  return gameDirectory / "corpus.json";
}

std::filesystem::path KnowledgeScope::gameNotesPath() const
{
  return gameDirectory / "notes.json";
}

Json toJson(const KnowledgeScope& scope)
{
  return Json{
    {"game", scope.game},
    {"mod", scope.mod ? Json(*scope.mod) : Json(nullptr)},
    {"path", scope.directory.string()},
  };
}

std::string sanitizeFolderName(const std::string_view name)
{
  auto result = std::string{};
  for (const auto c : name)
  {
    const auto uc = static_cast<unsigned char>(c);
    result += std::isalnum(uc) || c == '.' || c == '-' || c == '_' || c == ' ' ? c : '_';
  }
  if (result.empty() || std::ranges::all_of(result, [](const char c) {
        return c == '.';
      }))
  {
    result = std::string(result.size() == 0 ? 1 : result.size(), '_');
  }
  return result;
}

KnowledgeScope knowledgeScope(
  const mdl::Map& map, const std::filesystem::path& knowledgeDirectory)
{
  auto scope = KnowledgeScope{};
  scope.game = map.gameInfo().gameConfig.name;
  if (const auto mods = mdl::enabledMods(map); !mods.empty())
  {
    scope.mod = mods.back();
  }
  const auto gameFolder = knowledgeDirectory / sanitizeFolderName(scope.game);
  scope.gameDirectory = gameFolder / std::string{GameScopeFolder};
  scope.directory =
    scope.mod ? gameFolder / sanitizeFolderName(*scope.mod) : scope.gameDirectory;
  return scope;
}

namespace
{

template <typename T>
struct CachedFile
{
  std::filesystem::file_time_type time;
  uintmax_t size = 0;
  std::shared_ptr<const T> value;
};

template <typename T>
std::map<std::filesystem::path, CachedFile<T>>& fileCache()
{
  // only used on the main thread
  static auto cache = std::map<std::filesystem::path, CachedFile<T>>{};
  return cache;
}

template <typename T, typename Parse>
Result<std::shared_ptr<const T>, std::string> readCached(
  const std::filesystem::path& path, const Parse& parse)
{
  auto error = std::error_code{};
  if (!std::filesystem::is_regular_file(path, error))
  {
    fileCache<T>().erase(path);
    return std::shared_ptr<const T>{};
  }
  const auto time = std::filesystem::last_write_time(path, error);
  const auto size = error ? uintmax_t(0) : std::filesystem::file_size(path, error);
  if (error)
  {
    return fmt::format("{} could not be read: {}", path, error.message());
  }

  auto& cache = fileCache<T>();
  if (const auto it = cache.find(path);
      it != cache.end() && it->second.time == time && it->second.size == size)
  {
    return it->second.value;
  }

  auto stream = std::ifstream{path, std::ios::binary};
  const auto text = std::string{std::istreambuf_iterator<char>{stream}, {}};
  if (stream.bad())
  {
    return fmt::format("{} could not be read.", path);
  }
  const auto json = parseJson(text);
  if (!json)
  {
    return fmt::format("{} is not valid JSON.", path);
  }
  auto parsed = parse(*json);
  if (parsed.is_error())
  {
    return fmt::format("{} is invalid: {}", path, std::get<std::string>(parsed.error()));
  }
  auto value = std::make_shared<const T>(std::move(parsed).value());
  cache[path] = CachedFile<T>{time, size, value};
  return value;
}

template <typename T>
Result<void, ToolError> writeCached(
  const std::filesystem::path& path, const T& value, const std::string& text)
{
  auto error = std::error_code{};
  std::filesystem::create_directories(path.parent_path(), error);
  if (error)
  {
    return makeError(
      ErrorCode::IoError,
      fmt::format("Could not create {}: {}", path.parent_path(), error.message()));
  }

  auto temporary = path;
  temporary += ".tmp";
  {
    auto stream = std::ofstream{temporary, std::ios::binary | std::ios::trunc};
    stream << text;
    stream.close();
    if (!stream)
    {
      std::filesystem::remove(temporary, error);
      return makeError(ErrorCode::IoError, fmt::format("Could not write {}.", temporary));
    }
  }
  std::filesystem::rename(temporary, path, error);
  if (error)
  {
    auto ignored = std::error_code{};
    std::filesystem::remove(temporary, ignored);
    return makeError(
      ErrorCode::IoError, fmt::format("Could not write {}: {}", path, error.message()));
  }

  const auto time = std::filesystem::last_write_time(path, error);
  const auto size = error ? uintmax_t(0) : std::filesystem::file_size(path, error);
  if (error)
  {
    fileCache<T>().erase(path);
  }
  else
  {
    fileCache<T>()[path] = CachedFile<T>{time, size, std::make_shared<const T>(value)};
  }
  return Result<void, ToolError>{};
}

} // namespace

Result<std::shared_ptr<const CorpusFile>, std::string> readCorpusFile(
  const std::filesystem::path& path)
{
  return readCached<CorpusFile>(path, corpusFileFromJson);
}

Result<std::shared_ptr<const NotesFile>, std::string> readNotesFile(
  const std::filesystem::path& path)
{
  return readCached<NotesFile>(path, notesFileFromJson);
}

Result<void, ToolError> writeCorpusFile(
  const std::filesystem::path& path, const CorpusFile& corpus)
{
  return writeCached(path, corpus, dumpJson(toJson(corpus)));
}

Result<void, ToolError> writeNotesFile(
  const std::filesystem::path& path, const NotesFile& notes)
{
  // indented, since users may edit notes by hand
  return writeCached(
    path,
    notes,
    toJson(notes).dump(2, ' ', false, Json::error_handler_t::replace) + "\n");
}

// Profiles

namespace
{

template <typename T>
Json sourcedJson(const std::optional<Sourced<T>>& value, const auto& toValue)
{
  if (!value)
  {
    return nullptr;
  }
  return Json{
    {"value", toValue(value->value)},
    {"source", value->source},
    {"samples", value->samples},
  };
}

} // namespace

Json toJson(const MaterialProfile& profile, const bool includeStatistics)
{
  const auto vec2 = [](const vm::vec2d& v) { return vec2Json(v); };
  const auto number = [](const double v) { return roundForOutput(v); };
  const auto range = [](const ScaleRange& r) { return toJson(r); };

  auto note = Json(nullptr);
  if (profile.note)
  {
    note = toJson(*profile.note);
    note["scope"] = profile.noteScope.value_or("game");
  }

  auto result = Json{
    {"name", profile.name},
    {"loaded", profile.loaded},
    {"textureSize", optionalVec2Json(profile.textureSize)},
    {"mapUsage", profile.mapUsage},
    {"kind",
     Json{
       {"value", toString(profile.kind.value)},
       {"source", profile.kind.source},
       {"samples", profile.kind.samples},
     }},
    {"typicalScale", sourcedJson(profile.typicalScale, vec2)},
    {"scaleRange", sourcedJson(profile.scaleRange, range)},
    {"texelDensity", sourcedJson(profile.texelDensity, vec2)},
    {"typicalFaceSize", sourcedJson(profile.typicalFaceSize, vec2)},
    {"typicalRepeats", sourcedJson(profile.typicalRepeats, vec2)},
    {"wholeRepeatFraction", sourcedJson(profile.wholeRepeatFraction, number)},
    {"alignedFraction", sourcedJson(profile.alignedFraction, number)},
    {"image", profile.image ? toJson(*profile.image) : Json(nullptr)},
    {"note", std::move(note)},
    {"configTag", profile.configTag ? Json(*profile.configTag) : Json(nullptr)},
  };
  if (profile.imageError)
  {
    result["imageError"] = *profile.imageError;
  }
  if (includeStatistics)
  {
    result["statistics"] = Json{
      {"corpus", profile.corpusStats ? toJson(*profile.corpusStats) : Json(nullptr)},
      {"map", profile.mapStats ? toJson(*profile.mapStats) : Json(nullptr)},
    };
  }
  return result;
}

MaterialKnowledge::MaterialKnowledge(
  const mdl::Map& map, std::optional<std::filesystem::path> knowledgeDirectory)
  : m_map{&map}
{
  if (!knowledgeDirectory)
  {
    return;
  }

  m_scope = knowledgeScope(map, *knowledgeDirectory);
  const auto read =
    [&](const auto& reader, const std::filesystem::path& path, auto& target) {
      auto result = reader(path);
      if (result.is_success())
      {
        target = std::move(result).value();
      }
      else
      {
        m_problems.push_back(std::get<std::string>(result.error()));
      }
    };
  read(readNotesFile, m_scope->gameNotesPath(), m_gameNotes);
  read(readCorpusFile, m_scope->gameCorpusPath(), m_gameCorpus);
  if (m_scope->mod)
  {
    read(readNotesFile, m_scope->notesPath(), m_modNotes);
    read(readCorpusFile, m_scope->corpusPath(), m_modCorpus);
  }
}

MaterialKnowledge::~MaterialKnowledge() = default;

MaterialKnowledge::MaterialKnowledge(MaterialKnowledge&&) noexcept = default;
MaterialKnowledge& MaterialKnowledge::operator=(MaterialKnowledge&&) noexcept = default;

const std::optional<KnowledgeScope>& MaterialKnowledge::scope() const
{
  return m_scope;
}

const std::vector<std::string>& MaterialKnowledge::problems() const
{
  return m_problems;
}

void MaterialKnowledge::computeMapStats()
{
  if (m_mapStats)
  {
    return;
  }
  m_mapStats.emplace();

  const auto visit = [&](const auto& self, const mdl::Node& node) -> void {
    if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node))
    {
      for (const auto& face : brushNode->brush().faces())
      {
        const auto key = lower(face.materialName());
        m_mapUsage[key] += 1;
        if (const auto sample = sampleFace(face, textureSizeOf(face.material())))
        {
          (*m_mapStats)[key].add(*sample);
        }
      }
    }
    for (const auto* child : node.children())
    {
      self(self, *child);
    }
  };
  visit(visit, m_map->worldNode());
}

const MaterialStats* MaterialKnowledge::mapStats(const std::string_view materialName)
{
  computeMapStats();
  const auto it = m_mapStats->find(lower(materialName));
  return it != m_mapStats->end() ? &it->second : nullptr;
}

std::vector<std::pair<std::string, uint64_t>> MaterialKnowledge::mapUsage()
{
  computeMapStats();

  // the spelling of the first face using a material
  auto spelling = std::unordered_map<std::string, std::string>{};
  const auto visit = [&](const auto& self, const mdl::Node& node) -> void {
    if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node))
    {
      for (const auto& face : brushNode->brush().faces())
      {
        spelling.try_emplace(lower(face.materialName()), face.materialName());
      }
    }
    for (const auto* child : node.children())
    {
      self(self, *child);
    }
  };
  visit(visit, m_map->worldNode());

  auto result = std::vector<std::pair<std::string, uint64_t>>{};
  for (const auto& [key, count] : m_mapUsage)
  {
    result.emplace_back(spelling[key], count);
  }
  std::ranges::sort(result, [](const auto& lhs, const auto& rhs) {
    return lhs.second != rhs.second ? lhs.second > rhs.second
                                    : lower(lhs.first) < lower(rhs.first);
  });
  return result;
}

std::optional<std::pair<MaterialNote, std::string>> MaterialKnowledge::note(
  const std::string_view materialName) const
{
  const auto key = lower(materialName);
  if (m_modNotes)
  {
    if (const auto it = m_modNotes->notes.find(key); it != m_modNotes->notes.end())
    {
      return std::pair{it->second, std::string{"mod"}};
    }
  }
  if (m_gameNotes)
  {
    if (const auto it = m_gameNotes->notes.find(key); it != m_gameNotes->notes.end())
    {
      return std::pair{it->second, std::string{"game"}};
    }
  }
  return std::nullopt;
}

const CorpusFile::Entry* MaterialKnowledge::corpusEntry(
  const std::string_view materialName) const
{
  const auto key = lower(materialName);
  for (const auto* corpus : {m_modCorpus.get(), m_gameCorpus.get()})
  {
    if (corpus)
    {
      if (const auto it = corpus->materials.find(key); it != corpus->materials.end())
      {
        return &it->second;
      }
    }
  }
  return nullptr;
}

void MaterialKnowledge::analyzeImage(MaterialProfile& profile)
{
  if (profile.image || profile.imageError)
  {
    return;
  }
  const auto* material = m_map->materialManager().material(profile.name);
  if (!material)
  {
    profile.imageError = "The material is not loaded.";
    return;
  }
  if (!m_imageLoader)
  {
    m_imageLoader = std::make_unique<MaterialImageLoader>(*m_map);
  }
  auto image = m_imageLoader->load(*material);
  if (image.is_error())
  {
    profile.imageError = errorOf(image).message;
    return;
  }
  profile.image = mcp::analyzeImage(image.value().image);
}

const MaterialProfile& MaterialKnowledge::profile(
  const std::string_view materialName, const bool withImage)
{
  const auto key = lower(materialName);
  if (const auto it = m_profiles.find(key); it != m_profiles.end())
  {
    auto& profile = it->second;
    if (withImage)
    {
      analyzeImage(profile);
      if (
        profile.kind.value == MaterialKind::Unknown && profile.image
        && profile.image->suggestedKind != MaterialKind::Unknown)
      {
        profile.kind = {profile.image->suggestedKind, "image", 0};
      }
    }
    return profile;
  }

  computeMapStats();

  auto profile = MaterialProfile{};
  profile.name = std::string{materialName};
  if (const auto* material = m_map->materialManager().material(profile.name))
  {
    profile.loaded = true;
    profile.name = material->name();
    profile.textureSize = textureSizeOf(material);
  }
  if (const auto it = m_mapUsage.find(key); it != m_mapUsage.end())
  {
    profile.mapUsage = it->second;
  }

  if (auto note = this->note(key))
  {
    profile.note = std::move(note->first);
    profile.noteScope = std::move(note->second);
  }

  if (const auto* entry = corpusEntry(key))
  {
    profile.corpusStats = summarize(
      entry->stats, profile.textureSize ? profile.textureSize : entry->textureSize);
  }
  if (const auto it = m_mapStats->find(key); it != m_mapStats->end())
  {
    profile.mapStats = summarize(it->second, profile.textureSize);
  }
  const auto& corpus = profile.corpusStats;
  const auto& map = profile.mapStats;
  const auto& note = profile.note;

  // kind
  const auto config = kindFromConfig(*m_map, profile.name);
  if (config)
  {
    profile.configTag = config->tag;
  }
  if (note && note->kind)
  {
    profile.kind = {*note->kind, "notes", 0};
  }
  else if (config)
  {
    profile.kind = {config->kind, "config", 0};
  }
  else if (const auto kind = corpus ? kindFromStats(*corpus) : std::nullopt)
  {
    profile.kind = {*kind, "corpus", corpus->sizedSamples};
  }
  else if (const auto mapKind = map ? kindFromStats(*map) : std::nullopt)
  {
    profile.kind = {*mapKind, "map", map->sizedSamples};
  }
  else if (const auto nameKind = kindFromName(profile.name))
  {
    profile.kind = {*nameKind, "name", 0};
  }

  if (withImage || profile.kind.value == MaterialKind::Unknown)
  {
    analyzeImage(profile);
    if (
      profile.kind.value == MaterialKind::Unknown && profile.image
      && profile.image->suggestedKind != MaterialKind::Unknown)
    {
      profile.kind = {profile.image->suggestedKind, "image", 0};
    }
  }

  // scale and texel density
  if (note && note->scale)
  {
    profile.typicalScale = {*note->scale, "notes", 0};
  }
  else if (corpus && corpus->typicalScale)
  {
    profile.typicalScale = {*corpus->typicalScale, "corpus", corpus->samples};
  }
  else if (map && map->typicalScale)
  {
    profile.typicalScale = {*map->typicalScale, "map", map->samples};
  }
  else
  {
    const auto& defaults =
      m_map->gameInfo().gameConfig.faceAttribsConfig.defaultUvAttributes;
    profile.typicalScale = {vm::abs(vm::vec2d{defaults.scale}), "config", 0};
  }
  profile.texelDensity = profile.typicalScale;

  if (corpus && corpus->scaleRange)
  {
    profile.scaleRange = {*corpus->scaleRange, "corpus", corpus->samples};
  }
  else if (map && map->scaleRange)
  {
    profile.scaleRange = {*map->scaleRange, "map", map->samples};
  }

  if (note && note->faceSize)
  {
    profile.typicalFaceSize = {*note->faceSize, "notes", 0};
  }
  else if (corpus && corpus->typicalFaceSize)
  {
    profile.typicalFaceSize = {*corpus->typicalFaceSize, "corpus", corpus->samples};
  }
  else if (map && map->typicalFaceSize)
  {
    profile.typicalFaceSize = {*map->typicalFaceSize, "map", map->samples};
  }

  const auto withRepeats = [](const auto& summary) {
    return summary && summary->typicalRepeats;
  };
  if (const auto& source = withRepeats(corpus) ? corpus : map; withRepeats(source))
  {
    const auto* name = &source == &corpus ? "corpus" : "map";
    profile.typicalRepeats = {*source->typicalRepeats, name, source->sizedSamples};
    if (source->wholeRepeatFraction)
    {
      profile.wholeRepeatFraction = {
        *source->wholeRepeatFraction, name, source->sizedSamples};
    }
    if (source->alignedFraction)
    {
      profile.alignedFraction = {*source->alignedFraction, name, source->sizedSamples};
    }
  }

  return m_profiles.emplace(key, std::move(profile)).first->second;
}

} // namespace tb::mcp
