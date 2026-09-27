# TrenchBroom MCP Server — Task Tracker (temporary)

Temporary working file. Delete it once all epics are done.

Specs: [01-PRD.md](01-PRD.md) · [03-functional-spec.md](03-functional-spec.md) · [04-scenarios.md](04-scenarios.md)

**Order:** epics run in numeric order. E16 is deferred.

**How we work:** one epic at a time, all of its subtasks in one go. Each epic ends buildable, with tests passing (Catch2, `<Name>LibTest` targets, run through `ctest --test-dir <build>/lib/<Name>/test -j`) and formatted with clang-format. Mark `[x]` when a subtask is done.

**Global rules for every tool** (spec §1): stable object IDs, one call = one "AI: …" Undo step, explicit transactions, atomic calls, change report, dry run, explicit IDs or current selection, actionable errors, pagination and field selection, no confirmations, game-aware validation.

| Epic | Area | Spec sections | Phase | Status |
|---|---|---|---|---|
| E1 | Server foundation and editor integration | §1, §2, §14 | MVP | Done |
| E2 | Documents, games and assets | §3, §4 | MVP + v1 | Done |
| E3 | Scene inspection, selection and resources | §5, §6, §21 | MVP + v1 | Done |
| E4 | Geometry: creation, transforms, editing, CSG | §7, §8, §9 | MVP + v1 | Done |
| E5 | Entities, NPCs and models | §10 | MVP + v1 | Done |
| E6 | Materials, UV and face attributes | §11 | MVP + v1 | Done |
| E7 | Compile maps | §16 | v1 | Done |
| E8 | Minimal upstream footprint | 01 §7.7 | v1 | Done |
| E9 | Organization, clipboard and import | §12, §13 | v1 | Done |
| E10 | Agent vision and editor console | §17 | v1 | Done |
| E11 | Level-design knowledge: texturing and model-aware placement | §10, §11 | v1 | Done |
| E12 | Spatial understanding: picking, rooms, free spots, placement checks | §5, §15, §17 | v1 | Done |
| E13 | Validation and engine launch | §15, §16 | MVP + v1 | Done |
| E14 | Views, camera, generic actions, preferences, knowledge | §17, §18, §19 | MVP + v1 | Done |
| E15 | Agent experience: prompts, guide, end-to-end scenarios | §22, 04 | v1 | Not started |
| E16 | Headless mode, batch and advanced features | v2 items | v2 | Not started |

---

## E1. Server foundation and editor integration

Goal: a running MCP server inside TrenchBroom that an MCP client can connect to, with the shared machinery every later tool relies on. After this epic, adding a tool is a small, local change.

**Architecture and protocol**

- [x] E1.1 Short design note (`docs/mcp/05-technical-design.md`): new library layout (e.g. `lib/TbMcpLib` for the Qt-free protocol core and tool implementations over `mdl::Map`, glue in `TbUiLib`), transport choice, JSON library choice, threading model.
- [x] E1.2 Create the MCP core library with CMake target, test target (`TbMcpLibTest`) and README, following the existing library conventions.
- [x] E1.3 JSON-RPC 2.0 message layer: requests, responses, notifications, batching, error codes.
- [x] E1.4 MCP lifecycle: `initialize` handshake, capability negotiation, protocol version, `ping`, shutdown, cancellation, progress notifications.
- [x] E1.5 Tool registry: declare a tool with name, description, input schema, output schema and handler; `tools/list` with pagination; `tools/call` dispatch; input validation against the schema with clear errors.
- [x] E1.6 Resource registry: `resources/list`, `resources/read`, resource templates, `subscribe` / `unsubscribe`, change notifications.
- [x] E1.7 Prompt registry: `prompts/list`, `prompts/get` with arguments.

**Transport**

- [x] E1.8 Streamable HTTP transport on localhost (Qt Network), with configurable port; remote binding as a preference.
- [x] E1.9 stdio bridge executable (`app/TrenchBroomMcp`) that forwards stdio MCP traffic to the running editor, so clients that only speak stdio can connect; starts the editor if it is not running.
- [x] E1.10 Multiple simultaneous clients: each gets its own session; notifications fan out.

**Editor integration**

- [x] E1.11 Start/stop the server with the application; preference to enable it and set the port (Preferences pane).
- [x] E1.12 Main-thread dispatch: every tool call runs on the Qt main thread; calls wait while the user is mid-interaction (drag / active modal tool gesture) (spec X13).
- [x] E1.13 Status bar indicator: connected clients, current agent action; "Stop agent" button that cancels in-flight calls and disconnects.
- [x] E1.14 Agent call log: to the editor console and to a log file in the user data folder.

**Shared tool machinery (spec §1)**

- [x] E1.15 Stable object IDs for layers, groups, entities, brushes, faces, patches; ID ↔ node lookup that survives Undo/Redo and document reloads where possible.
- [x] E1.16 Call wrapper: each modifying call runs in one transaction named "AI: <tool>"; rollback on any failure (atomicity).
- [x] E1.17 Change report built from document notifications: created / modified / removed IDs, resulting selection, issues introduced by the change.
- [x] E1.18 Dry run: execute in a transaction, collect the change report, roll back.
- [x] E1.19 Common argument helpers: object targets (explicit IDs or current selection), vectors, boxes, angles, pagination, field selection.
- [x] E1.20 Error model: error codes, message, involved IDs, suggested fix.
- [x] E1.21 Document targeting: tools act on the document chosen via `document_activate`, defaulting to the focused window.

**First tools (proof of the pipeline)**

- [x] E1.22 `editor_status`, `document_list`, `document_activate`.
- [x] E1.23 History tools: `undo`, `redo`, `history_get`, `transaction_begin`, `transaction_commit`, `transaction_rollback`.
- [x] E1.24 `session_log`.

**Done when:** Claude Code connects (HTTP and stdio), lists tools, runs `editor_status`, and `transaction_*` / `undo` work on a live document; unit tests cover the protocol, registries, IDs, transactions, change reports and dry run.

---

## E2. Documents, games and assets

Goal: the agent can manage map files and everything that controls how a map loads.

- [x] E2.1 `document_new` (game + format; initial map template).
- [x] E2.2 `document_open` with game/format detection or explicit values; returns load warnings.
- [x] E2.3 `document_save`, `document_save_as` (with `overwrite`).
- [x] E2.4 `document_close` (with `save` / `discard`), `document_revert`.
- [x] E2.5 `document_recent`, `map_files_list`.
- [x] E2.6 `document_export_map` (omit-from-export layers, strip editor-only properties).
- [x] E2.7 `document_export_obj` (texture path mode).
- [x] E2.8 `autosave_list`.
- [x] E2.9 `game_list`, `game_info` (formats, file system, materials setup, definition files, tags, face flags, soft bounds, compile tools).
- [x] E2.10 `game_set_path`.
- [x] E2.11 `mods_get`, `mods_set`.
- [x] E2.12 `entity_definitions_get`, `entity_definitions_set`, `entity_definitions_reload`.
- [x] E2.13 `materials_collections_get`, `materials_collections_set` (folder collections and ordered WAD list), `materials_reload`.
- [x] E2.14 `soft_bounds_get`, `soft_bounds_set`.
- [x] E2.15 Resources: document info, game configuration.
- [x] E2.16 Long operations (open, reload) report progress and support cancellation.

**Done when:** an agent creates a Quake map, saves it, closes it, reopens it, switches mods and WADs, and all of it is covered by tests on test fixtures.

---

## E3. Scene inspection, selection and resources

Goal: the agent can understand a map without a screen and select anything.

- [x] E3.1 `map_summary`.
- [x] E3.2 `map_tree` with root, depth, filters, pagination.
- [x] E3.3 `object_get` with field selection (entities: properties; brushes: faces, materials, alignment; common: parent, layer, bounds, visibility, lock).
- [x] E3.4 `objects_find` (type, classname, property key/value, material, layer, group, tag, region, visibility).
- [x] E3.5 `objects_at_point`, `ray_pick`.
- [x] E3.6 `space_check` (overlaps, floor and ceiling heights for a box).
- [x] E3.7 `map_text_get`, `map_stats`.
- [x] E3.8 `map_plan_view` (text form: top-down grid of a region at a height; image form delivered in E14).
- [x] E3.9 `selection_get`, `selection_set` (replace / add / remove; objects and faces), `selection_clear`.
- [x] E3.10 `select_all`, `select_invert`, `select_by` (classname, material, layer, linked group).
- [x] E3.11 `select_spatial` (touching, inside, tall), `select_siblings`, `select_by_line`, `select_faces_of`.
- [x] E3.12 Resources with subscriptions: editor status, map summary, selection.

**Done when:** on a real sample map an agent can answer "what is in this room, what is under this entity, which brushes use this material" using only these tools; tests cover filters and pagination.

---

## E4. Geometry: creation, transforms, editing, CSG

Goal: the agent can build and reshape any brush geometry the editor can.

**Creation**

- [x] E4.1 `brush_create_box`.
- [x] E4.2 `brush_create_shape`: stairs, arch, cylinder (hollow), cone, UV sphere, icosphere, with all shape tool parameters.
- [x] E4.3 `brush_create_hull`.
- [x] E4.4 `room_create`, `opening_cut` (convenience builders).

**Transforms**

- [x] E4.5 `objects_move`, `objects_rotate` (center, axis, angle, update entity angles), `objects_flip`.
- [x] E4.6 `objects_scale` (factors or target box, anchor), `objects_shear`.
- [x] E4.7 `objects_duplicate`, `objects_delete`.
- [x] E4.8 `objects_array` (line, grid, circle with rotation).
- [x] E4.9 `command_repeat`, `command_repeat_clear`.
- [x] E4.10 `grid_get`, `grid_set`; `locks_get`, `locks_set` (texture lock, UV lock) — shared with E6.

**Editing**

- [x] E4.11 `brush_clip` (2–3 points or face plane; front / back / both).
- [x] E4.12 `face_extrude`, `face_extrude_new` (new brush, stamp).
- [x] E4.13 `vertices_move` (vertices, edges, faces; shared handles move together; UV lock).
- [x] E4.14 `vertex_add`, `vertices_remove`, `vertices_snap`.
- [x] E4.15 `csg_merge`, `csg_subtract`, `csg_intersect`, `csg_hollow` (thickness).
- [x] E4.16 Validity errors: explain non-convex, out-of-bounds and degenerate results with the involved IDs.

**Done when:** scenario S7 (column ring, spiral stairs) works; every tool has tests including invalid-input and dry-run cases.

---

## E5. Entities, NPCs and models

Goal: the agent can place, configure and link any entity using the game's definitions.

- [x] E5.1 `entity_classes_list` (prefix groups, point / brush, usage counts).
- [x] E5.2 `entity_class_describe` (description, size, color, model, typed properties, defaults, choices, spawnflags).
- [x] E5.3 `entity_create_point` (position, properties, drop to floor, apply defaults; grid snap).
- [x] E5.4 `entity_create_brush`.
- [x] E5.5 `entity_properties_set` (many entities, worldspawn), `entity_property_remove`, `entity_property_rename`.
- [x] E5.6 Value validation against property types (integer, float, choices, flags, color, link) with warnings (spec X14).
- [x] E5.7 `entity_spawnflags_set` by flag names.
- [x] E5.8 `entity_defaults_apply` (missing / existing / all).
- [x] E5.9 `entity_move_brushes` (to entity or world).
- [x] E5.10 `entity_links_get`, `entity_link` (unique targetname generation).
- [x] E5.11 `entity_model_info`, `entity_color_set`.
- [x] E5.12 Resource: entity definitions.

**Done when:** an agent places a player start, lights and monsters on floors with difficulty spawnflags, creates a door and a trigger linked to it; tests cover FGD, DEF and ENT based games.

---

## E6. Materials, UV and face attributes

Goal: the agent can texture a map as well as a human.

- [x] E6.1 `materials_list` (collection, size, usage, filters, pagination).
- [x] E6.2 `material_apply`, `material_set_current`.
- [x] E6.3 `material_replace` (patterns, scope, per-material counts, unmatched report).
- [x] E6.4 `face_attributes_get`, `face_attributes_set` (offset, scale, rotation, material, surface/content flags by name, value, color).
- [x] E6.5 `face_attributes_copy` (project, rotate axes, material only).
- [x] E6.6 `uv_align` (justify, align to edge, fit N×M, auto-fit, reset, reset to world, flip, rotate 90°), `uv_nudge`.
- [x] E6.7 `tags_list`, `tag_apply`, `tag_remove`.
- [x] E6.8 `material_preview` (small image of a material).
- [x] E6.9 Resource: materials.

**Done when:** scenario S3 passes; tests cover Standard and Valve 220 alignment and Quake 2 flags.

---

## E7. Compile maps

Goal: the agent turns the open map into a playable compiled file (e.g. a `.bsp`) for the game, reads the compiler output, and finds leaks. Running the game itself is not required here: the agent gets the path of the compiled file and can start the game on its own.

- [x] E7.1 `compile_tools_get` / `compile_tools_set`: the compile tool paths of the current game (e.g. Half-Life `csg`, `bsp`, `vis`, `rad`; Quake `qbsp`, `vis`, `light`; Quake 3 `q3map2`), with a check that each file exists and is executable.
- [x] E7.2 `compile_profiles_list`, `compile_profile_save`, `compile_profile_delete`: the editor's compile profiles with all task types (export map, run tool, copy, rename, delete files, launch engine) and variables.
- [x] E7.3 Built-in presets for game families (Half-Life, Quake, Quake 2, Quake 3), available through `compile_presets_list` and usable directly or saved as a profile: fast (no vis, fast light), normal, and full quality. Each preset exports the map, runs the tool chain, and copies the result into the game's (or mod's) `maps` folder.
- [x] E7.4 `compile_run`: runs a profile or preset in the background and returns a run handle immediately; "test" mode only reports the commands that would run. Unsaved changes are compiled from the current state (the export step writes the map file).
- [x] E7.5 `compile_status`: progress, current task, exit codes, the log tail or full log, parsed errors and warnings, leak detection, elapsed time, and on success the absolute path of the compiled file and where it was copied.
- [x] E7.6 `compile_cancel`; one compile at a time per document, with a clear error if one is already running.
- [x] E7.7 `pointfile_load` / `pointfile_unload` (returns the leak path as points and the entities nearest to its ends), `portalfile_load` / `portalfile_unload`; a leak reported by `compile_status` names the point file to load.
- [x] E7.8 Resource: compile log of a run (streaming, subscribable).
- [x] E7.9 Tests with the existing `CmdTool` stub: success, failure, cancel, test mode, output path reporting, log parsing, and presets resolving tool variables.

**Done when:** with Half-Life compile tools configured locally, an agent compiles a map built through MCP into a `.bsp` in the game's `maps` folder, and on a leak loads the point file and reports where it is (scenario S5).

---

## E8. Minimal upstream footprint

Goal: the fork stays easy to sync with upstream TrenchBroom. The MCP server lives almost entirely in new files; original TrenchBroom files carry only critical, small, clearly justified changes.

**Rules (apply to this and every later epic)**

- Code goes into `lib/TbMcpLib` or new `Mcp*` files. Original TrenchBroom files are changed only when there is no other way, and each change is as small as possible.
- Tests for anything go into new test files of `TbMcpLibTest` (or new files elsewhere); existing upstream test files are never edited.
- Every remaining upstream change is listed with its reason in 05-technical-design.md ("Upstream changes").

**Tasks**

- [x] E8.1 Audit every upstream file changed on the branch (diff against the merge base with upstream `master`); classify each change as required or avoidable; plan the removal of the avoidable ones.
- [x] E8.2 Move all tests added to upstream test files (`tst_CommandProcessor`, `tst_Map_Commands`, `tst_Map_Geometry`, `tst_Node`, `tst_LoggingHub`) into new test files in `TbMcpLibTest`, and revert the upstream test files.
- [x] E8.3 Move the MCP preferences out of `Preferences.h` into an MCP-owned header.
- [x] E8.4 Implement hollow-with-thickness inside TbMcpLib and revert the `Map_Geometry` change.
- [x] E8.5 Replace `Node::runtimeId()` with an ID registry owned by TbMcpLib if it can stay reliable across undo, redo and linked-group updates; otherwise keep the smallest possible hook and document why.
- [x] E8.6 Reduce the UI integration to the minimum: one start-up hook in `Main.cpp`, and small explicit hooks in `MapWindow`, `MapWindowManager`, `PreferenceDialog` and `CompilationDialog` instead of workarounds that depend on upstream internals.
- [x] E8.7 Keep the necessary core fixes (redo history kept after a rolled-back transaction, `canRedoCommand`, transaction depth) as small, isolated changes with their tests in `TbMcpLibTest`.
- [x] E8.8 CMake: new libraries and the bridge are added with the fewest possible lines in upstream CMake files.
- [x] E8.9 Script `scripts/upstream-footprint.sh` (new file) that lists the upstream files changed by the fork with line counts, and checks that a merge with the latest upstream `master` has no conflicts.
- [x] E8.10 Write the "Upstream changes" section of 05-technical-design.md: each remaining upstream change, where it is and why it is required.
- [x] E8.11 Shared tool helpers (`src/tools/*Utils`, `NodeJson`) take the narrowest context they need: a node, then `mdl::Map&`, then `IdRegistry`, then `CallContext`, never `ui::MapDocument`. State this rule in 05-technical-design.md §10.2; add direct unit tests in `TbMcpLibTest` over `mdl::MapFixture` for the pure model helpers (`castRay`, `intersectsInterior`, `classifyBrush`, `addBrushes`, `ScopedLockOverride`); check whether `GeometryTools` can drop its `MapDocument` dependency in `shapeExtension`.

**Done when:** upstream files carry only small explicit hooks and critical fixes (currently 17 files, +140/−5 lines), none of them an upstream test file; all test suites pass; the footprint script shows a conflict-free merge with the latest upstream `master`; shared tool helpers take no `ui::MapDocument`, and the pure model helpers have direct unit tests.

---

## E9. Organization, clipboard and import

Goal: the agent can structure a map and bring in content from other maps.

- [x] E9.1 `layers_list`, `layer_create`, `layer_rename`, `layer_remove`, `layer_reorder`.
- [x] E9.2 `layer_set_state` (current, hidden, locked, omit from export, isolate), `objects_move_to_layer`.
- [x] E9.3 `group_create`, `group_ungroup`, `group_rename`, `groups_merge`, `group_add_objects`, `group_remove_objects`.
- [x] E9.4 `group_open`, `group_close`.
- [x] E9.5 `linked_group_duplicate`, `linked_group_select`, `linked_group_separate`, `linked_group_extract`.
- [x] E9.6 `visibility_set` (hide, show, isolate, show all).
- [x] E9.7 `clipboard_copy`, `clipboard_cut`, `clipboard_paste` (original position or given point; face text applies attributes).
- [x] E9.8 `map_import`: read another map, filter by layer / group / classname / region, convert format, place at a position in a target layer, report missing materials.

**Done when:** scenario S4 passes; tests cover layer and group state, linked group updates and import across formats.

---

## E10. Agent vision and editor console

Goal: the agent can see what is happening in the editor: it checks its own work visually and reads the editor console, where errors and useful log messages appear.

The agent can check its own work visually. It renders images of the map from cameras it controls, with its own view settings, **without touching the user's views**: no camera jumps, no changed filters, no visible window, no waiting for the user to stop interacting.

**Rendering pipeline**

- [x] E10.1 Offscreen renderer: renders the map into an offscreen framebuffer that shares GPU resources (materials, models) with the editor, on the main thread, in small time slices so the UI stays responsive. Works when no map window is focused or visible.
- [x] E10.2 Independent render state per snapshot: its own camera and its own visibility and render settings, so snapshots never change the user's view filters, hidden objects, selection highlight or camera.
- [x] E10.3 Image output as MCP image content (PNG, optionally JPEG), with size limits (default 1024×768, max 2048×2048), and an option to also save the file to disk.

**Agent cameras**

- [x] E10.4 `agent_camera_set` / `agent_camera_get` / `agent_camera_list`: named cameras owned by the agent session (perspective with position, look-at or angles, FOV; orthographic top/front/side with center and zoom). They are never shown as the user's camera.
- [x] E10.5 Framing helpers: frame given objects or a box; look from a point at a target; orbit around a target by yaw/pitch/distance; place the camera at a player's eye height inside a room (per-game height from the agent guide).

**Snapshot tools**

- [x] E10.6 `view_snapshot`: render from an agent camera or inline camera, 3D or 2D (XY/XZ/YZ), with per-snapshot options:
    - face mode (textured, flat, wireframe), shading, fog, edges;
    - show/hide by smart tag (e.g. triggers, clip, skip, hint), by entity class, point entities, entity models, brush entities, patches;
    - include hidden layers/objects or not; isolate given objects;
    - highlight given objects in a color; draw bounding boxes, classnames, entity links, the leak path of a loaded point file, the grid and axes.
- [x] E10.7 `view_snapshots_around`: several images in one call (e.g. 4 sides + top) around objects or a region, returned together, with a small label per image.
- [x] E10.8 `map_plan_view` image form: top-down orthographic render of a region at a height slice, with entities marked; complements the text form from E3.
- [x] E10.9 `view_snapshot_compare`: render the same camera before and after a change (using undo history or two named states) and return both images side by side, plus a changed-pixel mask.
- [x] E10.10 `view_snapshot_user`: capture what the user currently sees in a given editor view (read-only; nothing changes).

**Editor console**

- [x] E10.11 `console_read`: returns console messages (all messages the editor logs, not only those caused by agent calls) with level (debug, info, warning, error), time, text and the document they belong to; filters by minimum level, text pattern and document; a cursor so the agent can fetch only messages newer than the last read; paginated. Messages are kept in a bounded in-memory buffer from editor start.
- [x] E10.12 Console resource `trenchbroom://console`, subscribable: the agent is notified when new messages arrive, with coalescing so a burst of messages sends one notification.
- [x] E10.13 Call results report console errors and warnings logged while the call ran (e.g. a failed texture load during `document_open`).
- [x] E10.14 `console_clear` clears the agent-visible buffer and the editor's console view.

**Quality and safety**

- [x] E10.15 Snapshots are read-only calls: they never enter the undo history and skip the busy wait for user interaction.
- [x] E10.16 Performance: a 1024×768 snapshot of a map with up to 5,000 brushes renders in under 500 ms; long multi-image calls report progress and can be cancelled.
- [x] E10.17 Tests: unit tests for camera math, framing and option handling with a fake renderer; a GPU smoke test that renders the sample map to an image and checks it is not empty and that toggling "triggers" changes the image (skipped when no GL context is available).
- [x] E10.18 Headless readiness: the offscreen renderer does not depend on a map window, so the E16 headless mode can reuse it.

**Done when:** an agent builds a room, takes a snapshot from inside at eye height and from above, and compares the images with and without triggers — while the user keeps editing in the same editor with no visible effect on their views. The agent reads a material loading error from the console without the user copying it.

---

## E11. Level-design knowledge: texturing and model-aware placement

Goal: the agent textures surfaces the way an experienced designer of that game would, and places models so that they really stand on floors and sit on furniture. Everything works for any supported game; per-game knowledge comes from data, not from hard-coded rules.

**Material profiles**

- [x] E11.1 Material profile: for each material, its kind (panel, seamless tile, trim, decal-like, sky, liquid, tool), typical scale per axis, texel density (world units per texel), typical face size, typical repeat counts, and how confident each value is (number of samples, source).
- [x] E11.2 Image analysis, independent of any game: texture size and aspect ratio, and whether opposite edges match (seamless tile) or not (panel). Used when no other data exists.
- [x] E11.3 Reference corpus: `material_corpus_scan` reads a folder of `.map` files of the game or mod (e.g. the original game's map sources) and collects per-material statistics: scales, repeat counts, face sizes, alignment to face edges. Results are cached per game and mod in the user data folder and reused by all later calls.
- [x] E11.4 Knowledge notes: `material_notes_set` / `material_notes_get` store explicit facts per game or mod ("LAB1_GAD2 is a panel, scale 0.5"), written by the agent or the user; notes override statistics.
- [x] E11.5 `material_usage`: the profile of one or more materials, merged from notes, corpus statistics, the current map and image analysis, with the source of each value.

**UV quality**

- [x] E11.6 `uv_check`: finds texturing problems on given faces or the whole map: aspect distortion above ~10%, fractional repeats on panel materials, panels not aligned to their face, scale far outside the material's typical range, texel density very different from neighbouring faces, and visible seams where coplanar neighbours do not continue the texture. Each finding names the face and a suggested fix.
- [x] E11.7 The same checks run as warnings in the results of material and UV tools (`material_apply`, `material_replace`, `face_attributes_set`, `uv_align`) and in `map_check`.
- [x] E11.8 `uv_align` fit with preserved aspect ratio: the agent sets the repeat count on one axis, the other axis follows the texture's aspect ratio; optional rounding to whole repeats; a "typical" mode that applies the material's typical scale and aligns it to the face.
- [x] E11.9 `material_fit_geometry`: for a panel material, the face size at which it fits the face exactly at its typical scale, and the resize needed for the given face — adjusting geometry to the texture instead of stretching the texture.

**Model-aware placement**

- [x] E11.10 `entity_model_info` lists a model's animations (sequences / frames with names where the format has them) and returns the real model bounds for each, not only the definition's box.
- [x] E11.11 Setting an entity's animation (the property the game uses, e.g. `sequence` or the model spec's frame) through the entity tools, with the resulting model bounds in the result.
- [x] E11.12 Placement checks use model bounds: `entity_create_point`, `objects_move` and `map_check` warn when a model penetrates brushes (floor, furniture) or floats above the surface below; `dropToFloor` can use the model bounds of the current animation.

**Tests and guide**

- [x] E11.13 Tests: corpus scan and profile merging on fixture maps; image tile detection on fixture textures; every `uv_check` finding; aspect-preserving fit; model bounds per animation and the placement warnings (e.g. a sitting model whose feet go below the floor).
- [x] E11.14 Agent guide: how to texture with profiles (check the profile, prefer the typical scale, adjust geometry for panels, run `uv_check`, look at a snapshot).

The placement checks are also available as `entity_placement_check` and the texturing checks as `uv_check`; `map_check` (E13) reuses the model placement checks.

**Done when:** after scanning a folder of reference maps, `material_usage` reports a panel and a tiling material correctly; `uv_check` finds a stretched texture and a fractional panel repeat on a fixture map; aspect-preserving fit works; placing a sitting character whose model reaches below the floor produces a warning.

---

## E12. Spatial understanding: picking, rooms, free spots, placement checks

Goal: the agent moves from "I see a problem in the picture" to "I know the object, the face and the coordinates" in one step, understands the map as rooms rather than brushes, finds where things fit, and gets placement problems reported automatically. An agent that built a whole map reported that most of its time went into this translation, not into building.

**Picture ↔ objects**

- [x] E12.1 `view_pick`: for a pixel (or a list of pixels) of a snapshot, the object, face, hit point, surface normal and distance, using the snapshot's camera. Snapshots keep their camera so they can be picked later.
- [x] E12.2 Snapshot annotations on request: labels with id, classname or group name and size; a coordinate grid on floors and walls with a chosen step; a compass; a player silhouette of the game's size as a scale reference next to a given point.

**Rooms instead of brushes**

- [x] E12.3 `spaces_list`: finds enclosed spaces by flood fill of the empty volume (at player-size resolution) and returns for each space its inner bounds, floor and ceiling heights, floor area, openings (doorways, windows) with their size and position, neighbouring spaces, and the layer or group names found in it. Spaces get stable ids until the geometry around them changes.
- [x] E12.4 `surroundings`: describes in text what is around a point: the space it is in, distances to walls in each direction, nearby objects with direction and distance, floor and ceiling height.
- [x] E12.5 `free_spots`: finds free positions for a box of a given size in a space, on the floor, or against a wall; returns positions, the wall normal, the id of the wall face, and the free clearance; options for distance from walls and other objects and for alignment to the grid.
- [x] E12.6 `walkable_plan`: top-down plan (text and image) of where a player can walk and reach, considering step height, jump height, clearance under ceilings and doorways, and doors.

**Placement checks**

- [x] E12.7 Z-fighting validator: visible coplanar overlapping faces of different brushes facing the same way (not hidden by a touching face), reported in `issuesIntroduced` of every modifying call and in `issues_list`.
- [x] E12.8 Leak prediction: point entities outside the sealed volume and openings to the void, found by flood fill before compiling, reported as issues with the entity and the nearest gap.
- [x] E12.9 Every modifying call reports placement problems it introduced: models intersecting brushes or floating (with model bounds), entities outside the hull, z-fighting, texture distortion.

**Map manifest**

- [x] E12.10 Map manifest: a per-map file next to the map (`<map>.mcp.json`) with spaces and their purpose, key points, notes and named agent cameras; tools to read and update it; agent cameras can be saved into it and restored in later sessions.

**Tests and guide**

- [x] E12.11 Tests on fixture maps for picking, annotations (fake renderer), spaces, openings, surroundings, free spots, walkability, z-fighting, leak prediction and the manifest; GPU smoke test for annotations when GL is available.
- [x] E12.12 Agent guide: navigating a map by spaces and picks, placing with free spots, keeping a manifest.

`issues_list` (E13.1) was built with E12 so that z-fighting and entities outside the hull appear in the server's issue list; the MCP checks are not registered in the editor's validator list. Per call, texture distortion is reported as `UV_ASPECT_DISTORTION`; the other UV findings come from `uv_check` and the material tools.

**Done when:** on a fixture map with two rooms and a doorway, an agent picks a chair in a snapshot and gets its id and face, lists both spaces with their doorway, finds a free wall spot for a poster with the wall face id, and gets z-fighting and an entity outside the hull reported right after the call that caused them.

---

## E13. Validation and engine launch

Goal: the agent can find and fix problems in a map and start the game with it.

- [x] E13.1 `issues_list` (types, include hidden, objects, available fixes).
- [x] E13.2 `issue_fix` (single and bulk), `issue_hide`, `issue_show`.
- [x] E13.3 `validators_list`, `validators_set`.
- [x] E13.4 `map_check`: agent-oriented checks: entities in walls or floating, missing player start, unreachable link targets, missing materials, entities outside rooms.
- [x] E13.5 `engine_profiles_list`, `engine_profile_save`, `engine_launch`.
- [x] E13.6 Resource: issues (subscribable).

**Done when:** scenario S2 passes; tests cover every quick fix and the agent-oriented checks.

---

## E14. Views, camera, generic actions, preferences, knowledge

The camera tools here move the **user's** editor camera. Agent snapshots with their own cameras are in E10.

Goal: the agent can see what it built, reach every remaining editor action, and look things up.

- [x] E14.1 `camera_get`, `camera_set`, `camera_focus`, `camera_step_pointfile`.
- [x] E14.2 `view_options_get`, `view_options_set` (render mode, shading, fog, edges, entity display, link mode, per-tag and per-class visibility).
- [x] E14.3 `view_layout_set`.
- [x] E14.4 `actions_list` — enumerate the action registry (menu, view, per-tag and per-entity actions) with label, path, shortcut, enabled / checked, opens-dialog flag.
- [x] E14.5 `action_invoke` — run any action by path; for dialog actions point to the matching semantic tool or open the dialog.
- [x] E14.6 `preferences_get`, `preferences_set` (all preferences).
- [x] E14.7 `manual_search`, `manual_section` over the bundled user manual; resource: user manual.
- [x] E14.8 Coverage check: script or test that lists every action in the registry and verifies it is reachable through a semantic tool or `action_invoke` (spec G1, coverage matrix).

**Done when:** the coverage check reports ≥ 95%, and the user's camera, view options and layout can be driven by an agent.

---

## E15. Agent experience: prompts, guide, end-to-end scenarios

Goal: agents use the server well without extra hints.

- [ ] E15.1 Agent guide resource: coordinate conventions, grid, typical dimensions per game (player size, door, step height, jump height), workflow tips, common pitfalls.
- [ ] E15.2 Review every tool description and schema for clarity; add examples.
- [ ] E15.3 Prompts: blockout a level, populate with enemies and items, lighting pass, texture pass, fix all issues, compile and debug, explain this map, explain an entity, convert / clean up a map.
- [ ] E15.4 Run scenarios S1–S7 and S9 from 04 with a real agent; record results and fix gaps.
- [ ] E15.5 User documentation: how to enable the server and connect Claude Code / Claude Desktop / IDEs; add a section to the TrenchBroom manual.
- [ ] E15.6 Lazy bridge start: the stdio bridge answers `initialize`, `tools/list`, `resources/list` and `prompts/list` itself (from the TbMcpLib registries) and launches or connects to the editor only on the first call that needs it, so starting an MCP client never opens TrenchBroom by itself.

**Done when:** all MVP and v1 scenarios pass with a real agent.

---

## E16. Headless mode, batch and advanced features (v2)

Goal: automation without the GUI and the remaining advanced editing features.

- [ ] E16.1 Headless server mode: command-line flag or separate executable that loads games and maps without a window, sharing the tool implementations.
- [ ] E16.2 Batch tools: run validation / export / compile over a folder; aggregated report (scenario S8).
- [ ] E16.3 `sweep`.
- [ ] E16.4 `patch_convert`, `patch_edit` (Quake 3).
- [ ] E16.5 `brush_create_from_planes`.
- [ ] E16.6 `prefab_save`, `prefab_list`, `prefab_insert`.
- [ ] E16.7 `linked_group_protect_property`.
- [ ] E16.8 `autosave_restore`.
- [ ] E16.9 `leak_locate` (start entity and likely gap from the point file).

**Done when:** scenario S8 passes headless; all v2 tools have tests.

