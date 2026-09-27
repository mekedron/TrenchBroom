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
#include "mcp/McpToolFixture.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include "vm/bbox.h"

#include <functional>
#include <string>

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
  return findNode(map.worldNode(), [&](const mdl::Node& node) {
    const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(&node);
    return entityNode && entityNode->entity().classname() == classname;
  });
}

mdl::Node* findBrush(mdl::Map& map, const vm::bbox3d& bounds)
{
  return findNode(map.worldNode(), [&](const mdl::Node& node) {
    return dynamic_cast<const mdl::BrushNode*>(&node) && node.logicalBounds() == bounds;
  });
}

bool containsId(const Json& list, const std::string& id, const std::string& key = "id")
{
  for (const auto& item : list)
  {
    if (item[key] == id)
    {
      return true;
    }
  }
  return false;
}

char cellAt(const Json& plan, const double x, const double y)
{
  const auto cellSize = plan["cellSize"].get<double>();
  const auto originX = plan["origin"][0].get<double>();
  const auto originY = plan["origin"][1].get<double>();
  const auto rows = plan["rows"].get<size_t>();
  const auto column = size_t((x - originX) / cellSize);
  const auto row = rows - 1 - size_t((y - originY) / cellSize);

  // skip the header line and the row label
  auto lines = std::vector<std::string>{};
  auto text = plan["text"].get<std::string>();
  for (size_t start = 0, end = 0; start <= text.size(); start = end + 1)
  {
    end = text.find('\n', start);
    if (end == std::string::npos)
    {
      end = text.size();
    }
    lines.push_back(text.substr(start, end - start));
  }
  const auto& line = lines.at(row + 1);
  return line.at(line.find('|') + 1 + column);
}

} // namespace

TEST_CASE("SpatialTools")
{
  auto fixture = McpToolFixture{};
  auto& document = fixture.load(
    getFixtureRoot() / "test" / "mcp" / "maps" / "two_rooms.map",
    {.mapFormat = mdl::MapFormat::Standard, .gameInfo = mdl::QuakeGameInfo});
  auto& map = document.map();

  auto* arenaFloor = findBrush(map, {{768, -16, -16}, {1296, 528, 0}});
  auto* westWall = findBrush(map, {{-16, -16, 0}, {0, 528, 256}});
  auto* roomAFloor = findBrush(map, {{-16, -16, -16}, {528, 528, 0}});
  auto* roomACeiling = findBrush(map, {{-16, -16, 256}, {528, 528, 272}});
  auto* pillar = findBrush(map, {{192, 192, 0}, {224, 224, 256}});
  auto* triggerBrush = findBrush(map, {{592, 224, 0}, {656, 288, 128}});
  auto* ogre = findEntity(map, "monster_ogre");
  auto* light = findNode(map.worldNode(), [](const mdl::Node& node) {
    const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(&node);
    return entityNode && entityNode->entity().classname() == "light"
           && entityNode->entity().origin() == vm::vec3d{256, 256, 200};
  });
  auto* pillars = findNode(map.worldNode(), [](const mdl::Node& node) {
    return dynamic_cast<const mdl::GroupNode*>(&node) != nullptr;
  });
  REQUIRE(arenaFloor);
  REQUIRE(westWall);
  REQUIRE(roomAFloor);
  REQUIRE(roomACeiling);
  REQUIRE(pillar);
  REQUIRE(triggerBrush);
  REQUIRE(ogre);
  REQUIRE(light);
  REQUIRE(pillars);

  SECTION("ray_pick")
  {
    SECTION("from an entity straight down")
    {
      const auto result = fixture.call("ray_pick", {{"from", fixture.id(*ogre)}});
      const auto& hit = result["hit"];
      REQUIRE(hit.is_object());
      CHECK(hit["object"] == fixture.id(*arenaFloor));
      CHECK(hit["kind"] == "brush");
      CHECK(hit["material"] == "floor_metal");
      CHECK(
        hit["face"].get<std::string>().starts_with(fixture.id(*arenaFloor) + "/face:"));
      CHECK(hit["normal"] == Json::array({0, 0, 1}));
      CHECK(hit["point"][2] == 0);
      CHECK(
        hit["distance"].get<double>()
        == Catch::Approx(ogre->logicalBounds().center().z()));
      CHECK(!hit.contains("entity"));
    }

    SECTION("explicit origin and direction")
    {
      const auto result = fixture.call(
        "ray_pick", {{"origin", {256, 100, 128}}, {"direction", {-2, 0, 0}}});
      const auto& hit = result["hit"];
      CHECK(result["direction"] == Json::array({-1, 0, 0}));
      CHECK(hit["object"] == fixture.id(*westWall));
      CHECK(hit["material"] == "wall_brick");
      CHECK(hit["normal"] == Json::array({1, 0, 0}));
      CHECK(hit["distance"] == 256);
      CHECK(hit["point"] == Json::array({0, 100, 128}));
    }

    SECTION("brush of a brush entity")
    {
      const auto result =
        fixture.call("ray_pick", {{"origin", {700, 256, 64}}, {"direction", {-1, 0, 0}}});
      const auto& hit = result["hit"];
      CHECK(hit["object"] == fixture.id(*triggerBrush));
      CHECK(hit["classname"] == "trigger_once");
      CHECK(hit["entity"].get<std::string>().starts_with("entity:"));
      CHECK(hit["distance"] == 44);
    }

    SECTION("miss")
    {
      const auto result = fixture.call(
        "ray_pick", {{"origin", {-100, -100, 500}}, {"direction", {0, 0, 1}}});
      CHECK(result["hit"].is_null());
    }

    SECTION("maxDistance")
    {
      const auto result = fixture.call(
        "ray_pick",
        {{"origin", {256, 100, 128}}, {"direction", {-1, 0, 0}}, {"maxDistance", 100}});
      CHECK(result["hit"].is_null());
    }

    SECTION("ignore")
    {
      const auto result = fixture.call(
        "ray_pick", {{"origin", {100, 100, 128}}, {"ignore", {fixture.id(*roomAFloor)}}});
      CHECK(result["hit"].is_null());
    }

    SECTION("kinds and all")
    {
      const auto upwards = Json{{"origin", {256, 256, 128}}, {"direction", {0, 0, 1}}};

      const auto first = fixture.call("ray_pick", upwards);
      CHECK(first["hit"]["object"] == fixture.id(*light));
      CHECK(first["hit"]["kind"] == "entity");
      CHECK(first["hit"]["label"] == "light");
      CHECK(!first.contains("hits"));

      auto brushesOnly = upwards;
      brushesOnly["kinds"] = {"brush"};
      const auto brush = fixture.call("ray_pick", brushesOnly);
      CHECK(brush["hit"]["object"] == fixture.id(*roomACeiling));
      CHECK(brush["hit"]["point"][2] == 256);

      auto all = upwards;
      all["all"] = true;
      const auto allHits = fixture.call("ray_pick", all);
      REQUIRE(allHits["hits"].size() == 2);
      CHECK(allHits["hits"][0]["object"] == fixture.id(*light));
      CHECK(allHits["hits"][1]["object"] == fixture.id(*roomACeiling));

      all["limit"] = 1;
      const auto limited = fixture.call("ray_pick", all);
      CHECK(limited["hits"].size() == 1);
      CHECK(limited["truncated"] == true);
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture
          .callExpectingError(
            "ray_pick", {{"origin", {0, 0, 0}}, {"direction", {0, 0, 0}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("ray_pick", Json::object()).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "ray_pick", {{"origin", {0, 0, 0}}, {"from", fixture.id(*ogre)}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("ray_pick", {{"from", "entity:999999"}}).code
        == ErrorCode::ObjectNotFound);
      CHECK(
        fixture
          .callExpectingError(
            "ray_pick", {{"origin", {0, 0, 0}}, {"ignore", {"brush:999999"}}})
          .code
        == ErrorCode::ObjectNotFound);
      CHECK(
        fixture
          .callExpectingError("ray_pick", {{"origin", {0, 0, 0}}, {"kinds", {"layer"}}})
          .code
        == ErrorCode::InvalidArgument);
    }
  }

  SECTION("objects_at_point")
  {
    SECTION("inside a pillar")
    {
      const auto result = fixture.call("objects_at_point", {{"point", {208, 208, 64}}});
      REQUIRE(result["containing"].size() == 1);
      CHECK(result["containing"][0]["id"] == fixture.id(*pillar));
      CHECK(result["containing"][0]["relation"] == "inside");
      CHECK(containsId(result["owners"], fixture.id(*pillars)));
      CHECK(result["insideSolid"] == true);
      CHECK(result["count"] == 1);
    }

    SECTION("open air")
    {
      const auto result = fixture.call("objects_at_point", {{"point", {100, 300, 128}}});
      CHECK(result["containing"].empty());
      CHECK(result["insideSolid"] == false);
    }

    SECTION("on a surface")
    {
      const auto result = fixture.call("objects_at_point", {{"point", {100, 300, 0}}});
      REQUIRE(result["containing"].size() == 1);
      CHECK(result["containing"][0]["id"] == fixture.id(*roomAFloor));
      CHECK(result["containing"][0]["relation"] == "touching");
      CHECK(result["insideSolid"] == false);

      const auto withTolerance =
        fixture.call("objects_at_point", {{"point", {100, 300, 0.5}}, {"tolerance", 1}});
      CHECK(withTolerance["containing"][0]["relation"] == "touching");
    }

    SECTION("inside a trigger")
    {
      const auto result = fixture.call("objects_at_point", {{"point", {600, 256, 64}}});
      REQUIRE(result["containing"].size() == 1);
      CHECK(result["containing"][0]["id"] == fixture.id(*triggerBrush));
      CHECK(result["owners"][0]["label"] == "trigger_once");
      CHECK(result["insideSolid"] == false);
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("objects_at_point", {{"point", {0, 0}}}).code
        == ErrorCode::InvalidArgument);
    }
  }

  SECTION("space_check")
  {
    SECTION("free space")
    {
      const auto result = fixture.call(
        "space_check", {{"box", {{"min", {96, 96, 0}}, {"max", {128, 128, 56}}}}});
      CHECK(result["free"] == true);
      CHECK(result["overlaps"].empty());
      CHECK(result["floor"]["z"] == 0);
      CHECK(result["floor"]["distance"] == 0);
      CHECK(result["floor"]["object"] == fixture.id(*roomAFloor));
      CHECK(result["floor"]["material"] == "floor_stone");
      CHECK(result["floor"]["supportedCorners"] == 4);
      CHECK(result["ceiling"]["z"] == 256);
      CHECK(result["ceiling"]["object"] == fixture.id(*roomACeiling));
      CHECK(result["clearance"] == 256);
      CHECK(result["insideWorldBounds"] == true);
    }

    SECTION("overlapping a pillar")
    {
      const auto result = fixture.call(
        "space_check", {{"box", {{"min", {180, 180, 8}}, {"max", {210, 210, 64}}}}});
      CHECK(result["free"] == false);
      REQUIRE(result["overlaps"].size() == 1);
      CHECK(result["overlaps"][0]["id"] == fixture.id(*pillar));
      CHECK(
        result["overlaps"][0]["overlap"]
        == Json{{"min", {192, 192, 8}}, {"max", {210, 210, 64}}});
      CHECK(result["floor"]["distance"] == 8);
      CHECK(result["floor"]["supportedCorners"] == 0);

      const auto ignored = fixture.call(
        "space_check",
        {{"box", {{"min", {180, 180, 8}}, {"max", {210, 210, 64}}}},
         {"ignore", {fixture.id(*pillars)}}});
      CHECK(ignored["free"] == true);
    }

    SECTION("touching is free")
    {
      const auto result = fixture.call(
        "space_check", {{"box", {{"min", {160, 192, 0}}, {"max", {192, 224, 256}}}}});
      CHECK(result["free"] == true);
      CHECK(result["clearance"] == 256);
    }

    SECTION("triggers")
    {
      const auto box = Json{{"min", {600, 240, 0}}, {"max", {640, 270, 56}}};
      const auto solidOnly = fixture.call("space_check", {{"box", box}});
      CHECK(solidOnly["free"] == true);
      CHECK(solidOnly["ceiling"]["z"] == 128);

      const auto all = fixture.call("space_check", {{"box", box}, {"solidOnly", false}});
      CHECK(all["free"] == false);
      CHECK(containsId(all["overlaps"], fixture.id(*triggerBrush)));
    }

    SECTION("point entities")
    {
      const auto box = Json{{"min", {1000, 100, 0}}, {"max", {1040, 140, 56}}};
      const auto result = fixture.call("space_check", {{"box", box}});
      CHECK(result["free"] == false);
      CHECK(containsId(result["overlaps"], fixture.id(*ogre)));

      const auto withoutEntities =
        fixture.call("space_check", {{"box", box}, {"includeEntities", false}});
      CHECK(withoutEntities["free"] == true);
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture
          .callExpectingError(
            "space_check", {{"box", {{"min", {0, 0, 0}}, {"max", {0, 32, 32}}}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "space_check", {{"box", {{"min", {0, 0, 0}}, {"max", {-1, 32, 32}}}}})
          .code
        == ErrorCode::InvalidArgument);
    }
  }

  SECTION("map_plan_view")
  {
    SECTION("whole map")
    {
      const auto plan = fixture.call("map_plan_view", {{"height", 64}});
      CHECK(plan["cellSize"] == 32);
      CHECK(plan["origin"] == Json::array({-32, -32}));
      CHECK(plan["columns"] == 42);
      CHECK(plan["rows"] == 18);
      CHECK(plan["height"] == 64);

      CHECK(cellAt(plan, 100, 300) == '.');
      CHECK(cellAt(plan, 400, 400) == '.');
      CHECK(cellAt(plan, -8, 300) == '#');
      CHECK(cellAt(plan, 208, 208) == '#');
      CHECK(cellAt(plan, 776, 240) == '+');
      CHECK(cellAt(plan, 620, 240) == 't');
      CHECK(cellAt(plan, 700, 256) == '.');
      CHECK(cellAt(plan, 700, 150) == ' ');
      CHECK(cellAt(plan, 64, 64) == 'P');
      CHECK(cellAt(plan, 1024, 128) == 'M');
      CHECK(cellAt(plan, 1024, 256) == 'L');
      CHECK(cellAt(plan, 400, 96) == 'I');

      for (const auto* c : {"#", "+", "t", ".", " ", "P", "M", "L", "I"})
      {
        CHECK(plan["legend"].contains(c));
      }
      CHECK(!plan["legend"].contains("E"));
      CHECK(plan["entities"].size() == 6);
      CHECK(containsId(plan["entities"], fixture.id(*ogre)));
    }

    SECTION("image markers and entitiesTruncated")
    {
      const auto plan =
        fixture.call("map_plan_view", {{"height", 64}, {"format", "image"}});
      CHECK(plan["entitiesTruncated"] == false);
      REQUIRE(!fixture.host().snapshot.requests.empty());
      const auto& request = fixture.host().snapshot.last();
      // every listed entity is marked, also those above the slice (the light at z 200)
      REQUIRE(request.markers.size() == plan["entities"].size());
      auto labels = std::string{};
      for (const auto& marker : request.markers)
      {
        CHECK(marker.position.z() < request.camera.position.z());
        CHECK(
          marker.position.z() > request.camera.position.z() - request.camera.farPlane);
        labels += marker.label;
      }
      CHECK(labels.find('L') != std::string::npos);
      CHECK(labels.find('P') != std::string::npos);

      const auto truncated =
        fixture.call("map_plan_view", {{"height", 64}, {"maxEntities", 2}});
      CHECK(truncated["entities"].size() == 2);
      CHECK(truncated["entitiesTruncated"] == true);
    }

    SECTION("defaults")
    {
      const auto plan = fixture.call("map_plan_view");
      CHECK(plan["cellSize"] == 32);
      CHECK(plan["height"] == 32);
    }

    SECTION("region and cellSize")
    {
      const auto plan = fixture.call(
        "map_plan_view",
        {{"region", {{"min", {0, 0, 0}}, {"max", {512, 512, 256}}}},
         {"cellSize", 64},
         {"showEntities", false}});
      CHECK(plan["origin"] == Json::array({0, 0}));
      CHECK(plan["columns"] == 8);
      CHECK(plan["rows"] == 8);
      CHECK(plan["height"] == 48);
      CHECK(plan["entities"].empty());
      CHECK(cellAt(plan, 32, 32) == '.');
      CHECK(cellAt(plan, 208, 208) == '#');
      CHECK(!plan["legend"].contains("P"));
    }

    SECTION("invalid input")
    {
      const auto error = fixture.callExpectingError("map_plan_view", {{"cellSize", 1}});
      CHECK(error.code == ErrorCode::InvalidArgument);
      CHECK(!error.hint.empty());
    }
  }
}

} // namespace tb::mcp
