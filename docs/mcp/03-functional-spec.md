# TrenchBroom MCP Server — Functional Specification

Date: 2026-09-26 · Status: Draft · Parent: [01-PRD.md](01-PRD.md)

This document describes **what** the MCP server offers to an AI agent: tools (actions), resources (readable data) and prompts (ready-made task templates). It describes behavior and requirements, not implementation. Names such as `document_open` are working names for the capability; the final naming is up to the engineering design.

Priority legend: **MVP** — first release; **v1** — full release; **v2** — later.

---

## 1. Cross-cutting requirements

These rules apply to every tool.

| ID | Requirement |
|---|---|
| X1 | **Stable object IDs.** Every map object (layer, group, entity, brush, face, patch) has an ID that stays the same across calls while the object exists. Faces are addressed as brush ID + face index or by face ID. |
| X2 | **One call = one Undo step.** Each modifying call creates exactly one named entry in the Undo history, prefixed "AI:" (e.g. "AI: Create brush"). |
| X3 | **Explicit transactions.** The agent can open a transaction, make several calls, then commit (one Undo step) or roll back (no trace). |
| X4 | **Atomicity.** If any part of a call fails, the map is unchanged and the error explains why. |
| X5 | **Change report.** Every modifying call returns the IDs of created, modified and removed objects, the new selection, and validation issues introduced by the change. |
| X6 | **Dry run.** Every modifying call accepts a "dry run" option that reports what would happen (including validity) without changing the map. |
| X7 | **Selection-independence.** Tools that act on objects accept explicit object IDs. If none are given, they act on the current selection, the way the editor does. |
| X8 | **Units and axes.** All coordinates are in map units; Z is up. Angles are in degrees. Responses state the grid size in effect. |
| X9 | **Actionable errors.** Errors name the problem, the object IDs involved, and a suggested fix. |
| X10 | **Pagination and field selection.** List results are paginated; the agent can choose which fields to return and get summaries instead of full data. |
| X11 | **No confirmations, no restrictions.** Every operation is allowed. The server never waits for human approval; if the agent wants confirmation, it asks the user itself (see section 20). |
| X12 | **Live visibility.** All changes appear immediately in the editor views. Agent activity is shown in the status bar. |
| X13 | **Human priority.** If the human is in the middle of an interaction (e.g. dragging), agent calls wait until it ends so the two edits do not collide. The human can stop the agent at any time. |
| X14 | **Game awareness.** Tools that take property names, flags, materials or classnames validate them against the current game and entity definitions and warn about unknown values. |

---

## 2. Session and editor state

| Tool | What it does | Key inputs | Returns | Priority |
|---|---|---|---|---|
| `editor_status` | Editor version, open windows/documents, active document, current tool, grid, locks, whether a compile is running | — | Status summary | MVP |
| `document_list` | Lists open documents (one per window) | — | Documents with path, game, format, modified flag | MVP |
| `document_activate` | Chooses which open document the agent works with | Document | Result | MVP |
| `session_log` | Returns the list of agent actions performed in this session | Filters | Log entries | v1 |

## 3. Documents and files

| Tool | What it does | Key inputs | Returns | Priority |
|---|---|---|---|---|
| `document_new` | Creates a new map | Game, map format | Document info | MVP |
| `document_open` | Opens a map file; detects game and format or uses the given ones | File path, optional game and format | Document info, load warnings | MVP |
| `document_save` | Saves the map | — | Path, time | MVP |
| `document_save_as` | Saves under a new path; overwrites an existing file when asked to | File path, overwrite | Path | MVP |
| `document_revert` | Discards unsaved changes and reloads from disk | — | Document info | v1 |
| `document_close` | Closes the document; with unsaved changes the agent says whether to save or discard | Save / discard | Result | MVP |
| `document_recent` | Lists recently opened maps | — | Paths | v1 |
| `document_export_map` | Exports a copy as `.map`, leaving out layers marked "omit from export" | File path, strip editor-only properties | Path | v1 |
| `document_export_obj` | Exports geometry as Wavefront OBJ + MTL | File path, texture path mode | Paths | v1 |
| `autosave_list` | Lists autosave backups of the current map | — | Backups with time | v1 |
| `autosave_restore` | Opens a backup as a new document | Backup | Document info | v2 |
| `map_files_list` | Lists map files in any folder | Folder, pattern | Paths | v1 |

## 4. Games, mods and assets

| Tool | What it does | Key inputs | Returns | Priority |
|---|---|---|---|---|
| `game_list` | Lists supported games, their map formats and whether each game path is configured | — | Games | MVP |
| `game_info` | Describes the current game: formats, file system, materials setup, entity definition files, tags, surface and content flags, soft bounds, compile tools | Game (default current) | Game description | MVP |
| `game_set_path` | Sets the game folder path | Game, folder | Result | v1 |
| `mods_get` / `mods_set` | Reads or sets the enabled mods and their priority for this map | Ordered mod list | Mods, available mods | v1 |
| `entity_definitions_get` / `entity_definitions_set` | Reads or chooses the entity definition file (built-in or external) | File | Current file, available built-ins | v1 |
| `entity_definitions_reload` | Reloads entity definitions and models | — | Warnings | v1 |
| `materials_collections_get` / `materials_collections_set` | Reads or sets the enabled material collections (folders) or the WAD list (ordered) | Collections / WAD paths | Collections | v1 |
| `materials_reload` | Reloads material collections | — | Warnings | v1 |
| `soft_bounds_get` / `soft_bounds_set` | Reads or sets soft map bounds (disabled, game default, custom box) | Mode, box | Bounds | v1 |

## 5. Scene inspection (reading the map)

The agent must be able to understand the map without a screen.

| Tool | What it does | Key inputs | Returns | Priority |
|---|---|---|---|---|
| `map_summary` | High-level overview: counts of brushes, entities by class, layers, groups, materials used, map bounds, issue count | — | Summary | MVP |
| `map_tree` | The hierarchy: world → layers → groups → entities → brushes/patches | Root ID, depth, filters | Tree (paginated) | MVP |
| `object_get` | Full details of one object: type, parent, layer, bounds, properties (entities), faces with materials and alignment (brushes), visibility and lock state | Object ID, fields | Object details | MVP |
| `objects_find` | Finds objects by type, classname, property key/value, material, layer, group, tag, bounding region, visibility | Filters | Object IDs with short descriptions | MVP |
| `objects_at_point` | Which objects contain or touch a point | Point | Object IDs | v1 |
| `ray_pick` | What a ray hits first (e.g. "what is below this entity?") | Origin, direction | Hit object, face, point, distance | v1 |
| `space_check` | Whether a box is free, what it overlaps, distance to floor and ceiling — used before placing entities or rooms | Box | Overlaps, floor/ceiling heights | v1 |
| `map_plan_view` | A text or image top-down plan of a region at a chosen height: rooms, walls, entities | Region, height | Plan | v1 |
| `map_text_get` | The map (or a selection of objects) as map-file text | Object IDs | Text | v1 |
| `map_stats` | Detailed statistics: brush counts by entity, face counts, materials by usage, entities by class, bounds per layer | — | Statistics | v1 |

## 6. Selection

| Tool | What it does | Key inputs | Returns | Priority |
|---|---|---|---|---|
| `selection_get` | Current selection (objects or faces) with a summary | Fields | Selection | MVP |
| `selection_set` | Replaces, adds to or removes from the selection | Object or face IDs, mode | Selection | MVP |
| `selection_clear` | Deselects everything | — | — | MVP |
| `select_all` / `select_invert` | Selects all visible, unlocked objects / inverts the selection | — | Selection | MVP |
| `select_by` | Selects by classname, material (faces or brushes), layer, linked group | Criterion | Selection | MVP |
| `select_spatial` | Selects objects touching, inside, or "tall" (inside in one axis) relative to the selected brushes | Mode | Selection | v1 |
| `select_siblings` | Selects all objects in the same entity or group | — | Selection | v1 |
| `select_by_line` | Selects objects defined at given lines of the map file | Line numbers | Selection | v1 |
| `select_faces_of` | Selects all faces of given brushes, or all touching coplanar faces | Brush or face | Face selection | v1 |

## 7. Brush creation

| Tool | What it does | Key inputs | Returns | Priority |
|---|---|---|---|---|
| `brush_create_box` | Creates a cuboid brush | Min and max corners, material | Brush ID | MVP |
| `brush_create_shape` | Creates a shape: stairs, arch, cylinder (optionally hollow), cone, UV sphere, icosphere | Shape, bounds, axis, sides, circle mode, thickness, rings, subdivision, step height and direction, material | Brush IDs | v1 |
| `brush_create_hull` | Creates a brush as the convex hull of points | Points, material | Brush ID | v1 |
| `brush_create_from_planes` | Creates a brush from a list of face planes (for exact geometry) | Planes with materials | Brush ID | v2 |
| `room_create` | Convenience: creates a hollow room (floor, ceiling, walls) of given inner size and wall thickness, with materials per surface | Inner box, thickness, materials | Brush IDs | v1 |
| `opening_cut` | Convenience: cuts a doorway or window through a wall | Wall brush, opening box | Brush IDs | v1 |

New brushes go to the current layer (or the open group) and use the current material when none is given.

## 8. Transforms

All transforms respect texture lock unless told otherwise, and accept a dry run.

| Tool | What it does | Key inputs | Returns | Priority |
|---|---|---|---|---|
| `objects_move` | Moves objects by a vector | Objects, vector | Change report | MVP |
| `objects_rotate` | Rotates around a center and axis by an angle; optionally updates entity angle properties | Objects, center, axis, angle, update angles | Change report | MVP |
| `objects_scale` | Scales by factors, or to fit a target box | Objects, factors or box, anchor | Change report | v1 |
| `objects_shear` | Shears along an axis | Objects, side, offset | Change report | v1 |
| `objects_flip` | Mirrors along an axis around the selection center | Objects, axis | Change report | v1 |
| `objects_duplicate` | Duplicates, optionally with an offset | Objects, offset | New IDs | MVP |
| `objects_array` | Convenience: makes N copies along a line, grid or circle (with rotation) | Objects, pattern, count, spacing/radius | New IDs | v1 |
| `objects_delete` | Deletes objects | Objects | Change report | MVP |
| `command_repeat` | Repeats the last repeatable sequence (move/duplicate/rotate) | Times | Change report | v1 |
| `command_repeat_clear` | Clears the repeat list | — | — | v1 |

## 9. Geometry editing

| Tool | What it does | Key inputs | Returns | Priority |
|---|---|---|---|---|
| `brush_clip` | Cuts brushes with a plane given by 2–3 points or an existing face's plane; keep front, back or both | Brushes, plane, side | Change report | v1 |
| `face_extrude` | Moves faces along their normals by a distance | Faces, distance | Change report | v1 |
| `face_extrude_new` | Creates a new brush from a face (extrude-to-new or stamp) | Face, distance, mode | New brush ID | v1 |
| `vertices_move` | Moves vertices, edges or faces of brushes by a vector; shared ones move together | Handles (vertex/edge/face positions), vector, UV lock | Change report | v1 |
| `vertex_add` / `vertices_remove` | Adds a vertex at a point / removes vertices, edges or faces | Brush, points | Change report | v1 |
| `vertices_snap` | Snaps vertices to the grid or to integers | Objects, mode | Change report | v1 |
| `csg_merge` | Convex merge of brushes or faces | Objects | New brush ID | v1 |
| `csg_subtract` | Subtracts selected brushes from other visible brushes | Cutter brushes | Change report | v1 |
| `csg_intersect` | Keeps only the shared volume | Brushes | Change report | v1 |
| `csg_hollow` | Hollows brushes into walls of given thickness (default: grid size) | Brushes, thickness | Change report | v1 |
| `sweep` | Fills the space between faces and a destination cap with brushes | Faces, destination transform, segments, path type, iterations, snap | Change report | v2 |
| `patch_convert` | Converts selected faces to Bezier patches (Quake 3 only) | Faces | Patch IDs | v2 |
| `patch_edit` | Moves control points; changes rows and columns | Patch, points, vector / rows, columns | Change report | v2 |

## 10. Entities (including NPCs, items, lights)

| Tool | What it does | Key inputs | Returns | Priority |
|---|---|---|---|---|
| `entity_classes_list` | Lists available entity classes, grouped by prefix (monster_, item_, weapon_, light, info_…), point vs brush, with usage counts | Filters | Classes | MVP |
| `entity_class_describe` | Full definition of a class: description, size, color, model, properties with types, defaults, choices, spawnflags | Classname | Definition | MVP |
| `entity_create_point` | Places a point entity (monster, item, light, player start…) at a position; optionally snaps it onto the floor below | Classname, position, properties, drop to floor, apply defaults | Entity ID | MVP |
| `entity_create_brush` | Turns brushes into a brush entity (door, trigger, platform…) | Classname, brushes, properties | Entity ID | MVP |
| `entity_properties_set` | Sets properties on one or many entities (worldspawn included) | Entities, key/value pairs | Change report | MVP |
| `entity_property_remove` / `entity_property_rename` | Removes or renames a property | Entities, key(s) | Change report | MVP |
| `entity_spawnflags_set` | Sets or clears spawnflags by name | Entities, flag names, on/off | Change report | MVP |
| `entity_defaults_apply` | Applies default property values (missing, existing or all) | Entities, mode | Change report | v1 |
| `entity_move_brushes` | Moves brushes into a brush entity, or back to the world ("make structural") | Brushes, target entity or world | Change report | v1 |
| `entity_links_get` | Shows link relations (target → targetname) for entities, including broken links | Entities or whole map | Links | v1 |
| `entity_link` | Links a source entity to a target, generating a unique name when needed | Source, target, link key | Change report | v1 |
| `entity_model_info` | Which model, skin and frame an entity shows; its animations with the real model bounds of each | Entity | Model info | v1 |

Placement uses the real model bounds of the entity's current animation: creating or moving an entity warns when the model penetrates brushes or floats, and dropping to the floor can use the model bounds.
| `entity_color_set` | Sets a color property in the right range (float or byte) for the class | Entity, key, color | Change report | v1 |

## 11. Materials, UV alignment and face attributes

| Tool | What it does | Key inputs | Returns | Priority |
|---|---|---|---|---|
| `materials_list` | Lists materials with collection, size, usage count; filter by name and "used only" | Filters | Materials (paginated) | MVP |
| `material_preview` | Returns a small image of a material | Material | Image | v1 |
| `material_apply` | Applies a material to faces or to all faces of brushes | Material, faces/brushes | Change report | MVP |
| `material_set_current` | Sets the current material for new brushes | Material | — | MVP |
| `material_replace` | Replaces one material with another in the selection or whole map; supports name patterns | From, to, scope | Change report | MVP |
| `face_attributes_get` | Reads offset, scale, rotation, material, flags, value, color of faces | Faces | Attributes | MVP |
| `face_attributes_set` | Sets any of the above on faces | Faces, attributes | Change report | MVP |
| `face_attributes_copy` | Copies material and alignment from one face to others (project, rotate axes, or material only) | Source face, target faces, mode | Change report | v1 |
| `uv_align` | Justify (to an edge or center), align to an edge, fit N×M, auto-fit, reset, reset to world, flip, rotate 90° | Faces, operation, parameters | Change report | v1 |
| `uv_nudge` | Moves or rotates textures by steps relative to the face | Faces, direction, step | Change report | v1 |
| `locks_get` / `locks_set` | Reads or sets texture lock and UV lock | Values | Values | v1 |
| `tags_list` | Lists smart tags of the game (clip, trigger, detail…) with what they match | — | Tags | v1 |
| `tag_apply` / `tag_remove` | Turns objects/faces into a tag type or back ("make structural") | Tag, objects/faces | Change report | v1 |

### 11.1. Texturing knowledge

The agent textures surfaces the way an experienced designer of the game would. Knowledge comes from data, for any game: explicit notes, a scanned corpus of reference maps, the current map, and analysis of the texture image.

| Tool | What it does | Key inputs | Returns | Priority |
|---|---|---|---|---|
| `material_corpus_scan` | Collects per-material statistics (scales, repeats, face sizes, alignment) from a folder of reference `.map` files; cached per game and mod | Folder | Summary | v1 |
| `material_notes_get` / `material_notes_set` | Explicit per-material facts for a game or mod (kind, typical scale); override statistics | Materials, notes | Notes | v1 |
| `material_usage` | Material profile: kind (panel, seamless tile, trim, decal-like, sky, liquid, tool), typical scale, texel density, typical face size and repeats, with the source of each value | Materials | Profiles | v1 |
| `uv_check` | Texturing problems: aspect distortion above ~10%, fractional repeats on panels, unaligned panels, unusual scale, texel density mismatch with neighbours, seams between coplanar faces; each with a suggested fix | Faces or map | Findings | v1 |
| `material_fit_geometry` | Face size at which a panel fits exactly at its typical scale, and the resize needed for a given face | Face, material | Sizes | v1 |

`uv_align` fit keeps the aspect ratio when asked: the agent sets repeats on one axis and the other follows. Material and UV tools report `uv_check` findings as warnings.

## 12. Layers, groups and visibility

| Tool | What it does | Key inputs | Returns | Priority |
|---|---|---|---|---|
| `layers_list` | Layers with order, current, hidden, locked, omit-from-export, object counts | — | Layers | v1 |
| `layer_create` / `layer_rename` / `layer_remove` / `layer_reorder` | Manage layers (removing moves objects to the default layer) | Name, layer, position | Layer ID / report | v1 |
| `layer_set_state` | Sets current, hidden, locked, omit-from-export, isolate | Layer, flags | Report | v1 |
| `objects_move_to_layer` | Moves objects to a layer | Objects, layer | Report | v1 |
| `group_create` / `group_ungroup` / `group_rename` / `groups_merge` | Manage groups | Objects, name | Group ID / report | v1 |
| `group_add_objects` / `group_remove_objects` | Adds or removes objects from a group | Group, objects | Report | v1 |
| `group_open` / `group_close` | Enters or leaves a group for editing | Group | Report | v1 |
| `linked_group_duplicate` | Creates a linked duplicate | Group, offset | New group ID | v1 |
| `linked_group_select` / `linked_group_separate` / `linked_group_extract` | Manages linked group sets | Groups / objects | Report | v1 |
| `linked_group_protect_property` | Marks entity properties that should differ between linked copies | Entity, keys | Report | v2 |
| `visibility_set` | Hides, shows, isolates objects; show all | Objects, mode | Report | v1 |

## 13. Clipboard, import and prefabs

| Tool | What it does | Key inputs | Returns | Priority |
|---|---|---|---|---|
| `clipboard_copy` / `clipboard_cut` | Copies or cuts objects or faces as map text; returns the text too | Objects/faces | Text | v1 |
| `clipboard_paste` | Pastes map text at the original position or at a given point; face text applies attributes to selected faces | Text, position mode | New IDs | v1 |
| `map_file_inspect` | Lists the format, layers, groups, classnames and materials of another map file, and which materials are missing in the current map | File | Summary | v1 |
| `map_import` | Imports all or part of another map file (by layer, group, classname or region) into the current map at a position, converting the format | File, filter, position, target layer | New IDs | v1 |
| `prefab_save` / `prefab_list` / `prefab_insert` | Saves selected objects as a named reusable fragment in a user prefab folder; lists and inserts them | Name, objects / prefab, position, rotation | IDs | v2 |

## 14. History

| Tool | What it does | Key inputs | Returns | Priority |
|---|---|---|---|---|
| `undo` / `redo` | Undo or redo steps | Count | Step names | MVP |
| `history_get` | Recent Undo/Redo step names, marking agent steps | Count | Steps | MVP |
| `transaction_begin` / `transaction_commit` / `transaction_rollback` | Groups calls into one Undo step, or discards them | Name | — | MVP |

## 15. Validation

| Tool | What it does | Key inputs | Returns | Priority |
|---|---|---|---|---|
| `issues_list` | Lists issues with type, description, object, line number and available fixes | Types, include hidden, objects | Issues | MVP |
| `issue_fix` | Applies a quick fix to one or many issues | Issues, fix | Change report | MVP |
| `issue_hide` / `issue_show` | Hides or shows issues | Issues | — | v1 |
| `validators_list` / `validators_set` | Lists validators and turns them on or off | Validators | Validators | v1 |
| `map_check` | Extra agent-oriented checks beyond validators: entities stuck in walls or floating, missing player start, unreachable link targets, missing materials, entities outside rooms | Checks | Findings | v1 |

## 16. Compile, run and debug

| Tool | What it does | Key inputs | Returns | Priority |
|---|---|---|---|---|
| `compile_tools_get` / `compile_tools_set` | Compile tool paths of the current game (e.g. Half-Life csg/bsp/vis/rad, Quake qbsp/vis/light, Quake 3 q3map2), checked for existence | Tool paths | Tools | v1 |
| `compile_presets_list` | Built-in tool chains per game family (fast, normal, full quality) that export the map, compile it and copy the result into the game's `maps` folder | — | Presets | v1 |
| `compile_profiles_list` | Lists compile profiles with their tasks | — | Profiles | v1 |
| `compile_profile_save` / `compile_profile_delete` | Creates or edits a profile (tasks: export map, run tool, launch engine, copy, rename, delete files) | Profile | Profile | v1 |
| `compile_run` | Runs a profile or preset in the background; "test" mode only prints what would run | Profile or preset, test mode | Run ID | v1 |
| `compile_status` | Progress, current task, exit codes, full or tail of the log, parsed errors and warnings, detected leaks, and the path of the compiled file | Run ID | Status, log, output path | v1 |
| `compile_cancel` | Stops a running compile | Run ID | — | v1 |
| `engine_profiles_list` / `engine_profile_save` | Lists or edits game engine profiles | Profile | Profiles | v1 |
| `engine_launch` | Launches the game engine with the current map | Engine profile, parameters | Result | v1 |
| `pointfile_load` / `pointfile_unload` | Loads a leak point file; returns the leak path as points | File (default next to map) | Path points | v1 |
| `portalfile_load` / `portalfile_unload` | Loads a portal file | File | Portal count | v1 |
| `leak_locate` | Summarizes where a leak goes: the entity it starts from and the brushes closest to the path where the gap likely is | — | Findings | v2 |

## 17. Views, camera and images

The camera tools in the first table drive the **user's** editor views. Agent vision (17.1) uses separate agent cameras and never changes what the user sees.

| Tool | What it does | Key inputs | Returns | Priority |
|---|---|---|---|---|
| `camera_get` / `camera_set` | Reads or sets the 3D camera position and direction | Position, look-at or angles | Camera | v1 |
| `camera_focus` | Focuses the camera on objects | Objects | Camera | v1 |
| `camera_step_pointfile` | Moves the camera to the next/previous leak point | Direction | Camera | v1 |
| `view_options_get` / `view_options_set` | Face render mode, shading, fog, edges, entity display, link display mode, show/hide by tag or entity class | Options | Options | v1 |
| `view_layout_set` | Sets the layout (1–4 panes) and maximized view | Layout | — | v2 |
| `grid_get` / `grid_set` | Grid size, show and snap | Size, flags | Grid | MVP |

### 17.1. Agent vision and editor console

The agent needs to check its work visually. Snapshots render offscreen from cameras the agent owns, with their own visibility settings. They never move the user's camera, never change the user's filters or hidden objects, never open a visible window, and never wait for the user to finish an interaction.

| Tool | What it does | Key inputs | Returns | Priority |
|---|---|---|---|---|
| `agent_camera_set` / `agent_camera_get` / `agent_camera_list` | Named cameras owned by the agent session: perspective (position, look-at or angles, FOV) or orthographic (top/front/side, center, zoom) | Name, camera | Camera | v1 |
| `view_snapshot` | Renders an image from an agent camera or an inline camera, 3D or 2D, with its own options: face mode, shading, fog, edges; show/hide by tag (triggers, clip, hint…), entity class, point entities, models; include hidden objects; isolate or highlight objects; bounding boxes, classnames, entity links, leak path, grid, axes | Camera, size, options | Image | v1 |
| `view_snapshots_around` | Several images in one call around objects or a region (e.g. 4 sides + top), labelled | Objects or box, views | Images | v1 |
| `map_plan_view` (image form) | Top-down orthographic render of a region at a height slice, entities marked | Region, height | Image | v1 |
| `view_snapshot_compare` | The same camera before and after a change, side by side, with a changed-pixel mask | Camera, states | Images | v1 |
| `view_snapshot_user` | Captures what the user currently sees in an editor view (read-only) | View | Image | v1 |

Framing helpers for agent cameras: frame objects or a box, look from a point at a target, orbit a target, stand at a player's eye height inside a room.

The agent also reads the editor console, where the editor reports errors (missing materials, failed model loads, invalid map data) and other useful messages:

| Tool | What it does | Key inputs | Returns | Priority |
|---|---|---|---|---|
| `console_read` | Console messages with level, time, text and document; filter by minimum level, text and document; cursor to fetch only newer messages | Filters, cursor | Messages (paginated) | v1 |
| `console_clear` | Clears the console buffer and the editor's console view | — | Result | v1 |

Every call result lists console warnings and errors logged while the call ran.

Snapshots and console reads are read-only: they do not enter the undo history. A 1024×768 snapshot of a 5,000-brush map renders in under 500 ms, and the editor stays responsive while it renders.

## 18. Generic editor actions

Guarantees coverage of every menu item and shortcut, including per-entity-class and per-tag actions.

| Tool | What it does | Key inputs | Returns | Priority |
|---|---|---|---|---|
| `actions_list` | Lists all named actions with label, menu path, shortcut, whether enabled/checked now, and whether it opens a dialog | Filter | Actions | MVP |
| `action_invoke` | Runs an action by its path. Actions that need a dialog are either refused with a pointer to the matching semantic tool, or open the dialog for the human | Action path | Result, change report | MVP |

## 19. Preferences and knowledge

| Tool | What it does | Key inputs | Returns | Priority |
|---|---|---|---|---|
| `preferences_get` / `preferences_set` | Reads and changes any preference: game paths, tool paths, view, colors, mouse, keyboard shortcuts, layout | Keys, values | Values | v1 |
| `manual_search` | Searches the built-in TrenchBroom user manual | Query | Matching sections | MVP |
| `manual_section` | Returns a manual section | Section | Text | MVP |

## 20. Destructive operations (no confirmation)

The server never asks the human to approve anything. Operations that can destroy data only require the agent to state its intent explicitly, so they cannot happen by accident:

| Operation | Explicit parameter |
|---|---|
| Close, revert or open another map over unsaved changes | "discard unsaved changes" or "save first" |
| Save As / export over an existing file | "overwrite" |
| Delete objects | object IDs or selection |
| Compile profile with a Delete Files task | none beyond running the profile |

Whether to check with the user first is the agent's decision, made in its own client.

---

## 21. Resources (readable data)

Resources let the agent read data without calling a tool, and subscribe to changes.

| Resource | Content | Updates |
|---|---|---|
| Editor status | Active document, tool, grid, locks | On change |
| Document info | Path, game, format, modified flag, mods, entity definitions, material collections | On change |
| Map summary | Counts, bounds, layers, issue count | On change |
| Selection | Current selection summary | On change |
| Issues | Current validation issues | On change |
| Game configuration | Current game description (formats, tags, flags, tools) | On reload |
| Entity definitions | All classes of the current game/mod | On reload |
| Materials | Material list | On reload |
| Compile log | Output of the running or last compile | Streaming |
| Editor console | All editor log messages with level and time | On new messages |
| User manual | TrenchBroom manual, by section | Static |
| Agent guide | How to use this server well: coordinate conventions, typical sizes (player height, door size, step height per game), workflow tips | Static |

## 22. Prompts (task templates)

Ready-made instructions a user can pick in their AI client.

| Prompt | What it guides the agent to do |
|---|---|
| Blockout a level | Ask for a layout description, build rooms and corridors with correct player-scale dimensions, add a player start, check for leaks |
| Populate with enemies and items | Read available monster and item classes, place them on floors in free space, respecting difficulty spawnflags |
| Lighting pass | Place light entities with sensible spacing, brightness and colors per room |
| Texture pass | Apply a consistent material theme per surface type (floor, wall, ceiling, trim), align UVs |
| Fix all issues | List issues, explain them, apply safe quick fixes, report what remains |
| Compile and debug | Run a compile profile, read the log, load the point file on a leak and locate the gap |
| Explain this map | Summarize layout, entities, gameplay flow (links and triggers), and problems |
| Explain an entity | Describe a class and its options in plain language |
| Convert / clean up a map | Remove unused objects, fix properties, export in a chosen format |

---

## 23. Coverage matrix (editor feature → MCP capability)

| Editor feature area | Covered by | Notes |
|---|---|---|
| File menu (new, open, save, export, revert, close, recent) | §3 | Full |
| Point/portal files, reload materials/definitions | §4, §16 | Full |
| Edit menu: undo/redo/repeat, clipboard, duplicate, delete | §8, §13, §14 | Full |
| Transform, CSG, vertices, patches submenus | §8, §9 | Full |
| Texture/UV lock, replace material | §11 | Full |
| Selection menu | §6 | Full |
| Groups and linked groups | §12 | Full |
| Tools: Brush, Clip, Rotate, Sweep, Scale, Shear, Vertex, Edge, Face, Control point | §7–§9 | Covered as direct operations, not as interactive tools |
| Shape tool and its parameters | §7 | Full |
| View menu: grid, camera, isolate/hide, layout | §12, §17 | Full |
| View options popup (filters, render modes) | §17 | Full |
| Run menu: compile, launch | §16 | Full |
| Map Inspector: layers, map properties, mods | §4, §12 | Full |
| Entity Inspector: properties, smart editors, defaults, entity browser | §10 | Full |
| Face Inspector: attributes, UV editor, material browser, collections | §4, §11 | Full |
| Issue Browser | §15 | Full |
| Preferences | §4, §16, §19 | Full read and write |
| Any remaining menu or shortcut action | §18 | Generic fallback |
| Capabilities without an editor equivalent: agent vision (offscreen snapshots, agent cameras), map import, prefabs, extra checks, leak locating, plan view | §5, §13, §15, §16, §17 | MCP-only |
