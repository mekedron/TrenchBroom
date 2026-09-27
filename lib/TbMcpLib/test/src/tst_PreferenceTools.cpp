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

#include "base/PreferenceManager.h"
#include "fs/TestEnvironment.h"
#include "mcp/McpToolFixture.h"
#include "mcp/tools/PreferenceCatalog.h"
#include "mdl/GameInfo.h"
#include "mdl/GameManager.h"
#include "prefs/Preferences.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <regex>
#include <set>
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

const Json& resultOf(const Json& structured)
{
  return structured["result"];
}

/** The paths of all preferences declared in prefs/Preferences.h. */
std::set<std::string> declaredPreferencePaths()
{
  auto stream = std::ifstream{MCP_TEST_PREFERENCES_HEADER};
  REQUIRE(stream);
  auto buffer = std::stringstream{};
  buffer << stream.rdbuf();
  const auto header = buffer.str();

  // Preference<T>{"path", ...} where T may contain '<' and '>'
  static const auto pattern = std::regex{R"re(Preference<[^{;]*?>\s*\{\s*"([^"]+)")re"};

  auto result = std::set<std::string>{};
  for (auto it = std::sregex_iterator{header.begin(), header.end(), pattern};
       it != std::sregex_iterator{};
       ++it)
  {
    result.insert((*it)[1].str());
  }
  return result;
}

/** Restores the values of the given preferences on destruction. */
class PreferenceRestorer
{
private:
  std::vector<std::function<void()>> m_restore;

public:
  explicit PreferenceRestorer(const std::vector<AnyPreference>& preferences)
  {
    for (const auto& preference : preferences)
    {
      std::visit(
        [&](auto* p) {
          m_restore.push_back([p, value = pref(*p)]() {
            if (pref(*p) != value)
            {
              setPref(*p, value);
            }
          });
        },
        preference);
    }
  }

  ~PreferenceRestorer()
  {
    for (const auto& restore : m_restore)
    {
      restore();
    }
  }
};

const Json* findItem(const Json& items, const std::string& path)
{
  for (const auto& item : items)
  {
    if (item["path"] == path)
    {
      return &item;
    }
  }
  return nullptr;
}

Json getOne(
  McpToolFixture& fixture, const std::string& path, const std::string& detail = "summary")
{
  const auto result =
    fixture.call("preferences_get", Json{{"paths", Json{path}}, {"detail", detail}});
  REQUIRE(result["items"].size() == 1);
  return result["items"][0];
}

} // namespace

TEST_CASE("PreferenceCatalog")
{
  SECTION("editorPreferences")
  {
    // A preference added to Preferences.h must be added to the catalog
    const auto declared = declaredPreferencePaths();
    REQUIRE(declared.size() > 100);

    auto cataloged = std::set<std::string>{};
    for (const auto& info : editorPreferences())
    {
      CHECK(cataloged.insert(info.path().generic_string()).second);
      CHECK(!info.category.empty());
      CHECK(!info.description.empty());
      CHECK(info.source == PreferenceSource::Editor);
    }
    CHECK(cataloged == declared);
  }

  SECTION("gamePreferences")
  {
    auto fixture = McpToolFixture{};
    auto& gameManager = fixture.host().gameManager();
    const auto preferences = gamePreferences(gameManager);

    auto paths = std::set<std::string>{};
    for (const auto& info : preferences)
    {
      paths.insert(info.path().generic_string());
      CHECK(info.category == "games");
      CHECK(info.source == PreferenceSource::Game);
    }
    CHECK(paths.contains("Games/Quake/Path"));
    CHECK(paths.contains("Games/Quake/Default Engine"));
    CHECK(paths.contains("Games/Quake/Tool Path/qbsp"));
    CHECK(paths.contains("Games/Quake/Tool Path/light"));
    CHECK(paths.contains("Games/Half-Life/Path"));

    auto expected = size_t{0};
    for (const auto& gameInfo : gameManager.gameInfos())
    {
      expected += 2 + gameInfo.gameConfig.compilationTools.size();
    }
    CHECK(preferences.size() == expected);
  }

  SECTION("allPreferences")
  {
    auto fixture = McpToolFixture{};
    auto hostFlag = Preference<bool>{"Test/Host flag", false};
    // a host preference with the path of an editor preference is dropped
    auto duplicate = Preference<bool>{"Map view/Show edges", false};
    fixture.host().preference.preferenceList = {
      HostPreference{&hostFlag, "test", "A host flag."},
      HostPreference{&duplicate, "test", "A duplicate."},
    };

    const auto preferences = allPreferences(fixture.host(), nullptr);
    CHECK(
      preferences.size()
      == editorPreferences().size() + gamePreferences(fixture.host().gameManager()).size()
           + 1);
    CHECK(preferences.back().path() == "Test/Host flag");
    CHECK(preferences.back().source == PreferenceSource::Host);

    fixture.host().supportsPreferences = false;
    CHECK(allPreferences(fixture.host(), nullptr).size() == preferences.size() - 1);
  }
}

TEST_CASE("PreferenceTools")
{
  auto fixture = McpToolFixture{};

  auto& quakeInfo = *fixture.host().gameManager().gameInfo("Quake");
  const auto restore = PreferenceRestorer{{
    &Preferences::MapViewLayout,
    &Preferences::ShowEdges,
    &Preferences::BackgroundColor,
    &Preferences::SelectionBoundsColor,
    &Preferences::CameraFov,
    &Preferences::Theme,
    &Preferences::FaceRenderMode,
    &Preferences::CameraFlyForward,
    &Preferences::CameraFlyBackward,
    &Preferences::RendererFontPath,
    &Preferences::GridAlpha,
    &quakeInfo.gamePathPreference,
    &quakeInfo.defaultEnginePathPreference,
  }};

  auto hostFlag = Preference<bool>{"Test/Host flag", false};
  auto hostPort = Preference<int>{"Test/Port", 100};
  auto hostToken = Preference<std::string>{"Test/Token", "secret"};
  auto hostShortcut = Preference<std::vector<KeySequence>>{
    "Menu/Test/Action", std::vector<KeySequence>{KeySequence{"Ctrl+K"}}};
  const auto restoreHost =
    PreferenceRestorer{{&hostFlag, &hostPort, &hostToken, &hostShortcut}};
  fixture.host().preference.preferenceList = {
    HostPreference{&hostFlag, "test", "A host flag."},
    HostPreference{
      .preference = &hostPort,
      .category = "test",
      .description = "A port.",
      .minimum = 1.0,
      .maximum = 1000.0,
      .lockedReason = "it would end the connection",
    },
    HostPreference{
      .preference = &hostToken,
      .category = "test",
      .description = "A token.",
      .secret = true,
    },
    HostPreference{&hostShortcut, "keyboard", "Menu: Test > Action"},
  };

  SECTION("preferences_get")
  {
    SECTION("all")
    {
      const auto result = fixture.call("preferences_get", Json{{"limit", 1000}});
      const auto expected = allPreferences(fixture.host(), nullptr).size();
      CHECK(result["total"] == expected);
      CHECK(result["items"].size() == expected);
      CHECK(result["categories"]["view"].get<size_t>() > 10);
      CHECK(result["categories"]["games"].get<size_t>() > 10);
      CHECK(result["categories"]["keyboard"] == 7);
      CHECK(result["categories"]["test"] == 3);

      const auto page = fixture.call("preferences_get", Json{{"limit", 10}});
      CHECK(page["items"].size() == 10);
      CHECK(page["nextCursor"].is_string());
    }

    SECTION("types and value forms")
    {
      const auto layout = getOne(fixture, "Views/Map view layout", "full");
      CHECK(layout["type"] == "int");
      CHECK(layout["value"] == 0);
      CHECK(layout["default"] == 0);
      CHECK(layout["modified"] == false);
      CHECK(layout["category"] == "view");
      CHECK(layout["allowedValues"] == Json{0, 1, 2, 3});
      CHECK(layout["persistence"] == "persistent");
      CHECK(layout["source"] == "editor");
      CHECK_THAT(
        layout["description"].get<std::string>(), ContainsSubstring("four panes"));

      const auto edges = getOne(fixture, "Map view/Show edges");
      CHECK(edges["type"] == "bool");
      CHECK(edges["value"] == true);
      CHECK(!edges.contains("default"));

      const auto fov = getOne(fixture, "Controls/Camera/Field of vision");
      CHECK(fov["type"] == "float");
      CHECK(fov["value"] == 90.0);
      CHECK(fov["minimum"] == 50.0);
      CHECK(fov["maximum"] == 150.0);

      // floats are rounded
      CHECK(getOne(fixture, "render/Grid/Alpha")["value"] == 0.5);
      CHECK(getOne(fixture, "render/Colors/Transparent faces")["value"] == 0.4);

      const auto theme = getOne(fixture, "Theme", "full");
      CHECK(theme["type"] == "string");
      CHECK(theme["allowedValues"] == Json{"System", "Dark"});
      CHECK_THAT(theme["note"].get<std::string>(), ContainsSubstring("restart"));

      const auto font = getOne(fixture, "render/Font name");
      CHECK(font["type"] == "path");
      CHECK(font["value"] == "fonts/SourceSansPro-Regular.otf");

      const auto background = getOne(fixture, "render/Colors/Background");
      CHECK(background["type"] == "color");
      CHECK(background["value"] == "#262626");
      CHECK(getOne(fixture, "render/Colors/Selection bounds")["value"] == "#FF000059");

      const auto xAxis = getOne(fixture, "render/Colors/X axis", "full");
      CHECK(xAxis["persistence"] == "readOnly");

      const auto forward = getOne(fixture, "Controls/Camera/Move forward");
      CHECK(forward["type"] == "shortcuts");
      CHECK(forward["value"] == Json{"W"});
      CHECK(forward["category"] == "keyboard");

      CHECK(
        getOne(fixture, "updater/Include draft releases", "full")["persistence"]
        == "transient");

      const auto gamePath = getOne(fixture, "Games/Quake/Path", "full");
      CHECK(gamePath["type"] == "path");
      CHECK(gamePath["category"] == "games");
      CHECK(gamePath["source"] == "game");

      const auto token = getOne(fixture, "Test/Token", "full");
      CHECK(token["value"].is_null());
      CHECK(token["default"].is_null());
      CHECK(token["secret"] == true);

      const auto port = getOne(fixture, "Test/Port", "full");
      CHECK(port["source"] == "host");
      CHECK(port["lockedReason"] == "it would end the connection");
      CHECK(port["minimum"] == 1.0);
    }

    SECTION("filters")
    {
      const auto view = fixture.call("preferences_get", Json{{"category", "VIEW"}});
      CHECK(view["total"] == view["items"].size());
      for (const auto& item : view["items"])
      {
        CHECK(item["category"] == "view");
      }

      const auto categories =
        fixture.call("preferences_get", Json{{"category", Json{"editor", "updater"}}});
      CHECK(categories["total"] == 8);

      const auto quake =
        fixture.call("preferences_get", Json{{"prefix", "games/quake/"}});
      CHECK(quake["total"] == 5);
      CHECK(findItem(quake["items"], "Games/Quake/Tool Path/vis"));

      const auto prefixes = fixture.call(
        "preferences_get", Json{{"prefix", Json{"Games/Quake/", "Editor/"}}});
      CHECK(prefixes["total"] == 8);

      const auto query =
        fixture.call("preferences_get", Json{{"query", "grid ALPHA"}, {"limit", 1000}});
      CHECK(query["total"] == 1);
      CHECK(query["items"][0]["path"] == "render/Grid/Alpha");

      // descriptions are searched too
      const auto describedQuery =
        fixture.call("preferences_get", Json{{"query", "antialiasing"}});
      CHECK(describedQuery["items"][0]["path"] == "render/Enable multisampling");

      const auto modifiedArgs = Json{{"prefix", "Map view/"}, {"modifiedOnly", true}};
      CHECK(fixture.call("preferences_get", modifiedArgs)["total"] == 0);
      setPref(Preferences::ShowEdges, false);
      const auto modified = fixture.call("preferences_get", modifiedArgs);
      CHECK(modified["total"] == 1);
      CHECK(modified["items"][0]["path"] == "Map view/Show edges");

      const auto paths = fixture.call(
        "preferences_get",
        Json{{"paths", Json{"theme", "Map view/Show edges"}}, {"fields", Json{"path"}}});
      CHECK(
        paths["items"]
        == Json{Json{{"path", "Theme"}}, Json{{"path", "Map view/Show edges"}}});
    }

    SECTION("document shortcuts")
    {
      // the document is passed to the preference host (the fake ignores it)
      fixture.create();
      CHECK(
        fixture.call("preferences_get", Json{{"category", "keyboard"}})["total"] == 7);
    }

    SECTION("unknown paths")
    {
      const auto error = fixture.callExpectingError(
        "preferences_get", Json{{"paths", Json{"Map view/Show edge", "Nothing/At all"}}});
      CHECK(error.code == ErrorCode::ObjectNotFound);
      CHECK_THAT(error.message, ContainsSubstring("Map view/Show edge, Nothing/At all"));
      CHECK_THAT(error.hint, ContainsSubstring("Map view/Show edges"));
    }
  }

  SECTION("preferences_set")
  {
    SECTION("values of every type")
    {
      const auto result = fixture.call(
        "preferences_set",
        Json{
          {"values",
           Json{
             {"Views/Map view layout", 3},
             {"Map view/Show edges", false},
             {"Controls/Camera/Field of vision", 100.5},
             {"Theme", "dark"},
             {"render/Font name", "fonts/Other.otf"},
             {"render/Colors/Background", "#102030"},
             {"Controls/Camera/Move forward", Json{"Up", " "}},
             {"Test/Host flag", true},
           }}});

      const auto& items = resultOf(result)["items"];
      REQUIRE(items.size() == 8);
      CHECK(items[0]["path"] == "Views/Map view layout");
      CHECK(items[0]["previous"] == 0);
      CHECK(items[0]["value"] == 3);
      CHECK(items[0]["changed"] == true);
      CHECK(items[3]["value"] == "Dark");
      CHECK_THAT(items[3]["note"].get<std::string>(), ContainsSubstring("restart"));
      CHECK(items[6]["value"] == Json{"Up"});

      CHECK(pref(Preferences::MapViewLayout) == 3);
      CHECK(pref(Preferences::ShowEdges) == false);
      CHECK(pref(Preferences::CameraFov) == 100.5f);
      CHECK(pref(Preferences::Theme) == Preferences::DarkTheme);
      CHECK(pref(Preferences::RendererFontPath) == "fonts/Other.otf");
      CHECK(getOne(fixture, "render/Colors/Background")["value"] == "#102030");
      CHECK(
        pref(Preferences::CameraFlyForward)
        == std::vector<KeySequence>{KeySequence{"Up"}});
      CHECK(pref(hostFlag) == true);

      // setting the same value again reports no change
      const auto again = fixture.call(
        "preferences_set", Json{{"values", Json{{"Views/Map view layout", 3}}}});
      CHECK(resultOf(again)["items"][0]["changed"] == false);
    }

    SECTION("colors")
    {
      const auto set = [&](const Json& value) {
        fixture.call(
          "preferences_set", Json{{"values", Json{{"render/Colors/Background", value}}}});
        return getOne(fixture, "render/Colors/Background")["value"];
      };
      CHECK(set("#ff000080") == "#FF000080");
      CHECK(set(Json{0.0, 1.0, 0.0}) == "#00FF00");
      CHECK(set(Json{0.0, 0.0, 1.0, 0.5}) == "#0000FF80");
      CHECK(set("1 1 0") == "#FFFF00");
      CHECK(set("255 128 0") == "#FF8000");

      for (const auto& invalid :
           {Json{"#12345"},
            Json{"#GG0000"},
            Json{"red"},
            Json{Json{2.0, 0.0, 0.0}},
            Json{1}})
      {
        CHECK(
          fixture
            .callExpectingError(
              "preferences_set",
              Json{{"values", Json{{"render/Colors/Background", invalid}}}})
            .code
          == ErrorCode::InvalidArgument);
      }
    }

    SECTION("reset")
    {
      setPref(Preferences::CameraFov, 120.0f);
      setPref(Preferences::ShowEdges, false);
      const auto result = fixture.call(
        "preferences_set",
        Json{{"reset", Json{"Controls/Camera/Field of vision", "Map view/Show edges"}}});
      const auto& items = resultOf(result)["items"];
      CHECK(items[0]["previous"] == 120.0);
      CHECK(items[0]["value"] == 90.0);
      CHECK(pref(Preferences::CameraFov) == 90.0f);
      CHECK(pref(Preferences::ShowEdges) == true);
    }

    SECTION("dry run")
    {
      const auto result = fixture.call(
        "preferences_set",
        Json{
          {"values", Json{{"Views/Map view layout", 2}}},
          {"reset", Json{"Theme"}},
          {"dryRun", true}});
      CHECK(resultOf(result)["items"][0]["value"] == 2);
      CHECK(resultOf(result)["wouldDo"] == "change 2 preferences");
      CHECK(pref(Preferences::MapViewLayout) == 0);
    }

    SECTION("invalid values change nothing")
    {
      const auto check =
        [&](const Json& values, const ErrorCode code, const std::string& text) {
          const auto error =
            fixture.callExpectingError("preferences_set", Json{{"values", values}});
          CHECK(error.code == code);
          CHECK_THAT(error.message, ContainsSubstring(text));
          CHECK(pref(Preferences::MapViewLayout) == 0);
          CHECK(pref(Preferences::ShowEdges) == true);
        };

      // atomic: the valid change is not applied either
      check(
        Json{{"Map view/Show edges", false}, {"Views/Map view layout", "two"}},
        ErrorCode::InvalidArgument,
        "expects an integer");
      check(
        Json{{"Views/Map view layout", 5}},
        ErrorCode::InvalidArgument,
        "one of 0, 1, 2, 3");
      check(Json{{"Views/Map view layout", 1.5}}, ErrorCode::InvalidArgument, "integer");
      check(
        Json{{"Map view/Show edges", "yes"}},
        ErrorCode::InvalidArgument,
        "true or false");
      check(
        Json{{"Controls/Camera/Field of vision", 10}},
        ErrorCode::InvalidArgument,
        "between 50 and 150");
      check(Json{{"Theme", "Pink"}}, ErrorCode::InvalidArgument, "\"System\", \"Dark\"");
      check(Json{{"render/Font name", 1}}, ErrorCode::InvalidArgument, "path");
      check(
        Json{{"Controls/Camera/Move forward", 1}},
        ErrorCode::InvalidArgument,
        "portable text");
      check(
        Json{{"render/Colors/X axis", "#FF0000"}},
        ErrorCode::ObjectNotEditable,
        "read-only");
      check(
        Json{{"Test/Port", 10}},
        ErrorCode::ObjectNotEditable,
        "it would end the connection");

      const auto several = fixture.callExpectingError(
        "preferences_set",
        Json{{"values", Json{{"Views/Map view layout", 5}, {"Map view/Show edges", 1}}}});
      CHECK(several.details["errors"].size() == 2);

      const auto unknown = fixture.callExpectingError(
        "preferences_set", Json{{"values", Json{{"Views/Map view layot", 1}}}});
      CHECK(unknown.code == ErrorCode::ObjectNotFound);
      CHECK_THAT(unknown.hint, ContainsSubstring("Views/Map view layout"));

      CHECK(
        fixture
          .callExpectingError(
            "preferences_set",
            Json{{"values", Json{{"Theme", "Dark"}}}, {"reset", Json{"theme"}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("preferences_set", Json::object()).code
        == ErrorCode::InvalidArgument);
    }

    SECTION("game preferences")
    {
      auto env = fs::TestEnvironment{};
      const auto result = fixture.call(
        "preferences_set",
        Json{
          {"values",
           Json{
             {"Games/Quake/Path", env.dir().string()},
             {"Games/Quake/Default Engine", (env.dir() / "missing").string()},
           }}});
      CHECK(pref(quakeInfo.gamePathPreference) == env.dir());
      CHECK(resultOf(result)["items"][0]["value"] == env.dir().generic_string());

      auto warnings = std::vector<std::string>{};
      for (const auto& warning : result["warnings"])
      {
        warnings.push_back(warning["code"].get<std::string>());
      }
      CHECK(warnings == std::vector<std::string>{"PATH_NOT_FOUND"});
    }

    SECTION("host preferences")
    {
      const auto result =
        fixture.call("preferences_set", Json{{"values", Json{{"Test/Token", "new"}}}});
      CHECK(resultOf(result)["items"][0]["value"].is_null());
      CHECK(resultOf(result)["items"][0]["previous"].is_null());
      CHECK(pref(hostToken) == "new");
    }

    SECTION("shortcut conflicts")
    {
      const auto result = fixture.call(
        "preferences_set",
        Json{{"values", Json{{"Controls/Camera/Move backward", Json{"ctrl+k"}}}}});
      const auto& conflicts = resultOf(result)["items"][0]["conflicts"];
      REQUIRE(conflicts.size() == 1);
      CHECK(conflicts[0]["path"] == "Menu/Test/Action");
      CHECK(conflicts[0]["shortcut"] == "ctrl+k");
      REQUIRE(result["warnings"].size() == 1);
      CHECK(result["warnings"][0]["code"] == "SHORTCUT_CONFLICT");

      // resolving the conflict in the same call reports no conflict
      const auto resolved = fixture.call(
        "preferences_set",
        Json{
          {"values",
           Json{
             {"Controls/Camera/Move forward", Json{"Ctrl+K"}},
             {"Menu/Test/Action", Json::array()}}},
          {"reset", Json{"Controls/Camera/Move backward"}}});
      for (const auto& item : resultOf(resolved)["items"])
      {
        CHECK(!item.contains("conflicts"));
      }
      CHECK(pref(hostShortcut).empty());
    }
  }

  SECTION("host without preferences")
  {
    fixture.host().supportsPreferences = false;
    CHECK(fixture.call("preferences_get", Json{{"category", "test"}})["total"] == 0);
    CHECK(
      fixture
        .callExpectingError(
          "preferences_set", Json{{"values", Json{{"Test/Host flag", true}}}})
        .code
      == ErrorCode::ObjectNotFound);
  }
}

} // namespace tb::mcp
