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
#include "base/Logger.h"
#include "fs/DiskFileSystem.h"
#include "fs/TestEnvironment.h"
#include "mdl/EntityModel.h"
#include "mdl/LoadAssimpModel.h"

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

std::vector<std::string> frameNames(const mdl::EntityModelData& data)
{
  auto result = std::vector<std::string>{};
  for (const auto& frame : data.frames())
  {
    result.push_back(frame.name());
  }
  return result;
}

} // namespace

// Tests the change to lib/TbMdlLib/src/LoadAssimpModel.cpp: each frame is named after its
// animation.
TEST_CASE("UpstreamLoadAssimpModel")
{
  auto logger = NullLogger{};

  SECTION("frames are named after the animations of the model")
  {
    // a copy of the Half-Life studio model of TbMdlLib's LoadAssimpModel fixture
    const auto fs = fs::DiskFileSystem{
      getFixtureRoot() / "test" / "mdl" / "Game" / "Quake" / "id1" / "models"};
    const auto data = mdl::loadAssimpModel("cube.mdl", fs, logger);
    REQUIRE(data.is_success());
    CHECK(frameNames(data.value()) == std::vector<std::string>{"idle", "slide", "spin"});
  }

  SECTION("a model without animations names its frame after the model path")
  {
    auto env = fs::TestEnvironment{[](auto& environment) {
      environment.createFile(
        "triangle.obj",
        "v 0 0 0\n"
        "v 16 0 0\n"
        "v 0 16 8\n"
        "f 1 2 3\n");
    }};
    const auto fs = fs::DiskFileSystem{env.dir()};
    const auto data = mdl::loadAssimpModel("triangle.obj", fs, logger);
    REQUIRE(data.is_success());
    CHECK(frameNames(data.value()) == std::vector<std::string>{"triangle.obj"});
  }
}

} // namespace tb::mcp
