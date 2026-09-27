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

#include "base/Result.h"
#include "mcp/tools/WadFile.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tb::mcp
{

/** The BSP version of Quake. */
constexpr auto QuakeBspVersion = 29;
/** The BSP version of Half-Life (GoldSrc). */
constexpr auto HalfLifeBspVersion = 30;

/** An entity of a BSP's entity lump. */
struct BspEntity
{
  std::vector<std::pair<std::string, std::string>> properties;

  /** The value of the first property with the given key, if any. */
  const std::string* property(std::string_view key) const;
  /** The classname, or an empty string. */
  std::string classname() const;
  /** The origin property, or (0, 0, 0). */
  vm::vec3d origin() const;
};

/** A drawn face of a compiled map, with its texture mapping and lightmap. */
struct BspFace
{
  /** The normal of the front side and the distance of the plane from the origin. */
  vm::vec3d normal = vm::vec3d{0, 0, 1};
  double distance = 0.0;
  std::vector<vm::vec3d> vertices;
  /** The index into BspData::textures; npos if the texture is missing. */
  size_t textureIndex = npos;
  /** The texture axes: s = dot(point, s.xyz) + s.w, likewise t. In texels. */
  std::array<double, 4> s = {1, 0, 0, 0};
  std::array<double, 4> t = {0, 1, 0, 0};
  /** The texinfo flags (1 = special: sky or liquid, not lightmapped). */
  unsigned int flags = 0;
  /** The light styles of the lightmaps (255 = unused). */
  std::array<unsigned char, 4> styles = {255, 255, 255, 255};
  /** The byte offset into BspData::lighting, if the face has a lightmap. */
  std::optional<size_t> lightOffset;
  /** The texture coordinates of the first luxel (multiples of 16). */
  int lightMinS = 0;
  int lightMinT = 0;
  /** The size of the lightmap in luxels (16 texels per luxel). */
  size_t lightWidth = 0;
  size_t lightHeight = 0;

  static constexpr auto npos = size_t(-1);

  /** The number of lightmaps (styles other than 255). */
  size_t lightmapCount() const;
  /** The texture coordinates of a point. */
  std::pair<double, double> textureCoords(const vm::vec3d& point) const;
  /** The point on the face's plane with the given texture coordinates. */
  vm::vec3d pointAt(double s, double t) const;
};

/** A model: 0 is the world, the others belong to brush entities (`model` "*n"). */
struct BspModel
{
  vm::bbox3d bounds;
  size_t firstFace = 0;
  size_t faceCount = 0;
};

/** The kind of surface a face shows, from its texture name. */
enum class BspSurface
{
  Normal,
  Sky,
  /** Water, slime, lava: drawn without lightmap. */
  Liquid,
};

/** A compiled map (Quake BSP 29 or Half-Life BSP 30) read into memory. */
struct BspData
{
  int version = HalfLifeBspVersion;
  std::vector<BspEntity> entities;
  std::vector<BspFace> faces;
  std::vector<BspModel> models;
  /** The textures of the BSP; textures without pixels are loaded from WADs. */
  std::vector<MipTexture> textures;
  std::vector<unsigned char> lighting;

  bool isHalfLife() const;
  /** 3 (RGB) for Half-Life, 1 (gray) for Quake. */
  size_t bytesPerLuxel() const;
  /** The worldspawn entity, if there is one. */
  const BspEntity* worldspawn() const;
  /** The WAD files of worldspawn's `wad` property, in order. */
  std::vector<std::string> wadPaths() const;
  /** The kind of surface of the face. */
  BspSurface surface(const BspFace& face) const;
};

/**
 * Reads a compiled map. Supports BSP 29 (Quake) and BSP 30 (Half-Life); other formats
 * (e.g. Quake 2 `IBSP`) fail with a message that names them.
 */
Result<BspData, std::string> readBsp(std::string_view bytes);

/** Parses an entity lump (`{ "key" "value" ... }` blocks). */
std::vector<BspEntity> parseEntityLump(std::string_view text);

/** Which light styles light a preview. */
enum class LightStyleSet
{
  /** Style 0 only: the lights that cannot be switched. */
  Base,
  /**
   * The styles lit when the map starts: style 0, the animated styles 1-31 and the
   * switchable styles (32+) of lights that do not start off (spawnflag 1).
   */
  Initial,
  /** Every style at full strength. */
  All,
};

/** The weight of each light style (0 or 1). */
using LightStyleWeights = std::array<double, 256>;

LightStyleWeights lightStyleWeights(const BspData& bsp, LightStyleSet set);
/** Style 0 and the given styles. */
LightStyleWeights lightStyleWeights(const std::vector<int>& styles);

/**
 * The light at the given texture coordinates of the face (bilinear between luxels):
 * RGB, 0-255 per channel for one full-strength style, summed over the weighted styles.
 * Faces without a lightmap return 0; if the BSP has no light data at all, every face is
 * fully lit (255), as the engines draw such maps.
 */
vm::vec3d sampleLight(
  const BspData& bsp,
  const BspFace& face,
  double s,
  double t,
  const LightStyleWeights& weights);

/** Whether a point on the face's plane lies inside the face, with a tolerance. */
bool faceContains(const BspFace& face, const vm::vec3d& point, double tolerance = 0.0);

/**
 * A brush entity model that the game draws: the model index, the entity's origin and its
 * render mode and amount (Half-Life `rendermode`, `renderamt`).
 */
struct BspDrawnModel
{
  size_t model = 0;
  vm::vec3d origin = vm::vec3d{0, 0, 0};
  std::string classname;
  int renderMode = 0;
  double renderAmount = 255.0;
};

/**
 * The world (model 0) and, if `entities`, the brush entity models that are visible in
 * the game: not trigger_* entities, not render modes that draw nothing (renderamt 0).
 */
std::vector<BspDrawnModel> drawnModels(const BspData& bsp, bool entities);

/**
 * The height (z) of the first floor below the point: the first face facing up that a
 * ray cast straight down hits within maxDepth, among the drawn models. Sky faces count.
 */
std::optional<double> bspFloorBelow(
  const BspData& bsp, const vm::vec3d& point, double maxDepth = 4096.0);

/** The first info_player_start (or deathmatch start): its origin and yaw. */
struct BspPlayerStart
{
  vm::vec3d origin = vm::vec3d{0, 0, 0};
  double yaw = 0.0;
  std::string classname;
};

std::optional<BspPlayerStart> playerStart(const BspData& bsp);

/** The eye position above a player start's origin (Half-Life 28, Quake 22). */
double eyeOffsetAboveOrigin(const BspData& bsp);

/** The light levels (0-255, the luminance of the light) of lightmap luxels. */
struct LightmapStats
{
  size_t faces = 0;
  /** Lightmapped faces without light data (black in the game). */
  size_t unlitFaces = 0;
  size_t luxels = 0;
  double meanLight = 0.0;
  /** Luxels below DarkLight ("pitch black"), below DimLight, at or above BrightLight. */
  double darkFraction = 0.0;
  double dimFraction = 0.0;
  double overexposedFraction = 0.0;
};

/** Light below this is pitch black in the game. */
constexpr auto DarkLight = 16.0;
/** Light below this is dim. */
constexpr auto DimLight = 48.0;
/** Light at or above this saturates the lightmap (washed out, too bright). */
constexpr auto BrightLight = 250.0;

/** The luminance of an RGB light (0-255). */
double lightLevel(const vm::vec3d& light);

/**
 * The statistics of the luxels of the drawn faces (not sky, not liquids) whose world
 * position lies inside the box (all luxels if there is no box).
 */
LightmapStats lightmapStats(
  const BspData& bsp,
  const LightStyleWeights& weights,
  const std::optional<vm::bbox3d>& box = std::nullopt);

} // namespace tb::mcp
