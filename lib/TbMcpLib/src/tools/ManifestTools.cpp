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

#include "mcp/tools/ManifestTools.h"

#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/MapManifest.h"
#include "mcp/Schema.h"
#include "mcp/ServerState.h"
#include "mcp/Session.h"
#include "mcp/ToolRegistry.h"

#include "kd/string_utils.h"

#include <fmt/format.h>
#include <fmt/std.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace tb::mcp
{
namespace
{
using namespace schema;

std::vector<std::string> sectionNames()
{
  return {ManifestSections.begin(), ManifestSections.end()};
}

std::string errorText(const auto& result)
{
  return std::get<std::string>(result.error());
}

ToolError invalidFileError(const std::string& message)
{
  return makeError(
    ErrorCode::IoError,
    message,
    "Fix or delete the file, or rewrite it with map_manifest_set and overwriteInvalid: "
    "true (its content is then lost).");
}

Json pathJson(const ManifestStore& store)
{
  const auto path = store.filePath();
  return path ? Json(path->string()) : Json(nullptr);
}

bool fileExists(const ManifestStore& store)
{
  auto error = std::error_code{};
  const auto path = store.filePath();
  return path && std::filesystem::exists(*path, error);
}

Json counts(const MapManifest& manifest)
{
  return Json{
    {"spaces", manifest.spaces.size()},
    {"keyPoints", manifest.keyPoints.size()},
    {"notes", manifest.notes.size()},
    {"cameras", manifest.cameras.size()},
  };
}

std::string sessionCameraNames(const Session& session)
{
  auto names = std::vector<std::string>{};
  for (const auto& [name, camera] : session.agentCameras)
  {
    names.push_back(name);
  }
  return names.empty() ? std::string{"none"} : kdl::str_join(names, ", ");
}

void warnSaveError(CallContext& context, const ManifestStore& store)
{
  if (const auto& error = store.saveError())
  {
    context.warn("MANIFEST_NOT_WRITTEN", *error);
  }
}

// map_manifest_get

ToolResult manifestGet(CallContext& context, const Args& args)
{
  auto& store = context.documentState().manifest;
  auto manifest = store.get();
  if (manifest.is_error())
  {
    return invalidFileError(errorText(manifest));
  }
  warnSaveError(context, store);

  const auto sections = args.getOr<std::vector<std::string>>("sections", sectionNames());
  const auto wants = [&](const std::string_view section) {
    return std::ranges::find(sections, section) != sections.end();
  };

  auto result = Json{
    {"path", pathJson(store)},
    {"exists", fileExists(store)},
    {"pending", store.pending()},
  };
  const auto list = [](const auto& items) {
    auto json = Json::array();
    for (const auto& item : items)
    {
      json.push_back(toJson(item));
    }
    return json;
  };
  const auto& value = manifest.value();
  if (wants("spaces"))
  {
    result["spaces"] = list(value.spaces);
  }
  if (wants("keyPoints"))
  {
    result["keyPoints"] = list(value.keyPoints);
  }
  if (wants("notes"))
  {
    result["notes"] = value.notes;
  }
  if (wants("cameras"))
  {
    result["cameras"] = list(value.cameras);
  }

  // restore saved cameras into the session
  if (const auto restore = args.getOptional<Json>("restoreCameras");
      restore && *restore != Json(false))
  {
    auto names = std::vector<std::string>{};
    if (restore->is_array())
    {
      names = restore->get<std::vector<std::string>>();
    }
    else
    {
      for (const auto& camera : value.cameras)
      {
        names.push_back(camera.name);
      }
    }

    auto& cameras = context.session().agentCameras;
    auto restored = Json::array();
    for (const auto& name : names)
    {
      const auto it = std::ranges::find_if(
        value.cameras, [&](const auto& camera) { return camera.name == name; });
      if (it == value.cameras.end())
      {
        context.warn(
          "UNKNOWN_CAMERA", fmt::format("The manifest has no camera '{}'.", name), {});
        continue;
      }
      if (!cameras.contains(name) && cameras.size() >= Session::MaxAgentCameras)
      {
        context.warn(
          "CAMERA_LIMIT",
          fmt::format(
            "The camera '{}' was not restored: a session can have at most {} agent "
            "cameras. Delete some with agent_camera_delete.",
            name,
            Session::MaxAgentCameras));
        continue;
      }
      cameras[name] = it->camera;
      restored.push_back(name);
    }
    result["restoredCameras"] = std::move(restored);
  }
  return result;
}

// map_manifest_set

Result<ManifestUpdate, ToolError> manifestUpdate(CallContext& context, const Args& args)
{
  auto update = ManifestUpdate{};
  const auto invalid = [](const std::string& message) {
    return makeError(
      ErrorCode::InvalidArgument,
      message,
      "E.g. {\"spaces\": [{\"id\": \"space:1\", \"name\": \"Hall\", \"purpose\": "
      "\"arrival\"}], \"keyPoints\": [{\"name\": \"altar\", \"position\": [0, 128, "
      "0]}]}");
  };

  for (const auto& space : args.getOr<std::vector<Json>>("spaces", {}))
  {
    auto parsed = spaceUpdateFromJson(space);
    if (parsed.is_error())
    {
      return invalid("spaces: " + errorText(parsed));
    }
    update.spaces.push_back(std::move(parsed).value());
  }
  for (const auto& keyPoint : args.getOr<std::vector<Json>>("keyPoints", {}))
  {
    auto parsed = keyPointUpdateFromJson(keyPoint);
    if (parsed.is_error())
    {
      return invalid("keyPoints: " + errorText(parsed));
    }
    update.keyPoints.push_back(std::move(parsed).value());
  }
  update.notes = args.getOr<std::vector<std::string>>("notes", {});
  update.replace = args.getOr<std::vector<std::string>>("replace", {});

  if (const auto remove = args.getOptional<Json>("remove"))
  {
    const auto names = [&](const std::string_view key) {
      const auto* member = findMember(*remove, key);
      return member ? member->get<std::vector<std::string>>()
                    : std::vector<std::string>{};
    };
    update.removeSpaces = names("spaces");
    update.removeKeyPoints = names("keyPoints");
    update.removeNotes = names("notes");
    update.removeCameras = names("cameras");
  }

  if (const auto save = args.getOptional<Json>("saveCameras"))
  {
    const auto& session = context.session();
    auto names = std::vector<std::string>{};
    if (save->is_string())
    {
      for (const auto& [name, camera] : session.agentCameras)
      {
        names.push_back(name);
      }
    }
    else
    {
      names = save->get<std::vector<std::string>>();
    }
    for (const auto& name : names)
    {
      const auto it = session.agentCameras.find(name);
      if (it == session.agentCameras.end())
      {
        return makeError(
          ErrorCode::InvalidArgument,
          fmt::format("This session has no agent camera '{}'.", name),
          fmt::format(
            "Create it with agent_camera_set first. Cameras: {}.",
            sessionCameraNames(session)));
      }
      update.cameras.push_back(ManifestCamera{name, it->second});
    }
  }
  return update;
}

ToolResult manifestSet(CallContext& context, const Args& args)
{
  auto& store = context.documentState().manifest;

  auto update = manifestUpdate(context, args);
  if (update.is_error())
  {
    return errorOf(update);
  }

  auto current = store.get();
  auto manifest = MapManifest{};
  if (current.is_error())
  {
    if (!args.get<bool>("overwriteInvalid"))
    {
      return invalidFileError(errorText(current));
    }
    context.warn(
      "MANIFEST_OVERWRITTEN",
      errorText(current) + " It is replaced because overwriteInvalid is set.");
  }
  else
  {
    manifest = std::move(current).value();
  }

  auto applied = applyUpdate(manifest, update.value());
  if (applied.is_error())
  {
    return makeError(
      ErrorCode::InvalidArgument,
      errorText(applied),
      "Give new key points a position; 'replace' takes section names.");
  }
  const auto& changes = applied.value();
  for (const auto& entry : changes.notFound)
  {
    context.warn(
      "MANIFEST_ENTRY_NOT_FOUND",
      fmt::format("The manifest has no entry {} to remove.", entry));
  }

  auto savedCameras = Json::array();
  for (const auto& camera : update.value().cameras)
  {
    savedCameras.push_back(camera.name);
  }

  auto result = Json{
    {"path", pathJson(store)},
    {"written", false},
    {"pending", false},
    {"changed",
     Json{
       {"spaces", changes.spaces},
       {"keyPoints", changes.keyPoints},
       {"notes", changes.notes},
       {"cameras", changes.cameras},
     }},
    {"removed", changes.removed},
    {"notFound", changes.notFound},
    {"savedCameras", std::move(savedCameras)},
    {"counts", counts(manifest)},
  };

  if (context.dryRun())
  {
    const auto path = store.filePath();
    result["wouldDo"] =
      path ? fmt::format("write the manifest {}", *path)
           : std::string{"keep the manifest in memory until the map is saved"};
    result["pending"] = !path.has_value();
    return result;
  }

  if (auto stored = store.set(std::move(manifest)); stored.is_error())
  {
    return makeError(
      ErrorCode::IoError, errorText(stored), "Check that the map's folder is writable.");
  }
  result["written"] = !store.pending();
  result["pending"] = store.pending();
  if (store.pending())
  {
    context.warn(
      "MANIFEST_PENDING",
      "The map has never been saved, so the manifest is kept in memory and written "
      "next to the map when it is saved (document_save_as or the editor's Save).");
  }
  return result;
}

Schema cameraEntrySchema()
{
  return object({
    field("name", string()).required(),
    field("camera", any()).required().describe("As agent_camera_get returns it"),
  });
}

} // namespace

void registerManifestTools(ToolRegistry& registry)
{
  const auto sections = sectionNames();

  registry.add(
    ToolDef{"map_manifest_get"}
      .title("Get Map Manifest")
      .description(
        "Returns the map's manifest: the agent's notes about the map, kept across "
        "sessions in a file next to it (<name>.mcp.json for <name>.map) and not "
        "part of the map: spaces (id, name, purpose, notes, bounds), keyPoints "
        "(name, position, note), notes (texts) and named agent cameras. Read it "
        "when you start working on a map. Does not change the map; restoreCameras "
        "(true or a list of names) loads saved cameras into this session's agent "
        "cameras for view_snapshot. For a map that was never saved the manifest is "
        "kept in memory (pending: true) until the map is saved. Update it with "
        "map_manifest_set. Example: {\"sections\": [\"spaces\", \"cameras\"], "
        "\"restoreCameras\": true}")
      .input(object({
        field("sections", array(enumOf(sections)).nonEmpty())
          .describe(
            "Only these sections (spaces, keyPoints, notes, cameras). Default: all"),
        field(
          "restoreCameras",
          oneOf({
            boolean().describe("true: restore all saved cameras"),
            array(string().describe("A saved camera name"))
              .nonEmpty()
              .describe("Restore only these cameras"),
          }))
          .describe(
            "true: load all saved cameras into this session's agent cameras; a list: "
            "only these. Existing session cameras of the same name are replaced"),
      }))
      .output(object({
        field("path", any()).required().describe("The manifest file, or null"),
        field("exists", boolean()).required().describe("Whether the file exists"),
        field("pending", boolean())
          .required()
          .describe("Kept in memory until the map is saved"),
        field("spaces", array(any())).describe("{id, name, purpose, notes, bounds}"),
        field("keyPoints", array(any())).describe("{name, position, note}"),
        field("notes", array(string())).describe("Free-text notes"),
        field("cameras", array(cameraEntrySchema())).describe("Saved agent cameras"),
        field("restoredCameras", array(string()))
          .describe("Cameras loaded into this session's agent cameras"),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .handler(manifestGet));

  registry.add(
    ToolDef{"map_manifest_set"}
      .title("Update Map Manifest")
      .description(
        "Updates the map's manifest (see map_manifest_get) and writes "
        "<name>.mcp.json next to the map, or keeps it in memory until a "
        "never-saved map is saved (saving under a new name carries it along). Not "
        "undoable and not part of the map. Sections merge by default: spaces by id "
        "(a spaces_list id or your own; given fields replace stored ones, null "
        "removes a field), keyPoints by name (new ones need a position), notes are "
        "appended unless the text exists; 'replace' lists sections that are "
        "replaced instead (cleared if not given). saveCameras: \"all\" or names of "
        "this session's agent cameras (agent_camera_set) to store, replacing saved "
        "cameras of the same name. 'remove' deletes entries by id, name or text. A "
        "manifest file that is not valid is only overwritten with "
        "overwriteInvalid: true. Returns changed, removed, notFound, savedCameras "
        "and counts per section. Example: {\"spaces\": [{\"id\": \"space:2\", "
        "\"name\": \"Armory\", \"purpose\": \"weapons, ambush from the "
        "balcony\"}], \"keyPoints\": [{\"name\": \"ambush\", \"position\": [320, "
        "64, 128], \"note\": \"monsters wait here\"}], \"saveCameras\": "
        "[\"armory\"]}")
      .input(object({
        field(
          "spaces",
          array(object({field("id", string()).required().describe("The space id")})
                  .allowAdditionalProperties()
                  .describe("{id, name?, purpose?, notes?, bounds?}")))
          .describe(
            "Spaces to add or change, by id: {id, name?, purpose?, notes?, bounds?: "
            "{min, max}}; null removes a field"),
        field(
          "keyPoints",
          array(
            object({field("name", string()).required().describe("The key point name")})
              .allowAdditionalProperties()
              .describe("{name, position?, note?}")))
          .describe(
            "Key points to add or change, by name: {name, position?: [x, y, z] (needed "
            "for new ones), note?}; a null note removes it"),
        field("notes", array(string())).describe("Notes to append"),
        field("replace", array(enumOf(sections)))
          .describe(
            "Sections to replace instead of merging (a listed section that is not given "
            "is cleared)"),
        field(
          "saveCameras",
          oneOf({
            enumOf({"all"}).describe("All agent cameras of this session"),
            array(string().describe("An agent camera name"))
              .nonEmpty()
              .describe("Only these agent cameras"),
          }))
          .describe("\"all\" or names of this session's agent cameras to save"),
        field(
          "remove",
          object({
            field("spaces", array(string())).describe("Space ids"),
            field("keyPoints", array(string())).describe("Key point names"),
            field("notes", array(string())).describe("Note texts"),
            field("cameras", array(string())).describe("Saved camera names"),
          }))
          .describe("Entries to remove, by id, name or text"),
        field("overwriteInvalid", boolean().defaultsTo(false))
          .describe("Replace a manifest file that is not valid (its content is lost)"),
      }))
      .output(object({
        field("path", any()).required().describe("The manifest file, or null"),
        field("written", boolean()).required().describe("The file was written"),
        field("pending", boolean())
          .required()
          .describe("Kept in memory until the map is saved"),
        field("changed", any()).required().describe("Entries added or changed"),
        field("removed", integer()).required().describe("Number of entries removed"),
        field("notFound", array(string()))
          .required()
          .describe("Keys in 'remove' that matched no entry"),
        field("savedCameras", array(string())).required().describe("Cameras stored"),
        field("counts", any()).required().describe("Entries per section afterwards"),
        field("wouldDo", string()).describe("Dry run only: what the call would do"),
      }))
      .mutation(Mutation::External)
      .documentUse(DocumentUse::Required)
      .handler(manifestSet));
}

} // namespace tb::mcp
