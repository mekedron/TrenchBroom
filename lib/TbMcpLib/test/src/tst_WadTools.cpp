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

#include "TestEnvironment.h"
#include "fs/Reader.h"
#include "fs/TestEnvironment.h"
#include "gl/Texture.h"
#include "mcp/Image.h"
#include "mcp/Json.h"
#include "mcp/McpToolFixture.h"
#include "mcp/tools/WadFile.h"
#include "mdl/LoadMipTexture.h"

#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

/** An image with a colour gradient: width * height distinct colours at most. */
RgbaImage gradient(const size_t width, const size_t height)
{
  auto image = makeImage(width, height, Rgba8{0, 0, 0, 255});
  for (size_t y = 0; y < height; ++y)
  {
    for (size_t x = 0; x < width; ++x)
    {
      auto* p = &image.pixels[(y * width + x) * 4];
      p[0] = static_cast<unsigned char>(x * 255 / (width - 1));
      p[1] = static_cast<unsigned char>(y * 255 / (height - 1));
      p[2] = static_cast<unsigned char>((x + y) * 127 / (width + height - 2));
    }
  }
  return image;
}

void setPixel(RgbaImage& image, const size_t x, const size_t y, const Rgba8& color)
{
  std::copy(
    color.begin(), color.end(), image.pixels.begin() + long((y * image.width + x) * 4));
}

std::string png(const RgbaImage& image)
{
  auto bytes = encodePng(image);
  REQUIRE(bytes);
  return *bytes;
}

bool hasWarning(const Json& result, const std::string& code)
{
  if (!result.contains("warnings"))
  {
    return false;
  }
  for (const auto& warning : result["warnings"])
  {
    if (warning["code"] == code)
    {
      return true;
    }
  }
  return false;
}

} // namespace

TEST_CASE("WadFile")
{
  SECTION("makeMipTexture")
  {
    SECTION("few colours are kept exactly")
    {
      auto image = makeImage(32, 16, Rgba8{10, 20, 30, 255});
      setPixel(image, 3, 3, Rgba8{200, 100, 50, 255});
      const auto result = makeMipTexture("test", image, false);
      CHECK(result.sourceColors == 2);
      CHECK(result.colorsUsed == 2);
      CHECK(result.meanError == 0.0);
      const auto& texture = result.texture;
      CHECK(texture.width == 32);
      CHECK(texture.height == 16);
      CHECK(texture.mips[0].size() == 512);
      CHECK(texture.mips[1].size() == 128);
      CHECK(texture.mips[2].size() == 32);
      CHECK(texture.mips[3].size() == 8);
      CHECK(pixelAt(mipTextureImage(texture, 0), 3, 3) == Rgba8{200, 100, 50, 255});
      CHECK(pixelAt(mipTextureImage(texture, 0), 0, 0) == Rgba8{10, 20, 30, 255});
    }

    SECTION("many colours are quantized to 256")
    {
      const auto image = gradient(64, 64);
      const auto result = makeMipTexture("gradient", image, false);
      CHECK(result.sourceColors > 256);
      CHECK(result.colorsUsed <= 256);
      CHECK(result.meanError < 8.0);
    }

    SECTION("masked textures")
    {
      auto image = gradient(16, 16);
      setPixel(image, 0, 0, Rgba8{0, 0, 0, 0});
      setPixel(image, 1, 0, Rgba8{0, 0, 255, 255});
      const auto result = makeMipTexture("{grate", image, true);
      CHECK(result.transparentPixels == 2);
      const auto& texture = result.texture;
      CHECK(texture.mips[0][0] == TransparentIndex);
      CHECK(texture.mips[0][1] == TransparentIndex);
      CHECK(texture.palette[TransparentIndex * 3 + 2] == 255);
      CHECK(result.colorsUsed <= 255);
      CHECK(pixelAt(mipTextureImage(texture, 0), 0, 0)[3] == 0);
      CHECK(pixelAt(mipTextureImage(texture, 0), 5, 5)[3] == 255);
    }
  }

  SECTION("WAD3 round trip")
  {
    const auto a = makeMipTexture("brick", gradient(16, 32), false).texture;
    const auto b =
      makeMipTexture("{fence", makeImage(16, 16, {1, 2, 3, 255}), true).texture;
    auto wad = readWad(writeWad3({a, b}));
    REQUIRE(wad.is_success());
    CHECK(wad.value().version == 3);
    REQUIRE(wad.value().entries.size() == 2);
    CHECK(wad.value().find("BRICK") == size_t(0));
    CHECK(wad.value().find("{fence") == size_t(1));
    CHECK(wad.value().find("none") == std::nullopt);
    CHECK(wad.value().mipTexture(wad.value().entries[0]).value() == a);
    CHECK(wad.value().mipTexture(wad.value().entries[1]).value() == b);

    CHECK(readWad("WAD3").is_error());
    CHECK(readWad("PACK00000000").is_error());
  }

  SECTION("the editor's loader reads written textures")
  {
    const auto texture = makeMipTexture("{fence", gradient(32, 16), true).texture;
    const auto bytes = writeMipTexture(texture);
    auto reader = fs::Reader::from(bytes.data(), bytes.data() + bytes.size());
    const auto loaded = mdl::loadHlMipTexture(reader, true);
    REQUIRE(loaded.is_success());
    CHECK(loaded.value().width() == 32);
    CHECK(loaded.value().height() == 16);
  }

  SECTION("checkMipTextureName")
  {
    CHECK(checkMipTextureName("brick1") == std::nullopt);
    CHECK(checkMipTextureName("{fence") == std::nullopt);
    CHECK(checkMipTextureName("+0lava!") == std::nullopt);
    CHECK(checkMipTextureName("") != std::nullopt);
    CHECK(checkMipTextureName("abcdefghijklmnop") != std::nullopt);
    CHECK(checkMipTextureName("two words") != std::nullopt);
  }

  SECTION("resizeImage")
  {
    const auto image = makeImage(40, 30, Rgba8{100, 150, 200, 255});
    const auto smaller = resizeImage(image, 32, 16);
    CHECK(smaller.width == 32);
    CHECK(smaller.height == 16);
    CHECK(pixelAt(smaller, 10, 10) == Rgba8{100, 150, 200, 255});
    const auto larger = resizeImage(image, 48, 32);
    CHECK(pixelAt(larger, 47, 31) == Rgba8{100, 150, 200, 255});
  }
}

TEST_CASE("WadTools")
{
  auto fixture = McpToolFixture{};
  auto env = fs::TestEnvironment{};
  env.createFile("brick.png", png(gradient(32, 32)));
  env.createFile("odd.png", png(makeImage(40, 30, Rgba8{90, 90, 90, 255})));
  auto fence = makeImage(16, 16, Rgba8{120, 120, 120, 255});
  setPixel(fence, 4, 4, Rgba8{0, 0, 0, 0});
  env.createFile("fence.png", png(fence));
  const auto path = [&](const std::string& name) { return (env.dir() / name).string(); };

  SECTION("materials_pack")
  {
    SECTION("writes a WAD3 file")
    {
      const auto result = fixture.call(
        "materials_pack",
        Json{
          {"output", path("test.wad")},
          {"images",
           Json{
             path("brick.png"), Json{{"path", path("fence.png")}, {"name", "{fence"}}}}});
      const auto& packed = result["result"];
      CHECK(packed["written"] == true);
      REQUIRE(packed["textures"].size() == 2);
      CHECK(packed["textures"][0]["name"] == "brick");
      CHECK(packed["textures"][0]["width"] == 32);
      CHECK(packed["textures"][1]["masked"] == true);
      CHECK(packed["textures"][1]["transparentPixels"] == 1);
      CHECK(env.fileExists("test.wad"));

      const auto wad = readWad(env.loadFile("test.wad"));
      REQUIRE(wad.is_success());
      CHECK(wad.value().entries.size() == 2);

      const auto listed = fixture.call("wad_list", Json{{"path", path("test.wad")}});
      CHECK(listed["version"] == 3);
      CHECK(listed["count"] == 2);
      CHECK(listed["entries"][1]["name"] == "{fence");
      CHECK(listed["entries"][1]["width"] == 16);
      CHECK(listed["entries"][1]["masked"] == true);
    }

    SECTION("sizes must be multiples of 16")
    {
      const auto error = fixture.callExpectingError(
        "materials_pack",
        Json{{"output", path("odd.wad")}, {"images", Json{path("odd.png")}}});
      CHECK(error.code == ErrorCode::InvalidArgument);
      CHECK(error.message.find("multiples of 16") != std::string::npos);
      CHECK(error.message.find("48x32") != std::string::npos);

      const auto nearest = fixture.call(
        "materials_pack",
        Json{
          {"output", path("odd.wad")},
          {"images", Json{path("odd.png")}},
          {"resize", "nearest"}});
      const auto& texture = nearest["result"]["textures"][0];
      CHECK(texture["width"] == 48);
      CHECK(texture["height"] == 32);
      CHECK(texture["resized"] == true);

      const auto down = fixture.call(
        "materials_pack",
        Json{
          {"output", path("odd.wad")},
          {"images", Json{path("odd.png")}},
          {"resize", "down"},
          {"overwrite", true}});
      CHECK(down["result"]["textures"][0]["width"] == 32);
      CHECK(down["result"]["textures"][0]["height"] == 16);
    }

    SECTION("names")
    {
      const auto tooLong = fixture.callExpectingError(
        "materials_pack",
        Json{
          {"output", path("names.wad")},
          {"images",
           Json{Json{{"path", path("brick.png")}, {"name", "a_very_long_name1"}}}}});
      CHECK(tooLong.code == ErrorCode::InvalidArgument);
      CHECK(tooLong.message.find("at most 15") != std::string::npos);

      const auto twice = fixture.callExpectingError(
        "materials_pack",
        Json{
          {"output", path("names.wad")},
          {"images",
           Json{
             path("brick.png"), Json{{"path", path("fence.png")}, {"name", "BRICK"}}}}});
      CHECK(twice.code == ErrorCode::InvalidArgument);

      const auto alpha = fixture.call(
        "materials_pack",
        Json{{"output", path("alpha.wad")}, {"images", Json{path("fence.png")}}});
      CHECK(hasWarning(alpha, "ALPHA_IGNORED"));
    }

    SECTION("existing files: merge and overwrite")
    {
      fixture.call(
        "materials_pack",
        Json{{"output", path("merge.wad")}, {"images", Json{path("brick.png")}}});

      const auto exists = fixture.callExpectingError(
        "materials_pack",
        Json{{"output", path("merge.wad")}, {"images", Json{path("fence.png")}}});
      CHECK(exists.code == ErrorCode::FileExists);

      const auto merged = fixture.call(
        "materials_pack",
        Json{
          {"output", path("merge.wad")},
          {"images", Json{Json{{"path", path("fence.png")}, {"name", "{fence"}}}},
          {"merge", true}});
      CHECK(merged["result"]["kept"] == Json{"brick"});
      CHECK(readWad(env.loadFile("merge.wad")).value().entries.size() == 2);

      fixture.call(
        "materials_pack",
        Json{
          {"output", path("merge.wad")},
          {"images", Json{path("brick.png")}},
          {"overwrite", true}});
      CHECK(readWad(env.loadFile("merge.wad")).value().entries.size() == 1);
    }

    SECTION("dry run")
    {
      const auto result = fixture.call(
        "materials_pack",
        Json{
          {"output", path("dry.wad")},
          {"images", Json{path("brick.png")}},
          {"dryRun", true}});
      CHECK(result["result"]["written"] == false);
      CHECK(result["result"]["wouldDo"].is_string());
      CHECK_FALSE(env.fileExists("dry.wad"));
    }

    SECTION("addToMap")
    {
      const auto noDocument = fixture.callExpectingError(
        "materials_pack",
        Json{
          {"output", path("map.wad")},
          {"images", Json{path("brick.png")}},
          {"addToMap", true}});
      CHECK(noDocument.code == ErrorCode::NoDocument);

      fixture.call("document_new", Json{{"game", "Quake"}});
      const auto result = fixture.call(
        "materials_pack",
        Json{
          {"output", path("map.wad")},
          {"images", Json{path("brick.png")}},
          {"addToMap", true}});
      CHECK(result["result"]["addedToMap"] == true);
      CHECK(result["result"]["wads"] == Json{path("map.wad")});
      CHECK(result["undoStep"] == "AI: Pack Materials");

      const auto again = fixture.call(
        "materials_pack",
        Json{
          {"output", path("map.wad")},
          {"images", Json{path("brick.png")}},
          {"addToMap", true},
          {"overwrite", true}});
      CHECK(again["result"]["addedToMap"] == false);
      CHECK(again["result"]["hint"].is_string());
      CHECK(again["result"]["wads"].size() == 1);
    }

    SECTION("errors")
    {
      const auto notWad = fixture.callExpectingError(
        "materials_pack",
        Json{{"output", path("test.txt")}, {"images", Json{path("brick.png")}}});
      CHECK(notWad.code == ErrorCode::InvalidArgument);

      const auto missing = fixture.callExpectingError(
        "materials_pack",
        Json{{"output", path("test.wad")}, {"images", Json{path("missing.png")}}});
      CHECK(missing.code == ErrorCode::IoError);

      env.createFile("broken.png", "not an image");
      const auto broken = fixture.callExpectingError(
        "materials_pack",
        Json{{"output", path("test.wad")}, {"images", Json{path("broken.png")}}});
      CHECK(broken.code == ErrorCode::InvalidArgument);

      const auto relative = fixture.callExpectingError(
        "materials_pack", Json{{"output", path("test.wad")}, {"images", Json{"a.png"}}});
      CHECK(relative.code == ErrorCode::InvalidArgument);
    }
  }

  SECTION("wad_list")
  {
    const auto wadPath = getFixtureRoot() / "test" / "mcp" / "wads" / "cr8_a_excerpt.wad";
    const auto all = fixture.call("wad_list", Json{{"path", wadPath.string()}});
    CHECK(all["count"].get<size_t>() > 0);
    CHECK(all["matched"] == all["count"]);

    const auto limited =
      fixture.call("wad_list", Json{{"path", wadPath.string()}, {"limit", 1}});
    CHECK(limited["entries"].size() == 1);
    CHECK(limited["truncated"] == (all["count"].get<size_t>() > 1));

    const auto none =
      fixture.call("wad_list", Json{{"path", wadPath.string()}, {"filter", "zzz*"}});
    CHECK(none["matched"] == 0);

    env.createFile("text.wad", "hello world!");
    const auto error =
      fixture.callExpectingError("wad_list", Json{{"path", path("text.wad")}});
    CHECK(error.code == ErrorCode::InvalidArgument);
  }
}

} // namespace tb::mcp
