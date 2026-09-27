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
#include "mcp/tools/Manual.h"
#include "mcp/tools/PreferenceCatalog.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/Tag.h"
#include "mdl/TagManager.h"
#include "mdl/TagMatcher.h"
#include "prefs/Preferences.h"
#include "ui/ActionManager.h"
#include "ui/AppController.h"
#include "ui/AppControllerFixture.h"
#include "ui/CatchConfig.h"
#include "ui/MapDocument.h"
#include "ui/MapDocumentFixture.h"
#include "ui/McpPreferenceHost.h"

#include "kd/invoke.h"

#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::ui
{
namespace
{

using Catch::Matchers::StartsWith;

const mcp::HostPreference* find(
  const std::vector<mcp::HostPreference>& preferences, const std::string& path)
{
  for (const auto& preference : preferences)
  {
    if (mcp::preferencePath(preference.preference).generic_string() == path)
    {
      return &preference;
    }
  }
  return nullptr;
}

} // namespace

TEST_CASE("McpPreferenceHost")
{
  auto appControllerFixture = AppControllerFixture{};
  auto& appController = appControllerFixture.appController();
  auto host = McpPreferenceHost{appController};

  SECTION("MCP preferences")
  {
    const auto preferences = host.preferences(nullptr);

    const auto* port = find(preferences, "MCP/Port");
    REQUIRE(port);
    CHECK(port->category == "mcp");
    CHECK(port->maximum == 65535.0);
    CHECK(!port->lockedReason.empty());

    for (const auto& path : {"MCP/Enabled", "MCP/Bind address", "MCP/Access token"})
    {
      const auto* preference = find(preferences, path);
      REQUIRE(preference);
      CHECK(!preference->lockedReason.empty());
    }
    CHECK(find(preferences, "MCP/Access token")->secret);

    for (const auto& path : {"MCP/Log to file", "MCP/Busy wait timeout"})
    {
      const auto* preference = find(preferences, path);
      REQUIRE(preference);
      CHECK(preference->lockedReason.empty());
      CHECK(!preference->secret);
    }
  }

  SECTION("action shortcuts")
  {
    const auto preferences = host.preferences(nullptr);

    auto paths = std::set<std::string>{};
    auto keyboardCount = size_t{0};
    for (const auto& preference : preferences)
    {
      CHECK(
        paths.insert(mcp::preferencePath(preference.preference).generic_string()).second);
      CHECK(!preference.description.empty());
      if (preference.category == "keyboard")
      {
        ++keyboardCount;
        CHECK(std::holds_alternative<Preference<std::vector<KeySequence>>*>(
          preference.preference));
      }
    }

    // every action of the action manager: menu and map view actions
    CHECK(keyboardCount == appController.actionManager().actionsMap().size());

    const auto* newDocument = find(preferences, "Menu/File/New");
    REQUIRE(newDocument);
    CHECK(newDocument->category == "keyboard");
    CHECK(newDocument->description == "Menu: File > New Document");

    const auto* moveObjects = find(preferences, "Menu/Edit/Move objects");
    REQUIRE(moveObjects);
    CHECK(moveObjects->description == "Menu: Edit > Transform > Move...");

    const auto* cycleView = find(preferences, "Controls/Map view/Cycle map view");
    REQUIRE(cycleView);
    CHECK_THAT(cycleView->description, StartsWith("Map view: "));

    // the preferences are the actions' own preferences
    auto& shortcut =
      *std::get<Preference<std::vector<KeySequence>>*>(newDocument->preference);
    const auto original = pref(shortcut);
    const auto restore = kdl::invoke_later{[&]() { setPref(shortcut, original); }};
    setPref(shortcut, std::vector<KeySequence>{KeySequence{"Ctrl+Alt+N"}});

    const auto& action = appController.actionManager().actionsMap().at(
      std::filesystem::path{"Menu/File/New"});
    CHECK(&action.preference() == &shortcut);
    CHECK(
      pref(action.preference()) == std::vector<KeySequence>{KeySequence{"Ctrl+Alt+N"}});
  }

  SECTION("tag and entity definition shortcuts of a document")
  {
    auto config = mdl::QuakeFixtureConfig;
    config.gameInfo.gameConfig.smartTags = {
      mdl::SmartTag{
        "trigger",
        {},
        std::make_unique<mdl::EntityClassNameTagMatcher>("trigger*", "trigger"),
      },
    };

    auto documentFixture = MapDocumentFixture{};
    auto& document = documentFixture.create(config);
    auto& map = document.map();

    const auto& tags = map.tagManager().smartTags();
    const auto& definitions = map.entityDefinitionManager().definitions();
    REQUIRE(!tags.empty());

    const auto withDocument = host.preferences(&document);
    const auto withoutDocument = host.preferences(nullptr);
    CHECK(withDocument.size() > withoutDocument.size());

    const auto documentPreferences = host.preferences(&document);
    for (const auto& tag : tags)
    {
      const auto* toggle =
        find(documentPreferences, "Filters/Tags/" + tag.name() + "/Toggle Visible");
      REQUIRE(toggle);
      CHECK(toggle->category == "keyboard");
      CHECK_THAT(toggle->description, StartsWith("Smart tag: "));
    }
    for (const auto& definition : definitions)
    {
      const auto* toggle =
        find(documentPreferences, "Entities/" + definition.name + "/Toggle");
      REQUIRE(toggle);
      CHECK_THAT(toggle->description, StartsWith("Entity definition: "));
    }

    // the pointers stay valid until the next call
    const auto* toggle = find(
      documentPreferences, "Filters/Tags/" + tags.front().name() + "/Toggle Visible");
    auto& shortcut = *std::get<Preference<std::vector<KeySequence>>*>(toggle->preference);
    // restored through a copy: the next call destroys the tag actions and their
    // preferences
    auto restorePreference = shortcut;
    const auto original = pref(shortcut);
    const auto restore =
      kdl::invoke_later{[&]() { setPref(restorePreference, original); }};
    setPref(shortcut, std::vector<KeySequence>{KeySequence{"Ctrl+Alt+T"}});

    // tag actions created again read the changed preference
    const auto again = host.preferences(&document);
    const auto* toggleAgain =
      find(again, "Filters/Tags/" + tags.front().name() + "/Toggle Visible");
    CHECK(
      pref(*std::get<Preference<std::vector<KeySequence>>*>(toggleAgain->preference))
      == std::vector<KeySequence>{KeySequence{"Ctrl+Alt+T"}});
  }
}

TEST_CASE("McpManual")
{
  // The manual that the build generates for the editor; see GenerateManual.cmake
  const auto path = std::filesystem::path{MCP_TEST_MANUAL_PATH};
  if (!std::filesystem::exists(path))
  {
    SKIP("The manual was not generated; build the GenerateManual target");
  }

  auto loaded = mcp::loadManual(path);
  REQUIRE(loaded.is_success());
  const auto& manual = loaded.value()->manual;

  CHECK_THAT(manual.title, StartsWith("TrenchBroom"));
  CHECK(manual.sections.size() > 150);

  auto ids = std::set<std::string>{};
  for (const auto& section : manual.sections)
  {
    CHECK(!section.id.empty());
    CHECK(!section.title.empty());
    CHECK(ids.insert(section.id).second);
  }
  for (const auto& id :
       {"introduction", "getting_started", "camera_navigation", "selection", "editing"})
  {
    CHECK(manual.find(id));
  }
  CHECK(!manual.sections[*manual.find("camera_navigation")].text.empty());

  // Every reference to a menu item or an action names a shortcut preference of the editor
  // (the action manager's actions or a static key preference), and every key is in the
  // manual's key table
  REQUIRE(loaded.value()->shortcuts);
  const auto& table = *loaded.value()->shortcuts;
  CHECK(table.menu.size() > 50);

  auto appControllerFixture = AppControllerFixture{};
  auto host = McpPreferenceHost{appControllerFixture.appController()};
  auto shortcutPaths = std::set<std::string>{};
  for (const auto& preference : host.preferences(nullptr))
  {
    shortcutPaths.insert(mcp::preferencePath(preference.preference).generic_string());
  }
  for (const auto* preference : Preferences::keyPreferences())
  {
    shortcutPaths.insert(preference->path.generic_string());
  }

  auto referenceCount = size_t{0};
  auto unresolved = std::vector<std::string>{};
  for (const auto& section : manual.sections)
  {
    mcp::resolveManualText(
      section.text, [&](const mcp::ManualReference kind, const std::string& key) {
        ++referenceCount;
        const auto known = kind == mcp::ManualReference::Key
                             ? table.keys.contains(key)
                             : shortcutPaths.contains(key);
        if (!known)
        {
          unresolved.push_back(key);
        }
        return std::string{};
      });
  }
  CHECK(referenceCount > 200);
  CHECK(unresolved == std::vector<std::string>{});
}

} // namespace tb::ui
