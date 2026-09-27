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

#include "mcp/tools/WadFile.h"

#include "kd/contracts.h"
#include "kd/string_compare.h"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <unordered_map>

namespace tb::mcp
{
namespace
{

constexpr auto MipHeaderSize = size_t(40);
constexpr auto WadHeaderSize = size_t(12);
constexpr auto WadEntrySize = size_t(32);
constexpr auto PaletteBytes = size_t(768);
constexpr auto HlMipTextureType = 0x43;
constexpr auto QuakeMipTextureType = 0x44;

uint32_t readU32(const std::string_view bytes, const size_t offset)
{
  auto result = uint32_t(0);
  for (size_t i = 0; i < 4; ++i)
  {
    result |= uint32_t(static_cast<unsigned char>(bytes[offset + i])) << (8 * i);
  }
  return result;
}

uint16_t readU16(const std::string_view bytes, const size_t offset)
{
  return uint16_t(
    uint16_t(static_cast<unsigned char>(bytes[offset]))
    | (uint16_t(static_cast<unsigned char>(bytes[offset + 1])) << 8));
}

void writeU32(std::string& out, const uint32_t value)
{
  for (size_t i = 0; i < 4; ++i)
  {
    out.push_back(char((value >> (8 * i)) & 0xFF));
  }
}

void writeU16(std::string& out, const uint16_t value)
{
  out.push_back(char(value & 0xFF));
  out.push_back(char((value >> 8) & 0xFF));
}

void writeName(std::string& out, const std::string_view name)
{
  auto field = std::array<char, 16>{};
  std::copy_n(name.begin(), std::min(name.size(), MaxMipTextureName), field.begin());
  out.append(field.data(), field.size());
}

std::string readName(const std::string_view bytes, const size_t offset)
{
  const auto field = bytes.substr(offset, 16);
  return std::string{field.substr(0, field.find('\0'))};
}

size_t mipSize(const size_t width, const size_t height, const size_t level)
{
  return (width >> level) * (height >> level);
}

// Colour quantization

struct ColorCount
{
  std::array<int, 3> rgb;
  size_t count;
};

using Palette = std::vector<std::array<int, 3>>;

int distanceSquared(const std::array<int, 3>& lhs, const std::array<int, 3>& rhs)
{
  const auto dr = lhs[0] - rhs[0];
  const auto dg = lhs[1] - rhs[1];
  const auto db = lhs[2] - rhs[2];
  return dr * dr + dg * dg + db * db;
}

size_t nearestIndex(const Palette& palette, const std::array<int, 3>& rgb)
{
  auto best = size_t(0);
  auto bestDistance = std::numeric_limits<int>::max();
  for (size_t i = 0; i < palette.size(); ++i)
  {
    const auto distance = distanceSquared(palette[i], rgb);
    if (distance < bestDistance)
    {
      bestDistance = distance;
      best = i;
      if (distance == 0)
      {
        break;
      }
    }
  }
  return best;
}

struct ColorBox
{
  size_t begin;
  size_t end;
};

std::array<int, 3> channelRanges(
  const std::vector<ColorCount>& colors, const ColorBox& box)
{
  auto min = std::array<int, 3>{255, 255, 255};
  auto max = std::array<int, 3>{0, 0, 0};
  for (auto i = box.begin; i < box.end; ++i)
  {
    for (size_t c = 0; c < 3; ++c)
    {
      min[c] = std::min(min[c], colors[i].rgb[c]);
      max[c] = std::max(max[c], colors[i].rgb[c]);
    }
  }
  return {max[0] - min[0], max[1] - min[1], max[2] - min[2]};
}

/** Median cut: splits the colour boxes until there are `maxColors` of them. */
Palette medianCut(std::vector<ColorCount>& colors, const size_t maxColors)
{
  auto boxes = std::vector<ColorBox>{{0, colors.size()}};
  while (boxes.size() < maxColors)
  {
    // split the box with the largest weighted range
    auto bestBox = std::optional<size_t>{};
    auto bestScore = 0.0;
    for (size_t i = 0; i < boxes.size(); ++i)
    {
      const auto& box = boxes[i];
      if (box.end - box.begin < 2)
      {
        continue;
      }
      const auto ranges = channelRanges(colors, box);
      const auto range = *std::ranges::max_element(ranges);
      auto count = size_t(0);
      for (auto j = box.begin; j < box.end; ++j)
      {
        count += colors[j].count;
      }
      const auto score = double(range) * std::sqrt(double(count));
      if (range > 0 && score > bestScore)
      {
        bestScore = score;
        bestBox = i;
      }
    }
    if (!bestBox)
    {
      break;
    }

    const auto box = boxes[*bestBox];
    const auto ranges = channelRanges(colors, box);
    const auto channel =
      size_t(std::distance(ranges.begin(), std::ranges::max_element(ranges)));
    std::sort(
      colors.begin() + std::ptrdiff_t(box.begin),
      colors.begin() + std::ptrdiff_t(box.end),
      [&](const auto& lhs, const auto& rhs) {
        return lhs.rgb[channel] < rhs.rgb[channel];
      });

    auto total = size_t(0);
    for (auto i = box.begin; i < box.end; ++i)
    {
      total += colors[i].count;
    }
    auto split = box.begin + 1;
    auto accumulated = colors[box.begin].count;
    while (split < box.end - 1 && accumulated * 2 < total)
    {
      accumulated += colors[split].count;
      ++split;
    }
    boxes[*bestBox] = ColorBox{box.begin, split};
    boxes.push_back(ColorBox{split, box.end});
  }

  auto palette = Palette{};
  palette.reserve(boxes.size());
  for (const auto& box : boxes)
  {
    auto sum = std::array<double, 3>{0, 0, 0};
    auto count = 0.0;
    for (auto i = box.begin; i < box.end; ++i)
    {
      for (size_t c = 0; c < 3; ++c)
      {
        sum[c] += double(colors[i].rgb[c]) * double(colors[i].count);
      }
      count += double(colors[i].count);
    }
    palette.push_back(
      {int(std::lround(sum[0] / count)),
       int(std::lround(sum[1] / count)),
       int(std::lround(sum[2] / count))});
  }
  return palette;
}

/** Moves the palette entries to the mean of the colours closest to them. */
void refinePalette(Palette& palette, const std::vector<ColorCount>& colors)
{
  const auto work = colors.size() * palette.size();
  const auto iterations = work <= 4'000'000 ? 4 : work <= 32'000'000 ? 1 : 0;
  for (int iteration = 0; iteration < iterations; ++iteration)
  {
    auto sums = std::vector<std::array<double, 4>>(palette.size(), {0, 0, 0, 0});
    for (const auto& color : colors)
    {
      auto& sum = sums[nearestIndex(palette, color.rgb)];
      for (size_t c = 0; c < 3; ++c)
      {
        sum[c] += double(color.rgb[c]) * double(color.count);
      }
      sum[3] += double(color.count);
    }
    for (size_t i = 0; i < palette.size(); ++i)
    {
      if (sums[i][3] > 0)
      {
        for (size_t c = 0; c < 3; ++c)
        {
          palette[i][c] = int(std::lround(sums[i][c] / sums[i][3]));
        }
      }
    }
  }
}

uint32_t packRgb(const std::array<int, 3>& rgb)
{
  return uint32_t(rgb[0]) << 16 | uint32_t(rgb[1]) << 8 | uint32_t(rgb[2]);
}

bool isTransparentPixel(const RgbaImage& image, const size_t index)
{
  const auto* p = &image.pixels[index * 4];
  return p[3] < 128 || (p[0] == 0 && p[1] == 0 && p[2] == 255);
}

/** The next mip level: 2x2 box filter; masked pixels need two opaque source pixels. */
RgbaImage halve(const RgbaImage& image, const bool masked)
{
  auto result = makeImage(image.width / 2, image.height / 2, Rgba8{0, 0, 0, 0});
  for (size_t y = 0; y < result.height; ++y)
  {
    for (size_t x = 0; x < result.width; ++x)
    {
      auto sum = std::array<size_t, 3>{0, 0, 0};
      auto count = size_t(0);
      for (size_t dy = 0; dy < 2; ++dy)
      {
        for (size_t dx = 0; dx < 2; ++dx)
        {
          const auto index = (y * 2 + dy) * image.width + (x * 2 + dx);
          if (masked && isTransparentPixel(image, index))
          {
            continue;
          }
          for (size_t c = 0; c < 3; ++c)
          {
            sum[c] += image.pixels[index * 4 + c];
          }
          ++count;
        }
      }
      auto* out = &result.pixels[(y * result.width + x) * 4];
      if (count >= 2 || (!masked && count > 0))
      {
        for (size_t c = 0; c < 3; ++c)
        {
          out[c] = static_cast<unsigned char>((sum[c] + count / 2) / count);
        }
        out[3] = 255;
      }
    }
  }
  return result;
}

// Resampling

/** The weights of the source samples for each target sample along one axis. */
std::vector<std::vector<std::pair<size_t, double>>> resampleWeights(
  const size_t source, const size_t target)
{
  auto result = std::vector<std::vector<std::pair<size_t, double>>>(target);
  const auto scale = double(source) / double(target);
  for (size_t i = 0; i < target; ++i)
  {
    auto& weights = result[i];
    if (scale > 1.0)
    {
      // box filter over the covered source samples
      const auto begin = double(i) * scale;
      const auto end = begin + scale;
      for (auto j = size_t(std::floor(begin)); j < source && double(j) < end; ++j)
      {
        const auto overlap = std::min(end, double(j + 1)) - std::max(begin, double(j));
        if (overlap > 0)
        {
          weights.emplace_back(j, overlap / scale);
        }
      }
    }
    else
    {
      // bilinear
      const auto center = (double(i) + 0.5) * scale - 0.5;
      const auto j0 = std::clamp(std::floor(center), 0.0, double(source - 1));
      const auto j1 = std::min(j0 + 1.0, double(source - 1));
      const auto f = std::clamp(center - j0, 0.0, 1.0);
      if (f > 0 && j1 != j0)
      {
        weights.emplace_back(size_t(j0), 1.0 - f);
        weights.emplace_back(size_t(j1), f);
      }
      else
      {
        weights.emplace_back(size_t(j0), 1.0);
      }
    }
  }
  return result;
}

} // namespace

bool MipTexture::hasPixels() const
{
  return !mips[0].empty();
}

bool isMaskedTextureName(const std::string_view name)
{
  return !name.empty() && name.front() == '{';
}

Result<MipTexture, std::string> readMipTexture(
  const std::string_view bytes, const bool withPalette)
{
  if (bytes.size() < MipHeaderSize)
  {
    return std::string{"The mip texture is truncated."};
  }
  auto result = MipTexture{};
  result.name = readName(bytes, 0);
  result.width = readU32(bytes, 16);
  result.height = readU32(bytes, 20);
  if (
    result.width == 0 || result.height == 0 || result.width > 8192
    || result.height > 8192)
  {
    return fmt::format(
      "The mip texture {} has an invalid size {}x{}.",
      result.name,
      result.width,
      result.height);
  }

  const auto firstOffset = readU32(bytes, 24);
  if (firstOffset == 0)
  {
    return result;
  }

  auto end = size_t(0);
  for (size_t level = 0; level < MipLevels; ++level)
  {
    const auto offset = size_t(readU32(bytes, 24 + level * 4));
    const auto size = mipSize(result.width, result.height, level);
    if (offset + size > bytes.size() || offset < MipHeaderSize)
    {
      return fmt::format("The mip level {} of {} is truncated.", level, result.name);
    }
    const auto* begin = reinterpret_cast<const unsigned char*>(bytes.data() + offset);
    result.mips[level].assign(begin, begin + size);
    end = offset + size;
  }

  if (withPalette)
  {
    if (end + 2 > bytes.size())
    {
      return fmt::format("The palette of {} is missing.", result.name);
    }
    const auto count = size_t(readU16(bytes, end));
    if (count > 256 || end + 2 + count * 3 > bytes.size())
    {
      return fmt::format("The palette of {} is truncated.", result.name);
    }
    const auto* begin = reinterpret_cast<const unsigned char*>(bytes.data() + end + 2);
    result.palette.assign(begin, begin + count * 3);
    result.palette.resize(PaletteBytes, 0);
  }
  return result;
}

std::string writeMipTexture(const MipTexture& texture)
{
  contract_pre(texture.hasPixels());
  contract_pre(texture.palette.size() == PaletteBytes);

  auto out = std::string{};
  writeName(out, texture.name);
  writeU32(out, uint32_t(texture.width));
  writeU32(out, uint32_t(texture.height));
  auto offset = MipHeaderSize;
  for (size_t level = 0; level < MipLevels; ++level)
  {
    writeU32(out, uint32_t(offset));
    offset += mipSize(texture.width, texture.height, level);
  }
  for (const auto& mip : texture.mips)
  {
    out.append(reinterpret_cast<const char*>(mip.data()), mip.size());
  }
  writeU16(out, 256);
  out.append(reinterpret_cast<const char*>(texture.palette.data()), PaletteBytes);
  writeU16(out, 0);
  return out;
}

RgbaImage mipTextureImage(
  const MipTexture& texture,
  const size_t level,
  const std::vector<unsigned char>* palette)
{
  contract_pre(texture.hasPixels());
  contract_pre(level < MipLevels);

  const auto* colors = texture.palette.size() == PaletteBytes       ? &texture.palette
                       : palette && palette->size() >= PaletteBytes ? palette
                                                                    : nullptr;
  const auto masked = isMaskedTextureName(texture.name);
  const auto width = texture.width >> level;
  const auto height = texture.height >> level;
  auto image = makeImage(width, height, Rgba8{0, 0, 0, 255});
  const auto& indices = texture.mips[level];
  for (size_t i = 0; i < width * height && i < indices.size(); ++i)
  {
    const auto index = size_t(indices[i]);
    auto* out = &image.pixels[i * 4];
    if (colors)
    {
      out[0] = (*colors)[index * 3];
      out[1] = (*colors)[index * 3 + 1];
      out[2] = (*colors)[index * 3 + 2];
    }
    else
    {
      out[0] = out[1] = out[2] = indices[i];
    }
    out[3] = masked && index == TransparentIndex ? 0 : 255;
  }
  return image;
}

std::optional<size_t> WadFile::find(const std::string_view name) const
{
  for (size_t i = 0; i < entries.size(); ++i)
  {
    if (kdl::ci::str_is_equal(entries[i].name, name))
    {
      return i;
    }
  }
  return std::nullopt;
}

bool WadFile::isMipTexture(const WadEntry& entry)
{
  return entry.type == HlMipTextureType || entry.type == QuakeMipTextureType;
}

Result<MipTexture, std::string> WadFile::mipTexture(const WadEntry& entry) const
{
  if (!isMipTexture(entry))
  {
    return fmt::format("{} is not a mip texture.", entry.name);
  }
  if (entry.compressed)
  {
    return fmt::format("{} is compressed, which is not supported.", entry.name);
  }
  if (entry.offset + entry.size > bytes.size())
  {
    return fmt::format("{} is truncated.", entry.name);
  }
  return readMipTexture(
           std::string_view{bytes}.substr(entry.offset, entry.size),
           entry.type == HlMipTextureType)
         | kdl::transform([&](auto texture) {
             texture.name = entry.name;
             return texture;
           });
}

Result<WadFile, std::string> readWad(std::string bytes)
{
  if (bytes.size() < WadHeaderSize)
  {
    return std::string{"The file is too short to be a WAD file."};
  }
  auto result = WadFile{};
  const auto magic = std::string_view{bytes}.substr(0, 4);
  if (magic == "WAD3")
  {
    result.version = 3;
  }
  else if (magic == "WAD2")
  {
    result.version = 2;
  }
  else
  {
    return std::string{"The file is not a WAD2 or WAD3 file."};
  }

  const auto count = size_t(readU32(bytes, 4));
  const auto directory = size_t(readU32(bytes, 8));
  if (directory + count * WadEntrySize > bytes.size())
  {
    return std::string{"The WAD directory is truncated."};
  }
  result.entries.reserve(count);
  for (size_t i = 0; i < count; ++i)
  {
    const auto base = directory + i * WadEntrySize;
    result.entries.push_back(WadEntry{
      readName(bytes, base + 16),
      int(static_cast<unsigned char>(bytes[base + 12])),
      size_t(readU32(bytes, base)),
      size_t(readU32(bytes, base + 4)),
      bytes[base + 13] != 0,
    });
  }
  result.bytes = std::move(bytes);
  return result;
}

std::string writeWad3(const std::vector<MipTexture>& textures)
{
  auto out = std::string{"WAD3"};
  writeU32(out, uint32_t(textures.size()));
  writeU32(out, 0); // directory offset, patched below

  auto directory = std::string{};
  for (const auto& texture : textures)
  {
    const auto lump = writeMipTexture(texture);
    const auto offset = out.size();
    out += lump;
    while (out.size() % 4 != 0)
    {
      out.push_back('\0');
    }
    writeU32(directory, uint32_t(offset));
    writeU32(directory, uint32_t(lump.size()));
    writeU32(directory, uint32_t(lump.size()));
    directory.push_back(char(HlMipTextureType));
    directory.push_back('\0'); // not compressed
    writeU16(directory, 0);
    writeName(directory, texture.name);
  }

  const auto directoryOffset = uint32_t(out.size());
  for (size_t i = 0; i < 4; ++i)
  {
    out[8 + i] = char((directoryOffset >> (8 * i)) & 0xFF);
  }
  out += directory;
  return out;
}

std::optional<std::string> checkMipTextureName(const std::string_view name)
{
  if (name.empty())
  {
    return "The texture name is empty.";
  }
  if (name.size() > MaxMipTextureName)
  {
    return fmt::format(
      "The texture name '{}' has {} characters; Half-Life allows at most {}.",
      name,
      name.size(),
      MaxMipTextureName);
  }
  for (const auto c : name)
  {
    if (c <= ' ' || c > '~' || c == '"' || c == '/' || c == '\\')
    {
      return fmt::format(
        "The texture name '{}' contains a space, a quote, a slash or a non-ASCII "
        "character.",
        name);
    }
  }
  return std::nullopt;
}

RgbaImage resizeImage(const RgbaImage& image, const size_t width, const size_t height)
{
  contract_pre(width > 0 && height > 0 && image.width > 0 && image.height > 0);

  // resample in premultiplied alpha so that transparent pixels do not bleed
  const auto xWeights = resampleWeights(image.width, width);
  const auto yWeights = resampleWeights(image.height, height);

  auto rows = std::vector<std::array<double, 4>>(width * image.height);
  for (size_t y = 0; y < image.height; ++y)
  {
    for (size_t x = 0; x < width; ++x)
    {
      auto sum = std::array<double, 4>{0, 0, 0, 0};
      for (const auto& [sx, weight] : xWeights[x])
      {
        const auto* p = &image.pixels[(y * image.width + sx) * 4];
        const auto alpha = double(p[3]) / 255.0;
        for (size_t c = 0; c < 3; ++c)
        {
          sum[c] += double(p[c]) * alpha * weight;
        }
        sum[3] += alpha * weight;
      }
      rows[y * width + x] = sum;
    }
  }

  auto result = makeImage(width, height, Rgba8{0, 0, 0, 0});
  for (size_t y = 0; y < height; ++y)
  {
    for (size_t x = 0; x < width; ++x)
    {
      auto sum = std::array<double, 4>{0, 0, 0, 0};
      for (const auto& [sy, weight] : yWeights[y])
      {
        const auto& p = rows[sy * width + x];
        for (size_t c = 0; c < 4; ++c)
        {
          sum[c] += p[c] * weight;
        }
      }
      auto* out = &result.pixels[(y * width + x) * 4];
      const auto alpha = sum[3];
      for (size_t c = 0; c < 3; ++c)
      {
        out[c] = alpha > 1e-9 ? static_cast<unsigned char>(
                                  std::clamp(std::lround(sum[c] / alpha), 0L, 255L))
                              : 0;
      }
      out[3] =
        static_cast<unsigned char>(std::clamp(std::lround(alpha * 255.0), 0L, 255L));
    }
  }
  return result;
}

MipConversion makeMipTexture(std::string name, const RgbaImage& image, const bool masked)
{
  contract_pre(image.width > 0 && image.width % 16 == 0);
  contract_pre(image.height > 0 && image.height % 16 == 0);

  auto result = MipConversion{};
  auto& texture = result.texture;
  texture.name = std::move(name);
  texture.width = image.width;
  texture.height = image.height;

  // the colours of the opaque pixels
  auto histogram = std::unordered_map<uint32_t, size_t>{};
  const auto pixelCount = image.width * image.height;
  for (size_t i = 0; i < pixelCount; ++i)
  {
    if (masked && isTransparentPixel(image, i))
    {
      continue;
    }
    const auto* p = &image.pixels[i * 4];
    ++histogram[packRgb({p[0], p[1], p[2]})];
  }
  auto colors = std::vector<ColorCount>{};
  colors.reserve(histogram.size());
  for (const auto& [rgb, count] : histogram)
  {
    colors.push_back(ColorCount{
      {int((rgb >> 16) & 0xFF), int((rgb >> 8) & 0xFF), int(rgb & 0xFF)}, count});
  }
  // deterministic order regardless of the hash map
  std::ranges::sort(colors, [](const auto& lhs, const auto& rhs) {
    return packRgb(lhs.rgb) < packRgb(rhs.rgb);
  });
  result.sourceColors = colors.size();

  const auto maxColors = masked ? size_t(255) : size_t(256);
  auto palette = Palette{};
  if (colors.size() <= maxColors)
  {
    for (const auto& color : colors)
    {
      palette.push_back(color.rgb);
    }
  }
  else
  {
    palette = medianCut(colors, maxColors);
    refinePalette(palette, colors);
  }
  if (palette.empty())
  {
    palette.push_back({0, 0, 0});
  }

  texture.palette.assign(PaletteBytes, 0);
  for (size_t i = 0; i < palette.size(); ++i)
  {
    for (size_t c = 0; c < 3; ++c)
    {
      texture.palette[i * 3 + c] = static_cast<unsigned char>(palette[i][c]);
    }
  }
  if (masked)
  {
    texture.palette[TransparentIndex * 3] = 0;
    texture.palette[TransparentIndex * 3 + 1] = 0;
    texture.palette[TransparentIndex * 3 + 2] = 255;
  }

  auto cache = std::unordered_map<uint32_t, unsigned char>{};
  const auto indexOf = [&](const std::array<int, 3>& rgb) {
    const auto key = packRgb(rgb);
    if (const auto it = cache.find(key); it != cache.end())
    {
      return it->second;
    }
    const auto index = static_cast<unsigned char>(nearestIndex(palette, rgb));
    cache.emplace(key, index);
    return index;
  };

  auto level = image;
  auto used = std::vector<bool>(256, false);
  auto errorSum = 0.0;
  auto opaquePixels = size_t(0);
  for (size_t mip = 0; mip < MipLevels; ++mip)
  {
    if (mip > 0)
    {
      level = halve(level, masked);
    }
    auto& indices = texture.mips[mip];
    indices.resize(level.width * level.height);
    for (size_t i = 0; i < indices.size(); ++i)
    {
      if (masked && isTransparentPixel(level, i))
      {
        indices[i] = static_cast<unsigned char>(TransparentIndex);
        if (mip == 0)
        {
          ++result.transparentPixels;
        }
        continue;
      }
      const auto* p = &level.pixels[i * 4];
      const auto rgb = std::array<int, 3>{p[0], p[1], p[2]};
      const auto index = indexOf(rgb);
      indices[i] = index;
      if (mip == 0)
      {
        used[index] = true;
        errorSum += std::sqrt(double(distanceSquared(rgb, palette[index])));
        ++opaquePixels;
      }
    }
  }

  result.colorsUsed = size_t(std::ranges::count(used, true));
  result.meanError = opaquePixels > 0 ? errorSum / double(opaquePixels) : 0.0;
  return result;
}

} // namespace tb::mcp
