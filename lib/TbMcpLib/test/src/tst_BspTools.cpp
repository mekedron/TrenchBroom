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

#include "fs/TestEnvironment.h"
#include "mcp/AgentCamera.h"
#include "mcp/CameraProjection.h"
#include "mcp/Image.h"
#include "mcp/Json.h"
#include "mcp/McpToolFixture.h"
#include "mcp/tools/BspFile.h"
#include "mcp/tools/BspRender.h"
#include "mcp/tools/WadFile.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

// A minimal BSP writer for the tests

void put32(std::string& out, const uint32_t value)
{
  for (size_t i = 0; i < 4; ++i)
  {
    out.push_back(char((value >> (8 * i)) & 0xFF));
  }
}

void putFloat(std::string& out, const double value)
{
  const auto f = float(value);
  auto bits = uint32_t(0);
  std::memcpy(&bits, &f, sizeof(bits));
  put32(out, bits);
}

void put16(std::string& out, const uint16_t value)
{
  out.push_back(char(value & 0xFF));
  out.push_back(char((value >> 8) & 0xFF));
}

struct TestTexInfo
{
  std::array<double, 4> s = {1, 0, 0, 0};
  std::array<double, 4> t = {0, 1, 0, 0};
  int32_t texture = 0;
};

struct TestFace
{
  std::vector<vm::vec3d> vertices;
  vm::vec3d normal;
  double distance = 0.0;
  size_t texInfo = 0;
  std::array<unsigned char, 4> styles = {0, 255, 255, 255};
  /** The light data of all styles; empty for no lightmap. */
  std::vector<unsigned char> light;
};

struct TestBsp
{
  int version = HalfLifeBspVersion;
  std::string entities;
  /** The texture lumps (writeMipTexture or a header without pixels). */
  std::vector<std::string> textures;
  std::vector<TestTexInfo> texInfos = {TestTexInfo{}};
  std::vector<TestFace> faces;
};

std::string writeBsp(const TestBsp& bsp)
{
  auto lumps = std::array<std::string, 15>{};
  lumps[0] = bsp.entities;
  lumps[0].push_back('\0');

  // textures
  auto& textures = lumps[2];
  put32(textures, uint32_t(bsp.textures.size()));
  auto offset = 4 + 4 * bsp.textures.size();
  for (const auto& texture : bsp.textures)
  {
    put32(textures, uint32_t(offset));
    offset += texture.size();
  }
  for (const auto& texture : bsp.textures)
  {
    textures += texture;
  }

  for (const auto& texInfo : bsp.texInfos)
  {
    for (const auto value : texInfo.s)
    {
      putFloat(lumps[6], value);
    }
    for (const auto value : texInfo.t)
    {
      putFloat(lumps[6], value);
    }
    put32(lumps[6], uint32_t(texInfo.texture));
    put32(lumps[6], 0);
  }

  put32(lumps[12], 0); // edge 0 is unused
  auto vertexCount = uint16_t(0);
  auto edgeCount = uint32_t(1);
  auto surfEdgeCount = uint32_t(0);
  auto bounds = std::optional<vm::bbox3d>{};
  for (size_t i = 0; i < bsp.faces.size(); ++i)
  {
    const auto& face = bsp.faces[i];
    for (size_t k = 0; k < 3; ++k)
    {
      putFloat(lumps[1], face.normal[k]);
    }
    putFloat(lumps[1], face.distance);
    put32(lumps[1], 0);

    const auto firstVertex = vertexCount;
    for (const auto& vertex : face.vertices)
    {
      for (size_t k = 0; k < 3; ++k)
      {
        putFloat(lumps[3], vertex[k]);
      }
      bounds = bounds ? vm::merge(*bounds, vertex) : vm::bbox3d{vertex, vertex};
      ++vertexCount;
    }
    const auto firstSurfEdge = surfEdgeCount;
    for (size_t k = 0; k < face.vertices.size(); ++k)
    {
      put16(lumps[12], uint16_t(firstVertex + k));
      put16(lumps[12], uint16_t(firstVertex + (k + 1) % face.vertices.size()));
      put32(lumps[13], edgeCount++);
      ++surfEdgeCount;
    }

    auto& faces = lumps[7];
    put16(faces, uint16_t(i));
    put16(faces, 0);
    put32(faces, firstSurfEdge);
    put16(faces, uint16_t(face.vertices.size()));
    put16(faces, uint16_t(face.texInfo));
    for (const auto style : face.styles)
    {
      faces.push_back(char(style));
    }
    if (face.light.empty())
    {
      put32(faces, uint32_t(-1));
    }
    else
    {
      put32(faces, uint32_t(lumps[8].size()));
      lumps[8].append(
        reinterpret_cast<const char*>(face.light.data()), face.light.size());
    }
  }

  // one model: the world
  const auto box = bounds.value_or(vm::bbox3d{});
  for (size_t k = 0; k < 3; ++k)
  {
    putFloat(lumps[14], box.min[k]);
  }
  for (size_t k = 0; k < 3; ++k)
  {
    putFloat(lumps[14], box.max[k]);
  }
  for (size_t k = 0; k < 3; ++k)
  {
    putFloat(lumps[14], 0);
  }
  for (size_t k = 0; k < 4; ++k)
  {
    put32(lumps[14], 0);
  }
  put32(lumps[14], 0);
  put32(lumps[14], 0);
  put32(lumps[14], uint32_t(bsp.faces.size()));

  auto out = std::string{};
  put32(out, uint32_t(bsp.version));
  auto position = size_t(4 + 15 * 8);
  for (const auto& lump : lumps)
  {
    put32(out, uint32_t(position));
    put32(out, uint32_t(lump.size()));
    position += lump.size();
  }
  for (const auto& lump : lumps)
  {
    out += lump;
  }
  return out;
}

/** A 16x16 texture of one colour, with a Half-Life palette. */
std::string solidTexture(const std::string& name, const Rgba8& color)
{
  return writeMipTexture(makeMipTexture(name, makeImage(16, 16, color), false).texture);
}

/** A texture header without pixels: the game loads it from a WAD. */
std::string externalTexture(const std::string& name)
{
  auto out = std::string{};
  auto field = std::array<char, 16>{};
  std::copy_n(name.begin(), std::min(name.size(), size_t(15)), field.begin());
  out.append(field.data(), field.size());
  put32(out, 16);
  put32(out, 16);
  for (size_t i = 0; i < 4; ++i)
  {
    put32(out, 0);
  }
  return out;
}

/**
 * A floor from (-64, -64, 0) to (64, 64, 0) facing up. Its lightmap has 9x9 luxels; the
 * four columns with x < 0 get `westLight`, the others `eastLight` (RGB, or gray for
 * Quake).
 */
TestFace floorFace(
  const unsigned char westLight,
  const unsigned char eastLight,
  const size_t bytesPerLuxel = 3,
  const size_t texInfo = 0)
{
  auto face = TestFace{};
  face.vertices = {
    vm::vec3d{-64, -64, 0},
    vm::vec3d{-64, 64, 0},
    vm::vec3d{64, 64, 0},
    vm::vec3d{64, -64, 0},
  };
  face.normal = vm::vec3d{0, 0, 1};
  face.texInfo = texInfo;
  for (size_t y = 0; y < 9; ++y)
  {
    for (size_t x = 0; x < 9; ++x)
    {
      for (size_t c = 0; c < bytesPerLuxel; ++c)
      {
        face.light.push_back(x < 4 ? westLight : eastLight);
      }
    }
  }
  return face;
}

TestBsp floorBsp()
{
  auto bsp = TestBsp{};
  bsp.entities = R"({
"classname" "worldspawn"
"wad" "\half-life\valve\missing.wad"
}
{
"classname" "info_player_start"
"origin" "0 0 36"
"angle" "90"
}
)";
  bsp.textures = {solidTexture("floor", Rgba8{200, 200, 200, 255})};
  bsp.faces = {floorFace(0, 200)};
  return bsp;
}

std::vector<Json> contentOf(const Json& raw, const std::string& type)
{
  auto result = std::vector<Json>{};
  for (const auto& block : raw["content"])
  {
    if (block["type"] == type)
    {
      result.push_back(block);
    }
  }
  return result;
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

const Json& region(const Json& view, const std::string& name)
{
  for (const auto& entry : view["regions"])
  {
    if (entry["region"] == name)
    {
      return entry;
    }
  }
  FAIL("no region " << name);
  return view;
}

Json topView()
{
  return Json{{"view", "top"}, {"center", Json{0, 0, 0}}, {"zoom", 1}};
}

} // namespace

TEST_CASE("BspFile")
{
  SECTION("readBsp")
  {
    const auto bytes = writeBsp(floorBsp());
    const auto bsp = readBsp(bytes);
    REQUIRE(bsp.is_success());
    const auto& data = bsp.value();
    CHECK(data.isHalfLife());
    CHECK(data.faces.size() == 1);
    CHECK(data.models.size() == 1);
    CHECK(data.textures.size() == 1);
    CHECK(data.textures[0].name == "floor");
    CHECK(data.textures[0].hasPixels());
    CHECK(data.entities.size() == 2);
    CHECK(data.wadPaths() == std::vector<std::string>{"\\half-life\\valve\\missing.wad"});

    const auto& face = data.faces[0];
    CHECK(face.vertices.size() == 4);
    CHECK(face.lightWidth == 9);
    CHECK(face.lightHeight == 9);
    CHECK(face.lightMinS == -64);
    CHECK(face.lightOffset == size_t(0));
    CHECK(data.surface(face) == BspSurface::Normal);

    const auto point = face.pointAt(16, -32);
    CHECK(point.x() == Catch::Approx(16));
    CHECK(point.y() == Catch::Approx(-32));
    CHECK(point.z() == Catch::Approx(0));
    CHECK(faceContains(face, vm::vec3d{0, 0, 0}));
    CHECK_FALSE(faceContains(face, vm::vec3d{70, 0, 0}));
    CHECK(faceContains(face, vm::vec3d{70, 0, 0}, 8.0));

    const auto start = playerStart(data);
    REQUIRE(start);
    CHECK(start->origin == vm::vec3d{0, 0, 36});
    CHECK(start->yaw == 90.0);
    CHECK(bspFloorBelow(data, vm::vec3d{0, 0, 100}) == 0.0);
    CHECK(bspFloorBelow(data, vm::vec3d{100, 0, 100}) == std::nullopt);
  }

  SECTION("unsupported formats")
  {
    auto bytes = writeBsp(floorBsp());
    bytes.replace(0, 4, "IBSP");
    const auto ibsp = readBsp(bytes);
    REQUIRE(ibsp.is_error());
    CHECK(std::get<std::string>(ibsp.error()).find("Quake 2") != std::string::npos);

    CHECK(readBsp("not a bsp").is_error());
    CHECK(readBsp(std::string(200, '\0')).is_error());
  }

  SECTION("sampleLight and lightmapStats")
  {
    const auto bsp = readBsp(writeBsp(floorBsp())).value();
    const auto weights = lightStyleWeights(bsp, LightStyleSet::Initial);
    const auto& face = bsp.faces[0];
    CHECK(sampleLight(bsp, face, -64, 0, weights).x() == Catch::Approx(0));
    CHECK(sampleLight(bsp, face, 64, 0, weights).x() == Catch::Approx(200));
    // halfway between the luxels at s = -16 (dark) and s = 0 (lit)
    CHECK(sampleLight(bsp, face, -8, 0, weights).x() == Catch::Approx(100));

    const auto all = lightmapStats(bsp, weights);
    CHECK(all.faces == 1);
    CHECK(all.luxels == 81);
    CHECK(all.darkFraction == Catch::Approx(36.0 / 81.0));
    CHECK(all.meanLight == Catch::Approx(200.0 * 45.0 / 81.0).epsilon(0.01));

    const auto west =
      lightmapStats(bsp, weights, vm::bbox3d{{-100, -100, -10}, {-10, 100, 10}});
    CHECK(west.luxels == 36);
    CHECK(west.darkFraction == 1.0);
  }

  SECTION("light styles")
  {
    auto test = floorBsp();
    test.entities += R"({
"classname" "light"
"targetname" "switch"
"style" "32"
"spawnflags" "1"
}
)";
    auto& face = test.faces[0];
    face.styles = {0, 32, 255, 255};
    face.light.clear();
    face.light.insert(face.light.end(), 81 * 3, 50);
    face.light.insert(face.light.end(), 81 * 3, 100);
    const auto bsp = readBsp(writeBsp(test)).value();

    const auto mean = [&](const LightStyleWeights& weights) {
      return lightmapStats(bsp, weights).meanLight;
    };
    CHECK(mean(lightStyleWeights(bsp, LightStyleSet::Initial)) == Catch::Approx(50));
    CHECK(mean(lightStyleWeights(bsp, LightStyleSet::Base)) == Catch::Approx(50));
    CHECK(mean(lightStyleWeights(bsp, LightStyleSet::All)) == Catch::Approx(150));
    CHECK(mean(lightStyleWeights(std::vector<int>{32})) == Catch::Approx(150));
  }

  SECTION("parseEntityLump")
  {
    const auto entities =
      parseEntityLump(R"({ "classname" "worldspawn" "wad" "a.wad;b.wad" }
{ "classname" "func_door" "model" "*1" })");
    REQUIRE(entities.size() == 2);
    CHECK(entities[0].classname() == "worldspawn");
    CHECK(*entities[0].property("wad") == "a.wad;b.wad");
    CHECK(entities[1].property("model") != nullptr);
    CHECK(entities[1].property("origin") == nullptr);
  }
}

TEST_CASE("BspRender")
{
  const auto bsp = readBsp(writeBsp(floorBsp())).value();
  auto textures = BspTextureImages{
    {mipTextureImage(bsp.textures[0], 0), mipTextureImage(bsp.textures[0], 1)}};
  const auto camera =
    orthographicCamera(OrthoView::Top, vm::vec3d{0, 0, 0}, 1.0, vm::bbox3d{256.0});
  const auto projection = ImageProjection::create(camera, 128, 128).value();

  auto options = BspRenderOptions{};
  options.styles = lightStyleWeights(bsp, LightStyleSet::Initial);

  SECTION("lit")
  {
    const auto result = renderBsp(bsp, textures, projection, options);
    CHECK(result.facesDrawn == 1);
    CHECK(pixelAt(result.image, 10, 64) == Rgba8{0, 0, 0, 255});
    const auto lit = pixelAt(result.image, 118, 64);
    CHECK(int(lit[0]) == Catch::Approx(200.0 * 200.0 / 255.0).margin(1.5));
    CHECK(result.stats.coverage == Catch::Approx(1.0));
    CHECK(result.regions[3].darkFraction == 1.0);
    CHECK(result.regions[5].darkFraction == 0.0);
    CHECK(result.regions[5].meanLight == Catch::Approx(200).margin(0.5));
    CHECK(imageRegionName(3) == "left");
  }

  SECTION("fullbright and lightmap")
  {
    options.shading = BspShading::Fullbright;
    CHECK(
      pixelAt(renderBsp(bsp, textures, projection, options).image, 10, 64)
      == Rgba8{200, 200, 200, 255});

    options.shading = BspShading::Lightmap;
    CHECK(
      pixelAt(renderBsp(bsp, textures, projection, options).image, 118, 64)
      == Rgba8{200, 200, 200, 255});
  }

  SECTION("brightness and gamma")
  {
    options.brightness = 2.0;
    const auto bright =
      pixelAt(renderBsp(bsp, textures, projection, options).image, 118, 64);
    CHECK(int(bright[0]) == 255);

    options.brightness = 1.0;
    options.gamma = 2.0;
    const auto gamma =
      pixelAt(renderBsp(bsp, textures, projection, options).image, 118, 64);
    CHECK(int(gamma[0]) > 190);
  }

  SECTION("back faces are culled")
  {
    const auto below =
      orthographicCamera(OrthoView::Top, vm::vec3d{0, 0, 0}, 1.0, vm::bbox3d{256.0});
    auto up = below;
    up.direction = vm::vec3d{0, 0, 1};
    up.position = vm::vec3d{0, 0, -300};
    const auto fromBelow = ImageProjection::create(up, 64, 64).value();
    const auto result = renderBsp(bsp, textures, fromBelow, options);
    CHECK(result.facesDrawn == 0);
    CHECK(result.stats.pixels == 0);
  }

  SECTION("sky")
  {
    auto test = floorBsp();
    test.textures.push_back(solidTexture("sky", Rgba8{0, 0, 255, 255}));
    test.texInfos.push_back(TestTexInfo{{1, 0, 0, 0}, {0, 1, 0, 0}, 1});
    auto sky = floorFace(0, 0, 3, 1);
    sky.vertices = {
      vm::vec3d{-32, -32, 100},
      vm::vec3d{-32, 32, 100},
      vm::vec3d{32, 32, 100},
      vm::vec3d{32, -32, 100},
    };
    sky.distance = 100.0;
    sky.light.clear();
    test.faces.push_back(sky);
    const auto withSky = readBsp(writeBsp(test)).value();
    CHECK(withSky.surface(withSky.faces[1]) == BspSurface::Sky);

    const auto skyTextures = BspTextureImages{textures[0], {}};
    const auto shown = renderBsp(withSky, skyTextures, projection, options);
    CHECK(shown.stats.skyPixels == 64 * 64);
    CHECK(pixelAt(shown.image, 64, 64) == options.skyColor);

    options.hideSky = true;
    const auto hidden = renderBsp(withSky, skyTextures, projection, options);
    CHECK(hidden.stats.skyPixels == 0);
    CHECK(hidden.stats.coverage == Catch::Approx(1.0));
  }

  SECTION("missing textures")
  {
    options.shading = BspShading::Fullbright;
    const auto result = renderBsp(bsp, BspTextureImages{{}}, projection, options);
    const auto pixel = pixelAt(result.image, 4, 4);
    CHECK((pixel == Rgba8{255, 0, 255, 255} || pixel == Rgba8{26, 26, 26, 255}));
  }
}

TEST_CASE("BspTools")
{
  auto fixture = McpToolFixture{};
  auto env = fs::TestEnvironment{};
  env.createDirectory("maps");
  const auto bspPath = env.dir() / "maps" / "floor.bsp";
  env.createFile("maps/floor.bsp", writeBsp(floorBsp()));

  SECTION("bsp_preview")
  {
    SECTION("renders an image with light statistics")
    {
      const auto raw = fixture.callRaw(
        "bsp_preview",
        Json{
          {"path", bspPath.string()},
          {"camera", topView()},
          {"width", 128},
          {"height", 128}});
      REQUIRE(raw["isError"] == false);
      CHECK(contentOf(raw, "image").size() == 1);

      const auto& result = raw["structuredContent"];
      CHECK(result["bsp"]["version"] == 30);
      CHECK(result["bsp"]["game"] == "Half-Life");
      CHECK(result["textures"]["embedded"] == 1);
      CHECK(result["textures"]["missing"].empty());
      CHECK(result["textures"]["wads"][0]["path"].is_null());
      CHECK(result["world"]["luxels"] == 81);

      REQUIRE(result["views"].size() == 1);
      const auto& view = result["views"][0];
      CHECK(view["image"]["width"] == 128);
      CHECK(view["light"]["coverage"] == Catch::Approx(1.0));
      CHECK(region(view, "left")["darkFraction"] == 1.0);
      CHECK(region(view, "right")["darkFraction"] == 0.0);
      auto findings = std::string{};
      for (const auto& finding : view["findings"])
      {
        findings += finding.get<std::string>() + "\n";
      }
      CHECK(findings.find("The left of the image is pitch black.") != std::string::npos);
    }

    SECTION("the player start is the default camera")
    {
      const auto result = fixture.call(
        "bsp_preview", Json{{"path", bspPath.string()}, {"width", 64}, {"height", 48}});
      const auto& view = result["views"][0];
      CHECK(view["label"] == "player start");
      CHECK(view["camera"]["position"] == Json{0, 0, 64});
      CHECK(view["camera"]["direction"][1] == Catch::Approx(1.0));
    }

    SECTION("eyeHeight stands on the BSP's floor")
    {
      const auto result = fixture.call(
        "bsp_preview",
        Json{
          {"path", bspPath.string()},
          {"camera",
           Json{{"eyeHeight", Json{{"point", Json{0, 0, 200}}}}, {"pitch", -60}}},
          {"width", 64},
          {"height", 48}});
      const auto& view = result["views"][0];
      CHECK(view["placement"]["floor"] == 0.0);
      CHECK(view["placement"]["eyeHeight"] == 64.0);
      CHECK(view["camera"]["position"] == Json{0, 0, 64});
    }

    SECTION("several views, saved")
    {
      const auto raw = fixture.callRaw(
        "bsp_preview",
        Json{
          {"path", bspPath.string()},
          {"views",
           Json{
             Json{{"label", "top"}, {"camera", topView()}},
             Json{{"camera", Json{{"orbit", Json{{"target", Json{0, 0, 0}}}}}}},
           }},
          {"width", 64},
          {"height", 64},
          {"shading", "lightmap"},
          {"saveTo", (env.dir() / "preview.png").string()}});
      REQUIRE(raw["isError"] == false);
      CHECK(contentOf(raw, "image").size() == 2);
      CHECK(contentOf(raw, "text").size() >= 2);
      const auto& views = raw["structuredContent"]["views"];
      CHECK(views[0]["label"] == "top");
      CHECK(views[1]["label"] == "view 2");
      CHECK(env.fileExists("preview-1.png"));
      CHECK(env.fileExists("preview-2.png"));

      const auto exists = fixture.callExpectingError(
        "bsp_preview",
        Json{
          {"path", bspPath.string()},
          {"camera", topView()},
          {"saveTo", (env.dir() / "preview-1.png").string()}});
      CHECK(exists.code == ErrorCode::FileExists);
    }

    SECTION("regions")
    {
      const auto result = fixture.call(
        "bsp_preview",
        Json{
          {"path", bspPath.string()},
          {"camera", topView()},
          {"width", 32},
          {"height", 32},
          {"regions",
           Json{Json{
             {"name", "west"},
             {"box", Json{{"min", Json{-100, -100, -10}}, {"max", Json{-10, 100, 10}}}},
           }}}});
      REQUIRE(result["regions"].size() == 1);
      CHECK(result["regions"][0]["name"] == "west");
      CHECK(result["regions"][0]["darkFraction"] == 1.0);

      const auto spaces = fixture.callExpectingError(
        "bsp_preview",
        Json{{"path", bspPath.string()}, {"camera", topView()}, {"regions", "spaces"}});
      CHECK(spaces.code == ErrorCode::NoDocument);
    }

    SECTION("textures from WADs")
    {
      auto test = floorBsp();
      test.textures = {externalTexture("brick")};
      test.entities = R"({
"classname" "worldspawn"
"wad" "C:\sierra\half-life\valve\bricks.wad"
}
)";
      env.createFile("maps/external.bsp", writeBsp(test));
      const auto path = (env.dir() / "maps" / "external.bsp").string();

      const auto missing = fixture.call(
        "bsp_preview",
        Json{{"path", path}, {"camera", topView()}, {"width", 32}, {"height", 32}});
      CHECK(missing["textures"]["missing"] == Json{"brick"});
      CHECK(hasWarning(missing, "TEXTURES_MISSING"));

      const auto brick =
        makeMipTexture("BRICK", makeImage(16, 16, Rgba8{150, 60, 40, 255}), false);
      env.createFile("bricks.wad", writeWad3({brick.texture}));
      const auto found = fixture.call(
        "bsp_preview",
        Json{
          {"path", path},
          {"camera", topView()},
          {"width", 32},
          {"height", 32},
          {"shading", "fullbright"}});
      CHECK(found["textures"]["missing"].empty());
      CHECK(found["textures"]["fromWads"] == 1);
      CHECK(found["textures"]["wads"][0]["texturesUsed"] == 1);
      CHECK(
        found["views"][0]["light"]["meanBrightness"] == Catch::Approx(84.6).margin(1));
    }

    SECTION("Quake BSPs")
    {
      auto test = floorBsp();
      test.version = QuakeBspVersion;
      auto texture =
        makeMipTexture("floor", makeImage(16, 16, Rgba8{1, 1, 1, 255}), false);
      texture.texture.palette.clear();
      // a Quake texture has no palette: the mip header and the levels only
      auto lump = writeMipTexture(MipTexture{
        "floor", 16, 16, texture.texture.mips, std::vector<unsigned char>(768)});
      lump.resize(40 + 16 * 16 * 85 / 64);
      test.textures = {lump};
      test.faces = {floorFace(0, 100, 1)};
      env.createFile("maps/quake.bsp", writeBsp(test));
      const auto path = (env.dir() / "maps" / "quake.bsp").string();

      const auto gray = fixture.call(
        "bsp_preview",
        Json{{"path", path}, {"camera", topView()}, {"width", 32}, {"height", 32}});
      CHECK(gray["bsp"]["game"] == "Quake");
      CHECK(hasWarning(gray, "PALETTE_NOT_FOUND"));

      auto palette = std::string(768, '\0');
      palette[texture.texture.mips[0][0] * 3] = char(100);
      env.createDirectory("gfx");
      env.createFile("gfx/palette.lmp", palette);
      const auto colored = fixture.call(
        "bsp_preview",
        Json{{"path", path}, {"camera", topView()}, {"width", 32}, {"height", 32}});
      CHECK_FALSE(hasWarning(colored, "PALETTE_NOT_FOUND"));
      CHECK(colored["textures"]["palette"].is_string());
      // Quake's lightmaps are overbright: light 100 doubles the texture colour
      CHECK(region(colored["views"][0], "right")["meanLight"] == Catch::Approx(100));
    }

    SECTION("no light data")
    {
      auto test = floorBsp();
      test.faces[0].light.clear();
      env.createFile("maps/unlit.bsp", writeBsp(test));
      const auto result = fixture.call(
        "bsp_preview",
        Json{
          {"path", (env.dir() / "maps" / "unlit.bsp").string()},
          {"camera", topView()},
          {"width", 32},
          {"height", 32}});
      CHECK(hasWarning(result, "NO_LIGHT_DATA"));
      CHECK(result["views"][0]["light"]["meanLight"] == Catch::Approx(255));
    }

    SECTION("errors")
    {
      const auto noPath = fixture.callExpectingError("bsp_preview");
      CHECK(noPath.code == ErrorCode::InvalidArgument);

      env.createFile("maps/text.bsp", "hello");
      const auto notBsp = fixture.callExpectingError(
        "bsp_preview", Json{{"path", (env.dir() / "maps" / "text.bsp").string()}});
      CHECK(notBsp.code == ErrorCode::Unsupported);

      const auto relative =
        fixture.callExpectingError("bsp_preview", Json{{"path", "maps/floor.bsp"}});
      CHECK(relative.code == ErrorCode::InvalidArgument);

      const auto unknownCamera = fixture.callExpectingError(
        "bsp_preview", Json{{"path", bspPath.string()}, {"camera", "nope"}});
      CHECK(unknownCamera.code == ErrorCode::InvalidArgument);

      const auto both = fixture.callExpectingError(
        "bsp_preview",
        Json{
          {"path", bspPath.string()},
          {"camera", topView()},
          {"views", Json{Json{{"camera", topView()}}}}});
      CHECK(both.code == ErrorCode::InvalidArgument);
    }

    SECTION("an agent camera by name")
    {
      fixture.call(
        "agent_camera_set",
        Json{
          {"name", "above"},
          {"camera", Json{{"position", Json{0, 0, 300}}, {"lookAt", Json{0, 0, 0}}}}});
      const auto result = fixture.call(
        "bsp_preview",
        Json{
          {"path", bspPath.string()},
          {"camera", "above"},
          {"width", 64},
          {"height", 64}});
      CHECK(result["views"][0]["cameraName"] == "above");
      CHECK(result["views"][0]["light"]["coverage"].get<double>() > 0.0);
    }
  }
}

} // namespace tb::mcp
