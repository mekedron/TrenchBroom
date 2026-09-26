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

#include "mcp/ObjectIds.h"
#include "mdl/Brush.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Geometry.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/TestFactory.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"
#include "ui/MapDocumentFixture.h"

#include <filesystem>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{

TEST_CASE("ObjectRef")
{
  SECTION("parseObjectRef")
  {
    CHECK(parseObjectRef("brush:12") == ObjectRef{ObjectKind::Brush, 12});
    CHECK(parseObjectRef("entity:1") == ObjectRef{ObjectKind::Entity, 1});
    CHECK(parseObjectRef("group:3") == ObjectRef{ObjectKind::Group, 3});
    CHECK(parseObjectRef("layer:4") == ObjectRef{ObjectKind::Layer, 4});
    CHECK(parseObjectRef("patch:5") == ObjectRef{ObjectKind::Patch, 5});
    CHECK(parseObjectRef("world") == ObjectRef{ObjectKind::World});
    CHECK(
      parseObjectRef("layer:default")
      == ObjectRef{ObjectKind::Layer, std::nullopt, true});
    CHECK(
      parseObjectRef("brush:12/face:3")
      == ObjectRef{ObjectKind::Brush, 12, false, size_t{3}});

    CHECK(parseObjectRef("") == std::nullopt);
    CHECK(parseObjectRef("brush") == std::nullopt);
    CHECK(parseObjectRef("brush:") == std::nullopt);
    CHECK(parseObjectRef("brush:01") == std::nullopt);
    CHECK(parseObjectRef("brush:-1") == std::nullopt);
    CHECK(parseObjectRef("brush:1x") == std::nullopt);
    CHECK(parseObjectRef("thing:1") == std::nullopt);
    CHECK(parseObjectRef("world:1") == std::nullopt);
    CHECK(parseObjectRef("entity:1/face:0") == std::nullopt);
    CHECK(parseObjectRef("brush:1/edge:0") == std::nullopt);
  }

  SECTION("formatObjectRef")
  {
    for (const auto* id :
         {"brush:12", "world", "layer:default", "layer:4", "brush:12/face:3", "patch:1"})
    {
      CHECK(formatObjectRef(*parseObjectRef(id)) == id);
    }
  }
}

TEST_CASE("IdRegistry")
{
  auto fixture = ui::MapDocumentFixture{};
  auto& document = fixture.create();
  auto& map = document.map();
  auto ids = IdRegistry{document};

  SECTION("format")
  {
    CHECK(ids.format(map.worldNode()) == "world");
    CHECK(ids.format(*map.worldNode().defaultLayer()) == "layer:default");

    auto* brushNode = mdl::createBrushNode(map);
    mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode}}});
    CHECK(ids.format(*brushNode) == "brush:" + std::to_string(brushNode->runtimeId()));
    CHECK(
      ids.formatFace(*brushNode, 2)
      == "brush:" + std::to_string(brushNode->runtimeId()) + "/face:2");
  }

  SECTION("resolve")
  {
    auto* brushNode = mdl::createBrushNode(map);
    auto* entityNode = new mdl::EntityNode{mdl::Entity{}};
    mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode, entityNode}}});

    const auto brushId = ids.format(*brushNode);
    const auto entityId = ids.format(*entityNode);

    CHECK(ids.resolve("world").value() == &map.worldNode());
    CHECK(ids.resolve("layer:default").value() == map.worldNode().defaultLayer());
    CHECK(ids.resolve(brushId).value() == brushNode);
    CHECK(ids.resolve(entityId).value() == entityNode);
    CHECK(ids.resolve(brushId + "/face:0").value() == brushNode);

    SECTION("reports invalid ids")
    {
      CHECK(errorOf(ids.resolve("nonsense")).code == ErrorCode::InvalidArgument);
    }

    SECTION("reports the wrong kind")
    {
      const auto wrongKind = "entity:" + std::to_string(brushNode->runtimeId());
      const auto error = errorOf(ids.resolve(wrongKind));
      CHECK(error.code == ErrorCode::WrongObjectKind);
      CHECK(error.hint.find(brushId) != std::string::npos);
    }

    SECTION("reports invalid face indices")
    {
      CHECK(errorOf(ids.resolve(brushId + "/face:99")).code == ErrorCode::ObjectNotFound);
    }

    SECTION("removed objects are not found, but undo restores them")
    {
      mdl::removeNodes(map, {brushNode});
      const auto error = errorOf(ids.resolve(brushId));
      CHECK(error.code == ErrorCode::ObjectNotFound);
      CHECK(error.objectIds == std::vector<std::string>{brushId});

      map.undoCommand();
      CHECK(ids.resolve(brushId).value() == brushNode);

      map.redoCommand();
      CHECK(ids.resolve(brushId).is_error());
    }

    SECTION("undoing the creation removes the object and redo restores it")
    {
      map.undoCommand();
      CHECK(ids.resolve(brushId).is_error());
      map.redoCommand();
      CHECK(ids.resolve(brushId).value() == brushNode);
    }

    SECTION("children of removed nodes are unregistered")
    {
      mdl::deselectAll(map);
      mdl::selectNodes(map, {brushNode});
      auto* groupNode = mdl::groupSelectedNodes(map, "group");
      REQUIRE(groupNode);
      CHECK(ids.resolve(brushId).value() == brushNode);

      mdl::deselectAll(map);
      mdl::removeNodes(map, {groupNode});
      CHECK(ids.resolve(brushId).is_error());
    }
  }

  SECTION("linked groups keep the ids of their members")
  {
    auto* brushNode = mdl::createBrushNode(map);
    mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode}}});
    mdl::selectNodes(map, {brushNode});
    auto* groupNode = mdl::groupSelectedNodes(map, "group");
    REQUIRE(groupNode);

    mdl::deselectAll(map);
    mdl::selectNodes(map, {groupNode});
    auto* linkedGroupNode = mdl::createLinkedDuplicate(map);
    REQUIRE(linkedGroupNode);
    mdl::deselectAll(map);

    auto* linkedBrushNode = linkedGroupNode->children().front();
    const auto linkedBrushId = ids.format(*linkedBrushNode);

    // editing the original replaces the children of the linked copy with clones
    mdl::openGroup(map, *groupNode);
    mdl::selectNodes(map, {brushNode});
    REQUIRE(mdl::translateSelection(map, {16, 0, 0}));
    mdl::deselectAll(map);
    mdl::closeGroup(map);

    auto* newLinkedBrushNode = linkedGroupNode->children().front();
    REQUIRE(newLinkedBrushNode != linkedBrushNode);

    CHECK(ids.resolve(linkedBrushId).value() == newLinkedBrushNode);
    CHECK(ids.format(*newLinkedBrushNode) == linkedBrushId);
  }

  SECTION("reloading invalidates object ids but remaps groups")
  {
    auto* brushNode = mdl::createBrushNode(map);
    auto* otherBrushNode = mdl::createBrushNode(map);
    mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode, otherBrushNode}}});
    mdl::selectNodes(map, {otherBrushNode});
    auto* groupNode = mdl::groupSelectedNodes(map, "group");
    REQUIRE(groupNode);
    mdl::deselectAll(map);

    const auto brushId = ids.format(*brushNode);
    const auto groupId = ids.format(*groupNode);

    const auto path =
      std::filesystem::temp_directory_path() / "tb-mcp-idregistry-reload-test.map";
    REQUIRE(map.saveAs(path).is_success());
    REQUIRE(document.reload().is_success());

    const auto error = errorOf(ids.resolve(brushId));
    CHECK(error.code == ErrorCode::ObjectNotFound);
    CHECK(error.message.find("reloaded") != std::string::npos);

    const auto remappedGroup = ids.resolve(groupId);
    REQUIRE(remappedGroup.is_success());
    CHECK(dynamic_cast<mdl::GroupNode*>(remappedGroup.value()) != nullptr);
    CHECK(remappedGroup.value() != groupNode);

    CHECK(
      ids.resolve("layer:default").value() == document.map().worldNode().defaultLayer());

    std::filesystem::remove(path);
  }
}

} // namespace tb::mcp
