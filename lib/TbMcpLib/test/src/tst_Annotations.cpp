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
#include "img/DecodeImage.h"
#include "mcp/AgentCamera.h"
#include "mcp/Annotations.h"
#include "mcp/CameraProjection.h"
#include "mcp/Image.h"
#include "mcp/JsonVm.h"
#include "mcp/McpToolFixture.h"
#include "mcp/Pagination.h"
#include "mdl/GameManager.h"
#include "mdl/GroupNode.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include "kd/result.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

constexpr auto Background = Rgba8{20, 20, 20, 255};
constexpr auto White = Rgba8{255, 255, 255, 255};

size_t countDifferent(const RgbaImage& image, const Rgba8& color)
{
  auto count = size_t(0);
  for (size_t y = 0; y < image.height; ++y)
  {
    for (size_t x = 0; x < image.width; ++x)
    {
      if (pixelAt(image, x, y) != color)
      {
        ++count;
      }
    }
  }
  return count;
}

bool changed(const RgbaImage& image, const long x, const long y)
{
  return pixelAt(image, size_t(x), size_t(y)) != Background;
}

/** Whether (x, y) or a pixel left of or above it changed: for lines on pixel borders. */
bool changedAt(const RgbaImage& image, const long x, const long y)
{
  return changed(image, x, y) || changed(image, x - 1, y) || changed(image, x, y - 1)
         || changed(image, x - 1, y - 1);
}

/** Whether a pixel within `radius` of (x, y) differs between the images. */
bool changedNear(
  const RgbaImage& before,
  const RgbaImage& after,
  const double x,
  const double y,
  const long radius)
{
  for (auto py = long(y) - radius; py <= long(y) + radius; ++py)
  {
    for (auto px = long(x) - radius; px <= long(x) + radius; ++px)
    {
      if (
        px >= 0 && py >= 0 && px < long(after.width) && py < long(after.height)
        && pixelAt(before, size_t(px), size_t(py))
             != pixelAt(after, size_t(px), size_t(py)))
      {
        return true;
      }
    }
  }
  return false;
}

/** A top view of 200x200 pixels centered on the origin: world (x, y) is (100 + x, 100 -
 * y). */
ImageProjection topView()
{
  return ImageProjection::create(
           orthographicCamera(
             OrthoView::Top, vm::vec3d{0, 0, 0}, 1.0, vm::bbox3d{4096.0}),
           200,
           200)
         | kdl::value();
}

bool isRed(const Rgba8& color)
{
  return color[0] > 180 && color[1] < 120 && color[2] < 120;
}

/** Whether red compass pixels exist in the given part of the compass. */
bool redIn(
  const RgbaImage& image, const long x0, const long y0, const long x1, const long y1)
{
  for (auto y = y0; y < y1; ++y)
  {
    for (auto x = x0; x < x1; ++x)
    {
      if (isRed(pixelAt(image, size_t(x), size_t(y))))
      {
        return true;
      }
    }
  }
  return false;
}

ui::MapDocument& loadRooms(McpToolFixture& fixture)
{
  const auto* gameInfo = fixture.host().gameManager().gameInfo("Quake");
  REQUIRE(gameInfo);
  return fixture.load(
    getFixtureRoot() / "test" / "mcp" / "maps" / "two_rooms.map",
    {.mapFormat = mdl::MapFormat::Standard, .gameInfo = *gameInfo});
}

RgbaImage decodedImage(const Json& raw)
{
  const auto it = std::ranges::find_if(
    raw["content"], [](const auto& block) { return block["type"] == "image"; });
  REQUIRE(it != raw["content"].end());
  const auto bytes = base64Decode((*it)["data"].get<std::string>());
  REQUIRE(bytes);
  const auto image =
    img::decodeImage(reinterpret_cast<const unsigned char*>(bytes->data()), bytes->size())
    | kdl::value();
  return RgbaImage{image.width, image.height, image.pixels};
}

} // namespace

TEST_CASE("Annotations")
{
  SECTION("blendPixel, drawLine, drawRect, fillRect, fillPolygon, drawCircle")
  {
    auto image = makeImage(20, 20, Background);
    blendPixel(image, 1, 1, White);
    CHECK(pixelAt(image, 1, 1) == White);
    blendPixel(image, 2, 1, Rgba8{255, 255, 255, 0});
    CHECK(pixelAt(image, 2, 1) == Background);
    blendPixel(image, 3, 1, Rgba8{220, 220, 220, 128});
    CHECK(pixelAt(image, 3, 1)[0] == 120);
    // outside: ignored
    blendPixel(image, -1, 0, White);
    blendPixel(image, 20, 0, White);

    image = makeImage(20, 20, Background);
    drawLine(image, vm::vec2d{0.5, 5.5}, vm::vec2d{9.5, 5.5}, White);
    for (size_t x = 0; x < 10; ++x)
    {
      CHECK(pixelAt(image, x, 5) == White);
    }
    CHECK(countDifferent(image, Background) == 10);

    // clipped to the image
    image = makeImage(20, 20, Background);
    drawLine(image, vm::vec2d{-1e7, 3.5}, vm::vec2d{1e7, 3.5}, White);
    CHECK(countDifferent(image, Background) == 20);
    drawLine(image, vm::vec2d{-50, -50}, vm::vec2d{-10, -40}, White);
    CHECK(countDifferent(image, Background) == 20);

    // thick lines
    image = makeImage(20, 20, Background);
    drawLine(image, vm::vec2d{2.5, 10.5}, vm::vec2d{12.5, 10.5}, White, 3);
    CHECK(changed(image, 5, 9));
    CHECK(changed(image, 5, 11));
    CHECK(!changed(image, 5, 13));

    image = makeImage(20, 20, Background);
    drawRect(image, 2, 2, 5, 4, White);
    CHECK(changed(image, 2, 2));
    CHECK(changed(image, 6, 5));
    CHECK(!changed(image, 4, 3));
    CHECK(countDifferent(image, Background) == 14);

    image = makeImage(20, 20, Background);
    fillRect(image, 15, 15, 10, 10, White);
    CHECK(countDifferent(image, Background) == 25);

    image = makeImage(20, 20, Background);
    fillPolygon(image, {vm::vec2d{0, 0}, vm::vec2d{10, 0}, vm::vec2d{0, 10}}, White);
    CHECK(changed(image, 1, 1));
    CHECK(!changed(image, 8, 8));
    const auto area = countDifferent(image, Background);
    CHECK(area >= 40);
    CHECK(area <= 60);

    image = makeImage(40, 40, Background);
    drawCircle(image, vm::vec2d{20, 20}, 10.0, White);
    CHECK(changed(image, 30, 20));
    CHECK(changed(image, 10, 20));
    CHECK(!changed(image, 20, 20));
  }

  SECTION("Font")
  {
    for (const auto c : std::string{"0123456789abcdefghijklmnopqrstuvwxyz"
                                    "ABCDEFGHIJKLMNOPQRSTUVWXYZ :_-./#()+=?*,"})
    {
      CAPTURE(c);
      CHECK(hasGlyph(c));
    }
    CHECK(!hasGlyph('~'));
    CHECK(textWidth("ab", 1) == 11);
    CHECK(textWidth("ab", 2) == 22);
    CHECK(textWidth("", 2) == 0);
    CHECK(textHeight(2) == 14);

    auto image = makeImage(12, 16, Background);
    drawText(image, 0, 0, "1", White);
    CHECK(countDifferent(image, Background) == 10);
    image = makeImage(12, 16, Background);
    drawText(image, 0, 0, "1", White, 2);
    CHECK(countDifferent(image, Background) == 40);

    // every glyph differs from the others
    const auto chars = std::string{
      "0123456789abcdefghijklmnopqrstuvwxyz"
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ:_-./#()+=?*,"};
    auto images = std::vector<RgbaImage>{};
    for (const auto c : chars)
    {
      auto glyph = makeImage(5, 7, Background);
      drawText(glyph, 0, 0, std::string(1, c), White);
      CHECK(countDifferent(glyph, Background) > 0);
      images.push_back(std::move(glyph));
    }
    for (size_t i = 0; i < images.size(); ++i)
    {
      for (size_t j = i + 1; j < images.size(); ++j)
      {
        CAPTURE(chars[i], chars[j]);
        CHECK(images[i] != images[j]);
      }
    }

    // unknown characters are drawn as '?'
    auto unknown = makeImage(5, 7, Background);
    drawText(unknown, 0, 0, "~", White);
    CHECK(unknown == images[chars.find('?')]);
  }

  SECTION("labelBox, drawLabel")
  {
    const auto box = labelBox(50, 50, {"ab"}, 1);
    CHECK(box.width == 15);
    CHECK(box.height == 11);
    CHECK(box.x == 43);
    CHECK(box.y == 45);
    CHECK(box.inside(100, 100));
    CHECK(!box.inside(50, 50));
    CHECK(box.intersects(PixelBox{50, 50, 10, 10}));
    CHECK(!box.intersects(PixelBox{58, 50, 10, 10}));

    const auto twoLines = labelBox(50, 50, {"ab", "abc"}, 2);
    CHECK(twoLines.width == 17 * 2 + 8);
    CHECK(twoLines.height == 2 * 14 + 4 + 8);

    auto image = makeImage(100, 100, Background);
    drawLabel(image, box, {"ab"}, White, Rgba8{0, 0, 0, 255}, 1);
    CHECK(pixelAt(image, 43, 45) == Rgba8({0, 0, 0, 255}));
    CHECK(pixelAt(image, 42, 45) == Background);
  }

  SECTION("drawLine3d")
  {
    const auto projection =
      ImageProjection::create(
        perspectiveCamera(vm::vec3d{0, 0, 0}, vm::vec3d{1, 0, 0}), 100, 100)
      | kdl::value();
    auto image = makeImage(100, 100, Background);
    CHECK(
      !drawLine3d(image, projection, vm::vec3d{-10, 0, 0}, vm::vec3d{-20, 5, 0}, White));
    CHECK(countDifferent(image, Background) == 0);
    CHECK(
      drawLine3d(image, projection, vm::vec3d{-10, 0, -5}, vm::vec3d{50, 0, -5}, White));
    CHECK(countDifferent(image, Background) > 0);
  }

  SECTION("drawAnnotations")
  {
    const auto projection = topView();

    SECTION("Labels")
    {
      auto image = makeImage(200, 200, Background);
      auto spec = AnnotationSpec{};
      for (int i = 0; i < 6; ++i)
      {
        spec.labels.push_back(LabelAnnotation{
          "brush:" + std::to_string(i), {"brush:1", "32x32x32"}, vm::vec3d{0, 0, 0}});
      }
      spec.labels.push_back(LabelAnnotation{"brush:9", {"far"}, vm::vec3d{1000, 0, 0}});
      const auto report = drawAnnotations(image, projection, spec);
      // five positions around the anchor fit; the sixth overlaps; one is outside
      CHECK(report.labelled.size() == 5);
      CHECK(report.labelsSkipped == 2);
      CHECK(changed(image, 100, 100));
      CHECK(!changed(image, 5, 5));
    }

    SECTION("Grid")
    {
      auto image = makeImage(200, 200, Background);
      auto spec = AnnotationSpec{};
      spec.grid = GridAnnotation{};
      spec.grid->step = 32.0;
      spec.grid->box = vm::bbox3d{vm::vec3d{-64, -64, -10}, vm::vec3d{64, 64, 10}};
      const auto report = drawAnnotations(image, projection, spec);
      // 5 lines along each axis; the walls are not seen from the top
      CHECK(report.gridLines == 10);
      CHECK(report.gridLabels > 0);
      CHECK(changedAt(image, 132, 90));
      CHECK(changedAt(image, 110, 68));
      CHECK(!changed(image, 116, 84));
      CHECK(!changed(image, 180, 20));

      // invisible everywhere
      image = makeImage(200, 200, Background);
      spec.grid->visible = [](const vm::vec3d&) { return false; };
      CHECK(drawAnnotations(image, projection, spec).gridLines == 0);
      CHECK(countDifferent(image, Background) == 0);

      // walls seen from inside
      const auto inside =
        ImageProjection::create(
          perspectiveCamera(vm::vec3d{0, 0, 0}, vm::vec3d{1, 0, 0}), 200, 200)
        | kdl::value();
      image = makeImage(200, 200, Background);
      spec.grid->visible = nullptr;
      spec.grid->floor = false;
      spec.grid->box = vm::bbox3d{vm::vec3d{-128, -128, -64}, vm::vec3d{128, 128, 64}};
      CHECK(drawAnnotations(image, inside, spec).gridLines > 0);
      // the far wall at x = 128: a vertical line at y = 0 through the image center
      CHECK(changedAt(image, 100, 70));
    }

    SECTION("Compass")
    {
      auto image = makeImage(200, 200, Background);
      auto spec = AnnotationSpec{};
      spec.compass = true;
      CHECK(drawAnnotations(image, projection, spec).compass);
      // the compass center (scale 1: radius 18, margin 4) is at (178, 22); north is up
      CHECK(changed(image, 178, 22));
      CHECK(redIn(image, 170, 4, 186, 18));
      CHECK(!redIn(image, 170, 27, 186, 40));
      CHECK(!changed(image, 100, 100));

      // looking east, north is on the left
      const auto east =
        ImageProjection::create(
          perspectiveCamera(vm::vec3d{0, 0, 0}, vm::vec3d{1, 0, 0}), 200, 200)
        | kdl::value();
      image = makeImage(200, 200, Background);
      drawAnnotations(image, east, spec);
      CHECK(redIn(image, 160, 14, 174, 30));
      CHECK(!redIn(image, 182, 14, 196, 30));
    }

    SECTION("Player")
    {
      auto image = makeImage(200, 200, Background);
      auto spec = AnnotationSpec{};
      spec.player = PlayerAnnotation{vm::vec3d{0, 0, 0}, 32.0, 56.0, 46.0};
      const auto report = drawAnnotations(image, projection, spec);
      CHECK(report.player);
      // the box covers (84, 84) to (116, 116)
      CHECK(changed(image, 100, 100));
      CHECK(changedAt(image, 84, 100));
      CHECK(!changed(image, 60, 100));
      CHECK(pixelAt(image, 84, 100)[1] > 200);
    }

    SECTION("annotationScale")
    {
      CHECK(annotationScale(320, 240) == 1);
      CHECK(annotationScale(640, 480) == 2);
      CHECK(annotationScale(1024, 768) == 2);
      CHECK(annotationScale(2048, 2048) == 3);
    }
  }

  SECTION("view_snapshot annotations")
  {
    auto fixture = McpToolFixture{};
    auto& document = loadRooms(fixture);
    auto& map = document.map();
    auto* group = [&]() -> mdl::Node* {
      for (auto* layer : map.worldNode().children())
      {
        for (auto* child : layer->children())
        {
          if (dynamic_cast<mdl::GroupNode*>(child))
          {
            return child;
          }
        }
      }
      return nullptr;
    }();
    REQUIRE(group);

    const auto camera =
      Json{{"position", {256, 40, 200}}, {"lookAt", {256, 400, 0}}, {"fov", 90}};
    const auto snapshot = [&](const Json& annotations) {
      auto args = Json{{"camera", camera}, {"width", 320}, {"height", 240}};
      if (!annotations.is_null())
      {
        args["annotations"] = annotations;
      }
      const auto raw = fixture.callRaw("view_snapshot", args);
      REQUIRE(raw.value("isError", false) == false);
      return raw;
    };
    const auto plainRaw = snapshot(nullptr);
    const auto plain = decodedImage(plainRaw);
    CHECK(!plainRaw["structuredContent"].contains("annotations"));
    const auto projection =
      ImageProjection::create(
        lookAtCamera(vm::vec3d{256, 40, 200}, vm::vec3d{256, 400, 0}), 320, 240)
      | kdl::value();

    SECTION("Labels")
    {
      const auto raw = snapshot(Json{{"labels", true}});
      const auto& report = raw["structuredContent"]["annotations"];
      CHECK(report["labels"].get<size_t>() > 0);
      CHECK(report["labels"] == report["labelled"].size());
      const auto groupId = fixture.id(*group);
      REQUIRE(
        std::ranges::find(report["labelled"], Json(groupId)) != report["labelled"].end());
      // the group label is at its bounds center, which is visible from the camera
      const auto anchor = projection.project(group->logicalBounds().center());
      CHECK(changedNear(plain, decodedImage(raw), anchor.x, anchor.y, 2));

      // explicit ids and a maximum
      const auto some = snapshot(Json{{"labels", Json{{"ids", {groupId}}}}});
      CHECK(
        some["structuredContent"]["annotations"]["labelled"] == Json::array({groupId}));
      const auto one = snapshot(Json{{"labels", Json{{"max", 1}}}});
      CHECK(one["structuredContent"]["annotations"]["labels"] == 1);

      const auto error = fixture.callExpectingError(
        "view_snapshot",
        Json{
          {"camera", camera},
          {"annotations", Json{{"labels", Json{{"ids", {"brush:99999"}}}}}}});
      CHECK(error.code == ErrorCode::ObjectNotFound);
    }

    SECTION("Grid")
    {
      const auto raw = snapshot(Json{{"grid", Json{{"step", 64}}}});
      const auto& grid = raw["structuredContent"]["annotations"]["grid"];
      // the space around the image center: the first room
      CHECK(
        grid["box"] == toJson(vm::bbox3d{vm::vec3d{0, 0, 0}, vm::vec3d{512, 512, 256}}));
      CHECK(grid["step"] == 64);
      CHECK(grid["lines"].get<size_t>() > 0);
      const auto image = decodedImage(raw);
      // floor lines in the open
      const auto onLine = projection.project(vm::vec3d{256, 448, 0});
      CHECK(changedNear(plain, image, onLine.x, onLine.y, 1));

      // too many lines: a coarser step and a warning
      const auto coarse = snapshot(Json{{"grid", Json{{"step", 1}}}});
      CHECK(coarse["structuredContent"]["annotations"]["grid"]["step"].get<double>() > 1);
      CHECK(std::ranges::any_of(
        coarse["structuredContent"]["warnings"],
        [](const auto& warning) { return warning["code"] == "GRID_STEP_INCREASED"; }));

      const auto boxed = snapshot(Json{
        {"grid",
         Json{
           {"planes", "floor"},
           {"box", Json{{"min", {0, 0, 0}}, {"max", {256, 256, 64}}}}}}});
      const auto& boxedGrid = boxed["structuredContent"]["annotations"]["grid"];
      CHECK(boxedGrid["walls"] == false);
      CHECK(boxedGrid["step"] == 64);
    }

    SECTION("Compass and player")
    {
      const auto raw =
        snapshot(Json{{"compass", true}, {"player", Json{{"point", {128, 300, 100}}}}});
      const auto& report = raw["structuredContent"]["annotations"];
      CHECK(report["compass"] == true);
      CHECK(report["player"]["feet"] == Json::array({128, 300, 0}));
      CHECK(report["player"]["height"] == 56);
      CHECK(report["player"]["eyeHeight"] == 46);
      CHECK(report["player"]["visible"] == true);
      const auto image = decodedImage(raw);
      const auto body = projection.project(vm::vec3d{128, 300, 28});
      CHECK(changedNear(plain, image, body.x, body.y, 0));
      CHECK(changedNear(plain, image, 320 - 22, 22, 0));

      const auto floating =
        snapshot(Json{{"player", Json{{"point", {128, 300, 100}}, {"onFloor", false}}}});
      CHECK(
        floating["structuredContent"]["annotations"]["player"]["feet"]
        == Json::array({128, 300, 100}));
    }

    SECTION("Kept snapshots have no annotations")
    {
      fixture.call(
        "view_snapshot",
        Json{
          {"camera", camera},
          {"width", 320},
          {"height", 240},
          {"keepAs", "plain"},
          {"annotations", Json{{"compass", true}}}});
      const auto compare = fixture.call(
        "view_snapshot_compare", Json{{"before", "plain"}, {"threshold", 0}});
      CHECK(compare["changedPixels"] == 0);
    }

    SECTION("view_snapshots_around")
    {
      const auto result = fixture.call(
        "view_snapshots_around",
        Json{
          {"ids", {fixture.id(*group)}},
          {"views", {"north", "top"}},
          {"width", 160},
          {"height", 120},
          {"annotations", Json{{"compass", true}}}});
      REQUIRE(result["images"].size() == 2);
      CHECK(result["images"][0]["annotations"]["compass"] == true);
      CHECK(result["images"][1]["annotations"]["compass"] == true);
    }
  }
}

} // namespace tb::mcp
