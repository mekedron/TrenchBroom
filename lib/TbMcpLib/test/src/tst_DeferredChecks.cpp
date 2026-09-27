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

#include "mcp/ChangeCollector.h"
#include "mcp/McpToolFixture.h"
#include "mcp/ServerState.h"
#include "ui/MapDocument.h"

#include <algorithm>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

std::string createBox(McpToolFixture& fixture, const Json& min, const Json& max)
{
  return fixture
    .call("brush_create_box", Json{{"min", min}, {"max", max}})["result"]["brush"]
    .get<std::string>();
}

/**
 * A closed room of six boxes from (-256, -256, 0) to (256, 256, 192) with a light, so
 * that entities outside it are reported.
 */
void createRoom(McpToolFixture& fixture)
{
  createBox(fixture, {-272, -272, -16}, {272, 272, 0});
  createBox(fixture, {-272, -272, 192}, {272, 272, 208});
  createBox(fixture, {-272, -272, 0}, {-256, 272, 192});
  createBox(fixture, {256, -272, 0}, {272, 272, 192});
  createBox(fixture, {-256, -272, 0}, {256, -256, 192});
  createBox(fixture, {-256, 256, 0}, {256, 272, 192});
  fixture.call(
    "entity_create_point", Json{{"classname", "light"}, {"position", {0, 0, 64}}});
}

size_t countOf(const Json& result, const std::string& code)
{
  auto count = size_t(0);
  for (const auto& issue : result["issuesIntroduced"])
  {
    if (issue["code"] == code)
    {
      ++count;
    }
  }
  return count;
}

} // namespace

TEST_CASE("issuesSummaryJson")
{
  const auto issue = [](std::string id, std::string code, std::string source) {
    return IntroducedIssue{
      std::move(id), "type", "description of " + code, code, std::move(source)};
  };
  const auto summary = issuesSummaryJson(
    {
      issue("entity:1", "A", "mcp"),
      issue("brush:2", "B", "editor"),
      issue("entity:3", "A", "mcp"),
      issue("entity:4", "A", "mcp"),
    },
    2);

  CHECK(summary["total"] == 4);
  CHECK(summary["bySource"] == Json{{"editor", 1}, {"mcp", 3}});
  REQUIRE(summary["byCode"].size() == 2);
  CHECK(summary["byCode"][0]["code"] == "A");
  CHECK(summary["byCode"][0]["source"] == "mcp");
  CHECK(summary["byCode"][0]["count"] == 3);
  CHECK(
    summary["byCode"][0]["examples"]
    == Json::array({
      {{"objectId", "entity:1"}, {"description", "description of A"}},
      {{"objectId", "entity:3"}, {"description", "description of A"}},
    }));
  CHECK(summary["byCode"][1]["code"] == "B");
  CHECK(summary["byCode"][1]["count"] == 1);

  CHECK(issuesSummaryJson({})["byCode"] == Json::array());
}

TEST_CASE("Deferred checks")
{
  auto fixture = McpToolFixture{};
  auto& document = fixture.create();
  createRoom(fixture);

  const auto lightOutside = [&](const double x, Json extra = Json::object()) {
    auto args = Json{{"classname", "light"}, {"position", {x, 0, 64}}};
    args.update(extra);
    return fixture.call("entity_create_point", args);
  };

  SECTION("issuesSummary with detail summary and cut issue lists")
  {
    // eight lights outside the room: eight introduced issues
    const auto light = lightOutside(1024)["result"]["entity"].get<std::string>();
    auto result = fixture.call(
      "objects_array",
      Json{
        {"ids", {light}},
        {"pattern", "line"},
        {"count", 9},
        {"offset", {64, 0, 0}},
        {"detail", "summary"}});
    // eight issues per code: the lights are outside the hull and the test game has no
    // definition for them
    CHECK(result["issuesIntroduced"].size() == 5);
    REQUIRE(result.contains("issuesSummary"));
    const auto& summary = result["issuesSummary"];
    CHECK(summary["total"] == 16);
    CHECK(summary["bySource"] == Json{{"editor", 8}, {"mcp", 8}});
    REQUIRE(summary["byCode"].size() == 2);
    for (const auto& code : summary["byCode"])
    {
      CHECK(code["count"] == 8);
      CHECK(code["examples"].size() == 2);
    }
    CHECK(std::ranges::any_of(summary["byCode"], [](const auto& code) {
      return code["code"] == "ENTITY_OUTSIDE_HULL" && code["source"] == "mcp";
    }));

    // not with a short list at the default detail level
    result = lightOutside(2048);
    CHECK(countOf(result, "ENTITY_OUTSIDE_HULL") == 1);
    CHECK_FALSE(result.contains("issuesSummary"));
  }

  SECTION("a deferred series is reported by the next reporting call")
  {
    auto result = lightOutside(1024, {{"checks", "defer"}});
    CHECK(result["issuesIntroduced"] == Json::array());
    CHECK(result["checks"] == Json{{"deferred", true}, {"calls", 1}});
    const auto light = result["result"]["entity"].get<std::string>();

    result = fixture.call(
      "brush_create_box",
      Json{{"min", {-64, -64, -8}}, {"max", {64, 64, 0}}, {"checks", "defer"}});
    CHECK(result.is_object());
    result = fixture.call(
      "brush_create_box",
      Json{{"min", {-32, -32, -16}}, {"max", {32, 32, 0}}, {"checks", "defer"}});
    CHECK(result["issuesIntroduced"] == Json::array());
    CHECK(result["checks"]["calls"] == 3);

    // a dry run neither ends the series nor defers its own checks
    result = lightOutside(1536, {{"dryRun", true}});
    CHECK(countOf(result, "ENTITY_OUTSIDE_HULL") == 1);
    CHECK_FALSE(result.contains("checks"));
    CHECK(fixture.server().state().documentState(document).deferredChecks.size() == 1);

    // a later call reports the issues of the whole series and its own
    result = lightOutside(2048);
    CHECK(countOf(result, "ENTITY_OUTSIDE_HULL") == 2);
    CHECK(countOf(result, "Z_FIGHTING") >= 1);
    CHECK(result["checks"]["deferredCalls"] == 3);
    CHECK(result["checks"]["changes"]["created"] == 4);
    CHECK(fixture.server().state().documentState(document).deferredChecks.empty());

    // the series is over
    result = lightOutside(3072);
    CHECK_FALSE(result.contains("checks"));
    CHECK(countOf(result, "ENTITY_OUTSIDE_HULL") == 1);
    CHECK(fixture.id(*fixture.node(light)) == light);
  }

  SECTION("objects created and removed in a series are not reported")
  {
    const auto light =
      lightOutside(1024, {{"checks", "defer"}})["result"]["entity"].get<std::string>();
    fixture.call("objects_delete", Json{{"ids", {light}}, {"checks", "defer"}});

    const auto result = fixture.call("checks_report");
    CHECK(result["result"]["deferredSeries"] == true);
    CHECK(result["result"]["deferredCalls"] == 2);
    CHECK(result["issuesIntroduced"] == Json::array());
    CHECK(result["checks"]["changes"]["created"] == 0);
    CHECK(result["undoStep"].is_null());

    const auto again = fixture.call("checks_report");
    CHECK(again["result"]["deferredSeries"] == false);
    CHECK_FALSE(again.contains("checks"));
  }

  SECTION("a failed first call does not start a series")
  {
    fixture.callExpectingError(
      "objects_delete", Json{{"ids", {"brush:999999"}}, {"checks", "defer"}});
    CHECK(fixture.server().state().documentState(document).deferredChecks.empty());
  }

  SECTION("transactions with deferred checks report on commit")
  {
    auto result = fixture.call(
      "transaction_begin", Json{{"name", "Import shell"}, {"checks", "defer"}});
    CHECK(result["result"]["checks"] == "defer");

    result = lightOutside(1024);
    CHECK(result["issuesIntroduced"] == Json::array());
    CHECK(result["checks"]["deferred"] == true);
    result = lightOutside(1088);
    CHECK(result["checks"]["calls"] == 3);

    result = fixture.call("transaction_commit", Json{{"detail", "summary"}});
    CHECK(result["undoStep"] == "AI: Import shell");
    CHECK(countOf(result, "ENTITY_OUTSIDE_HULL") == 2);
    CHECK(result["issuesSummary"]["byCode"][0]["count"] == 2);
    CHECK(result["checks"]["deferredCalls"] == 3);
    CHECK(fixture.server().state().documentState(document).deferredChecks.empty());
  }

  SECTION("an explicit report inside a deferring transaction ends the series")
  {
    fixture.call("transaction_begin", Json{{"name", "Build"}, {"checks", "defer"}});
    lightOutside(1024);
    auto result = lightOutside(1088, {{"checks", "report"}});
    CHECK(countOf(result, "ENTITY_OUTSIDE_HULL") == 2);
    CHECK(result["checks"]["deferredCalls"] == 2);

    // the following calls defer again
    result = lightOutside(1152);
    CHECK(result["checks"] == Json{{"deferred", true}, {"calls", 1}});
    result = fixture.call("transaction_commit");
    CHECK(countOf(result, "ENTITY_OUTSIDE_HULL") == 1);
  }

  SECTION("closing the session drops its series")
  {
    lightOutside(1024, {{"checks", "defer"}});
    CHECK(fixture.server().state().documentState(document).deferredChecks.size() == 1);
    fixture.server().state().closeSession(fixture.sessionId());
    CHECK(fixture.server().state().documentState(document).deferredChecks.empty());
  }
}

} // namespace tb::mcp
