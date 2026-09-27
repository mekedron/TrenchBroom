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

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{

TEST_CASE("Image")
{
  const auto red = Rgba8{255, 0, 0, 255};
  const auto blue = Rgba8{0, 0, 255, 255};
  const auto white = Rgba8{255, 255, 255, 255};

  SECTION("makeImage and pixelAt")
  {
    const auto image = makeImage(3, 2, red);
    CHECK(image.width == 3);
    CHECK(image.height == 2);
    CHECK(image.pixels.size() == 24);
    CHECK(pixelAt(image, 2, 1) == red);
  }

  SECTION("encodePng")
  {
    const auto png = encodePng(makeImage(4, 4, blue));
    REQUIRE(png.has_value());
    CHECK(png->substr(1, 3) == "PNG");
    CHECK(!encodePng(RgbaImage{}).has_value());
  }

  SECTION("downscale")
  {
    SECTION("small images are unchanged")
    {
      const auto image = makeImage(8, 4, red);
      CHECK(downscale(image, 8) == image);
    }

    SECTION("keeps the aspect ratio and averages pixels")
    {
      auto image = makeImage(4, 2, red);
      // the right half is blue
      for (size_t y = 0; y < 2; ++y)
      {
        for (size_t x = 2; x < 4; ++x)
        {
          auto* p = image.pixels.data() + (y * 4 + x) * 4;
          std::copy(blue.begin(), blue.end(), p);
        }
      }
      const auto scaled = downscale(image, 2);
      CHECK(scaled.width == 2);
      CHECK(scaled.height == 1);
      CHECK(pixelAt(scaled, 0, 0) == red);
      CHECK(pixelAt(scaled, 1, 0) == blue);

      const auto averaged = downscale(image, 1);
      CHECK(averaged.width == 1);
      CHECK(averaged.height == 1);
      CHECK(pixelAt(averaged, 0, 0) == Rgba8{128, 0, 128, 255});
    }
  }

  SECTION("composeSideBySide")
  {
    const auto composed =
      composeSideBySide({makeImage(2, 3, red), makeImage(3, 1, blue)}, 2, white);
    CHECK(composed.width == 7);
    CHECK(composed.height == 3);
    CHECK(pixelAt(composed, 0, 2) == red);
    CHECK(pixelAt(composed, 1, 0) == red);
    CHECK(pixelAt(composed, 2, 0) == white);
    CHECK(pixelAt(composed, 3, 0) == white);
    CHECK(pixelAt(composed, 4, 0) == blue);
    CHECK(pixelAt(composed, 6, 0) == blue);
    // below the lower image
    CHECK(pixelAt(composed, 4, 1) == white);

    CHECK(composeSideBySide({}, 4, white).width == 0);
  }

  SECTION("diffImages")
  {
    const auto before = makeImage(10, 8, Rgba8{100, 100, 100, 255});

    SECTION("identical images")
    {
      const auto diff = diffImages(before, before);
      REQUIRE(diff.has_value());
      CHECK(diff->changedPixels == 0);
      CHECK(diff->changedRatio == 0.0);
      CHECK(!diff->changedBounds.has_value());
      CHECK(pixelAt(diff->mask, 0, 0) == Rgba8{25, 25, 25, 255});
    }

    SECTION("changed pixels, bounds and mask")
    {
      auto after = before;
      for (const auto [x, y] : {std::pair{2, 3}, std::pair{5, 6}})
      {
        auto* p = after.pixels.data() + (size_t(y) * 10 + size_t(x)) * 4;
        p[0] = 200;
      }
      // below the threshold
      after.pixels[(0 * 10 + 9) * 4 + 1] = 104;

      const auto diff = diffImages(before, after, 8);
      REQUIRE(diff.has_value());
      CHECK(diff->changedPixels == 2);
      CHECK(diff->changedRatio == 2.0 / 80.0);
      CHECK(diff->changedBounds == PixelRect{2, 3, 4, 4});
      CHECK(pixelAt(diff->mask, 2, 3) == DiffMaskColor);
      CHECK(pixelAt(diff->mask, 5, 6) == DiffMaskColor);
      CHECK(pixelAt(diff->mask, 9, 0) != DiffMaskColor);

      // without a threshold every difference counts
      CHECK(diffImages(before, after)->changedPixels == 3);
    }

    SECTION("different sizes")
    {
      CHECK(!diffImages(before, makeImage(10, 9, red)).has_value());
    }
  }
}

} // namespace tb::mcp
