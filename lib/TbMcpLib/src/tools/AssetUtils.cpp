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

#include "mcp/tools/AssetUtils.h"

#include "ToolUtils.h"
#include "base/Logger.h"
#include "gl/GlUtils.h"
#include "gl/Material.h"
#include "gl/Texture.h"
#include "gl/TextureBuffer.h"
#include "mdl/Entity.h"
#include "mdl/EntityProperties.h"
#include "mdl/EnvironmentConfig.h"
#include "mdl/GameConfig.h"
#include "mdl/GameFileSystem.h"
#include "mdl/GameInfo.h"
#include "mdl/LoadTexture.h"
#include "mdl/Map.h"
#include "mdl/Map_World.h"
#include "mdl/Palette.h"
#include "mdl/WadPropertyUtils.h"
#include "mdl/WorldNode.h"

#include "kd/result.h"

#include <fmt/format.h>
#include <fmt/std.h>

#include <optional>
#include <string>
#include <vector>

namespace tb::mcp
{
namespace
{

Result<RgbaImage, ToolError> toRgbaImage(
  const gl::Texture& texture, const std::string& name)
{
  const auto format = texture.format();
  if (gl::isCompressedFormat(format))
  {
    return makeError(
      ErrorCode::Unsupported,
      fmt::format(
        "Material '{}' uses a compressed texture format, which cannot be previewed.",
        name));
  }
  if (format != GL_RGB && format != GL_BGR && format != GL_RGBA && format != GL_BGRA)
  {
    return makeError(
      ErrorCode::Unsupported,
      fmt::format("Material '{}' uses an unsupported texture format.", name));
  }

  const auto& buffers = texture.buffersIfLoaded();
  const auto width = texture.width();
  const auto height = texture.height();
  const auto bytesPerPixel = gl::bytesPerPixelForFormat(format);
  if (
    buffers.empty() || buffers.front().size() < width * height * bytesPerPixel
    || width == 0 || height == 0)
  {
    return makeError(
      ErrorCode::OperationFailed,
      fmt::format("The image data of material '{}' is not available.", name));
  }

  const auto swap = format == GL_BGR || format == GL_BGRA;
  const auto* data = buffers.front().data();
  auto image = RgbaImage{width, height, std::vector<unsigned char>(width * height * 4)};
  for (size_t i = 0; i < width * height; ++i)
  {
    const auto* src = data + i * bytesPerPixel;
    auto* dst = image.pixels.data() + i * 4;
    dst[0] = swap ? src[2] : src[0];
    dst[1] = src[1];
    dst[2] = swap ? src[0] : src[2];
    dst[3] = bytesPerPixel == 4 ? src[3] : 255;
  }
  return image;
}

} // namespace

std::unique_ptr<mdl::GameFileSystem> createGameFileSystem(
  const mdl::Map& map, Logger& logger)
{
  const auto& gameConfig = map.gameInfo().gameConfig;
  const auto& materialConfig = gameConfig.materialConfig;

  auto fs = std::make_unique<mdl::GameFileSystem>();
  auto mods = std::vector<std::filesystem::path>{};
  for (const auto& mod : mdl::enabledMods(map))
  {
    mods.emplace_back(mod);
  }
  fs->initialize(map.environmentConfig(), gameConfig, map.gamePath(), mods, logger);

  if (const auto* wads = map.worldNode().entity().property(mdl::EntityPropertyKeys::Wad))
  {
    auto wadPaths = std::vector<std::filesystem::path>{};
    for (const auto& wad : mdl::splitWadProperty(*wads))
    {
      wadPaths.emplace_back(wad);
    }
    const auto searchPaths = std::vector<std::filesystem::path>{
      map.path().parent_path(),
      map.gamePath(),
      map.environmentConfig().appFolderPath,
    };
    fs->reloadWads(materialConfig.root, searchPaths, wadPaths, logger);
  }

  return fs;
}

MaterialImageLoader::MaterialImageLoader(const mdl::Map& map)
  : m_map{&map}
  , m_logger{std::make_unique<NullLogger>()}
{
}

MaterialImageLoader::~MaterialImageLoader() = default;

MaterialImageLoader::MaterialImageLoader(MaterialImageLoader&&) noexcept = default;
MaterialImageLoader& MaterialImageLoader::operator=(MaterialImageLoader&&) noexcept =
  default;

void MaterialImageLoader::initialize()
{
  if (m_initialized)
  {
    return;
  }
  m_initialized = true;

  const auto& materialConfig = m_map->gameInfo().gameConfig.materialConfig;
  m_fileSystem = createGameFileSystem(*m_map, *m_logger);
  if (!materialConfig.palette.empty())
  {
    m_fileSystem->openFile(materialConfig.palette) | kdl::and_then([&](auto file) {
      return mdl::loadPalette(*file, materialConfig.palette);
    }) | kdl::transform([&](auto loaded) {
      m_palette = std::make_unique<mdl::Palette>(std::move(loaded));
    }) | kdl::transform_error([](auto) {});
  }
}

Result<MaterialImage, ToolError> MaterialImageLoader::load(const gl::Material& material)
{
  const auto* texture = material.texture();
  if (texture && !texture->buffersIfLoaded().empty())
  {
    return toRgbaImage(*texture, material.name()) | kdl::transform([](auto image) {
             return MaterialImage{std::move(image), true};
           });
  }

  initialize();
  auto palette = m_palette ? std::optional{*m_palette} : std::nullopt;
  auto reloaded =
    mdl::loadTexture(material.relativePath(), material.name(), *m_fileSystem, palette);
  if (reloaded.is_error())
  {
    return makeError(
      ErrorCode::OperationFailed,
      fmt::format(
        "Could not load the image of material '{}' from {}: {}",
        material.name(),
        material.relativePath().generic_string(),
        errorMessage(reloaded)),
      "Shader materials and materials without an image file cannot be previewed.");
  }
  return toRgbaImage(reloaded.value(), material.name()) | kdl::transform([](auto image) {
           return MaterialImage{std::move(image), false};
         });
}

Result<MaterialImage, ToolError> loadMaterialImage(
  const mdl::Map& map, const gl::Material& material)
{
  return MaterialImageLoader{map}.load(material);
}

} // namespace tb::mcp
