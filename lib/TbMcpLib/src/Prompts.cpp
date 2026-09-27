/*
 Copyright (C) 2026 Nikita Rabykin

 This file is part of TrenchBroom.

 TrenchBroom is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 TrenchBroom is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with TrenchBroom. If not, see <http://www.gnu.org/licenses/>.
 */

#include "mcp/Prompts.h"

#include "mcp/PromptRegistry.h"

#include "kd/string_format.h"

#include <optional>
#include <string>

namespace tb::mcp
{
namespace
{

/*
 * Prompt texts write tool names in backticks and nothing else in backticks
 * (tst_Prompts checks that every backticked name is a registered tool).
 */

constexpr auto GuideHint =
  "If you have not read the agent guide (resource trenchbroom://guide) yet, read it "
  "first: conventions, the player dimensions of each game and common pitfalls.\n\n";

/** The value of an argument, or nothing if it is missing or blank. */
std::optional<std::string> optionalArgument(
  const PromptArguments& arguments, const std::string& name)
{
  if (const auto it = arguments.find(name); it != arguments.end())
  {
    if (auto value = kdl::str_trim(it->second); !value.empty())
    {
      return value;
    }
  }
  return std::nullopt;
}

std::string argumentOr(
  const PromptArguments& arguments, const std::string& name, const std::string& fallback)
{
  return optionalArgument(arguments, name).value_or(fallback);
}

std::string blockoutLevel(const PromptArguments& arguments)
{
  const auto description = argumentOr(arguments, "description", "");
  const auto game = optionalArgument(arguments, "game");
  const auto style =
    argumentOr(arguments, "style", "none given: keep the shapes simple and readable");

  return std::string{GuideHint} + "Block out a level in TrenchBroom. Layout wanted: "
         + description + "\nStyle: " + style + "\n\n"
         + "1. Setup: `editor_status`. "
         + (game ? "The level is for " + *game
                     + ". If the active document is not a map of this game, "
                   : std::string{"Use the active document's game; if no map is open, "
                                 "ask the user for the game, and "})
         + "create the map with `document_new` (use `game_list` for the names; prefer a "
           "Valve 220 format) and save it at once with `document_save_as`. Read "
           "`game_info` and set the texture collections with "
           "`materials_collections_get` / `materials_collections_set`.\n"
           "2. Scale: take the player box, eye height, step height and doorway size of "
           "this game from the guide's table. Rooms 2-3 player heights tall, corridors "
           "at least 3 player widths wide, walls 16 thick; every coordinate on the grid "
           "(`grid_get`).\n"
           "3. Rooms: one layer per area (`layer_create` with \"makeCurrent\": true). "
           "Build each room and corridor with `room_create` {\"min\", \"max\", "
           "\"thickness\": 16, \"group\": \"<room name>\"}. Neighbouring rooms touch "
           "with their outer faces and never overlap. Cut doorways and windows with "
           "`opening_cut`, passing the wall brush ids of both rooms. Stairs: "
           "`brush_create_shape` {\"shape\": \"stairs\", \"stepHeight\": <the game's step "
           "height>}. Other blocks: `brush_create_box`. Pass \"dryRun\": true when "
           "unsure, and save with `document_save` after every room.\n"
           "4. Player start: read the class with `entity_class_describe` (e.g. "
           "info_player_start; the name depends on the game), then place it with "
           "`entity_create_point` and \"dropToFloor\": true in the first room.\n"
           "5. Check: `spaces_list` (one space per room, \"sealed\": true, openings where "
           "you cut them), `walkable_plan` (every room reachable from the start), "
           "`view_snapshot` from above and with an eyeHeight camera per room "
           "(`agent_camera_set`), `map_check`, `issues_list` {\"codes\": "
           "[\"Z_FIGHTING\"]}. Fix what they report.\n"
           "6. If `compile_tools_get` shows the tools as ok, `compile_run` {\"preset\": "
           "\"fast\"} and poll `compile_status`; on a leak `pointfile_load` and close the "
           "gap.\n"
           "7. Record the rooms with `map_manifest_set` (spaces with name and purpose, "
           "the player start as a key point) and `document_save`.\n\n"
           "Report: each room with its size and group, the openings between them, what "
           "the checks found and fixed, what is still open, and one overview snapshot.";
}

std::string difficultyPlan(const std::string& difficulty)
{
  if (difficulty == "easy")
  {
    return "easy: everything you place appears on every skill (no difficulty flags).";
  }
  if (difficulty == "normal")
  {
    return "normal: the new entities appear on normal and hard only (set the class's "
           "'not on easy' flag).";
  }
  if (difficulty == "hard")
  {
    return "hard: the new entities appear on hard only (set the 'not on easy' and 'not "
           "on normal' flags).";
  }
  if (difficulty == "all")
  {
    return "all: a base population for every skill, plus extra enemies that are "
           "hidden on easy ('not on easy') and a few hard-only ones ('not on easy' and "
           "'not on normal'); fewer health items on hard.";
  }
  return difficulty + " (as the user described it).";
}

std::string populateLevel(const PromptArguments& arguments)
{
  const auto difficulty = argumentOr(arguments, "difficulty", "all");
  const auto theme = argumentOr(arguments, "theme", "whatever fits the level");
  const auto spaces = argumentOr(arguments, "spaces", "the whole map");

  return std::string{GuideHint} + "Populate the level with enemies and items.\nArea: "
         + spaces + "\nTheme: " + theme + "\nDifficulty: " + difficultyPlan(difficulty)
         + "\n\n"
           "1. Learn the level: `map_summary`, `map_manifest_get`, `spaces_list` "
           "{\"detail\": \"full\"} (rooms, sizes, openings, what is inside), "
           "`walkable_plan` (the order in which the player reaches the rooms).\n"
           "2. Learn the classes of this game, do not assume Quake: "
           "`entity_classes_list` {\"prefix\": \"monster_\"} and the item prefixes the "
           "game uses (e.g. item_, weapon_, ammo_). `entity_class_describe` every class "
           "you use: size, model and its spawnflags. The difficulty flags have "
           "game-specific names (Quake \"Not on Easy\", Quake 2 \"Not in Easy\"); if the "
           "classes have none (e.g. Half-Life), tell the user; you can put the extra "
           "enemies into their own layer (`layer_create`) that the user can leave out of "
           "a build with `layer_set_state` {\"omitFromExport\": true}.\n"
           "3. Plan: weaker enemies near the start, harder ones farther away, health "
           "and ammo before and after fights, nothing in doorways or on the player's "
           "path out of the start.\n"
           "4. Place: `free_spots` {\"size\": <the class size>, \"placement\": \"floor\", "
           "\"space\": <space id>, \"objectDistance\": 32} gives free origins; then "
           "`entity_create_point` {\"classname\", \"position\": <spot origin>, \"angle\": "
           "<yaw toward where the player comes from>, \"dropToFloor\": true}. Set the "
           "difficulty flags with `entity_spawnflags_set` {\"ids\", \"set\": [<flag "
           "names>]}.\n"
           "5. Check: `entity_placement_check` {\"scope\": \"map\", \"onlyProblems\": "
           "true}, `map_check` {\"checks\": [\"placement\", \"rooms\"]}, and a "
           "`view_snapshot` per room with {\"annotations\": {\"labels\": true}}. Apply "
           "suggested moves with `objects_move`.\n"
           "6. `map_manifest_set` (notes on the encounters) and `document_save`.\n\n"
           "Report: per room what you placed (class, count, facing, difficulty flags) "
           "and why, and anything the checks still report.";
}

std::string lightingPass(const PromptArguments& arguments)
{
  const auto mood = argumentOr(
    arguments,
    "mood",
    "neutral and readable: every walkable area lit, key points (doors, stairs, items) "
    "brighter");
  const auto spaces = argumentOr(arguments, "spaces", "every room of the map");

  return std::string{GuideHint} + "Light the level.\nMood: " + mood + "\nArea: " + spaces
         + "\n\n"
           "1. Learn the rooms: `spaces_list` {\"detail\": \"full\"}, "
           "`map_manifest_get`, and the existing lights with `objects_find` "
           "{\"classname\": \"light*\"}.\n"
           "2. Learn how this game lights: `entity_classes_list` {\"prefix\": "
           "\"light\"} and `entity_class_describe` of the light class. The brightness "
           "and color keys differ per game (Quake \"light\" and \"_color\", Half-Life "
           "\"_light\" as \"r g b brightness\"); `entity_color_set` writes the color in "
           "the right form. Some games also light from textures (Half-Life texture "
           "lights, Quake 3 shaders): prefer visible fixtures with such textures there.\n"
           "3. Put the lights into their own layer (`layer_create` {\"name\": "
           "\"Lights\", \"makeCurrent\": true}).\n"
           "4. Place: about one light per 256 units of floor, 16-32 units below the "
           "ceiling and away from walls; `free_spots` {\"placement\": \"ceiling\"} and "
           "`surroundings` help. Create with `entity_create_point` {\"classname\", "
           "\"position\", \"properties\": {<brightness>}}, color with `entity_color_set`. "
           "Match the mood: warm or cold colors, contrast, a few styled (flickering or "
           "pulsing) lights where the game supports light styles.\n"
           "5. Check: `map_check` {\"checks\": [\"placement\", \"rooms\"]} (lights may "
           "float, but not sit inside brushes) and a `view_snapshot` per room with "
           "{\"annotations\": {\"labels\": true}} to see the positions. The editor does "
           "not preview compiled light: `compile_run` {\"preset\": \"normal\"}, poll "
           "`compile_status`, and offer `engine_launch` so that the user can judge it.\n\n"
           "Report: the lights per room (count, brightness, color) and the idea behind "
           "them, and the compile result.";
}

std::string texturePass(const PromptArguments& arguments)
{
  const auto theme =
    argumentOr(arguments, "theme", "consistent with the materials the map already uses");
  const auto materials = optionalArgument(arguments, "materials");

  return std::string{GuideHint} + "Texture the level.\nTheme: " + theme
         + "\nMaterials: "
         + (materials ? "use " + *materials
                          + " (enable it with `materials_collections_set`: \"wads\" for "
                            "WAD games, \"enabled\" for material folders)"
                      : std::string{"the collections already enabled "
                                    "(`materials_collections_get`)"})
         + "\n\n"
           "1. Survey: `map_summary` (the most used materials), `materials_list` "
           "{\"usedOnly\": true, \"includeMissing\": true}, `spaces_list`.\n"
           "2. Choose one material per surface type (floor, wall, ceiling, trim, "
           "doors, details): search with `materials_list` {\"search\": ...}, look at the "
           "candidates with `material_preview` (names can mislead) and read "
           "`material_usage` (kind and typical scale).\n"
           "3. Apply: `material_apply` on faces or whole brushes; floors face up "
           "(normal z > 0.7), ceilings down, the rest are walls (`object_get` "
           "{\"fields\": [\"faces.id\", \"faces.normal\"]}). `material_replace` swaps "
           "materials in bulk and keeps the alignment.\n"
           "4. Align: `uv_align` {\"operation\": \"typical\"}; walls split by openings: "
           "`uv_align` {\"operation\": \"resetToWorld\"} and one `face_attributes_set` "
           "scale for all parts so that the pattern continues. Panels fit whole: "
           "`material_fit_geometry` gives the face size; never distort the aspect.\n"
           "5. Check: `uv_check` {\"scope\": \"map\"} and apply its fixes, "
           "`map_check` {\"checks\": [\"materials\"]}, and `view_snapshot` with an "
           "eyeHeight camera in each room.\n\n"
           "Report: the material chosen for each surface type, rooms that differ and "
           "why, and the problems left.";
}

std::string fixIssues(const PromptArguments& arguments)
{
  const auto scope = argumentOr(arguments, "scope", "safe");
  const auto policy =
    scope == "all"
      ? std::string{"all: apply every fix that resolves an issue, but before a fix "
                    "deletes objects, list them for the user with a dry run first."}
      : std::string{"safe: apply only fixes that neither delete objects nor change "
                    "gameplay (Delete Property on empty values, Replace quotation "
                    "marks, Snap Vertices, Reset UV Scale, Apply Suggested Move, Apply "
                    "Suggested UV Fix), plus Delete Objects for EMPTY_BRUSH_ENTITY and "
                    "EMPTY_GROUP. Report everything else instead of fixing it."};

  return std::string{GuideHint} + "Find and fix the problems of the map.\nScope: "
         + policy
         + "\n\n"
           "1. Collect: `issues_list` (editor validators and MCP checks), `map_check` "
           "(placement, player start, links, materials, rooms) and `entity_links_get` "
           "{\"brokenOnly\": true}.\n"
           "2. Explain each code to the user in one line: what is wrong and why it "
           "matters in the game.\n"
           "3. Fix with `issue_fix` by codes or issue ids; call it with \"dryRun\": true "
           "first and read what it would change. Never delete entities only because "
           "the definition file lacks their class.\n"
           "4. `map_check` findings carry a suggestedFix call: check that it makes sense, "
           "then call it.\n"
           "5. Z_FIGHTING and ENTITY_OUTSIDE_HULL need your own edits: move or shrink one "
           "of the faces (`vertices_move`, `objects_move`); close the gap named in the "
           "issue (`brush_create_box`) or move the entity inside.\n"
           "6. Run `issues_list` and `map_check` again. Hide accepted issues with "
           "`issue_hide` only when the user agrees.\n\n"
           "Report: fixed issues per code, every object deleted or changed, the issues "
           "left with the reason, and the counts before and after.";
}

std::string compileAndDebug(const PromptArguments& arguments)
{
  const auto profile = optionalArgument(arguments, "profile");
  const auto preset = argumentOr(arguments, "preset", "normal");
  const auto how = profile ? "the saved compile profile \"" + *profile
                               + "\": `compile_run` {\"profile\": \"" + *profile + "\"}"
                           : "the preset \"" + preset
                               + "\": `compile_run` {\"preset\": \"" + preset + "\"}";

  return std::string{GuideHint} + "Compile the map and debug the result with " + how
         + ".\n\n"
           "1. `editor_status`, then `document_save`. A map that was never saved "
           "cannot compile: ask the user for a path and use `document_save_as`.\n"
           "2. Tools: `compile_tools_get` must show every tool of the chain as ok; "
           "otherwise ask the user for the paths and set them with "
           "`compile_tools_set`. `compile_presets_list` and `compile_profiles_list` show "
           "what exists.\n"
           "3. Before compiling, `map_check` and fix its errors.\n"
           "4. Start the compile; it returns a run handle at once. Poll `compile_status` "
           "until the state is no longer running; read its errors and warnings "
           "(\"log\": \"full\" for the whole log).\n"
           "5. On a leak: `pointfile_load` returns the path, the entity at its start and "
           "where it leaves the map. Look there with `surroundings` and a "
           "`view_snapshot` with {\"options\": {\"leakPath\": true}}, move the entity "
           "inside (`objects_move`) or close the gap (`brush_create_box`), "
           "`pointfile_unload`, and compile again.\n"
           "6. Other errors: explain them from the log; `console_read` {\"minLevel\": "
           "\"warning\"} shows what the editor logged.\n"
           "7. On success: if `engine_profiles_list` has a profile, offer to start the "
           "game with `engine_launch` (ask first if the user may be playing).\n\n"
           "Report: success or failure, the compiled file, leaks and how you fixed them, "
           "warnings that matter, and what you changed in the map.";
}

std::string explainMap(const PromptArguments& arguments)
{
  const auto focus =
    argumentOr(arguments, "focus", "the whole map: layout, gameplay flow and problems");

  return std::string{GuideHint} + "Explain the active map to the user. Focus: " + focus
         + "\nDo not change the map.\n\n"
           "1. Overview: `map_summary`, `game_info`, `map_manifest_get` (the rooms and "
           "notes other agents recorded).\n"
           "2. Layout: `spaces_list` (rooms, sizes, openings, neighbours), "
           "`map_plan_view` {\"format\": \"image\"}, and a `view_snapshot` overview plus "
           "`view_snapshots_around` for the important areas.\n"
           "3. Entities: the classes in `map_summary`; `objects_find` {\"kinds\": [\"entity\"]} "
           "for positions; `entity_class_describe` for classes you do not know in this "
           "game.\n"
           "4. Gameplay flow: `entity_links_get` (what triggers doors, lifts, monsters and "
           "relays), `walkable_plan` (where the player can go from the start).\n"
           "5. Problems: `issues_list` and `map_check`.\n\n"
           "Write: the game and size of the map, a room-by-room walkthrough in the order "
           "the player meets them, the gameplay flow (what triggers what), notable "
           "entities and secrets, and the problems with a suggested fix for each.";
}

std::string explainEntity(const PromptArguments& arguments)
{
  const auto classname = argumentOr(arguments, "classname", "");

  return "Explain the entity class \"" + classname
         + "\" of the active map's game in plain language.\n\n"
           "1. `entity_class_describe` {\"classname\": \""
         + classname
         + "\"}: description, point or brush class, size, model, properties with "
           "types, defaults and choices, and spawnflags. If the class is unknown, find "
           "similar ones with `entity_classes_list` {\"search\": ...} and ask which one "
           "is meant.\n"
           "2. Its use in this map: `objects_find` {\"classname\": \""
         + classname
         + "\"} and, for the instances, `entity_links_get` {\"ids\": [...]}.\n"
           "3. If it has a model: `entity_model_info` on an instance lists its animations "
           "and their bounds.\n\n"
           "Explain: what it does in the game, whether it is a point entity "
           "(`entity_create_point`) or made from brushes (`entity_create_brush`), the "
           "properties and spawnflags that matter with sensible values, how it connects "
           "to other entities (target and targetname), and common mistakes. Stay with "
           "what the definition says for this game and mark anything you add from "
           "general knowledge.";
}

std::string cleanupMap(const PromptArguments& arguments)
{
  const auto format = argumentOr(arguments, "format", "map");
  const auto exportPath = argumentOr(
    arguments,
    "exportPath",
    "a new file next to the map with the suffix _clean (ask the user if unsure)");

  auto exportStep = std::string{};
  if (format == "map")
  {
    exportStep =
      "`document_export_map` {\"path\": <export path>, "
      "\"stripEditorProperties\": true} writes a .map copy without the "
      "TrenchBroom properties and without layers marked omitFromExport.";
  }
  else if (format == "obj")
  {
    exportStep =
      "`document_export_obj` {\"path\": <export path>} writes the geometry as "
      "OBJ with an MTL file.";
  }
  else
  {
    exportStep = "the map format \"" + format
                 + "\": check that `game_info` lists it. There is no in-place "
                   "conversion: `document_save`, then `document_new` {\"game\": <the "
                   "same game>, \"format\": \""
                 + format
                 + "\"}, delete the template brush it may start with (`objects_find`, "
                   "`objects_delete`), `map_import` {\"path\": <the saved map>}, set the "
                   "texture collections again (`materials_collections_set`), compare "
                   "`map_stats` of both documents, and `document_save_as` {\"path\": "
                   "<export path>}. Layers are not imported.";
  }

  return std::string{GuideHint} + "Clean up the active map and export it.\nFormat: "
         + format + "\nExport path: " + exportPath
         + "\n\n"
           "Every step below is an undo step; tell the user before removing anything "
           "that is not clearly unused.\n"
           "1. Survey: `map_summary`, `map_stats`, `layers_list`, `issues_list`, "
           "`map_check`.\n"
           "2. Unused objects: `issue_fix` {\"codes\": [\"EMPTY_BRUSH_ENTITY\", "
           "\"EMPTY_GROUP\"]}; empty layers (no objects in `layers_list`) with "
           "`layer_remove`; names nothing refers to with `entity_links_get` "
           "{\"includeUnreferenced\": true} (report them, remove only dead helpers).\n"
           "3. Properties: `issues_list` for empty values, quotation marks and long "
           "values, fixed with `issue_fix`; broken links from `entity_links_get` "
           "{\"brokenOnly\": true} fixed with `entity_link` or "
           "`entity_property_remove`.\n"
           "4. Geometry and materials: `vertices_snap` {\"mode\": \"integer\"} for "
           "non-integer vertices, `issues_list` {\"codes\": [\"Z_FIGHTING\"]}, "
           "`materials_list` {\"usedOnly\": true, \"includeMissing\": true} and "
           "`material_replace` for missing materials.\n"
           "5. `map_check` and `issues_list` again, then `document_save`.\n"
           "6. Export: "
         + exportStep
         + "\n\n"
           "Report: what was removed and changed (counts and ids), what was left and "
           "why, and the path of the exported file.";
}

} // namespace

void registerPrompts(PromptRegistry& prompts)
{
  prompts.add(PromptDef{
    "blockout_level",
    "Block Out a Level",
    "Builds rooms and corridors from a layout description at the game's player scale, "
    "adds a player start and checks for leaks and reachability.",
    {
      PromptArgument{
        "description",
        "The layout wanted, e.g. 'two 512x512 rooms joined by a corridor'",
        true},
      PromptArgument{"game", "The game, e.g. 'Quake' (default: the active document's)"},
      PromptArgument{"style", "Architectural style or theme, e.g. 'gothic castle'"},
    },
    [](const PromptArguments& arguments) -> Result<Json, ToolError> {
      return userTextMessage(blockoutLevel(arguments));
    },
  });

  prompts.add(PromptDef{
    "populate_level",
    "Populate with Enemies and Items",
    "Places the game's monsters and items on floors in free space, with difficulty "
    "spawnflags, and checks their placement.",
    {
      PromptArgument{
        "difficulty",
        "easy, normal, hard or all: the skills the new entities appear on (default: "
        "all)"},
      PromptArgument{"theme", "Kind of enemies and items, e.g. 'undead', 'military'"},
      PromptArgument{"spaces", "Space ids from spaces_list or an area to populate"},
    },
    [](const PromptArguments& arguments) -> Result<Json, ToolError> {
      return userTextMessage(populateLevel(arguments));
    },
  });

  prompts.add(PromptDef{
    "lighting_pass",
    "Lighting Pass",
    "Places light entities per room with sensible spacing, brightness and colors, and "
    "compiles to judge them.",
    {
      PromptArgument{"mood", "The mood, e.g. 'dark and tense', 'bright lab'"},
      PromptArgument{"spaces", "Space ids from spaces_list or an area to light"},
    },
    [](const PromptArguments& arguments) -> Result<Json, ToolError> {
      return userTextMessage(lightingPass(arguments));
    },
  });

  prompts.add(PromptDef{
    "texture_pass",
    "Texture Pass",
    "Applies a consistent material theme per surface type (floor, wall, ceiling, trim) "
    "and aligns the textures.",
    {
      PromptArgument{"theme", "The look, e.g. 'rusty industrial'"},
      PromptArgument{
        "materials", "A material collection or WAD file to use, e.g. 'halflife.wad'"},
    },
    [](const PromptArguments& arguments) -> Result<Json, ToolError> {
      return userTextMessage(texturePass(arguments));
    },
  });

  prompts.add(PromptDef{
    "fix_issues",
    "Fix All Issues",
    "Lists the map's problems, explains them, applies quick fixes and reports what "
    "remains.",
    {
      PromptArgument{
        "scope",
        "safe (default: only fixes that delete nothing but empty objects) or all"},
    },
    [](const PromptArguments& arguments) -> Result<Json, ToolError> {
      return userTextMessage(fixIssues(arguments));
    },
  });

  prompts.add(PromptDef{
    "compile_and_debug",
    "Compile and Debug",
    "Compiles the map, reads the result, finds and closes leaks with the point file and "
    "offers to launch the game.",
    {
      PromptArgument{"preset", "fast, normal (default) or full"},
      PromptArgument{"profile", "A saved compile profile to use instead of a preset"},
    },
    [](const PromptArguments& arguments) -> Result<Json, ToolError> {
      return userTextMessage(compileAndDebug(arguments));
    },
  });

  prompts.add(PromptDef{
    "explain_map",
    "Explain This Map",
    "Summarizes the map's layout, entities, gameplay flow (links and triggers) and "
    "problems without changing it.",
    {
      PromptArgument{"focus", "What to concentrate on, e.g. 'the secret areas'"},
    },
    [](const PromptArguments& arguments) -> Result<Json, ToolError> {
      return userTextMessage(explainMap(arguments));
    },
  });

  prompts.add(PromptDef{
    "explain_entity",
    "Explain an Entity",
    "Describes an entity class of the current game and its options in plain language.",
    {
      PromptArgument{"classname", "The entity class, e.g. 'func_door'", true},
    },
    [](const PromptArguments& arguments) -> Result<Json, ToolError> {
      return userTextMessage(explainEntity(arguments));
    },
  });

  prompts.add(PromptDef{
    "cleanup_map",
    "Clean Up a Map",
    "Removes unused objects, fixes properties and exports the map as a .map copy, OBJ or "
    "another map format.",
    {
      PromptArgument{
        "format",
        "map (default: a .map copy without editor properties), obj, or a map format "
        "of the game such as 'Valve' or 'Standard'"},
      PromptArgument{"exportPath", "Absolute path of the exported file"},
    },
    [](const PromptArguments& arguments) -> Result<Json, ToolError> {
      return userTextMessage(cleanupMap(arguments));
    },
  });
}

} // namespace tb::mcp
