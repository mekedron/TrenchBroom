# TrenchBroom MCP Server

This fork of TrenchBroom contains an [MCP](https://modelcontextprotocol.io) server. An AI agent such as Claude Code can use it to work in the editor the way a level designer does: open and create maps, build geometry, place entities and NPCs, texture surfaces, look at the result, fix problems, compile the map and start the game.

The agent works in the live editor window next to you. Every edit it makes shows up right away and is one step in the normal Undo history, named `AI: …`.

> [!NOTE]
> Every line of this code was written by AI agents, and no human has reviewed any of it. It passes about 380 automated test cases and has been used to build real Half-Life maps, but read it as an experiment. See [Why a fork](#why-a-fork).

## At a glance

| | |
|---|---|
| Tools | 204, in 37 groups |
| Prompts | 9 ready-made tasks (block out a level, lighting pass, texture pass, fix all issues, compile and debug, …) |
| Resources | 12, several of them subscribable (editor status, selection, issues, console, compile log, …) |
| Editor coverage | 98.7% of the editor's menu and shortcut actions are reachable (390 of 395; the missing 5 are debug-only) |
| Games | Every game TrenchBroom supports; tested most with Half-Life |
| Transport | Streamable HTTP on `127.0.0.1:47100`, plus a stdio bridge (`TrenchBroomMcp`) for clients that only speak stdio |
| Upstream footprint | 26 original files changed (+216/−14 lines); the rest is in new libraries |

## What an agent can do

**Maps and games.** Create, open, save, revert and export maps (`.map`, OBJ). Set up games, mods, WADs and material collections, entity definitions and soft bounds.

**Geometry.** Boxes, stairs, arches, cylinders, cones and spheres with every option of the editor's shape tool; convex hulls; whole rooms and door openings in one call. Move, rotate, scale, shear, flip, duplicate, and make arrays along lines, grids and circles. Clip, extrude faces, edit vertices, and run CSG operations. Invalid results fail atomically, and the error names the objects involved.

**Entities, NPCs and models.** Describe any entity class from the game's definitions, including its properties, spawnflags, links and model. Place point entities and drop them onto the floor, using the real bounds of the model's current animation. Create doors, triggers and other brush entities, set properties and spawnflags by name, and link entities to each other.

**Materials that look right.** Apply and replace materials by name pattern, align and fit UVs, and set surface and content flags. Each material has a *profile* built from four sources: notes the agent keeps per game, statistics from a folder of reference maps, the current map, and analysis of the texture image (seamless tile or panel). `uv_check` finds stretched textures, fractional repeats on panels, and seams.

**Vision.** The agent has its own cameras and renders snapshots offscreen, so your view never moves. A snapshot can hide triggers or tool textures, highlight objects, and add labels, a coordinate grid, a compass and a player-size box. Other tools compare a view before and after a change, show one spot from several angles at once, or draw a top-down plan. `view_pick` turns a pixel of a snapshot into the object, face and point under it.

**Understanding space.** `spaces_list` finds the rooms of a map with their floors, ceilings, openings and neighbours. `surroundings` describes what is around a point. `free_spots` finds room for an object on a floor or a wall. `walkable_plan` shows where a player can actually go.

**Catching problems early.** Every call that changes the map reports what it broke: z-fighting, entities outside the sealed hull (a leak before you compile), models stuck in walls or floating, stretched textures. `map_check`, the editor's issue list and its quick fixes are available too.

**Compile and play.** Built-in compile presets for Half-Life (VHLT/sdHLT), Quake and Quake 2 (ericw-tools), and Quake 3 (q3map2). They run in the background, parse errors and leaks, and report the path of the compiled `.bsp`. Point files show where a leak is. Game engine profiles launch the game.

**Everything else.** Layers, groups and linked groups; clipboard; importing parts of other maps; the editor console; your camera and view options; preferences; the user manual; and any editor action by name.

## What makes it pleasant to use

- **Nothing to confirm.** The server never asks for approval. Destructive calls state their intent explicitly (`overwrite`, `unsavedChanges: "discard"`), and everything else can be undone.
- **Dry runs.** Every modifying tool can report what it would do without doing it.
- **Stable ids.** Objects keep their ids across calls, undo and redo.
- **Errors you can act on.** Each error has a code, the ids involved, and a hint for the next call.
- **Out of your way.** Agent calls wait while you drag something. A status bar indicator shows what the agent is doing, and **Stop agent** cancels it.
- **Knowledge that grows.** Material notes and a per-map manifest (rooms, key points, cameras) are kept on disk between sessions.

## Mapping skills

Two [Claude Code skills](../../.claude/skills) teach agents the craft on top of the tools:

- `trenchbroom-mapping` — a working method for any game: workflow, checking work with snapshots, texturing principles, rooms and openings, z-fighting, entities, compiling, porting pieces of other maps.
- `halflife-mapping` — GoldSrc specifics: units, WADs, textures that work and how Valve used them, entity recipes, NPC poses and scripted scenes, compiling and launching Half-Life.

Tools marked *(untested)* in the skills are covered by automated tests but have not been used on a real map yet.

## Getting started

1. Build this fork (see [BUILD.md](../../BUILD.md)).
2. Turn the server on under **Preferences → AI Agents**, or start the editor with `--mcp-server`.
3. Register it with your client, for example:
   ```bash
   claude mcp add --scope user trenchbroom -- /path/to/TrenchBroomMcp
   ```
   The bridge starts TrenchBroom the first time a tool needs it.

Details and troubleshooting are in [CONNECTING.md](CONNECTING.md).

## How it is built

- `lib/TbMcpLib` — a Qt-free core: MCP protocol, HTTP/SSE transport, registries, and every tool. It changes the map only through the editor's own model functions, inside one transaction per call.
- `lib/TbMcpUiLib` — the editor integration: TCP server, main-thread scheduling, offscreen snapshot renderer, preference pane, status bar indicator.
- `app/TrenchBroomMcp` — the stdio bridge.
- `scripts/upstream-footprint.sh` — lists every change to original TrenchBroom files and tests a merge with upstream `master`.

The design is in [05-technical-design.md](05-technical-design.md); all documents are listed in [README.md](README.md).

## Why a fork

TrenchBroom is an excellent editor, and its author, Kristian Duske, has kept it that way for more than a decade. Maintaining a project of this quality takes real effort, and TrenchBroom's contribution rules ask contributors to understand and be able to explain the code they submit. That is the right rule, and this code does not meet it: it was written by AI agents and has not been reviewed line by line by a human.

So this is an experiment in a fork, not a pull request. It was built to see how far an AI agent can get when it can really use a level editor. The honest answer so far: surprisingly far. It is usable, it is fun, and it makes interesting things possible.

The fork is kept easy to sync with upstream: MCP code lives in its own libraries, and original files carry only small hooks and a few bug fixes. Bugs found in TrenchBroom itself along the way are listed in [05-technical-design.md](05-technical-design.md) (§15 for the ones fixed here, §12 for the ones worked around).
