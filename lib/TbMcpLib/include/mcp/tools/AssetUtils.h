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
#include "mcp/Errors.h"
#include "mcp/Image.h"

#include <memory>

namespace tb
{
class Logger;

namespace gl
{
class Material;
}

namespace mdl
{
class GameFileSystem;
class Map;
class Palette;
} // namespace mdl
} // namespace tb

namespace tb::mcp
{

/**
 * Creates the file system of the map's game the way the map does when it loads its
 * assets: the game path with the enabled mods, and the WAD files of the world's wad
 * property. Needed because the map's own game file system is private; loading assets
 * from it directly gives the CPU data that the editor drops after uploading to the GPU.
 */
std::unique_ptr<mdl::GameFileSystem> createGameFileSystem(
  const mdl::Map& map, Logger& logger);

struct MaterialImage
{
  /** Mip level 0 as RGBA pixels. */
  RgbaImage image;
  /** Whether the pixels came from the loaded texture (true) or were read from its file.
   */
  bool fromMemory = false;
};

/**
 * Loads the images of many materials of one map, building the game file system
 * (createGameFileSystem) and loading the game's palette only once, when the first image
 * has to be read from a file. The map must outlive the loader.
 */
class MaterialImageLoader
{
private:
  const mdl::Map* m_map;
  std::unique_ptr<Logger> m_logger;
  std::unique_ptr<mdl::GameFileSystem> m_fileSystem;
  std::unique_ptr<mdl::Palette> m_palette;
  bool m_initialized = false;

public:
  explicit MaterialImageLoader(const mdl::Map& map);
  ~MaterialImageLoader();

  MaterialImageLoader(MaterialImageLoader&&) noexcept;
  MaterialImageLoader& operator=(MaterialImageLoader&&) noexcept;

  /** Like loadMaterialImage. */
  Result<MaterialImage, ToolError> load(const gl::Material& material);

private:
  void initialize();
};

/**
 * The image of a material: mip level 0 of its texture if the texture still holds its CPU
 * buffers, otherwise loaded from the game file system (createGameFileSystem). Fails with
 * UNSUPPORTED for compressed and other non-RGB(A) formats and with OPERATION_FAILED if
 * the image cannot be loaded (e.g. a shader material without an image file).
 */
Result<MaterialImage, ToolError> loadMaterialImage(
  const mdl::Map& map, const gl::Material& material);

} // namespace tb::mcp
