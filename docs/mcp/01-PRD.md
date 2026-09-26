# TrenchBroom MCP Server — Product Requirements Document

Date: 2026-09-26 · Status: Draft · Owner: mekedron

Related documents:

- [02-editor-capabilities.md](02-editor-capabilities.md) — what the editor can do today (codebase analysis)
- [03-functional-spec.md](03-functional-spec.md) — functional specification: the tools, resources and prompts of the MCP server
- [04-scenarios.md](04-scenarios.md) — end-to-end agent scenarios and acceptance criteria

---

## 1. Overview and vision

The TrenchBroom MCP Server gives an AI agent the same abilities a human level designer has in TrenchBroom: open or create a map, build geometry, place entities and NPCs, apply textures, find and fix problems, compile the map and launch it in the game engine.

**Problem.** Today TrenchBroom can only be driven by mouse and keyboard. It has no scripting, no plugins and no external API. An AI agent cannot read the state of a map, make an edit or check the result. Building a level stays fully manual, even for repetitive work: placing lights, replacing textures, blocking out a standard room, fixing validation issues.

**Solution.** An MCP (Model Context Protocol) server is a standard "port" through which any compatible AI client (Claude Code, Claude Desktop, IDE assistants) sees the editor as a set of clear actions and data. The agent works inside the live editor window next to the human: each action is visible on screen and can be reverted with the normal Undo.

**Value.**

- For designers — a co-pilot: rough blockouts, routine bulk edits, finding and fixing problems from natural-language requests.
- For modders and newcomers — a way into level design without knowing every shortcut, file format and config.
- For studios and communities — pipeline automation: batch validation, conversion and compilation of maps.

**Completeness principle.** Everything a human can do in the editor through menus, tools and panels, an agent must be able to do through MCP. Exceptions are listed explicitly under Non-goals.

## 2. Goals

| # | Goal | How we measure it |
|---|---|---|
| G1 | Full coverage of editor functionality | ≥ 95% of menu items and inspector operations reachable through MCP (see the coverage matrix in 03) |
| G2 | An agent can build a playable map from scratch | Scenario S1 in 04 passes end to end: the map compiles without leaks and runs in the engine |
| G3 | Reversible edits | 100% of modifying operations revert with a single Undo; no operation discards unsaved work implicitly (only when the agent asks for it explicitly) |
| G4 | The agent can "see" its result | The agent can get a scene description, statistics and an image of any view without human help |
| G5 | Human and agent work together | The human sees agent changes live and can stop or revert them |
| G6 | Clear for a model to use | Every tool has a description, examples and actionable error messages; an agent completes 90% of the standard tasks in 04 without extra hints |

## 3. Non-goals (first release)

- Replacing the graphical interface, or acting as a cloud rendering service.
- Generating new assets (textures, models, sounds). The agent uses what already exists in the game and mods.
- Authoring game configuration files or entity definition files (FGD/DEF/ENT). The agent can read them; editing them is out of scope.
- Networked multi-user editing.
- A user-facing scripting language. That is a separate initiative; MCP does not replace it.

## 4. Users

| Persona | Who they are | What they want from the agent |
|---|---|---|
| Level designer | Experienced TrenchBroom user | Speed up routine work: bulk replacements, texture alignment, placing lights and items, finding problems |
| Newcomer modder | Knows the game, not the editor | "Make me a room with a door and two monsters" — and an explanation of what was built |
| Technical artist / studio | Owns the build pipeline | Batch validation, compilation, problem reports, format conversion |
| AI agent (system user) | Autonomous agent working on a task | Predictable tools, machine-readable answers, fast ways to verify results |

## 5. Key user stories

1. As a designer, I ask the agent "open map01 and find all problems" and get a list of issues with explanations and proposed fixes.
2. As a newcomer, I ask "create a new Quake map: a corridor, two rooms, a door between them, lights, a player start and three monsters" and get a working draft.
3. As a designer, I ask "replace all `wall_old*` textures with `wall_new*` in the selected area", and the change reverts with one Ctrl+Z.
4. As a designer, I ask "insert the room from prefab_room.map here", and the agent imports a fragment of another map at the right place.
5. As a designer, I ask "compile and run", and the agent runs a compile profile, reads the log, loads the point file on a leak and shows me where the leak is.
6. As a studio, I point the agent at a folder of maps and get a report: maps with issues, missing textures, missing entity definitions.
7. As a designer, I ask "what does this entity do and what are its options?", and the agent explains it using the entity definition.
8. As a designer, I ask "make 10 copies of this column in a circle with radius 256", and the agent uses duplication and command repetition.

## 6. Product concept

### 6.1. Operating modes

- **Live mode (primary).** The MCP server runs inside the running TrenchBroom application. The agent works with an open document; all edits appear on screen, enter the Undo history, and the human can keep working at the same time.
- **Headless mode (phase v2).** The server runs without a window: open, validate, edit, save, export and compile maps. Used for batch processing and CI.

### 6.2. Three layers of capability

1. **Semantic tools** — high-level operations with clear parameters: "create a cuboid brush", "create an entity", "set a property", "apply a material", "select by classname". This is the main interface for the agent.
2. **Generic editor actions** — access to any named menu or shortcut action (about 180 actions plus per-entity and per-tag actions) by its path, for example "Menu/Edit/CSG/Hollow". This guarantees completeness even where no dedicated semantic tool exists.
3. **Resources and knowledge** — reading state: the map tree, the selection, materials, entity definitions, issues, the game configuration, and the user manual.

### 6.3. Interaction model

- The agent refers to map objects by stable identifiers that do not change between calls while the object exists.
- Every change is one named transaction in the Undo history (for example "AI: Create room"). The agent can group several calls into one transaction.
- Every modifying call returns what changed (created, modified and removed objects), new validation issues, and the resulting selection.
- The agent can "try" an operation first (dry run) and learn whether it would be valid — for example, whether a brush would stay convex.
- Long operations (loading a map, compiling) report progress and can be cancelled.

## 7. Non-functional requirements

### 7.1. Full access, no confirmations

- **Every operation is allowed to the agent by default.** The server never blocks an operation or waits for human approval: saving and overwriting files, closing or reverting maps, deleting objects, running compile tools and engines, changing game paths, mods, WAD lists, entity definitions, compile and engine profiles, and preferences.
- Deciding whether to ask the user is the agent's responsibility. If the agent wants confirmation, it asks the user itself through its own client.
- The server has no read-only mode, no file system allow-list and no "sensitive operation" list.
- The server listens for connections from the local machine by default; remote access is a setting. This is a transport default, not a limit on what a connected agent can do.
- The user enables the MCP server in Preferences. The UI shows that an agent is connected and what it is doing.

### 7.2. Reversibility

- Every agent edit can be undone, and is labelled as an agent action in the history.
- Destructive operations state their intent through explicit parameters (for example "discard unsaved changes", "overwrite existing file"), so nothing destructive happens by accident. They never require approval.
- The editor's autosave and backups keep working as usual.

### 7.3. Performance

- Reading the state of a small map (up to 5,000 brushes): under 200 ms.
- A simple edit (create a brush, set a property): under 100 ms, excluding redraw.
- Large results are paginated; the agent can request only the fields it needs.
- The editor UI never freezes for more than 1 second because of agent work.

### 7.4. Reliability and clear errors

- A failed call does not change the map (atomicity): an operation is either fully applied or not applied at all.
- Error messages explain the cause and suggest a fix, e.g. "The brush would become non-convex; try a smaller vertex offset."
- A server failure never crashes the editor.

### 7.5. Compatibility

- Works on Windows, macOS and Linux — wherever TrenchBroom runs.
- Supports every game and map format the editor supports, with no per-game special setup.
- Compatible with the current MCP specification and major clients (Claude Code, Claude Desktop, IDEs).

### 7.6. Observability

- A log of all agent calls is available in the editor console and in a separate file.
- The user can review which changes the agent made during a session.

## 8. Phases

| Phase | Scope | Done when |
|---|---|---|
| MVP | Live mode; document (open, create, save); reading the map tree and selection; primitive brushes; entities and properties; materials on faces; selection; Undo/Redo; validation issues; generic editor actions | Scenarios S1–S3 in 04 are completed by an agent |
| v1 | Full geometry (transforms, CSG, clip, vertices, extrude, sweep); layers, groups, linked groups; UV alignment and face attributes; importing map fragments; compile and launch; point/portal files; camera and view snapshots; prompt templates | All scenarios in 04; coverage ≥ 95% |
| v2 | Headless mode; batch processing of folders; Quake 3 patches; richer agent "vision" (multi-angle renders, top-down layout plans); remote access | Batch scenario S8 runs without the GUI |

## 9. Risks and assumptions

| Risk | Impact | Mitigation |
|---|---|---|
| Agents struggle to reason about 3D space from text | Misaligned, overlapping structures | View snapshots, top-down scene descriptions, overlap and leak checks |
| Agent edits interfere with a human working at the same time | Lost work, confusion | Transactions, agent activity indicator, "stop agent" button, Undo, autosave |
| Large maps produce huge responses | Model context overflow | Pagination, filters, summaries instead of full dumps |
| Agent runs external programs or overwrites files | Unwanted side effects | Allowed by design; every call is logged, map edits are undoable, autosave keeps backups. Asking the user is the agent's choice |
| Games differ (formats, flags, entities) | Agent uses wrong parameters | Tools to read the game config and entity definitions; validation of property values |
| The editor has no screenshot feature today | G4 cannot be met without new work | Add view-to-image rendering as part of this project |
| No import or prefab feature exists today | Story 4 needs new work | Build "import map fragment" on top of the existing paste mechanism |

**Assumptions.**

- The user has configured game and engine paths in TrenchBroom; the agent can read and change them.
- Compile tools (qbsp, vis, light, etc.) are installed by the user; MCP does not ship them.
- In the first release the agent works with one document at a time (the editor may have several windows open; the agent picks which one it works with).

## 10. Open questions

- Do we ship a high-level "construction kit" (room, corridor, stairs, door with trigger), or does the agent build everything from primitives?

**Decided (2026-09-26):**

- No human review or confirmation step. The agent edits the live map directly, and every operation is allowed by default; the agent asks the user itself when it wants to.
- The agent may change anything the editor can change, including mods, WAD lists, game paths, profiles and preferences.
