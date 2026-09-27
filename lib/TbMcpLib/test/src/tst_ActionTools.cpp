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

#include "mcp/FakeActionHost.h"
#include "mcp/FakeHost.h"
#include "mcp/McpServer.h"
#include "mcp/McpToolFixture.h"
#include "mcp/ToolRegistry.h"
#include "mcp/tools/ActionCatalog.h"
#include "mdl/EntityNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Entities.h"
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

EditorAction menuAction(
  std::string path,
  std::string label,
  std::vector<std::string> menu,
  const bool enabled = true)
{
  return EditorAction{
    .path = std::move(path),
    .label = std::move(label),
    .kind = "menu",
    .menu = std::move(menu),
    .shortcuts = {},
    .context = "any",
    .enabled = enabled,
    .checkable = false,
    .checked = false,
  };
}

EditorAction otherAction(std::string path, std::string label, std::string kind)
{
  return EditorAction{
    .path = std::move(path),
    .label = std::move(label),
    .kind = std::move(kind),
    .menu = {},
    .shortcuts = {},
    .context = "any",
    .enabled = true,
    .checkable = false,
    .checked = false,
  };
}

std::vector<EditorAction> testActions()
{
  auto convexMerge =
    menuAction("Menu/Edit/CSG/Convex Merge", "Convex Merge", {"Edit", "CSG"}, false);
  convexMerge.shortcuts = {"Ctrl+J"};
  convexMerge.context = "any view, objects selected";

  auto clipTool = menuAction("Menu/Edit/Tools/Clip Tool", "Clip Tool", {"Tools"});
  clipTool.checkable = true;
  clipTool.shortcuts = {"C"};

  return {
    menuAction("Menu/File/Save as...", "Save Document as...", {"File"}),
    menuAction("Menu/Edit/Undo", "Undo", {"Edit"}),
    convexMerge,
    menuAction("Menu/Edit/Select All", "Select All", {"Edit"}),
    clipTool,
    menuAction("Menu/View/Grid/Set Grid Size 16", "Set Grid Size 16", {"View", "Grid"}),
    menuAction("Menu/Debug/Crash...", "Crash...", {"Debug"}),
    menuAction("Menu/Future/Frobnicate...", "Frobnicate...", {"Future"}),
    otherAction("Controls/Map view/Perform clip", "Perform Clip", "view"),
    otherAction(
      "Controls/Map view/Flip objects horizontally", "Flip Horizontally", "view"),
    otherAction(
      "Controls/Map view/Flip textures horizontally", "Flip Horizontally", "view"),
    otherAction("Filters/Tags/Trigger/Toggle Visible", "Toggle Trigger visible", "tag"),
    otherAction("Entities/light/Create", "Create light", "entity"),
  };
}

std::vector<std::string> paths(const Json& items)
{
  auto result = std::vector<std::string>{};
  for (const auto& item : items)
  {
    result.push_back(item["path"].get<std::string>());
  }
  return result;
}

} // namespace

TEST_CASE("ActionCatalog")
{
  SECTION("findActionClass")
  {
    const auto* undo = findActionClass("Menu/Edit/Undo");
    REQUIRE(undo != nullptr);
    CHECK(undo->handling == ActionHandling::Refuse);
    CHECK(undo->tools == std::vector<std::string_view>{"undo"});

    const auto* saveAs = findActionClass("Menu/File/Save as...");
    REQUIRE(saveAs != nullptr);
    CHECK(saveAs->handling == ActionHandling::Dialog);
    CHECK(saveAs->dialog == "file");
    CHECK(saveAs->tools == std::vector<std::string_view>{"document_save_as"});

    const auto* merge = findActionClass("Menu/Edit/CSG/Convex Merge");
    REQUIRE(merge != nullptr);
    CHECK(merge->handling == ActionHandling::Invoke);
    CHECK(merge->reason.empty());

    CHECK(findActionClass("Menu/Future/Frobnicate") == nullptr);
  }

  SECTION("patterns of the tag and entity definition actions")
  {
    const auto* toggleTag = findActionClass("Filters/Tags/Detail/Toggle Visible");
    REQUIRE(toggleTag != nullptr);
    CHECK(toggleTag->handling == ActionHandling::Invoke);

    const auto* enableTag = findActionClass("Tags/Trigger/Enable");
    REQUIRE(enableTag != nullptr);
    CHECK(enableTag->handling == ActionHandling::Dialog);
    CHECK(enableTag->dialog == "menu");
    CHECK(enableTag->tools == std::vector<std::string_view>{"tag_apply"});

    const auto* disableTag = findActionClass("Tags/Trigger/Disable");
    REQUIRE(disableTag != nullptr);
    CHECK(disableTag->tools == std::vector<std::string_view>{"tag_remove"});

    CHECK(findActionClass("Entities/light/Toggle") != nullptr);
    CHECK(findActionClass("Entities/func_door/Create") != nullptr);

    // the name must not be empty
    CHECK(findActionClass("Entities//Create") == nullptr);
    CHECK(findActionClass("Tags/Enable") == nullptr);
  }

  SECTION("classifyAction")
  {
    CHECK(classifyAction("Menu/Edit/Undo", "Undo").handling == ActionHandling::Refuse);
    CHECK(
      classifyAction("Menu/Future/Frobnicate...", "Frobnicate...").handling
      == ActionHandling::Dialog);
    CHECK(
      classifyAction("Menu/Future/Frobnicate", "Frobnicate").handling
      == ActionHandling::Invoke);
  }

  SECTION("actionCatalogToolNames")
  {
    const auto names = actionCatalogToolNames();
    CHECK(std::ranges::is_sorted(names));
    CHECK(std::ranges::adjacent_find(names) == names.end());
    CHECK(std::ranges::find(names, "csg_merge") != names.end());
    CHECK(std::ranges::find(names, "tag_apply") != names.end());
  }

  SECTION("every path is listed once")
  {
    auto catalogPaths = std::vector<std::string_view>{};
    for (const auto& entry : actionCatalog())
    {
      catalogPaths.push_back(entry.path);
      // Dialog and Refuse actions explain why
      CHECK(
        (entry.actionClass.handling == ActionHandling::Invoke)
        == entry.actionClass.reason.empty());
      CHECK(
        (entry.actionClass.handling == ActionHandling::Dialog)
        != entry.actionClass.dialog.empty());
    }
    std::ranges::sort(catalogPaths);
    CHECK(std::ranges::adjacent_find(catalogPaths) == catalogPaths.end());
  }
}

TEST_CASE("ActionTools")
{
  auto fixture = McpToolFixture{};
  auto& document = fixture.create();
  auto& actionHost = fixture.host().action;
  actionHost.actionList = testActions();

  SECTION("actions_list")
  {
    SECTION("lists all actions with their classification")
    {
      const auto result = fixture.call("actions_list");
      CHECK(result["total"] == actionHost.actionList.size());
      CHECK(result["nextCursor"].is_null());
      CHECK(result["kinds"] == Json{{"entity", 1}, {"menu", 8}, {"tag", 1}, {"view", 3}});

      const auto& items = result["items"];
      const auto merge =
        std::ranges::find(items, "Menu/Edit/CSG/Convex Merge", [](const auto& item) {
          return item["path"].template get<std::string>();
        });
      REQUIRE(merge != items.end());
      CHECK(
        *merge
        == Json{
          {"path", "Menu/Edit/CSG/Convex Merge"},
          {"label", "Convex Merge"},
          {"kind", "menu"},
          {"menu", {"Edit", "CSG"}},
          {"shortcuts", {"Ctrl+J"}},
          {"enabled", false},
          {"checked", nullptr},
          {"opensDialog", false},
          {"invokable", true},
          {"tools", {"csg_merge"}},
        });

      const auto& saveAs = items[0];
      CHECK(saveAs["opensDialog"] == true);
      CHECK(saveAs["invokable"] == false);
      CHECK(saveAs["tools"] == Json{"document_save_as"});

      const auto& clipTool = items[4];
      CHECK(clipTool["path"] == "Menu/Edit/Tools/Clip Tool");
      CHECK(clipTool["checked"] == false);
    }

    SECTION("detail full adds the context and the reason")
    {
      const auto result =
        fixture.call("actions_list", Json{{"query", "save as"}, {"detail", "full"}});
      REQUIRE(result["items"].size() == 1);
      const auto& item = result["items"][0];
      CHECK(item["context"] == "any");
      CHECK(item["checkable"] == false);
      CHECK(item["handling"] == "dialog");
      CHECK(item["dialog"] == "file");
      CHECK(item["reason"] == "asks for the file name");
    }

    SECTION("filters")
    {
      CHECK(
        paths(fixture.call("actions_list", Json{{"kind", "view"}})["items"])
        == std::vector<std::string>{
          "Controls/Map view/Perform clip",
          "Controls/Map view/Flip objects horizontally",
          "Controls/Map view/Flip textures horizontally"});
      CHECK(
        paths(fixture.call("actions_list", Json{{"menu", "edit"}})["items"])
        == std::vector<std::string>{
          "Menu/Edit/Undo", "Menu/Edit/CSG/Convex Merge", "Menu/Edit/Select All"});
      CHECK(
        paths(fixture.call("actions_list", Json{{"menu", "Edit/CSG/"}})["items"])
        == std::vector<std::string>{"Menu/Edit/CSG/Convex Merge"});
      CHECK(paths(fixture.call("actions_list", Json{{"menu", "Edi"}})["items"]).empty());
      CHECK(
        paths(fixture.call("actions_list", Json{{"query", "GRID SIZE"}})["items"])
        == std::vector<std::string>{"Menu/View/Grid/Set Grid Size 16"});
      CHECK(
        paths(fixture.call("actions_list", Json{{"handling", "refuse"}})["items"])
        == std::vector<std::string>{"Menu/Edit/Undo", "Menu/Debug/Crash..."});

      const auto enabled = fixture.call("actions_list", Json{{"enabledOnly", true}});
      CHECK(enabled["total"] == actionHost.actionList.size() - 1);
      CHECK(enabled["kinds"]["menu"] == 7);
    }

    SECTION("pagination and fields")
    {
      const auto first = fixture.call("actions_list", Json{{"limit", 2}});
      CHECK(first["items"].size() == 2);
      CHECK(first["total"] == actionHost.actionList.size());
      REQUIRE(first["nextCursor"].is_string());

      const auto second = fixture.call(
        "actions_list",
        Json{{"limit", 2}, {"cursor", first["nextCursor"]}, {"fields", {"path"}}});
      CHECK(
        second["items"]
        == Json{
          {{"path", "Menu/Edit/CSG/Convex Merge"}}, {{"path", "Menu/Edit/Select All"}}});
    }

    SECTION("view")
    {
      fixture.call("actions_list", Json{{"view", "xz"}});

      actionHost.viewIds = {"3d"};
      const auto error = fixture.callExpectingError("actions_list", Json{{"view", "xz"}});
      CHECK(error.code == ErrorCode::OperationFailed);
      CHECK(error.message.find("xz") != std::string::npos);

      CHECK(
        fixture.callExpectingError("actions_list", Json{{"view", "front"}}).code
        == ErrorCode::InvalidArgument);
    }

    SECTION("host errors")
    {
      actionHost.error = "The document has no window";
      const auto error = fixture.callExpectingError("actions_list");
      CHECK(error.code == ErrorCode::OperationFailed);
      CHECK(error.message.find("no window") != std::string::npos);
    }

    SECTION("host without actions")
    {
      fixture.host().supportsActions = false;
      CHECK(
        fixture.callExpectingError("actions_list").code == ErrorCode::UnsupportedInHost);
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("actions_list", Json{{"kind", "toolbar"}}).code
        == ErrorCode::InvalidArgument);
    }
  }

  SECTION("action_invoke")
  {
    SECTION("runs an action")
    {
      fixture.host().toolName = "Clip Tool";
      const auto result =
        fixture.call("action_invoke", Json{{"path", "Menu/View/Grid/Set Grid Size 16"}});
      CHECK(result["undoStep"].is_null());
      CHECK(
        result["result"]
        == Json{
          {"path", "Menu/View/Grid/Set Grid Size 16"},
          {"label", "Set Grid Size 16"},
          {"kind", "menu"},
          {"executed", true},
          {"opened", nullptr},
          {"mapChanged", false},
          {"enabled", true},
          {"checked", nullptr},
          {"currentTool", "Clip Tool"},
          {"tools", {"grid_set"}},
        });
      REQUIRE(actionHost.invocations.size() == 1);
      CHECK(actionHost.invocations[0].document == &document);
      CHECK(actionHost.invocations[0].path == "Menu/View/Grid/Set Grid Size 16");
      CHECK(actionHost.invocations[0].viewId == std::nullopt);
      CHECK(actionHost.invocations[0].deferred == false);
    }

    SECTION("keeps the active tool")
    {
      fixture.host().prepareNotes = {"deactivated tool: Clip Tool"};
      const auto result =
        fixture.call("action_invoke", Json{{"path", "Controls/Map view/Perform clip"}});
      CHECK(fixture.host().prepareCount == 0);
      CHECK(result["warnings"] == Json::array());
    }

    SECTION("in a view")
    {
      fixture.call(
        "action_invoke",
        Json{{"path", "Controls/Map view/Perform clip"}, {"view", "xy"}});
      REQUIRE(actionHost.invocations.size() == 1);
      CHECK(actionHost.invocations[0].viewId == "xy");

      actionHost.viewIds = {"3d"};
      CHECK(
        fixture
          .callExpectingError(
            "action_invoke",
            Json{{"path", "Controls/Map view/Perform clip"}, {"view", "xy"}})
          .code
        == ErrorCode::OperationFailed);
    }

    SECTION("reports the checked state afterwards")
    {
      const auto result =
        fixture.call("action_invoke", Json{{"path", "Menu/Edit/Tools/Clip Tool"}});
      CHECK(result["result"]["checked"] == true);
    }

    SECTION("map changes are one undo step with a change report")
    {
      actionHost.onInvoke = [](ui::MapDocument& doc, EditorAction&) {
        mdl::setEntityProperty(doc.map(), "message", "hello");
      };
      const auto result =
        fixture.call("action_invoke", Json{{"path", "Menu/Edit/Select All"}});
      CHECK(result["result"]["mapChanged"] == true);
      CHECK(result["undoStep"] == "AI: Invoke Action");
      CHECK(result["changes"]["modified"].size() == 1);
      CHECK(document.map().worldNode().entity().property("message") != nullptr);
    }

    SECTION("accepts a unique label")
    {
      const auto result = fixture.call("action_invoke", Json{{"path", "select all"}});
      CHECK(result["result"]["path"] == "Menu/Edit/Select All");

      const auto error =
        fixture.callExpectingError("action_invoke", Json{{"path", "Flip Horizontally"}});
      CHECK(error.code == ErrorCode::InvalidArgument);
      CHECK(
        error.details["candidates"]
        == Json{
          "Controls/Map view/Flip objects horizontally",
          "Controls/Map view/Flip textures horizontally"});
    }

    SECTION("unknown action")
    {
      const auto error = fixture.callExpectingError(
        "action_invoke", Json{{"path", "Menu/View/Grid/Set Grid Size"}});
      CHECK(error.code == ErrorCode::InvalidArgument);
      CHECK(error.details["suggestions"] == Json{"Menu/View/Grid/Set Grid Size 16"});
      CHECK(error.hint.find("Set Grid Size 16") != std::string::npos);
      CHECK(actionHost.invocations.empty());
    }

    SECTION("disabled action")
    {
      fixture.host().toolName = "Vertex Tool";
      const auto error = fixture.callExpectingError(
        "action_invoke", Json{{"path", "Menu/Edit/CSG/Convex Merge"}});
      CHECK(error.code == ErrorCode::OperationFailed);
      CHECK(error.message.find("disabled") != std::string::npos);
      CHECK(error.details["context"] == "any view, objects selected");
      CHECK(error.details["currentTool"] == "Vertex Tool");
      CHECK(error.hint.find("csg_merge") != std::string::npos);
      CHECK(actionHost.invocations.empty());
    }

    SECTION("dialog actions")
    {
      SECTION("are refused without openDialog")
      {
        const auto error = fixture.callExpectingError(
          "action_invoke", Json{{"path", "Menu/File/Save as..."}});
        CHECK(error.code == ErrorCode::DialogRequired);
        CHECK(error.hint.find("document_save_as") != std::string::npos);
        CHECK(error.details["tools"] == Json{"document_save_as"});
        CHECK(error.details["dialog"] == "file");
        CHECK(actionHost.invocations.empty());
      }

      SECTION("open the dialog for the user with openDialog")
      {
        const auto result = fixture.call(
          "action_invoke", Json{{"path", "Menu/File/Save as..."}, {"openDialog", true}});
        CHECK(result["result"]["executed"] == false);
        CHECK(result["result"]["opened"] == "dialog");
        CHECK(result["undoStep"].is_null());
        REQUIRE(result["warnings"].size() == 1);
        CHECK(result["warnings"][0]["code"] == "DIALOG_OPENED");
        REQUIRE(actionHost.invocations.size() == 1);
        CHECK(actionHost.invocations[0].deferred == true);
      }

      SECTION("unknown actions whose label ends with ... are dialog actions")
      {
        const auto error = fixture.callExpectingError(
          "action_invoke", Json{{"path", "Menu/Future/Frobnicate..."}});
        CHECK(error.code == ErrorCode::DialogRequired);
        CHECK(error.hint.find("openDialog") != std::string::npos);
      }
    }

    SECTION("refused actions")
    {
      const auto undo =
        fixture.callExpectingError("action_invoke", Json{{"path", "Menu/Edit/Undo"}});
      CHECK(undo.code == ErrorCode::ActionRefused);
      CHECK(undo.hint == "Use undo instead.");
      CHECK(undo.details["tools"] == Json{"undo"});

      const auto crash = fixture.callExpectingError(
        "action_invoke", Json{{"path", "Menu/Debug/Crash..."}, {"openDialog", true}});
      CHECK(crash.code == ErrorCode::ActionRefused);
      CHECK(actionHost.invocations.empty());
    }

    SECTION("dry run")
    {
      const auto result = fixture.call(
        "action_invoke", Json{{"path", "Menu/Edit/Tools/Clip Tool"}, {"dryRun", true}});
      CHECK(result["dryRun"] == true);
      CHECK(result["result"]["executed"] == false);
      CHECK(result["result"]["wouldDo"] == "run 'Clip Tool'");
      CHECK(result["result"]["checked"] == false);

      const auto dialog = fixture.call(
        "action_invoke",
        Json{{"path", "Menu/File/Save as..."}, {"openDialog", true}, {"dryRun", true}});
      CHECK(
        dialog["result"]["wouldDo"]
        == "open the dialog of 'Save Document as...' for the user");
      CHECK(actionHost.invocations.empty());

      // a dry run still checks the action
      CHECK(
        fixture
          .callExpectingError(
            "action_invoke", Json{{"path", "Menu/Edit/Undo"}, {"dryRun", true}})
          .code
        == ErrorCode::ActionRefused);
    }

    SECTION("host errors")
    {
      actionHost.error = "The document has no window";
      CHECK(
        fixture
          .callExpectingError("action_invoke", Json{{"path", "Menu/Edit/Select All"}})
          .code
        == ErrorCode::OperationFailed);
    }

    SECTION("host without actions")
    {
      fixture.host().supportsActions = false;
      CHECK(
        fixture
          .callExpectingError("action_invoke", Json{{"path", "Menu/Edit/Select All"}})
          .code
        == ErrorCode::UnsupportedInHost);
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("action_invoke").code == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("action_invoke", Json{{"path", ""}}).code
        == ErrorCode::InvalidArgument);
    }
  }

  SECTION("the catalog refers only to registered tools")
  {
    const auto& tools = fixture.server().tools();
    for (const auto& name : actionCatalogToolNames())
    {
      CAPTURE(name);
      CHECK(tools.find(name) != nullptr);
    }
  }
}

} // namespace tb::mcp
