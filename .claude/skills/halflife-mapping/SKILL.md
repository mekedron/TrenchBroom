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
  for Xen, `liquids.wad` for more water.

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
| Looping music | `ambient_generic`, `message sound/<dir>/<file>.wav` relative to `sound/`, spawnflag 8 (large radius). The WAV must have a cue point — make it with `scripts/makeloop.py` |
| Lightning | `env_beam` with `LightningStart` (info_target, outside solids), spawnflags 1+4+32 (start on, random strike, end sparks), `Radius`, `texture sprites/lgtning.spr` |
| Glow, steam | `env_sprite` `sprites/glow01.spr`, `glow02.spr`, `steam1.spr`, `rendermode 5`, spawnflag 1 |
| Pulsing lights | `light` with `style` (2 slow pulse, 5 gentle pulse, 9 slow strobe, 11 slow pulse no black). A face takes at most 4 styles; reuse styles near each other |
| Model prop | `cycler` with `model models/<x>.mdl` (e.g. `crystal.mdl`) |
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
  (`yes`, `no`, `wave`, `checktie`, `franticbutton`, `buysoda`; Barney `barn_wave`),
  spawnflags 4+32+64 (repeatable, no interruptions, override AI); a `multi_manager` with
  spawnflag 1 (multithreaded) that fires the moves with delays and itself after 4–5 s; a
  `trigger_auto` starts it. Moves fired while the previous one still plays are skipped
  (`can't play` in the log).
- **Busy NPCs must not follow the player**: spawnflag 256 (`Pre-Disaster`) on scientists and
  guards — they decline to follow and keep their script. Seated scientists have it
  implicitly. Leave one guard without it if the player should get a follower.
- `ERROR: Monster ... stuck in wall` in the log: the monster's hull (32×32×72) intersects
  solids at spawn; check with `space_check` on the hull box one unit above the floor.
- Speech: `scripted_sentence` (`sentence !SC_...`, `entity` = monster name, `radius`).

## Compile and test

- The compiled BSP is copied to `<game>/valve/maps`.
- Launch: `steam -applaunch 70 -condebug -dev +developer 2 +map <map>`. The console log is
  written to `<game>/qconsole.log` (the game root, not `valve/`). Check for
  `script "..." using monster "..."` (scripts grabbed their NPCs), `stuck in wall`,
  `Host_Error`, missing models and sounds.
- Stop the game with `kill $(pgrep -x hl_linux)`.

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
