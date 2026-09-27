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

#pragma once

#include "mcp/Host.h"
#include "mcp/Json.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tb::mdl
{
class GameManager;
}

namespace tb::mcp
{

/** Where a preference comes from. */
enum class PreferenceSource
{
  /** A static editor preference (prefs/Preferences.h). */
  Editor,
  /** A preference of a configured game: game path, default engine, tool paths. */
  Game,
  /** A preference that only the host knows (PreferenceHost). */
  Host,
};

/** A preference with what preferences_get reports about it. */
struct PreferenceInfo
{
  AnyPreference preference;
  /** e.g. "view", "colors", "camera", "keyboard", "games", "mcp" */
  std::string category;
  std::string description;
  PreferenceSource source = PreferenceSource::Editor;
  /** The values a string or integer preference accepts; empty if not limited. */
  std::vector<Json> allowedValues = {};
  /** The range of a numeric preference, if limited. */
  std::optional<double> minimum = std::nullopt;
  std::optional<double> maximum = std::nullopt;
  /** Reported when the preference is changed, e.g. "Takes effect after a restart." */
  std::string note = {};
  /** If not empty, agents cannot change the preference, for this reason. */
  std::string lockedReason = {};
  /** Whether the value is a secret that is not reported. */
  bool secret = false;

  const std::filesystem::path& path() const;

  PreferenceInfo withValues(std::vector<Json> values) const;
  PreferenceInfo withRange(double min, double max) const;
  PreferenceInfo withNote(std::string text) const;
};

/**
 * The editor's static preferences (prefs/Preferences.h), each path once. The occluded
 * move trace color shares its path with the move trace color and is not listed
 * separately.
 */
const std::vector<PreferenceInfo>& editorPreferences();

/**
 * The preferences of all configured games: the game path, the default engine and the
 * paths of the compilation tools (category "games").
 */
std::vector<PreferenceInfo> gamePreferences(mdl::GameManager& gameManager);

/**
 * All preferences: the editor's, the games' and the host's (with the document's tag and
 * entity definition shortcuts if a document is given), each path once. The host's
 * preference pointers stay valid until the host is asked again.
 */
std::vector<PreferenceInfo> allPreferences(McpHost& host, ui::MapDocument* document);

/** "bool", "int", "float", "string", "path", "color" or "shortcuts". */
std::string preferenceTypeName(const AnyPreference& preference);

const std::filesystem::path& preferencePath(const AnyPreference& preference);

PreferencePersistencePolicy preferencePersistence(const AnyPreference& preference);

} // namespace tb::mcp
