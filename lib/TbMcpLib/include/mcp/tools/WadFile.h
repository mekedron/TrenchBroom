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
#include "mcp/Image.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tb::mcp
{

/** The number of mip levels of a mip texture. */
constexpr auto MipLevels = size_t(4);

/** The longest name of a mip texture (the name field has 16 bytes with the NUL). */
constexpr auto MaxMipTextureName = size_t(15);

/** The palette index that `{` textures draw transparent. */
constexpr auto TransparentIndex = size_t(255);

/**
 * A palettized texture with four mip levels, as Quake (WAD2, BSP 29) and Half-Life
 * (WAD3, BSP 30) store them. Half-Life textures carry their own 256-colour palette;
 * Quake textures use the game's palette (`gfx/palette.lmp`).
 */
struct MipTexture
{
  std::string name;
  size_t width = 0;
  size_t height = 0;
  /**
   * The palette indices of the mip levels (level i has (width >> i) * (height >> i)
   * pixels). Empty if the texture has no pixels (a BSP texture that the game loads
   * from a WAD).
   */
  std::array<std::vector<unsigned char>, MipLevels> mips;
  /** 768 bytes RGB; empty if the texture uses the game's palette. */
  std::vector<unsigned char> palette;

  bool hasPixels() const;

  bool operator==(const MipTexture&) const = default;
};

/** Whether the name is a masked texture (`{name`, palette index 255 transparent). */
bool isMaskedTextureName(std::string_view name);

/**
 * Reads a mip texture (`miptex_t`) from the given bytes. `withPalette` reads the
 * Half-Life palette that follows the mip levels. A texture whose first mip offset is 0
 * has no pixels.
 */
Result<MipTexture, std::string> readMipTexture(std::string_view bytes, bool withPalette);

/**
 * Writes a mip texture in the Half-Life format: header, four mip levels, the palette
 * size (256), the palette and two padding bytes. Precondition: hasPixels() and a
 * 768-byte palette.
 */
std::string writeMipTexture(const MipTexture& texture);

/**
 * The RGBA pixels of a mip level. `palette` is used if the texture has none (Quake);
 * without any palette the indices are shown as gray levels. In masked textures, index
 * 255 is transparent (alpha 0). Precondition: hasPixels() and level < MipLevels
 */
RgbaImage mipTextureImage(
  const MipTexture& texture,
  size_t level,
  const std::vector<unsigned char>* palette = nullptr);

/** An entry of a WAD file's directory. */
struct WadEntry
{
  std::string name;
  /** The lump type: 0x43 Half-Life mip texture, 0x44 Quake mip texture, ... */
  int type = 0;
  size_t offset = 0;
  size_t size = 0;
  bool compressed = false;
};

/** A WAD2 (Quake) or WAD3 (Half-Life) file read into memory. */
struct WadFile
{
  /** 2 or 3. */
  int version = 3;
  std::vector<WadEntry> entries;
  std::string bytes;

  /** The index of the entry with the given name (case-insensitive), if any. */
  std::optional<size_t> find(std::string_view name) const;

  /** Whether the entry is a mip texture. */
  static bool isMipTexture(const WadEntry& entry);

  /**
   * Reads the mip texture of the given entry; the entry's name replaces the name in
   * the lump.
   */
  Result<MipTexture, std::string> mipTexture(const WadEntry& entry) const;
};

Result<WadFile, std::string> readWad(std::string bytes);

/** Writes a WAD3 file with the given textures (each written by writeMipTexture). */
std::string writeWad3(const std::vector<MipTexture>& textures);

/**
 * Why the name cannot be a Half-Life texture name, or nullopt if it can: 1 to 15
 * printable ASCII characters without spaces, quotes, slashes and backslashes.
 */
std::optional<std::string> checkMipTextureName(std::string_view name);

/** Resamples an image (box filter when shrinking, bilinear when enlarging). */
RgbaImage resizeImage(const RgbaImage& image, size_t width, size_t height);

/** The result of converting an image to a mip texture. */
struct MipConversion
{
  MipTexture texture;
  /** The number of distinct palette entries that the pixels of mip level 0 use. */
  size_t colorsUsed = 0;
  /** The number of distinct colours of the source image's opaque pixels. */
  size_t sourceColors = 0;
  /** The pixels of mip level 0 that became transparent (masked textures only). */
  size_t transparentPixels = 0;
  /** The mean colour error per opaque pixel of mip level 0 (RGB distance, 0-441). */
  double meanError = 0.0;
};

/**
 * Converts an RGBA image into a Half-Life mip texture: quantizes it to a palette of
 * 256 colours (median cut refined by k-means), generates the four mip levels with a
 * box filter and maps them to the palette. For masked textures, pixels with alpha below
 * 128 and pure blue (0, 0, 255) pixels become index 255, which is (0, 0, 255), and
 * the other pixels use at most 255 colours. Precondition: the width and height are
 * positive multiples of 16.
 */
MipConversion makeMipTexture(std::string name, const RgbaImage& image, bool masked);

} // namespace tb::mcp
