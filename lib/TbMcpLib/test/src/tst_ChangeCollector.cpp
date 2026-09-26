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

    const auto truncated = changesToJson(report, 2);
    CHECK(truncated["created"].size() == 2);
    CHECK(truncated["truncated"] == true);
    CHECK(truncated["counts"]["created"] == 3);
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
}

} // namespace tb::mcp
