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

#include "mcp/tools/BspRender.h"

#include "mcp/CameraProjection.h"

#include "vm/vec_ext.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace tb::mcp
{
namespace
{

enum class PixelKind : unsigned char
{
  None,
  Lit,
  Sky,
  Liquid,
};

/** A projected polygon vertex. */
struct ScreenVertex
{
  double x = 0.0;
  double y = 0.0;
  double depth = 0.0;
  double s = 0.0;
  double t = 0.0;
};

struct DrawFace
{
  const BspFace* face = nullptr;
  const BspDrawnModel* model = nullptr;
  BspSurface surface = BspSurface::Normal;
  std::vector<ScreenVertex> vertices;
  double maxDepth = 0.0;
};

struct Buffers
{
  size_t width = 0;
  size_t height = 0;
  std::vector<double> depth;
  std::vector<vm::vec3d> color;
  std::vector<double> light;
  std::vector<PixelKind> kind;
};

double edgeFunction(
  const ScreenVertex& a, const ScreenVertex& b, const double x, const double y)
{
  return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
}

vm::vec3d toColor(const Rgba8& rgba)
{
  return vm::vec3d{double(rgba[0]), double(rgba[1]), double(rgba[2])} / 255.0;
}

class Rasterizer
{
private:
  const BspData& m_bsp;
  const BspTextureImages& m_textures;
  const ImageProjection& m_projection;
  const BspRenderOptions& m_options;
  Buffers& m_buffers;
  bool m_perspective;
  double m_overbright;

public:
  Rasterizer(
    const BspData& bsp,
    const BspTextureImages& textures,
    const ImageProjection& projection,
    const BspRenderOptions& options,
    Buffers& buffers)
    : m_bsp{bsp}
    , m_textures{textures}
    , m_projection{projection}
    , m_options{options}
    , m_buffers{buffers}
    , m_perspective{projection.camera().projection == CameraProjection::Perspective}
    , m_overbright{bsp.isHalfLife() ? 1.0 : 2.0}
  {
  }

  /** Culls, clips and projects a face; returns nullopt if nothing of it is visible. */
  std::optional<DrawFace> prepare(const BspFace& face, const BspDrawnModel& model) const
  {
    if (face.vertices.size() < 3)
    {
      return std::nullopt;
    }
    const auto& camera = m_projection.camera();
    const auto first = face.vertices.front() + model.origin;
    const auto facing = m_perspective ? vm::dot(face.normal, camera.position - first)
                                      : -vm::dot(face.normal, camera.direction);
    if (facing <= 0.0)
    {
      return std::nullopt;
    }

    // clip against the near plane in world space
    const auto nearPlane = std::max(camera.nearPlane, m_perspective ? 0.05 : -1e9);
    struct ClipVertex
    {
      vm::vec3d world;
      double s;
      double t;
      double depth;
    };
    auto input = std::vector<ClipVertex>{};
    input.reserve(face.vertices.size());
    for (const auto& vertex : face.vertices)
    {
      const auto world = vertex + model.origin;
      const auto [s, t] = face.textureCoords(vertex);
      input.push_back(ClipVertex{world, s, t, m_projection.depth(world)});
    }
    auto clipped = std::vector<ClipVertex>{};
    clipped.reserve(input.size() + 2);
    for (size_t i = 0; i < input.size(); ++i)
    {
      const auto& a = input[i];
      const auto& b = input[(i + 1) % input.size()];
      const auto aIn = a.depth >= nearPlane;
      const auto bIn = b.depth >= nearPlane;
      if (aIn)
      {
        clipped.push_back(a);
      }
      if (aIn != bIn)
      {
        const auto f = (nearPlane - a.depth) / (b.depth - a.depth);
        clipped.push_back(ClipVertex{
          a.world + f * (b.world - a.world),
          a.s + f * (b.s - a.s),
          a.t + f * (b.t - a.t),
          nearPlane});
      }
    }
    if (clipped.size() < 3)
    {
      return std::nullopt;
    }

    auto result = DrawFace{&face, &model, m_bsp.surface(face), {}, 0.0};
    if (result.surface == BspSurface::Sky && m_options.hideSky)
    {
      return std::nullopt;
    }
    result.vertices.reserve(clipped.size());
    auto minX = std::numeric_limits<double>::max();
    auto maxX = std::numeric_limits<double>::lowest();
    auto minY = std::numeric_limits<double>::max();
    auto maxY = std::numeric_limits<double>::lowest();
    for (const auto& vertex : clipped)
    {
      const auto projected = m_projection.project(vertex.world);
      const auto depth = std::max(vertex.depth, nearPlane);
      result.vertices.push_back(
        ScreenVertex{projected.x, projected.y, depth, vertex.s, vertex.t});
      result.maxDepth = std::max(result.maxDepth, depth);
      minX = std::min(minX, projected.x);
      maxX = std::max(maxX, projected.x);
      minY = std::min(minY, projected.y);
      maxY = std::max(maxY, projected.y);
    }
    if (
      maxX < 0.0 || maxY < 0.0 || minX > double(m_buffers.width)
      || minY > double(m_buffers.height))
    {
      return std::nullopt;
    }
    return result;
  }

  /** Draws the face; translucent faces blend with `alpha` and do not write depth. */
  void draw(
    const DrawFace& face, const bool translucent, const double alpha, const bool additive)
  {
    const auto& v = face.vertices;
    for (size_t i = 1; i + 1 < v.size(); ++i)
    {
      drawTriangle(face, v[0], v[i], v[i + 1], translucent, alpha, additive);
    }
  }

private:
  const std::vector<RgbaImage>* textureOf(const BspFace& face) const
  {
    if (face.textureIndex < m_textures.size() && !m_textures[face.textureIndex].empty())
    {
      return &m_textures[face.textureIndex];
    }
    return nullptr;
  }

  /** The texel colour (0-1) and alpha at the given texture coordinates. */
  std::pair<vm::vec3d, bool> texel(
    const std::vector<RgbaImage>* mips,
    const double s,
    const double t,
    size_t level) const
  {
    if (!mips)
    {
      const auto checker = (int(std::floor(s / 8.0)) + int(std::floor(t / 8.0))) % 2 != 0;
      return {checker ? vm::vec3d{1, 0, 1} : vm::vec3d{0.1, 0.1, 0.1}, true};
    }
    level = std::min(level, mips->size() - 1);
    const auto& image = (*mips)[level];
    const auto scale = double(size_t(1) << level);
    const auto w = int64_t(image.width);
    const auto h = int64_t(image.height);
    auto x = int64_t(std::floor(s / scale)) % w;
    auto y = int64_t(std::floor(t / scale)) % h;
    x = x < 0 ? x + w : x;
    y = y < 0 ? y + h : y;
    const auto* p = &image.pixels[(size_t(y) * image.width + size_t(x)) * 4];
    return {vm::vec3d{double(p[0]), double(p[1]), double(p[2])} / 255.0, p[3] >= 128};
  }

  void drawTriangle(
    const DrawFace& face,
    const ScreenVertex& a,
    const ScreenVertex& b,
    const ScreenVertex& c,
    const bool translucent,
    const double alpha,
    const bool additive)
  {
    const auto area = edgeFunction(a, b, c.x, c.y);
    if (std::abs(area) < 1e-12)
    {
      return;
    }
    const auto width = m_buffers.width;
    const auto height = m_buffers.height;
    const auto minX = std::max(0.0, std::floor(std::min({a.x, b.x, c.x})));
    const auto maxX = std::min(double(width - 1), std::ceil(std::max({a.x, b.x, c.x})));
    const auto minY = std::max(0.0, std::floor(std::min({a.y, b.y, c.y})));
    const auto maxY = std::min(double(height - 1), std::ceil(std::max({a.y, b.y, c.y})));
    if (minX > maxX || minY > maxY)
    {
      return;
    }

    const auto* mips = textureOf(*face.face);
    const auto masked =
      mips && face.face->textureIndex < m_bsp.textures.size()
      && isMaskedTextureName(m_bsp.textures[face.face->textureIndex].name);
    const auto farPlane = m_projection.camera().farPlane;

    // attributes as functions of the barycentric weights
    const auto ia = m_perspective ? 1.0 / a.depth : 1.0;
    const auto ib = m_perspective ? 1.0 / b.depth : 1.0;
    const auto ic = m_perspective ? 1.0 / c.depth : 1.0;
    const auto interpolate = [&](const double x, const double y) {
      const auto wa = edgeFunction(b, c, x, y) / area;
      const auto wb = edgeFunction(c, a, x, y) / area;
      const auto wc = 1.0 - wa - wb;
      struct Sample
      {
        double wa, wb, wc, depth, s, t;
      };
      if (m_perspective)
      {
        const auto inverse = wa * ia + wb * ib + wc * ic;
        const auto depth = 1.0 / inverse;
        return Sample{
          wa,
          wb,
          wc,
          depth,
          (wa * a.s * ia + wb * b.s * ib + wc * c.s * ic) * depth,
          (wa * a.t * ia + wb * b.t * ib + wc * c.t * ic) * depth};
      }
      return Sample{
        wa,
        wb,
        wc,
        wa * a.depth + wb * b.depth + wc * c.depth,
        wa * a.s + wb * b.s + wc * c.s,
        wa * a.t + wb * b.t + wc * c.t};
    };

    for (auto py = size_t(minY); py <= size_t(maxY); ++py)
    {
      for (auto px = size_t(minX); px <= size_t(maxX); ++px)
      {
        const auto x = double(px) + 0.5;
        const auto y = double(py) + 0.5;
        const auto sample = interpolate(x, y);
        if (sample.wa < -1e-9 || sample.wb < -1e-9 || sample.wc < -1e-9)
        {
          continue;
        }
        const auto index = py * width + px;
        if (sample.depth >= m_buffers.depth[index] || sample.depth > farPlane)
        {
          continue;
        }

        // the texel footprint picks the mip level
        const auto right = interpolate(x + 1.0, y);
        const auto down = interpolate(x, y + 1.0);
        const auto footprint = std::max(
          std::hypot(right.s - sample.s, right.t - sample.t),
          std::hypot(down.s - sample.s, down.t - sample.t));
        const auto level =
          footprint > 1.0 ? size_t(std::clamp(std::floor(std::log2(footprint)), 0.0, 3.0))
                          : size_t(0);

        const auto [color, opaque] = texel(mips, sample.s, sample.t, level);
        if (masked && !opaque)
        {
          continue;
        }
        shade(face, index, sample.s, sample.t, color, translucent, alpha, additive);
        if (!translucent)
        {
          m_buffers.depth[index] = sample.depth;
        }
      }
    }
  }

  void shade(
    const DrawFace& face,
    const size_t index,
    const double s,
    const double t,
    const vm::vec3d& texture,
    const bool translucent,
    const double alpha,
    const bool additive)
  {
    auto color = vm::vec3d{0, 0, 0};
    auto kind = PixelKind::Lit;
    auto lightValue = 0.0;
    switch (face.surface)
    {
    case BspSurface::Sky:
      kind = PixelKind::Sky;
      color = toColor(m_options.skyColor);
      break;
    case BspSurface::Liquid:
      kind = PixelKind::Liquid;
      color =
        m_options.shading == BspShading::Lightmap ? vm::vec3d{0.5, 0.5, 0.5} : texture;
      break;
    case BspSurface::Normal: {
      const auto light = additive
                           ? vm::vec3d{255, 255, 255}
                           : sampleLight(m_bsp, *face.face, s, t, m_options.styles);
      lightValue = std::min(255.0, lightLevel(light));
      const auto lit = light / 255.0 * m_overbright;
      switch (m_options.shading)
      {
      case BspShading::Lit:
        color =
          vm::vec3d{texture.x() * lit.x(), texture.y() * lit.y(), texture.z() * lit.z()};
        break;
      case BspShading::Fullbright:
        color = texture;
        break;
      case BspShading::Lightmap:
        color = lit;
        break;
      }
      break;
    }
    }

    if (kind != PixelKind::Sky)
    {
      color = color * m_options.brightness;
      if (m_options.gamma != 1.0)
      {
        const auto exponent = 1.0 / m_options.gamma;
        color = vm::vec3d{
          std::pow(std::max(0.0, color.x()), exponent),
          std::pow(std::max(0.0, color.y()), exponent),
          std::pow(std::max(0.0, color.z()), exponent)};
      }
    }

    if (translucent)
    {
      const auto& dst = m_buffers.color[index];
      m_buffers.color[index] =
        additive ? dst + alpha * color : alpha * color + (1.0 - alpha) * dst;
      return;
    }
    m_buffers.color[index] = color;
    m_buffers.kind[index] = kind;
    m_buffers.light[index] = lightValue;
  }
};

void accumulate(
  ImageLightStats& stats,
  const PixelKind kind,
  const double light,
  const double brightness,
  std::array<double, 2>& sums)
{
  switch (kind)
  {
  case PixelKind::None:
    return;
  case PixelKind::Sky:
    ++stats.skyPixels;
    return;
  case PixelKind::Liquid:
    ++stats.liquidPixels;
    return;
  case PixelKind::Lit:
    break;
  }
  ++stats.pixels;
  sums[0] += light;
  sums[1] += brightness;
  stats.darkFraction += light < DarkLight ? 1.0 : 0.0;
  stats.dimFraction += light < DimLight ? 1.0 : 0.0;
  stats.overexposedFraction += light >= BrightLight || brightness >= 254.5 ? 1.0 : 0.0;
}

void finish(ImageLightStats& stats, const std::array<double, 2>& sums, const size_t total)
{
  stats.coverage = total > 0 ? double(stats.pixels) / double(total) : 0.0;
  if (stats.pixels > 0)
  {
    const auto count = double(stats.pixels);
    stats.meanLight = sums[0] / count;
    stats.meanBrightness = sums[1] / count;
    stats.darkFraction /= count;
    stats.dimFraction /= count;
    stats.overexposedFraction /= count;
  }
}

} // namespace

std::string_view imageRegionName(const size_t index)
{
  static constexpr auto names = std::array<std::string_view, 9>{
    "top-left",
    "top",
    "top-right",
    "left",
    "center",
    "right",
    "bottom-left",
    "bottom",
    "bottom-right"};
  return index < names.size() ? names[index] : std::string_view{};
}

BspRenderResult renderBsp(
  const BspData& bsp,
  const BspTextureImages& textures,
  const ImageProjection& projection,
  const BspRenderOptions& options,
  const std::atomic<bool>* cancel)
{
  const auto width = projection.width();
  const auto height = projection.height();
  auto buffers = Buffers{
    width,
    height,
    std::vector<double>(width * height, std::numeric_limits<double>::infinity()),
    std::vector<vm::vec3d>(width * height, toColor(options.background)),
    std::vector<double>(width * height, 0.0),
    std::vector<PixelKind>(width * height, PixelKind::None),
  };

  auto result = BspRenderResult{};
  auto rasterizer = Rasterizer{bsp, textures, projection, options, buffers};
  const auto models = drawnModels(bsp, options.entities);

  auto translucent = std::vector<DrawFace>{};
  auto count = size_t(0);
  for (const auto& model : models)
  {
    const auto& bspModel = bsp.models[model.model];
    const auto blended = model.renderMode == 1 || model.renderMode == 2
                         || model.renderMode == 3 || model.renderMode == 5;
    for (auto i = bspModel.firstFace; i < bspModel.firstFace + bspModel.faceCount; ++i)
    {
      if (cancel && (++count % 256) == 0 && cancel->load())
      {
        result.cancelled = true;
        return result;
      }
      auto face = rasterizer.prepare(bsp.faces[i], model);
      if (!face)
      {
        continue;
      }
      ++result.facesDrawn;
      if (blended)
      {
        translucent.push_back(std::move(*face));
      }
      else
      {
        rasterizer.draw(*face, false, 1.0, false);
      }
    }
  }

  std::ranges::sort(translucent, [](const auto& lhs, const auto& rhs) {
    return lhs.maxDepth > rhs.maxDepth;
  });
  for (const auto& face : translucent)
  {
    const auto alpha = std::clamp(face.model->renderAmount / 255.0, 0.0, 1.0);
    rasterizer.draw(face, true, alpha, face.model->renderMode == 5);
  }

  result.image = makeImage(width, height, options.background);
  auto sums = std::array<double, 2>{0.0, 0.0};
  auto regionSums = std::array<std::array<double, 2>, 9>{};
  auto regionTotals = std::array<size_t, 9>{};
  for (size_t y = 0; y < height; ++y)
  {
    for (size_t x = 0; x < width; ++x)
    {
      const auto index = y * width + x;
      const auto& color = buffers.color[index];
      auto* out = &result.image.pixels[index * 4];
      for (size_t c = 0; c < 3; ++c)
      {
        out[c] =
          static_cast<unsigned char>(std::clamp(std::lround(color[c] * 255.0), 0L, 255L));
      }
      out[3] = 255;

      const auto brightness =
        0.299 * double(out[0]) + 0.587 * double(out[1]) + 0.114 * double(out[2]);
      const auto region =
        std::min(size_t(2), y * 3 / height) * 3 + std::min(size_t(2), x * 3 / width);
      ++regionTotals[region];
      accumulate(
        result.stats, buffers.kind[index], buffers.light[index], brightness, sums);
      accumulate(
        result.regions[region],
        buffers.kind[index],
        buffers.light[index],
        brightness,
        regionSums[region]);
    }
  }
  finish(result.stats, sums, width * height);
  for (size_t i = 0; i < result.regions.size(); ++i)
  {
    finish(result.regions[i], regionSums[i], regionTotals[i]);
  }
  return result;
}

} // namespace tb::mcp
