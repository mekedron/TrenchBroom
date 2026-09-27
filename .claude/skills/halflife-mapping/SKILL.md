---
name: halflife-mapping
description: Half-Life (GoldSrc) specifics for mapping through the TrenchBroom MCP server — units, WADs, proven textures and how Valve used them, texture lights, entity recipes (doors, glass, water, ladders, music, lightning), NPC poses and scripted scenes, sdHLT/VHLT compiling and launching the map in Half-Life. Use together with the trenchbroom-mapping skill whenever the map is for Half-Life.
---

# Half-Life mapping

Half-Life specifics for the TrenchBroom MCP server. The general method — workflow, snapshots,
texturing principles, geometry, z-fighting, porting, compiling — is in the
**trenchbroom-mapping** skill; read it first. This skill adds what is true only for GoldSrc.

Helper scripts live in `scripts/` next to this file (Python 3, numpy, Pillow). The generic
ones (`zfight.py`, `mapio.py`) are in the trenchbroom-mapping skill.

## Setup

- Game `Half-Life`, format `Valve`: `document_new {"game":"Half-Life","format":"Valve"}`.
- `compile_tools_get {"game":"Half-Life"}` — `csg`, `bsp`, `vis`, `rad` must be `ok` (sdHLT or VHLT).
- `materials_collections_set` with the WADs from `<game>/valve/`: `halflife.wad`; add `xeno.wad`
  for Xen, `liquids.wad` for more water. The server stores their paths so compiling finds them.
- The editor's "unused targetname" issues are false positives for Half-Life logic that is fired by
  `multi_manager` keys or `m_iszEntity`; hide them with `issue_hide` instead of deleting names.
- Sky: worldspawn `skyname` picks the skybox from `valve/gfx/env` (`night`, `city`, `black`, ...);
  without it the game shows the default desert sky.
- `func_detail` (sdHLT) is not in the built-in FGD: the editor reports it as a missing
  definition, the compiler handles it. Turn that validator off with `validators_set`.

## Assets from other games

The map runs in `valve`, which only sees files inside `valve/`. Textures, models and sprites of
other installed games (Counter-Strike, Condition Zero, Day of Defeat, TFC, Opposing Force, Blue
Shift, Quake 1/2) work once a script copies them there; the repository keeps only the script.

- Textures: combine the chosen ones into one WAD3 in `valve/` (8-bit, 256-colour palette per
  texture, 4 mips, sizes multiple of 16, names ≤ 15 chars with the special prefix first:
  `{`, `~`, `+0`). Quake and Quake 2 textures convert through their game palette. Original
  art (signs, posters, animated screens `+0NAME` … `+9NAME`, ~10 fps) goes the same way. List
  the WADs with `materials_collections_set` and add their `~` textures to an `info_texlights`.
- Models: copy the `.mdl` with its `<name>t.mdl` texture file and its `<name>01.mdl`… sequence
  files, and rewrite the sequence-group paths inside the model to the new folder; otherwise a
  copied model loads a same-named Valve file (Blue Shift's `scientist01.mdl`).
- A BSP compiled against such WADs has no embedded textures and can be committed.

## Units

- Player: 32×32×72, eye at 64 (`eyeHeight` snapshots use 64). Step height 18.
- Doorway 64×96 or wider, corridor ≥ 96.
- Rooms: height 128–160 for offices and cafés, 224+ for halls. Wall thickness 16.
- Chairs: seat top 16–18. Tables: top 26–28, thin top (2 units) so seated arms rest on it.
  Counter: 40–44. Monitors: 36–48 tall.

## Textures

### How Valve used them

Valve almost always used **scale 1** and sized the geometry to the texture; scale 0.5 appears
on small detail panels (`LAB1_GAD2`, `+0~C2A4_CMP1`) and 0.25–0.35 on screens and chargers.
Run `scripts/texstats.py <game>/valve/maps usage.json --show NAME` to see how the original
maps use any texture (median scale, typical face size, number of maps). Make walls as tall as
the texture (128 for `C1A1_W1`); a panel texture such as `METAL_WALL04` must not repeat
vertically; size a picture texture's face from its texture (keyboard `FIFTIES_KEY` 80×32 →
face 40×16).

The TrenchBroom knowledge folder already holds notes for the textures used in the maps so
far (`material_notes_get`): panels such as `GEN_VEND1`, doors, screens, signs and chargers
with their face sizes. Add every texture you learn with `material_notes_set {"scope":"game"}`;
`material_corpus_scan` on decompiled maps adds scales; it merges split faces and warns
(`DECOMPILED_INPUT`), but walls still mostly come out as tiles, so notes stay the best source.

### Choosing

`scripts/texsheet.py list WAD --grep KEY` and
`scripts/texsheet.py sheet out.png WAD... --names A B C`, then read the PNG; or
`material_preview`. Names lie (`METAL_BORD01` looks icy, it is freezer metal).

Proven choices (`halflife.wad` unless noted):

| Purpose | Texture |
|---|---|
| Machine/cabinet metal (most used in HL) | `LAB1_CAB2` 48×48 |
| Chrome legs, posts, rails | `GENERIC028` |
| Dark metal trim | `DRKMTLT_BORD11` |
| Office/lab wall panels | `C1A1_W1` 128×128 |
| Lab corridor wall | `C1A0_W1`, floor `C1A0_LABFLR`/`C1A0_LABFLRE` |
| Fifties café | floor `FIFTIES_FLR01`, walls `FIFTIES_WALL03`, wainscot `FIFTIES_WALL06`, ceiling `FIFTIES_CEIL01`, wood `FIFTIES_DSK1`, table top `FIFTIES_TBL1` |
| Riveted industrial wall | `METAL_WALL04` 96×128 (panel) |
| Pool | deck `FIFTIES_FLR5`, walls `FIFTIES_W13`, basin `C2A4_FLR3`, water `!WATERBLUE` |
| Sauna | planks `TRRM_WOOD`, benches `TRRM_WOOD2`, stones `CRETE3_FLR04` |
| Xen (`xeno.wad`) | swamp floor `C4A1A_SWMPFLR`, crystals `CRYS_*`, alien tech `TECH_PAN*` |
| Screens | `+0~LAB_CRT7`, `~LAB_CRT10A`, `+0FLICKERMON`, `LAB1_RADSCRN2`, `C2A2_SATORB` |
| Consoles | `+0~C2A4_CMP1`, `+0~LAB1_CMP1`, `LAB1_GAD2`, `LAB1_COMP10A` |
| Doors | `C1A0B_DR3A`/`DR3B` (double leaves), `C1A3SECDR02` (SECURITY), `LAB1_DOOR2A` (blast door) |
| Chargers | `+0MEDKIT` 64×96, `+0RECHARGE` 128×192 |
| Props | `GEN_VEND1` vending machine, `CLOCK1`, `FIFTIES_KEY` keyboard, `C1A1_LOCKER` |
| Signs/posters | `C1A0_WDSIGN`, `C1A3_SIGN2`, `C3A2SIGN00` (lambda), `POSTER12/13/15/16` |
| Lights | `~LIGHT3B` (white), `~LIGHT3C` (warm), `~SPOTBLUE/RED/GREEN` |
| Glass | `GLASSGREEN`, `GLASSBLUE1/2`, `GLASS_BRIGHT` |
| Ladder, grates | `{LADDER1` (transparent, needs a `func_wall` with `rendermode 4`) |

Light fixtures: the housing in `LAB1_CAB2`, `~LIGHT3B` (64×16) only on the bottom face, fixture
64×16×4.

### Texture lights

`lights.rad` of the compile tools makes `~LIGHT3B`, `~LIGHT3C` and other light textures emit
light. For others (e.g. the `~SPOT*` dance-floor tiles) add an `info_texlights` point entity
with keys `"~SPOTBLUE" "20 90 255 120"`. The game logs `Can't init info_texlights` — harmless.

### How dark is too dark

A club, a basement or a maze lit only by coloured spots reads as pitch black on a normal
monitor. Give every walkable area a dim fill (a `light` of 70–120 every ~250 units in a maze)
and a floor for the whole map with RAD's `-ambient r g b` (0.07–0.1 keeps the mood; 0.14 already
washes it out). Pass it through a compile profile (`compile_profile_save`, rad parameters
`-ambient 0.09 0.08 0.10 "<map>"`). A BSP whose RAD step was stopped has no lightmaps and renders
fully bright — never judge lighting from it. Models are lit by the lightmap under their origin:
an NPC on an unlit patch is a black silhouette.

## Preview versus game

The editor shows models in their default pose, not the scripted one (sunbathers and sitters
stand), and ignores `rendermode` (glass is opaque, invisible `AAATRIGGER` buttons show).
Hide helpers in snapshots with
`"options":{"hideTags":["clip","trigger"],"hideClassnames":["light*","scripted_*","info_*","multi*","ambient_*","infodecal","trigger_*"]}`.

## Entities and gameplay recipes

Angles: monsters and doors read `"angles" "pitch yaw roll"`; `entity_create_point {"angle":..}`
writes `angle`, which is fine for most classes. Up for doors: `"angles" "-90 0 0"`. Small
detail goes into `func_wall`.

| Goal | Recipe |
|---|---|
| Double sliding door | two `func_door` leaves with the same `targetname`, `angles` 0 and 180, `lip 4`, plus a `trigger_multiple` around the doorway targeting them (doors with a name do not open on touch) |
| Hinged door | `func_door_rotating` from the door brush **and an ORIGIN brush at the hinge**, `distance 90` |
| Locked door with feedback | `func_door` with `master` = a `multisource` that has an input that never fires (a `trigger_relay` targeting it); `locked_sound 2`, `locked_sentence` (3 = blast door). A multisource without inputs counts as triggered |
| Breakable glass | `func_breakable`, `material 0`, `health 10–30`, `rendermode 2`, `renderamt 70–110`, `"gibmodel": null` (the editor adds it empty otherwise); one entity per pane so panes break separately |
| Water | `func_water`, `skin -3`, `WaveHeight 2`, `rendermode 2`, `renderamt 170` |
| Ladder | invisible `func_ladder` (AAATRIGGER brush) + visible `{LADDER1` brush in a `func_wall` with `rendermode 4 renderamt 255` |
| Rotating object | `func_rotating`, spawnflag 1, with an ORIGIN brush at the rotation centre |
| Health / HEV chargers | `func_healthcharger` (`+0MEDKIT`), `func_recharge` (`+0RECHARGE`) |
| Looping music | `ambient_generic`, `message sound/<dir>/<file>.wav` relative to `sound/`, spawnflag 8 (large radius). The WAV must have a cue point — make it with `scripts/makeloop.py`; 16-bit mono 22050 Hz keeps it small |
| Music per room | one `ambient_generic` per zone with spawnflags 1+16 (everywhere, start silent), placed at the zone's source (DJ, stage, door of the loud room): the volume stays even, the stereo image points at the placement — an ambient far away plays "behind the player". A `trigger_multiple` (`wait 1`) filling each zone fires a `multi_manager` that fires `trigger_relay`s with `triggerstate 1` for its ambient and `0` for all others (ambient_generic honours on/off, so repeats are harmless) |
| Lightning | `env_beam` with `LightningStart` (info_target, outside solids), spawnflags 1+4+32 (start on, random strike, end sparks), `Radius`, `texture sprites/lgtning.spr` |
| Glow, steam | `env_sprite` `sprites/glow01.spr`, `glow02.spr`, `steam1.spr`, `rendermode 5`, spawnflag 1 |
| Pulsing lights | `light` with `style` (2 slow pulse, 5 gentle pulse, 9 slow strobe, 11 slow pulse no black). A face takes at most 4 styles; reuse styles near each other |
| Model prop (static) | `env_sprite` with `model models/<x>.mdl`, `framerate 0`, `"angles" "0 <yaw> 360"` (with roll 0 the sprite code copies the yaw into roll and tips the model over): non-solid, no AI. Check each model's forward axis in a test — some props face +Y |
| Model prop (animated) | `cycler` with `model models/<x>.mdl`: solid 32×32×72 box, not dropped to the floor; sequence 0 animates on its own, any other `sequence` only after the cycler is triggered once (fire all cyclers from a `trigger_auto` → `multi_manager`, at most 16 targets per manager, chain them) |
| Xen life | `xen_plantlight`, `xen_hair`, `xen_spore_small/medium/large`; `xen_tree` is 188 tall |
| Items | `weapon_9mmhandgun`, `ammo_9mmclip`, `item_battery`, `item_suit` — `dropToFloor` onto furniture |

## NPCs

- Check an animation's bounding box before placing: `scripts/mdlseq.py <game>/valve/models/scientist.mdl --grep sit`
  (or `entity_model_info`). Scientists: `sitting2/3`, `sitlookleft` reach 35 below the origin
  (the origin is at the chest, the pelvis ~13 below it); `sitidle` sits on the floor with legs
  22 forward; `lying_on_back` lies along the facing direction, −34…+37. Barney:
  `lying_on_back` −33…+46; `sit1` is offset 57 units and unusable for free placement.
- **Seated scientist**: `monster_sitting_scientist` drops to the first solid surface below.
  Put a `CLIP` brush on the seat from the seat top to seat top + 17 and the origin at the clip
  top. Works in the editor preview and in the game. The chair must be solid (world or
  `func_wall`).
- **Holding a pose forever**: a `scripted_sequence` with `m_iszEntity` = the monster's
  `targetname`, `m_iszIdle` = a **looping** sequence (`console`, `coffee`, `crouch_idle`,
  `sitidle`, `lying_on_back`, `relaxstand`, `standing_idle`), `m_fMoveTo 0`, placed at the
  monster, and a `targetname` that nothing ever fires.
- **Repeating actions (dancing, buying soda)**: `scripted_sequence` per move with `m_iszPlay`
  (`yes`, `no`, `wave`, `franticbutton`, `buysoda`; Barney `barn_wave`),
  spawnflags 4+32+64 (repeatable, no interruptions, override AI); a `multi_manager` with
  spawnflag 1 (multithreaded) that fires the moves with delays and itself after 4–5 s; a
  `trigger_auto` starts it. Moves fired while the previous one still plays are skipped
  (`can't play` in the log). Never loop `checktie`: its model event says "why do we all have
  to wear these ridiculous ties" every time; the other moves above are silent.
- **Other models as NPCs**: `monster_generic` with `model` (any GoldSrc model, also from other
  mods copied into `valve/models/`) is friendly, drops to the floor and can be held in a
  looping pose by a scripted_sequence; its hull is 32×32×72 from the origin up. Keep poles and
  props out of that box or it spawns "stuck in wall" inside a yellow particle field.
- **Poses that read wrong**: player models (CS, CZ, DoD, TFC, `player/gina`) in a `cycler`
  render their sequences at blend 0 — bent over, aiming at the floor; use NPC models instead.
  The female assassin's `fly_attack` is a shooting pose standing on one leg; her dance-like loops
  are `fly_down`, `grenadethrow`, `idle1`, `idle3` (`body 1` hides the pistol). Opposing Force
  soldiers' `victorydance` looks like firing a rifle even unarmed — two of them facing each
  other stage a shoot-out; recruits' `jumping_jacks` and `pushups` are the safe (and funny) choice.
- **Pole dancer**: a non-solid pole (`func_illusionary`) and a `cycler` with the dancer's loop
  placed ~12 units from the pole axis; a `monster_generic` there would spawn stuck.
- **Busy NPCs must not follow the player**: spawnflag 256 (`Pre-Disaster`) on scientists and
  guards — they decline to follow and keep their script. Seated scientists have it
  implicitly. Leave one guard without it if the player should get a follower.
- `ERROR: Monster ... stuck in wall` in the log: the monster's hull (32×32×72) intersects
  solids at spawn; check with `space_check` on the hull box one unit above the floor.
- Speech: `scripted_sentence` (`sentence !SC_...`, `entity` = monster name, `radius`).

## Compile and test

- The compiled BSP is copied to `<game>/valve/maps`.
- Launch: `engine_launch` with the engine profile `Half-Life (Steam)` (path `/usr/bin/steam`,
  parameters `-applaunch 70 -condebug -dev +developer 2 +map ${MAP_BASE_NAME}`; create it with
  `engine_profile_save` if missing), or the same command from a shell. The console log is
  written to `<game>/qconsole.log` (the game root, not `valve/`). Check for
  `script "..." using monster "..."` (scripts grabbed their NPCs), `stuck in wall`,
  `Host_Error`, missing models and sounds.
- Stop the game with `kill $(pgrep -x hl_linux)`.
- **Entity budget**: the engine allocates 900 edicts by default; a detailed map with hundreds of
  props, NPCs and scripts fails with `ED_Alloc: no free edicts`. Unnamed `light`s are removed at
  spawn, everything else counts (each `env_beam` adds its beam at run time). Count the spawned
  entities before release; above ~800 either cut props or launch with `-num_edicts 2048` (the
  engine maximum) and say so in the map's notes.
- **Screenshots from the game** (for checks and for publishing): add `trigger_camera`s
  (`spawnflags 4` freezes the player, `wait` = hold time) with `info_target`s, fired one after
  another by a `multi_manager` from a `trigger_auto`; keep them in a layer that is omitted from
  export, or write them straight into the entity lump of a copy of the compiled BSP
  (`<map>_album.bsp`: parse the lump with a quote-aware tokenizer, append the new lump at the
  end of the file and update its header entry). A `maps/<map>_load.cfg` runs at load:
  `hud_draw 0; crosshair 0; r_drawviewmodel 0; developer 0; default_fov 110` and a loop
  `alias shotloop "w90;snapshot;shotloop"` (w90 = 90 `wait`s) writes `<map>NNNN.bmp` to `valve/`
  roughly every 1.5 s, independent of window focus. The game reuses the first free number, so
  move each BMP away under your own counter before the next one lands. Launch windowed at the
  wanted size (`-windowed -w 2560 -h 1440`), pick the sharpest frame per camera, and delete the
  load cfg afterwards — it also runs for players.

## Original maps worth porting from

Decompiled original maps are in the Standard format; `map_import` converts them to Valve 220.
Useful originals: the microwave in `c1a0d` (five stacked `func_button`s, `func_rotating` stew,
`func_breakable` gibs), `env_beverage` (a vending machine that drops a soda can). Helper
entities of gags: `multi_manager`, `env_render`, sounds, `scripted_sentence`.

Never commit Half-Life assets (WAD, MDL, SPR, WAV from the game, decompiled maps) to a
repository. Map sources, compiled BSPs without embedded textures (the default of the compile
tools) and original sounds are fine.

## Scripts

| Script | Purpose |
|---|---|
| `texstats.py MAPS_DIR OUT.json --show TEX` | how the original maps scale and size each texture (reads BSP v30) |
| `texsheet.py list/sheet` | texture names with sizes; labelled contact sheets from WAD3 files |
| `mdlseq.py MODEL.mdl --grep NAME` | animation sequences of a GoldSrc model with loop flag and bounding boxes |
| `makeloop.py SRC OUT --bpm .. --start-bar .. --bars ..` | seamless looped WAV with a cue point; `--analyze` shows bar energy to find phrases |
