# TrenchBroom MCP Server — Task Tracker (temporary)

Temporary working file. Delete it once all epics are done.

Specs: [01-PRD.md](01-PRD.md) · [03-functional-spec.md](03-functional-spec.md) · [04-scenarios.md](04-scenarios.md)

**How we work:** one epic at a time, all of its subtasks in one go. Each epic ends buildable, with tests passing (Catch2, `<Name>LibTest` targets, run through `ctest --test-dir <build>/lib/<Name>/test -j`) and formatted with clang-format. Mark `[x]` when a subtask is done.

**Global rules for every tool** (spec §1): stable object IDs, one call = one "AI: …" Undo step, explicit transactions, atomic calls, change report, dry run, explicit IDs or current selection, actionable errors, pagination and field selection, no confirmations, game-aware validation.

| Epic | Area | Spec sections | Phase | Status |
|---|---|---|---|---|
| E1 | Server foundation and editor integration | §1, §2, §14 | MVP | Done |
| E2 | Documents, games and assets | §3, §4 | MVP + v1 | Done |
| E3 | Scene inspection, selection and resources | §5, §6, §21 | MVP + v1 | Done |
| E4 | Geometry: creation, transforms, editing, CSG | §7, §8, §9 | MVP + v1 | Done |
| E5 | Entities, NPCs and models | §10 | MVP + v1 | Not started |
| E6 | Materials, UV and face attributes | §11 | MVP + v1 | Not started |
| E7 | Organization, clipboard and import | §12, §13 | v1 | Not started |
| E8 | Validation, compile, run and debug | §15, §16 | MVP + v1 | Not started |
| E9 | Views, camera, snapshots, generic actions, preferences, knowledge | §17, §18, §19 | MVP + v1 | Not started |
| E10 | Agent experience: prompts, guide, end-to-end scenarios | §22, 04 | v1 | Not started |
| E11 | Headless mode, batch and advanced features | v2 items | v2 | Not started |

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
- [x] E3.8 `map_plan_view` (text form: top-down grid of a region at a height; image form delivered in E9).
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

- [ ] E5.1 `entity_classes_list` (prefix groups, point / brush, usage counts).
- [ ] E5.2 `entity_class_describe` (description, size, color, model, typed properties, defaults, choices, spawnflags).
- [ ] E5.3 `entity_create_point` (position, properties, drop to floor, apply defaults; grid snap).
- [ ] E5.4 `entity_create_brush`.
- [ ] E5.5 `entity_properties_set` (many entities, worldspawn), `entity_property_remove`, `entity_property_rename`.
- [ ] E5.6 Value validation against property types (integer, float, choices, flags, color, link) with warnings (spec X14).
- [ ] E5.7 `entity_spawnflags_set` by flag names.
- [ ] E5.8 `entity_defaults_apply` (missing / existing / all).
- [ ] E5.9 `entity_move_brushes` (to entity or world).
- [ ] E5.10 `entity_links_get`, `entity_link` (unique targetname generation).
- [ ] E5.11 `entity_model_info`, `entity_color_set`.
- [ ] E5.12 Resource: entity definitions.

**Done when:** an agent places a player start, lights and monsters on floors with difficulty spawnflags, creates a door and a trigger linked to it; tests cover FGD, DEF and ENT based games.

---

## E6. Materials, UV and face attributes

Goal: the agent can texture a map as well as a human.

- [ ] E6.1 `materials_list` (collection, size, usage, filters, pagination).
- [ ] E6.2 `material_apply`, `material_set_current`.
- [ ] E6.3 `material_replace` (patterns, scope, per-material counts, unmatched report).
- [ ] E6.4 `face_attributes_get`, `face_attributes_set` (offset, scale, rotation, material, surface/content flags by name, value, color).
- [ ] E6.5 `face_attributes_copy` (project, rotate axes, material only).
- [ ] E6.6 `uv_align` (justify, align to edge, fit N×M, auto-fit, reset, reset to world, flip, rotate 90°), `uv_nudge`.
- [ ] E6.7 `tags_list`, `tag_apply`, `tag_remove`.
- [ ] E6.8 `material_preview` (small image of a material).
- [ ] E6.9 Resource: materials.

**Done when:** scenario S3 passes; tests cover Standard and Valve 220 alignment and Quake 2 flags.

---

## E7. Organization, clipboard and import

Goal: the agent can structure a map and bring in content from other maps.

- [ ] E7.1 `layers_list`, `layer_create`, `layer_rename`, `layer_remove`, `layer_reorder`.
- [ ] E7.2 `layer_set_state` (current, hidden, locked, omit from export, isolate), `objects_move_to_layer`.
- [ ] E7.3 `group_create`, `group_ungroup`, `group_rename`, `groups_merge`, `group_add_objects`, `group_remove_objects`.
- [ ] E7.4 `group_open`, `group_close`.
- [ ] E7.5 `linked_group_duplicate`, `linked_group_select`, `linked_group_separate`, `linked_group_extract`.
- [ ] E7.6 `visibility_set` (hide, show, isolate, show all).
- [ ] E7.7 `clipboard_copy`, `clipboard_cut`, `clipboard_paste` (original position or given point; face text applies attributes).
- [ ] E7.8 `map_import` — new feature: read another map, filter by layer / group / classname / region, convert format, place at a position in a target layer, report missing materials.

**Done when:** scenario S4 passes; tests cover layer and group state, linked group updates and import across formats.

---

## E8. Validation, compile, run and debug

Goal: the agent can find and fix problems and get the map into the game.

- [ ] E8.1 `issues_list` (types, include hidden, objects, available fixes).
- [ ] E8.2 `issue_fix` (single and bulk), `issue_hide`, `issue_show`.
- [ ] E8.3 `validators_list`, `validators_set`.
- [ ] E8.4 `map_check` — new agent-oriented checks: entities in walls or floating, missing player start, unreachable link targets, missing materials, entities outside rooms.
- [ ] E8.5 `compile_profiles_list`, `compile_profile_save`, `compile_profile_delete`.
- [ ] E8.6 `compile_run` (background, test mode), `compile_status` (progress, exit codes, log tail, detected leaks and errors), `compile_cancel`.
- [ ] E8.7 `engine_profiles_list`, `engine_profile_save`, `engine_launch`.
- [ ] E8.8 `pointfile_load` / `pointfile_unload` (returns leak path points), `portalfile_load` / `portalfile_unload`.
- [ ] E8.9 Resources: issues (subscribable), compile log (streaming), editor console (streaming).

**Done when:** scenarios S2 and S5 pass (with real compile tools configured locally); tests cover issue fixes and compile runner control with the existing `CmdTool` stub.

---

## E9. Views, camera, snapshots, generic actions, preferences, knowledge

Goal: the agent can see what it built, reach every remaining editor action, and look things up.

- [ ] E9.1 `camera_get`, `camera_set`, `camera_focus`, `camera_step_pointfile`.
- [ ] E9.2 `view_snapshot` — new feature: render the 3D or a 2D view to an image (current or given camera, size, render options).
- [ ] E9.3 `map_plan_view` image form (top-down render of a region).
- [ ] E9.4 `view_options_get`, `view_options_set` (render mode, shading, fog, edges, entity display, link mode, per-tag and per-class visibility).
- [ ] E9.5 `view_layout_set`.
- [ ] E9.6 `actions_list` — enumerate the action registry (menu, view, per-tag and per-entity actions) with label, path, shortcut, enabled / checked, opens-dialog flag.
- [ ] E9.7 `action_invoke` — run any action by path; for dialog actions point to the matching semantic tool or open the dialog.
- [ ] E9.8 `preferences_get`, `preferences_set` (all preferences).
- [ ] E9.9 `manual_search`, `manual_section` over the bundled user manual; resource: user manual.
- [ ] E9.10 Coverage check: script or test that lists every action in the registry and verifies it is reachable through a semantic tool or `action_invoke` (spec G1, coverage matrix).

**Done when:** an agent can take snapshots from several angles of what it built; the coverage check reports ≥ 95%.

---

## E10. Agent experience: prompts, guide, end-to-end scenarios

Goal: agents use the server well without extra hints.

- [ ] E10.1 Agent guide resource: coordinate conventions, grid, typical dimensions per game (player size, door, step height, jump height), workflow tips, common pitfalls.
- [ ] E10.2 Review every tool description and schema for clarity; add examples.
- [ ] E10.3 Prompts: blockout a level, populate with enemies and items, lighting pass, texture pass, fix all issues, compile and debug, explain this map, explain an entity, convert / clean up a map.
- [ ] E10.4 Run scenarios S1–S7 and S9 from 04 with a real agent; record results and fix gaps.
- [ ] E10.5 User documentation: how to enable the server and connect Claude Code / Claude Desktop / IDEs; add a section to the TrenchBroom manual.
- [ ] E10.6 Lazy bridge start: the stdio bridge answers `initialize`, `tools/list`, `resources/list` and `prompts/list` itself (from the TbMcpLib registries) and launches or connects to the editor only on the first call that needs it, so starting an MCP client never opens TrenchBroom by itself. Then `--no-launch` stops being necessary.

**Done when:** all MVP and v1 scenarios pass with a real agent.

---

## E11. Headless mode, batch and advanced features (v2)

Goal: automation without the GUI and the remaining advanced editing features.

- [ ] E11.1 Headless server mode: command-line flag or separate executable that loads games and maps without a window, sharing the tool implementations.
- [ ] E11.2 Batch tools: run validation / export / compile over a folder; aggregated report (scenario S8).
- [ ] E11.3 `sweep`.
- [ ] E11.4 `patch_convert`, `patch_edit` (Quake 3).
- [ ] E11.5 `brush_create_from_planes`.
- [ ] E11.6 `prefab_save`, `prefab_list`, `prefab_insert`.
- [ ] E11.7 `linked_group_protect_property`.
- [ ] E11.8 `autosave_restore`.
- [ ] E11.9 `leak_locate` (start entity and likely gap from the point file).
- [ ] E11.10 `view_snapshots_around` (multi-angle snapshots).

**Done when:** scenario S8 passes headless; all v2 tools have tests.
