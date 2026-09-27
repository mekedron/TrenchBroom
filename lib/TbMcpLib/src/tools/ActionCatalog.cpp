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

#include "mcp/tools/ActionCatalog.h"

#include <algorithm>
#include <set>

namespace tb::mcp
{
namespace
{

using Tools = std::vector<std::string_view>;

ActionClass invoke(Tools tools = {})
{
  return {ActionHandling::Invoke, {}, std::move(tools), {}};
}

ActionClass dialog(std::string_view kind, Tools tools, std::string_view reason)
{
  return {ActionHandling::Dialog, kind, std::move(tools), reason};
}

ActionClass refuse(Tools tools, std::string_view reason)
{
  return {ActionHandling::Refuse, {}, std::move(tools), reason};
}

// The paths are the actions' preference paths as ActionManager.cpp defines them. The
// coverage test in TbMcpUiLibTest checks that every action of the registry has an entry
// and that every entry still exists in the registry.
std::vector<ActionCatalogEntry> createCatalog()
{
  const auto moveObjects = Tools{"objects_move", "vertices_move"};
  const auto duplicateAndMove = Tools{"objects_duplicate"};
  const auto rotateObjects = Tools{"objects_rotate"};
  const auto nudgeUv = Tools{"uv_nudge"};
  const auto viewOptions = Tools{"view_options_set"};
  const auto grid = Tools{"grid_set"};
  // MapViewBase::showAllEntityLinks() and its siblings store the link mode in the face
  // render mode preference
  const auto entityLinkBug = std::string_view{
    "stores the entity link mode in the face render mode preference (an editor bug), "
    "which breaks face rendering; view_options_set sets the link mode correctly"};

  return {
    // Map view: tool specific actions
    {"Controls/Map view/Create brush", invoke({"brush_create_hull"})},
    {"Controls/Map view/Toggle clip side", invoke({"brush_clip"})},
    {"Controls/Map view/Perform clip", invoke({"brush_clip"})},
    {"Controls/Map view/Perform sweep", invoke({"face_extrude_new"})},
    {"Controls/Map view/Decrease sweep scale", invoke()},
    {"Controls/Map view/Increase sweep scale", invoke()},

    // Map view: translation, duplication, rotation
    {"Controls/Map view/Move objects up; Move objects forward", invoke(moveObjects)},
    {"Controls/Map view/Move objects down; Move objects backward", invoke(moveObjects)},
    {"Controls/Map view/Move objects left", invoke(moveObjects)},
    {"Controls/Map view/Move objects right", invoke(moveObjects)},
    {"Controls/Map view/Move objects backward; Move objects up", invoke(moveObjects)},
    {"Controls/Map view/Move objects forward; Move objects down", invoke(moveObjects)},
    {"Controls/Map view/Duplicate and move objects up; Duplicate and move objects "
     "forward",
     invoke(duplicateAndMove)},
    {"Controls/Map view/Duplicate and move objects down; Duplicate and move objects "
     "backward",
     invoke(duplicateAndMove)},
    {"Controls/Map view/Duplicate and move objects left", invoke(duplicateAndMove)},
    {"Controls/Map view/Duplicate and move objects right", invoke(duplicateAndMove)},
    {"Controls/Map view/Duplicate and move objects backward; Duplicate and move objects "
     "up",
     invoke(duplicateAndMove)},
    {"Controls/Map view/Duplicate and move objects forward; Duplicate and move objects "
     "down",
     invoke(duplicateAndMove)},
    {"Controls/Map view/Roll objects clockwise", invoke(rotateObjects)},
    {"Controls/Map view/Roll objects counter-clockwise", invoke(rotateObjects)},
    {"Controls/Map view/Yaw objects clockwise", invoke(rotateObjects)},
    {"Controls/Map view/Yaw objects counter-clockwise", invoke(rotateObjects)},
    {"Controls/Map view/Pitch objects clockwise", invoke(rotateObjects)},
    {"Controls/Map view/Pitch objects counter-clockwise", invoke(rotateObjects)},

    // Map view: texturing
    {"Controls/Map view/Move textures up", invoke(nudgeUv)},
    {"Controls/Map view/Move textures up (coarse)", invoke(nudgeUv)},
    {"Controls/Map view/Move textures up (fine)", invoke(nudgeUv)},
    {"Controls/Map view/Move textures down", invoke(nudgeUv)},
    {"Controls/Map view/Move textures down (coarse)", invoke(nudgeUv)},
    {"Controls/Map view/Move textures down (fine)", invoke(nudgeUv)},
    {"Controls/Map view/Move textures left", invoke(nudgeUv)},
    {"Controls/Map view/Move textures left (coarse)", invoke(nudgeUv)},
    {"Controls/Map view/Move textures left (fine)", invoke(nudgeUv)},
    {"Controls/Map view/Move textures right", invoke(nudgeUv)},
    {"Controls/Map view/Move textures right (coarse)", invoke(nudgeUv)},
    {"Controls/Map view/Move textures right (fine)", invoke(nudgeUv)},
    {"Controls/Map view/Rotate textures clockwise", invoke(nudgeUv)},
    {"Controls/Map view/Rotate textures clockwise (coarse)", invoke(nudgeUv)},
    {"Controls/Map view/Rotate textures clockwise (fine)", invoke(nudgeUv)},
    {"Controls/Map view/Rotate textures counter-clockwise", invoke(nudgeUv)},
    {"Controls/Map view/Rotate textures counter-clockwise (coarse)", invoke(nudgeUv)},
    {"Controls/Map view/Rotate textures counter-clockwise (fine)", invoke(nudgeUv)},
    {"Controls/Map view/Reveal in texture browser", invoke()},
    {"Controls/Map view/Flip textures horizontally", invoke({"uv_align"})},
    {"Controls/Map view/Flip textures vertically", invoke({"uv_align"})},
    {"Controls/Map view/Reset texture alignment", invoke({"uv_align"})},
    {"Controls/Map view/Reset texture alignment to world aligned", invoke({"uv_align"})},
    {"Controls/Map view/Make structural", invoke({"tag_remove", "entity_move_brushes"})},

    // Map view: view filters
    {"Controls/Map view/View Filter > Toggle show entity classnames",
     invoke(viewOptions)},
    {"Controls/Map view/View Filter > Toggle show group bounds", invoke(viewOptions)},
    {"Controls/Map view/View Filter > Toggle show brush entity bounds",
     invoke(viewOptions)},
    {"Controls/Map view/View Filter > Toggle show point entity bounds",
     invoke(viewOptions)},
    {"Controls/Map view/View Filter > Toggle show point entities", invoke(viewOptions)},
    {"Controls/Map view/View Filter > Toggle show point entity models",
     invoke(viewOptions)},
    {"Controls/Map view/View Filter > Toggle show brushes", invoke(viewOptions)},
    {"Controls/Map view/View Filter > Show textures", invoke(viewOptions)},
    {"Controls/Map view/View Filter > Hide textures", invoke(viewOptions)},
    {"Controls/Map view/View Filter > Hide faces", invoke(viewOptions)},
    {"Controls/Map view/View Filter > Shade faces", invoke(viewOptions)},
    {"Controls/Map view/View Filter > Use fog", invoke(viewOptions)},
    {"Controls/Map view/View Filter > Show edges", invoke(viewOptions)},
    {"Controls/Map view/View Filter > Show all entity links",
     refuse(viewOptions, entityLinkBug)},
    {"Controls/Map view/View Filter > Show transitively selected entity links",
     refuse(viewOptions, entityLinkBug)},
    {"Controls/Map view/View Filter > Show directly selected entity links",
     refuse(viewOptions, entityLinkBug)},
    {"Controls/Map view/View Filter > Hide entity links",
     refuse(viewOptions, entityLinkBug)},

    // Map view: misc
    {"Controls/Map view/Cycle map view", invoke()},
    {"Controls/Map view/Reset camera zoom", invoke({"camera_set"})},
    {"Controls/Map view/Cancel", invoke({"selection_clear", "group_close"})},

    // File
    {"Menu/File/New",
     dialog("modal", {"document_new"}, "asks for the game and the map format")},
    {"Menu/File/Open...", dialog("file", {"document_open"}, "asks for the map file")},
    {"Menu/File/Save",
     dialog(
       "file",
       {"document_save"},
       "asks for a file name if the map was never saved and reports errors in a "
       "message box")},
    {"Menu/File/Save as...",
     dialog("file", {"document_save_as"}, "asks for the file name")},
    {"Menu/File/Export/Wavefront OBJ...",
     dialog("window", {"document_export_obj"}, "opens the OBJ export dialog")},
    {"Menu/File/Export/Map...",
     dialog("file", {"document_export_map"}, "asks for the file name")},
    {"Menu/File/Load Point File...",
     dialog("file", {"pointfile_load"}, "asks for the point file")},
    {"Menu/File/Reload Point File", invoke({"pointfile_load"})},
    {"Menu/File/Unload Point File", invoke({"pointfile_unload"})},
    {"Menu/File/Load Portal File...",
     dialog("file", {"portalfile_load"}, "asks for the portal file")},
    {"Menu/File/Reload Portal File", invoke({"portalfile_load"})},
    {"Menu/File/Unload Portal File", invoke({"portalfile_unload"})},
    {"Menu/File/Reload Material Collections",
     refuse(
       {"materials_reload"},
       "reloads all materials, which the call's transaction must not include; "
       "materials_reload reloads them outside of a transaction and reports problems")},
    {"Menu/File/Reload Entity Definitions",
     refuse(
       {"entity_definitions_reload"},
       "reloads the entity definitions, which the call's transaction must not include "
       "and which replaces the tag and entity actions; entity_definitions_reload "
       "reloads them outside of a transaction and reports problems")},
    {"Menu/File/Revert",
     dialog(
       "confirmation",
       {"document_revert"},
       "asks for confirmation if the map has unsaved changes and reloads the document, "
       "which must not happen during a call")},
    {"Menu/File/Close",
     dialog(
       "confirmation",
       {"document_close"},
       "asks whether to save unsaved changes and closes the document, which must not "
       "happen during a call")},
    {"Menu/File/Preferences...",
     dialog(
       "modal", {"preferences_get", "preferences_set"}, "opens the Preferences dialog")},
    {"Menu/File/About TrenchBroom",
     dialog("window", {"editor_status"}, "opens the About window")},

    // Edit
    {"Menu/Edit/Undo",
     refuse(
       {"undo"}, "cannot undo inside the call's transaction; use the undo tool instead")},
    {"Menu/Edit/Redo",
     refuse(
       {"redo"}, "cannot redo inside the call's transaction; use the redo tool instead")},
    {"Menu/Edit/Repeat",
     refuse(
       {"command_repeat"},
       "commands are not repeated inside the call's transaction; command_repeat "
       "repeats them as one undo step")},
    {"Menu/Edit/Clear Repeatable Commands", invoke({"command_repeat_clear"})},
    {"Menu/Edit/Cut", invoke({"clipboard_cut"})},
    {"Menu/Edit/Copy", invoke({"clipboard_copy"})},
    {"Menu/Edit/Paste", invoke({"clipboard_paste"})},
    {"Menu/Edit/Paste at Original Position", invoke({"clipboard_paste"})},
    {"Menu/Edit/Duplicate", invoke({"objects_duplicate"})},
    {"Menu/Edit/Delete", invoke({"objects_delete", "vertices_remove"})},
    {"Controls/Map view/Flip objects horizontally", invoke({"objects_flip"})},
    {"Controls/Map view/Flip objects vertically", invoke({"objects_flip"})},
    {"Menu/Edit/Move objects", dialog("input", {"objects_move"}, "asks for the offset")},
    {"Menu/Edit/CSG/Convex Merge", invoke({"csg_merge"})},
    {"Menu/Edit/CSG/Subtract", invoke({"csg_subtract"})},
    {"Menu/Edit/CSG/Hollow", invoke({"csg_hollow"})},
    {"Menu/Edit/CSG/Intersect", invoke({"csg_intersect"})},
    {"Menu/Edit/Snap Vertices to Integer", invoke({"vertices_snap"})},
    {"Menu/Edit/Snap Vertices to Grid", invoke({"vertices_snap"})},
    {"Menu/Edit/Convert Selection to Patches", invoke()},
    {"Menu/Edit/Texture Lock", invoke({"locks_set"})},
    {"Menu/Edit/UV Lock", invoke({"locks_set"})},
    {"Menu/Edit/Replace Material...",
     dialog("modal", {"material_replace"}, "opens the Replace Material dialog")},

    // Selection
    {"Menu/Edit/Select All", invoke({"select_all"})},
    {"Menu/Edit/Invert Selection", invoke({"select_invert"})},
    {"Menu/Edit/Deselect All", invoke({"selection_clear"})},
    {"Menu/Edit/Select Siblings", invoke({"select_siblings"})},
    {"Menu/Edit/Select Touching", invoke({"select_spatial"})},
    {"Menu/Edit/Select Inside", invoke({"select_spatial"})},
    {"Menu/Edit/Select Tall", invoke({"select_spatial"})},
    {"Menu/Edit/Select by Line Number",
     dialog("input", {"select_by_line"}, "asks for the line numbers")},

    // Groups
    {"Menu/Edit/Group", dialog("input", {"group_create"}, "asks for the group name")},
    {"Menu/Edit/Ungroup", invoke({"group_ungroup"})},
    {"Menu/Edit/Rename Groups",
     dialog("input", {"group_rename"}, "asks for the new group name")},
    {"Menu/Edit/Create Linked Duplicate", invoke({"linked_group_duplicate"})},
    {"Menu/Edit/Select Linked Groups", invoke({"linked_group_select"})},
    {"Menu/Edit/Separate Linked Groups", invoke({"linked_group_separate"})},
    {"Menu/Edit/Extract Linked Groups", invoke({"linked_group_extract"})},
    {"Menu/Edit/Clear Protected Properties", invoke()},

    // Tools
    {"Menu/Edit/Tools/Brush Tool", invoke({"brush_create_hull"})},
    {"Menu/Edit/Tools/Clip Tool", invoke({"brush_clip"})},
    {"Menu/Edit/Tools/Rotate Tool", invoke({"objects_rotate"})},
    {"Menu/Edit/Tools/Sweep Tool", invoke({"face_extrude_new"})},
    {"Menu/Edit/Tools/Scale Tool", invoke({"objects_scale"})},
    {"Menu/Edit/Tools/Shear Tool", invoke({"objects_shear"})},
    {"Menu/Edit/Tools/Vertex Tool",
     invoke({"vertices_move", "vertex_add", "vertices_remove"})},
    {"Menu/Edit/Tools/Edge Tool", invoke({"vertices_move"})},
    {"Menu/Edit/Tools/Face Tool", invoke({"vertices_move", "face_extrude"})},
    {"Menu/Edit/Tools/Control Point Tool", invoke()},
    {"Controls/Map view/Deactivate current tool", invoke()},

    // View
    {"Menu/View/Grid/Show Grid", invoke(grid)},
    {"Menu/View/Grid/Snap to Grid", invoke(grid)},
    {"Menu/View/Grid/Increase Grid Size", invoke(grid)},
    {"Menu/View/Grid/Decrease Grid Size", invoke(grid)},
    {"Menu/View/Grid/Set Grid Size 0.125", invoke(grid)},
    {"Menu/View/Grid/Set Grid Size 0.25", invoke(grid)},
    {"Menu/View/Grid/Set Grid Size 0.5", invoke(grid)},
    {"Menu/View/Grid/Set Grid Size 1", invoke(grid)},
    {"Menu/View/Grid/Set Grid Size 2", invoke(grid)},
    {"Menu/View/Grid/Set Grid Size 4", invoke(grid)},
    {"Menu/View/Grid/Set Grid Size 8", invoke(grid)},
    {"Menu/View/Grid/Set Grid Size 16", invoke(grid)},
    {"Menu/View/Grid/Set Grid Size 32", invoke(grid)},
    {"Menu/View/Grid/Set Grid Size 64", invoke(grid)},
    {"Menu/View/Grid/Set Grid Size 128", invoke(grid)},
    {"Menu/View/Grid/Set Grid Size 256", invoke(grid)},
    {"Menu/View/Camera/Move to Next Point", invoke({"camera_step_pointfile"})},
    {"Menu/View/Camera/Move to Previous Point", invoke({"camera_step_pointfile"})},
    {"Menu/View/Camera/Reset 2D Cameras", invoke({"camera_set"})},
    {"Menu/View/Camera/Focus on Selection", invoke({"camera_focus"})},
    {"Menu/View/Camera/Move Camera to...",
     dialog("input", {"camera_set"}, "asks for the camera position")},
    {"Menu/View/Isolate", invoke({"visibility_set"})},
    {"Menu/View/Hide", invoke({"visibility_set"})},
    {"Menu/View/Show All", invoke({"visibility_set"})},
    {"Menu/View/Switch to Map Inspector", invoke()},
    {"Menu/View/Switch to Entity Inspector", invoke()},
    {"Menu/View/Switch to Face Inspector", invoke()},
    {"Menu/View/Toggle Toolbar", invoke()},
    {"Menu/View/Toggle Info Panel", invoke()},
    {"Menu/View/Toggle Inspector", invoke()},
    {"Menu/View/Maximize Current View", invoke({"view_layout_set"})},

    // Run
    {"Menu/Run/Compile...",
     dialog("window", {"compile_run"}, "opens the compilation dialog")},
    {"Menu/Run/Launch...",
     dialog("modal", {"engine_launch"}, "opens the Launch Engine dialog")},
    {"Menu/Run/Rerun...",
     dialog(
       "window",
       {"compile_run"},
       "opens the compilation dialog and runs the last profile in it")},

    // Debug (debug builds only)
    {"Menu/Debug/Print Vertices", invoke()},
    {"Menu/Debug/Create Brush...",
     dialog("input", {"brush_create_hull"}, "asks for the brush vertices")},
    {"Menu/Debug/Create Cube...",
     dialog("input", {"brush_create_box"}, "asks for the cube size")},
    {"Menu/Debug/Crash...",
     refuse({}, "asks for a crash type and crashes the editor on purpose")},
    {"Menu/Debug/Throw Exception During Command",
     refuse({}, "throws an exception inside a command on purpose")},
    {"Menu/Debug/Show Crash Report Dialog",
     dialog("modal", {}, "opens the crash report dialog")},
    {"Menu/Debug/Set Window Size...", dialog("input", {}, "asks for the window size")},
    {"Menu/Debug/Show Palette...", dialog("window", {}, "opens the palette window")},

    // Help
    {"Menu/Help/TrenchBroom Manual",
     dialog(
       "browser",
       {"manual_search", "manual_section"},
       "opens the manual in the user's web browser")},
  };
}

std::vector<ActionCatalogPattern> createPatterns()
{
  return {
    {"Filters/Tags/", "/Toggle Visible", invoke({"view_options_set"})},
    {"Tags/",
     "/Enable",
     dialog(
       "menu",
       {"tag_apply"},
       "asks the user in a popup menu which material, flag or entity class to use if "
       "the tag allows several")},
    {"Tags/", "/Disable", invoke({"tag_remove"})},
    {"Entities/", "/Toggle", invoke({"view_options_set"})},
    {"Entities/", "/Create", invoke({"entity_create_point", "entity_create_brush"})},
  };
}

} // namespace

const std::vector<ActionCatalogEntry>& actionCatalog()
{
  static const auto catalog = createCatalog();
  return catalog;
}

const std::vector<ActionCatalogPattern>& actionCatalogPatterns()
{
  static const auto patterns = createPatterns();
  return patterns;
}

const ActionClass* findActionClass(const std::string_view path)
{
  const auto& catalog = actionCatalog();
  if (const auto it = std::ranges::find(catalog, path, &ActionCatalogEntry::path);
      it != catalog.end())
  {
    return &it->actionClass;
  }

  for (const auto& pattern : actionCatalogPatterns())
  {
    if (
      path.size() > pattern.prefix.size() + pattern.suffix.size()
      && path.starts_with(pattern.prefix) && path.ends_with(pattern.suffix))
    {
      return &pattern.actionClass;
    }
  }

  return nullptr;
}

ActionClass classifyAction(const std::string_view path, const std::string_view label)
{
  if (const auto* actionClass = findActionClass(path))
  {
    return *actionClass;
  }
  return label.ends_with("...")
           ? dialog(
               "modal",
               {},
               "the action is unknown to the MCP server and asks for "
               "more input (its label ends with \"...\")")
           : invoke();
}

std::vector<std::string> actionCatalogToolNames()
{
  auto names = std::set<std::string>{};
  for (const auto& entry : actionCatalog())
  {
    names.insert(entry.actionClass.tools.begin(), entry.actionClass.tools.end());
  }
  for (const auto& pattern : actionCatalogPatterns())
  {
    names.insert(pattern.actionClass.tools.begin(), pattern.actionClass.tools.end());
  }
  return {names.begin(), names.end()};
}

std::string_view toString(const ActionHandling handling)
{
  switch (handling)
  {
  case ActionHandling::Invoke:
    return "invoke";
  case ActionHandling::Dialog:
    return "dialog";
  case ActionHandling::Refuse:
    return "refuse";
  }
  return "invoke";
}

} // namespace tb::mcp
