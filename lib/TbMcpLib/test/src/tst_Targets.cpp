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

#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/McpServer.h"
#include "mcp/McpToolFixture.h"
#include "mcp/Targets.h"
#include "mcp/ToolRegistry.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/CommandProcessor.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Brushes.h"
#include "mdl/Map_Geometry.h"
#include "mdl/Map_NodeLocking.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/Selection.h"
#include "mdl/TestFactory.h"
#include "mdl/UpdateBrushFaceAttributes.h"
#include "ui/MapDocument.h"

#include "vm/bbox.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
using namespace schema;

namespace
{

void registerTestTools(McpServer& server)
{
  const auto move = [](const SelectionAfter after) {
    return [after](CallContext& context, const Args& args) -> ToolResult {
      auto targets = resolveTargets(context, args);
      if (targets.is_error())
      {
        return errorOf(targets);
      }
      return withTargets(
        context,
        targets.value(),
        [&]() -> ToolResult {
          if (!mdl::translateSelection(context.map(), args.get<vm::vec3d>("vector")))
          {
            return context.operationFailed("Could not move.");
          }
          return Json{{"moved", context.map().selection().nodes.size()}};
        },
        after);
    };
  };

  server.tools().add(ToolDef{"test_move"}
                       .input(object({
                         idsField(),
                         field("vector", vec3()).required(),
                       }))
                       .mutation(Mutation::Map)
                       .handler(move(SelectionAfter::Restore)));

  server.tools().add(ToolDef{"test_move_select"}
                       .input(object({
                         idsField(),
                         field("vector", vec3()).required(),
                       }))
                       .mutation(Mutation::Map)
                       .handler(move(SelectionAfter::Result)));

  server.tools().add(ToolDef{"test_brushes_only"}
                       .input(object({idsField()}))
                       .mutation(Mutation::Map)
                       .handler([](CallContext& context, const Args& args) -> ToolResult {
                         auto targets =
                           resolveTargets(context, args, "ids", {ObjectKind::Brush});
                         if (targets.is_error())
                         {
                           return errorOf(targets);
                         }
                         return Json{{"count", targets.value().size()}};
                       }));

  server.tools().add(
    ToolDef{"test_faces"}
      .input(object({faceTargetsField()}))
      .mutation(Mutation::Map)
      .handler([](CallContext& context, const Args& args) -> ToolResult {
        auto faces = resolveFaceTargets(context, args);
        if (faces.is_error())
        {
          return errorOf(faces);
        }
        return withFaces(context, faces.value(), [&]() -> ToolResult {
          auto& map = context.map();
          const auto selected = map.selection().brushFaces.size();
          if (!mdl::setBrushFaceAttributes(map, {.materialName = "changed"}))
          {
            return context.operationFailed("Could not change the faces.");
          }
          return Json{{"faces", faces.value().size()}, {"selected", selected}};
        });
      }));

  server.tools().add(ToolDef{"test_face"}
                       .input(object({field("face", string()).required()}))
                       .documentUse(DocumentUse::Required)
                       .handler([](CallContext& context, const Args& args) -> ToolResult {
                         auto face = resolveFace(context, args.get<std::string>("face"));
                         if (face.is_error())
                         {
                           return errorOf(face);
                         }
                         return Json{{"index", face.value().faceIndex()}};
                       }));
}

} // namespace

TEST_CASE("Targets")
{
  auto fixture = McpToolFixture{};
  registerTestTools(fixture.server());
  auto& document = fixture.create();
  auto& map = document.map();

  auto* brushNode1 = mdl::createBrushNode(map);
  auto* brushNode2 = mdl::createBrushNode(map);
  auto* entityNode = new mdl::EntityNode{mdl::Entity{}};
  mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode1, brushNode2, entityNode}}});
  const auto brushId1 = fixture.id(*brushNode1);
  const auto brushId2 = fixture.id(*brushNode2);
  const auto entityId = fixture.id(*entityNode);

  const auto bounds1 = brushNode1->logicalBounds();
  const auto bounds2 = brushNode2->logicalBounds();

  SECTION("resolveTargets")
  {
    SECTION("uses explicit ids")
    {
      fixture.call("test_move", Json{{"ids", {brushId1}}, {"vector", {16, 0, 0}}});
      CHECK(brushNode1->logicalBounds() == bounds1.translate({16, 0, 0}));
      CHECK(brushNode2->logicalBounds() == bounds2);
    }

    SECTION("defaults to the selection")
    {
      mdl::selectNodes(map, {brushNode2});
      fixture.call("test_move", Json{{"vector", {16, 0, 0}}});
      CHECK(brushNode1->logicalBounds() == bounds1);
      CHECK(brushNode2->logicalBounds() == bounds2.translate({16, 0, 0}));
    }

    SECTION("fails without ids and selection")
    {
      CHECK(
        fixture.callExpectingError("test_move", Json{{"vector", {16, 0, 0}}}).code
        == ErrorCode::NoSelection);
    }

    SECTION("fails for unknown ids")
    {
      const auto error = fixture.callExpectingError(
        "test_move", Json{{"ids", {"brush:999999"}}, {"vector", {16, 0, 0}}});
      CHECK(error.code == ErrorCode::ObjectNotFound);
      CHECK(brushNode1->logicalBounds() == bounds1);
    }

    SECTION("fails for the wrong kind")
    {
      CHECK(
        fixture.callExpectingError("test_brushes_only", Json{{"ids", {entityId}}}).code
        == ErrorCode::WrongObjectKind);
      CHECK(
        fixture.call(
          "test_brushes_only", Json{{"ids", {brushId1, brushId2}}})["result"]["count"]
        == 2);
    }

    SECTION("fails for locked objects")
    {
      mdl::lockNodes(map, {brushNode1});
      const auto error = fixture.callExpectingError(
        "test_move", Json{{"ids", {brushId1}}, {"vector", {16, 0, 0}}});
      CHECK(error.code == ErrorCode::ObjectNotEditable);
      CHECK(error.objectIds == std::vector<std::string>{brushId1});
    }
  }

  SECTION("withTargets")
  {
    SECTION("restores the selection")
    {
      mdl::selectNodes(map, {brushNode2});
      const auto result =
        fixture.call("test_move", Json{{"ids", {brushId1}}, {"vector", {16, 0, 0}}});
      CHECK(result["result"]["moved"] == 1);
      CHECK(map.selection().nodes == std::vector<mdl::Node*>{brushNode2});
      CHECK(result["selection"]["ids"] == Json::array({brushId2}));

      // selecting and moving is one undo step
      CHECK(*map.undoCommandName() == "AI: test_move");
      map.undoCommand();
      CHECK(brushNode1->logicalBounds() == bounds1);
      CHECK(map.selection().nodes == std::vector<mdl::Node*>{brushNode2});
    }

    SECTION("restores a face selection")
    {
      mdl::selectBrushFaces(map, {{brushNode2, 0}});
      fixture.call("test_move", Json{{"ids", {brushId1}}, {"vector", {16, 0, 0}}});
      REQUIRE(map.selection().brushFaces.size() == 1);
      CHECK(map.selection().brushFaces[0] == mdl::BrushFaceHandle{brushNode2, 0});
    }

    SECTION("leaves the result selected with SelectionAfter::Result")
    {
      mdl::selectNodes(map, {brushNode2});
      fixture.call("test_move_select", Json{{"ids", {brushId1}}, {"vector", {16, 0, 0}}});
      CHECK(map.selection().nodes == std::vector<mdl::Node*>{brushNode1});
    }

    SECTION("a dry run leaves the selection unchanged")
    {
      mdl::selectNodes(map, {brushNode2});
      fixture.call(
        "test_move", Json{{"ids", {brushId1}}, {"vector", {16, 0, 0}}, {"dryRun", true}});
      CHECK(map.selection().nodes == std::vector<mdl::Node*>{brushNode2});
      CHECK(brushNode1->logicalBounds() == bounds1);
    }
  }

  SECTION("resolveFaceTargets and withFaces")
  {
    const auto faceCount = brushNode1->brush().faceCount();
    const auto materialOf = [](const mdl::BrushNode& brushNode, const size_t index) {
      return brushNode.brush().face(index).materialName();
    };

    SECTION("face ids")
    {
      mdl::selectNodes(map, {brushNode2});
      const auto result = fixture.call(
        "test_faces", Json{{"ids", {brushId1 + "/face:1", brushId1 + "/face:1"}}});
      CHECK(result["result"] == Json{{"faces", 1}, {"selected", 1}});
      CHECK(materialOf(*brushNode1, 1) == "changed");
      CHECK(materialOf(*brushNode1, 0) != "changed");
      CHECK(map.selection().nodes == std::vector<mdl::Node*>{brushNode2});

      // selecting and changing is one undo step
      map.undoCommand();
      CHECK(materialOf(*brushNode1, 1) != "changed");
      CHECK(map.selection().nodes == std::vector<mdl::Node*>{brushNode2});
    }

    SECTION("brush ids stand for all their faces")
    {
      const auto result =
        fixture.call("test_faces", Json{{"ids", {brushId1, brushId2 + "/face:0"}}});
      CHECK(result["result"]["faces"] == faceCount + 1);
      CHECK(materialOf(*brushNode2, 0) == "changed");
      CHECK(materialOf(*brushNode2, 1) != "changed");
      CHECK(map.selection().nodes.empty());
      CHECK(map.selection().brushFaces.empty());
    }

    SECTION("defaults to the selected faces")
    {
      mdl::selectBrushFaces(map, {{brushNode2, 2}});
      const auto result = fixture.call("test_faces");
      CHECK(result["result"] == Json{{"faces", 1}, {"selected", 1}});
      CHECK(materialOf(*brushNode2, 2) == "changed");
      CHECK(
        map.selection().brushFaces == std::vector{mdl::BrushFaceHandle{brushNode2, 2}});
    }

    SECTION("defaults to the faces of the selected objects")
    {
      mdl::selectNodes(map, {brushNode1, entityNode});
      const auto result = fixture.call("test_faces");
      CHECK(result["result"]["faces"] == faceCount);
      CHECK(materialOf(*brushNode1, 0) == "changed");
      CHECK(map.selection().nodes.size() == 2);
    }

    SECTION("fails without faces")
    {
      CHECK(fixture.callExpectingError("test_faces").code == ErrorCode::NoSelection);

      mdl::selectNodes(map, {entityNode});
      CHECK(fixture.callExpectingError("test_faces").code == ErrorCode::NoSelection);

      const auto error =
        fixture.callExpectingError("test_faces", Json{{"ids", {entityId}}});
      CHECK(error.code == ErrorCode::ObjectNotEditable);
      CHECK(error.objectIds == std::vector<std::string>{entityId});
    }

    SECTION("a layer stands for the faces of its brushes")
    {
      // both brushes
      const auto result = fixture.call("test_faces", Json{{"ids", {"layer:default"}}});
      CHECK(result["result"]["faces"] == 2 * faceCount);
    }

    SECTION("fails for bad ids")
    {
      CHECK(
        fixture.callExpectingError("test_faces", Json{{"ids", {"world"}}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("test_faces", Json{{"ids", {brushId1 + "/face:99"}}})
          .code
        == ErrorCode::ObjectNotFound);
    }

    SECTION("fails for locked faces")
    {
      mdl::lockNodes(map, {brushNode1});
      const auto faceId = brushId1 + "/face:0";
      const auto error =
        fixture.callExpectingError("test_faces", Json{{"ids", {faceId}}});
      CHECK(error.code == ErrorCode::ObjectNotEditable);
      CHECK(error.objectIds == std::vector<std::string>{faceId});
    }
  }

  SECTION("resolveFace")
  {
    CHECK(fixture.call("test_face", Json{{"face", brushId1 + "/face:2"}})["index"] == 2);
    CHECK(
      fixture.callExpectingError("test_face", Json{{"face", brushId1}}).code
      == ErrorCode::InvalidArgument);
    CHECK(
      fixture.callExpectingError("test_face", Json{{"face", brushId1 + "/face:99"}}).code
      == ErrorCode::ObjectNotFound);
  }
}

} // namespace tb::mcp
