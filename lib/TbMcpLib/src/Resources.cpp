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

Conventions
- All coordinates and lengths are in map units; Z is up. Angles are in degrees.
- Object ids look like 'brush:1042', 'entity:7', 'group:3', 'layer:5', 'layer:default',
  'world'. Faces are 'brush:1042/face:3'. Ids stay valid while the object exists,
  including across undo and redo. They do not survive reloading a document.
- Tools that act on objects take 'ids'; without ids they act on the current selection.

Changing the map
- Every modifying call is one undo step named 'AI: <tool title>' and is atomic: if it
  fails, the map is unchanged.
- Every modifying call accepts 'dryRun': it reports the changes and introduced issues
  without changing anything.
- Group several calls into one undo step with transaction_begin / transaction_commit,
  or discard them with transaction_rollback.
- If the user is dragging or has a dialog open, modifying calls wait until they finish.

Understanding the map
- Start with map_summary, then narrow down with objects_find (filters such as
  classname, material, layer, region) and object_get for details. map_tree shows the
  hierarchy, map_plan_view a top-down text plan of a region.
- Spatial questions: ray_pick ({"from": "entity:12"} finds what is below an entity),
  objects_at_point, and space_check before placing entities or rooms.
- Selection tools (selection_set, select_by, ...) change the editor's selection; each
  call is an undo step, like selecting in the editor.
- Subscribe to trenchbroom://documents/{doc}/summary and .../selection to learn about
  changes the user makes; updates are coalesced.

Entities
- Find a class with entity_classes_list (prefix such as "monster_", group, search),
  then read entity_class_describe for its size, model, properties, choices and
  spawnflags before placing it.
- Place point entities with entity_create_point; dropToFloor puts them on the floor
  below. Set spawnflags by name with entity_spawnflags_set, and connect entities
  (target / targetname) with entity_link.
- Unknown classes, unknown property keys and invalid values are reported as warnings;
  they never block a call.
- trenchbroom://documents/{doc}/entity-definitions lists all classes of a document.

Materials and faces
- Material names are case-insensitive. Find materials with materials_list (search
  "wall_*", usedOnly, includeMissing for materials the map uses but that are not
  loaded) and look at one with material_preview, which returns a small image.
- material_apply puts a material on faces ('brush:12/face:3') or on all faces of
  brushes, groups and entities; material_set_current sets the material of new
  brushes. Unknown materials are applied anyway with an UNKNOWN_MATERIAL warning.
- material_replace swaps materials by name or pattern in the selection, the map, a
  layer ({"layer": "Castle"}) or given ids: {"from": "wall_old*", "to": "wall_new*"}
  fills each wildcard of 'to' with the text matched in 'from'. It keeps the
  alignment, skips hidden and locked faces, and reports targets that are not loaded
  in 'unmatched' instead of guessing. Check the result with dryRun first.
- trenchbroom://documents/{doc}/materials lists all loaded materials with their sizes.
- face_attributes_get reads offset, scale, rotation, flags, value and color; its
  'format' block says whether the map format saves flags and colors and lists the
  game's flag names. face_attributes_set takes flag names or raw bits ({"add":
  ["slick"]}), relative offsetBy / scaleBy / rotateBy, and 'unset' to return to the
  material's defaults.
- face_attributes_copy works like the editor's alt-click: "project", "rotate" (wraps
  around corners, Valve 220 only) or "material" only.
- uv_align: justify, align, fit (repeatU / repeatV for an exact tiling, needs a loaded
  material), autoFit, reset, resetToWorld, flip, rotate90. uv_nudge moves or rotates
  in each face's texture axes, not relative to a camera.
- Smart tags: tags_list shows the game's object tags (trigger, detail) and face tags
  (clip, skip, hint) with what they match. tag_apply is "Turn into <tag>", tag_remove
  "Make non-<tag>"; 'option' picks one choice when a tag offers several.

Layers, groups and visibility
- layers_list shows the layers with their state; layer_set_state sets current, hidden,
  locked, omitFromExport or isolate. New objects go into the current layer; move
  existing ones with objects_move_to_layer.
- group_create / group_ungroup / groups_merge / group_add_objects /
  group_remove_objects manage groups; group_open enters a group for editing like a
  double-click, group_close leaves it.
- linked_group_duplicate makes linked copies (count, offset); editing one copy updates
  the others when the call commits, and ids in the other copies stay valid.
  linked_group_select, linked_group_separate and linked_group_extract manage link sets.
- visibility_set hides, shows or isolates objects; "show_all" shows every object.

Clipboard and import
- clipboard_copy / clipboard_cut return map text and keep it in the server's own
  clipboard (not the system clipboard); clipboard_paste pastes it (or given text) at
  the original position, at a 'position' with an 'anchor', or by an 'offset'. Face
  text applies its attributes to the target faces.
- To bring content from another map, call map_file_inspect on the file to see its
  layers, groups and classnames, then map_import with a filter (layer, group,
  classname, region), a position and a targetLayer. The format is converted, missing
  materials are reported and the imported objects are selected. Check the space first
  with space_check.

Compiling
- compile_tools_get shows the game's compile tool paths and whether each is an
  executable file; set them with compile_tools_set (Half-Life: csg/bsp/vis/rad from
  VHLT or ZHLT; Quake: qbsp/vis/light from ericw-tools; Quake 3: q3map2).
- compile_presets_list shows the fast / normal / full tool chains for the game; each
  exports the map (including unsaved changes) to <map folder>/compile/, runs the tools
  and copies the .bsp into <game>/<mod>/maps. compile_profiles_list and
  compile_profile_save manage the editor's own compile profiles.
- compile_run {"preset": "normal"} starts in the background and returns a run handle
  such as 'run:1' at once; poll compile_status (state, current task, errors, warnings,
  leak, compiled file) or subscribe to trenchbroom://compile/{run}/log. "test": true
  only prints the commands. One compile runs per document at a time; compile_cancel
  stops it.
- On a leak, compile_status names the point file: pointfile_load returns the leak path,
  the entities near its ends and where it leaves the map; close the gap there and
  compile again. portalfile_load shows the portals written by vis.

Looking at your work
- Check what you built with images. view_snapshot renders the map offscreen from your
  own camera and returns a PNG; it never moves the user's camera or changes their view
  filters, hidden objects or selection, and it does not wait for the user.
- Without a camera, view_snapshot frames the whole map from above at an angle. Name
  cameras with agent_camera_set: a perspective camera from 'position' plus 'lookAt',
  'direction' or 'yaw' / 'pitch'; an orthographic 'view' (top, front, side) with
  'center' and 'zoom'; or a helper: 'frame' (ids or a box), 'orbit' (target, yaw,
  pitch, distance) or 'eyeHeight' (a point inside a room: the camera stands on the
  floor below at the player's eye height). Eye heights above the floor: Quake and
  Quake 2 46 (player 56 tall, 32 wide), Half-Life 64 (player 72 x 32), Quake 3 50
  (player 56 x 30), other games 48.
- Options per snapshot: faceMode (textured, flat, wireframe), shading, fog, edges,
  hideTags (e.g. ["trigger", "clip"]), hideClassnames, pointEntities, brushEntities,
  patches, entityModels, includeHidden, isolate (only these ids), highlight (ids in a
  color), bounds, classnames, entityLinks, leakPath, grid, axes. Wireframe with
  hideTags shows inside rooms from outside.
- view_snapshots_around renders several labelled views around objects or a box (e.g.
  north, east, south, west, above). map_plan_view {"format": "image"} draws a top-down
  plan of a height slice with entities marked.
- To see what a change did, keep a snapshot ({"keepAs": "before"}), change the map, then
  call view_snapshot_compare {"before": "before"}; or compare with the state before the
  last undo steps: {"undoSteps": 1}. It returns both images side by side and a mask of
  the changed pixels.
- view_snapshot_user captures what the user currently sees in one of their views and
  returns that view's camera.

Editor console
- console_read returns the messages the editor logs (material, model and definition
  load errors, compile and export messages, ...), with level, time and document. Pass
  the returned 'lastSeq' as 'after' to fetch only newer messages; filter with minLevel,
  text (or a regex) and document. Subscribe to trenchbroom://console to be notified of
  new messages. console_clear clears the buffer and the editor's console views.
- Every call result lists under 'console' the warnings and errors logged while it ran.

Results
- Modifying calls return 'changes' (created / modified / removed ids), 'selection',
  'issuesIntroduced', 'warnings' and the grid size in effect.
- Errors carry a code, a message, the involved object ids and a hint for the next step.
- List results are paginated: pass 'cursor' from 'nextCursor' to get the next page.

Resumability
- Server-sent event streams cannot be resumed with Last-Event-ID. After reconnecting,
  read the resources again.
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
    "How to use this server well: conventions, ids, transactions, dry runs.",
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
