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

#include "mcp/tools/BspFile.h"

#include "kd/string_compare.h"
#include "kd/string_format.h"
#include "kd/string_utils.h"

#include "vm/vec_ext.h"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>

namespace tb::mcp
{
namespace
{

enum Lump : size_t
{
  EntitiesLump = 0,
  PlanesLump = 1,
  TexturesLump = 2,
  VerticesLump = 3,
  TexInfoLump = 6,
  FacesLump = 7,
  LightingLump = 8,
  EdgesLump = 12,
  SurfEdgesLump = 13,
  ModelsLump = 14,
  LumpCount = 15,
};

constexpr auto PlaneSize = size_t(20);
constexpr auto VertexSize = size_t(12);
constexpr auto TexInfoSize = size_t(40);
constexpr auto FaceSize = size_t(20);
constexpr auto EdgeSize = size_t(4);
constexpr auto SurfEdgeSize = size_t(4);
constexpr auto ModelSize = size_t(64);

/** Little-endian reads with bounds checks. */
class ByteReader
{
private:
  std::string_view m_bytes;

public:
  explicit ByteReader(std::string_view bytes)
    : m_bytes{bytes}
  {
  }

  bool has(const size_t offset, const size_t size) const
  {
    return offset <= m_bytes.size() && size <= m_bytes.size() - offset;
  }

  uint32_t u32(const size_t offset) const
  {
    auto result = uint32_t(0);
    for (size_t i = 0; i < 4; ++i)
    {
      result |= uint32_t(static_cast<unsigned char>(m_bytes[offset + i])) << (8 * i);
    }
    return result;
  }

  int32_t i32(const size_t offset) const { return static_cast<int32_t>(u32(offset)); }

  uint16_t u16(const size_t offset) const
  {
    return uint16_t(
      uint16_t(static_cast<unsigned char>(m_bytes[offset]))
      | (uint16_t(static_cast<unsigned char>(m_bytes[offset + 1])) << 8));
  }

  int16_t i16(const size_t offset) const { return static_cast<int16_t>(u16(offset)); }

  unsigned char u8(const size_t offset) const
  {
    return static_cast<unsigned char>(m_bytes[offset]);
  }

  float f32(const size_t offset) const
  {
    const auto bits = u32(offset);
    auto result = 0.0f;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
  }

  vm::vec3d vec3(const size_t offset) const
  {
    return vm::vec3d{f32(offset), f32(offset + 4), f32(offset + 8)};
  }

  std::string_view sub(const size_t offset, const size_t size) const
  {
    return m_bytes.substr(offset, size);
  }
};

struct LumpInfo
{
  size_t offset = 0;
  size_t length = 0;
};

vm::vec3d parseVec3(const std::string& value)
{
  auto result = vm::vec3d{0, 0, 0};
  const auto parts = kdl::str_split(value, " \t");
  for (size_t i = 0; i < 3 && i < parts.size(); ++i)
  {
    result[i] = std::strtod(parts[i].c_str(), nullptr);
  }
  return result;
}

double parseNumber(const std::string* value, const double defaultValue)
{
  if (!value || value->empty())
  {
    return defaultValue;
  }
  char* end = nullptr;
  const auto result = std::strtod(value->c_str(), &end);
  return end == value->c_str() ? defaultValue : result;
}

/** The normal of the polygon's winding (Newell's method). */
vm::vec3d windingNormal(const std::vector<vm::vec3d>& vertices)
{
  auto result = vm::vec3d{0, 0, 0};
  for (size_t i = 0; i < vertices.size(); ++i)
  {
    const auto& a = vertices[i];
    const auto& b = vertices[(i + 1) % vertices.size()];
    result[0] += (a.y() - b.y()) * (a.z() + b.z());
    result[1] += (a.z() - b.z()) * (a.x() + b.x());
    result[2] += (a.x() - b.x()) * (a.y() + b.y());
  }
  return result;
}

double dot3(const std::array<double, 4>& axis, const vm::vec3d& point)
{
  return axis[0] * point.x() + axis[1] * point.y() + axis[2] * point.z() + axis[3];
}

vm::vec3d axisVector(const std::array<double, 4>& axis)
{
  return vm::vec3d{axis[0], axis[1], axis[2]};
}

bool startsWithCi(const std::string_view str, const std::string_view prefix)
{
  return str.size() >= prefix.size()
         && kdl::ci::str_is_equal(str.substr(0, prefix.size()), prefix);
}

} // namespace

const std::string* BspEntity::property(const std::string_view key) const
{
  for (const auto& [k, v] : properties)
  {
    if (k == key)
    {
      return &v;
    }
  }
  return nullptr;
}

std::string BspEntity::classname() const
{
  const auto* value = property("classname");
  return value ? *value : std::string{};
}

vm::vec3d BspEntity::origin() const
{
  const auto* value = property("origin");
  return value ? parseVec3(*value) : vm::vec3d{0, 0, 0};
}

size_t BspFace::lightmapCount() const
{
  return size_t(std::ranges::count_if(styles, [](const auto s) { return s != 255; }));
}

std::pair<double, double> BspFace::textureCoords(const vm::vec3d& point) const
{
  return {dot3(s, point), dot3(t, point)};
}

vm::vec3d BspFace::pointAt(const double sCoord, const double tCoord) const
{
  // solve [s; t; normal] * p = [sCoord - s.w; tCoord - t.w; distance]
  const auto a = axisVector(s);
  const auto b = axisVector(t);
  const auto& c = normal;
  const auto bc = vm::cross(b, c);
  const auto det = vm::dot(a, bc);
  if (std::abs(det) < 1e-12)
  {
    return vertices.empty() ? vm::vec3d{0, 0, 0} : vertices.front();
  }
  const auto r0 = sCoord - s[3];
  const auto r1 = tCoord - t[3];
  const auto r2 = distance;
  return (r0 * bc + r1 * vm::cross(c, a) + r2 * vm::cross(a, b)) / det;
}

bool BspData::isHalfLife() const
{
  return version == HalfLifeBspVersion;
}

size_t BspData::bytesPerLuxel() const
{
  return isHalfLife() ? 3 : 1;
}

const BspEntity* BspData::worldspawn() const
{
  for (const auto& entity : entities)
  {
    if (entity.classname() == "worldspawn")
    {
      return &entity;
    }
  }
  return nullptr;
}

std::vector<std::string> BspData::wadPaths() const
{
  auto result = std::vector<std::string>{};
  if (const auto* world = worldspawn())
  {
    if (const auto* wad = world->property("wad"))
    {
      for (auto& path : kdl::str_split(*wad, ";"))
      {
        path = kdl::str_trim(path);
        if (!path.empty())
        {
          result.push_back(std::move(path));
        }
      }
    }
  }
  return result;
}

BspSurface BspData::surface(const BspFace& face) const
{
  if (face.textureIndex >= textures.size())
  {
    return BspSurface::Normal;
  }
  const auto& name = textures[face.textureIndex].name;
  if (startsWithCi(name, "sky"))
  {
    return BspSurface::Sky;
  }
  if (!name.empty() && (name.front() == '!' || name.front() == '*'))
  {
    return BspSurface::Liquid;
  }
  if (isHalfLife() && startsWithCi(name, "water"))
  {
    return BspSurface::Liquid;
  }
  return BspSurface::Normal;
}

std::vector<BspEntity> parseEntityLump(const std::string_view text)
{
  auto result = std::vector<BspEntity>{};
  auto current = std::optional<BspEntity>{};
  auto key = std::optional<std::string>{};
  auto i = size_t(0);
  while (i < text.size())
  {
    const auto c = text[i];
    if (c == '{')
    {
      current = BspEntity{};
      key = std::nullopt;
      ++i;
    }
    else if (c == '}')
    {
      if (current)
      {
        result.push_back(std::move(*current));
        current = std::nullopt;
      }
      ++i;
    }
    else if (c == '"')
    {
      const auto end = text.find('"', i + 1);
      if (end == std::string_view::npos)
      {
        break;
      }
      auto token = std::string{text.substr(i + 1, end - i - 1)};
      i = end + 1;
      if (!current)
      {
        continue;
      }
      if (!key)
      {
        key = std::move(token);
      }
      else
      {
        current->properties.emplace_back(std::move(*key), std::move(token));
        key = std::nullopt;
      }
    }
    else
    {
      ++i;
    }
  }
  return result;
}

Result<BspData, std::string> readBsp(const std::string_view bytes)
{
  const auto reader = ByteReader{bytes};
  if (!reader.has(0, 4 + LumpCount * 8))
  {
    return std::string{"The file is too short to be a BSP file."};
  }

  const auto magic = bytes.substr(0, 4);
  if (magic == "IBSP")
  {
    return std::string{
      "This is a Quake 2 or Quake 3 BSP (IBSP), which is not supported; only Quake "
      "(BSP 29) and Half-Life (BSP 30) maps can be previewed."};
  }
  if (magic == "VBSP" || magic == "BSP2" || magic == "2PSB" || magic == "RBSP")
  {
    return fmt::format(
      "This is a {} BSP, which is not supported; only Quake (BSP 29) and Half-Life "
      "(BSP 30) maps can be previewed.",
      magic);
  }

  auto bsp = BspData{};
  bsp.version = reader.i32(0);
  if (bsp.version != QuakeBspVersion && bsp.version != HalfLifeBspVersion)
  {
    return fmt::format(
      "Unsupported BSP version {}; only Quake (29) and Half-Life (30) are supported.",
      bsp.version);
  }

  auto lumps = std::array<LumpInfo, LumpCount>{};
  for (size_t i = 0; i < LumpCount; ++i)
  {
    const auto offset = reader.i32(4 + i * 8);
    const auto length = reader.i32(8 + i * 8);
    if (offset < 0 || length < 0 || !reader.has(size_t(offset), size_t(length)))
    {
      return fmt::format("The BSP file is truncated (lump {}).", i);
    }
    lumps[i] = LumpInfo{size_t(offset), size_t(length)};
  }

  // entities
  {
    const auto& lump = lumps[EntitiesLump];
    auto text = reader.sub(lump.offset, lump.length);
    text = text.substr(0, text.find('\0'));
    bsp.entities = parseEntityLump(text);
  }

  // textures
  {
    const auto& lump = lumps[TexturesLump];
    if (lump.length >= 4)
    {
      const auto count = size_t(reader.u32(lump.offset));
      if (!reader.has(lump.offset + 4, count * 4) || 4 + count * 4 > lump.length)
      {
        return std::string{"The BSP's texture lump is truncated."};
      }
      for (size_t i = 0; i < count; ++i)
      {
        const auto offset = reader.i32(lump.offset + 4 + i * 4);
        if (offset < 0 || size_t(offset) >= lump.length)
        {
          bsp.textures.push_back(MipTexture{});
          continue;
        }
        auto texture = readMipTexture(
          reader.sub(lump.offset + size_t(offset), lump.length - size_t(offset)),
          bsp.isHalfLife());
        bsp.textures.push_back(
          texture.is_success() ? std::move(texture).value() : MipTexture{});
      }
    }
  }

  const auto& planes = lumps[PlanesLump];
  const auto& vertices = lumps[VerticesLump];
  const auto& texInfos = lumps[TexInfoLump];
  const auto& edges = lumps[EdgesLump];
  const auto& surfEdges = lumps[SurfEdgesLump];
  const auto planeCount = planes.length / PlaneSize;
  const auto vertexCount = vertices.length / VertexSize;
  const auto texInfoCount = texInfos.length / TexInfoSize;
  const auto edgeCount = edges.length / EdgeSize;
  const auto surfEdgeCount = surfEdges.length / SurfEdgeSize;

  // lighting
  {
    const auto& lump = lumps[LightingLump];
    const auto* begin =
      reinterpret_cast<const unsigned char*>(bytes.data() + lump.offset);
    bsp.lighting.assign(begin, begin + lump.length);
  }

  // faces
  const auto& faces = lumps[FacesLump];
  const auto faceCount = faces.length / FaceSize;
  bsp.faces.reserve(faceCount);
  for (size_t i = 0; i < faceCount; ++i)
  {
    const auto base = faces.offset + i * FaceSize;
    const auto planeIndex = size_t(reader.u16(base));
    const auto side = reader.i16(base + 2);
    const auto firstEdge = reader.i32(base + 4);
    const auto numEdges = size_t(reader.u16(base + 8));
    const auto texInfoIndex = size_t(reader.u16(base + 10));
    if (
      planeIndex >= planeCount || texInfoIndex >= texInfoCount || firstEdge < 0
      || size_t(firstEdge) + numEdges > surfEdgeCount)
    {
      return fmt::format("The BSP's face {} refers to missing data.", i);
    }

    auto face = BspFace{};
    const auto planeBase = planes.offset + planeIndex * PlaneSize;
    face.normal = reader.vec3(planeBase);
    face.distance = reader.f32(planeBase + 12);
    if (side != 0)
    {
      face.normal = -face.normal;
      face.distance = -face.distance;
    }

    const auto texInfoBase = texInfos.offset + texInfoIndex * TexInfoSize;
    for (size_t k = 0; k < 4; ++k)
    {
      face.s[k] = reader.f32(texInfoBase + k * 4);
      face.t[k] = reader.f32(texInfoBase + 16 + k * 4);
    }
    const auto textureIndex = reader.i32(texInfoBase + 32);
    face.textureIndex = textureIndex >= 0 && size_t(textureIndex) < bsp.textures.size()
                          ? size_t(textureIndex)
                          : BspFace::npos;
    face.flags = reader.u32(texInfoBase + 36);

    for (size_t k = 0; k < 4; ++k)
    {
      face.styles[k] = reader.u8(base + 12 + k);
    }
    const auto lightOffset = reader.i32(base + 16);
    if (lightOffset >= 0 && size_t(lightOffset) < bsp.lighting.size())
    {
      face.lightOffset = size_t(lightOffset);
    }

    face.vertices.reserve(numEdges);
    for (size_t k = 0; k < numEdges; ++k)
    {
      const auto surfEdge = reader.i32(surfEdges.offset + (size_t(firstEdge) + k) * 4);
      const auto edgeIndex = size_t(surfEdge < 0 ? -int64_t(surfEdge) : surfEdge);
      if (edgeIndex >= edgeCount)
      {
        return fmt::format("The BSP's face {} refers to a missing edge.", i);
      }
      const auto edgeBase = edges.offset + edgeIndex * EdgeSize;
      const auto vertexIndex =
        size_t(surfEdge < 0 ? reader.u16(edgeBase + 2) : reader.u16(edgeBase));
      if (vertexIndex >= vertexCount)
      {
        return fmt::format("The BSP's face {} refers to a missing vertex.", i);
      }
      face.vertices.push_back(reader.vec3(vertices.offset + vertexIndex * VertexSize));
    }

    // lightmap extents, like the engine computes them
    if (!face.vertices.empty())
    {
      auto minS = std::numeric_limits<double>::max();
      auto minT = std::numeric_limits<double>::max();
      auto maxS = std::numeric_limits<double>::lowest();
      auto maxT = std::numeric_limits<double>::lowest();
      for (const auto& vertex : face.vertices)
      {
        const auto [sc, tc] = face.textureCoords(vertex);
        minS = std::min(minS, sc);
        maxS = std::max(maxS, sc);
        minT = std::min(minT, tc);
        maxT = std::max(maxT, tc);
      }
      const auto bminS = int(std::floor(minS / 16.0));
      const auto bminT = int(std::floor(minT / 16.0));
      const auto bmaxS = int(std::ceil(maxS / 16.0));
      const auto bmaxT = int(std::ceil(maxT / 16.0));
      face.lightMinS = bminS * 16;
      face.lightMinT = bminT * 16;
      face.lightWidth = size_t(std::max(0, bmaxS - bminS) + 1);
      face.lightHeight = size_t(std::max(0, bmaxT - bminT) + 1);
    }

    if (face.lightOffset)
    {
      const auto size =
        face.lightWidth * face.lightHeight * bsp.bytesPerLuxel() * face.lightmapCount();
      if (*face.lightOffset + size > bsp.lighting.size())
      {
        face.lightOffset = std::nullopt;
      }
    }
    bsp.faces.push_back(std::move(face));
  }

  // models
  const auto& models = lumps[ModelsLump];
  const auto modelCount = models.length / ModelSize;
  for (size_t i = 0; i < modelCount; ++i)
  {
    const auto base = models.offset + i * ModelSize;
    auto model = BspModel{};
    model.bounds = vm::bbox3d{reader.vec3(base), reader.vec3(base + 12)};
    const auto firstFace = reader.i32(base + 56);
    const auto count = reader.i32(base + 60);
    if (
      firstFace < 0 || count < 0 || size_t(firstFace) + size_t(count) > bsp.faces.size())
    {
      return fmt::format("The BSP's model {} refers to missing faces.", i);
    }
    model.firstFace = size_t(firstFace);
    model.faceCount = size_t(count);
    bsp.models.push_back(model);
  }
  if (bsp.models.empty())
  {
    return std::string{"The BSP has no world model."};
  }

  return bsp;
}

LightStyleWeights lightStyleWeights(const BspData& bsp, const LightStyleSet set)
{
  auto weights = LightStyleWeights{};
  weights.fill(0.0);
  weights[0] = 1.0;
  if (set == LightStyleSet::Base)
  {
    return weights;
  }
  for (size_t i = 1; i < 255; ++i)
  {
    weights[i] = 1.0;
  }
  if (set == LightStyleSet::All)
  {
    return weights;
  }

  // switchable lights that start off
  auto on = std::set<int>{};
  auto off = std::set<int>{};
  for (const auto& entity : bsp.entities)
  {
    if (!startsWithCi(entity.classname(), "light"))
    {
      continue;
    }
    const auto style = int(parseNumber(entity.property("style"), 0.0));
    if (style < 32 || style > 254)
    {
      continue;
    }
    const auto flags = int(parseNumber(entity.property("spawnflags"), 0.0));
    ((flags & 1) ? off : on).insert(style);
  }
  for (const auto style : off)
  {
    if (!on.contains(style))
    {
      weights[size_t(style)] = 0.0;
    }
  }
  return weights;
}

LightStyleWeights lightStyleWeights(const std::vector<int>& styles)
{
  auto weights = LightStyleWeights{};
  weights.fill(0.0);
  weights[0] = 1.0;
  for (const auto style : styles)
  {
    if (style >= 0 && style < 255)
    {
      weights[size_t(style)] = 1.0;
    }
  }
  return weights;
}

namespace
{

vm::vec3d luxel(
  const BspData& bsp,
  const BspFace& face,
  const size_t x,
  const size_t y,
  const LightStyleWeights& weights)
{
  const auto bpp = bsp.bytesPerLuxel();
  const auto mapSize = face.lightWidth * face.lightHeight * bpp;
  auto result = vm::vec3d{0, 0, 0};
  auto map = size_t(0);
  for (const auto style : face.styles)
  {
    if (style == 255)
    {
      break;
    }
    const auto weight = weights[style];
    if (weight > 0.0)
    {
      const auto index =
        *face.lightOffset + map * mapSize + (y * face.lightWidth + x) * bpp;
      if (bpp == 3)
      {
        result = result
                 + weight
                     * vm::vec3d{
                       double(bsp.lighting[index]),
                       double(bsp.lighting[index + 1]),
                       double(bsp.lighting[index + 2])};
      }
      else
      {
        const auto value = double(bsp.lighting[index]);
        result = result + weight * vm::vec3d{value, value, value};
      }
    }
    ++map;
  }
  return result;
}

} // namespace

vm::vec3d sampleLight(
  const BspData& bsp,
  const BspFace& face,
  const double s,
  const double t,
  const LightStyleWeights& weights)
{
  if (bsp.lighting.empty())
  {
    // the engines draw maps without light data fully lit
    return vm::vec3d{255, 255, 255};
  }
  if (!face.lightOffset || face.lightWidth == 0 || face.lightHeight == 0)
  {
    return vm::vec3d{0, 0, 0};
  }
  const auto fs =
    std::clamp((s - double(face.lightMinS)) / 16.0, 0.0, double(face.lightWidth - 1));
  const auto ft =
    std::clamp((t - double(face.lightMinT)) / 16.0, 0.0, double(face.lightHeight - 1));
  const auto x0 = size_t(fs);
  const auto y0 = size_t(ft);
  const auto x1 = std::min(x0 + 1, face.lightWidth - 1);
  const auto y1 = std::min(y0 + 1, face.lightHeight - 1);
  const auto dx = fs - double(x0);
  const auto dy = ft - double(y0);
  const auto top = (1.0 - dx) * luxel(bsp, face, x0, y0, weights)
                   + dx * luxel(bsp, face, x1, y0, weights);
  const auto bottom = (1.0 - dx) * luxel(bsp, face, x0, y1, weights)
                      + dx * luxel(bsp, face, x1, y1, weights);
  return (1.0 - dy) * top + dy * bottom;
}

bool faceContains(const BspFace& face, const vm::vec3d& point, const double tolerance)
{
  if (face.vertices.size() < 3)
  {
    return false;
  }
  const auto winding = windingNormal(face.vertices);
  const auto sign = vm::dot(winding, face.normal) >= 0.0 ? 1.0 : -1.0;
  for (size_t i = 0; i < face.vertices.size(); ++i)
  {
    const auto& a = face.vertices[i];
    const auto& b = face.vertices[(i + 1) % face.vertices.size()];
    const auto edge = b - a;
    const auto length = vm::length(edge);
    if (length < 1e-9)
    {
      continue;
    }
    // positive inside for a counterclockwise winding around the face normal
    const auto side = sign * vm::dot(vm::cross(edge, point - a), face.normal) / length;
    if (side < -tolerance)
    {
      return false;
    }
  }
  return true;
}

std::vector<BspDrawnModel> drawnModels(const BspData& bsp, const bool entities)
{
  auto result = std::vector<BspDrawnModel>{BspDrawnModel{0, {0, 0, 0}, "worldspawn"}};
  if (!entities)
  {
    return result;
  }
  for (const auto& entity : bsp.entities)
  {
    const auto* model = entity.property("model");
    if (!model || model->size() < 2 || model->front() != '*')
    {
      continue;
    }
    const auto index = std::strtoul(model->c_str() + 1, nullptr, 10);
    if (index == 0 || index >= bsp.models.size())
    {
      continue;
    }
    const auto classname = entity.classname();
    if (startsWithCi(classname, "trigger_") || startsWithCi(classname, "func_ladder"))
    {
      continue;
    }
    const auto renderMode = int(parseNumber(entity.property("rendermode"), 0.0));
    const auto renderAmount = parseNumber(entity.property("renderamt"), 255.0);
    if (renderMode != 0 && renderMode != 4 && renderAmount <= 0.0)
    {
      continue;
    }
    result.push_back(
      BspDrawnModel{size_t(index), entity.origin(), classname, renderMode, renderAmount});
  }
  return result;
}

std::optional<double> bspFloorBelow(
  const BspData& bsp, const vm::vec3d& point, const double maxDepth)
{
  auto best = std::optional<double>{};
  for (const auto& drawn : drawnModels(bsp, true))
  {
    const auto& model = bsp.models[drawn.model];
    const auto local = point - drawn.origin;
    for (auto i = model.firstFace; i < model.firstFace + model.faceCount; ++i)
    {
      const auto& face = bsp.faces[i];
      if (face.normal.z() <= 0.05)
      {
        continue;
      }
      // local.z - depth hits the plane: dot(n, local - depth * z) = distance
      const auto depth = (vm::dot(face.normal, local) - face.distance) / face.normal.z();
      if (depth < -0.01 || depth > maxDepth || (best && point.z() - depth <= *best))
      {
        continue;
      }
      const auto hit = local - vm::vec3d{0, 0, depth};
      if (faceContains(face, hit, 0.01))
      {
        best = point.z() - depth;
      }
    }
  }
  return best;
}

std::optional<BspPlayerStart> playerStart(const BspData& bsp)
{
  for (const auto* classname :
       {"info_player_start", "info_player_deathmatch", "info_player_coop"})
  {
    for (const auto& entity : bsp.entities)
    {
      if (entity.classname() != classname)
      {
        continue;
      }
      auto yaw = 0.0;
      if (const auto* angles = entity.property("angles"))
      {
        yaw = parseVec3(*angles).y();
      }
      else
      {
        yaw = parseNumber(entity.property("angle"), 0.0);
      }
      return BspPlayerStart{entity.origin(), yaw, classname};
    }
  }
  return std::nullopt;
}

double eyeOffsetAboveOrigin(const BspData& bsp)
{
  return bsp.isHalfLife() ? 28.0 : 22.0;
}

double lightLevel(const vm::vec3d& light)
{
  return 0.299 * light.x() + 0.587 * light.y() + 0.114 * light.z();
}

LightmapStats lightmapStats(
  const BspData& bsp,
  const LightStyleWeights& weights,
  const std::optional<vm::bbox3d>& box)
{
  auto stats = LightmapStats{};
  auto sum = 0.0;
  auto dark = size_t(0);
  auto dim = size_t(0);
  auto bright = size_t(0);

  for (const auto& drawn : drawnModels(bsp, true))
  {
    const auto& model = bsp.models[drawn.model];
    for (auto i = model.firstFace; i < model.firstFace + model.faceCount; ++i)
    {
      const auto& face = bsp.faces[i];
      if (bsp.surface(face) != BspSurface::Normal || (face.flags & 1) != 0)
      {
        continue;
      }
      auto faceLuxels = size_t(0);
      for (size_t y = 0; y < face.lightHeight; ++y)
      {
        for (size_t x = 0; x < face.lightWidth; ++x)
        {
          const auto local = face.pointAt(
            double(face.lightMinS) + 16.0 * double(x),
            double(face.lightMinT) + 16.0 * double(y));
          if (!faceContains(face, local, 8.0))
          {
            continue;
          }
          if (box && !box->contains(local + drawn.origin))
          {
            continue;
          }
          const auto level =
            bsp.lighting.empty() ? 255.0
            : face.lightOffset
              ? std::min(255.0, lightLevel(luxel(bsp, face, x, y, weights)))
              : 0.0;
          sum += level;
          dark += level < DarkLight ? 1 : 0;
          dim += level < DimLight ? 1 : 0;
          bright += level >= BrightLight ? 1 : 0;
          ++faceLuxels;
        }
      }
      if (faceLuxels > 0)
      {
        ++stats.faces;
        stats.unlitFaces += face.lightOffset || bsp.lighting.empty() ? 0u : 1u;
        stats.luxels += faceLuxels;
      }
    }
  }

  if (stats.luxels > 0)
  {
    const auto count = double(stats.luxels);
    stats.meanLight = sum / count;
    stats.darkFraction = double(dark) / count;
    stats.dimFraction = double(dim) / count;
    stats.overexposedFraction = double(bright) / count;
  }
  return stats;
}

} // namespace tb::mcp
