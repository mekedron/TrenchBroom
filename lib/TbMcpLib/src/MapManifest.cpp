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

#include "mcp/MapManifest.h"

#include "mcp/JsonVm.h"

#include "vm/vec.h"

#include <fmt/format.h>
#include <fmt/std.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace tb::mcp
{
namespace
{

bool isSection(const std::string_view key)
{
  return key == "format" || key == "version"
         || std::ranges::find(ManifestSections, key) != ManifestSections.end();
}

/** An optional string member: absent or null -> nullopt. */
Result<std::optional<std::string>, std::string> optionalString(
  const Json& value, const std::string_view key)
{
  const auto* member = findMember(value, key);
  if (!member || member->is_null())
  {
    return std::optional<std::string>{};
  }
  if (!member->is_string())
  {
    return fmt::format("'{}' must be a string", key);
  }
  return std::optional<std::string>{member->get<std::string>()};
}

/** A field update: absent -> unchanged, null -> removed. */
Result<FieldUpdate<std::string>, std::string> stringUpdate(
  const Json& value, const std::string_view key)
{
  const auto* member = findMember(value, key);
  if (!member)
  {
    return FieldUpdate<std::string>{};
  }
  if (member->is_null())
  {
    return FieldUpdate<std::string>{std::optional<std::string>{}};
  }
  if (!member->is_string())
  {
    return fmt::format("'{}' must be a string or null", key);
  }
  return FieldUpdate<std::string>{member->get<std::string>()};
}

/** A required non-empty string member, or nullopt. */
std::optional<std::string> requiredString(const Json& value, const std::string_view key)
{
  const auto* member = findMember(value, key);
  if (!member || !member->is_string() || member->get<std::string>().empty())
  {
    return std::nullopt;
  }
  return member->get<std::string>();
}

std::string errorText(const auto& result)
{
  return std::get<std::string>(result.error());
}

template <typename T, typename F>
Result<std::vector<T>, std::string> readList(
  const Json& manifest, const std::string_view key, const F& read)
{
  auto result = std::vector<T>{};
  const auto* member = findMember(manifest, key);
  if (!member || member->is_null())
  {
    return result;
  }
  if (!member->is_array())
  {
    return fmt::format("'{}' must be an array", key);
  }
  for (size_t i = 0; i < member->size(); ++i)
  {
    auto item = read((*member)[i]);
    if (item.is_error())
    {
      return fmt::format("{}[{}]: {}", key, i, errorText(item));
    }
    result.push_back(std::move(item).value());
  }
  return result;
}

template <typename T, typename Key>
auto findByKey(std::vector<T>& items, const Key& key, std::string T::*member)
{
  return std::ranges::find_if(
    items, [&](const auto& item) { return item.*member == key; });
}

bool validSection(const std::string_view name)
{
  return std::ranges::find(ManifestSections, name) != ManifestSections.end();
}

} // namespace

bool MapManifest::empty() const
{
  return spaces.empty() && keyPoints.empty() && notes.empty() && cameras.empty()
         && extra.empty();
}

std::filesystem::path manifestPath(const std::filesystem::path& mapPath)
{
  auto result = mapPath;
  result.replace_extension(".mcp.json");
  return result;
}

Result<AgentCamera, std::string> cameraFromJson(const Json& value)
{
  if (!value.is_object())
  {
    return std::string{"a camera must be an object"};
  }

  auto camera = AgentCamera{};
  const auto projection = findMember(value, "projection");
  if (!projection || !projection->is_string())
  {
    return std::string{
      "a camera needs 'projection' (\"perspective\" or \"orthographic\")"};
  }
  if (*projection == "perspective")
  {
    camera.projection = CameraProjection::Perspective;
  }
  else if (*projection == "orthographic")
  {
    camera.projection = CameraProjection::Orthographic;
  }
  else
  {
    return fmt::format("unknown camera projection {}", projection->dump());
  }

  const auto vector = [&](const std::string_view key) -> std::optional<vm::vec3d> {
    const auto* member = findMember(value, key);
    return member ? vec3FromJson(*member) : std::nullopt;
  };
  const auto position = vector("position");
  const auto direction = vector("direction");
  const auto up = vector("up");
  if (!position || !direction || !up)
  {
    return std::string{"a camera needs 'position', 'direction' and 'up' as [x, y, z]"};
  }
  if (vm::length(*direction) < 1e-9 || vm::length(*up) < 1e-9)
  {
    return std::string{"a camera's 'direction' and 'up' must not be zero"};
  }
  camera.position = *position;
  camera.direction = vm::normalize(*direction);
  // make up orthogonal to the direction again after rounding
  const auto orthogonalUp = *up - camera.direction * vm::dot(*up, camera.direction);
  if (vm::length(orthogonalUp) < 1e-6)
  {
    return std::string{"a camera's 'up' must not be parallel to its 'direction'"};
  }
  camera.up = vm::normalize(orthogonalUp);

  const auto number = [&](const std::string_view key, double& target) -> bool {
    const auto* member = findMember(value, key);
    if (!member)
    {
      return true;
    }
    if (!member->is_number())
    {
      return false;
    }
    target = member->get<double>();
    return true;
  };
  if (
    !number("fov", camera.fov) || !number("zoom", camera.zoom)
    || !number("near", camera.nearPlane) || !number("far", camera.farPlane))
  {
    return std::string{"a camera's 'fov', 'zoom', 'near' and 'far' must be numbers"};
  }
  if (camera.fov <= 0.0 || camera.fov >= 180.0 || camera.zoom <= 0.0)
  {
    return std::string{"a camera's 'fov' must be in (0, 180) and 'zoom' positive"};
  }
  return camera;
}

Json toJson(const ManifestSpace& space)
{
  auto result = Json{{"id", space.id}};
  if (space.name)
  {
    result["name"] = *space.name;
  }
  if (space.purpose)
  {
    result["purpose"] = *space.purpose;
  }
  if (space.notes)
  {
    result["notes"] = *space.notes;
  }
  if (space.bounds)
  {
    result["bounds"] = toJson(*space.bounds);
  }
  return result;
}

Json toJson(const ManifestKeyPoint& keyPoint)
{
  auto result = Json{{"name", keyPoint.name}, {"position", toJson(keyPoint.position)}};
  if (keyPoint.note)
  {
    result["note"] = *keyPoint.note;
  }
  return result;
}

Json toJson(const ManifestCamera& camera)
{
  return Json{{"name", camera.name}, {"camera", toJson(camera.camera)}};
}

Json toJson(const MapManifest& manifest)
{
  auto result = Json{
    {"format", ManifestFormat},
    {"version", ManifestVersion},
  };
  const auto list = [](const auto& items) {
    auto json = Json::array();
    for (const auto& item : items)
    {
      json.push_back(toJson(item));
    }
    return json;
  };
  result["spaces"] = list(manifest.spaces);
  result["keyPoints"] = list(manifest.keyPoints);
  result["notes"] = manifest.notes;
  result["cameras"] = list(manifest.cameras);
  for (const auto& [key, value] : manifest.extra.items())
  {
    if (!isSection(key))
    {
      result[key] = value;
    }
  }
  return result;
}

Result<ManifestSpace, std::string> spaceFromJson(const Json& value)
{
  if (!value.is_object())
  {
    return std::string{"a space must be an object"};
  }
  auto id = requiredString(value, "id");
  if (!id)
  {
    return std::string{"a space needs a non-empty string 'id'"};
  }
  auto space = ManifestSpace{std::move(*id)};
  for (const auto& [key, target] :
       {std::pair{"name", &space.name},
        std::pair{"purpose", &space.purpose},
        std::pair{"notes", &space.notes}})
  {
    auto member = optionalString(value, key);
    if (member.is_error())
    {
      return errorText(member);
    }
    *target = std::move(member).value();
  }
  if (const auto* bounds = findMember(value, "bounds"); bounds && !bounds->is_null())
  {
    space.bounds = boxFromJson(*bounds);
    if (!space.bounds)
    {
      return std::string{"'bounds' must be {\"min\": [x, y, z], \"max\": [x, y, z]}"};
    }
  }
  return space;
}

Result<ManifestKeyPoint, std::string> keyPointFromJson(const Json& value)
{
  if (!value.is_object())
  {
    return std::string{"a key point must be an object"};
  }
  auto name = requiredString(value, "name");
  if (!name)
  {
    return std::string{"a key point needs a non-empty string 'name'"};
  }
  const auto* position = findMember(value, "position");
  const auto point = position ? vec3FromJson(*position) : std::nullopt;
  if (!point)
  {
    return std::string{"a key point needs 'position' as [x, y, z]"};
  }
  auto note = optionalString(value, "note");
  if (note.is_error())
  {
    return errorText(note);
  }
  return ManifestKeyPoint{std::move(*name), *point, std::move(note).value()};
}

Result<ManifestCamera, std::string> manifestCameraFromJson(const Json& value)
{
  if (!value.is_object())
  {
    return std::string{"a camera entry must be an object"};
  }
  auto name = requiredString(value, "name");
  if (!name)
  {
    return std::string{"a camera entry needs a non-empty string 'name'"};
  }
  const auto* cameraJson = findMember(value, "camera");
  if (!cameraJson)
  {
    return std::string{"a camera entry needs 'camera'"};
  }
  auto camera = cameraFromJson(*cameraJson);
  if (camera.is_error())
  {
    return errorText(camera);
  }
  return ManifestCamera{std::move(*name), std::move(camera).value()};
}

Result<MapManifest, std::string> manifestFromJson(const Json& value)
{
  if (!value.is_object())
  {
    return std::string{"the manifest must be a JSON object"};
  }
  if (const auto* format = findMember(value, "format");
      format && *format != std::string{ManifestFormat})
  {
    return fmt::format(
      "the manifest's format is {}, expected \"{}\"", format->dump(), ManifestFormat);
  }
  if (const auto* version = findMember(value, "version");
      version && (!version->is_number_integer() || version->get<int>() > ManifestVersion))
  {
    return fmt::format(
      "the manifest's version {} is not supported (at most {})",
      version->dump(),
      ManifestVersion);
  }

  auto manifest = MapManifest{};
  auto spaces = readList<ManifestSpace>(value, "spaces", spaceFromJson);
  if (spaces.is_error())
  {
    return errorText(spaces);
  }
  manifest.spaces = std::move(spaces).value();

  auto keyPoints = readList<ManifestKeyPoint>(value, "keyPoints", keyPointFromJson);
  if (keyPoints.is_error())
  {
    return errorText(keyPoints);
  }
  manifest.keyPoints = std::move(keyPoints).value();

  if (const auto* notes = findMember(value, "notes"); notes && !notes->is_null())
  {
    if (!notes->is_array())
    {
      return std::string{"'notes' must be an array of strings"};
    }
    for (const auto& note : *notes)
    {
      if (!note.is_string())
      {
        return std::string{"'notes' must be an array of strings"};
      }
      manifest.notes.push_back(note.get<std::string>());
    }
  }

  auto cameras = readList<ManifestCamera>(value, "cameras", manifestCameraFromJson);
  if (cameras.is_error())
  {
    return errorText(cameras);
  }
  manifest.cameras = std::move(cameras).value();

  for (const auto& [key, member] : value.items())
  {
    if (!isSection(key))
    {
      manifest.extra[key] = member;
    }
  }
  return manifest;
}

Result<std::optional<MapManifest>, std::string> readManifestFile(
  const std::filesystem::path& path)
{
  auto error = std::error_code{};
  if (!std::filesystem::exists(path, error))
  {
    return std::optional<MapManifest>{};
  }

  auto stream = std::ifstream{path, std::ios::binary};
  if (!stream)
  {
    return fmt::format("The manifest {} could not be opened.", path);
  }
  const auto text = std::string{std::istreambuf_iterator<char>{stream}, {}};
  if (stream.bad())
  {
    return fmt::format("The manifest {} could not be read.", path);
  }
  const auto json = parseJson(text);
  if (!json)
  {
    return fmt::format("The manifest {} is not valid JSON.", path);
  }
  auto manifest = manifestFromJson(*json);
  if (manifest.is_error())
  {
    return fmt::format("The manifest {} is invalid: {}.", path, errorText(manifest));
  }
  return std::optional<MapManifest>{std::move(manifest).value()};
}

Result<void, std::string> writeManifestFile(
  const std::filesystem::path& path, const MapManifest& manifest)
{
  auto temporary = path;
  temporary += ".tmp";
  {
    auto stream = std::ofstream{temporary, std::ios::binary | std::ios::trunc};
    stream << toJson(manifest).dump(2) << "\n";
    stream.close();
    if (!stream)
    {
      auto ignored = std::error_code{};
      std::filesystem::remove(temporary, ignored);
      return fmt::format("Could not write {}.", temporary);
    }
  }

  auto error = std::error_code{};
  std::filesystem::rename(temporary, path, error);
  if (error)
  {
    auto ignored = std::error_code{};
    std::filesystem::remove(temporary, ignored);
    return fmt::format("Could not write {}: {}", path, error.message());
  }
  return Result<void, std::string>{};
}

Result<SpaceUpdate, std::string> spaceUpdateFromJson(const Json& value)
{
  if (!value.is_object())
  {
    return std::string{"a space must be an object"};
  }
  auto id = requiredString(value, "id");
  if (!id)
  {
    return std::string{"a space needs a non-empty string 'id'"};
  }
  auto update = SpaceUpdate{std::move(*id)};
  for (const auto& [key, target] :
       {std::pair{"name", &update.name},
        std::pair{"purpose", &update.purpose},
        std::pair{"notes", &update.notes}})
  {
    auto field = stringUpdate(value, key);
    if (field.is_error())
    {
      return errorText(field);
    }
    *target = std::move(field).value();
  }
  if (const auto* bounds = findMember(value, "bounds"))
  {
    if (bounds->is_null())
    {
      update.bounds = std::optional<vm::bbox3d>{};
    }
    else if (const auto box = boxFromJson(*bounds))
    {
      update.bounds = std::optional<vm::bbox3d>{*box};
    }
    else
    {
      return std::string{
        "'bounds' must be {\"min\": [x, y, z], \"max\": [x, y, z]} or null"};
    }
  }
  return update;
}

Result<KeyPointUpdate, std::string> keyPointUpdateFromJson(const Json& value)
{
  if (!value.is_object())
  {
    return std::string{"a key point must be an object"};
  }
  auto name = requiredString(value, "name");
  if (!name)
  {
    return std::string{"a key point needs a non-empty string 'name'"};
  }
  auto update = KeyPointUpdate{std::move(*name)};
  if (const auto* position = findMember(value, "position"))
  {
    update.position = vec3FromJson(*position);
    if (!update.position)
    {
      return std::string{"'position' must be [x, y, z]"};
    }
  }
  auto note = stringUpdate(value, "note");
  if (note.is_error())
  {
    return errorText(note);
  }
  update.note = std::move(note).value();
  return update;
}

Result<ManifestUpdateResult, std::string> applyUpdate(
  MapManifest& manifest, const ManifestUpdate& update)
{
  for (const auto& section : update.replace)
  {
    if (!validSection(section))
    {
      return fmt::format("unknown section '{}'", section);
    }
  }
  const auto replaces = [&](const std::string_view section) {
    return std::ranges::find(update.replace, section) != update.replace.end();
  };

  auto result = ManifestUpdateResult{};
  auto updated = manifest;

  if (replaces("spaces"))
  {
    result.removed += updated.spaces.size();
    updated.spaces.clear();
  }
  if (replaces("keyPoints"))
  {
    result.removed += updated.keyPoints.size();
    updated.keyPoints.clear();
  }
  if (replaces("notes"))
  {
    result.removed += updated.notes.size();
    updated.notes.clear();
  }
  if (replaces("cameras"))
  {
    result.removed += updated.cameras.size();
    updated.cameras.clear();
  }

  for (const auto& change : update.spaces)
  {
    auto it = findByKey(updated.spaces, change.id, &ManifestSpace::id);
    if (it == updated.spaces.end())
    {
      updated.spaces.push_back(ManifestSpace{change.id});
      it = std::prev(updated.spaces.end());
    }
    if (change.name)
    {
      it->name = *change.name;
    }
    if (change.purpose)
    {
      it->purpose = *change.purpose;
    }
    if (change.notes)
    {
      it->notes = *change.notes;
    }
    if (change.bounds)
    {
      it->bounds = *change.bounds;
    }
    ++result.spaces;
  }

  for (const auto& change : update.keyPoints)
  {
    auto it = findByKey(updated.keyPoints, change.name, &ManifestKeyPoint::name);
    if (it == updated.keyPoints.end())
    {
      if (!change.position)
      {
        return fmt::format("the new key point '{}' needs a position", change.name);
      }
      updated.keyPoints.push_back(ManifestKeyPoint{change.name, *change.position});
      it = std::prev(updated.keyPoints.end());
    }
    if (change.position)
    {
      it->position = *change.position;
    }
    if (change.note)
    {
      it->note = *change.note;
    }
    ++result.keyPoints;
  }

  for (const auto& note : update.notes)
  {
    if (std::ranges::find(updated.notes, note) == updated.notes.end())
    {
      updated.notes.push_back(note);
      ++result.notes;
    }
  }

  for (const auto& camera : update.cameras)
  {
    if (auto it = findByKey(updated.cameras, camera.name, &ManifestCamera::name);
        it != updated.cameras.end())
    {
      it->camera = camera.camera;
    }
    else
    {
      updated.cameras.push_back(camera);
    }
    ++result.cameras;
  }

  const auto remove =
    [&](auto& items, const auto& keys, const auto& keyOf, const char* section) {
      for (const auto& key : keys)
      {
        const auto removed =
          std::erase_if(items, [&](const auto& item) { return keyOf(item) == key; });
        if (removed == 0)
        {
          result.notFound.push_back(fmt::format("{}:{}", section, key));
        }
        result.removed += removed;
      }
    };
  remove(
    updated.spaces, update.removeSpaces, [](const auto& s) { return s.id; }, "spaces");
  remove(
    updated.keyPoints,
    update.removeKeyPoints,
    [](const auto& k) { return k.name; },
    "keyPoints");
  remove(updated.notes, update.removeNotes, [](const auto& n) { return n; }, "notes");
  remove(
    updated.cameras,
    update.removeCameras,
    [](const auto& c) { return c.name; },
    "cameras");

  manifest = std::move(updated);
  return result;
}

// ManifestStore

ManifestStore::ManifestStore(const std::filesystem::path& mapPath)
  : m_mapPath{mapPath.is_absolute() ? mapPath : std::filesystem::path{}}
{
}

const std::filesystem::path& ManifestStore::mapPath() const
{
  return m_mapPath;
}

std::optional<std::filesystem::path> ManifestStore::filePath() const
{
  return m_mapPath.empty() ? std::nullopt : std::optional{manifestPath(m_mapPath)};
}

bool ManifestStore::pending() const
{
  return m_pending;
}

const std::optional<std::string>& ManifestStore::saveError() const
{
  return m_saveError;
}

Result<MapManifest, std::string> ManifestStore::get()
{
  if (m_manifest)
  {
    return *m_manifest;
  }
  if (const auto path = filePath())
  {
    auto read = readManifestFile(*path);
    if (read.is_error())
    {
      return errorText(read);
    }
    m_manifest = std::move(read).value().value_or(MapManifest{});
    return *m_manifest;
  }
  return MapManifest{};
}

Result<void, std::string> ManifestStore::set(MapManifest manifest)
{
  if (const auto path = filePath())
  {
    auto written = writeManifestFile(*path, manifest);
    if (written.is_error())
    {
      return written;
    }
    m_manifest = std::move(manifest);
    m_pending = false;
    return Result<void, std::string>{};
  }

  m_manifest = std::move(manifest);
  m_pending = true;
  return Result<void, std::string>{};
}

void ManifestStore::mapWasSaved(const std::filesystem::path& mapPath)
{
  const auto newPath = mapPath.is_absolute() ? mapPath : std::filesystem::path{};
  const auto moved = newPath != m_mapPath;
  if (!moved && !m_pending)
  {
    return;
  }
  m_saveError.reset();

  // read the manifest of the old file before switching to the new one
  if (!m_manifest && !m_mapPath.empty())
  {
    auto read = readManifestFile(manifestPath(m_mapPath));
    if (read.is_error())
    {
      m_saveError = errorText(read) + " It was not copied to the new map file.";
      m_mapPath = newPath;
      return;
    }
    m_manifest = std::move(read).value();
  }

  m_mapPath = newPath;
  if (!m_manifest || (m_manifest->empty() && !m_pending) || newPath.empty())
  {
    // nothing to carry: the new file's manifest (if any) applies
    if (!m_pending)
    {
      m_manifest.reset();
    }
    return;
  }

  if (auto written = writeManifestFile(manifestPath(newPath), *m_manifest);
      written.is_error())
  {
    m_saveError = errorText(written);
    m_pending = true;
    return;
  }
  m_pending = false;
}

void ManifestStore::mapWasLoaded(const std::filesystem::path& mapPath)
{
  m_mapPath = mapPath.is_absolute() ? mapPath : std::filesystem::path{};
  m_manifest.reset();
  m_pending = false;
  m_saveError.reset();
}

} // namespace tb::mcp
