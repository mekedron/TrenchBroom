# TrenchBroom Editor Capabilities — Analysis Summary

Date: 2026-09-26 · Source: codebase analysis of this repository (manual at `app/TrenchBroom/resources/documentation/manual/index.md`, libraries under `lib/`, game configs under `app/TrenchBroom/resources/games/`).

This document lists what a user can do in TrenchBroom today. It is the baseline the MCP server must cover. Gaps that the MCP project must fill are marked **Gap**.

---

## 1. Documents and files

- **New map:** choose a game and, if the game allows several, a map format. The map starts from the game's initial map template, or a single brush at the origin.
- **Open / Open Recent:** the game and format are detected from the map file header; otherwise the user picks them. Up to 10 recent files.
- **Save / Save As:** `.map` files only. A header records game, format and generator.
- **Revert:** discard unsaved changes and reload from disk (after confirmation).
- **Close:** prompts to save or discard; also asks before stopping a running compile.
- **Export:** a copy as `.map` (layers marked "omit from export" are left out) or Wavefront OBJ + MTL (texture paths relative to the game or to the export folder).
- **Autosave:** every 10 minutes if the map has changed, into an `autosave/` folder next to the map, up to 50 backups.
- **Welcome window:** New, Open, recent files.
- **Gap:** no "import map" or prefab feature; other maps can only be brought in by copy and paste.
- **Gap:** no explicit map format conversion; the format is fixed when the map is created or opened. Paste converts fragments into the current map's format.

## 2. Games and map formats

**Map formats:** Standard (Quake), Valve 220, Quake 2, Quake 2 (Valve), Hexen 2, Daikatana, Quake 3 (legacy), Quake 3 (Valve), Quake 3 brush primitives (partially supported).

**Built-in games:** Quake, Quake 2, Quake 3, Half-Life, Hexen 2, Daikatana, Heretic 2, Soldier of Fortune, Kingpin, D-Day: Normandy, Digital Paint: Paintball 2, Quetoo, Wrath, Neverball, Generic.

**A game configuration defines:** supported formats and initial maps; file system (search path and package type: PAK, PK3/ZIP, Daikatana PAK); material settings (root folder, extensions, palette, WAD property, exclusions); entity definitions and default model scale; smart tags; surface and content flags; soft map bounds; compile tool names.

**Per-game preferences:** game folder path; compile tool paths; game engine profiles (name + executable).

**Per-map settings (stored in the map):** enabled mods and their priority; external entity definition file; enabled material collections or WAD list; soft map bounds; layers and groups.

## 3. Brush geometry

Brushes are always convex. Every operation is checked first; if any selected brush would become invalid or leave the world bounds, the whole operation is refused.

**Creating brushes**

- **Shape tool:** Cuboid, Stairs, Arch, Cylinder (optionally hollow), Cone, UV sphere, Icosphere. Parameters: axis, number of sides, circle mode (edge-aligned, vertex-aligned, scalable), hollow thickness, arch fill, sphere rings, icosphere subdivision, stair step height and direction.
- **Brush tool:** place points on existing faces; the brush is the convex hull of the points.
- **Extrude / stamp from a face**, **Sweep**, **Duplicate**, **Paste**.

**Transforming**

- Move (drag, nudge by grid step, or by an exact vector), with axis lock.
- Duplicate in place, duplicate and move, linked duplicate.
- Rotate: 90° roll/pitch/yaw shortcuts, or the Rotate tool with a custom center, angle and axis. Entity angle properties update automatically (optional).
- Flip horizontally / vertically.
- Scale: by typed factors or by dragging the bounding box.
- Shear: by dragging a side of the bounding box.
- Repeat: replay recent move/duplicate/rotate steps (e.g. spiral stairs).

**Shaping**

- **Extrude tool:** move a face along its normal; create a new brush from a face; stamp; move a face freely.
- **Sweep tool:** fill space between selected faces and a movable destination cap with a series of brushes. Parameters: segments, path (arc, straight, S-bend), iterations, scale, rotation.
- **Clip tool:** 2–3 points define a plane (or pick an existing face's plane); keep front, back or both.
- **Vertex / Edge / Face tools:** select handles (click, lasso), move them, merge vertices, add vertices on the grid, delete vertices/edges/faces. Shared vertices of several brushes move together (terrain).
- **Snap vertices** to the grid or to integer coordinates.

**CSG:** Convex merge, Subtract (from all other visible brushes), Intersect, Hollow (walls one grid step thick).

**Grid:** sizes 0.125 to 256; show and snap toggles.

**Locks:** Texture lock (keep UVs during object transforms, on by default); UV lock (keep UVs during vertex/face editing, off by default).

**Quake 3 patches:** convert faces to Bezier patches; edit control points; change rows/columns.

## 4. Entities, NPCs and models

- **Point entities** (monsters/NPCs, items, weapons, lights, player starts) and **brush entities** (doors, triggers, platforms). Brushes not in an entity belong to the world. There is no separate NPC concept: a monster is a point entity.
- **Create point entity:** from the context menu (grouped by classname prefix), by dragging from the Entity Browser, or by a per-class shortcut. Placement: on the surface under the cursor in 3D, grid-snapped.
- **Create brush entity:** turn selected brushes into e.g. `func_door`.
- **Move brushes** into another brush entity, or back to the world ("Make structural").
- **Select all entities of a class.**
- **Definition files:** FGD, DEF and ENT. They provide classnames, descriptions, colors, bounding box sizes, model specifications, and typed properties: string, integer, float, choices, flags (spawnflags), color, origin, link source/target, inputs/outputs. Built-in definitions ship for every supported game; an external file can be chosen per map.
- **Model specifications** can depend on entity properties (a different model, skin or frame based on spawnflags or other keys).
- **Model formats:** Quake MDL, MD2, MD3, BSP models, SPR sprites, MDX, DKM, FM, ASE, image sprites, and via Assimp: OBJ, FBX, glTF/GLB, DAE, 3DS, BLEND, MD5, IQM, SMD, and more. Mods can override base-game models.
- **Property editor:** add, remove, rename, edit properties; autocomplete; descriptions from the definition; default values shown and can be applied ("set missing / existing / all defaults"); multi-selection editing.
- **Smart editors:** spawnflag checkboxes, choice dropdowns, color picker (float or byte), WAD list editor.
- **Entity links:** `target` / `killtarget` → `targetname`, or whatever keys the definitions mark as link source and target. Drawn as lines; four display modes.
- **Entity Browser:** model previews; sort by name or usage; group by prefix; show only used; text search.
- **Rotation rules:** `angle`, `angles`, `mangle` are updated when entities rotate.

## 5. Materials and face attributes

- **Formats:** Quake/Hexen 2 WAD2, Half-Life WAD3, Quake 2/Daikatana WAL, Heretic 2 M8, SoF M32, DDS, generic images (TGA, PNG, JPG, PCX, BMP), Quake 3 shaders. Palettes per game.
- **Collections:** WAD files listed in the map, or folders under the game's textures root (enabled per map). Reload on demand.
- **Material browser:** sort by name or usage, group by collection, show only used, name filter, "select faces / brushes with this material", copy name. **Gap:** no favorites.
- **Applying:** to selected faces or all faces of selected brushes; transfer material and alignment from one face to another; copy and paste face attributes.
- **Replace Material:** find one material and replace it with another in the selection or in the whole map.
- **UV alignment:** offset, scale, rotation; justify to face edges; align to an edge; fit N times; auto-fit; reset; reset to world-aligned; flip U/V; rotate ±90°; nudge in coarse or fine steps. A UV editor for one face. Valve 220 maps also support UV shear and exact alignment lock.
- **Face attributes** (game-dependent): surface flags, content flags, surface value (Quake 2, Quake 3 family), color (Daikatana).
- **Smart tags:** Trigger, Clip, Skip, Hint, Detail, Liquid, Caulk, Areaportal, etc., depending on the game. Each can be shown/hidden, and applied or removed with one command.

## 6. Map organization

- **Layers:** create, rename, remove (objects move to the default layer), reorder, set current, hide, isolate, lock, omit from export, move selection to a layer, select everything in a layer.
- **Groups:** group, ungroup, rename, merge, add/remove objects, open/close for editing, nesting.
- **Linked groups:** create linked duplicates; edits to one copy update all copies (each with its own transform); select linked, separate, extract; protected properties that differ between copies.
- **Hide / Isolate / Show all.** Locking is per layer only.

## 7. Selection

- Object mode and face mode.
- Select all, deselect, invert, siblings, touching, inside, tall, by line number in the map file, linked groups, all of a classname, all faces or brushes with a material.
- **Gap:** no map outliner or search over map objects.

## 8. Validation (Issue Browser)

20 validators, most with quick fixes:

| Issue | Quick fix |
|---|---|
| Missing entity classname | Delete |
| Missing entity definition | Delete |
| Missing mod directory | Remove mod |
| Empty group | Delete |
| Empty brush entity | Delete |
| Point entity with brushes | Move brushes to world |
| Missing link source / target | Delete property |
| Non-integer vertices | Snap vertices |
| Mixed brush content flags | — |
| Objects outside world bounds / soft map bounds | Delete |
| Empty property name / value | Delete property |
| Property key or value too long | Delete, or truncate value |
| Quotes in property key or value | Delete, or replace `"` with `'` |
| Backslashes in paths | Replace with `/` |
| Invalid UV scale | Reset UV scale |

Issues can be filtered by type, hidden, and selected (which selects the object).

## 9. History and clipboard

- Unlimited Undo/Redo with named steps; selection and visibility changes are undoable.
- Transactions group several edits into one step.
- Repeat last commands; clear the repeat list.
- Copy/Cut/Paste as plain map text; Paste at mouse position or at original position. Copied faces paste their attributes onto selected faces.

## 10. Compile and run

- **Compile profiles:** a working directory and a list of tasks, each can be turned on/off: Export map (with options to strip entities, add an entity at the camera, strip editor-only properties), Run tool, Launch engine, Copy/Rename/Delete files. Variables for paths, map name, mods, CPU count and tool paths.
- **Compile dialog:** Compile, Stop, Test (dry run); output log.
- **Launch engine:** engine profile, parameters per profile with variables.
- **Point files** (leak path, shown as a line; camera steps along it) and **portal files** (shown as translucent polygons).

## 11. Views, camera and rendering

- **Layouts:** 1, 2, 3 or 4 panes; 3D perspective view and 2D XY/XZ/YZ views.
- **Camera:** fly, orbit, pan, zoom; focus on selection; move camera to a position; step through point-file points.
- **Render options:** textured / flat / hidden faces, shading, fog, edges, soft bounds, entity classnames, bounds, models, entity link mode, per-tag and per-class visibility.
- **Gap:** no screenshot or render-to-image; no saved camera bookmarks.
- **Gap:** no map statistics dialog (only the status bar selection summary).

## 12. Preferences

Six panes: Games (paths, engines, tools), View (theme, layout, brightness, FOV, filtering, MSAA), Colors (~50), Mouse, Keyboard (every action rebindable), Update.

## 13. Automation today

- **Gap:** no IPC, scripting, plugins or network API. The only command-line options are map files to open, `--portable` and a draft-updates flag.
- The editing core is independent of the UI and already works without a window in tests, which makes a headless mode feasible.
- All edits go through one command/transaction system with notifications, which makes change reporting and Undo integration feasible.
- A registry of about 180 named actions (menus and shortcuts) plus per-entity-class and per-tag actions exists and can be enumerated.
