# TrenchBroom MCP Server — Scenario Results

Date: 2026-09-27 · Parent: [04-scenarios.md](04-scenarios.md)

The scenarios of [04-scenarios.md](04-scenarios.md) were run by an AI agent (Claude) through the MCP server alone:
a separate editor on the `offscreen` Qt platform with its own user profile, driven through the stdio bridge
`TrenchBroomMcp`. The agent read the agent guide and the tool descriptions, made its own design decisions, and judged
its work from PNG snapshots. The scenarios ran twice: once to find gaps, and again on the final build to confirm the
fixes. This document describes the results on the final build.

**Environment.** Half-Life with its game data and the sdHLT compile tools. Quake, Quake 2 and Quake 3 have no game
data on the test machine, so scenarios that need textures or a compiler use Half-Life; the Quake entity classes were
checked without textures. No game was launched (the engine launch is covered by `tst_EngineTools` and
`tst_McpEngineHost`).

## Summary

| ID | Scenario | Result |
|---|---|---|
| S1 | Build a small playable map from scratch | **Pass** (Half-Life, compiled without a leak); engine launch not run |
| S2 | Audit and fix an existing map | **Pass** |
| S3 | Bulk material replacement | **Pass** |
| S4 | Import a room from another map | **Pass** |
| S5 | Compile, find and fix a leak | **Pass** |
| S6 | Populate a level with enemies and items | **Pass**, except difficulty flags, which Half-Life does not have (see S6) |
| S7 | Repetitive construction | **Pass** |
| S8 | Batch audit of a folder of maps | v2, not run |
| S9 | Human and agent work together | Needs a human; covered by automated tests (see S9) |

## S1. Build a small playable map from scratch

Half-Life, Valve 220 format, `halflife.wad`. Two 512×512 rooms (160 high, the height of the wall texture), a 160×128
corridor, a `func_door` that moves up in the doorway of room 2, a light in each room and in the corridor, an
`info_player_start` in room 1, and a zombie, a bullsquid and a headcrab in room 2. About 30 calls: `document_new`
(template brush deleted via `initialObjects`), `document_save_as`, `materials_collections_set`, `room_create` ×2 with
groups, the corridor from boxes, `group_open` / `opening_cut` / `group_close`, `entity_create_brush`,
`entity_create_point` with `dropToFloor`, `spaces_list`, `free_spots`, snapshots, `uv_check`, `map_check`,
`compile_run`.

| Criterion | Result |
|---|---|
| Opens without issues in the Issue Browser | Pass: `issues_list` 0 issues, `map_check` 0 findings |
| Monsters and player start on floors, inside rooms, not intersecting | Pass: every create reports `onFloor` and no overlaps, origins are integers, `spaces_list` puts them in the right rooms, snapshots confirm it |
| The door separates the corridor from room 2 | Pass: the door fills the doorway; `spaces_list` lists it for that opening only |
| Undo step by step, each step "AI: …" | Pass: 31 steps, all "AI: …"; undoing all of them restored the empty map, redoing restored it exactly |
| (v1) Compiles without a leak | Pass: `compile_run` normal preset, 4 tools exit 0, no leak |
| (v1) The engine launches it | Not run by design (see Environment) |

A texture-less Quake copy confirmed the Quake classes: player 32×32×56 with the origin 24 above the floor, "Not on
Easy / Normal / Hard" spawnflags.

## S2. Audit and fix an existing map

A copy of a decompiled Half-Life original (`c1a0d.map`, Standard format, 1129 brushes, 268 entities, a hidden layer with
1356 objects). The agent listed every issue with its code, object and explanation (`issues_list`, `map_check`,
`uv_check`), deleted one empty `func_wall` at the origin (the only entity outside the hull), snapped the vertices of 7
detail brushes inside brush entities (not the world brushes, where snapping can open gaps), and listed everything else
with reasons: the editor's "unused targetname" issues are false positives for Half-Life logic (multi_manager keys,
`m_iszEntity`), the z-fighting comes from the decompile, and the UV distortions are Valve's own texture scales.

| Criterion | Result |
|---|---|
| Every issue with type, object and explanation | Pass |
| Safe quick fixes applied; every deleted or changed object reported | Pass: the `issue_fix` change report names the removed entity and the 7 brushes |
| The issue count drops; remaining issues listed with reasons | Pass: `map_check` findings drop to 16, `issues_list` to about 350 |
| Fixes are named undo steps | Pass: two steps "AI: Fix Issues" |

## S3. Bulk material replacement

A Half-Life map with layers "Castle" and "Village" and two material families (`OUT_WALL*` → `LAB1_W*`,
`C1A3_W1*` → `C1A1_W1*`), with non-default alignment on some faces. `material_replace` with two rules and
`{"layer": "Castle"}`, dry run first.

| Criterion | Result |
|---|---|
| Only Castle faces change; counts per material | Pass: 36 faces (12 + 18 + 6); the Village layer shows 0 changed pixels in `view_snapshot_compare` |
| Materials without a match are reported, not guessed | Pass: `C1A1_W1C`, `C1A1_W1E`, `LAB1_W5A` reported in `unmatched`; the faces of a hidden brush are reported as skipped |
| Texture alignment is preserved | Pass: offset, scale, rotation and texture axes identical on all 102 faces |
| One Ctrl+Z reverts the whole change | Pass: one step "AI: Replace Materials"; after `undo` all faces equal the state before |

## S4. Import a room from another map

A Standard-format prefab map with groups "Armory" and "Storage" (one material only in `xeno.wad`), and a Valve-format
target map with a selected room and an obstacle east of it. `map_file_inspect`, `space_check` to find a free place,
`map_import` with a group filter, a position and an anchor.

| Criterion | Result |
|---|---|
| Lists the groups, imports the right one, converts the format, no overlap | Pass: only Armory imported (8 brushes, 5 entities), Valve 220 texture axes, flush against the east wall, touching but not overlapping the obstacle |
| Missing materials reported | Pass: `missingMaterials: ["C4A1A_SWMPFLR"]` and a `MISSING_MATERIALS` warning |
| Imported objects selected and in the current layer | Pass: the imported group is selected and in the current layer "Imports"; one undo step |

## S5. Compile, find and fix a leak

A script secretly opened a gap in a copy of the S1 map (moving a wall or floor brush); the agent learned what it did
only after fixing it.

| Criterion | Result |
|---|---|
| On a leak: load the point file, identify the start entity and the gap, fix it | Pass: `map_check` (ENTITY_OUTSIDE_HULL with the gap box) and `spaces_list` (`sealed: false`, openings to the void) predict the leak before compiling; `compile_status` names the leak entity and position from the sdHLT log; `pointfile_load` returns the path, the nearest entity and `leavesMapAt`; a leak-path snapshot and `objects_find` in the region locate the moved brush. For a small hole through a wall, `map_check` offers a ready `brush_create_box` fix, which closes the leak |
| A second compile succeeds without a leak | Pass |

## S6. Populate a level with enemies and items

A four-room version of S1. The agent read the classes and all spawnflags from the definitions (`entity_classes_list`,
`entity_class_describe`), planned weaker enemies near the start (headcrab) and harder ones farther away (houndeyes and
a zombie, then a human grunt squad with a squad leader and an alien grunt), placed health kits and batteries before and
after fights with `free_spots` and `entity_create_point` (`dropToFloor`, facing the room entrance), and summarized what
it placed where.

| Criterion | Result |
|---|---|
| Classes and spawnflags read from the definitions | Pass |
| On floors, in free space, facing plausible directions | Pass: integer origins, no overlaps, snapshots at eye height |
| Difficulty spawnflags as requested | Not expressible in Half-Life: its FGD has no difficulty flags and the game has no per-skill entity filtering. The agent reported this and put the "hard only" monsters into their own layer, which a build leaves out with `layer_set_state {"omitFromExport": true}`; the `populate_level` prompt suggests the same. On the Quake copy, "Not on Easy" and "Not on Normal" give spawnflags 768 as expected |
| Summary of what was placed where | Pass |

## S7. Repetitive construction

| Request | Result |
|---|---|
| 12 columns in a circle with radius 384 around the room center | Pass: `objects_array` circle, all at radius 384.00, 30° apart, the front face of each faces the center; 8 rotated columns reported as NON_INTEGER_VERTICES; one undo step. The `count` of `objects_array` includes the original |
| A spiral staircase with 20 steps | Pass: a pole and one wedge step arrayed with `rise` 16 (below the Half-Life step height 18), one undo step through a transaction; `walkable_plan` shows the top step reachable. Inside the transaction calls report no undo step, and `transaction_commit` returns the net changes |

## S9. Human and agent work together

S9 needs a human at the editor, so it is covered by automated tests instead:

| Criterion | Tests |
|---|---|
| The editor shows that the agent is connected and what it is doing | `tst_McpServerController.cpp` "status indicator", "adds the status indicator to the open map windows"; `formatMcpActivity` |
| Agent calls wait while the human drags or uses a modal tool | `tst_CallRunner.cpp` "busy gate" (calls wait, a human transaction makes the document busy); `tst_QtMcpHost.cpp` "busyState" (modal dialog, mouse drag, modal tools); `tst_McpServerController.cpp` "busy wait timeout" |
| The human can stop the agent with one click and undo any agent step | `tst_McpServer.cpp` "stopAgents"; `tst_McpServerController.cpp` "stopAgents keeps the server listening"; `tst_HistoryTools.cpp` (undo of agent steps) |
| Unsaved human work is never discarded implicitly | `tst_DocumentTools.cpp` "document_close" / "unsaved changes require a decision", `document_new` and `document_open` with `unsavedChanges` |

## Gaps found and fixed

The first run found these gaps; all are fixed and covered by tests in `TbMcpLibTest`, and the second run confirmed
them in the editor.

| Area | Gap | Fix |
|---|---|---|
| Materials | A relative WAD path (`valve/halflife.wad`) loaded in the editor but broke the Half-Life compiler | `materials_collections_set` stores WAD paths it can find as absolute paths (`keepRelative` keeps them, with a `RELATIVE_WAD_PATH` warning); `compile_run` warns about relative WAD paths |
| Compiling | `compile_status` did not read the leak entity of sdHLT logs (`Entity light at ( … )`) | The leak parser reads the `@`, `at` and `at:` forms and the id qbsp occupant line |
| Compiling | `pointfile_load` returned `leavesMapAt: null` for gaps between rooms | Falls back to the space analysis: the first path point in the void |
| Spaces | `spaces_list` listed a door in openings far away from it | Doors must touch the opening's box |
| Spaces | `free_spots` ignored `wallDistance` for walls in groups | Grouped brushes count as walls when their group encloses the spot |
| Spaces | `openingSize` was hard to use | Descriptions state the rule; `OPENING_SIZE_TOO_LARGE` warns when no room is left |
| Placement checks | Lights, sounds and sprites got MODEL_* issues (with a fix that moved lights to the floor); issues_list and map_check disagreed | One shared placement rule for issues_list, issuesIntroduced, entity_placement_check, the entity tools' warnings and map_check |
| Placement checks | Hidden floors were ignored (false MODEL_NO_FLOOR); drop to floor ignored hidden floors | Hidden brushes count (only layers omitted from export do not) |
| Placement checks | MODEL_BELOW_FLOOR for 1 unit of animation bounds | 2-unit tolerance for model bounds |
| map_check | Logic entities (multisource, scripted_sentence, …) inside brushes were errors; "?." in a message; ENTITY_OUTSIDE_HULL fixes with `tool: null` | Logic classes are skipped; message fixed; a real `brush_create_box` fix for holes, or no fix with an explanation |
| issuesIntroduced | Snap Vertices re-reported existing UV and z-fighting issues | Issues are matched by brush, material and plane orientation |
| Entities | `dropToFloor` wrote fractional origins | Dropped heights are integers (rounded up, never sinking) |
| Geometry | `room_create` / `brush_create_shape` with a group reported the brushes as modified | The group is created with the brushes inside |
| Geometry | `opening_cut` on walls in a closed group failed without saying what to do | The error names the group and the `group_open` → cut → `group_close` steps |
| Transactions | Calls inside a transaction reported an undo step; the commit reported no changes | `undoStep` is null inside; `transaction_commit` returns the net changes |
| Scene | `object_get` ignored unknown `fields` | `UNKNOWN_FIELD` warning |
| UV check | UV_SEAM said "1 x 1 vs 1 x 1" | The message names what differs (scale, rotation or texture axes) |
| Plan view | `map_plan_view` set `entitiesTruncated` wrongly and did not label entities above the slice | Fixed |
| Documents | New maps contain the game's template brush, which z-fights with the first room | `document_new` returns `initialObjects` with an `INITIAL_OBJECTS` warning; the guide says to delete it |
| Guide | Snapshot options and camera arguments were described inaccurately; cameras are per session | Guide rewritten (E15.1) |

## Open gaps

Minor problems found in the second run and not fixed yet:

- `transaction_commit` returns the net changes but not the issues introduced inside the transaction (each call inside
  reports its own).
- `monster_sitting_scientist` is checked with its default animation, and a desk top above the origin can be taken
  as its floor, which gives false MODEL_* findings.
- When Snap Vertices splits a face, new UV findings on the split faces are counted once, and the face count change is
  not warned about. Every `issue_fix` step is named "AI: Fix Issues".
- `map_plan_view` labels of entities at the same position overlap.
- `map_check` offers no ready fix for thin slits (only for holes whose passage runs across the wall).
- `entity_placement_check` with explicit ids refuses hidden entities (OBJECT_NOT_EDITABLE), although it only reads.
- `infodecal` entities slightly inside walls are reported as ENTITY_OUTSIDE_SPACES.
