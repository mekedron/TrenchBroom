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

#include "mcp/tools/WadTools.h"

#include "ToolUtils.h"
#include "img/DecodeImage.h"
#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/ToolRegistry.h"
#include "mcp/tools/WadFile.h"
#include "mdl/Entity.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/NodeContents.h"
#include "mdl/WadPropertyUtils.h"
#include "mdl/WorldNode.h"

#include "kd/string_compare.h"
#include "kd/string_format.h"

#include <fmt/format.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace tb::mcp
{
namespace
{
using namespace schema;

constexpr auto MaxImages = size_t(512);
/** Larger textures are refused; Half-Life's tools and engine prefer at most 512. */
constexpr auto MaxTextureSide = size_t(4096);
constexpr auto LargeTextureSide = size_t(512);

std::optional<std::string> readFileBytes(const std::filesystem::path& path)
{
  auto stream = std::ifstream{path, std::ios::binary};
  if (!stream)
  {
    return std::nullopt;
  }
  auto buffer = std::ostringstream{};
  buffer << stream.rdbuf();
  if (stream.bad())
  {
    return std::nullopt;
  }
  return std::move(buffer).str();
}

size_t roundTo16(const size_t value, const std::string_view mode)
{
  const auto down = std::max(size_t(16), value / 16 * 16);
  const auto up = (value + 15) / 16 * 16;
  if (mode == "up")
  {
    return up;
  }
  if (mode == "down")
  {
    return down;
  }
  return value - down < up - value ? down : up;
}

struct ImageSpec
{
  std::filesystem::path path;
  std::string name;
};

struct PackedTexture
{
  ImageSpec spec;
  MipConversion conversion;
  size_t sourceWidth = 0;
  size_t sourceHeight = 0;
  bool hadAlpha = false;
};

Result<std::vector<ImageSpec>, ToolError> parseImages(const Args& args)
{
  auto result = std::vector<ImageSpec>{};
  for (const auto& item : args.get<Json>("images"))
  {
    auto spec = ImageSpec{};
    const auto pathString =
      item.is_string() ? item.get<std::string>() : item["path"].get<std::string>();
    spec.path = std::filesystem::path{pathString}.lexically_normal();
    if (!spec.path.is_absolute())
    {
      return makeError(
        ErrorCode::InvalidArgument,
        fmt::format("The image path {} is not absolute.", pathString),
        "Pass absolute paths of PNG, TGA or BMP files.");
    }
    spec.name = item.is_object() && item.contains("name")
                  ? item["name"].get<std::string>()
                  : spec.path.stem().string();
    if (const auto problem = checkMipTextureName(spec.name))
    {
      return makeError(
        ErrorCode::InvalidArgument,
        *problem,
        "Pass a shorter name for the image: {\"path\": ..., \"name\": \"brick1\"}; "
        "names start with { for transparent textures, ! for water, +0 for animation "
        "frames.");
    }
    if (std::ranges::any_of(result, [&](const auto& other) {
          return kdl::ci::str_is_equal(other.name, spec.name);
        }))
    {
      return makeError(
        ErrorCode::InvalidArgument,
        fmt::format("The texture name '{}' is used twice.", spec.name),
        "Texture names are case-insensitive; give each image its own name.");
    }
    result.push_back(std::move(spec));
  }
  return result;
}

Result<PackedTexture, ToolError> packImage(
  CallContext& context, const ImageSpec& spec, const std::string& resize)
{
  const auto bytes = readFileBytes(spec.path);
  if (!bytes)
  {
    return makeError(
      ErrorCode::IoError,
      fmt::format("Cannot read {}.", spec.path.string()),
      "Check the path.");
  }
  auto decoded = img::decodeImage(
    reinterpret_cast<const unsigned char*>(bytes->data()), bytes->size());
  if (decoded.is_error())
  {
    return makeError(
      ErrorCode::InvalidArgument,
      fmt::format(
        "{} is not a readable image: {}",
        spec.path.string(),
        std::get<Error>(decoded.error()).msg),
      "Pass PNG, TGA or BMP files.");
  }
  auto image = RgbaImage{
    decoded.value().width, decoded.value().height, std::move(decoded.value().pixels)};

  auto result = PackedTexture{spec, {}, image.width, image.height, false};
  for (size_t i = 3; i < image.pixels.size(); i += 4)
  {
    if (image.pixels[i] < 128)
    {
      result.hadAlpha = true;
      break;
    }
  }

  if (image.width > MaxTextureSide || image.height > MaxTextureSide)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      fmt::format(
        "{} is {}x{}; textures can be at most {}x{}.",
        spec.path.string(),
        image.width,
        image.height,
        MaxTextureSide,
        MaxTextureSide),
      "Scale the image down first.");
  }
  if (image.width % 16 != 0 || image.height % 16 != 0)
  {
    const auto width = roundTo16(image.width, resize);
    const auto height = roundTo16(image.height, resize);
    if (resize == "none")
    {
      return makeError(
        ErrorCode::InvalidArgument,
        fmt::format(
          "{} is {}x{}; Half-Life textures need sizes that are multiples of 16 (e.g. "
          "{}x{}).",
          spec.path.string(),
          image.width,
          image.height,
          width,
          height),
        "Pass resize: \"nearest\", \"up\" or \"down\" to scale the images to multiples "
        "of 16, or resize them yourself.");
    }
    image = resizeImage(image, width, height);
  }
  if (image.width > LargeTextureSide || image.height > LargeTextureSide)
  {
    context.warn(
      "TEXTURE_LARGE",
      fmt::format(
        "{} is {}x{}; Half-Life's compile tools and engine may refuse or scale down "
        "textures larger than {}x{}.",
        spec.name,
        image.width,
        image.height,
        LargeTextureSide,
        LargeTextureSide));
  }
  const auto masked = isMaskedTextureName(spec.name);
  if (result.hadAlpha && !masked)
  {
    context.warn(
      "ALPHA_IGNORED",
      fmt::format(
        "{} has transparent pixels, but only textures whose name starts with {{ are "
        "transparent in Half-Life; they become opaque. Name it {{{} to keep them.",
        spec.path.filename().string(),
        spec.name.substr(0, MaxMipTextureName - 1)));
  }
  result.conversion = makeMipTexture(spec.name, image, masked);
  return result;
}

bool sameWadPath(const std::string& listed, const std::filesystem::path& path)
{
  auto normalized = listed;
  std::ranges::replace(normalized, '\\', '/');
  const auto listedPath = std::filesystem::path{normalized}.lexically_normal();
  return listedPath == path
         || (!listedPath.is_absolute()
             && kdl::ci::str_is_equal(
               listedPath.filename().string(), path.filename().string()));
}

ToolResult materialsPack(CallContext& context, const Args& args)
{
  auto output = absolutePathArgument(args, "output");
  if (output.is_error())
  {
    return errorOf(output);
  }
  const auto path = output.value();
  if (!kdl::ci::str_is_equal(path.extension().string(), ".wad"))
  {
    return makeError(
      ErrorCode::InvalidArgument,
      fmt::format("{} does not end in .wad.", path.string()),
      "Pass the path of the WAD file to write, e.g. /maps/mymap.wad.");
  }
  auto ec = std::error_code{};
  if (!std::filesystem::is_directory(path.parent_path(), ec))
  {
    return makeError(
      ErrorCode::IoError,
      fmt::format("The folder {} does not exist.", path.parent_path().string()),
      "Write into an existing folder; the server does not create folders.");
  }
  const auto merge = args.getOr("merge", false);
  const auto overwrite = args.getOr("overwrite", false);
  const auto exists = pathExists(path);
  if (exists && !merge && !overwrite)
  {
    return makeError(
      ErrorCode::FileExists,
      fmt::format("{} already exists.", path.string()),
      "Pass merge: true to add the textures to it (replacing those with the same "
      "names), or overwrite: true to replace the file.");
  }

  const auto addToMap = args.getOr("addToMap", false);
  const auto* wadProperty = static_cast<const std::string*>(nullptr);
  if (addToMap)
  {
    if (!context.hasDocument())
    {
      return makeError(
        ErrorCode::NoDocument,
        "addToMap needs an open document.",
        "Open the map (document_open) or pass addToMap: false.");
    }
    const auto& property = context.map().gameInfo().gameConfig.materialConfig.property;
    if (!property)
    {
      return makeError(
        ErrorCode::Unsupported,
        fmt::format(
          "{} loads materials from folders, not from WAD files.",
          context.map().gameInfo().gameConfig.name),
        "Pass addToMap: false.");
    }
    wadProperty = &*property;
  }

  auto images = parseImages(args);
  if (images.is_error())
  {
    return errorOf(images);
  }
  const auto resize = args.getOr<std::string>("resize", "none");

  // keep the textures of the existing file when merging
  auto textures = std::vector<MipTexture>{};
  auto kept = std::vector<std::string>{};
  if (exists && merge)
  {
    auto bytes = readFileBytes(path);
    if (!bytes)
    {
      return makeError(ErrorCode::IoError, fmt::format("Cannot read {}.", path.string()));
    }
    auto wad = readWad(std::move(*bytes));
    if (wad.is_error() || wad.value().version != 3)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        fmt::format("{} is not a Half-Life WAD3 file.", path.string()),
        "Pass merge: false and overwrite: true to replace it.");
    }
    for (const auto& entry : wad.value().entries)
    {
      if (std::ranges::any_of(images.value(), [&](const auto& spec) {
            return kdl::ci::str_is_equal(spec.name, entry.name);
          }))
      {
        continue;
      }
      auto texture = wad.value().mipTexture(entry);
      if (
        texture.is_error() || !texture.value().hasPixels()
        || texture.value().palette.size() != 768)
      {
        context.warn(
          "ENTRY_DROPPED",
          fmt::format(
            "The entry {} of {} is not a Half-Life texture and is dropped.",
            entry.name,
            path.filename().string()));
        continue;
      }
      kept.push_back(entry.name);
      textures.push_back(std::move(texture).value());
    }
  }

  auto packed = Json::array();
  for (const auto& spec : images.value())
  {
    auto texture = packImage(context, spec, resize);
    if (texture.is_error())
    {
      return errorOf(texture);
    }
    const auto& result = texture.value();
    const auto& conversion = result.conversion;
    packed.push_back(Json{
      {"name", conversion.texture.name},
      {"source", spec.path.string()},
      {"width", conversion.texture.width},
      {"height", conversion.texture.height},
      {"sourceWidth", result.sourceWidth},
      {"sourceHeight", result.sourceHeight},
      {"resized",
       result.sourceWidth != conversion.texture.width
         || result.sourceHeight != conversion.texture.height},
      {"masked", isMaskedTextureName(conversion.texture.name)},
      {"transparentPixels", conversion.transparentPixels},
      {"sourceColors", conversion.sourceColors},
      {"colorsUsed", conversion.colorsUsed},
      {"meanColorError", roundForOutput(conversion.meanError)},
    });
    textures.push_back(conversion.texture);
  }

  const auto bytes = writeWad3(textures);
  auto result = Json{
    {"path", path.string()},
    {"bytes", bytes.size()},
    {"textures", std::move(packed)},
    {"kept", kept},
    {"written", false},
    {"addedToMap", false},
  };

  // the map's WAD list
  auto wads = std::vector<std::string>{};
  auto addWad = false;
  if (addToMap)
  {
    const auto& map = context.map();
    if (const auto* value = map.worldNode().entity().property(*wadProperty))
    {
      wads = mdl::splitWadProperty(*value);
    }
    addWad = std::ranges::none_of(
      wads, [&](const auto& listed) { return sameWadPath(listed, path); });
    if (addWad)
    {
      wads.push_back(path.string());
    }
  }

  if (context.dryRun())
  {
    result["wouldDo"] = fmt::format(
      "write {} textures to {}{}",
      textures.size(),
      path.string(),
      addWad ? " and add it to the map's WAD list" : "");
    if (addToMap)
    {
      result["wads"] = wads;
    }
    return result;
  }

  // write atomically: a temporary file renamed over the target
  auto temporary = path;
  temporary += ".tmp";
  {
    auto stream = std::ofstream{temporary, std::ios::binary | std::ios::trunc};
    stream.write(bytes.data(), std::streamsize(bytes.size()));
    stream.close();
    if (!stream)
    {
      std::filesystem::remove(temporary, ec);
      return makeError(
        ErrorCode::IoError,
        fmt::format("Could not write {}.", temporary.string()),
        "Check that the folder is writable.");
    }
  }
  std::filesystem::rename(temporary, path, ec);
  if (ec)
  {
    std::filesystem::remove(temporary, ec);
    return makeError(
      ErrorCode::IoError,
      fmt::format("Could not replace {}.", path.string()),
      "Check that the file is not locked and the folder is writable.");
  }
  result["written"] = true;

  if (addToMap)
  {
    auto& map = context.map();
    if (addWad)
    {
      auto entity = map.worldNode().entity();
      entity.addOrUpdateProperty(*wadProperty, mdl::joinWadProperty(wads));
      if (!mdl::updateNodeContents(
            map,
            "AI: Pack Materials",
            {{&map.worldNode(), mdl::NodeContents{std::move(entity)}}}))
      {
        return context.operationFailed(
          fmt::format("The WAD was written, but could not be added to the map."));
      }
      context.setUndoStep("AI: Pack Materials");
      result["addedToMap"] = true;
    }
    else
    {
      result["hint"] =
        "The WAD was already in the map's WAD list; run materials_reload to load the "
        "new textures.";
    }
    result["wads"] = wads;
  }
  return result;
}

// wad_list

std::string_view lumpTypeName(const int type)
{
  switch (type)
  {
  case 0x40:
    return "palette";
  case 0x42:
    return "picture";
  case 0x43:
  case 0x44:
    return "miptex";
  case 0x45:
    return "font";
  default:
    return "other";
  }
}

ToolResult wadList(CallContext&, const Args& args)
{
  auto input = absolutePathArgument(args, "path");
  if (input.is_error())
  {
    return errorOf(input);
  }
  const auto path = input.value();
  auto bytes = readFileBytes(path);
  if (!bytes)
  {
    return makeError(
      ErrorCode::IoError,
      fmt::format("Cannot read {}.", path.string()),
      "Check the path; materials_collections_get lists the map's WADs.");
  }
  auto wad = readWad(std::move(*bytes));
  if (wad.is_error())
  {
    return makeError(
      ErrorCode::InvalidArgument,
      fmt::format("{}: {}", path.string(), std::get<std::string>(wad.error())),
      "Pass a WAD2 or WAD3 file.");
  }

  const auto filter = args.getOr<std::string>("filter", "");
  const auto offset = size_t(args.getOr<int64_t>("offset", 0));
  const auto limit = size_t(args.getOr<int64_t>("limit", 500));
  auto entries = Json::array();
  auto matched = size_t(0);
  const auto& file = wad.value();
  for (const auto& entry : file.entries)
  {
    if (!filter.empty() && !kdl::ci::str_matches_glob(entry.name, filter))
    {
      continue;
    }
    ++matched;
    if (matched <= offset || entries.size() >= limit)
    {
      continue;
    }
    auto json = Json{
      {"name", entry.name},
      {"type", std::string{lumpTypeName(entry.type)}},
      {"size", entry.size},
    };
    if (
      WadFile::isMipTexture(entry) && entry.offset + 24 <= file.bytes.size()
      && entry.size >= 24)
    {
      const auto read32 = [&](const size_t at) {
        auto value = size_t(0);
        for (size_t i = 0; i < 4; ++i)
        {
          value |= size_t(static_cast<unsigned char>(file.bytes[at + i])) << (8 * i);
        }
        return value;
      };
      json["width"] = read32(entry.offset + 16);
      json["height"] = read32(entry.offset + 20);
      json["masked"] = isMaskedTextureName(entry.name);
    }
    entries.push_back(std::move(json));
  }

  return Json{
    {"path", path.string()},
    {"version", file.version},
    {"count", file.entries.size()},
    {"matched", matched},
    {"offset", offset},
    {"entries", std::move(entries)},
    {"truncated", matched > offset + limit},
  };
}

} // namespace

void registerWadTools(ToolRegistry& registry)
{
  registry.add(
    ToolDef{"materials_pack"}
      .title("Pack Materials")
      .description(
        "Builds a Half-Life WAD3 file from PNG, TGA or BMP images: each image is "
        "quantized to its own 256-colour palette (median cut refined by k-means), "
        "gets the four mip levels and is named after its file (or name, at most 15 "
        "characters). Names starting with { are transparent textures: pixels with "
        "alpha below 128 and pure blue (0, 0, 255) become palette index 255. Sizes "
        "must be multiples of 16; resize: nearest, up or down scales images that are "
        "not (default none: refuse). merge adds to an existing WAD (replacing textures "
        "with the same names); overwrite replaces it. addToMap appends the WAD to the "
        "document's WAD list (one undo step) so the textures can be applied at once. "
        "Example: {\"output\": \"/maps/mymap.wad\", \"images\": [\"/art/brick_red.png\", "
        "{\"path\": \"/art/fence.png\", \"name\": \"{fence1\"}], \"resize\": "
        "\"nearest\", "
        "\"addToMap\": true}")
      .input(object({
        field("output", string().nonEmpty())
          .required()
          .describe("Absolute path of the WAD file to write (.wad)"),
        field(
          "images",
          array(
            oneOf({
              string().nonEmpty().describe("Absolute path of an image"),
              object({
                field("path", string().nonEmpty())
                  .required()
                  .describe("Absolute path of the image"),
                field("name", string().nonEmpty())
                  .describe("Texture name, at most 15 characters (default: the file name "
                            "without extension)"),
              }),
            }))
            .nonEmpty()
            .maxSize(MaxImages))
          .required()
          .describe("The images: paths, or {path, name}"),
        field("resize", enumOf({"none", "nearest", "up", "down"}))
          .describe(
            "Scale images whose sizes are not multiples of 16 to the nearest, next "
            "larger or next smaller multiple (default none: refuse)"),
        field("merge", boolean())
          .describe("Add to an existing WAD, keeping its other textures (default false)"),
        field("overwrite", boolean())
          .describe("Replace an existing WAD file (default false)"),
        field("addToMap", boolean())
          .describe("Append the WAD to the document's WAD list (default false)"),
      }))
      .output(object({
        field("path", string()).describe("The WAD file"),
        field("bytes", integer()).describe("The size of the WAD file"),
        field("textures", array(any()))
          .describe("Per image: {name, source, width, height, sourceWidth, sourceHeight, "
                    "resized, masked, transparentPixels, sourceColors, colorsUsed, "
                    "meanColorError}"),
        field("kept", array(string())).describe("Textures kept from the existing WAD"),
        field("written", boolean()).describe("The file was written (false in dry runs)"),
        field("addedToMap", boolean()).describe("The WAD was added to the WAD list"),
        field("wads", array(string())).describe("The map's WAD list (addToMap)"),
        field("hint", string()).describe("A next step, if any"),
        field("wouldDo", string()).describe("Dry run: what the call would do"),
      }))
      .mutation(Mutation::External)
      .documentUse(DocumentUse::Optional)
      .handler(materialsPack));

  registry.add(
    ToolDef{"wad_list"}
      .title("List WAD")
      .description(
        "Lists the entries of a WAD2 (Quake) or WAD3 (Half-Life) file: name, type "
        "(miptex, picture, palette, font), size and, for textures, width, height and "
        "whether they are transparent ({ names). filter is a case-insensitive glob; "
        "offset and limit page long lists. Read-only. Example: {\"path\": "
        "\"/games/Half-Life/valve/halflife.wad\", \"filter\": \"*door*\", \"limit\": 50}")
      .input(object({
        field("path", string().nonEmpty())
          .required()
          .describe("Absolute path of the WAD file"),
        field("filter", string()).describe("Glob on the names, e.g. 'c1a*' or '{*'"),
        field("offset", integer().min(0)).describe("Matches to skip (default 0)"),
        field("limit", integer().min(1).max(5000))
          .describe("Most entries to return (default 500)"),
      }))
      .output(object({
        field("path", string()).describe("The WAD file"),
        field("version", integer()).describe("2 (Quake) or 3 (Half-Life)"),
        field("count", integer()).describe("All entries of the file"),
        field("matched", integer()).describe("Entries matching the filter"),
        field("offset", integer()).describe("The skipped matches"),
        field("entries", array(any()))
          .describe("{name, type, size, width?, height?, masked?}"),
        field("truncated", boolean()).describe("More matches follow (raise offset)"),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::None)
      .handler(wadList));
}

} // namespace tb::mcp
