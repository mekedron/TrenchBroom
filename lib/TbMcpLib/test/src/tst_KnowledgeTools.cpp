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
#include "base/PreferenceManager.h"
#include "fs/TestEnvironment.h"
#include "mcp/McpToolFixture.h"
#include "mcp/tools/Manual.h"
#include "prefs/Preferences.h"

#include "kd/invoke.h"

#include <fmt/format.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::mcp
{
namespace
{

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;

std::filesystem::path manualPath()
{
  return getFixtureRoot() / "test" / "mcp" / "manual" / "index.html";
}

std::string readFile(const std::filesystem::path& path)
{
  auto stream = std::ifstream{path, std::ios::binary};
  auto buffer = std::stringstream{};
  buffer << stream.rdbuf();
  return buffer.str();
}

/** Resolves every reference to "[kind:key]". */
std::string markReferences(const std::string& text)
{
  return resolveManualText(text, [](const ManualReference kind, const std::string& key) {
    const auto kindName = kind == ManualReference::MenuItem ? "menu"
                          : kind == ManualReference::Action ? "action"
                                                            : "key";
    return fmt::format("[{}:{}]", kindName, key);
  });
}

const ManualSection& sectionOf(const Manual& manual, const std::string& id)
{
  const auto index = manual.find(id);
  REQUIRE(index);
  return manual.sections[*index];
}

std::vector<std::string> ids(const Json& items)
{
  auto result = std::vector<std::string>{};
  for (const auto& item : items)
  {
    result.push_back(item["id"].get<std::string>());
  }
  return result;
}

Json readResource(McpToolFixture& fixture, const std::string& uri)
{
  const auto read = fixture.rpc("resources/read", Json{{"uri", uri}});
  REQUIRE(read.contains("result"));
  const auto& contents = read["result"]["contents"];
  REQUIRE(contents.size() == 1);
  return contents[0];
}

} // namespace

TEST_CASE("Manual")
{
  const auto html = readFile(manualPath());

  SECTION("parseManual")
  {
    const auto manual = parseManual(html);
    CHECK(manual.title == "TrenchBroom Test Reference Manual");

    auto sectionIds = std::vector<std::string>{};
    for (const auto& section : manual.sections)
    {
      sectionIds.push_back(section.id);
    }
    // the table of contents in <nav> is skipped
    CHECK(
      sectionIds
      == std::vector<std::string>{
        "introduction",
        "features",
        "getting_started",
        "main_window",
        "the-info-bar",
        "camera_navigation",
        "looking-and-moving-around",
        "orbiting",
        "editing",
        "vertex_editing",
      });

    const auto& looking = sectionOf(manual, "looking-and-moving-around");
    CHECK(looking.title == "Looking and Moving Around");
    CHECK(looking.level == 3);
    CHECK(manual.sections[*looking.parent].id == "camera_navigation");
    CHECK(
      manual.titlePath(*manual.find("orbiting"))
      == std::vector<std::string>{
        "Getting Started", "Camera Navigation", "Looking and Moving Around"});

    const auto& gettingStarted = sectionOf(manual, "getting_started");
    CHECK(!gettingStarted.parent);
    REQUIRE(gettingStarted.children.size() == 2);
    CHECK(manual.sections[gettingStarted.children[0]].id == "main_window");
    CHECK(manual.sections[gettingStarted.children[1]].id == "camera_navigation");

    // entities, collapsed whitespace
    CHECK(
      sectionOf(manual, "introduction").text
      == "TrenchBroom is a level editing program for brush-based game engines such as "
         "Quake, Quake 2, and Hexen 2. It's easy to use & fast.");

    // nested lists
    CHECK(
      sectionOf(manual, "features").text
      == "- **General**\n"
         "  - Full support for editing in 3D and in up to three 2D views\n"
         "  - Unlimited Undo and Redo\n"
         "- **Brush Editing**\n"
         "  - Robust vertex editing");

    // figures (the duplicate caption is skipped), scripts, links
    CHECK(
      markReferences(sectionOf(manual, "main_window").text)
      == "![The main editing window (Ubuntu Linux)](images/MainWindow.png)\n\n"
         "You can show or hide the info bar by choosing [menu:Menu/View/Toggle Info "
         "Panel]. It contains the console and the live [issue "
         "browser](#issue_browser). Maximize a view with [menu:Menu/View/Maximize "
         "Current View].");

    CHECK(
      sectionOf(manual, "the-info-bar").text
      == "The info bar shows the *console* and the `issue` browser.");

    // tables with escaped cells
    CHECK(
      markReferences(looking.text)
      == "You can use the following keyboard shortcuts to move the camera:\n\n"
         "| Direction | Key |\n"
         "| --- | --- |\n"
         "| Forward | [action:Controls/Camera/Move forward] |\n"
         "| Toggle \\| Mode | [action:Controls/Map view/Unknown action] |\n\n"
         "Move the camera to the next point with [menu:Menu/View/Camera/Move to Next "
         "Point].");

    // ordered lists
    CHECK(
      markReferences(sectionOf(manual, "orbiting").text)
      == "The camera orbit mode rotates the camera about a point.\n\n"
         "1. Select an object and choose [menu:Menu/Edit/Copy]\n"
         "2. Hold [key:Alt] and drag the mouse.");

    // code blocks and definition lists
    CHECK(
      sectionOf(manual, "editing").text
      == "The map file format:\n\n"
         "```\n"
         "1. `Map      = Entity {Entity}`\n"
         "2. `Entity   = {Property} {Brush}`\n"
         "```\n\n"
         "**Group**\n"
         "[Groups](#groups) the selected objects.\n\n"
         "**Ungroup**\n"
         "Ungroups the selected objects.");
  }

  SECTION("Manual::find")
  {
    const auto manual = parseManual(html);
    CHECK(manual.find("camera_navigation") == 5);
    CHECK(manual.find("#Camera_Navigation") == 5);
    CHECK(manual.find("camera navigation") == 5);
    CHECK(manual.find("Looking and Moving Around") == 6);
    CHECK(manual.find("nothing") == std::nullopt);
  }

  SECTION("parseManual without the manual template")
  {
    const auto manual = parseManual(
      "<html><body><h1>First Part</h1><p>One&nbsp;two &lt;three&gt; &#x41;&#66; "
      "&unknown;</p><h2 id=\"sub\">Sub</h2><blockquote><p>Quoted</p></blockquote>"
      "<table><tr><td>a</td><td>b</td></tr></table><hr/><p>x<br>y</p></body></html>");
    REQUIRE(manual.sections.size() == 2);
    CHECK(manual.sections[0].id == "first-part");
    CHECK(manual.sections[0].text == "One two <three> AB &unknown;");
    CHECK(
      manual.sections[1].text
      == "> Quoted\n\n|  |  |\n| --- | --- |\n| a | b |\n\n---\n\nx\ny");
  }

  SECTION("parseManualShortcuts")
  {
    const auto shortcuts =
      parseManualShortcuts(readFile(manualPath().parent_path() / "shortcuts.js"));
    CHECK(shortcuts.keys.at("Alt") == "Alt");
    CHECK(shortcuts.keys.at("'") == "'");
    CHECK(shortcuts.keys.at("\\") == "\\");

    const auto& maximize = shortcuts.menu.at("Menu/View/Maximize Current View");
    CHECK(maximize.path == std::vector<std::string>{"View", "Maximize Current View"});
    CHECK(maximize.shortcuts == std::vector<std::string>{"Ctrl+Space"});
    CHECK(shortcuts.menu.at("Menu/View/Camera/Move to Next Point").shortcuts.empty());
    CHECK(
      shortcuts.actions.at("Controls/Camera/Move forward")
      == std::vector<std::string>{"W"});
  }

  SECTION("loadManual")
  {
    auto env = fs::TestEnvironment{};
    env.createFile("index.html", "<h1 id=\"a\">A</h1><p>first</p>");

    const auto first = loadManual(env.dir() / "index.html");
    REQUIRE(first.is_success());
    CHECK(first.value()->manual.sections.front().text == "first");
    CHECK(!first.value()->shortcuts);

    // cached while the file is unchanged
    CHECK(loadManual(env.dir() / "index.html").value() == first.value());

    env.createFile("index.html", "<h1 id=\"a\">A</h1><p>second version</p>");
    const auto second = loadManual(env.dir() / "index.html");
    REQUIRE(second.is_success());
    CHECK(second.value()->manual.sections.front().text == "second version");

    CHECK(loadManual(env.dir() / "missing.html").is_error());
  }
}

TEST_CASE("KnowledgeTools")
{
  auto fixture = McpToolFixture{};
  fixture.host().manualFile = manualPath();

  SECTION("manual_search")
  {
    const auto result = fixture.call("manual_search", Json{{"query", "vertex tool"}});
    REQUIRE(result["total"].get<size_t>() >= 2);
    const auto& first = result["items"][0];
    CHECK(first["id"] == "vertex_editing");
    CHECK(first["title"] == "Vertex Editing");
    CHECK(first["path"] == Json{"Editing"});
    CHECK(first["uri"] == "trenchbroom://manual/vertex_editing");
    CHECK(first["score"].get<double>() > result["items"][1]["score"].get<double>());
    REQUIRE(!first["snippets"].empty());
    CHECK_THAT(first["snippets"][0].get<std::string>(), ContainsSubstring("vertex"));

    // case-insensitive; titles rank before text
    CHECK(
      fixture.call(
        "manual_search", Json{{"query", "CAMERA NAVIGATION"}})["items"][0]["id"]
      == "camera_navigation");

    // the resolved shortcuts are searchable
    CHECK(
      ids(fixture.call("manual_search", Json{{"query", "Ctrl+Space"}})["items"])
      == std::vector<std::string>{"main_window"});

    CHECK(fixture.call("manual_search", Json{{"query", "xyzzy"}})["total"] == 0);

    const auto page =
      fixture.call("manual_search", Json{{"query", "camera"}, {"limit", 1}});
    CHECK(page["items"].size() == 1);
    CHECK(page["total"].get<size_t>() > 1);
    REQUIRE(page["nextCursor"].is_string());
    const auto next = fixture.call(
      "manual_search",
      Json{{"query", "camera"}, {"limit", 1}, {"cursor", page["nextCursor"]}});
    CHECK(next["items"][0]["id"] != page["items"][0]["id"]);

    CHECK(
      fixture.callExpectingError("manual_search", Json{{"query", ""}}).code
      == ErrorCode::InvalidArgument);
  }

  SECTION("manual_section")
  {
    const auto section =
      fixture.call("manual_section", Json{{"section", "camera_navigation"}});
    CHECK(section["title"] == "Camera Navigation");
    CHECK(section["level"] == 2);
    CHECK(section["path"] == Json{"Getting Started"});
    CHECK(section["uri"] == "trenchbroom://manual/camera_navigation");
    CHECK_THAT(
      section["text"].get<std::string>(),
      ContainsSubstring("[preferences](#mouse_input). Hold **Alt** to move"));
    CHECK(section["nextOffset"].is_null());
    CHECK(section["subsections"].size() == 1);
    CHECK(section["subsections"][0]["id"] == "looking-and-moving-around");
    CHECK(section["parent"]["id"] == "getting_started");
    CHECK(section["previous"]["id"] == "the-info-bar");
    CHECK(section["next"]["id"] == "looking-and-moving-around");

    const auto withSubsections = fixture.call(
      "manual_section",
      Json{{"section", "camera_navigation"}, {"includeSubsections", true}});
    const auto text = withSubsections["text"].get<std::string>();
    CHECK_THAT(text, ContainsSubstring("\n\n### Looking and Moving Around\n\n"));
    CHECK_THAT(text, ContainsSubstring("\n\n#### Orbiting\n\n"));

    // by title, by anchor and by uri
    CHECK(
      fixture.call("manual_section", Json{{"section", "vertex editing"}})["id"]
      == "vertex_editing");
    CHECK(
      fixture.call("manual_section", Json{{"section", "#vertex_editing"}})["id"]
      == "vertex_editing");
    CHECK(
      fixture.call(
        "manual_section", Json{{"section", "trenchbroom://manual/vertex_editing"}})["id"]
      == "vertex_editing");

    const auto first = fixture.call("manual_section", Json{{"section", "introduction"}});
    CHECK(first["previous"].is_null());
    CHECK(first["parent"].is_null());
    CHECK(fixture.call("manual_section", Json{{"section", "vertex_editing"}})["next"]
            .is_null());

    // paging
    const auto full = fixture.call("manual_section", Json{{"section", "vertex_editing"}});
    const auto fullText = full["text"].get<std::string>();
    REQUIRE(fullText.size() > 500);
    const auto page1 = fixture.call(
      "manual_section", Json{{"section", "vertex_editing"}, {"maxChars", 500}});
    REQUIRE(page1["nextOffset"].is_number());
    CHECK(page1["totalLength"] == fullText.size());
    CHECK(page1["length"].get<size_t>() <= 500);
    auto joined = page1["text"].get<std::string>();
    auto offset = page1["nextOffset"];
    while (offset.is_number())
    {
      const auto page = fixture.call(
        "manual_section",
        Json{{"section", "vertex_editing"}, {"maxChars", 500}, {"offset", offset}});
      CHECK(page["offset"] == offset);
      joined += page["text"].get<std::string>();
      offset = page["nextOffset"];
    }
    CHECK(joined == fullText);

    const auto error =
      fixture.callExpectingError("manual_section", Json{{"section", "vertex editng"}});
    CHECK(error.code == ErrorCode::ObjectNotFound);
    CHECK_THAT(error.hint, ContainsSubstring("vertex_editing"));
  }

  SECTION("references")
  {
    const auto textOf = [&](const std::string& id) {
      return fixture.call("manual_section", Json{{"section", id}})["text"]
        .get<std::string>();
    };

    SECTION("from the manual's shortcut table and the preferences")
    {
      const auto mainWindow = textOf("main_window");
      CHECK_THAT(
        mainWindow, ContainsSubstring("choosing **View > Toggle Info Panel** (Ctrl+4)."));
      CHECK_THAT(
        mainWindow, ContainsSubstring("**View > Maximize Current View** (Ctrl+Space)."));

      const auto looking = textOf("looking-and-moving-around");
      // the current value of the preference
      CHECK_THAT(looking, ContainsSubstring("| Forward | **W** |"));
      CHECK_THAT(
        looking,
        ContainsSubstring("| Toggle \\| Mode | **Unknown action** (no shortcut) |"));
      // an item without a shortcut
      CHECK_THAT(
        looking,
        ContainsSubstring("with **View > Camera > Move Camera to Next Point**."));

      const auto original = pref(Preferences::CameraFlyForward);
      const auto restore =
        kdl::invoke_later{[&]() { setPref(Preferences::CameraFlyForward, original); }};
      setPref(Preferences::CameraFlyForward, std::vector<KeySequence>{KeySequence{"Up"}});
      CHECK_THAT(
        textOf("looking-and-moving-around"), ContainsSubstring("| Forward | **Up** |"));
    }

    SECTION("from the host's shortcut preferences")
    {
      auto copyShortcut = Preference<std::vector<KeySequence>>{
        "Menu/Edit/Copy", std::vector<KeySequence>{KeySequence{"Ctrl+Shift+C"}}};
      fixture.host().preference.preferenceList.push_back(
        HostPreference{&copyShortcut, "keyboard", "Menu: Edit > Copy"});

      CHECK_THAT(
        textOf("orbiting"),
        ContainsSubstring(
          "1. Select an object and choose **Edit > Copy** (Ctrl+Shift+C)"));
    }

    SECTION("from the action host of the document's window")
    {
      fixture.create();
      fixture.host().action.actionList = {EditorAction{
        .path = "Menu/View/Toggle Info Panel",
        .label = "Show Info Panel",
        .kind = "menu",
        .menu = {"Window"},
        .shortcuts = {"F4"},
      }};
      CHECK_THAT(
        textOf("main_window"),
        ContainsSubstring("choosing **Window > Show Info Panel** (F4)."));
    }

    SECTION("without a shortcut table")
    {
      auto env = fs::TestEnvironment{};
      env.createFile("index.html", readFile(manualPath()));
      fixture.host().manualFile = env.dir() / "index.html";

      CHECK_THAT(
        textOf("main_window"),
        ContainsSubstring("choosing **View > Toggle Info Panel**."));
      CHECK_THAT(
        textOf("orbiting"), ContainsSubstring("2. Hold **Alt** and drag the mouse."));
      // the static fly key preference is still known
      CHECK_THAT(
        textOf("looking-and-moving-around"), ContainsSubstring("| Forward | **W** |"));
    }
  }

  SECTION("resources")
  {
    const auto toc = readResource(fixture, "trenchbroom://manual");
    CHECK(toc["mimeType"] == "application/json");
    const auto tocJson = *parseJson(toc["text"].get<std::string>());
    CHECK(tocJson["title"] == "TrenchBroom Test Reference Manual");
    CHECK(tocJson["sectionCount"] == 10);
    CHECK(tocJson["sections"][1]["id"] == "features");
    CHECK(tocJson["sections"][1]["parent"] == "introduction");
    CHECK(tocJson["sections"][1]["uri"] == "trenchbroom://manual/features");

    const auto section = readResource(fixture, "trenchbroom://manual/camera_navigation");
    CHECK(section["mimeType"] == "text/markdown");
    const auto text = section["text"].get<std::string>();
    CHECK_THAT(text, StartsWith("## Camera Navigation\n\nNavigation in TrenchBroom"));
    CHECK_THAT(
      text,
      ContainsSubstring("Subsections:\n\n- Looking and Moving Around "
                        "(trenchbroom://manual/looking-and-moving-around)"));

    CHECK(fixture.rpc("resources/read", Json{{"uri", "trenchbroom://manual/nothing"}})
            .contains("error"));

    const auto templates = fixture.rpc("resources/templates/list");
    auto found = false;
    for (const auto& resourceTemplate : templates["result"]["resourceTemplates"])
    {
      found =
        found || resourceTemplate["uriTemplate"] == "trenchbroom://manual/{section}";
    }
    CHECK(found);
  }

  SECTION("host without a manual")
  {
    fixture.host().manualFile = std::nullopt;
    CHECK(
      fixture.callExpectingError("manual_search", Json{{"query", "camera"}}).code
      == ErrorCode::UnsupportedInHost);
    CHECK(
      fixture.callExpectingError("manual_section", Json{{"section", "editing"}}).code
      == ErrorCode::UnsupportedInHost);
    CHECK(fixture.rpc("resources/read", Json{{"uri", "trenchbroom://manual"}})
            .contains("error"));

    fixture.host().manualFile = "/nonexistent/manual/index.html";
    CHECK(
      fixture.callExpectingError("manual_search", Json{{"query", "camera"}}).code
      == ErrorCode::IoError);
  }
}

} // namespace tb::mcp
