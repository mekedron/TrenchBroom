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

#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QSurfaceFormat>

#include "gl/GlManager.h"
#include "mcp/Json.h"
#include "mcp/McpToolFixture.h"
#include "mcp/Pagination.h"
#include "ui/McpSnapshotRenderer.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

/** Whether this process can create an OpenGL 2.1 context, e.g. not under offscreen. */
bool glAvailable()
{
  auto format = QSurfaceFormat{};
  format.setRenderableType(QSurfaceFormat::OpenGL);
  format.setVersion(2, 1);
  format.setProfile(QSurfaceFormat::CompatibilityProfile);

  auto surface = QOffscreenSurface{};
  surface.setFormat(format);
  surface.create();

  auto context = QOpenGLContext{};
  context.setFormat(format);
  return surface.isValid() && context.create() && !context.isOpenGLES()
         && context.makeCurrent(&surface) && (context.doneCurrent(), true);
}

gl::GlManager createGlManager()
{
  return gl::GlManager{[](const std::filesystem::path& path) {
    return std::filesystem::path{MCP_TEST_RESOURCE_DIR} / path;
  }};
}

/** The image of a call result, decoded. */
QImage imageOf(const mcp::Json& raw)
{
  REQUIRE(raw.value("isError", false) == false);
  const auto& content = raw["content"];
  const auto it = std::ranges::find_if(
    content, [](const auto& block) { return block["type"] == "image"; });
  REQUIRE(it != content.end());
  const auto bytes = mcp::base64Decode((*it)["data"].get<std::string>());
  REQUIRE(bytes);
  auto image = QImage::fromData(
    reinterpret_cast<const uchar*>(bytes->data()), int(bytes->size()), "PNG");
  REQUIRE(!image.isNull());
  return image.convertToFormat(QImage::Format_RGBA8888);
}

size_t countDifferentPixels(const QImage& lhs, const QImage& rhs)
{
  REQUIRE(lhs.size() == rhs.size());
  auto count = size_t(0);
  for (int y = 0; y < lhs.height(); ++y)
  {
    for (int x = 0; x < lhs.width(); ++x)
    {
      if (lhs.pixel(x, y) != rhs.pixel(x, y))
      {
        ++count;
      }
    }
  }
  return count;
}

/** Saves the image if MCP_SNAPSHOT_TEST_DIR is set, to inspect it manually. */
void saveForInspection(const QImage& image, const std::string& name)
{
  if (const auto* dir = std::getenv("MCP_SNAPSHOT_TEST_DIR"))
  {
    image.save(
      QString::fromStdString((std::filesystem::path{dir} / (name + ".png")).string()));
  }
}

} // namespace

TEST_CASE("Annotations with McpSnapshotRenderer", "[gpu]")
{
  if (!glAvailable())
  {
    SKIP("No OpenGL context available on this platform");
  }

  auto glManager = createGlManager();
  auto renderer = McpSnapshotRenderer{glManager};

  auto fixture = mcp::McpToolFixture{};
  fixture.host().snapshotRendererOverride = &renderer;
  fixture.call(
    "document_open",
    mcp::Json{
      {"path", (std::filesystem::path{MCP_TEST_MAPS_DIR} / "two_rooms.map").string()},
      {"game", "Quake"}});

  SECTION("Annotations are drawn onto the rendered image")
  {
    const auto camera = mcp::Json{
      {"position", mcp::Json::array({256, 40, 200})},
      {"lookAt", mcp::Json::array({256, 400, 0})}};
    const auto snapshot = [&](const mcp::Json& annotations) {
      auto args = mcp::Json{{"camera", camera}, {"width", 640}, {"height", 480}};
      if (!annotations.is_null())
      {
        args["annotations"] = annotations;
      }
      return fixture.callRaw("view_snapshot", args);
    };

    const auto plain = imageOf(snapshot(nullptr));
    const auto raw = snapshot(mcp::Json{
      {"labels", true},
      {"grid", true},
      {"compass", true},
      {"player", mcp::Json{{"point", mcp::Json::array({128, 300, 100})}}}});
    const auto annotated = imageOf(raw);
    saveForInspection(plain, "annotations_plain");
    saveForInspection(annotated, "annotations");

    const auto& report = raw["structuredContent"]["annotations"];
    CHECK(report["labels"].get<size_t>() > 0);
    CHECK(report["grid"]["lines"].get<size_t>() > 0);
    CHECK(report["compass"] == true);
    CHECK(report["player"]["visible"] == true);

    CHECK(countDifferentPixels(plain, annotated) > 1000);
    // the compass in the top right corner (text scale 2 at this size)
    CHECK(plain.pixel(640 - 44, 44) != annotated.pixel(640 - 44, 44));
    // a compass alone changes the image, too
    CHECK(imageOf(snapshot(mcp::Json{{"compass", true}})) != plain);
  }

  SECTION("view_pick finds the objects the renderer drew")
  {
    // looking east at the west face of the first pillar
    const auto camera = mcp::Json{
      {"position", mcp::Json::array({100, 208, 128})},
      {"direction", mcp::Json::array({1, 0, 0})}};
    const auto shot = fixture.callRaw(
      "view_snapshot", mcp::Json{{"camera", camera}, {"width", 320}, {"height", 240}});
    const auto plain = imageOf(shot);
    const auto snapshotId = shot["structuredContent"]["snapshotId"];

    for (const auto& [x, y] :
         {std::pair{160, 120}, std::pair{20, 220}, std::pair{20, 15}})
    {
      CAPTURE(x, y);
      const auto pick = fixture.call(
        "view_pick",
        mcp::Json{{"snapshot", snapshotId}, {"pixel", mcp::Json{{"x", x}, {"y", y}}}});
      REQUIRE(pick["hit"].is_object());

      // tinting the picked object changes the picked pixel
      const auto highlighted = imageOf(fixture.callRaw(
        "view_snapshot",
        mcp::Json{
          {"camera", camera},
          {"width", 320},
          {"height", 240},
          {"highlight",
           mcp::Json{
             {"ids", mcp::Json::array({pick["hit"]["object"]})},
             {"color", mcp::Json::array({1, 0, 0})}}}}));
      CHECK(highlighted.pixel(x, y) != plain.pixel(x, y));
    }
  }
}

} // namespace tb::ui
