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

#include "base/PreferenceManager.h"
#include "fs/TestEnvironment.h"
#include "mcp/FakeHost.h"
#include "mcp/McpToolFixture.h"
#include "mcp/tools/ComposedDefinitions.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/GameManager.h"
#include "mdl/Map.h"
#include "ui/MapDocument.h"

#include "kd/invoke.h"

#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::mcp
{
namespace
{

using Catch::Matchers::ContainsSubstring;

const Json& resultOf(const Json& structured)
{
  return structured["result"];
}

void resetHalfLifeToolPaths(FakeHost& host)
{
  for (auto& tool : host.gameManager().gameInfo("Half-Life")->gameConfig.compilationTools)
  {
    setPref(tool.pathPreference, std::filesystem::path{});
  }
}

std::filesystem::path createExecutable(
  fs::TestEnvironment& env, const std::filesystem::path& path)
{
  env.createDirectory(path.parent_path());
  env.createFile(path, "#!/bin/sh\nexit 0\n");
  const auto absolute = env.dir() / path;
  std::filesystem::permissions(
    absolute,
    std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec
      | std::filesystem::perms::others_exec,
    std::filesystem::perm_options::add);
  return absolute;
}

const auto GameFgd =
  std::string{R"(@BaseClass = Targetname [ targetname(target_source) : "Name" ]
@include "common.fgd"
@PointClass base(Targetname) size(-16 -16 0, 16 16 72) studio() = monster_generic : "Generic" [ model(studio) : "model" ]
@PointClass base(Targetname) studio("models/scientist.mdl") = monster_scientist : "Scientist" []
)"};

const auto CommonFgd = std::string{R"(@SolidClass base(Targetname) = func_wall : "Wall" []
)"};

const auto CompilerFgd = std::string{R"(@SolidClass = func_detail : "Detail brushes" []
@PointClass color(255 0 0) = info_texlights : "Texture lights" []
)"};

} // namespace

TEST_CASE("ComposedDefinitions")
{
  auto env = fs::TestEnvironment{};

  SECTION("composedFgdPath")
  {
    CHECK(composedFgdPath("/maps/bar.map") == std::filesystem::path{"/maps/bar.mcp.fgd"});
  }

  SECTION("readFgdInlined and fgdClassNames")
  {
    env.createDirectory("game");
    env.createFile("game/game.fgd", GameFgd);
    env.createFile("game/common.fgd", CommonFgd);

    const auto text = readFgdInlined(env.dir() / "game/game.fgd");
    REQUIRE(text.is_success());
    CHECK_THAT(text.value(), ContainsSubstring("= func_wall"));
    CHECK_THAT(
      text.value(), ContainsSubstring("// included: \"common.fgd\"\n@SolidClass"));
    CHECK_THAT(text.value(), !ContainsSubstring("@include"));
    CHECK(
      fgdClassNames(text.value())
      == std::vector<std::string>{
        "Targetname", "func_wall", "monster_generic", "monster_scientist"});

    env.createFile("game/commented.fgd", "// @include \"missing.fgd\"\n");
    CHECK(
      readFgdInlined(env.dir() / "game/commented.fgd").value()
      == "// @include \"missing.fgd\"\n");
    env.createFile("game/broken.fgd", "@include \"missing.fgd\"\n");
    CHECK(readFgdInlined(env.dir() / "game/broken.fgd").is_error());
    env.createFile("game/self.fgd", "@include \"self.fgd\"\n");
    CHECK(readFgdInlined(env.dir() / "game/self.fgd").is_error());
  }

  SECTION("applyModelAddition")
  {
    const auto additions = mcpAdditions("halflife");
    REQUIRE(additions.models.size() == 4);
    CHECK(additions.classes.size() == 1);
    CHECK(mcpAdditions("quake").models.empty());

    const auto& generic = additions.models[0];
    CHECK(generic.classname == "monster_generic");

    auto text = GameFgd;
    REQUIRE(applyModelAddition(text, generic));
    CHECK_THAT(
      text,
      ContainsSubstring(
        R"(size(-16 -16 0, 16 16 72) model({"path": model, "skin": skin, "frame": sequence}) = monster_generic)"));
    CHECK_THAT(text, !ContainsSubstring("studio() = monster_generic"));

    // applying it again gives the same text
    auto again = text;
    REQUIRE(applyModelAddition(again, generic));
    CHECK(again == text);

    // only the last definition changes
    auto twice = GameFgd + GameFgd;
    REQUIRE(applyModelAddition(twice, generic));
    CHECK_THAT(
      twice.substr(0, GameFgd.size()), ContainsSubstring("studio() = monster_generic"));

    CHECK_FALSE(applyModelAddition(
      text, ModelAddition{"cycler", "model({})", "not defined in this text"}));
  }

  SECTION("composeFgd and composedFgdSources")
  {
    const auto text = composeFgd({
      {"game", "/games/hl.fgd", GameFgd},
      {"compiler", "/tools/sdhlt.fgd", CompilerFgd},
      {"mcp", {}, "@SolidClass = extra []"},
    });
    CHECK(text.starts_with(ComposedFgdMarker));
    CHECK_THAT(text, ContainsSubstring("// ---- compiler: /tools/sdhlt.fgd ----"));
    CHECK(
      composedFgdSources(text)
      == std::vector<std::pair<std::string, std::filesystem::path>>{
        {"game", "/games/hl.fgd"}, {"compiler", "/tools/sdhlt.fgd"}});
    CHECK(composedFgdSources(GameFgd).empty());
  }

  SECTION("entity_definitions_compose")
  {
    auto fixture = McpToolFixture{};
    resetHalfLifeToolPaths(fixture.host());
    auto resetLater =
      kdl::invoke_later{[&]() { resetHalfLifeToolPaths(fixture.host()); }};

    // sdHLT: tools/sdhlt.fgd next to tools/Linux/sdHLCSG
    const auto csg = createExecutable(env, "sdhlt/tools/Linux/sdHLCSG");
    env.createFile("sdhlt/tools/sdhlt.fgd", CompilerFgd);

    fixture.call("document_new", Json{{"game", "Half-Life"}});
    auto& map = fixture.host().documentList.back().document->map();

    // an unsaved map needs a path
    CHECK(
      fixture.callExpectingError("entity_definitions_compose").code
      == ErrorCode::UnsavedChanges);

    env.createDirectory("maps");
    const auto mapPath = env.dir() / "maps" / "bar.map";
    fixture.call("document_save_as", Json{{"path", mapPath.string()}});
    fixture.call(
      "compile_tools_set",
      Json{{"game", "Half-Life"}, {"tools", {{"csg", csg.string()}}}});

    const auto before = map.entityDefinitionManager().definitions().size();
    CHECK(!map.entityDefinitionManager().definition("func_detail"));

    // a dry run writes nothing
    auto result = fixture.call("entity_definitions_compose", Json{{"dryRun", true}});
    CHECK(resultOf(result)["wouldDo"].is_string());
    CHECK(!env.fileExists("maps/bar.mcp.fgd"));

    result = fixture.call("entity_definitions_compose");
    CHECK(result["undoStep"] == "AI: Compose Entity Definitions");
    const auto& composed = resultOf(result);
    CHECK(composed["path"] == (env.dir() / "maps" / "bar.mcp.fgd").string());
    CHECK(composed["spec"] == "external:bar.mcp.fgd");
    REQUIRE(composed["sources"].size() == 2);
    CHECK(composed["sources"][0]["role"] == "game");
    CHECK(composed["sources"][1]["role"] == "compiler");
    CHECK(
      composed["sources"][1]["path"] == (env.dir() / "sdhlt/tools/sdhlt.fgd").string());
    CHECK(composed["sources"][1]["classes"] == 2);

    // the model additions, but no func_detail: the compiler FGD defines it
    auto classes = std::vector<std::string>{};
    for (const auto& addition : composed["additions"])
    {
      classes.push_back(addition["classname"].get<std::string>());
    }
    CHECK(
      classes
      == std::vector<std::string>{
        "monster_generic", "monster_furniture", "cycler", "cycler_weapon"});

    // loaded: func_detail is known now, monster_generic has a model from its key
    CHECK(env.fileExists("maps/bar.mcp.fgd"));
    CHECK(map.entityDefinitionManager().definition("func_detail"));
    CHECK(map.entityDefinitionManager().definition("info_texlights"));
    CHECK(map.entityDefinitionManager().definitions().size() == before + 2);
    CHECK(composed["entityDefinitions"]["spec"] == "external:bar.mcp.fgd");
    const auto text = env.loadFile("maps/bar.mcp.fgd");
    CHECK_THAT(
      text,
      ContainsSubstring(
        R"(model({"path": model, "skin": skin, "frame": sequence}) = monster_generic)"));

    // regenerating keeps the sources, also without the compile tools
    resetHalfLifeToolPaths(fixture.host());
    result = fixture.call("entity_definitions_compose");
    CHECK(resultOf(result)["sources"].size() == 2);
    CHECK(env.loadFile("maps/bar.mcp.fgd") == text);

    // without the compiler FGD, the MCP adds func_detail
    result =
      fixture.call("entity_definitions_compose", Json{{"includeCompilerFgd", false}});
    CHECK(resultOf(result)["sources"].size() == 1);
    CHECK(resultOf(result)["additions"].back()["classname"] == "func_detail");
    CHECK(map.entityDefinitionManager().definition("func_detail"));

    // an explicit compiler FGD; a file that was not composed is not overwritten
    env.createFile("maps/own.fgd", "@PointClass = mine []\n");
    const auto error = fixture.callExpectingError(
      "entity_definitions_compose",
      Json{
        {"path", (env.dir() / "maps/own.fgd").string()},
        {"compilerFgd", (env.dir() / "sdhlt/tools/sdhlt.fgd").string()}});
    CHECK(error.code == ErrorCode::FileExists);
    result = fixture.call(
      "entity_definitions_compose",
      Json{
        {"path", (env.dir() / "maps/own.fgd").string()},
        {"compilerFgd", (env.dir() / "sdhlt/tools/sdhlt.fgd").string()},
        {"overwrite", true}});
    CHECK(resultOf(result)["spec"] == "external:own.fgd");
    CHECK(resultOf(result)["sources"].size() == 2);

    // invalid arguments
    CHECK(
      fixture
        .callExpectingError("entity_definitions_compose", Json{{"path", "relative.fgd"}})
        .code
      == ErrorCode::InvalidArgument);
    CHECK(
      fixture.callExpectingError("entity_definitions_compose", Json{{"base", "nope.fgd"}})
        .code
      == ErrorCode::InvalidArgument);
  }
}

} // namespace tb::mcp
