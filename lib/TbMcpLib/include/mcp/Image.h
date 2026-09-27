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

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace tb::mcp
{

/** An 8-bit RGBA image. Rows are stored top to bottom, 4 bytes per pixel. */
struct RgbaImage
{
  size_t width = 0;
  size_t height = 0;
  std::vector<unsigned char> pixels;

  bool operator==(const RgbaImage&) const = default;
};

/** An RGBA color with 8 bits per channel. */
using Rgba8 = std::array<unsigned char, 4>;

/** A rectangle of pixels; x grows to the right, y downwards. */
struct PixelRect
{
  size_t x = 0;
  size_t y = 0;
  size_t width = 0;
  size_t height = 0;

  bool operator==(const PixelRect&) const = default;
};

/** Returns the PNG file bytes of the given image, or nullopt if encoding failed. */
std::optional<std::string> encodePng(const RgbaImage& image);

/** An image of the given size filled with the given color. */
RgbaImage makeImage(size_t width, size_t height, const Rgba8& color);

/** The color of the pixel at x, y. Precondition: x < width and y < height */
Rgba8 pixelAt(const RgbaImage& image, size_t x, size_t y);

/**
 * Scales the image down (box filter) so that its longer side is at most maxSize pixels,
 * keeping the aspect ratio. Smaller images are returned unchanged.
 */
RgbaImage downscale(const RgbaImage& image, size_t maxSize);

/**
 * Places the images next to each other from left to right, separated by `gap` pixels
 * and aligned at the top. The result is as high as the highest image; uncovered pixels
 * have the background color.
 */
RgbaImage composeSideBySide(
  const std::vector<RgbaImage>& images, size_t gap, const Rgba8& background);

/** The differences between two images of the same size. */
struct ImageDiff
{
  /**
   * The changed pixels in the mask color on a dimmed grayscale copy of the second
   * image, so that the changes can be located.
   */
  RgbaImage mask;
  size_t changedPixels = 0;
  /** changedPixels / all pixels. */
  double changedRatio = 0.0;
  /** The bounding rectangle of the changed pixels, or nullopt if none changed. */
  std::optional<PixelRect> changedBounds;
};

/** The color of changed pixels in ImageDiff::mask. */
constexpr auto DiffMaskColor = Rgba8{255, 0, 255, 255};

/**
 * Compares two images pixel by pixel. A pixel changed if any of its channels differs by
 * more than `threshold` (0-255). Returns nullopt if the images differ in size.
 */
std::optional<ImageDiff> diffImages(
  const RgbaImage& before, const RgbaImage& after, int threshold = 0);

} // namespace tb::mcp
