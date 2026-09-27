---
name: trenchbroom-mapping
description: Build and edit maps for any game TrenchBroom supports (Half-Life, Quake, Quake 2, Quake 3 and others) through the TrenchBroom MCP server (mcp__trenchbroom__* tools), check them with view snapshots, compile them and test them in the game. Use when creating rooms, choosing and aligning textures, placing NPCs, items and scripted scenes, adding doors, glass, water, lights or sound, importing pieces of other maps, fixing z-fighting or leaks, and compiling and launching a map. Game-specific conventions and recipes are in the game skills (e.g. halflife-mapping).
---

# Mapping with the TrenchBroom MCP server

A working method for building maps that look and play right the first time, for any game
TrenchBroom supports. The MCP guide (`trenchbroom://guide`) covers the tools; this skill
covers what the tools do not tell you: conventions, recipes that work in the engine, and
the traps.

**Game skills.** Also use the skill of the game you are mapping; it adds units, textures,
entity recipes and the compile and launch commands of that game:

| Game | Skill |
|---|---|
| Half-Life (GoldSrc) | `halflife-mapping` |

For a game without its own skill, use this one and learn the game's conventions from its
original maps (see *Textures* and *Porting pieces of other maps*).

**Untested tools.** Tools marked *(untested)* are implemented and covered by automated tests
but have not been used on a real map yet. Use them, check the result with a snapshot, and
report anything that behaves unexpectedly.

Generic helper scripts live in `scripts/` next to this file (Python 3, numpy); game-specific
ones are in the game skills.

## Workflow

1. `editor_status`, then `compile_tools_get {"game":"<game>"}` — the tool paths must be `ok`.
2. `document_new {"game":"<game>","format":"<format>"}` (prefer a Valve 220 format where the
   game has one) and **`document_save_as` at once**: compiling needs a saved file, and the
   editor can restart and lose unsaved work.
3. `materials_collections_set` with the game's texture collections (see the game skill).
4. Build in small steps and **save after every step**. Prefer plain calls over long
   `transaction_begin` spans: if the MCP session reconnects, the open transaction stays
   bound to the old session.
5. After each area: **look at it** (see *Seeing your work*), check z-fighting, compile with the
   `normal` preset, launch the game and read its console log (see *Compile and test*).
6. A document reload (`document_revert`, `document_open`) or an editor restart invalidates
   all object ids — query them again (`objects_find` by region, material or classname).

## Keeping the map navigable

- Put every object made of several brushes into a named group (`table_1`, `cash_desk`,
  `dj_console`) with `group_create`, and every room into its own layer. Then objects are found
  by name, and rooms or objects can be hidden or isolated for snapshots.
- Repeated objects (all tables, all chairs) as linked groups: one edit updates all copies.
- Name a camera per room with `agent_camera_set` and reuse it after every change.
- Keep a short note of the rooms, their purpose and key points (spawn, doors, triggers) and
  update it as the map grows.

## Seeing your work

Check every room with images instead of reasoning about coordinates; most visual mistakes
(wrong facing, stretched textures, NPCs inside furniture, props floating or sunk) are obvious
in one picture.

- Player view: `view_snapshot {"camera":{"eyeHeight":{"point":[x,y,z]},"lookAt":[..]}}` — the
  camera stands on the floor below the point at the player's eye height of the game.
- Name a camera per room with `agent_camera_set` and reuse it; the user's view never moves.
- Hide editor helpers for a game-like picture with `"options":{"hideTags":[..],"hideClassnames":[..]}`
  (tool textures such as clip and trigger; lights, scripts, info and trigger entities).
- A prop from several sides: `view_snapshots_around {"box":{..},"views":["southeast","above"]}`.
- What a change did: `view_snapshot_compare {"undoSteps":1,"camera":"<name>"}`, or
  `view_snapshot {"keepAs":"before"}` … change … `view_snapshot_compare {"before":"before"}`;
  the changed-pixel mask shows exactly what moved.
- What the user is looking at: `view_snapshot_user` (returns their camera too).
- Showing the user something: `camera_focus` *(untested)* moves the **user's** camera to objects;
  `camera_set` *(untested)* places it. Use this only when you want the user to look; for your own
  checks use agent cameras and snapshots.
- From picture to object: every snapshot has a `snapshotId`; `view_pick {"snapshotId":..,"pixels":[[x,y],..]}`
  *(untested)* returns the object, face id, group, layer, hit point and normal under each pixel —
  no need to search by region.
- Annotated snapshots *(untested)*: `"annotations"` with labels (id, classname or group name,
  size), a coordinate grid on floors and walls, a compass, and a player box for scale.
- The editor preview differs from the game: models show their default pose, not a scripted
  one, and render modes such as transparency are ignored. Judge poses and transparency in
  the game.
- `console_read {"minLevel":"warning"}` shows material, model and definition load problems;
  every call result also lists the warnings it caused under `console`.

## Understanding the space

Think in rooms, not brushes:

- `spaces_list` *(untested)* — enclosed rooms with stable `space:` ids, bounds, floor and ceiling
  heights, area, openings (doorways, windows, doors) and neighbours, and whether each is sealed.
- `surroundings {"point":[..]}` *(untested)* — the room of a point, distances to the walls (with
  their faces), floor, ceiling and nearby objects, as data and a sentence. Use it to look around
  without rendering.
- `free_spots` *(untested)* — free positions for a box of a given size on the floor, against a
  wall (with the wall normal and face id — for posters, machines, lights) or on the ceiling.
- `walkable_plan` *(untested)* — where a player can really walk and reach, as text and image.
- Keep the map's structure in its manifest *(untested)*: `map_manifest_set` stores spaces and
  their purpose, key points, notes and cameras in `<map>.mcp.json` next to the map;
  `map_manifest_get` restores them in a later session.
- Every modifying call reports the problems it introduced in `issuesIntroduced` *(untested)*:
  `Z_FIGHTING`, `ENTITY_OUTSIDE_HULL` (with the nearest gap), model placement and
  `UV_ASPECT_DISTORTION`. Fix them right away; `issues_list` *(untested)* shows all of them plus
  the editor's own checks.

## Units and proportions

Take the player size, eye height, step height and jump height from the game skill, and
derive the rest from them:

- Doorways at least player width × 1.3 player height; corridors at least 3 player widths.
- Rooms: about 2 player heights for small rooms, 3+ for halls. Walls 8–16 thick.
- Furniture scaled to the player: seat top ≈ 1/4 player height, table top ≈ 0.38, counter ≈ 0.58.
- `brush_create_box` faces are ordered `0:-x 1:-y 2:-z 3:+z 4:+y 5:+x`; verify with
  `object_get {"fields":["faces.id","faces.normal"]}` for shapes, clips and reloaded maps.

## Textures

### How the original maps use them

Designers of classic engines almost always use **scale 1** and size the geometry to the
texture; smaller scales appear on small detail panels and screens. Before texturing, learn
how the game's original maps use a texture: `material_usage` *(untested)* (typical scale, face size, panel
or tile; after `material_corpus_scan` *(untested)* on a folder of the game's map sources), or the game's
statistics script if the game skill has one.

Consequences:
- Make walls as tall as the texture, or an exact multiple.
- A panel texture (one panel per wall height) must not repeat; fit it once to the wall height
  and keep U and V scale close to each other.
- Never distort aspect: pick the brush size from the texture size, or pick a texture whose
  aspect matches the face (`material_fit_geometry` *(untested)* gives the size for a panel).
- Give every face its own role: a front-panel texture only on the front face, neutral
  material on the sides and top. Never leave a picture texture on all six faces of a box.
- Run `uv_check` *(untested)* on each finished area.

### Choosing

Look before choosing: `material_preview`, or a contact sheet from the game's scripts. Names
lie — judge by the picture. Keep the choices that work in the game skill.

### Aligning

- Single face, exact fit: `uv_align {"operation":"fit","repeatU":1,"repeatV":1}`. For several
  repeats choose counts that keep `scaleU ≈ scaleV`, or set one axis with `keepAspect` *(untested)*
  (optionally `round` to whole repeats). `uv_align {"operation":"typical"}` *(untested)* applies
  the material's typical scale and aligns it to the face.
- Material and UV tools return `UV_*` warnings *(untested)* for stretched textures and fractional
  panel repeats; fix them before moving on.
- A wall made of several brushes (cut by doorways): `uv_align resetToWorld`, then
  `face_attributes_set` with one scale and offset for all of them, so the pattern continues.
  Offset for a panel to start at a corner: `offset = -(corner / scale) mod textureSize` along
  the U axis; `v = -z/scaleV + offsetV`, so offset 0 starts panels at z = 0.
- Orientation: the viewer's right on a face is `viewDirection × up`. If `uAxis` points the
  other way the picture is mirrored; if `vAxis` points away from the viewer's "down" it is
  upside down. Both wrong → `face_attributes_set {"rotateBy":180}` and fit again.
- Light fixtures: the housing in a neutral material, the glowing texture only on the bottom
  face, fitted 1×1, with the fixture sized to the texture.

## Geometry

### Rooms and openings

- `room_create` builds sealed rooms. Adjacent rooms share a wall plane: create the new room
  so that its outer faces **touch** the existing walls instead of overlapping them, or delete
  the duplicate wall.
- Doorways: `opening_cut` with explicit `ids` of **every** wall brush in the way (both rooms'
  walls and trim strips), reveal `material` = trim texture.
- A floor under a doorway must belong to one room only; trim the other floor so their top
  faces do not overlap.
- A sunken pool: a second `room_create` below the floor, delete its ceiling, `opening_cut` the
  floor above it, fill with the game's water brush entity or liquid texture.

### Z-fighting (flicker and wrong textures in game)

Two visible faces of different brushes in the same plane, facing the same way, overlapping.
The compiler then picks one arbitrarily (a ceiling texture on a wall, a table leg on a table
top). Typical causes: overlapping walls of adjacent rooms, a room's ceiling edge reaching into
another room's wall, a post or leg ending exactly at the top of the slab it supports, two
crossing bars of equal height.

Rules: end supports 2 units below the surface they carry; make crossing parts different in
height and width; let edges end at a touching wall instead of inside it.

Check after every area: `scripts/zfight.py MAP` (lists only faces not hidden by a touching
face). Fix by moving faces: `vertices_move {"ids":[..],"faces":[[...all vertices...]],"vector":[...]}`.

### Shapes

- Round things: `brush_create_shape` with `circleMode "scalable"` (integer vertices). Other
  sizes or modes give non-integer vertices → `vertices_snap {"mode":"integer"}`.
- Stairs: `shape "stairs"`, `stairDirection` = direction in which they rise; step height from
  the game skill.
- Sloped surfaces (consoles, keyboards, loungers): `brush_create_hull` from 8 points.
- Arched doorway: `shape "arch"`, `axis` = passage direction, `spandrel true`. The bounds are
  the **outer** radius: inner radius = outer − `thickness`. Make the bounds wider than the
  opening by the thickness, then `brush_clip` the arch to the opening width so its faces do
  not overlap the wall. Round window: rectangular `opening_cut`, top arch, `objects_duplicate`,
  `objects_flip {"axis":"z","center":[.., .., middle]}`.
- Chamfers: `brush_clip` with 2 points and axis z; the plane normal is `(dy, -dx)` of `b - a`,
  `keep` = `back` keeps the side opposite the normal.
- Avoid `csg_subtract` with round cutters (dozens of overlapping fragments) and `csg_hollow` on
  polygonal brushes (messy walls); use shapes with `hollow` or the arch method.
- Transform tools (`objects_move`, `objects_rotate`, `objects_flip`, ...) take brushes, point
  entities, brush entities (their brushes move with them) and groups. Rotating updates the
  angle properties of point and brush entities.
- Small detail (tiles, chairs, arches) in the world slows down visibility compilation; put it
  into the game's non-structural brush entity (see the game skill).

## Entities

- Read a class before using it: `entity_class_describe` lists its properties, choices,
  spawnflags and model.
- Every point entity must be inside the sealed hull; one outside causes a leak (entities
  ported from other maps are the usual suspects).
- Links: `entity_link` sets `target` → `targetname` with a unique name; `entity_links_get`
  finds broken links.
- `null` as a property value in `entity_create_*` and `entity_properties_set` removes the key,
  including defaults the editor adds.
- Place NPCs and props against their real model bounds: `entity_model_info` *(untested)* lists
  animations with their bounds and the property that selects them; `entity_animation_set`
  *(untested)* switches the animation. A pose that reaches below the origin needs the origin that
  much above the surface. `entity_create_point {"dropToFloor":true,"dropUsing":"model"}`
  *(untested)* drops by the model bounds, and `entity_placement_check` *(untested)* reports models
  in brushes or floating (`MODEL_*` warnings also come from creating and moving entities). Seated
  poses always touch their seat — judge them with a snapshot. Check the NPC's hull with
  `space_check` one unit above the floor.
- Items and props: `dropToFloor` onto floors and furniture.

## Anything else the editor can do

- `actions_list` *(untested)* lists every editor action (menus, map view shortcuts, tag and
  entity actions) with its state; `action_invoke` *(untested)* runs one by path. Actions that need
  a dialog name the tool to use instead.
- `preferences_get` / `preferences_set` *(untested)* read and change any preference.
- `manual_search` / `manual_section` *(untested)* answer "how does TrenchBroom do X" from the
  user manual.

## Checking and fixing the map

- `map_check` *(untested)* runs the agent checks: entities in walls or floating, a missing
  player start, broken links, missing materials, entities outside rooms — each finding with a
  suggested fix.
- `issue_fix` *(untested)* applies quick fixes by issue id, code or object (one undo step);
  `issue_hide` / `issue_show` *(untested)* hide accepted issues; `validators_set` *(untested)*
  turns checks off for a document.
- Run `map_check` before every compile.

## Compile and test

- `compile_run {"preset":"normal"}` then `compile_status`; the compiled map is copied into the
  game's `maps` folder. The compile log is `<map dir>/compile/<map>.log`.
- Leak: `pointfile_load` → the path starts at the entity that is outside; move it inside and
  compile again; `pointfile_unload` afterwards.
- Launch the game with the map and its console logging on (command in the game skill, or
  `engine_launch` *(untested)* with a configured engine profile, which returns the process id),
  then read the log for errors, missing assets and stuck NPCs.
- Stop the game by its exact process name (`kill $(pgrep -x <name>)`) — never `pkill -f`, it
  matches the shell running the command. Ask before restarting the game while the user is
  playing.
- Wait for compiles and the game with an `until grep -q ...; do sleep 2; done` loop.

## Porting pieces of other maps

Original map sources (or decompiled maps) are good references for props and scripted gags.
`map_file_inspect {"path":..}` lists their layers, groups, classnames and materials;
`map_import {"path":..,"region":{..},"regionMode":"inside","position":[..]}` copies a box of
brushes and entities into the document and converts the map format (try `dryRun` first).
Entities that drive the gag often sit outside the prop's box (relays, sounds, speech): find
them by their `targetname` links and import them with a second region or `classname` filter.
Rotate the result with `objects_rotate` if it faces the wrong way; move helper entities that
end up outside the hull.

Never commit a game's assets (textures, models, sprites, sounds, decompiled maps) to a
repository. Your own map sources and compiled maps without embedded textures are fine.

## Scripts

| Script | Purpose |
|---|---|
| `scripts/zfight.py MAP` | visible coplanar overlaps (z-fighting) in a .map file |
| `scripts/mapio.py` | minimal .map parser used by the other scripts |

Game-specific scripts are listed in the game skills.
