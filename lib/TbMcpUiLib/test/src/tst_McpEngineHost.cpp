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

#include <QtTest/QTest>

#include "CmdTool.h"
#include "fs/TestEnvironment.h"
#include "mdl/GameEngineProfile.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "ui/CatchConfig.h"
#include "ui/MapDocument.h"
#include "ui/MapDocumentFixture.h"
#include "ui/McpEngineHost.h"

#include "kd/result.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

std::string readFile(const std::filesystem::path& path)
{
  auto stream = std::ifstream{path};
  auto buffer = std::stringstream{};
  buffer << stream.rdbuf();
  return buffer.str();
}

/** Waits until the file has the expected contents; CmdTool runs detached. */
bool waitForFile(const std::filesystem::path& path, const std::string& expected)
{
  return QTest::qWaitFor([&]() { return readFile(path) == expected; }, 10000);
}

} // namespace

TEST_CASE("McpEngineHost")
{
  auto env = fs::TestEnvironment{};
  const auto logFile = env.dir() / "engine.log";

  auto fixture = MapDocumentFixture{};
  auto& document = fixture.create();
  REQUIRE(document.map().saveAs(env.dir() / "mymap.map"));

  auto host = McpEngineHost{logFile};
  const auto profile = mdl::GameEngineProfile{
    .id = "engine-id",
    .name = "CmdTool",
    .path = CMD_TOOL_PATH,
    .parameterSpec = R"(--printArgs +map ${MAP_BASE_NAME} "with spaces")",
  };

  SECTION("engineParameters")
  {
    CHECK(
      host.engineParameters(document, "+map ${MAP_BASE_NAME}")
      == Result<std::string>{"+map mymap"});
    CHECK(host.engineParameters(document, "+map ${MAP_BASE_NAME").is_error());
  }

  SECTION("launchEngine")
  {
    SECTION("launches the profile's engine with its interpolated parameters")
    {
      const auto processId = host.launchEngine(document, profile, std::nullopt);
      REQUIRE(processId.is_success());
      CHECK(processId.value() > 0);
      CHECK(waitForFile(logFile, "+map\nmymap\nwith spaces\n"));
    }

    SECTION("launches with overridden parameters")
    {
      const auto processId = host.launchEngine(
        document, profile, std::string{"--printArgs -game mod ${MAP_BASE_NAME}"});
      REQUIRE(processId.is_success());
      CHECK(processId.value() > 0);
      CHECK(waitForFile(logFile, "-game\nmod\nmymap\n"));
    }

    SECTION("fails if the engine does not exist")
    {
      auto missing = profile;
      missing.path = env.dir() / "does-not-exist";
      CHECK(host.launchEngine(document, missing, std::nullopt).is_error());
    }
  }
}

} // namespace tb::ui
