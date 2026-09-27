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
#include "mcp/ObjectIds.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Entities.h"
#include "mdl/Map_Geometry.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/TestFactory.h"
#include "mdl/TransactionScope.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"
#include "ui/MapDocumentFixture.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{

TEST_CASE("ChangeCollector")
{
  auto fixture = ui::MapDocumentFixture{};
  auto& document = fixture.create();
  auto& map = document.map();
  auto ids = IdRegistry{document};

  SECTION("finish")
  {
    SECTION("reports created objects and their modified parent")
    {
      auto collector = ChangeCollector{document, ids};
      auto* brushNode = mdl::createBrushNode(map);
      mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode}}});

      const auto report = collector.finish();
      CHECK(report.created == std::vector<std::string>{ids.format(*brushNode)});
      CHECK(report.modified == std::vector<std::string>{"layer:default"});
      CHECK(report.removed.empty());
    }

    SECTION("reports removed objects")
    {
      auto* brushNode = mdl::createBrushNode(map);
      mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode}}});
      const auto brushId = ids.format(*brushNode);

      auto collector = ChangeCollector{document, ids};
      mdl::removeNodes(map, {brushNode});

      const auto report = collector.finish();
      CHECK(report.created.empty());
      CHECK(report.removed == std::vector<std::string>{brushId});
    }

    SECTION("reports modified objects")
    {
      auto* brushNode = mdl::createBrushNode(map);
      mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode}}});
      mdl::selectNodes(map, {brushNode});

      auto collector = ChangeCollector{document, ids};
      REQUIRE(mdl::translateSelection(map, {16, 0, 0}));

      const auto report = collector.finish();
      CHECK(report.modified == std::vector<std::string>{ids.format(*brushNode)});
      CHECK(report.created.empty());
      CHECK(report.removed.empty());
    }

    SECTION("objects created and removed in the same call are not reported")
    {
      auto collector = ChangeCollector{document, ids};
      map.startTransaction("", mdl::TransactionScope::Oneshot);
      auto* brushNode = mdl::createBrushNode(map);
      mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode}}});
      mdl::removeNodes(map, {brushNode});
      map.commitTransaction();

      const auto report = collector.finish();
      CHECK(report.created.empty());
      CHECK(report.removed.empty());
    }

    SECTION("records selection changes")
    {
      auto* brushNode = mdl::createBrushNode(map);
      mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode}}});

      auto collector = ChangeCollector{document, ids};
      mdl::selectNodes(map, {brushNode});
      CHECK(collector.finish().selectionChanged);
    }

    SECTION("linked group updates are reported as modifications")
    {
      auto* brushNode = mdl::createBrushNode(map);
      mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode}}});
      mdl::selectNodes(map, {brushNode});
      auto* groupNode = mdl::groupSelectedNodes(map, "group");
      mdl::deselectAll(map);
      mdl::selectNodes(map, {groupNode});
      auto* linkedGroupNode = mdl::createLinkedDuplicate(map);
      mdl::deselectAll(map);
      const auto linkedBrushId = ids.format(*linkedGroupNode->children().front());

      mdl::openGroup(map, *groupNode);
      mdl::selectNodes(map, {brushNode});

      auto collector = ChangeCollector{document, ids};
      REQUIRE(mdl::translateSelection(map, {16, 0, 0}));
      const auto report = collector.finish();

      CHECK(report.created.empty());
      CHECK(report.removed.empty());
      CHECK(std::ranges::find(report.modified, linkedBrushId) != report.modified.end());
      CHECK(
        std::ranges::find(report.modified, ids.format(*brushNode))
        != report.modified.end());
    }

    SECTION("reports introduced issues")
    {
      auto collector = ChangeCollector{document, ids};
      auto* entityNode = new mdl::EntityNode{mdl::Entity{}};
      mdl::addNodes(map, {{&mdl::parentForNodes(map), {entityNode}}});

      const auto report = collector.finish();
      REQUIRE(!report.issuesIntroduced.empty());
      CHECK(report.issuesIntroduced[0].objectId == ids.format(*entityNode));
    }

    SECTION("existing issues are not reported again")
    {
      auto* entityNode = new mdl::EntityNode{mdl::Entity{}};
      mdl::addNodes(map, {{&mdl::parentForNodes(map), {entityNode}}});
      mdl::selectNodes(map, {entityNode});

      auto collector = ChangeCollector{document, ids};
      mdl::setEntityProperty(map, "some_key", "some_value");

      const auto report = collector.finish();
      CHECK(
        std::ranges::find(report.modified, ids.format(*entityNode))
        != report.modified.end());
      CHECK(report.issuesIntroduced.empty());
    }
  }
}

TEST_CASE("ChangeReport")
{
  SECTION("changesToJson")
  {
    auto report = ChangeReport{};
    report.created = {"brush:1", "brush:2", "brush:3"};
    report.removed = {"entity:4"};

    CHECK(
      changesToJson(report)
      == Json{
        {"created", {"brush:1", "brush:2", "brush:3"}},
        {"modified", Json::array()},
        {"removed", {"entity:4"}},
      });

    // summary: at most 5 ids per list, counts per kind
    report.created.clear();
    for (size_t i = 0; i < 60; ++i)
    {
      report.created.push_back("brush:" + std::to_string(i + 1));
    }
    report.created.push_back("entity:100");
    const auto summary = changesToJson(report, ListDetail::Summary);
    CHECK(summary["created"].size() == 5);
    CHECK(summary["truncated"] == true);
    CHECK(summary["counts"] == Json{{"created", 61}, {"modified", 0}, {"removed", 1}});
    CHECK(
      summary["countsByKind"]
      == Json{
        {"created", {{"brush", 60}, {"entity", 1}}},
        {"removed", {{"entity", 1}}},
      });

    // ids: at most 50 ids per list; the cut list is reported in full
    auto truncated = std::vector<TruncatedList>{};
    const auto ids = changesToJson(report, ListDetail::Ids, &truncated);
    CHECK(ids["created"].size() == 50);
    CHECK(ids["removed"] == Json{"entity:4"});
    CHECK(ids["truncated"] == true);
    CHECK(ids["counts"]["created"] == 61);
    REQUIRE(truncated.size() == 1);
    CHECK(truncated[0].path == "changes.created");
    CHECK(truncated[0].items.size() == 61);

    // full: everything, no counts
    const auto full = changesToJson(report, ListDetail::Full);
    CHECK(full["created"].size() == 61);
    CHECK_FALSE(full.contains("truncated"));
    CHECK_FALSE(full.contains("counts"));
  }

  SECTION("selectionSummary")
  {
    auto fixture = ui::MapDocumentFixture{};
    auto& document = fixture.create();
    auto& map = document.map();
    auto ids = IdRegistry{document};

    CHECK(
      selectionSummary(map, ids)
      == Json{
        {"mode", "none"}, {"count", 0}, {"ids", Json::array()}, {"truncated", false}});

    auto* brushNode = mdl::createBrushNode(map);
    mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode}}});
    mdl::selectNodes(map, {brushNode});
    CHECK(
      selectionSummary(map, ids)
      == Json{
        {"mode", "objects"},
        {"count", 1},
        {"ids", {ids.format(*brushNode)}},
        {"truncated", false}});
  }

  SECTION("issueCode")
  {
    CHECK(issueCode("Empty brush entity") == "EMPTY_BRUSH_ENTITY");
    CHECK(issueCode("Invalid UV scale") == "INVALID_UV_SCALE");
    CHECK(
      issueCode("Paths must use forward slashes") == "PATHS_MUST_USE_FORWARD_SLASHES");
    CHECK(issueCode("Non-integer vertices") == "NON_INTEGER_VERTICES");
  }

  SECTION("issuesToJson")
  {
    auto issues = std::vector<IntroducedIssue>{
      IntroducedIssue{
        "entity:1",
        "Missing entity classname",
        "no classname",
        "MISSING_ENTITY_CLASSNAME"},
      introducedIssue(McpIssue{
        "Z_FIGHTING",
        "Z-fighting",
        "brush:2/face:1",
        "overlap",
        Json{{"area", 64}},
        "signature",
        {"brush:2/face:1", "brush:3/face:0"}}),
    };
    const auto json = issuesToJson(issues);
    CHECK(
      json[0]
      == Json{
        {"objectId", "entity:1"},
        {"type", "Missing entity classname"},
        {"description", "no classname"},
        {"code", "MISSING_ENTITY_CLASSNAME"},
        {"source", "editor"}});
    CHECK(
      json[1]
      == Json{
        {"objectId", "brush:2/face:1"},
        {"type", "Z-fighting"},
        {"description", "overlap"},
        {"code", "Z_FIGHTING"},
        {"source", "mcp"},
        {"details", {{"area", 64}}}});
  }

  SECTION("removeIssuesWarnedAbout")
  {
    auto issues = std::vector<IntroducedIssue>{
      introducedIssue(McpIssue{
        .code = "MODEL_FLOATING", .type = "Model placement", .objectId = "entity:5"}),
      introducedIssue(McpIssue{
        .code = "MODEL_FLOATING", .type = "Model placement", .objectId = "entity:6"}),
      introducedIssue(McpIssue{
        .code = "Z_FIGHTING", .type = "Z-fighting", .objectId = "brush:2/face:1"}),
      IntroducedIssue{"entity:5", "Missing entity classname", "", "MODEL_FLOATING"},
    };
    removeIssuesWarnedAbout(
      issues,
      {Warning{"MODEL_FLOATING", "floats", {"entity:5", "brush:1"}},
       Warning{"Z_FIGHTING", "other object", {"brush:9"}}});
    REQUIRE(issues.size() == 3);
    CHECK(issues[0].objectId == "entity:6");
    CHECK(issues[1].code == "Z_FIGHTING");
    // editor issues are never removed
    CHECK(issues[2].source == "editor");
  }
}

} // namespace tb::mcp
