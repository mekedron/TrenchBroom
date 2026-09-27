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

#pragma once

#include "mcp/Image.h"
#include "mcp/tools/BspFile.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <string_view>
#include <vector>

namespace tb::mcp
{
class ImageProjection;

/** What a BSP preview shows. */
enum class BspShading
{
  /** Textures modulated by the lightmaps, as the game draws them. */
  Lit,
  /** Textures without light. */
  Fullbright,
  /** The light alone, on white surfaces. */
  Lightmap,
};

struct BspRenderOptions
{
  BspShading shading = BspShading::Lit;
  /** A factor on the final colour. */
  double brightness = 1.0;
  /** The output gamma: colour^(1 / gamma); above 1 brightens dark areas. */
  double gamma = 1.0;
  /** Sky faces are not drawn, e.g. to look into a map from outside its sky box. */
  bool hideSky = false;
  /** Draw brush entities (doors, walls, glass), not only the world. */
  bool entities = true;
  LightStyleWeights styles = {};
  Rgba8 background = Rgba8{24, 24, 28, 255};
  Rgba8 skyColor = Rgba8{118, 146, 184, 255};
};

/**
 * The RGBA mip levels of each texture of the BSP (by index); an empty vector for a
 * texture without pixels (drawn as a magenta checkerboard).
 */
using BspTextureImages = std::vector<std::vector<RgbaImage>>;

/** Light statistics of the lit surfaces (not sky, not liquids) in an image. */
struct ImageLightStats
{
  /** The pixels showing lit surfaces, and their share of all pixels. */
  size_t pixels = 0;
  double coverage = 0.0;
  size_t skyPixels = 0;
  size_t liquidPixels = 0;
  /** The mean light level (0-255, the luminance of the lightmap light). */
  double meanLight = 0.0;
  /** The mean luminance of the drawn colour (0-255). */
  double meanBrightness = 0.0;
  /** The pixels below DarkLight, below DimLight, at or above BrightLight. */
  double darkFraction = 0.0;
  double dimFraction = 0.0;
  double overexposedFraction = 0.0;
};

/** The names of the 3x3 image regions, row by row from the top left. */
std::string_view imageRegionName(size_t index);

struct BspRenderResult
{
  RgbaImage image;
  ImageLightStats stats;
  /** The statistics of the 3x3 regions of the image, row by row from the top left. */
  std::array<ImageLightStats, 9> regions;
  /** The faces that were in front of the camera and facing it. */
  size_t facesDrawn = 0;
  bool cancelled = false;
};

/**
 * Renders the BSP on the CPU from the projection's camera: a z-buffered, perspective
 * correct rasterizer that culls back faces like the game, picks the texture's mip level
 * from the texel footprint, samples the lightmaps bilinearly and discards the
 * transparent texels of `{` textures. Brush entities with a translucent render mode are
 * blended afterwards, from back to front. Lit colour = texture * light / 255 (Quake: * 2,
 * its overbright range), then * brightness and ^(1 / gamma). Stops early if `cancel` is
 * set.
 */
BspRenderResult renderBsp(
  const BspData& bsp,
  const BspTextureImages& textures,
  const ImageProjection& projection,
  const BspRenderOptions& options,
  const std::atomic<bool>* cancel = nullptr);

} // namespace tb::mcp
