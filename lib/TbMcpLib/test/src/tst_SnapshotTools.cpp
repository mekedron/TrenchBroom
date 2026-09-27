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
#include "fs/TestEnvironment.h"
#include "mcp/Image.h"
#include "mcp/JsonRpc.h"
#include "mcp/JsonVm.h"
#include "mcp/McpServer.h"
#include "mcp/McpToolFixture.h"
#include "mcp/ServerState.h"
#include "mcp/Session.h"
#include "mcp/ToolRegistry.h"
#include "mdl/BrushNode.h"
#include "mdl/CommandProcessor.h"
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GameManager.h"
#include "mdl/Grid.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/TransactionScope.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

mdl::Node* findNode(
  mdl::Node& node, const std::function<bool(const mdl::Node&)>& predicate)
{
  if (predicate(node))
  {
    return &node;
  }
  for (auto* child : node.children())
  {
    if (auto* result = findNode(*child, predicate))
    {
      return result;
    }
  }
  return nullptr;
}

mdl::Node* findEntity(mdl::Map& map, const std::string& classname)
{
  auto* node = findNode(map.worldNode(), [&](const mdl::Node& n) {
    const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(&n);
    return entityNode && entityNode->entity().classname() == classname;
  });
  REQUIRE(node);
  return node;
}

std::vector<mdl::Node*> brushesOf(mdl::Node& node)
{
  auto result = std::vector<mdl::Node*>{};
  for (auto* child : node.children())
  {
    if (dynamic_cast<mdl::BrushNode*>(child))
    {
      result.push_back(child);
    }
  }
  return result;
}

mdl::Node* findGroup(mdl::Map& map)
{
  auto* node = findNode(map.worldNode(), [](const mdl::Node& n) {
    return dynamic_cast<const mdl::GroupNode*>(&n) != nullptr;
  });
  REQUIRE(node);
  return node;
}

/** Loads two_rooms.map with the real Quake configuration, so that smart tags exist. */
ui::MapDocument& loadRooms(McpToolFixture& fixture)
{
  const auto* gameInfo = fixture.host().gameManager().gameInfo("Quake");
  REQUIRE(gameInfo);
  return fixture.load(
    getFixtureRoot() / "test" / "mcp" / "maps" / "two_rooms.map",
    {.mapFormat = mdl::MapFormat::Standard, .gameInfo = *gameInfo});
}

std::vector<Json> contentOf(const Json& raw, const std::string& type)
{
  auto result = std::vector<Json>{};
  for (const auto& block : raw["content"])
  {
    if (block["type"] == type)
    {
      result.push_back(block);
    }
  }
  return result;
}

/** The base64 data of the only image of a call result. */
std::string imageData(const Json& raw)
{
  const auto images = contentOf(raw, "image");
  REQUIRE(images.size() == 1);
  return images.front()["data"].get<std::string>();
}

bool hasWarning(const Json& result, const std::string& code)
{
  return result.contains("warnings")
         && std::ranges::any_of(result["warnings"], [&](const auto& warning) {
              return warning["code"] == code;
            });
}

size_t undoCount(ui::MapDocument& document)
{
  return document.map().commandProcessor().undoCommandNames().size();
}

std::string createBox(McpToolFixture& fixture, const Json& min, const Json& max)
{
  return fixture
    .call("brush_create_box", Json{{"min", min}, {"max", max}})["result"]["brush"]
    .get<std::string>();
}

} // namespace

TEST_CASE("SnapshotTools")
{
  auto fixture = McpToolFixture{};
  auto& document = loadRooms(fixture);
  auto& map = document.map();
  auto& renderer = fixture.host().snapshot;

  auto* trigger = findEntity(map, "trigger_once");
  auto* triggerBrush = brushesOf(*trigger).front();
  auto* door = findEntity(map, "func_door");
  auto* player = findEntity(map, "info_player_start");
  auto* ogre = findEntity(map, "monster_ogre");
  auto* group = findGroup(map);
  REQUIRE(triggerBrush);

  const auto initialUndoCount = undoCount(document);
  const auto initialModificationCount = map.modificationCount();

  SECTION("agent_camera_set, agent_camera_get, agent_camera_list, agent_camera_delete")
  {
    SECTION("perspective with lookAt")
    {
      const auto result = fixture.call(
        "agent_camera_set",
        Json{
          {"name", "hall"},
          {"camera",
           Json{{"position", {0, 0, 64}}, {"lookAt", {100, 0, 64}}, {"fov", 70}}}});
      CHECK(result["name"] == "hall");
      CHECK(result["replaced"] == false);
      CHECK(result["camera"]["projection"] == "perspective");
      CHECK(result["camera"]["direction"] == Json::array({1, 0, 0}));
      CHECK(result["camera"]["up"] == Json::array({0, 0, 1}));
      CHECK(result["camera"]["yaw"] == 0);
      CHECK(result["camera"]["pitch"] == 0);
      CHECK(result["camera"]["fov"] == 70);

      const auto got = fixture.call("agent_camera_get", Json{{"name", "hall"}});
      CHECK(got["camera"] == result["camera"]);

      const auto replaced = fixture.call(
        "agent_camera_set",
        Json{{"name", "hall"}, {"camera", Json{{"position", {0, 0, 64}}, {"yaw", 90}}}});
      CHECK(replaced["replaced"] == true);
      CHECK(replaced["camera"]["direction"] == Json::array({0, 1, 0}));

      fixture.call(
        "agent_camera_set",
        Json{
          {"name", "top"},
          {"camera", Json{{"view", "top"}, {"center", {256, 256, 0}}, {"zoom", 0.5}}}});
      const auto list = fixture.call("agent_camera_list");
      REQUIRE(list["cameras"].size() == 2);
      CHECK(list["cameras"][0]["name"] == "hall");
      CHECK(list["cameras"][1]["name"] == "top");
      CHECK(list["cameras"][1]["camera"]["view"] == "top");
      CHECK(list["cameras"][1]["camera"]["zoom"] == 0.5);
      CHECK(list["keptSnapshots"].empty());

      CHECK(
        fixture.call("agent_camera_delete", Json{{"name", "hall"}})["deleted"] == "hall");
      CHECK(
        fixture.callExpectingError("agent_camera_get", Json{{"name", "hall"}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("agent_camera_delete", Json{{"name", "hall"}}).code
        == ErrorCode::InvalidArgument);

      // the cameras belong to the session
      const auto other = fixture.openSession();
      CHECK(
        fixture.callAs(other, "agent_camera_list", Json::object())["cameras"].empty());
    }

    SECTION("eye height")
    {
      const auto result = fixture.call(
        "agent_camera_set",
        Json{
          {"name", "eye"},
          {"camera", Json{{"eyeHeight", {{"point", {256, 256, 100}}}}, {"yaw", 90}}}});
      CHECK(result["camera"]["position"] == Json::array({256, 256, 46}));
      CHECK(result["camera"]["direction"] == Json::array({0, 1, 0}));
      CHECK(result["placement"]["floor"] == 0);
      CHECK(result["placement"]["eyeHeight"] == 46);
      CHECK(result["placement"]["game"] == "quake");

      const auto custom = fixture.call(
        "agent_camera_set",
        Json{
          {"name", "eye"},
          {"camera",
           Json{
             {"eyeHeight", {{"point", {256, 256, 100}}, {"height", 60}}},
             {"lookAt", {256, 400, 60}}}}});
      CHECK(custom["camera"]["position"] == Json::array({256, 256, 60}));
      CHECK(custom["camera"]["direction"] == Json::array({0, 1, 0}));

      const auto error = fixture.callExpectingError(
        "agent_camera_set",
        Json{
          {"name", "eye"}, {"camera", Json{{"eyeHeight", {{"point", {700, 150, 64}}}}}}});
      CHECK(error.code == ErrorCode::InvalidArgument);
      CHECK(!error.hint.empty());
    }

    SECTION("frame and orbit")
    {
      const auto ogreId = fixture.id(*ogre);
      const auto framed = fixture.call(
        "agent_camera_set",
        Json{{"name", "ogre"}, {"camera", Json{{"frame", {{"ids", {ogreId}}}}}}});
      CHECK(framed["camera"]["yaw"] == 45);
      CHECK(framed["camera"]["pitch"] == -30);
      const auto position = vec3FromJson(framed["camera"]["position"]).value();
      const auto center = ogre->logicalBounds().center();
      CHECK(position.x() < center.x());
      CHECK(position.y() < center.y());
      CHECK(position.z() > center.z());

      const auto orbit = fixture.call(
        "agent_camera_set",
        Json{
          {"name", "orbit"},
          {"camera",
           Json{
             {"orbit",
              {{"target", {0, 0, 0}}, {"yaw", 90}, {"pitch", 0}, {"distance", 100}}}}}});
      CHECK(orbit["camera"]["position"] == Json::array({0, -100, 0}));
      CHECK(orbit["camera"]["direction"] == Json::array({0, 1, 0}));

      const auto orthoFrame = fixture.call(
        "agent_camera_set",
        Json{
          {"name", "plan"},
          {"camera",
           Json{
             {"view", "xy"},
             {"frame", {{"box", {{"min", {0, 0, 0}}, {"max", {512, 256, 64}}}}}}}},
          {"width", 512},
          {"height", 512}});
      CHECK(orthoFrame["camera"]["view"] == "top");
      CHECK(orthoFrame["camera"]["zoom"] == Catch::Approx(512.0 / (512.0 * 1.1)));
      CHECK(orthoFrame["camera"]["position"][0] == 256);
      CHECK(orthoFrame["camera"]["position"][1] == 128);
    }

    SECTION("invalid input")
    {
      const auto set = [&](const Json& camera) {
        return fixture
          .callExpectingError("agent_camera_set", Json{{"name", "x"}, {"camera", camera}})
          .code;
      };
      CHECK(
        set(Json{{"position", {0, 0, 0}}, {"lookAt", {1, 0, 0}}, {"yaw", 0}})
        == ErrorCode::InvalidArgument);
      CHECK(
        set(Json{{"position", {0, 0, 0}}, {"lookAt", {0, 0, 0}}})
        == ErrorCode::InvalidArgument);
      CHECK(
        set(Json{{"view", "top"}, {"position", {0, 0, 0}}})
        == ErrorCode::InvalidArgument);
      CHECK(set(Json{{"center", {0, 0, 0}}}) == ErrorCode::InvalidArgument);
      CHECK(
        set(Json{{"orbit", {{"yaw", 0}}}, {"frame", Json::object()}})
        == ErrorCode::InvalidArgument);
      CHECK(set(Json{{"orbit", {{"yaw", 0}}}}) == ErrorCode::InvalidArgument);
      CHECK(set(Json{{"pitch", 100}}) == ErrorCode::InvalidArgument);
      CHECK(
        set(Json{{"frame", {{"ids", {"brush:999999"}}}}}) == ErrorCode::ObjectNotFound);
      CHECK(
        set(Json{{"position", {0, 0, 0}}, {"near", 10}, {"far", 5}})
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "agent_camera_set", Json{{"name", "no spaces"}, {"camera", Json::object()}})
          .code
        == ErrorCode::InvalidArgument);
    }

    // session state only
    CHECK(undoCount(document) == initialUndoCount);
    CHECK(map.modificationCount() == initialModificationCount);
  }

  SECTION("view_snapshot")
  {
    SECTION("default camera and options")
    {
      const auto raw = fixture.callRaw("view_snapshot");
      REQUIRE(raw["isError"] == false);
      const auto& result = raw["structuredContent"];
      CHECK(result["image"]["width"] == 1024);
      CHECK(result["image"]["height"] == 768);
      CHECK(result["image"]["format"] == "png");
      CHECK(result["image"]["savedTo"].is_null());
      CHECK(result["image"]["bytes"].get<size_t>() > 0);
      CHECK(result["cameraName"].is_null());
      CHECK(result["keptAs"].is_null());
      CHECK(result["resourcesPending"] == false);
      CHECK(result["camera"]["projection"] == "perspective");
      CHECK(result["camera"]["yaw"] == 45);
      CHECK(result["counts"]["pointEntities"] == 6);
      CHECK(result["counts"]["brushEntities"] == 2);
      CHECK(result["counts"]["groups"] == 1);
      CHECK(result["counts"]["hiddenFaces"] == 0);

      const auto images = contentOf(raw, "image");
      REQUIRE(images.size() == 1);
      CHECK(images.front()["mimeType"] == "image/png");

      REQUIRE(renderer.requests.size() == 1);
      const auto& request = renderer.last();
      CHECK(request.width == 1024);
      CHECK(request.height == 768);
      CHECK(request.options.faceMode == FaceMode::Textured);
      CHECK(request.options.gridSize == map.grid().actualSize());
      CHECK(request.contains(*triggerBrush));
      CHECK(request.contains(*trigger));
      CHECK(request.contains(*door));
      CHECK(request.contains(*player));
      CHECK(request.contains(*group));
      CHECK(request.highlighted.empty());
      CHECK(!request.hasFaceFilter);

      // read-only: no undo step, no change
      CHECK(undoCount(document) == initialUndoCount);
      CHECK(map.modificationCount() == initialModificationCount);
    }

    SECTION("hiding the trigger tag changes the scene and the image")
    {
      const auto all = imageData(fixture.callRaw("view_snapshot"));
      const auto raw =
        fixture.callRaw("view_snapshot", Json{{"options", {{"hideTags", {"trigger"}}}}});
      REQUIRE(raw["isError"] == false);
      CHECK(!renderer.last().contains(*triggerBrush));
      CHECK(!renderer.last().contains(*trigger));
      CHECK(renderer.last().contains(*door));
      CHECK(raw["structuredContent"]["counts"]["brushEntities"] == 1);
      CHECK(imageData(raw) != all);

      const auto unknown =
        fixture.call("view_snapshot", Json{{"options", {{"hideTags", {"nonsense"}}}}});
      CHECK(hasWarning(unknown, "UNKNOWN_TAG"));
    }

    SECTION("face tags hide faces")
    {
      auto* brush = brushesOf(*map.worldNode().defaultLayer()).front();
      const auto brushId = fixture.id(*brush);
      fixture.call(
        "material_apply",
        Json{{"material", "clip"}, {"ids", Json::array({brushId + "/face:0"})}});
      const auto clipBrush = createBox(fixture, {2000, 2000, 0}, {2064, 2064, 64});
      fixture.call("material_apply", Json{{"material", "clip"}, {"ids", {clipBrush}}});

      const auto result =
        fixture.call("view_snapshot", Json{{"options", {{"hideTags", {"clip"}}}}});
      const auto& request = renderer.last();
      CHECK(request.hasFaceFilter);
      const auto faces = request.facesOf(*brush);
      REQUIRE(faces.has_value());
      CHECK(faces->size() == 5);
      CHECK(std::ranges::find(*faces, size_t(0)) == faces->end());
      // a brush whose faces are all hidden is not drawn
      CHECK(!request.contains(*fixture.node(clipBrush)));
      CHECK(result["counts"]["hiddenFaces"] == 1);

      fixture.call("view_snapshot");
      CHECK(renderer.last().facesOf(*brush)->size() == 6);
      CHECK(renderer.last().contains(*fixture.node(clipBrush)));
    }

    SECTION("classnames and object kinds")
    {
      fixture.call("view_snapshot", Json{{"options", {{"hideClassnames", {"LIGHT*"}}}}});
      CHECK(!renderer.last().contains(*findEntity(map, "light")));
      CHECK(renderer.last().contains(*player));

      fixture.call("view_snapshot", Json{{"options", {{"pointEntities", false}}}});
      CHECK(!renderer.last().contains(*player));
      CHECK(!renderer.last().contains(*ogre));
      CHECK(renderer.last().contains(*triggerBrush));

      fixture.call("view_snapshot", Json{{"options", {{"brushEntities", false}}}});
      CHECK(!renderer.last().contains(*door));
      CHECK(!renderer.last().contains(*triggerBrush));
      CHECK(renderer.last().contains(*player));

      fixture.call("view_snapshot", Json{{"options", {{"brushes", false}}}});
      CHECK(std::ranges::none_of(renderer.last().nodes, [](const auto* node) {
        return dynamic_cast<const mdl::BrushNode*>(node) != nullptr;
      }));
      CHECK(renderer.last().contains(*player));

      fixture.call(
        "view_snapshot", Json{{"options", {{"hideClassnames", {"worldspawn"}}}}});
      CHECK(
        !renderer.last().contains(*brushesOf(*map.worldNode().defaultLayer()).front()));
      CHECK(renderer.last().contains(*triggerBrush));
    }

    SECTION("isolate and highlight")
    {
      const auto groupId = fixture.id(*group);
      const auto result = fixture.call(
        "view_snapshot",
        Json{
          {"isolate", {groupId, fixture.id(*ogre)}},
          {"highlight", {{"ids", {fixture.id(*ogre)}}, {"color", {0, 1, 0}}}}});
      const auto& request = renderer.last();
      CHECK(request.contains(*group));
      for (auto* brush : brushesOf(*group))
      {
        CHECK(request.contains(*brush));
      }
      CHECK(request.contains(*ogre));
      CHECK(!request.contains(*player));
      CHECK(!request.contains(*triggerBrush));
      CHECK(request.highlighted == std::vector<const mdl::Node*>{ogre});
      CHECK(request.highlightColor.to<RgbaF>() == RgbaF{0.0f, 1.0f, 0.0f, 1.0f});
      CHECK(result["counts"]["highlighted"] == 1);
      CHECK(result["counts"]["pointEntities"] == 1);

      CHECK(
        fixture.callExpectingError("view_snapshot", Json{{"isolate", {"brush:999999"}}})
          .code
        == ErrorCode::ObjectNotFound);
    }

    SECTION("hidden objects and the user's view filters")
    {
      fixture.call(
        "visibility_set", Json{{"mode", "hide"}, {"ids", {fixture.id(*ogre)}}});
      fixture.call("view_snapshot");
      CHECK(!renderer.last().contains(*ogre));
      CHECK(renderer.last().contains(*player));

      fixture.call("view_snapshot", Json{{"options", {{"includeHidden", true}}}});
      CHECK(renderer.last().contains(*ogre));

      // isolating a hidden object draws it
      fixture.call("view_snapshot", Json{{"isolate", {fixture.id(*ogre)}}});
      CHECK(renderer.last().contains(*ogre));

      // the user's filters do not apply to snapshots
      map.editorContext().setShowPointEntities(false);
      map.editorContext().setHiddenTags(triggerBrush->tagMask());
      fixture.call("view_snapshot");
      CHECK(renderer.last().contains(*player));
      CHECK(renderer.last().contains(*triggerBrush));
      map.editorContext().setShowPointEntities(true);
      map.editorContext().setHiddenTags(0);
    }

    SECTION("cameras")
    {
      fixture.call(
        "agent_camera_set",
        Json{{"name", "hall"}, {"camera", Json{{"position", {1, 2, 3}}, {"yaw", 90}}}});
      const auto named = fixture.call("view_snapshot", Json{{"camera", "hall"}});
      CHECK(named["cameraName"] == "hall");
      CHECK(renderer.last().camera.position == vm::vec3d{1, 2, 3});
      CHECK(renderer.last().camera.projection == CameraProjection::Perspective);

      for (const auto& [view, direction] :
           {std::pair{"xy", vm::vec3d{0, 0, -1}},
            std::pair{"xz", vm::vec3d{0, 1, 0}},
            std::pair{"yz", vm::vec3d{-1, 0, 0}}})
      {
        const auto result =
          fixture.call("view_snapshot", Json{{"camera", {{"view", view}}}});
        CHECK(renderer.last().camera.projection == CameraProjection::Orthographic);
        CHECK(renderer.last().camera.direction == direction);
        CHECK(result["camera"]["projection"] == "orthographic");
      }

      fixture.call(
        "view_snapshot",
        Json{{"camera", {{"view", "top"}, {"center", {100, 200, 0}}, {"zoom", 2}}}});
      CHECK(renderer.last().camera.zoom == 2);
      CHECK(renderer.last().camera.position.x() == 100);
      CHECK(renderer.last().camera.position.y() == 200);

      CHECK(
        fixture.callExpectingError("view_snapshot", Json{{"camera", "missing"}}).code
        == ErrorCode::InvalidArgument);
      CHECK(renderer.requests.size() == 5);
    }

    SECTION("size, options and formats")
    {
      fixture.call(
        "view_snapshot",
        Json{
          {"width", 320},
          {"height", 200},
          {"options",
           {{"faceMode", "wireframe"},
            {"fog", true},
            {"grid", true},
            {"gridSize", 64},
            {"axes", true},
            {"shading", false},
            {"background", {0, 0, 1}}}}});
      const auto& request = renderer.last();
      CHECK(request.width == 320);
      CHECK(request.height == 200);
      CHECK(request.options.faceMode == FaceMode::Wireframe);
      CHECK(request.options.fog);
      CHECK(request.options.grid);
      CHECK(request.options.gridSize == 64);
      CHECK(request.options.axes);
      CHECK(!request.options.shading);
      CHECK(request.options.background.has_value());

      CHECK(
        fixture.callExpectingError("view_snapshot", Json{{"width", 4096}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("view_snapshot", Json{{"height", 8}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("view_snapshot", Json{{"options", {{"fog", 1}}}}).code
        == ErrorCode::InvalidArgument);

      SECTION("jpeg")
      {
        const auto png = fixture.call("view_snapshot", Json{{"format", "jpeg"}});
        CHECK(png["image"]["format"] == "png");
        CHECK(hasWarning(png, "JPEG_UNSUPPORTED"));

        renderer.supportsJpeg = true;
        const auto raw =
          fixture.callRaw("view_snapshot", Json{{"format", "jpeg"}, {"quality", 50}});
        CHECK(raw["structuredContent"]["image"]["format"] == "jpeg");
        CHECK(contentOf(raw, "image").front()["mimeType"] == "image/jpeg");
      }
    }

    SECTION("saveTo")
    {
      auto env = fs::TestEnvironment{};
      const auto path = env.dir() / "shot.png";
      const auto result =
        fixture.call("view_snapshot", Json{{"saveTo", path.string()}, {"width", 64}});
      CHECK(result["image"]["savedTo"] == path.string());
      REQUIRE(std::filesystem::exists(path));
      CHECK(std::filesystem::file_size(path) == result["image"]["bytes"].get<size_t>());

      CHECK(
        fixture.callExpectingError("view_snapshot", Json{{"saveTo", path.string()}}).code
        == ErrorCode::FileExists);
      fixture.call("view_snapshot", Json{{"saveTo", path.string()}, {"overwrite", true}});

      CHECK(
        fixture.callExpectingError("view_snapshot", Json{{"saveTo", "relative.png"}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "view_snapshot", Json{{"saveTo", (env.dir() / "missing" / "x.png").string()}})
          .code
        == ErrorCode::IoError);
    }

    SECTION("waits for resources")
    {
      renderer.pendingChecks = 2;
      const auto ready = fixture.call("view_snapshot");
      CHECK(ready["resourcesPending"] == false);
      CHECK(renderer.resourceChecks == 3);
      CHECK(!hasWarning(ready, "RESOURCES_LOADING"));

      renderer.pendingChecks = 1000;
      const auto pending = fixture.call("view_snapshot");
      CHECK(pending["resourcesPending"] == true);
      CHECK(hasWarning(pending, "RESOURCES_LOADING"));
      CHECK(renderer.requests.size() == 2);
    }

    SECTION("render failure")
    {
      renderer.renderError = "no context";
      const auto error = fixture.callExpectingError("view_snapshot");
      CHECK(error.code == ErrorCode::OperationFailed);
      CHECK(error.message.find("no context") != std::string::npos);
    }
  }

  SECTION("view_snapshots_around")
  {
    const auto doorId = fixture.id(*door);

    SECTION("default views")
    {
      const auto raw = fixture.callRaw("view_snapshots_around", Json{{"ids", {doorId}}});
      REQUIRE(raw["isError"] == false);
      const auto& result = raw["structuredContent"];
      REQUIRE(result["images"].size() == 5);
      CHECK(result["images"][0]["label"] == "from north");
      CHECK(result["images"][4]["label"] == "top (orthographic)");
      CHECK(result["images"][4]["camera"]["view"] == "top");
      CHECK(result["target"] == toJson(door->logicalBounds()));

      // north: the camera is north of the door, looking south and slightly down
      const auto north = vec3FromJson(result["images"][0]["camera"]["position"]).value();
      CHECK(north.y() > door->logicalBounds().max.y());
      CHECK(result["images"][0]["camera"]["yaw"] == -90);
      CHECK(result["images"][0]["camera"]["pitch"] == -20);

      REQUIRE(renderer.requests.size() == 5);
      CHECK(renderer.requests[0].width == 640);
      CHECK(renderer.requests[0].height == 480);

      // a label before each image
      const auto& content = raw["content"];
      REQUIRE(content.size() == 11);
      for (size_t i = 0; i < 5; ++i)
      {
        CHECK(content[1 + i * 2]["type"] == "text");
        CHECK(content[1 + i * 2]["text"] == result["images"][i]["label"]);
        CHECK(content[2 + i * 2]["type"] == "image");
      }
    }

    SECTION("custom views, box and options")
    {
      fixture.call(
        "agent_camera_set",
        Json{{"name", "hall"}, {"camera", Json{{"position", {1, 2, 3}}, {"yaw", 90}}}});
      const auto result = fixture.call(
        "view_snapshots_around",
        Json{
          {"box", {{"min", {0, 0, 0}}, {"max", {512, 512, 256}}}},
          {"views",
           {"above",
            Json{{"label", "hall camera"}, {"camera", "hall"}},
            Json{{"label", "side"}, {"camera", {{"view", "yz"}}}}}},
          {"width", 200},
          {"height", 100},
          {"options", {{"hideTags", {"trigger"}}}}});
      REQUIRE(result["images"].size() == 3);
      CHECK(result["images"][1]["label"] == "hall camera");
      CHECK(result["images"][1]["camera"]["position"] == Json::array({1, 2, 3}));
      CHECK(result["images"][0]["camera"]["pitch"] == -90);
      CHECK(result["images"][2]["camera"]["view"] == "side");
      for (const auto& request : renderer.requests)
      {
        CHECK(!request.contains(*triggerBrush));
        CHECK(request.width == 200);
      }
    }

    SECTION("progress and cancellation")
    {
      auto stream = fixture.post(
        fixture.sessionId(),
        jsonrpc::makeRequest(
          900,
          "tools/call",
          Json{
            {"name", "view_snapshots_around"},
            {"arguments", Json{{"ids", {doorId}}}},
            {"_meta", Json{{"progressToken", "around"}}},
          }));
      CHECK(!stream->response.has_value());

      // cancel while the second image renders
      renderer.onRender = [&](const auto&) {
        if (renderer.requests.size() == 2)
        {
          fixture.post(
            fixture.sessionId(),
            jsonrpc::makeNotification(
              "notifications/cancelled", Json{{"requestId", 900}}));
        }
      };
      fixture.scheduler().runPending();

      REQUIRE(stream->response.has_value());
      const auto& result = (*stream->response)["result"];
      CHECK(result["isError"] == true);
      CHECK(result["structuredContent"]["error"]["code"] == "CANCELLED");
      CHECK(renderer.requests.size() == 2);
      REQUIRE(stream->notifications.size() == 2);
      CHECK(stream->notifications[0]["method"] == "notifications/progress");
      CHECK(stream->notifications[1]["params"]["progress"] == 1);
      CHECK(stream->notifications[1]["params"]["total"] == 5);
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture
          .callExpectingError(
            "view_snapshots_around", Json{{"views", {"north", "upside-down"}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "view_snapshots_around",
            Json{{"ids", {doorId}}, {"box", {{"min", {0, 0, 0}}, {"max", {1, 1, 1}}}}})
          .code
        == ErrorCode::InvalidArgument);
      auto many = Json::array();
      for (size_t i = 0; i < 13; ++i)
      {
        many.push_back("north");
      }
      CHECK(
        fixture.callExpectingError("view_snapshots_around", Json{{"views", many}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError("view_snapshots_around", Json{{"views", Json::array()}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(renderer.requests.empty());
    }
  }

  SECTION("map_plan_view image form")
  {
    SECTION("text is the default and does not render")
    {
      const auto plan = fixture.call("map_plan_view", Json{{"height", 64}});
      CHECK(plan.contains("text"));
      CHECK(!plan.contains("image"));
      CHECK(renderer.requests.empty());
    }

    SECTION("image")
    {
      const auto raw = fixture.callRaw(
        "map_plan_view", Json{{"height", 64}, {"format", "image"}, {"floorDepth", 256}});
      REQUIRE(raw["isError"] == false);
      const auto& plan = raw["structuredContent"];
      CHECK(!plan.contains("text"));
      CHECK(!plan.contains("legend"));
      CHECK(plan["cellSize"] == 32);
      CHECK(plan["entities"].size() == 6);
      // 42 x 18 cells at 16 pixels each
      CHECK(plan["image"]["width"] == 672);
      CHECK(plan["image"]["height"] == 288);
      CHECK(contentOf(raw, "image").size() == 1);

      const auto& request = renderer.last();
      CHECK(request.camera.projection == CameraProjection::Orthographic);
      CHECK(request.camera.direction == vm::vec3d{0, 0, -1});
      CHECK(request.camera.up == vm::vec3d{0, 1, 0});
      CHECK(request.camera.position.z() == 64);
      CHECK(request.camera.nearPlane == 0);
      CHECK(request.camera.farPlane == 256);
      CHECK(request.camera.zoom == Catch::Approx(0.5));
      CHECK(request.camera.position.x() == Catch::Approx(-32 + 42 * 32 / 2.0));
      REQUIRE(request.markers.size() == 6);
      const auto playerMarker = std::ranges::find_if(
        request.markers, [](const auto& marker) { return marker.label == "P"; });
      REQUIRE(playerMarker != request.markers.end());
      CHECK(playerMarker->position == vm::vec3d{64, 64, 24});
      CHECK(playerMarker->color.to<RgbaF>() == RgbaF{0.2f, 1.0f, 0.2f, 1.0f});
    }

    SECTION("both, with an image size")
    {
      const auto plan = fixture.call(
        "map_plan_view",
        Json{
          {"height", 64},
          {"format", "both"},
          {"imageWidth", 400},
          {"imageHeight", 300},
          {"showEntities", false}});
      CHECK(plan.contains("text"));
      CHECK(plan["image"]["width"] == 400);
      CHECK(plan["image"]["height"] == 300);
      CHECK(renderer.last().markers.empty());
      CHECK(renderer.last().width == 400);
    }

    SECTION("unsupported host")
    {
      fixture.host().supportsSnapshots = false;
      CHECK(
        fixture.callExpectingError("map_plan_view", Json{{"format", "image"}}).code
        == ErrorCode::UnsupportedInHost);
      CHECK(fixture.call("map_plan_view").contains("text"));
    }
  }

  SECTION("view_snapshot_compare")
  {
    SECTION("kept snapshots")
    {
      const auto kept = fixture.call(
        "view_snapshot", Json{{"keepAs", "before"}, {"width", 256}, {"height", 128}});
      CHECK(kept["keptAs"] == "before");
      CHECK(fixture.call("agent_camera_list")["keptSnapshots"].size() == 1);

      const auto beforeCamera = renderer.last().camera;
      createBox(fixture, {100, 100, 0}, {164, 164, 64});

      const auto raw =
        fixture.callRaw("view_snapshot_compare", Json{{"before", "before"}});
      REQUIRE(raw["isError"] == false);
      const auto& result = raw["structuredContent"];
      CHECK(result["mode"] == "kept");
      CHECK(result["before"] == "before");
      CHECK(result["after"].is_null());
      CHECK(result["width"] == 256);
      CHECK(result["changedPixels"].get<size_t>() > 0);
      CHECK(result["changedRatio"].get<double>() > 0.0);
      CHECK(result["changedBounds"]["width"].get<size_t>() > 0);
      // re-rendered with the kept camera
      CHECK(renderer.last().camera == beforeCamera);
      CHECK(renderer.last().width == 256);

      const auto images = contentOf(raw, "image");
      REQUIRE(images.size() == 2);
      const auto texts = contentOf(raw, "text");
      CHECK(texts.size() == 3);

      // two kept snapshots, nothing rendered
      fixture.call(
        "view_snapshot", Json{{"keepAs", "after"}, {"width", 256}, {"height", 128}});
      const auto requests = renderer.requests.size();
      const auto both = fixture.call(
        "view_snapshot_compare", Json{{"before", "before"}, {"after", "after"}});
      CHECK(both["after"] == "after");
      CHECK(both["changedPixels"].get<size_t>() > 0);
      CHECK(renderer.requests.size() == requests);

      const auto same = fixture.call(
        "view_snapshot_compare", Json{{"before", "after"}, {"after", "after"}});
      CHECK(same["changedPixels"] == 0);
      CHECK(same["changedBounds"].is_null());

      fixture.call("view_snapshot", Json{{"keepAs", "big"}});
      CHECK(
        fixture
          .callExpectingError(
            "view_snapshot_compare", Json{{"before", "before"}, {"after", "big"}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("view_snapshot_compare", Json{{"before", "nope"}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "view_snapshot_compare", Json{{"before", "before"}, {"camera", "x"}})
          .code
        == ErrorCode::InvalidArgument);
    }

    SECTION("keeps a bounded number of snapshots")
    {
      for (size_t i = 0; i < Session::MaxKeptSnapshots; ++i)
      {
        fixture.call(
          "view_snapshot",
          Json{{"keepAs", "s" + std::to_string(i)}, {"width", 16}, {"height", 16}});
      }
      const auto dropped = fixture.call(
        "view_snapshot", Json{{"keepAs", "last"}, {"width", 16}, {"height", 16}});
      CHECK(hasWarning(dropped, "SNAPSHOT_DROPPED"));
      const auto list = fixture.call("agent_camera_list")["keptSnapshots"];
      CHECK(list.size() == Session::MaxKeptSnapshots);
      CHECK(list.front()["name"] == "s1");
      CHECK(list.back()["name"] == "last");
    }

    SECTION("undo steps")
    {
      const auto first = createBox(fixture, {100, 100, 0}, {164, 164, 64});
      const auto second = createBox(fixture, {300, 100, 0}, {364, 164, 64});
      fixture.call("undo");
      // the second box is on the redo stack

      const auto& commandProcessor = map.commandProcessor();
      const auto undoNames = commandProcessor.undoCommandNames();
      const auto redoNames = commandProcessor.redoCommandNames();
      const auto modificationCount = map.modificationCount();
      REQUIRE(redoNames.size() == 1);

      const auto result = fixture.call(
        "view_snapshot_compare", Json{{"undoSteps", 1}, {"width", 128}, {"height", 96}});
      CHECK(result["mode"] == "undo");
      CHECK(result["undoSteps"] == 1);
      CHECK(result["undone"] == Json::array({undoNames.front()}));
      CHECK(result["historyRestored"] == true);
      CHECK(result["changedPixels"].get<size_t>() > 0);

      REQUIRE(renderer.requests.size() == 2);
      auto* firstNode = fixture.node(first);
      REQUIRE(firstNode);
      // after, then before, with the same camera
      CHECK(renderer.requests[0].contains(*firstNode));
      CHECK(!renderer.requests[1].contains(*firstNode));
      CHECK(renderer.requests[0].camera == renderer.requests[1].camera);

      // the document and its history are unchanged
      CHECK(commandProcessor.undoCommandNames() == undoNames);
      CHECK(commandProcessor.redoCommandNames() == redoNames);
      CHECK(map.modificationCount() == modificationCount);
      CHECK(fixture.node(first) == firstNode);
      (void)second;
    }

    SECTION("undo steps are refused while a transaction is open")
    {
      createBox(fixture, {100, 100, 0}, {164, 164, 64});
      fixture.call("transaction_begin", Json{{"name", "work"}});
      CHECK(
        fixture.callExpectingError("view_snapshot_compare", Json{{"undoSteps", 1}}).code
        == ErrorCode::TransactionActive);
      fixture.call("transaction_rollback");

      map.startTransaction("user drag", mdl::TransactionScope::LongRunning);
      CHECK(
        fixture.callExpectingError("view_snapshot_compare", Json{{"undoSteps", 1}}).code
        == ErrorCode::TransactionActive);
      map.cancelTransaction();

      CHECK(renderer.requests.empty());
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture
          .callExpectingError(
            "view_snapshot_compare", Json{{"undoSteps", int(initialUndoCount) + 1}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("view_snapshot_compare").code
        == ErrorCode::InvalidArgument);
      fixture.call("view_snapshot", Json{{"keepAs", "a"}});
      CHECK(
        fixture
          .callExpectingError(
            "view_snapshot_compare", Json{{"before", "a"}, {"undoSteps", 1}})
          .code
        == ErrorCode::InvalidArgument);
    }
  }

  SECTION("view_snapshot_user")
  {
    auto camera = AgentCamera{};
    camera.position = vm::vec3d{10, 20, 30};
    renderer.userViewList = {
      UserView{"3d", true, 800, 600, camera},
      UserView{"xy", false, 400, 300, AgentCamera{}},
    };
    renderer.captures = {{"3d", makeImage(8, 6, Rgba8{1, 2, 3, 255})}};

    SECTION("captures a view")
    {
      const auto raw = fixture.callRaw("view_snapshot_user");
      REQUIRE(raw["isError"] == false);
      const auto& result = raw["structuredContent"];
      CHECK(result["view"] == "3d");
      CHECK(result["views"].size() == 2);
      CHECK(result["views"][1]["visible"] == false);
      CHECK(result["camera"]["position"] == Json::array({10, 20, 30}));
      CHECK(result["image"]["width"] == 8);
      CHECK(result["image"]["height"] == 6);
      CHECK(contentOf(raw, "image").size() == 1);
      CHECK(renderer.capturedViews == std::vector<std::string>{"3d"});
    }

    SECTION("lists the views")
    {
      const auto raw = fixture.callRaw("view_snapshot_user", Json{{"listOnly", true}});
      CHECK(raw["structuredContent"]["views"].size() == 2);
      CHECK(raw["structuredContent"]["view"].is_null());
      CHECK(contentOf(raw, "image").empty());
      CHECK(renderer.capturedViews.empty());
    }

    SECTION("errors")
    {
      CHECK(
        fixture.callExpectingError("view_snapshot_user", Json{{"view", "xy"}}).code
        == ErrorCode::OperationFailed);
      CHECK(
        fixture.callExpectingError("view_snapshot_user", Json{{"view", "yz"}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("view_snapshot_user", Json{{"view", "4d"}}).code
        == ErrorCode::InvalidArgument);
      renderer.userViewList.clear();
      CHECK(
        fixture.callExpectingError("view_snapshot_user").code
        == ErrorCode::OperationFailed);
    }
  }

  SECTION("unsupported host")
  {
    createBox(fixture, {100, 100, 0}, {164, 164, 64});
    fixture.host().supportsSnapshots = false;
    for (const auto& [tool, args] :
         {std::pair{"view_snapshot", Json::object()},
          std::pair{"view_snapshots_around", Json::object()},
          std::pair{"view_snapshot_user", Json::object()},
          std::pair{"view_snapshot_compare", Json{{"undoSteps", 1}}}})
    {
      const auto error = fixture.callExpectingError(tool, args);
      CHECK(error.code == ErrorCode::UnsupportedInHost);
      CHECK(!error.hint.empty());
    }
  }

  SECTION("read-only tools")
  {
    for (const auto* tool :
         {"agent_camera_set",
          "agent_camera_get",
          "agent_camera_list",
          "agent_camera_delete",
          "view_snapshot",
          "view_snapshots_around",
          "view_snapshot_compare",
          "view_snapshot_user"})
    {
      const auto* def = fixture.server().tools().find(tool);
      REQUIRE(def);
      CHECK(def->mutation() == Mutation::None);
    }
  }
}

} // namespace tb::mcp
