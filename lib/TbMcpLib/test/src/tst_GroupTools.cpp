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

#include "mcp/McpToolFixture.h"
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/LinkedGroupUtils.h"
#include "mdl/Map.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/Node.h"
#include "mdl/Selection.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <algorithm>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

const Json& resultOf(const Json& structured)
{
  return structured["result"];
}

mdl::BrushNode* addBox(mdl::Map& map, const vm::bbox3d& bounds)
{
  auto brush = mdl::BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()}
                 .createCuboid(bounds, "stone")
                 .value();
  auto* brushNode = new mdl::BrushNode{std::move(brush)};
  mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode}}});
  return brushNode;
}

mdl::GroupNode* makeGroup(
  mdl::Map& map, const std::vector<mdl::Node*>& nodes, const std::string& name)
{
  mdl::deselectAll(map);
  mdl::selectNodes(map, nodes);
  auto* groupNode = mdl::groupSelectedNodes(map, name);
  mdl::deselectAll(map);
  return groupNode;
}

mdl::GroupNode* makeLinkedDuplicate(mdl::Map& map, mdl::GroupNode& groupNode)
{
  mdl::deselectAll(map);
  mdl::selectNodes(map, {&groupNode});
  auto* duplicate = mdl::createLinkedDuplicate(map);
  mdl::deselectAll(map);
  return duplicate;
}

bool hasWarning(const Json& result, const std::string& code)
{
  return std::ranges::any_of(
    result["warnings"], [&](const auto& warning) { return warning["code"] == code; });
}

bool contains(const Json& list, const std::string& id)
{
  return std::ranges::find(list, Json(id)) != list.end();
}

std::vector<std::string> selectedIds(McpToolFixture& fixture, const mdl::Map& map)
{
  auto result = std::vector<std::string>{};
  for (const auto* node : map.selection().nodes)
  {
    result.push_back(fixture.id(*node));
  }
  return result;
}

std::vector<std::string> sorted(std::vector<std::string> ids)
{
  std::ranges::sort(ids);
  return ids;
}

size_t linkSetSize(mdl::Map& map, const mdl::GroupNode& groupNode)
{
  return mdl::collectGroupsWithLinkId({&map.worldNode()}, groupNode.linkId()).size();
}

} // namespace

TEST_CASE("GroupTools")
{
  auto fixture = McpToolFixture{};
  auto& document = fixture.create();
  auto& map = document.map();
  auto* defaultLayer = map.worldNode().defaultLayer();

  const auto boundsA = vm::bbox3d{{0, 0, 0}, {64, 64, 64}};
  const auto boundsB = vm::bbox3d{{128, 0, 0}, {192, 64, 64}};
  const auto boundsC = vm::bbox3d{{256, 0, 0}, {320, 64, 64}};
  auto* brushA = addBox(map, boundsA);
  auto* brushB = addBox(map, boundsB);
  auto* brushC = addBox(map, boundsC);
  mdl::deselectAll(map);

  const auto idA = fixture.id(*brushA);
  const auto idB = fixture.id(*brushB);
  const auto idC = fixture.id(*brushC);

  SECTION("group_create")
  {
    SECTION("explicit ids")
    {
      const auto created =
        fixture.call("group_create", Json{{"ids", {idA, idB}}, {"name", "house"}});
      CHECK(created["undoStep"] == "AI: Create Group");

      const auto groupId = resultOf(created)["group"].get<std::string>();
      auto* groupNode = dynamic_cast<mdl::GroupNode*>(fixture.node(groupId));
      REQUIRE(groupNode);
      CHECK(groupNode->name() == "house");
      CHECK(brushA->parent() == groupNode);
      CHECK(brushB->parent() == groupNode);
      CHECK(brushC->parent() == defaultLayer);
      CHECK(resultOf(created)["objects"] == Json{idA, idB});
      CHECK(contains(created["changes"]["created"], groupId));
      CHECK(selectedIds(fixture, map) == std::vector<std::string>{groupId});

      // one undo step
      fixture.call("undo");
      CHECK(brushA->parent() == defaultLayer);
      CHECK(fixture.node(groupId) == nullptr);
    }

    SECTION("selection")
    {
      mdl::selectNodes(map, {brushC});
      const auto created = fixture.call("group_create", Json{{"name", "c"}});
      CHECK(brushC->parent() != defaultLayer);
      CHECK(
        fixture.id(*brushC->parent()) == resultOf(created)["group"].get<std::string>());
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("group_create", Json{{"ids", {idA}}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("group_create", Json{{"ids", {idA}}, {"name", ""}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "group_create", Json{{"ids", {"layer:default"}}, {"name", "x"}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("group_create", Json{{"name", "x"}}).code
        == ErrorCode::NoSelection);
    }

    SECTION("dry run")
    {
      const auto dryRun = fixture.call(
        "group_create", Json{{"ids", {idA}}, {"name", "x"}, {"dryRun", true}});
      CHECK(dryRun["changes"]["ephemeral"] == true);
      CHECK(brushA->parent() == defaultLayer);
    }
  }

  SECTION("group_ungroup")
  {
    auto* groupNode = makeGroup(map, {brushA, brushB}, "house");
    REQUIRE(groupNode);
    const auto groupId = fixture.id(*groupNode);

    SECTION("explicit ids")
    {
      const auto ungrouped = fixture.call("group_ungroup", Json{{"ids", {groupId}}});
      CHECK(ungrouped["undoStep"] == "AI: Ungroup");
      CHECK(resultOf(ungrouped)["objects"] == Json{idA, idB});
      CHECK(brushA->parent() == defaultLayer);
      CHECK(brushB->parent() == defaultLayer);
      CHECK(fixture.node(groupId) == nullptr);
      CHECK(contains(ungrouped["changes"]["removed"], groupId));
      CHECK(selectedIds(fixture, map) == std::vector<std::string>{idA, idB});
    }

    SECTION("selection")
    {
      mdl::selectNodes(map, {groupNode});
      fixture.call("group_ungroup");
      CHECK(brushA->parent() == defaultLayer);
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("group_ungroup", Json{{"ids", {idC}}}).code
        == ErrorCode::InvalidArgument);

      mdl::selectNodes(map, {brushC});
      CHECK(
        fixture.callExpectingError("group_ungroup").code == ErrorCode::WrongObjectKind);
    }

    SECTION("dry run")
    {
      fixture.call("group_ungroup", Json{{"ids", {groupId}}, {"dryRun", true}});
      CHECK(brushA->parent() == groupNode);
      CHECK(fixture.node(groupId) == groupNode);
    }
  }

  SECTION("group_rename")
  {
    auto* groupNode = makeGroup(map, {brushA}, "house");
    REQUIRE(groupNode);
    const auto groupId = fixture.id(*groupNode);

    const auto renamed =
      fixture.call("group_rename", Json{{"ids", {groupId}}, {"name", "tower"}});
    CHECK(renamed["undoStep"] == "AI: Rename Groups");
    CHECK(resultOf(renamed) == Json{{"groups", {groupId}}, {"name", "tower"}});
    CHECK(groupNode->name() == "tower");
    CHECK(renamed["changes"]["modified"] == Json{groupId});
    CHECK(map.selection().nodes.empty());

    mdl::selectNodes(map, {groupNode});
    fixture.call("group_rename", Json{{"name", "hall"}});
    CHECK(groupNode->name() == "hall");

    fixture.call("group_rename", Json{{"name", "x"}, {"dryRun", true}});
    CHECK(groupNode->name() == "hall");

    CHECK(
      fixture.callExpectingError("group_rename", Json{{"ids", {groupId}}}).code
      == ErrorCode::InvalidArgument);
  }

  SECTION("groups_merge")
  {
    auto* group1 = makeGroup(map, {brushA}, "one");
    auto* group2 = makeGroup(map, {brushB}, "two");
    REQUIRE(group1);
    REQUIRE(group2);
    const auto id1 = fixture.id(*group1);
    const auto id2 = fixture.id(*group2);

    SECTION("explicit ids")
    {
      const auto merged =
        fixture.call("groups_merge", Json{{"ids", {id1, id2}}, {"target", id1}});
      CHECK(merged["undoStep"] == "AI: Merge Groups");
      CHECK(resultOf(merged)["group"] == id1);
      CHECK(resultOf(merged)["merged"] == Json{id2});
      CHECK(resultOf(merged)["objects"] == Json{idB});
      CHECK(brushB->parent() == group1);
      CHECK(fixture.node(id2) == nullptr);
      CHECK(contains(merged["changes"]["removed"], id2));
      CHECK(selectedIds(fixture, map) == std::vector<std::string>{id1});
    }

    SECTION("selection")
    {
      mdl::selectNodes(map, {group2});
      fixture.call("groups_merge", Json{{"target", id1}});
      CHECK(brushB->parent() == group1);
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("groups_merge", Json{{"ids", {id1}}, {"target", id1}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("groups_merge", Json{{"ids", {id1}}, {"target", idC}})
          .code
        == ErrorCode::InvalidArgument);

      // the target must be selectable, so it cannot be inside a closed group
      auto* outer = makeGroup(map, {group1}, "outer");
      REQUIRE(outer);
      const auto containsTarget = fixture.callExpectingError(
        "groups_merge", Json{{"ids", {fixture.id(*outer)}}, {"target", id1}});
      CHECK(containsTarget.code == ErrorCode::ObjectNotEditable);
    }

    SECTION("dry run")
    {
      fixture.call(
        "groups_merge", Json{{"ids", {id1, id2}}, {"target", id1}, {"dryRun", true}});
      CHECK(brushB->parent() == group2);
    }
  }

  SECTION("group_add_objects")
  {
    auto* groupNode = makeGroup(map, {brushA}, "house");
    REQUIRE(groupNode);
    const auto groupId = fixture.id(*groupNode);

    SECTION("explicit ids")
    {
      const auto added =
        fixture.call("group_add_objects", Json{{"group", groupId}, {"ids", {idC}}});
      CHECK(added["undoStep"] == "AI: Add Objects to Group");
      CHECK(resultOf(added) == Json{{"group", groupId}, {"objects", {idC}}});
      CHECK(brushC->parent() == groupNode);
      CHECK(fixture.id(*brushC) == idC);
      CHECK(selectedIds(fixture, map) == std::vector<std::string>{groupId});
    }

    SECTION("selection")
    {
      mdl::selectNodes(map, {brushB});
      fixture.call("group_add_objects", Json{{"group", groupId}});
      CHECK(brushB->parent() == groupNode);
    }

    SECTION("objects already in the group")
    {
      mdl::openGroup(map, *groupNode);
      const auto error = fixture.callExpectingError(
        "group_add_objects", Json{{"group", groupId}, {"ids", {idA}}});
      CHECK(error.code == ErrorCode::InvalidArgument);
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture
          .callExpectingError(
            "group_add_objects", Json{{"group", groupId}, {"ids", {groupId}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError("group_add_objects", Json{{"group", idB}, {"ids", {idC}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("group_add_objects", Json{{"group", groupId}}).code
        == ErrorCode::NoSelection);
    }

    SECTION("dry run")
    {
      fixture.call(
        "group_add_objects", Json{{"group", groupId}, {"ids", {idC}}, {"dryRun", true}});
      CHECK(brushC->parent() == defaultLayer);
    }
  }

  SECTION("group_remove_objects")
  {
    auto* groupNode = makeGroup(map, {brushA, brushB}, "house");
    REQUIRE(groupNode);
    const auto groupId = fixture.id(*groupNode);

    SECTION("closed group")
    {
      const auto removed = fixture.call("group_remove_objects", Json{{"ids", {idA}}});
      CHECK(removed["undoStep"] == "AI: Remove Objects from Group");
      CHECK(resultOf(removed)["objects"] == Json{idA});
      CHECK(
        resultOf(removed)["moved"]
        == Json::array({{{"id", idA}, {"from", groupId}, {"to", "layer:default"}}}));
      CHECK(brushA->parent() == defaultLayer);
      CHECK(brushB->parent() == groupNode);
      CHECK(selectedIds(fixture, map) == std::vector<std::string>{idA});

      // removing the last object removes the group
      const auto last = fixture.call("group_remove_objects", Json{{"ids", {idB}}});
      CHECK(fixture.node(groupId) == nullptr);
      CHECK(contains(last["changes"]["removed"], groupId));
    }

    SECTION("open group and selection")
    {
      mdl::openGroup(map, *groupNode);
      mdl::selectNodes(map, {brushA});
      const auto removed = fixture.call("group_remove_objects");
      CHECK(brushA->parent() == defaultLayer);
      CHECK(resultOf(removed)["context"]["openGroup"].is_null());
      CHECK(map.editorContext().currentGroup() == nullptr);
      CHECK(selectedIds(fixture, map) == std::vector<std::string>{idA});
    }

    SECTION("nested groups")
    {
      auto* outer = makeGroup(map, {groupNode, brushC}, "outer");
      REQUIRE(outer);
      const auto removed = fixture.call("group_remove_objects", Json{{"ids", {idA}}});
      CHECK(brushA->parent() == outer);
      CHECK(resultOf(removed)["moved"][0]["to"] == fixture.id(*outer));
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("group_remove_objects", Json{{"ids", {idC}}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("group_remove_objects").code
        == ErrorCode::NoSelection);

      auto* other = makeGroup(map, {brushC}, "other");
      REQUIRE(other);
      mdl::openGroup(map, *other);
      CHECK(
        fixture.callExpectingError("group_remove_objects", Json{{"ids", {idA}}}).code
        == ErrorCode::ObjectNotEditable);
    }

    SECTION("dry run")
    {
      fixture.call("group_remove_objects", Json{{"ids", {idA}}, {"dryRun", true}});
      CHECK(brushA->parent() == groupNode);
    }
  }

  SECTION("group_open and group_close")
  {
    auto* inner = makeGroup(map, {brushA}, "inner");
    REQUIRE(inner);
    auto* outer = makeGroup(map, {inner, brushB}, "outer");
    REQUIRE(outer);
    const auto innerId = fixture.id(*inner);
    const auto outerId = fixture.id(*outer);
    const auto& editorContext = map.editorContext();

    // objects in closed groups cannot be edited individually
    CHECK(
      fixture
        .callExpectingError("objects_move", Json{{"ids", {idA}}, {"vector", {16, 0, 0}}})
        .code
      == ErrorCode::ObjectNotEditable);

    SECTION("open nested group")
    {
      const auto opened = fixture.call("group_open", Json{{"group", innerId}});
      CHECK(opened["undoStep"] == "AI: Open Group");
      CHECK(
        resultOf(opened)
        == Json{{"openGroup", innerId}, {"openGroups", {outerId, innerId}}});
      CHECK(editorContext.currentGroup() == inner);
      CHECK(outer->hasOpenedDescendant());

      fixture.call("objects_move", Json{{"ids", {idA}}, {"vector", {16, 0, 0}}});

      const auto closed = fixture.call("group_close");
      CHECK(resultOf(closed)["closed"] == Json{innerId});
      CHECK(resultOf(closed)["openGroup"] == outerId);
      CHECK(editorContext.currentGroup() == outer);

      const auto all = fixture.call("group_close", Json{{"all", true}});
      CHECK(resultOf(all)["closed"] == Json{outerId});
      CHECK(resultOf(all)["openGroup"].is_null());
      CHECK(editorContext.currentGroup() == nullptr);

      const auto none = fixture.call("group_close");
      CHECK(hasWarning(none, "NO_OPEN_GROUP"));
      CHECK(none["undoStep"].is_null());
    }

    SECTION("close all")
    {
      fixture.call("group_open", Json{{"group", innerId}});
      const auto all = fixture.call("group_close", Json{{"all", true}});
      CHECK(resultOf(all)["closed"] == Json{innerId, outerId});
      CHECK(editorContext.currentGroup() == nullptr);
    }

    SECTION("opening an outer group closes the inner one")
    {
      fixture.call("group_open", Json{{"group", innerId}});
      const auto opened = fixture.call("group_open", Json{{"group", outerId}});
      CHECK(resultOf(opened)["openGroups"] == Json{outerId});
      CHECK(!inner->opened());
    }

    SECTION("one undo step")
    {
      fixture.call("group_open", Json{{"group", innerId}});
      fixture.call("undo");
      CHECK(editorContext.currentGroup() == nullptr);
      CHECK(!outer->hasOpenedDescendant());
    }

    SECTION("invalid input and dry run")
    {
      CHECK(
        fixture.callExpectingError("group_open", Json{{"group", idC}}).code
        == ErrorCode::InvalidArgument);
      CHECK(fixture.callExpectingError("group_open").code == ErrorCode::InvalidArgument);

      fixture.call("group_open", Json{{"group", innerId}, {"dryRun", true}});
      CHECK(editorContext.currentGroup() == nullptr);
    }
  }
}

TEST_CASE("LinkedGroupTools")
{
  auto fixture = McpToolFixture{};
  auto& document = fixture.create();
  auto& map = document.map();

  const auto boundsA = vm::bbox3d{{0, 0, 0}, {64, 64, 64}};
  const auto boundsB = vm::bbox3d{{128, 0, 0}, {192, 64, 64}};
  auto* brushA = addBox(map, boundsA);
  auto* brushB = addBox(map, boundsB);
  auto* brushC = addBox(map, {{512, 0, 0}, {576, 64, 64}});
  auto* groupNode = makeGroup(map, {brushA, brushB}, "house");
  REQUIRE(groupNode);

  const auto idA = fixture.id(*brushA);
  const auto idB = fixture.id(*brushB);
  const auto idC = fixture.id(*brushC);
  const auto groupId = fixture.id(*groupNode);

  SECTION("linked_group_duplicate")
  {
    SECTION("offset and count")
    {
      const auto duplicated = fixture.call(
        "linked_group_duplicate",
        Json{{"group", groupId}, {"offset", {0, 256, 0}}, {"count", 2}});
      CHECK(duplicated["undoStep"] == "AI: Create Linked Duplicate");

      const auto& groups = resultOf(duplicated)["groups"];
      REQUIRE(groups.size() == 2);
      CHECK(resultOf(duplicated)["linkedGroups"].size() == 3);
      CHECK(linkSetSize(map, *groupNode) == 3);

      for (size_t i = 0; i < 2; ++i)
      {
        auto* duplicate =
          dynamic_cast<mdl::GroupNode*>(fixture.node(groups[i].get<std::string>()));
        REQUIRE(duplicate);
        CHECK(duplicate->linkId() == groupNode->linkId());
        const auto offset = vm::vec3d{0, 256.0 * double(i + 1), 0};
        CHECK(
          duplicate->logicalBounds()
          == vm::bbox3d{
            groupNode->logicalBounds().min + offset,
            groupNode->logicalBounds().max + offset});
        CHECK(contains(duplicated["changes"]["created"], groups[i].get<std::string>()));
      }
      CHECK(
        selectedIds(fixture, map)
        == std::vector<std::string>{
          groups[0].get<std::string>(), groups[1].get<std::string>()});

      fixture.call("undo");
      CHECK(linkSetSize(map, *groupNode) == 1);
    }

    SECTION("invalid input and dry run")
    {
      CHECK(
        fixture
          .callExpectingError(
            "linked_group_duplicate", Json{{"group", groupId}, {"count", 2}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("linked_group_duplicate", Json{{"group", idC}}).code
        == ErrorCode::InvalidArgument);

      fixture.call("linked_group_duplicate", Json{{"group", groupId}, {"dryRun", true}});
      CHECK(linkSetSize(map, *groupNode) == 1);
    }
  }

  SECTION("linked group updates keep ids")
  {
    auto* duplicate = makeLinkedDuplicate(map, *groupNode);
    REQUIRE(duplicate);
    const auto duplicateId = fixture.id(*duplicate);
    REQUIRE(duplicate->children().size() == 2);
    auto* copyOfA = duplicate->children()[0];
    const auto idCopyA = fixture.id(*copyOfA);
    const auto idCopyB = fixture.id(*duplicate->children()[1]);

    // edit the original: the children of the copy are replaced by clones
    fixture.call("group_open", Json{{"group", groupId}});
    const auto moved =
      fixture.call("objects_move", Json{{"ids", {idA}}, {"vector", {0, 0, 16}}});
    fixture.call("group_close");

    auto* newCopyOfA = fixture.node(idCopyA);
    REQUIRE(newCopyOfA);
    CHECK(newCopyOfA != copyOfA);
    CHECK(newCopyOfA->parent() == duplicate);
    CHECK(newCopyOfA->logicalBounds().min.z() == 16.0);
    CHECK(fixture.id(*newCopyOfA) == idCopyA);
    CHECK(fixture.node(idCopyB) != nullptr);
    CHECK(contains(moved["changes"]["modified"], idA));
    CHECK(contains(moved["changes"]["modified"], idCopyA));
    CHECK(moved["changes"]["created"].empty());
    CHECK(moved["changes"]["removed"].empty());

    // edit the copy: the ids of the original stay valid
    fixture.call("group_open", Json{{"group", duplicateId}});
    const auto movedCopy =
      fixture.call("objects_move", Json{{"ids", {idCopyB}}, {"vector", {0, 0, 16}}});
    fixture.call("group_close");

    auto* newA = fixture.node(idA);
    auto* newB = fixture.node(idB);
    REQUIRE(newA);
    REQUIRE(newB);
    CHECK(newB->parent() == groupNode);
    CHECK(newB->logicalBounds().min.z() == 16.0);
    CHECK(contains(movedCopy["changes"]["modified"], idB));
    CHECK(movedCopy["changes"]["created"].empty());

    // adding objects to a linked group adds them to the copies
    const auto added =
      fixture.call("group_add_objects", Json{{"group", groupId}, {"ids", {idC}}});
    CHECK(duplicate->children().size() == 3);
    CHECK(added["changes"]["created"].size() == 1);
    CHECK(fixture.node(idCopyA) != nullptr);

    // undo restores the old state and the ids
    fixture.call("undo");
    CHECK(duplicate->children().size() == 2);
    CHECK(fixture.node(idCopyA) != nullptr);
    CHECK(fixture.node(idA) != nullptr);
  }

  SECTION("linked_group_select")
  {
    CHECK(
      fixture.callExpectingError("linked_group_select", Json{{"ids", {groupId}}}).code
      == ErrorCode::InvalidArgument);

    auto* duplicate = makeLinkedDuplicate(map, *groupNode);
    REQUIRE(duplicate);
    const auto duplicateId = fixture.id(*duplicate);

    SECTION("objects in closed groups")
    {
      const auto selected = fixture.call("linked_group_select", Json{{"ids", {idA}}});
      CHECK(selected["undoStep"] == "AI: Select Linked Groups");
      CHECK(resultOf(selected)["count"] == 2);
      CHECK(
        sorted(resultOf(selected)["groups"].get<std::vector<std::string>>())
        == sorted({groupId, duplicateId}));
      CHECK(sorted(selectedIds(fixture, map)) == sorted({groupId, duplicateId}));
    }

    SECTION("selection")
    {
      mdl::selectNodes(map, {duplicate});
      fixture.call("linked_group_select");
      CHECK(groupNode->selected());
      CHECK(duplicate->selected());
    }

    SECTION("dry run")
    {
      fixture.call("linked_group_select", Json{{"ids", {groupId}}, {"dryRun", true}});
      CHECK(map.selection().nodes.empty());
    }

    SECTION("not selectable")
    {
      fixture.call("group_open", Json{{"group", groupId}});
      const auto error =
        fixture.callExpectingError("linked_group_select", Json{{"ids", {groupId}}});
      CHECK(error.code == ErrorCode::ObjectNotEditable);
    }
  }

  SECTION("linked_group_separate")
  {
    auto* duplicate1 = makeLinkedDuplicate(map, *groupNode);
    auto* duplicate2 = makeLinkedDuplicate(map, *groupNode);
    REQUIRE(duplicate1);
    REQUIRE(duplicate2);
    const auto id1 = fixture.id(*duplicate1);
    const auto id2 = fixture.id(*duplicate2);
    const auto idCopy1 = fixture.id(*duplicate1->children().front());
    const auto idCopy2 = fixture.id(*duplicate2->children().front());

    SECTION("one group")
    {
      const auto separated = fixture.call("linked_group_separate", Json{{"ids", {id2}}});
      CHECK(separated["undoStep"] == "AI: Separate Linked Groups");
      CHECK(
        resultOf(separated)["groups"]
        == Json::array({{{"id", id2}, {"linkedWith", Json::array()}}}));
      CHECK(linkSetSize(map, *groupNode) == 2);
      CHECK(linkSetSize(map, *duplicate2) == 1);
      CHECK(fixture.node(idCopy2) == duplicate2->children().front());
      CHECK(map.selection().nodes.empty());
    }

    SECTION("several groups stay linked with each other")
    {
      const auto separated =
        fixture.call("linked_group_separate", Json{{"ids", {id1, id2}}});
      CHECK(
        resultOf(separated)["groups"]
        == Json::array(
          {{{"id", id1}, {"linkedWith", {id2}}}, {{"id", id2}, {"linkedWith", {id1}}}}));
      CHECK(linkSetSize(map, *groupNode) == 1);
      CHECK(linkSetSize(map, *duplicate1) == 2);
      CHECK(fixture.node(idCopy1) != nullptr);
      CHECK(fixture.node(idCopy2) != nullptr);

      // editing one of them updates the other only, and ids stay valid
      fixture.call("group_open", Json{{"group", id1}});
      const auto moved =
        fixture.call("objects_move", Json{{"ids", {idCopy1}}, {"vector", {0, 0, 16}}});
      fixture.call("group_close");
      CHECK(contains(moved["changes"]["modified"], idCopy2));
      CHECK(!contains(moved["changes"]["modified"], idA));
      CHECK(fixture.node(idCopy2)->logicalBounds().min.z() == 16.0);
      CHECK(brushA->logicalBounds().min.z() == 0.0);
    }

    SECTION("selection")
    {
      mdl::selectNodes(map, {duplicate2});
      fixture.call("linked_group_separate");
      CHECK(linkSetSize(map, *duplicate2) == 1);
      CHECK(selectedIds(fixture, map) == std::vector<std::string>{id2});
    }

    SECTION("invalid input and dry run")
    {
      CHECK(
        fixture
          .callExpectingError("linked_group_separate", Json{{"ids", {groupId, id1, id2}}})
          .code
        == ErrorCode::InvalidArgument);

      auto* single = makeGroup(map, {brushC}, "single");
      REQUIRE(single);
      CHECK(
        fixture
          .callExpectingError(
            "linked_group_separate", Json{{"ids", {fixture.id(*single)}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("linked_group_separate", Json{{"ids", {idC}}}).code
        == ErrorCode::InvalidArgument);

      fixture.call("linked_group_separate", Json{{"ids", {id2}}, {"dryRun", true}});
      CHECK(linkSetSize(map, *groupNode) == 3);
    }
  }

  SECTION("linked_group_extract")
  {
    auto* duplicate = makeLinkedDuplicate(map, *groupNode);
    REQUIRE(duplicate);
    const auto duplicateId = fixture.id(*duplicate);
    const auto idCopyB = fixture.id(*duplicate->children()[1]);

    SECTION("closed group")
    {
      const auto extracted = fixture.call("linked_group_extract", Json{{"ids", {idA}}});
      CHECK(extracted["undoStep"] == "AI: Extract Linked Groups");

      const auto newGroupId = resultOf(extracted)["group"].get<std::string>();
      auto* newGroup = dynamic_cast<mdl::GroupNode*>(fixture.node(newGroupId));
      REQUIRE(newGroup);
      REQUIRE(newGroup->children().size() == 1);
      CHECK(newGroup->children().front()->logicalBounds() == boundsA);
      CHECK(
        resultOf(extracted)["objects"]
        == Json{fixture.id(*newGroup->children().front())});

      const auto& groups = resultOf(extracted)["groups"];
      REQUIRE(groups.size() == 2);
      CHECK(
        std::ranges::count(groups, Json{{"source", groupId}, {"extracted", newGroupId}})
        == 1);
      CHECK(std::ranges::any_of(
        groups, [&](const auto& entry) { return entry["source"] == duplicateId; }));
      CHECK(linkSetSize(map, *newGroup) == 2);

      // the objects that stay keep their ids, also in the linked copy
      CHECK(groupNode->children().size() == 1);
      CHECK(duplicate->children().size() == 1);
      CHECK(fixture.node(idB) == groupNode->children().front());
      CHECK(fixture.node(idCopyB) == duplicate->children().front());
      CHECK(fixture.node(idA) == nullptr);

      CHECK(map.editorContext().currentGroup() == nullptr);
      CHECK(selectedIds(fixture, map) == std::vector<std::string>{newGroupId});

      fixture.call("undo");
      CHECK(groupNode->children().size() == 2);
      CHECK(fixture.node(idA) == brushA);
    }

    SECTION("open group and selection")
    {
      mdl::openGroup(map, *groupNode);
      mdl::selectNodes(map, {brushB});
      const auto extracted = fixture.call("linked_group_extract");
      auto* newGroup = fixture.node(resultOf(extracted)["group"].get<std::string>());
      REQUIRE(newGroup);
      CHECK(newGroup->children().front()->logicalBounds() == boundsB);
      CHECK(map.editorContext().currentGroup() == nullptr);
    }

    SECTION("invalid input and dry run")
    {
      CHECK(
        fixture.callExpectingError("linked_group_extract", Json{{"ids", {idA, idB}}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("linked_group_extract", Json{{"ids", {idC}}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("linked_group_extract", Json{{"ids", {idA, idCopyB}}})
          .code
        == ErrorCode::InvalidArgument);

      auto* single = makeGroup(map, {brushC}, "single");
      REQUIRE(single);
      CHECK(
        fixture.callExpectingError("linked_group_extract", Json{{"ids", {idC}}}).code
        == ErrorCode::InvalidArgument);

      fixture.call("linked_group_extract", Json{{"ids", {idA}}, {"dryRun", true}});
      CHECK(groupNode->children().size() == 2);
      CHECK(fixture.node(idA) == brushA);
      CHECK(map.editorContext().currentGroup() == nullptr);
    }
  }
}

} // namespace tb::mcp
