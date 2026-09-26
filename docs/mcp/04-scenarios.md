# TrenchBroom MCP Server — Scenarios and Acceptance Criteria

Date: 2026-09-26 · Status: Draft · Parent: [01-PRD.md](01-PRD.md)

Each scenario is an end-to-end task an agent must complete through the MCP server alone, with the editor open. The phase column says from which release the scenario must pass.

| ID | Scenario | Phase |
|---|---|---|
| S1 | Build a small playable map from scratch | MVP (without compile), v1 (with compile) |
| S2 | Audit and fix an existing map | MVP |
| S3 | Bulk material replacement | MVP |
| S4 | Import a room from another map | v1 |
| S5 | Compile, find and fix a leak | v1 |
| S6 | Populate a level with enemies and items | v1 |
| S7 | Repetitive construction (column ring, staircase) | v1 |
| S8 | Batch audit of a folder of maps | v2 |
| S9 | Human and agent work together | MVP |

---

## S1. Build a small playable map from scratch

**Request:** "Create a new Quake map: two 512×512 rooms joined by a corridor, a door between the corridor and the second room, lights in each room, a player start in the first room and three monsters in the second."

**Expected flow:** check that Quake is configured → create a new map (Valve format) → read typical dimensions from the agent guide → build rooms and corridor → cut openings → create a `func_door` from a brush → apply materials by surface type → place lights, `info_player_start` and three monsters on the floors → validate → save → (v1) compile and launch.

**Acceptance criteria:**

- The map opens in TrenchBroom without issues in the Issue Browser.
- All monsters and the player start stand on floors, inside rooms, not intersecting brushes.
- The door separates the corridor from the second room.
- The whole build can be undone step by step, each step named "AI: …".
- (v1) The map compiles without a leak, and the engine launches it.

## S2. Audit and fix an existing map

**Request:** "Open maps/e1m1_edit.map, find all problems and fix what can be fixed safely."

**Acceptance criteria:**

- The agent reports every issue with its type, object and explanation.
- Quick fixes are applied where they resolve the issue; the agent reports every object it deleted or changed.
- After the fixes, the issue count drops, and the remaining issues are listed with reasons.
- All fixes are one or more named Undo steps.

## S3. Bulk material replacement

**Request:** "Replace every `wall_old*` material with the matching `wall_new*` one, only in the 'Castle' layer."

**Acceptance criteria:**

- Only faces in the Castle layer change; the agent reports how many faces changed per material.
- Materials that have no match are reported, not guessed.
- Texture alignment is preserved.
- One Ctrl+Z reverts the whole change.

## S4. Import a room from another map

**Request:** "Insert the 'Armory' group from prefabs/rooms.map next to the east wall of the selected room."

**Acceptance criteria:**

- The agent lists groups in the other map, imports the right one, converts it to the current format and places it without overlapping existing brushes.
- Materials that are missing in the current map's collections are reported.
- Imported objects are selected and in the current layer.

## S5. Compile, find and fix a leak

**Request:** "Compile the map. If it leaks, find and fix the leak."

**Acceptance criteria:**

- On a leak, the agent loads the point file, identifies the start entity and the likely gap, and proposes or makes a fix (e.g. closing a gap with a brush).
- A second compile succeeds without a leak.

## S6. Populate a level with enemies and items

**Request:** "Add enemies and health to this level: harder enemies further from the start, some only on hard difficulty."

**Acceptance criteria:**

- The agent reads available monster and item classes and their spawnflags from the definitions.
- Entities are placed on floors, in free space, facing plausible directions.
- Difficulty spawnflags are set as requested.
- The agent summarizes what was placed where.

## S7. Repetitive construction

**Request:** "Make 12 copies of the selected column in a circle with radius 384 around the room center." / "Build a spiral staircase with 20 steps."

**Acceptance criteria:**

- Copies are evenly spaced and rotated to face the center.
- Coordinates are on the grid where possible; non-integer vertices are reported.
- The whole operation is one Undo step.

## S8. Batch audit of a folder of maps (headless)

**Request:** "Check every map in maps/ and give me a report."

**Acceptance criteria:**

- Runs without opening editor windows.
- The report lists per map: game, format, issue counts by type, missing materials, missing entity definitions, maps that fail to load.
- No map file is modified.

## S9. Human and agent work together

**Situation:** the human is editing while the agent works on another part of the map.

**Acceptance criteria:**

- The editor shows that the agent is connected and what it is doing.
- Agent calls wait while the human is dragging or using a modal tool.
- The human can stop the agent with one click and undo any agent step.
- Unsaved human work is never discarded implicitly; the agent must request it explicitly.
