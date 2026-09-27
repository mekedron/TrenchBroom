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

#include "base/Result.h"
#include "mcp/AgentCamera.h"
#include "mcp/Json.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tb::mcp
{

// The map manifest (E12.10): the agent's notes about a map, kept in a file next to the
// map (`<name>.mcp.json` for `<name>.map`): spaces with their purpose, key points, notes,
// named agent cameras and the validators turned off. It is not part of the map or its
// undo history.

/** The value of the manifest file's "format" member. */
inline constexpr auto ManifestFormat = std::string_view{"trenchbroom-mcp-manifest"};
inline constexpr auto ManifestVersion = 1;

/** The manifest's sections, in file order. */
inline constexpr auto ManifestSections =
  std::array<std::string_view, 4>{"spaces", "keyPoints", "notes", "cameras"};

/** A space (room, area) of the map, e.g. one found by spaces_list. */
struct ManifestSpace
{
  /** The key: a space id from spaces_list or the agent's own id. */
  std::string id;
  std::optional<std::string> name = std::nullopt;
  std::optional<std::string> purpose = std::nullopt;
  std::optional<std::string> notes = std::nullopt;
  std::optional<vm::bbox3d> bounds = std::nullopt;

  bool operator==(const ManifestSpace&) const = default;
};

/** A named point, e.g. the player start of a scene or a place for an NPC. */
struct ManifestKeyPoint
{
  /** The key. */
  std::string name;
  vm::vec3d position = vm::vec3d{0, 0, 0};
  std::optional<std::string> note = std::nullopt;

  bool operator==(const ManifestKeyPoint&) const = default;
};

/** A named agent camera. */
struct ManifestCamera
{
  /** The key. */
  std::string name;
  AgentCamera camera = {};

  bool operator==(const ManifestCamera&) const = default;
};

struct MapManifest
{
  std::vector<ManifestSpace> spaces;
  std::vector<ManifestKeyPoint> keyPoints;
  std::vector<std::string> notes;
  std::vector<ManifestCamera> cameras;
  /**
   * The validators and MCP checks turned off with validators_set (member
   * "disabledValidators", written only when not empty); applied when the document is
   * opened.
   */
  std::vector<std::string> disabledValidators;
  /** Top-level members this version does not know; kept when the file is rewritten. */
  Json extra = Json::object();

  bool empty() const;

  bool operator==(const MapManifest&) const = default;
};

/** The manifest file of a map: `/x/foo.map` -> `/x/foo.mcp.json`. */
std::filesystem::path manifestPath(const std::filesystem::path& mapPath);

/**
 * Reads a camera in the form of toJson(AgentCamera): projection, position, direction,
 * up, fov or zoom, near, far. Directions are normalized. Returns an error message if
 * the value is not such a camera.
 */
Result<AgentCamera, std::string> cameraFromJson(const Json& value);

Json toJson(const ManifestSpace& space);
Json toJson(const ManifestKeyPoint& keyPoint);
Json toJson(const ManifestCamera& camera);
/**
 * `{"format", "version", "spaces", "keyPoints", "notes", "cameras",
 * "disabledValidators"?, ...extra}`
 */
Json toJson(const MapManifest& manifest);

Result<ManifestSpace, std::string> spaceFromJson(const Json& value);
Result<ManifestKeyPoint, std::string> keyPointFromJson(const Json& value);
Result<ManifestCamera, std::string> manifestCameraFromJson(const Json& value);
/** Parses a manifest; missing sections are empty, a different format is an error. */
Result<MapManifest, std::string> manifestFromJson(const Json& value);

/**
 * Reads a manifest file. Returns nullopt if the file does not exist, and an error
 * message if it cannot be read or is not a valid manifest.
 */
Result<std::optional<MapManifest>, std::string> readManifestFile(
  const std::filesystem::path& path);

/** Writes a manifest file atomically (a temporary file renamed over the file). */
Result<void, std::string> writeManifestFile(
  const std::filesystem::path& path, const MapManifest& manifest);

/** A field of an update: nullopt = unchanged, a nullopt value = removed. */
template <typename T>
using FieldUpdate = std::optional<std::optional<T>>;

/** Changes to a space, merged by id. */
struct SpaceUpdate
{
  std::string id;
  FieldUpdate<std::string> name = std::nullopt;
  FieldUpdate<std::string> purpose = std::nullopt;
  FieldUpdate<std::string> notes = std::nullopt;
  FieldUpdate<vm::bbox3d> bounds = std::nullopt;
};

/** Changes to a key point, merged by name; a new key point needs a position. */
struct KeyPointUpdate
{
  std::string name;
  std::optional<vm::vec3d> position = std::nullopt;
  FieldUpdate<std::string> note = std::nullopt;
};

struct ManifestUpdate
{
  std::vector<SpaceUpdate> spaces;
  std::vector<KeyPointUpdate> keyPoints;
  std::vector<std::string> notes;
  std::vector<ManifestCamera> cameras;

  /** Sections replaced instead of merged ("spaces", "keyPoints", "notes", "cameras"). */
  std::vector<std::string> replace;

  std::vector<std::string> removeSpaces;
  std::vector<std::string> removeKeyPoints;
  std::vector<std::string> removeNotes;
  std::vector<std::string> removeCameras;
};

/** Reads a space update: {id, name?, purpose?, notes?, bounds?}, null removes a field. */
Result<SpaceUpdate, std::string> spaceUpdateFromJson(const Json& value);
/** Reads a key point update: {name, position?, note?}, null removes the note. */
Result<KeyPointUpdate, std::string> keyPointUpdateFromJson(const Json& value);

/** What applyUpdate did. */
struct ManifestUpdateResult
{
  /** Entries added or changed, per section. */
  size_t spaces = 0;
  size_t keyPoints = 0;
  size_t notes = 0;
  size_t cameras = 0;
  /** Entries removed. */
  size_t removed = 0;
  /** Entries to remove that did not exist, as "<section>:<key>". */
  std::vector<std::string> notFound;
};

/**
 * Applies an update. Sections in `replace` are cleared first. Spaces are merged by id and
 * key points by name (present fields replace the stored ones, cleared fields are
 * removed; a new key point needs a position), notes are appended unless the same text
 * exists, cameras are merged by name. Removals run last. Returns an error if a new key
 * point has no position.
 */
Result<ManifestUpdateResult, std::string> applyUpdate(
  MapManifest& manifest, const ManifestUpdate& update);

/**
 * The manifest of one document. It is read lazily from the file next to the map; for a
 * map without a file it is kept in memory and written when the map is saved. Saving the
 * map under another name carries the manifest to the new name.
 */
class ManifestStore
{
private:
  /** The map's absolute path, empty if the map was never saved. */
  std::filesystem::path m_mapPath;
  /** The manifest, once read or set. */
  std::optional<MapManifest> m_manifest;
  /** The manifest was set but not written (unsaved map or failed write). */
  bool m_pending = false;
  /** The last problem while carrying or writing the manifest on a save. */
  std::optional<std::string> m_saveError;

public:
  /** `mapPath` is empty or relative for a map that was never saved. */
  explicit ManifestStore(const std::filesystem::path& mapPath = {});

  const std::filesystem::path& mapPath() const;
  /** The manifest file, or nullopt for a map that was never saved. */
  std::optional<std::filesystem::path> filePath() const;
  /** Whether the manifest is kept in memory until the map is saved. */
  bool pending() const;
  /** The last problem while writing the manifest on a save, cleared by the next save. */
  const std::optional<std::string>& saveError() const;

  /**
   * The manifest: the one set before, else the file's (read once), else an empty one.
   * Fails if the file is not a valid manifest.
   */
  Result<MapManifest, std::string> get();

  /**
   * Replaces the manifest and writes it if the map has a file; otherwise keeps it in
   * memory until the map is saved.
   */
  Result<void, std::string> set(MapManifest manifest);

  /**
   * The map was saved, possibly under a new path: writes a pending manifest, and carries
   * a manifest (read from the old file if needed) to the new path. A manifest that cannot
   * be read or written is left alone and reported by saveError().
   */
  void mapWasSaved(const std::filesystem::path& mapPath);

  /** Another map was loaded into the document: forgets the manifest. */
  void mapWasLoaded(const std::filesystem::path& mapPath);
};

} // namespace tb::mcp
