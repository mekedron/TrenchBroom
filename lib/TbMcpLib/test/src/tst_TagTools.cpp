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
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/Map.h"
#include "mdl/Map_Selection.h"
#include "mdl/Node.h"
#include "mdl/Selection.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

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

/** Creates a document through document_new, which loads the game's config and tags. */
ui::MapDocument& newDocument(McpToolFixture& fixture, const std::string& game)
{
  const auto result = fixture.call("document_new", Json{{"game", game}});
  const auto id = resultOf(result)["document"]["id"].get<std::string>();
  for (const auto& info : fixture.host().documentList)
  {
    if (info.id == id)
    {
      return *info.document;
    }
  }
  FAIL("document not found");
  return *fixture.host().documentList.front().document;
}

bool hasWarning(const Json& result, const std::string& code)
{
  return std::ranges::any_of(
    result["warnings"], [&](const auto& warning) { return warning["code"] == code; });
}

const Json* findTag(const Json& list, const std::string& name)
{
  for (const auto& tag : list["tags"])
  {
    if (tag["name"] == name)
    {
      return &tag;
    }
  }
  return nullptr;
}

std::string createBox(McpToolFixture& fixture, const Json& min, const Json& max)
{
  return resultOf(
           fixture.call("brush_create_box", Json{{"min", min}, {"max", max}}))["brush"]
    .get<std::string>();
}

mdl::BrushNode& brushNode(McpToolFixture& fixture, const std::string& id)
{
  auto* node = dynamic_cast<mdl::BrushNode*>(fixture.node(id));
  REQUIRE(node);
  return *node;
}

std::vector<std::string> faceIds(const std::string& brushId, const size_t count)
{
  auto result = std::vector<std::string>{};
  for (size_t i = 0; i < count; ++i)
  {
    result.push_back(brushId + "/face:" + std::to_string(i));
  }
  return result;
}

int contentFlag(const mdl::Map& map, const std::string& name)
{
  return map.gameInfo().gameConfig.faceAttribsConfig.contentFlags.flagValue(name);
}

int surfaceFlag(const mdl::Map& map, const std::string& name)
{
  return map.gameInfo().gameConfig.faceAttribsConfig.surfaceFlags.flagValue(name);
}

bool allFaces(const mdl::BrushNode& node, const auto& predicate)
{
  return std::ranges::all_of(node.brush().faces(), predicate);
}

std::string owningClassname(const mdl::Node& node)
{
  const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(node.parent());
  return entityNode ? entityNode->entity().classname() : std::string{};
}

} // namespace

TEST_CASE("TagTools")
{
  auto fixture = McpToolFixture{};

  SECTION("tags_list")
  {
    SECTION("Quake 2 tags with flags")
    {
      auto& map = newDocument(fixture, "Quake 2").map();
      const auto brush = createBox(fixture, {256, 0, 0}, {320, 64, 64});
      fixture.call("tag_apply", Json{{"tag", "Detail"}, {"ids", {brush}}});

      const auto result = fixture.call("tags_list");
      CHECK(result["game"] == "Quake 2");

      const auto* trigger = findTag(result, "Trigger");
      REQUIRE(trigger);
      CHECK((*trigger)["kind"] == "object");
      CHECK(
        (*trigger)["match"]
        == Json{
          {"type", "classname"}, {"pattern", "trigger*"}, {"material", "e1u1/trigger"}});
      CHECK((*trigger)["attributes"] == Json::array({"transparent"}));
      CHECK((*trigger)["canApply"] == true);
      CHECK((*trigger)["canRemove"] == true);
      CHECK((*trigger)["count"] == 0);
      CHECK(!trigger->contains("brushes"));
      const auto& triggerOptions = (*trigger)["options"];
      CHECK(
        std::ranges::find(triggerOptions, Json("trigger_once")) != triggerOptions.end());
      CHECK((*trigger)["optionCount"] == triggerOptions.size());

      const auto* liquid = findTag(result, "Liquid");
      REQUIRE(liquid);
      CHECK((*liquid)["kind"] == "face");
      CHECK((*liquid)["match"]["type"] == "contentflag");
      CHECK((*liquid)["match"]["flags"] == Json::array({"lava", "slime", "water"}));
      CHECK(
        (*liquid)["match"]["mask"]
        == (contentFlag(map, "lava") | contentFlag(map, "slime")
            | contentFlag(map, "water")));
      CHECK((*liquid)["options"] == Json::array({"lava", "slime", "water"}));

      const auto* hint = findTag(result, "Hint");
      REQUIRE(hint);
      CHECK(
        (*hint)["match"]
        == Json{
          {"type", "surfaceflag"},
          {"flags", {"hint"}},
          {"mask", surfaceFlag(map, "hint")}});

      const auto* detail = findTag(result, "Detail");
      REQUIRE(detail);
      CHECK((*detail)["count"] == 6);
      CHECK((*detail)["brushes"] == 1);
      CHECK(
        (*detail)["description"].get<std::string>().find("detail") != std::string::npos);

      // the Quake 2 config names the flag "clip", which Quake 2 does not define
      const auto* clip = findTag(result, "Clip");
      REQUIRE(clip);
      CHECK((*clip)["kind"] == "face");
      CHECK((*clip)["match"]["type"] == "invalidflags");
      CHECK((*clip)["canApply"] == false);
      CHECK((*clip)["canRemove"] == false);

      const auto faceTags = fixture.call("tags_list", Json{{"kind", "face"}});
      CHECK(std::ranges::all_of(
        faceTags["tags"], [](const auto& tag) { return tag["kind"] == "face"; }));
      CHECK(!findTag(faceTags, "Trigger"));
      CHECK(findTag(faceTags, "Clip"));
    }

    SECTION("Quake tags with materials and classnames")
    {
      newDocument(fixture, "Quake");
      const auto result = fixture.call("tags_list", Json{{"kind", "object"}});
      const auto* detail = findTag(result, "Detail");
      REQUIRE(detail);
      CHECK(
        (*detail)["match"] == Json{{"type", "classname"}, {"pattern", "func_detail*"}});
      CHECK(
        (*detail)["options"]
        == Json::array({"func_detail", "func_detail_illusionary", "func_detail_wall"}));

      const auto all = fixture.call("tags_list");
      const auto* clip = findTag(all, "Clip");
      REQUIRE(clip);
      CHECK((*clip)["match"] == Json{{"type", "material"}, {"pattern", "clip"}});
      CHECK((*clip)["canApply"] == true);
      CHECK((*clip)["canRemove"] == false);
    }

    SECTION("invalid input")
    {
      newDocument(fixture, "Quake");
      CHECK(
        fixture.callExpectingError("tags_list", Json{{"kind", "brush"}}).code
        == ErrorCode::InvalidArgument);
    }
  }

  SECTION("tag_apply")
  {
    SECTION("Quake 2")
    {
      auto& quake2Map = newDocument(fixture, "Quake 2").map();
      const auto brush = createBox(fixture, {256, 0, 0}, {320, 64, 64});
      mdl::deselectAll(quake2Map);

      SECTION("object tag with option, explicit ids, one undo step")
      {
        const auto applied = fixture.call(
          "tag_apply",
          Json{{"tag", "trigger"}, {"ids", {brush}}, {"option", "TRIGGER_ONCE"}});
        const auto& result = resultOf(applied);
        CHECK(result["tag"] == "Trigger");
        CHECK(result["kind"] == "object");
        CHECK(result["option"] == "trigger_once");
        CHECK(result["tagged"] == Json::array({brush}));
        CHECK(result["untagged"] == Json::array());
        REQUIRE(result["entities"].size() == 1);
        CHECK(applied["changes"]["created"] == result["entities"]);
        CHECK(!hasWarning(applied, "TAG_OPTION_CHOSEN"));
        CHECK(applied["undoStep"] == "AI: Apply Smart Tag");

        auto& node = brushNode(fixture, brush);
        CHECK(owningClassname(node) == "trigger_once");
        CHECK(allFaces(
          node, [](const auto& face) { return face.materialName() == "e1u1/trigger"; }));
        // the selection is restored
        CHECK(quake2Map.selection().nodes.empty());

        fixture.call("undo");
        CHECK(owningClassname(brushNode(fixture, brush)).empty());
        CHECK(fixture.node(result["entities"][0].get<std::string>()) == nullptr);
      }

      SECTION("object tag without option uses the first with a warning")
      {
        const auto options = (*findTag(fixture.call("tags_list"), "Trigger"))["options"];
        const auto applied =
          fixture.call("tag_apply", Json{{"tag", "Trigger"}, {"ids", {brush}}});
        CHECK(resultOf(applied)["option"] == options[0]);
        CHECK(hasWarning(applied, "TAG_OPTION_CHOSEN"));
        CHECK(
          owningClassname(brushNode(fixture, brush)) == options[0].get<std::string>());
      }

      SECTION("object tag with a single option on the selection")
      {
        mdl::selectNodes(quake2Map, {fixture.node(brush)});
        const auto applied = fixture.call("tag_apply", Json{{"tag", "Areaportal"}});
        CHECK(resultOf(applied)["option"] == "func_areaportal");
        CHECK(resultOf(applied)["tagged"] == Json::array({brush}));
        CHECK(!hasWarning(applied, "TAG_OPTION_CHOSEN"));
        CHECK(owningClassname(brushNode(fixture, brush)) == "func_areaportal");
        CHECK(quake2Map.selection().nodes == std::vector{fixture.node(brush)});
      }

      SECTION("face tag on explicit faces")
      {
        const auto faces = faceIds(brush, 2);
        const auto applied =
          fixture.call("tag_apply", Json{{"tag", "Detail"}, {"ids", faces}});
        const auto& result = resultOf(applied);
        CHECK(result["kind"] == "face");
        CHECK(result["tagged"] == faces);
        CHECK(result["untagged"] == Json::array());
        CHECK(result["entities"] == Json::array());
        CHECK(applied["changes"]["modified"] == Json::array({brush}));

        const auto& node = brushNode(fixture, brush);
        const auto detail = contentFlag(quake2Map, "detail");
        CHECK((node.brush().face(0).resolvedSurfaceContents() & detail) != 0);
        CHECK((node.brush().face(1).resolvedSurfaceContents() & detail) != 0);
        CHECK((node.brush().face(2).resolvedSurfaceContents() & detail) == 0);
      }

      SECTION("face tag on the selected faces and on a selected brush")
      {
        mdl::selectBrushFaces(
          quake2Map, {mdl::BrushFaceHandle{&brushNode(fixture, brush), 3}});
        const auto onFaces = fixture.call("tag_apply", Json{{"tag", "Hint"}});
        CHECK(resultOf(onFaces)["tagged"] == Json::array({brush + "/face:3"}));
        CHECK(quake2Map.selection().brushFaces.size() == 1);

        mdl::deselectAll(quake2Map);
        mdl::selectNodes(quake2Map, {fixture.node(brush)});
        const auto onBrush = fixture.call("tag_apply", Json{{"tag", "Skip"}});
        CHECK(resultOf(onBrush)["tagged"] == faceIds(brush, 6));
        CHECK(allFaces(brushNode(fixture, brush), [&](const auto& face) {
          return (face.resolvedSurfaceFlags() & surfaceFlag(quake2Map, "skip")) != 0;
        }));
      }

      SECTION("face tag with options")
      {
        const auto water = fixture.call(
          "tag_apply", Json{{"tag", "Liquid"}, {"ids", {brush}}, {"option", "water"}});
        CHECK(resultOf(water)["option"] == "water");
        CHECK(!hasWarning(water, "TAG_OPTION_CHOSEN"));
        CHECK(allFaces(brushNode(fixture, brush), [&](const auto& face) {
          return (face.resolvedSurfaceContents() & contentFlag(quake2Map, "water")) != 0
                 && (face.resolvedSurfaceContents() & contentFlag(quake2Map, "lava"))
                      == 0;
        }));

        const auto first =
          fixture.call("tag_apply", Json{{"tag", "Liquid"}, {"ids", {brush}}});
        CHECK(resultOf(first)["option"] == "lava");
        CHECK(hasWarning(first, "TAG_OPTION_CHOSEN"));

        const auto single = fixture.call(
          "tag_apply", Json{{"tag", "Detail"}, {"ids", {brush}}, {"option", "water"}});
        CHECK(hasWarning(single, "OPTION_IGNORED"));
      }

      SECTION("invalid input")
      {
        const auto unknown = fixture.callExpectingError(
          "tag_apply", Json{{"tag", "Glass"}, {"ids", {brush}}});
        CHECK(unknown.code == ErrorCode::InvalidArgument);
        CHECK(unknown.message.find("Trigger") != std::string::npos);

        const auto badOption = fixture.callExpectingError(
          "tag_apply", Json{{"tag", "Liquid"}, {"ids", {brush}}, {"option", "lemonade"}});
        CHECK(badOption.code == ErrorCode::InvalidArgument);
        CHECK(badOption.message.find("slime") != std::string::npos);
        CHECK(
          (brushNode(fixture, brush).brush().face(0).resolvedSurfaceContents()
           & contentFlag(quake2Map, "slime"))
          == 0);

        const auto invalidFlags = fixture.callExpectingError(
          "tag_apply", Json{{"tag", "Clip"}, {"ids", {brush}}});
        CHECK(invalidFlags.code == ErrorCode::Unsupported);
        CHECK(invalidFlags.hint.find("face_attributes_set") != std::string::npos);

        const auto faceForObjectTag = fixture.callExpectingError(
          "tag_apply", Json{{"tag", "Trigger"}, {"ids", {brush + "/face:0"}}});
        CHECK(faceForObjectTag.code == ErrorCode::WrongObjectKind);

        const auto point = resultOf(fixture.call(
          "entity_create_point",
          Json{
            {"classname", "info_player_start"},
            {"position", {-256, -256, 64}}}))["entity"]
                             .get<std::string>();
        CHECK(
          fixture
            .callExpectingError("tag_apply", Json{{"tag", "Trigger"}, {"ids", {point}}})
            .code
          == ErrorCode::WrongObjectKind);

        mdl::deselectAll(quake2Map);
        CHECK(
          fixture.callExpectingError("tag_apply", Json{{"tag", "Detail"}}).code
          == ErrorCode::NoSelection);
      }

      SECTION("dry run")
      {
        const auto dryRun = fixture.call(
          "tag_apply", Json{{"tag", "Trigger"}, {"ids", {brush}}, {"dryRun", true}});
        CHECK(dryRun["dryRun"] == true);
        CHECK(dryRun["changes"]["created"].size() == 1);
        CHECK(resultOf(dryRun)["tagged"] == Json::array({brush}));
        CHECK(owningClassname(brushNode(fixture, brush)).empty());
      }
    }

    SECTION("Quake")
    {
      auto& map = newDocument(fixture, "Quake").map();
      const auto brush = createBox(fixture, {256, 0, 0}, {320, 64, 64});
      mdl::deselectAll(map);

      SECTION("object tag")
      {
        const auto applied = fixture.call(
          "tag_apply",
          Json{{"tag", "Detail"}, {"ids", {brush}}, {"option", "func_detail"}});
        CHECK(resultOf(applied)["tagged"] == Json::array({brush}));
        CHECK(owningClassname(brushNode(fixture, brush)) == "func_detail");
      }

      SECTION("material tag without loaded materials")
      {
        const auto clip =
          fixture.call("tag_apply", Json{{"tag", "Clip"}, {"ids", {brush}}});
        CHECK(hasWarning(clip, "UNKNOWN_MATERIAL"));
        CHECK(resultOf(clip)["option"] == "clip");
        CHECK(resultOf(clip)["tagged"] == faceIds(brush, 6));
        CHECK(allFaces(brushNode(fixture, brush), [](const auto& face) {
          return face.materialName() == "clip";
        }));

        CHECK(
          fixture.callExpectingError("tag_apply", Json{{"tag", "Hint"}, {"ids", {brush}}})
            .code
          == ErrorCode::InvalidArgument);
        CHECK(
          fixture
            .callExpectingError(
              "tag_apply", Json{{"tag", "Hint"}, {"ids", {brush}}, {"option", "skip"}})
            .code
          == ErrorCode::InvalidArgument);

        const auto hint = fixture.call(
          "tag_apply",
          Json{{"tag", "Hint"}, {"ids", {brush + "/face:0"}}, {"option", "hintskip"}});
        CHECK(resultOf(hint)["option"] == "hintskip");
        CHECK(resultOf(hint)["tagged"] == Json::array({brush + "/face:0"}));
      }
    }
  }

  SECTION("tag_remove")
  {
    SECTION("Quake 2")
    {
      auto& map = newDocument(fixture, "Quake 2").map();
      const auto brush = createBox(fixture, {256, 0, 0}, {320, 64, 64});
      const auto other = createBox(fixture, {512, 0, 0}, {576, 64, 64});
      mdl::deselectAll(map);

      SECTION("object tag by entity id")
      {
        const auto entity = resultOf(fixture.call(
          "tag_apply",
          Json{
            {"tag", "Trigger"},
            {"ids", {brush}},
            {"option", "trigger_once"}}))["entities"][0]
                              .get<std::string>();

        const auto removed =
          fixture.call("tag_remove", Json{{"tag", "Trigger"}, {"ids", {entity}}});
        CHECK(resultOf(removed)["kind"] == "object");
        CHECK(resultOf(removed)["removed"] == Json::array({brush}));
        CHECK(resultOf(removed)["tagged"] == Json::array());
        CHECK(resultOf(removed)["skipped"] == Json::array());
        CHECK(removed["changes"]["removed"] == Json::array({entity}));
        CHECK(removed["undoStep"] == "AI: Remove Smart Tag");
        CHECK(owningClassname(brushNode(fixture, brush)).empty());
        CHECK(fixture.node(entity) == nullptr);

        fixture.call("undo");
        CHECK(owningClassname(brushNode(fixture, brush)) == "trigger_once");
      }

      SECTION("face tag on the selection, skipping untagged faces")
      {
        fixture.call("tag_apply", Json{{"tag", "Detail"}, {"ids", faceIds(brush, 2)}});
        mdl::selectNodes(map, {fixture.node(brush)});

        const auto removed = fixture.call("tag_remove", Json{{"tag", "Detail"}});
        CHECK(resultOf(removed)["kind"] == "face");
        CHECK(resultOf(removed)["removed"] == faceIds(brush, 2));
        CHECK(resultOf(removed)["skipped"].size() == 4);
        CHECK(allFaces(brushNode(fixture, brush), [&](const auto& face) {
          return (face.resolvedSurfaceContents() & contentFlag(map, "detail")) == 0;
        }));
        CHECK(map.selection().nodes == std::vector{fixture.node(brush)});
      }

      SECTION("tag not present")
      {
        const auto removed =
          fixture.call("tag_remove", Json{{"tag", "Hint"}, {"ids", {other}}});
        CHECK(hasWarning(removed, "TAG_NOT_PRESENT"));
        CHECK(resultOf(removed)["removed"] == Json::array());
        CHECK(removed["changes"]["modified"] == Json::array());
      }

      SECTION("invalid input")
      {
        CHECK(
          fixture
            .callExpectingError("tag_remove", Json{{"tag", "nope"}, {"ids", {brush}}})
            .code
          == ErrorCode::InvalidArgument);
        CHECK(
          fixture
            .callExpectingError(
              "tag_remove", Json{{"tag", "Trigger"}, {"ids", {brush + "/face:1"}}})
            .code
          == ErrorCode::WrongObjectKind);
      }

      SECTION("dry run")
      {
        fixture.call("tag_apply", Json{{"tag", "Detail"}, {"ids", {brush}}});
        const auto dryRun = fixture.call(
          "tag_remove", Json{{"tag", "Detail"}, {"ids", {brush}}, {"dryRun", true}});
        CHECK(dryRun["dryRun"] == true);
        CHECK(resultOf(dryRun)["removed"] == faceIds(brush, 6));
        CHECK(allFaces(brushNode(fixture, brush), [&](const auto& face) {
          return (face.resolvedSurfaceContents() & contentFlag(map, "detail")) != 0;
        }));
      }
    }

    SECTION("Quake material tags cannot be removed")
    {
      newDocument(fixture, "Quake");
      const auto brush = createBox(fixture, {256, 0, 0}, {320, 64, 64});
      fixture.call("tag_apply", Json{{"tag", "Clip"}, {"ids", {brush}}});
      const auto error =
        fixture.callExpectingError("tag_remove", Json{{"tag", "Clip"}, {"ids", {brush}}});
      CHECK(error.code == ErrorCode::Unsupported);
      CHECK(error.hint.find("material_apply") != std::string::npos);
    }
  }
}

} // namespace tb::mcp
