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

#include "mcp/Resources.h"

#include "mcp/McpServer.h"
#include "mcp/ResourceRegistry.h"
#include "mcp/ServerState.h"
#include "mcp/tools/DocumentTools.h"
#include "mcp/tools/EntityClassTools.h"
#include "mcp/tools/GameTools.h"
#include "mcp/tools/MaterialTools.h"
#include "mcp/tools/SceneTools.h"
#include "mcp/tools/SelectionTools.h"
#include "mcp/tools/SessionTools.h"
#include "mcp/tools/ValidationTools.h"
#include "mdl/GameInfo.h"
#include "mdl/Map.h"
#include "tools/ToolUtils.h"
#include "ui/MapDocument.h"

#include <algorithm>

namespace tb::mcp
{
namespace
{

constexpr auto AgentGuide = R"(# TrenchBroom MCP server — agent guide

Tool names are in backticks; each tool's description has its arguments and an example.
The prompts (prompts/list) walk through whole tasks: blockout, population, lighting,
texturing, fixing, compiling, explaining and cleaning up a map.

## Conventions
- Map units; Z is up. Angles are in degrees. Yaw 0 looks along +X (east), 90 along +Y
  (north), counterclockwise seen from above; pitch > 0 looks up. North is +Y everywhere
  (compass annotations, `surroundings`, `map_plan_view`, the walls of `room_create`).
- An entity's 'angle' is its yaw (`entity_create_point` "angle"). Classes that read
  'angles' ("pitch yaw roll") need it set with `entity_properties_set`.
- Ids: 'brush:1042', 'entity:7', 'group:3', 'layer:5', 'layer:default', 'world'; faces
  'brush:1042/face:3'; spaces 'space:…'; snapshots 'snap:3'; compile runs 'run:1'. Ids
  survive undo and redo. A face index is valid until its brush's geometry changes: read
  the faces again after editing a brush.
- Tools that act on objects take 'ids'; without ids they act on the current selection.
- Grid: `grid_get` / `grid_set` (powers of two). Every modifying call reports the grid in
  effect. Brush coordinates are used exactly as given: keep them on the grid (multiples of
  8 or 16) so that vertices stay integers; `entity_create_point` snaps positions itself.
- Material names ignore case. Unknown materials, classes, properties and values are
  warnings, never errors.

## Player dimensions
Used by the eyeHeight camera helper, the "player" annotation, `spaces_list` cells (half
the player width) and `walkable_plan`. Units; "~" marks approximate engine values.

| Game family  | Player box W×H | Eye | Step | Jump      | Doorway (typical) |
|--------------|----------------|-----|------|-----------|-------------------|
| Quake        | 32×56          | 46  | 18   | ~45       | ~64×96            |
| Quake 2      | 32×56          | 46  | 18   | ~45       | ~64×96            |
| Half-Life    | 32×72          | 64  | 18   | ~45 (~63 crouch jump) | 64×96 or wider |
| Quake 3      | 30×56          | 50  | 18   | ~45       | ~96×128           |
| other games  | 32×56          | 48  | 18   | ~45       | ~64×96            |

- The family comes from the game name or its compile tools: Half-Life tools (csg, bsp,
  vis, rad), Quake tools (qbsp, vis, light), Quake 2 tools with a Quake 2 format (e.g.
  Heretic 2, SoF), q3map2 with baseq3. All other games use the last row.
- `walkable_plan` assumes step 18 and jump 45 (pass jumpHeight 63 for Half-Life crouch
  jumps) and walks on faces with normal z ≥ 0.7 (slopes up to ~45°, as the engines do).
- Derive the rest from the player: corridors at least 3 player widths wide, rooms 2–3
  player heights tall, walls 8–16 thick.

## Workflow
1. Orient: `editor_status`, `game_info`, `compile_tools_get`. On an existing map:
   `map_summary`, `map_manifest_get` {"restoreCameras": true}, `spaces_list`.
2. New map: `document_new` (a Valve 220 format where the game has one), delete its
   'initialObjects' (the template brush) with `objects_delete`, then `document_save_as` at
   once: compiling needs a saved file, and unsaved work is lost if the editor restarts.
3. Materials: `materials_collections_get` / `materials_collections_set` (WADs or folders).
4. Blockout: `room_create` per room, `opening_cut` for doorways and windows,
   `brush_create_shape` (stairs, arches, cylinders), `brush_create_box`,
   `brush_create_hull`. Put rooms into layers (`layer_create`) and multi-brush objects into
   named groups (`group_create`). Repeats (column rings, spiral stairs, rows):
   `objects_array` (its count includes the original). Use "dryRun": true when unsure.
   Save after each step.
5. Placement: `spaces_list`, `surroundings`, `free_spots` (floor, wall, ceiling spots with
   an 'origin' for point entities), `space_check` for a box, `ray_pick`.
6. Entities: `entity_classes_list`, `entity_class_describe` (size, model, properties,
   spawnflags), then `entity_create_point` with "dropToFloor": true, or
   `entity_create_brush` from brushes. `entity_spawnflags_set` sets flags by name,
   `entity_link` connects target and targetname.
7. Look: `view_snapshot` with agent cameras (`agent_camera_set`), `view_snapshots_around`,
   `view_pick` (what a pixel shows), `view_snapshot_compare` (what a change did).
8. Texture: `material_usage`, `material_apply`, `uv_align` {"operation": "typical"}, then
   `uv_check` {"scope": "map"}.
9. Check: `map_check`, `issues_list`, `issue_fix`.
10. Compile and test: `compile_run`, poll `compile_status`, `pointfile_load` on a leak,
    `engine_launch`.
11. Record: `map_manifest_set` (spaces with purpose, key points, notes, cameras); it is
    written next to the map on save.

## Changing the map
- Every modifying call is one undo step 'AI: <tool title>' and atomic: on failure the map
  is unchanged. "dryRun": true reports the changes and issues without changing anything.
- `transaction_begin` / `transaction_commit` make several calls one undo step;
  `transaction_rollback` discards them. Keep transactions short (see pitfalls).
- Results list 'changes' (created / modified / removed ids), 'selection',
  'issuesIntroduced' (editor validators and the MCP checks: Z_FIGHTING,
  ENTITY_OUTSIDE_HULL, MODEL_*, UV_ASPECT_DISTORTION), 'warnings', 'grid' and 'console'
  (warnings and errors the editor logged meanwhile). Fix introduced issues right away.
- Errors carry a code, a message, the object ids and a hint. Lists are paginated: pass
  'nextCursor' as 'cursor'.
- While the user drags or has a dialog open, modifying calls wait.
- `undo`, `redo`, `history_get`; `command_repeat` repeats the recorded transforms on the
  current selection.

## Seeing your work
- Judge rooms, props and textures from images, not coordinates. `view_snapshot` renders
  offscreen from your own camera and never touches the user's views, selection or
  filters. Without a camera it frames the whole map from above.
- `agent_camera_set` {"name": ..., "camera": {...}} names cameras: perspective
  (position + lookAt / yaw / pitch), orthographic (top, front, side), or the helpers
  frame, orbit and eyeHeight (stand on the floor below a point at the game's eye height).
  Reuse one camera per room.
- "options" hide helpers: {"hideTags": ["trigger", "clip"], "hideClassnames": [...]};
  wireframe shows rooms from outside. "isolate" and "highlight" (top-level arguments)
  show only or mark given ids. "annotations" add labels, a coordinate grid, a compass and
  a player box for scale.
- `view_snapshot` {"keepAs": "before"} … edit … `view_snapshot_compare` {"before":
  "before"}, or {"undoSteps": 1}: both images and a mask of the changed pixels.
- `view_pick` turns snapshot pixels into object, face, point and normal.
- `map_plan_view` draws a top-down plan (text or image); `walkable_plan` shows where the
  player can walk from the start.
- `view_snapshot_user` shows what the user sees. `camera_focus`, `camera_set` and
  `camera_step_pointfile` move the user's views: only to show the user something.

## Entities and models
- `entity_model_info` lists a model's animations with their real bounds and the property
  that selects them; `entity_animation_set` switches pose. The class box is often not the
  model: "dropUsing": "model" rests the model on the floor.
- `entity_placement_check` {"scope": "map", "onlyProblems": true} finds models in brushes
  or floating; apply 'suggestedMove' with `objects_move`.
- A null property value removes the key, including defaults the game adds on creation.
- `entity_links_get` {"brokenOnly": true} finds broken target links.

## Materials
- Find with `materials_list`, look with `material_preview`. `material_usage` tells a
  material's kind (panel, tile, trim, …) and typical scale from notes, the scanned
  original maps (`material_corpus_scan`) and the image; record facts with
  `material_notes_set`.
- Apply with `material_apply` (faces or whole objects), swap in bulk with
  `material_replace`, set the material of new brushes with `material_set_current`.
- Align with `uv_align` (typical, fit, justify, resetToWorld, …), `uv_nudge`,
  `face_attributes_set` and `face_attributes_copy`. Panels fit whole:
  `material_fit_geometry` gives the face size. Never distort the aspect.
- Smart tags: `tags_list`, `tag_apply` (e.g. turn a brush into a trigger), `tag_remove`.

## Checking, compiling, testing
- `map_check`: entities in walls or floating, a missing player start, broken links,
  missing materials, entities outside rooms, each with a ready 'suggestedFix' call.
- `issues_list` adds the editor validators; `issue_fix` applies their quick fixes (fixing
  by code may delete objects: check with dryRun). Z_FIGHTING and ENTITY_OUTSIDE_HULL need
  your own edits. `issue_hide` accepts an issue; `validators_set` turns checks off.
- `compile_presets_list` (fast / normal / full per tool chain), `compile_run` returns a
  run handle at once; poll `compile_status`. On a leak, `pointfile_load` returns the path
  and where it leaves the map; close the gap, compile again, `pointfile_unload`.
- `engine_profiles_list`, `engine_profile_save`, `engine_launch` start the game with the
  last compiled map. `console_read` shows the editor's log.

## Pitfalls
- Reloading a document (`document_revert`) invalidates brush, entity and face ids (layer
  and group ids stay); after an editor restart look every id up again (`objects_find`).
- Adjacent rooms: outer faces may touch but walls must not overlap, or the faces
  z-fight (Z_FIGHTING). A floor under a doorway belongs to one room only. End supports
  2 units below the surface they carry.
- `opening_cut` without ids cuts every editable brush in the box; list the wall brushes
  of both rooms to protect other objects.
- A point entity outside the sealed hull makes the map leak (ENTITY_OUTSIDE_HULL); while
  the blockout is still open, `validators_set` can turn that check off.
- A transaction belongs to the session that opened it: closing the session rolls it back,
  and after a reconnect the new session gets TRANSACTION_ACTIVE until the old one closes.
- A never-saved map cannot compile; unsaved changes of a saved map are compiled. The
  engine plays the last compiled .bsp: compile again after edits.
- Snapshot ids and `view_pick` use the current map: take a new snapshot after edits.
- Agent cameras and kept snapshots belong to the MCP session and are gone after a
  reconnect; keep cameras in the manifest (`map_manifest_set` {"saveCameras": "all"}) and
  restore them with `map_manifest_get` {"restoreCameras": true}.
- The editor preview is not the game: models show their default pose, render modes
  (transparency, invisible triggers) and light are not shown. Judge them in the game.
- Hidden objects and tags (`visibility_set`, `view_options_set`, hidden layers) are still
  compiled; only layers with omitFromExport are left out.
- `brush_create_box` faces are 0 -x, 1 -y, 2 -z, 3 +z, 4 +y, 5 +x (faces are sorted by
  normal). Read other brushes' faces with `object_get` {"fields": ["faces.id",
  "faces.normal"]}.
- Round shapes and rotations can give non-integer vertices (NON_INTEGER_VERTICES):
  `vertices_snap` {"mode": "integer"}.
- `preferences_set`, `grid_set`, `view_options_set` and `view_layout_set` are not undoable
  and change the user's editor: tell the user.

## More
- Editor menus and shortcuts: `actions_list`, `action_invoke`. Preferences:
  `preferences_get`, `preferences_set`. How TrenchBroom works: `manual_search`,
  `manual_section`, trenchbroom://manual.
- Import from other maps: `map_file_inspect`, `map_import`; clipboard: `clipboard_copy`,
  `clipboard_paste`. Layers and groups: `layers_list`, `layer_set_state`,
  `objects_move_to_layer`, `linked_group_duplicate`.
- Resources: trenchbroom://editor/status, trenchbroom://documents/{doc}/summary,
  selection, issues, entity-definitions, materials, info; trenchbroom://console;
  trenchbroom://compile/{run}/log. Subscribe to follow the user's changes; updates are
  coalesced. Event streams cannot be resumed with Last-Event-ID: read the resources again
  after reconnecting.
)";

/** Lists one entry per open document for a document resource template. */
ResourceLister documentLister(
  const DocumentAspect aspect, std::string name, std::string title)
{
  return [=](ServerState& state, Session&) {
    auto entries = std::vector<Json>{};
    for (const auto& document : state.host.documents())
    {
      entries.push_back(Json{
        {"uri", ServerState::documentResourceUri(document.id, aspect)},
        {"name", name + "-" + document.id},
        {"title", title + ": " + document.windowTitle},
        {"mimeType", "application/json"},
      });
    }
    return entries;
  };
}

/** Reads a document resource: finds the document named by {doc} and describes it. */
ResourceReader documentReader(
  std::function<Json(ServerState&, const DocumentInfo&, Session&)> describe)
{
  return [describe = std::move(describe)](
           ServerState& state,
           Session& session,
           const std::string& uri,
           const ResourceVariables& vars) -> Result<Json, ToolError> {
    const auto documentId = vars.at("doc");
    if (const auto document = state.findDocument(documentId))
    {
      return jsonResourceContents(uri, describe(state, *document, session));
    }
    return makeError(
      ErrorCode::DocumentNotFound,
      "Document " + documentId + " is not open.",
      "Use document_list to see the open documents.");
  };
}

} // namespace

std::string_view agentGuide()
{
  return AgentGuide;
}

void registerResources(McpServer& server)
{
  auto& resources = server.resources();

  resources.add(ResourceDef{
    "trenchbroom://editor/status",
    "editor-status",
    "Editor Status",
    "Active document, open documents, current tool, grid, locks and selection summary. "
    "Subscribe to get notified when documents are opened, closed or saved, or the tool, "
    "locks, open transaction or selection change.",
    "application/json",
    [](ServerState& state, Session& session, const std::string& uri, const auto&)
      -> Result<Json, ToolError> {
      return jsonResourceContents(uri, editorStatus(state, session));
    },
  });

  resources.addTemplate(ResourceTemplateDef{
    "trenchbroom://documents/{doc}/info",
    "document-info",
    "Document Info",
    "Path, game, format, modified flag, game folder, mods, entity definitions, material "
    "collections and soft bounds of an open document ({doc} is a handle such as "
    "doc:1). Subscribe to get notified when it is saved, reloaded, modified or its "
    "settings change.",
    "application/json",
    documentReader(
      [](ServerState& state, const DocumentInfo& document, Session& session) {
        return documentInfo(state, document, session);
      }),
    documentLister(DocumentAspect::Info, "document-info", "Document Info"),
  });

  resources.addTemplate(ResourceTemplateDef{
    "trenchbroom://documents/{doc}/summary",
    "map-summary",
    "Map Summary",
    "Overview of an open document's map, as returned by map_summary: object counts, "
    "entities by class, layers, materials, bounds and issue count ({doc} is a handle "
    "such as doc:1). Subscribe to get notified when objects are added, removed or "
    "changed.",
    "application/json",
    documentReader([](ServerState& state, const DocumentInfo& document, Session&) {
      auto& map = document.document->map();
      return mapSummary(map, state.documentState(*document.document).ids);
    }),
    documentLister(DocumentAspect::Summary, "map-summary", "Map Summary"),
  });

  resources.addTemplate(ResourceTemplateDef{
    "trenchbroom://documents/{doc}/selection",
    "selection",
    "Selection",
    "The current selection of an open document (objects or faces) with a short "
    "description of each selected item, at most 100 ({doc} is a handle such as doc:1). "
    "Subscribe to get notified when the selection or the selected objects change.",
    "application/json",
    documentReader([](ServerState& state, const DocumentInfo& document, Session&) {
      const auto& map = document.document->map();
      return selectionDetails(map, state.documentState(*document.document).ids, 100);
    }),
    documentLister(DocumentAspect::Selection, "selection", "Selection"),
  });

  resources.addTemplate(ResourceTemplateDef{
    "trenchbroom://documents/{doc}/entity-definitions",
    "entity-definitions",
    "Entity Definitions",
    "The entity classes an open document can use, from its current definition file "
    "(which depends on the document's mods and chosen FGD/DEF/ENT file): spec, count, "
    "and per class name, type, group, first description line, size (point classes), "
    "property keys and spawnflag names ({doc} is a handle such as doc:1). Use "
    "entity_class_describe for the details of a class. Subscribe to get notified when "
    "the definitions are reloaded or replaced.",
    "application/json",
    documentReader([](ServerState&, const DocumentInfo& document, Session&) {
      auto result = entityDefinitionsResource(document.document->map());
      result["document"] = document.id;
      return result;
    }),
    documentLister(
      DocumentAspect::EntityDefinitions, "entity-definitions", "Entity Definitions"),
  });

  resources.addTemplate(ResourceTemplateDef{
    "trenchbroom://documents/{doc}/materials",
    "materials",
    "Materials",
    "The materials an open document can use: its material collections (WAD files or "
    "folders) with material counts, and per material name, collection, width and "
    "height in pixels (null while the image is not loaded yet), sorted by name ({doc} "
    "is a handle such as doc:1). Use materials_list for usage counts and filters. "
    "Subscribe to get notified when the collections are loaded, reloaded or changed.",
    "application/json",
    documentReader([](ServerState&, const DocumentInfo& document, Session&) {
      auto result = materialsResource(document.document->map());
      result["document"] = document.id;
      return result;
    }),
    documentLister(DocumentAspect::Materials, "materials", "Materials"),
  });

  resources.addTemplate(ResourceTemplateDef{
    "trenchbroom://documents/{doc}/issues",
    "issues",
    "Issues",
    "The current problems of an open document, as issues_list returns them without "
    "filters: the editor validators' issues (hidden ones excluded) and the MCP checks "
    "(z-fighting, entities outside the hull, model placement, texture distortion), "
    "with total, counts per code and at most 200 items; validators turned off with "
    "validators_set are skipped ({doc} is a handle such as doc:1). Subscribe to get "
    "notified when objects change, issues are hidden or shown, or validators are "
    "turned on or off.",
    "application/json",
    documentReader([](ServerState& state, const DocumentInfo& document, Session&) {
      return issuesResource(state, document);
    }),
    documentLister(DocumentAspect::Issues, "issues", "Issues"),
  });

  resources.addTemplate(ResourceTemplateDef{
    "trenchbroom://games/{game}/config",
    "game-config",
    "Game Configuration",
    "A game's configuration: map formats, file system, material setup, entity "
    "definition files, smart tags, surface and content flags, soft bounds and compile "
    "tools ({game} is the percent-encoded game name, e.g. Quake%202). Updated when the "
    "game folder changes.",
    "application/json",
    [](ServerState& state, Session&, const std::string& uri, const auto& vars)
      -> Result<Json, ToolError> {
      const auto gameName = percentDecode(vars.at("game"));
      const auto* gameInfo = gameName ? findGame(state.host, *gameName) : nullptr;
      if (!gameInfo)
      {
        return unknownGameError(state.host, gameName.value_or(vars.at("game")));
      }
      return jsonResourceContents(uri, gameConfigJson(*gameInfo));
    },
    [](ServerState& state, Session&) {
      // only the games of open documents, to keep the list short
      auto names = std::vector<std::string>{};
      for (const auto& document : state.host.documents())
      {
        const auto& name = document.document->map().gameInfo().gameConfig.name;
        if (std::ranges::find(names, name) == names.end())
        {
          names.push_back(name);
        }
      }

      auto entries = std::vector<Json>{};
      for (const auto& name : names)
      {
        entries.push_back(Json{
          {"uri", gameConfigUri(name)},
          {"name", "game-config-" + percentEncode(name)},
          {"title", "Game Configuration: " + name},
          {"mimeType", "application/json"},
        });
      }
      return entries;
    },
  });

  resources.add(ResourceDef{
    "trenchbroom://guide",
    "agent-guide",
    "Agent Guide",
    "How to use this server well: units, axes, yaw and ids, the player dimensions of "
    "each "
    "game family (box, eye height, step, jump, doorway), the recommended workflow from "
    "setup to compiling, dry runs and transactions, how to check your work with "
    "snapshots "
    "and checks, and common pitfalls.",
    "text/markdown",
    [](ServerState&, Session&, const std::string& uri, const auto&)
      -> Result<Json, ToolError> {
      return Json::array({Json{
        {"uri", uri},
        {"mimeType", "text/markdown"},
        {"text", AgentGuide},
      }});
    },
  });
}

} // namespace tb::mcp
