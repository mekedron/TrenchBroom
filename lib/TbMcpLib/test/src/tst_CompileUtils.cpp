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

#include "TestEnvironment.h"
#include "base/PreferenceManager.h"
#include "fs/DiskIO.h"
#include "fs/TestEnvironment.h"
#include "mcp/tools/CompileUtils.h"
#include "mdl/Entity.h"
#include "mdl/EntityProperties.h"
#include "mdl/GameConfig.h"
#include "mdl/ParseGameConfig.h"

#include "kd/cmd_utils.h"
#include "kd/invoke.h"
#include "kd/overload.h"
#include "kd/string_compare.h"
#include "kd/string_utils.h"

#include <filesystem>
#include <regex>
#include <set>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace tb::mcp
{
namespace
{

mdl::GameConfig loadGameConfig(const std::string& folder)
{
  const auto configPath = getFixtureRoot() / "games" / folder / "GameConfig.cfg";
  return fs::Disk::withInputStream(
           configPath,
           [](auto& stream) {
             return std::string{std::istreambuf_iterator<char>{stream}, {}};
           })
         | kdl::and_then(
           [&](const auto& str) { return mdl::parseGameConfig(str, configPath); })
         | kdl::value();
}

std::set<std::string> toolNames(const mdl::GameConfig& config)
{
  auto result = std::set<std::string>{};
  for (const auto& tool : config.compilationTools)
  {
    result.insert(tool.name);
  }
  return result;
}

/** The variable names referenced by `${...}` expressions in the given spec. */
std::set<std::string> referencedVariables(const std::string& spec)
{
  static const auto expression = std::regex{R"(\$\{([A-Za-z0-9_]+)[^}]*\})"};
  auto result = std::set<std::string>{};
  for (auto it = std::sregex_iterator{spec.begin(), spec.end(), expression};
       it != std::sregex_iterator{};
       ++it)
  {
    result.insert((*it)[1].str());
  }
  return result;
}

std::vector<std::string> presetCommands(const CompilePreset& preset)
{
  auto result = std::vector<std::string>{};
  for (const auto& task : preset.profile.tasks)
  {
    if (const auto* runTool = std::get_if<mdl::CompilationRunTool>(&task))
    {
      result.push_back(runTool->toolSpec + " " + runTool->parameterSpec);
    }
  }
  return result;
}

const auto CompileVariables = std::set<std::string>{
  "WORK_DIR_PATH",
  "MAP_DIR_PATH",
  "MAP_BASE_NAME",
  "MAP_FULL_NAME",
  "GAME_DIR_PATH",
  "MODS",
  "CPU_COUNT",
  "APP_DIR_PATH",
};

std::vector<mdl::CompilationTask> allTaskTypes()
{
  return {
    mdl::CompilationExportMap{
      .enabled = true,
      .stripTbProperties = false,
      .stripEntityPattern = "info_player_*",
      .entityToAdd = mdl::Entity{{
        {"classname", "info_player_start"},
        {"angle", "90"},
      }},
      .targetSpec = "${WORK_DIR_PATH}/test.map",
    },
    mdl::CompilationExportMap{
      .enabled = false,
      .stripTbProperties = true,
      .stripEntityPattern = std::nullopt,
      .entityToAdd = std::nullopt,
      .targetSpec = "out.map",
    },
    mdl::CompilationCopyFiles{
      .enabled = true,
      .sourceSpec = "${MAP_BASE_NAME}.bsp",
      .targetSpec = "${GAME_DIR_PATH}/${MODS[-1]}/maps",
    },
    mdl::CompilationRenameFile{
      .enabled = true,
      .sourceSpec = "a.bsp",
      .targetSpec = "b.bsp",
    },
    mdl::CompilationDeleteFiles{
      .enabled = false,
      .targetSpec = "*.prt",
    },
    mdl::CompilationRunTool{
      .enabled = true,
      .toolSpec = "${qbsp}",
      .parameterSpec = "-nopercent \"${MAP_BASE_NAME}.map\"",
      .treatNonZeroResultCodeAsError = false,
    },
    mdl::CompilationLaunchEngine{
      .enabled = true,
      .engineProfileId = "quakespasm",
      .treatLaunchFailureAsError = false,
    },
  };
}

std::string errorMessage(const Result<mdl::CompilationTask, ToolError>& result)
{
  REQUIRE(result.is_error());
  const auto error = errorOf(result);
  CHECK(error.code == ErrorCode::InvalidArgument);
  return error.message;
}

} // namespace

TEST_CASE("CompileUtils")
{
  SECTION("toString")
  {
    CHECK(toString(CompileFamily::HalfLife) == "halflife");
    CHECK(toString(CompileFamily::Quake) == "quake");
    CHECK(toString(CompileFamily::Quake2) == "quake2");
    CHECK(toString(CompileFamily::Quake3) == "quake3");
  }

  SECTION("compileFamily")
  {
    CHECK(compileFamily(loadGameConfig("Halflife")) == CompileFamily::HalfLife);
    CHECK(compileFamily(loadGameConfig("Quake")) == CompileFamily::Quake);
    CHECK(compileFamily(loadGameConfig("Quake2")) == CompileFamily::Quake2);
    CHECK(compileFamily(loadGameConfig("Quake3")) == CompileFamily::Quake3);
    CHECK(compileFamily(loadGameConfig("SoF")) == CompileFamily::Quake2);

    // Hexen 2 has bsp, vis and light, but not the Quake 2 map format
    CHECK(compileFamily(loadGameConfig("Hexen2")) == std::nullopt);
    CHECK(compileFamily(loadGameConfig("Generic")) == std::nullopt);
    // Wrath has q3map2, but the Quake 3 presets pass -game quake3
    CHECK(compileFamily(loadGameConfig("Wrath")) == std::nullopt);
    CHECK(compileFamily(mdl::GameConfig{}) == std::nullopt);
  }

  SECTION("compilePresets")
  {
    SECTION("presets only reference variables that the game defines")
    {
      const auto folder = GENERATE(
        "Halflife", "Quake", "Quake2", "Quake3", "SoF", "Heretic2", "DDayNormandy");
      CAPTURE(folder);

      const auto config = loadGameConfig(folder);
      const auto tools = toolNames(config);
      const auto presets = compilePresets(config);
      REQUIRE(presets.size() == 3);
      CHECK(presets[0].name == "fast");
      CHECK(presets[1].name == "normal");
      CHECK(presets[2].name == "full");

      for (const auto& preset : presets)
      {
        CAPTURE(preset.name);
        CHECK(!preset.description.empty());
        CHECK(preset.profile.workDirSpec == "${MAP_DIR_PATH}");
        REQUIRE(preset.profile.tasks.size() >= 3);
        CHECK(std::ranges::all_of(preset.profile.tasks, isTaskEnabled));

        // first the export, last the copy
        CHECK(
          preset.profile.tasks.front()
          == mdl::CompilationTask{mdl::CompilationExportMap{
            .enabled = true,
            .stripTbProperties = true,
            .stripEntityPattern = std::nullopt,
            .entityToAdd = std::nullopt,
            .targetSpec = "${WORK_DIR_PATH}/compile/${MAP_BASE_NAME}.map",
          }});
        CHECK(
          preset.profile.tasks.back()
          == mdl::CompilationTask{mdl::CompilationCopyFiles{
            .enabled = true,
            .sourceSpec =
              std::get<mdl::CompilationCopyFiles>(preset.profile.tasks.back()).sourceSpec,
            .targetSpec = "${GAME_DIR_PATH}/${MODS[-1]}/maps",
          }});

        for (const auto& tool : preset.tools)
        {
          CHECK(tools.contains(tool));
        }

        for (const auto& task : preset.profile.tasks)
        {
          std::visit(
            kdl::overload(
              [&](const mdl::CompilationRunTool& runTool) {
                const auto toolVariables = referencedVariables(runTool.toolSpec);
                REQUIRE(toolVariables.size() == 1);
                CHECK(tools.contains(*toolVariables.begin()));
                CHECK(runTool.treatNonZeroResultCodeAsError);
                for (const auto& variable : referencedVariables(runTool.parameterSpec))
                {
                  CHECK(CompileVariables.contains(variable));
                }

                // the file path argument stays one argument even with spaces
                const auto args = kdl::cmd_parse_args(kdl::str_replace_every(
                  runTool.parameterSpec, "${WORK_DIR_PATH}", "/my maps"));
                REQUIRE(!args.empty());
                CHECK(kdl::cs::str_is_prefix(args.back(), "/my maps/compile/"));
              },
              [&](const mdl::CompilationCopyFiles& copyFiles) {
                CHECK(kdl::cs::str_is_prefix(
                  copyFiles.sourceSpec, "${WORK_DIR_PATH}/compile/${MAP_BASE_NAME}."));
              },
              [&](const auto&) {}),
            task);
        }
      }
    }

    SECTION("Half-Life")
    {
      const auto presets = compilePresets(loadGameConfig("Halflife"));
      REQUIRE(presets.size() == 3);
      CHECK(presets[0].profile.name == "Half-Life (fast)");
      CHECK(presets[0].tools == std::vector<std::string>{"csg", "bsp", "rad"});
      CHECK(
        presetCommands(presets[0])
        == std::vector<std::string>{
          R"(${csg} "${WORK_DIR_PATH}/compile/${MAP_BASE_NAME}")",
          R"(${bsp} "${WORK_DIR_PATH}/compile/${MAP_BASE_NAME}")",
          R"(${rad} -fast "${WORK_DIR_PATH}/compile/${MAP_BASE_NAME}")",
        });
      CHECK(
        presetCommands(presets[1])
        == std::vector<std::string>{
          R"(${csg} "${WORK_DIR_PATH}/compile/${MAP_BASE_NAME}")",
          R"(${bsp} "${WORK_DIR_PATH}/compile/${MAP_BASE_NAME}")",
          R"(${vis} "${WORK_DIR_PATH}/compile/${MAP_BASE_NAME}")",
          R"(${rad} "${WORK_DIR_PATH}/compile/${MAP_BASE_NAME}")",
        });
      CHECK(
        presetCommands(presets[2])
        == std::vector<std::string>{
          R"(${csg} "${WORK_DIR_PATH}/compile/${MAP_BASE_NAME}")",
          R"(${bsp} "${WORK_DIR_PATH}/compile/${MAP_BASE_NAME}")",
          R"(${vis} -full "${WORK_DIR_PATH}/compile/${MAP_BASE_NAME}")",
          R"(${rad} -extra "${WORK_DIR_PATH}/compile/${MAP_BASE_NAME}")",
        });
    }

    SECTION("Quake")
    {
      const auto presets = compilePresets(loadGameConfig("Quake"));
      REQUIRE(presets.size() == 3);
      CHECK(presets[1].profile.name == "Quake (normal)");
      CHECK(presets[0].tools == std::vector<std::string>{"qbsp", "light"});
      CHECK(
        presetCommands(presets[0])
        == std::vector<std::string>{
          R"(${qbsp} "${WORK_DIR_PATH}/compile/${MAP_BASE_NAME}.map")",
          R"(${light} "${WORK_DIR_PATH}/compile/${MAP_BASE_NAME}.bsp")",
        });
      CHECK(
        presetCommands(presets[1])
        == std::vector<std::string>{
          R"(${qbsp} "${WORK_DIR_PATH}/compile/${MAP_BASE_NAME}.map")",
          R"(${vis} "${WORK_DIR_PATH}/compile/${MAP_BASE_NAME}.bsp")",
          R"(${light} -extra "${WORK_DIR_PATH}/compile/${MAP_BASE_NAME}.bsp")",
        });
      CHECK(
        presetCommands(presets[2])
        == std::vector<std::string>{
          R"(${qbsp} "${WORK_DIR_PATH}/compile/${MAP_BASE_NAME}.map")",
          R"(${vis} -level 4 "${WORK_DIR_PATH}/compile/${MAP_BASE_NAME}.bsp")",
          R"(${light} -extra4 -bounce "${WORK_DIR_PATH}/compile/${MAP_BASE_NAME}.bsp")",
        });

      // the bsp and the lit file are copied
      const auto& tasks = presets[1].profile.tasks;
      REQUIRE(tasks.size() == 6);
      CHECK(
        std::get<mdl::CompilationCopyFiles>(tasks[4]).sourceSpec
        == "${WORK_DIR_PATH}/compile/${MAP_BASE_NAME}.bsp");
      CHECK(
        std::get<mdl::CompilationCopyFiles>(tasks[5]).sourceSpec
        == "${WORK_DIR_PATH}/compile/${MAP_BASE_NAME}.lit");
    }

    SECTION("Quake 2")
    {
      const auto presets = compilePresets(loadGameConfig("Quake2"));
      REQUIRE(presets.size() == 3);
      CHECK(presets[2].profile.name == "Quake 2 (full)");
      CHECK(presets[2].tools == std::vector<std::string>{"bsp", "vis", "light"});
      CHECK(
        presetCommands(presets[2])
        == std::vector<std::string>{
          R"(${bsp} -q2bsp "${WORK_DIR_PATH}/compile/${MAP_BASE_NAME}.map")",
          R"(${vis} -level 4 "${WORK_DIR_PATH}/compile/${MAP_BASE_NAME}.bsp")",
          R"(${light} -extra4 -bounce "${WORK_DIR_PATH}/compile/${MAP_BASE_NAME}.bsp")",
        });
      CHECK(presets[2].profile.tasks.size() == 5);
    }

    SECTION("Quake 3")
    {
      const auto presets = compilePresets(loadGameConfig("Quake3"));
      REQUIRE(presets.size() == 3);
      CHECK(presets[0].profile.name == "Quake 3 (fast)");
      CHECK(presets[0].tools == std::vector<std::string>{"q3map2"});

      const auto game =
        std::string{R"(${q3map2} -game quake3 -fs_basepath "${GAME_DIR_PATH}" )"}
        + "-fs_game ${MODS[-1]} ";
      const auto map = std::string{R"("${WORK_DIR_PATH}/compile/${MAP_BASE_NAME}.map")"};
      CHECK(
        presetCommands(presets[0])
        == std::vector<std::string>{
          game + "-meta " + map,
          game + "-light -fast " + map,
        });
      CHECK(
        presetCommands(presets[1])
        == std::vector<std::string>{
          game + "-meta " + map,
          game + "-vis -saveprt " + map,
          game + "-light -fast -filter " + map,
        });
      CHECK(
        presetCommands(presets[2])
        == std::vector<std::string>{
          game + "-meta " + map,
          game + "-vis -saveprt " + map,
          game + "-light -fast -super 2 -filter -bounce 8 " + map,
        });
    }

    SECTION("games without a family have no presets")
    {
      CHECK(compilePresets(loadGameConfig("Generic")).empty());
    }
  }

  SECTION("findCompilePreset")
  {
    const auto config = loadGameConfig("Quake");
    const auto preset = findCompilePreset(config, "NORMAL");
    REQUIRE(preset);
    CHECK(preset->name == "normal");
    CHECK(preset->profile.name == "Quake (normal)");

    CHECK(findCompilePreset(config, "ultra") == std::nullopt);
    CHECK(findCompilePreset(loadGameConfig("Generic"), "fast") == std::nullopt);
  }

  SECTION("compilePresetsSummary")
  {
    const auto summary = compilePresetsSummary();
    for (const auto* expected :
         {"Half-Life",
          "Quake 2",
          "Quake 3",
          "q3map2",
          "-q2bsp",
          "rad -extra",
          "light -extra4 -bounce",
          "compile/",
          "maps",
          ".pts",
          ".lin",
          ".lit"})
    {
      CAPTURE(expected);
      CHECK(summary.find(expected) != std::string::npos);
    }
  }

  SECTION("checkCompileToolPath")
  {
    auto env = fs::TestEnvironment{[](auto& e) {
      e.createDirectory("dir");
      e.createFile("tool", "#!/bin/sh\n");
      e.createFile("tool.exe", "MZ");
      e.createFile("data.txt", "text");
    }};

    CHECK(checkCompileToolPath({}) == "notSet");
    CHECK(checkCompileToolPath(env.dir() / "missing") == "notFound");
    CHECK(checkCompileToolPath(env.dir() / "dir") == "notAFile");

#ifdef _WIN32
    CHECK(checkCompileToolPath(env.dir() / "tool.exe") == "ok");
    CHECK(checkCompileToolPath(env.dir() / "data.txt") == "notExecutable");
#else
    using std::filesystem::perms;
    std::filesystem::permissions(
      env.dir() / "tool", perms::owner_read | perms::owner_write);
    CHECK(checkCompileToolPath(env.dir() / "tool") == "notExecutable");

    std::filesystem::permissions(
      env.dir() / "tool", perms::owner_exec, std::filesystem::perm_options::add);
    CHECK(checkCompileToolPath(env.dir() / "tool") == "ok");
#endif
  }

  SECTION("compileToolStatus")
  {
    auto env = fs::TestEnvironment{[](auto& e) { e.createDirectory("dir"); }};

    auto tool = mdl::CompilationTool{
      "qbsp",
      "The bsp compiler",
      Preference<std::filesystem::path>{"Games/Test/Tool Path/qbsp", {}},
    };

    auto status = compileToolStatus(tool);
    CHECK(status.name == "qbsp");
    CHECK(status.description == "The bsp compiler");
    CHECK(status.path.empty());
    CHECK(status.status == "notSet");

    CHECK(
      toJson(status)
      == Json{
        {"name", "qbsp"},
        {"description", "The bsp compiler"},
        {"variable", "${qbsp}"},
        {"path", ""},
        {"status", "notSet"},
      });

    setPref(tool.pathPreference, env.dir() / "dir");
    auto resetPref =
      kdl::invoke_later{[&]() { setPref(tool.pathPreference, std::filesystem::path{}); }};

    status = compileToolStatus(tool);
    CHECK(status.path == env.dir() / "dir");
    CHECK(status.status == "notAFile");

    tool.description = std::nullopt;
    CHECK(toJson(compileToolStatus(tool))["description"].is_null());
  }

  SECTION("taskType")
  {
    const auto tasks = allTaskTypes();
    CHECK(taskType(tasks[0]) == "exportMap");
    CHECK(taskType(tasks[2]) == "copyFiles");
    CHECK(taskType(tasks[3]) == "renameFile");
    CHECK(taskType(tasks[4]) == "deleteFiles");
    CHECK(taskType(tasks[5]) == "runTool");
    CHECK(taskType(tasks[6]) == "launchEngine");
  }

  SECTION("isTaskEnabled")
  {
    const auto tasks = allTaskTypes();
    CHECK(isTaskEnabled(tasks[0]));
    CHECK(!isTaskEnabled(tasks[1]));
    CHECK(!isTaskEnabled(tasks[4]));
  }

  SECTION("enabledTasks")
  {
    const auto tasks = allTaskTypes();
    const auto profile = mdl::CompilationProfile{"test", "${MAP_DIR_PATH}", tasks};
    CHECK(
      enabledTasks(profile)
      == std::vector<mdl::CompilationTask>{
        tasks[0], tasks[2], tasks[3], tasks[5], tasks[6]});
  }

  SECTION("toJson")
  {
    const auto tasks = allTaskTypes();
    CHECK(
      toJson(tasks[0])
      == Json{
        {"type", "exportMap"},
        {"enabled", true},
        {"target", "${WORK_DIR_PATH}/test.map"},
        {"stripTbProperties", false},
        {"stripEntityPattern", "info_player_*"},
        {"entityToAdd",
         {{"properties", {{"classname", "info_player_start"}, {"angle", "90"}}}}},
      });
    CHECK(toJson(tasks[1])["stripEntityPattern"].is_null());
    CHECK(toJson(tasks[1])["entityToAdd"].is_null());
    CHECK(
      toJson(tasks[2])
      == Json{
        {"type", "copyFiles"},
        {"enabled", true},
        {"source", "${MAP_BASE_NAME}.bsp"},
        {"target", "${GAME_DIR_PATH}/${MODS[-1]}/maps"},
      });
    CHECK(
      toJson(tasks[3])
      == Json{
        {"type", "renameFile"},
        {"enabled", true},
        {"source", "a.bsp"},
        {"target", "b.bsp"},
      });
    CHECK(
      toJson(tasks[4])
      == Json{{"type", "deleteFiles"}, {"enabled", false}, {"target", "*.prt"}});
    CHECK(
      toJson(tasks[5])
      == Json{
        {"type", "runTool"},
        {"enabled", true},
        {"tool", "${qbsp}"},
        {"parameters", "-nopercent \"${MAP_BASE_NAME}.map\""},
        {"treatNonZeroExitCodeAsError", false},
      });
    CHECK(
      toJson(tasks[6])
      == Json{
        {"type", "launchEngine"},
        {"enabled", true},
        {"engineProfile", "quakespasm"},
        {"treatLaunchFailureAsError", false},
      });

    const auto profile = mdl::CompilationProfile{"test", "${MAP_DIR_PATH}", {tasks[4]}};
    CHECK(
      toJson(profile)
      == Json{
        {"name", "test"},
        {"workDir", "${MAP_DIR_PATH}"},
        {"tasks", Json::array({toJson(tasks[4])})},
      });
  }

  SECTION("compilationTaskSchema")
  {
    const auto schema = compilationTaskSchema();
    auto errors = std::vector<schema::SchemaError>{};

    SECTION("every task round trips")
    {
      for (const auto& task : allTaskTypes())
      {
        const auto json = toJson(task);
        CAPTURE(json.dump());
        const auto validated = schema.validate(json, errors);
        REQUIRE(errors.empty());
        REQUIRE(validated);
        const auto converted = compilationTaskFromJson(*validated, "task");
        REQUIRE(converted.is_success());
        CHECK(converted.value() == task);
      }
    }

    SECTION("rejects unknown types and keys")
    {
      CHECK(!schema.validate(Json{{"type", "compile"}}, errors));
      errors.clear();
      CHECK(!schema.validate(Json{{"type", "runTool"}, {"tol", "${qbsp}"}}, errors));
      errors.clear();
      CHECK(!schema.validate(Json{{"tool", "${qbsp}"}}, errors));
      errors.clear();
      CHECK(!schema.validate(
        Json{
          {"type", "exportMap"},
          {"target", "a.map"},
          {"entityToAdd", {{"properties", {{"classname", 1}}}}},
        },
        errors));
    }

    SECTION("does not add type-specific defaults")
    {
      const auto validated = schema.validate(Json{{"type", "deleteFiles"}}, errors);
      REQUIRE(validated);
      CHECK(*validated == Json{{"type", "deleteFiles"}, {"enabled", true}});
    }

    SECTION("describes every field")
    {
      for (const auto& field : schema.fields)
      {
        CAPTURE(field.name);
        CHECK(!field.schema.description.empty());
      }
    }
  }

  SECTION("compilationTaskFromJson")
  {
    SECTION("applies defaults")
    {
      CHECK(
        compilationTaskFromJson(Json{{"type", "runTool"}, {"tool", "${light}"}}, "")
          .value()
        == mdl::CompilationTask{mdl::CompilationRunTool{
          .enabled = true,
          .toolSpec = "${light}",
          .parameterSpec = "",
          .treatNonZeroResultCodeAsError = true,
        }});
      CHECK(
        compilationTaskFromJson(Json{{"type", "exportMap"}, {"target", "a.map"}}, "")
          .value()
        == mdl::CompilationTask{mdl::CompilationExportMap{
          .enabled = true,
          .stripTbProperties = true,
          .stripEntityPattern = std::nullopt,
          .entityToAdd = std::nullopt,
          .targetSpec = "a.map",
        }});
      CHECK(
        compilationTaskFromJson(
          Json{{"type", "launchEngine"}, {"engineProfile", "qs"}, {"enabled", false}}, "")
          .value()
        == mdl::CompilationTask{mdl::CompilationLaunchEngine{
          .enabled = false,
          .engineProfileId = "qs",
          .treatLaunchFailureAsError = true,
        }});
    }

    SECTION("accepts null for optional keys")
    {
      CHECK(compilationTaskFromJson(
              Json{
                {"type", "exportMap"},
                {"target", "a.map"},
                {"stripEntityPattern", nullptr},
                {"entityToAdd", nullptr},
              },
              "")
              .is_success());
    }

    SECTION("rejects keys of other types")
    {
      const auto message = errorMessage(compilationTaskFromJson(
        Json{{"type", "exportMap"}, {"target", "a.map"}, {"tool", "${qbsp}"}},
        "tasks[2]"));
      CHECK(kdl::cs::str_is_prefix(message, "tasks[2]: "));
      CHECK(message.find("'tool'") != std::string::npos);
    }

    SECTION("rejects missing required keys")
    {
      CHECK(
        errorMessage(compilationTaskFromJson(Json{{"type", "runTool"}}, "tasks[0]"))
        == "tasks[0]: a runTool task requires 'tool'");
      CHECK(
        errorMessage(
          compilationTaskFromJson(Json{{"type", "copyFiles"}, {"source", "a"}}, ""))
        == "a copyFiles task requires 'target'");
      CHECK(
        errorMessage(
          compilationTaskFromJson(Json{{"type", "renameFile"}, {"target", "a"}}, ""))
        == "a renameFile task requires 'source'");
      CHECK(
        errorMessage(compilationTaskFromJson(Json{{"type", "deleteFiles"}}, ""))
        == "a deleteFiles task requires 'target'");
      CHECK(
        errorMessage(compilationTaskFromJson(Json{{"type", "exportMap"}}, ""))
        == "an exportMap task requires 'target'");
      CHECK(
        errorMessage(compilationTaskFromJson(Json{{"type", "launchEngine"}}, ""))
        == "a launchEngine task requires 'engineProfile'");
    }

    SECTION("rejects invalid input")
    {
      errorMessage(compilationTaskFromJson(Json::array(), ""));
      errorMessage(compilationTaskFromJson(Json::object(), ""));
      errorMessage(compilationTaskFromJson(Json{{"type", "compile"}}, ""));
      errorMessage(
        compilationTaskFromJson(Json{{"type", "deleteFiles"}, {"target", 1}}, ""));
    }
  }

  SECTION("compilationProfileSchema")
  {
    const auto schema = compilationProfileSchema();
    auto errors = std::vector<schema::SchemaError>{};

    const auto validated = schema.validate(
      Json{
        {"name", "Custom"},
        {"tasks",
         {
           {{"type", "exportMap"}, {"target", "a.map"}},
           {{"type", "runTool"}, {"tool", "${qbsp}"}, {"parameters", "a.map"}},
         }},
      },
      errors);
    REQUIRE(errors.empty());
    REQUIRE(validated);
    CHECK((*validated)["workDir"] == "${MAP_DIR_PATH}");

    CHECK(!schema.validate(Json{{"name", ""}, {"tasks", Json::array()}}, errors));
    errors.clear();
    CHECK(!schema.validate(Json{{"name", "x"}}, errors));
    errors.clear();
    CHECK(!schema.validate(
      Json{{"name", "x"}, {"tasks", {{{"type", "runTool"}, {"bogus", 1}}}}}, errors));
  }

  SECTION("compilationProfileFromJson")
  {
    SECTION("round trip")
    {
      const auto profile = mdl::CompilationProfile{"test", "/maps", allTaskTypes()};
      const auto converted = compilationProfileFromJson(toJson(profile));
      REQUIRE(converted.is_success());
      CHECK(converted.value() == profile);
    }

    SECTION("presets round trip")
    {
      for (const auto& preset : compilePresets(loadGameConfig("Quake3")))
      {
        const auto converted = compilationProfileFromJson(toJson(preset.profile));
        REQUIRE(converted.is_success());
        CHECK(converted.value() == preset.profile);
      }
    }

    SECTION("defaults the work directory")
    {
      const auto converted =
        compilationProfileFromJson(Json{{"name", "x"}, {"tasks", Json::array()}});
      REQUIRE(converted.is_success());
      CHECK(converted.value().workDirSpec == "${MAP_DIR_PATH}");
    }

    SECTION("reports the failing task")
    {
      const auto converted = compilationProfileFromJson(Json{
        {"name", "x"},
        {"tasks",
         {
           {{"type", "exportMap"}, {"target", "a.map"}},
           {{"type", "runTool"}},
         }},
      });
      REQUIRE(converted.is_error());
      CHECK(errorOf(converted).code == ErrorCode::InvalidArgument);
      CHECK(errorOf(converted).message == "tasks[1]: a runTool task requires 'tool'");
    }

    SECTION("rejects a missing name or tasks")
    {
      CHECK(compilationProfileFromJson(Json{{"tasks", Json::array()}}).is_error());
      CHECK(compilationProfileFromJson(Json{{"name", "x"}}).is_error());
    }
  }
}

} // namespace tb::mcp
