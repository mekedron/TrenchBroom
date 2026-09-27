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

#include "mcp/Image.h"

#include <miniz.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace tb::mcp
{

std::optional<std::string> encodePng(const RgbaImage& image)
{
  if (image.width == 0 || image.height == 0)
  {
    return std::nullopt;
  }

  auto size = size_t{0};
  auto* png = tdefl_write_image_to_png_file_in_memory_ex(
    image.pixels.data(), int(image.width), int(image.height), 4, &size, 6, MZ_FALSE);
  if (!png)
  {
    return std::nullopt;
  }
  auto result = std::string{static_cast<const char*>(png), size};
  mz_free(png);
  return result;
}

RgbaImage makeImage(const size_t width, const size_t height, const Rgba8& color)
{
  auto image = RgbaImage{width, height, std::vector<unsigned char>(width * height * 4)};
  for (size_t i = 0; i < width * height; ++i)
  {
    std::copy(color.begin(), color.end(), image.pixels.begin() + long(i * 4));
  }
  return image;
}

Rgba8 pixelAt(const RgbaImage& image, const size_t x, const size_t y)
{
  const auto* p = image.pixels.data() + (y * image.width + x) * 4;
  return Rgba8{p[0], p[1], p[2], p[3]};
}

RgbaImage downscale(const RgbaImage& image, const size_t maxSize)
{
  const auto longest = std::max(image.width, image.height);
  if (longest <= maxSize)
  {
    return image;
  }

  const auto scale = double(maxSize) / double(longest);
  const auto width =
    std::max(size_t{1}, size_t(std::lround(double(image.width) * scale)));
  const auto height =
    std::max(size_t{1}, size_t(std::lround(double(image.height) * scale)));

  auto result = RgbaImage{width, height, std::vector<unsigned char>(width * height * 4)};
  for (size_t y = 0; y < height; ++y)
  {
    const auto y0 = y * image.height / height;
    const auto y1 = std::max(y0 + 1, (y + 1) * image.height / height);
    for (size_t x = 0; x < width; ++x)
    {
      const auto x0 = x * image.width / width;
      const auto x1 = std::max(x0 + 1, (x + 1) * image.width / width);

      // box filter: average of the covered source pixels
      size_t sum[4] = {0, 0, 0, 0};
      for (auto sy = y0; sy < y1; ++sy)
      {
        for (auto sx = x0; sx < x1; ++sx)
        {
          const auto* src = image.pixels.data() + (sy * image.width + sx) * 4;
          for (size_t c = 0; c < 4; ++c)
          {
            sum[c] += src[c];
          }
        }
      }
      const auto count = (y1 - y0) * (x1 - x0);
      auto* dst = result.pixels.data() + (y * width + x) * 4;
      for (size_t c = 0; c < 4; ++c)
      {
        dst[c] = static_cast<unsigned char>((sum[c] + count / 2) / count);
      }
    }
  }
  return result;
}

RgbaImage composeSideBySide(
  const std::vector<RgbaImage>& images, const size_t gap, const Rgba8& background)
{
  auto width = size_t(0);
  auto height = size_t(0);
  for (const auto& image : images)
  {
    width += image.width;
    height = std::max(height, image.height);
  }
  if (!images.empty())
  {
    width += gap * (images.size() - 1);
  }

  auto result = makeImage(width, height, background);
  auto left = size_t(0);
  for (const auto& image : images)
  {
    for (size_t y = 0; y < image.height; ++y)
    {
      const auto* src = image.pixels.data() + y * image.width * 4;
      auto* dst = result.pixels.data() + (y * width + left) * 4;
      std::copy(src, src + image.width * 4, dst);
    }
    left += image.width + gap;
  }
  return result;
}

std::optional<ImageDiff> diffImages(
  const RgbaImage& before, const RgbaImage& after, const int threshold)
{
  if (before.width != after.width || before.height != after.height)
  {
    return std::nullopt;
  }

  const auto width = after.width;
  const auto height = after.height;
  auto result = ImageDiff{};
  result.mask = RgbaImage{width, height, std::vector<unsigned char>(width * height * 4)};

  auto minX = width;
  auto minY = height;
  auto maxX = size_t(0);
  auto maxY = size_t(0);
  for (size_t y = 0; y < height; ++y)
  {
    for (size_t x = 0; x < width; ++x)
    {
      const auto offset = (y * width + x) * 4;
      const auto* a = before.pixels.data() + offset;
      const auto* b = after.pixels.data() + offset;
      auto* dst = result.mask.pixels.data() + offset;

      auto changed = false;
      for (size_t c = 0; c < 4; ++c)
      {
        if (std::abs(int(a[c]) - int(b[c])) > threshold)
        {
          changed = true;
          break;
        }
      }

      if (changed)
      {
        std::copy(DiffMaskColor.begin(), DiffMaskColor.end(), dst);
        ++result.changedPixels;
        minX = std::min(minX, x);
        minY = std::min(minY, y);
        maxX = std::max(maxX, x);
        maxY = std::max(maxY, y);
      }
      else
      {
        // a dimmed gray version of the image for orientation
        const auto gray = (int(b[0]) * 299 + int(b[1]) * 587 + int(b[2]) * 114) / 1000;
        const auto dimmed = static_cast<unsigned char>(gray / 4);
        dst[0] = dimmed;
        dst[1] = dimmed;
        dst[2] = dimmed;
        dst[3] = 255;
      }
    }
  }

  const auto total = width * height;
  result.changedRatio = total > 0 ? double(result.changedPixels) / double(total) : 0.0;
  if (result.changedPixels > 0)
  {
    result.changedBounds = PixelRect{minX, minY, maxX - minX + 1, maxY - minY + 1};
  }
  return result;
}

} // namespace tb::mcp
