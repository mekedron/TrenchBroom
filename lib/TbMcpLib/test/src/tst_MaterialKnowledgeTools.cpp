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
#include "gl/MaterialManager.h"
#include "gl/ResourceManager.h"
#include "gl/TestGl.h"
#include "gl/TestUtils.h"
#include "mcp/JsonRpc.h"
#include "mcp/McpToolFixture.h"
#include "mcp/tools/MaterialKnowledge.h"
#include "mdl/BrushNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Selection.h"
#include "ui/MapDocument.h"

#include "vm/vec.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

const Json& resultOf(const Json& structured)
{
  return structured["result"];
}

bool hasWarning(const Json& result, const std::string& code)
{
  return result.contains("warnings")
         && std::ranges::any_of(result["warnings"], [&](const auto& warning) {
              return warning["code"] == code;
            });
}

std::filesystem::path corpusFolder()
{
  return getFixtureRoot() / "test" / "mcp" / "corpus";
}

/** Loads and uploads all pending resources, like the editor does after loading. */
void processResources(mdl::Map& map)
{
  auto gl = gl::TestGl{};
  gl::processResourcesSync(
    map.resourceManager(), gl::ProcessContext{gl, [](auto, auto) {}});
}

/** A Quake document with the materials of fixture/mcp/wads/knowledge.wad loaded. */
ui::MapDocument& knowledgeDocument(McpToolFixture& fixture, const std::string& format)
{
  const auto created =
    fixture.call("document_new", Json{{"game", "Quake"}, {"format", format}});
  const auto id = resultOf(created)["document"]["id"].get<std::string>();
  auto* document = fixture.host().documentList.back().document;
  REQUIRE(fixture.documentId(*document) == id);
  const auto wad = getFixtureRoot() / "test" / "mcp" / "wads" / "knowledge.wad";
  fixture.call("materials_collections_set", Json{{"wads", Json{wad.string()}}});
  processResources(document->map());
  REQUIRE(document->map().materialManager().materials().size() == 4);
  return *document;
}

Json profileOf(const Json& usage, const std::string& name)
{
  for (const auto& profile : usage["profiles"])
  {
    if (profile["name"] == name)
    {
      return profile;
    }
  }
  FAIL("no profile for " << name);
  return Json{};
}

std::vector<std::string> profileNames(const Json& usage)
{
  auto result = std::vector<std::string>{};
  for (const auto& profile : usage["profiles"])
  {
    result.push_back(profile["name"].get<std::string>());
  }
  return result;
}

} // namespace

TEST_CASE("MaterialKnowledgeTools")
{
  auto fixture = McpToolFixture{};
  const auto knowledgeDir = *fixture.host().knowledgeDir;
  const auto corpusPath = knowledgeDir / "Quake" / "_game" / "corpus.json";

  SECTION("material_corpus_scan")
  {
    knowledgeDocument(fixture, "Standard");

    SECTION("scans a folder and reports the materials")
    {
      const auto result =
        fixture.call("material_corpus_scan", Json{{"folder", corpusFolder().string()}});
      const auto& scan = resultOf(result);
      CHECK(scan["scope"]["game"] == "Quake");
      CHECK(scan["scope"]["mod"].is_null());
      CHECK(scan["scope"]["path"] == (knowledgeDir / "Quake" / "_game").string());
      CHECK(scan["mode"] == "replace");
      CHECK(scan["files"]["total"] == 3);
      CHECK(scan["files"]["scanned"] == 2);
      REQUIRE(scan["files"]["failed"].size() == 1);
      CHECK(
        scan["files"]["failed"][0]["path"] == (corpusFolder() / "broken.map").string());
      CHECK(scan["faces"] == 72);
      CHECK(scan["materials"] == 4);
      CHECK(scan["written"] == true);
      CHECK(scan["corpus"]["path"] == corpusPath.string());
      CHECK(std::filesystem::exists(corpusPath));

      const auto& top = scan["topMaterials"];
      REQUIRE(top.size() == 4);
      CHECK(top[0]["name"] == "k_tile");
      CHECK(top[0]["kind"] == "tile");

      const auto corpus = readCorpusFile(corpusPath);
      REQUIRE(corpus.is_success());
      CHECK(corpus.value()->files.size() == 2);
      CHECK(corpus.value()->materials.at("k_panel").stats.samples == 10);
      CHECK(corpus.value()->materials.at("k_panel").textureSize == vm::vec2d{64, 64});

      SECTION("merge adds to the corpus, replace replaces it")
      {
        fixture.call(
          "material_corpus_scan",
          Json{{"folder", corpusFolder().string()}, {"mode", "merge"}});
        CHECK(readCorpusFile(corpusPath).value()->faces == 144);
        CHECK(readCorpusFile(corpusPath).value()->files.size() == 2);

        fixture.call(
          "material_corpus_scan",
          Json{{"folder", corpusFolder().string()}, {"recursive", false}});
        const auto replaced = readCorpusFile(corpusPath);
        CHECK(replaced.value()->faces == 36);
        CHECK(replaced.value()->files.size() == 1);
      }
    }

    SECTION("pattern and recursion")
    {
      const auto result = fixture.call(
        "material_corpus_scan",
        Json{
          {"folder", corpusFolder().string()},
          {"recursive", false},
          {"pattern", "PANELS_*.MAP"},
        });
      CHECK(resultOf(result)["files"]["total"] == 1);
      CHECK(resultOf(result)["files"]["failed"].empty());
    }

    SECTION("split faces are merged into surfaces")
    {
      // every surface of the map is split into pieces, like in a decompiled map: the
      // panels' fronts show k_panel once each only when their pieces are merged
      const auto folder = getFixtureRoot() / "test" / "mcp" / "corpus_split";
      const auto result =
        fixture.call("material_corpus_scan", Json{{"folder", folder.string()}});
      const auto& scan = resultOf(result);
      CHECK(scan["faces"] == 864);
      // floor: top, bottom and four sides; panels: front, back, top and two ends each
      CHECK(scan["surfaces"] == 26);
      CHECK(hasWarning(result, "DECOMPILED_INPUT"));

      const auto corpus = readCorpusFile(corpusPath);
      REQUIRE(corpus.is_success());
      const auto& panel = corpus.value()->materials.at("k_panel");
      CHECK(panel.stats.samples == 4);
      CHECK(panel.stats.once == 4);
      CHECK(
        kindFromStats(summarize(panel.stats, panel.textureSize)) == MaterialKind::Panel);
      const auto& tile = corpus.value()->materials.at("k_tile");
      CHECK(tile.stats.samples == 6);

      // original map sources are not reported
      const auto original =
        fixture.call("material_corpus_scan", Json{{"folder", corpusFolder().string()}});
      CHECK(resultOf(original)["surfaces"] == 72);
      CHECK_FALSE(hasWarning(original, "DECOMPILED_INPUT"));
    }

    SECTION("dry run")
    {
      const auto result = fixture.call(
        "material_corpus_scan",
        Json{{"folder", corpusFolder().string()}, {"dryRun", true}});
      CHECK(resultOf(result)["written"] == false);
      CHECK(resultOf(result)["wouldDo"].get<std::string>().starts_with("write "));
      CHECK(resultOf(result)["faces"] == 72);
      CHECK_FALSE(std::filesystem::exists(corpusPath));
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("material_corpus_scan", Json{{"folder", "relative"}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "material_corpus_scan",
            Json{{"folder", (corpusFolder() / "missing").string()}})
          .code
        == ErrorCode::IoError);
      CHECK(
        fixture
          .callExpectingError(
            "material_corpus_scan",
            Json{{"folder", corpusFolder().string()}, {"pattern", "*.bsp"}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "material_corpus_scan",
            Json{{"folder", corpusFolder().string()}, {"mode", "append"}})
          .code
        == ErrorCode::InvalidArgument);
      // only unparsable files
      CHECK(
        fixture
          .callExpectingError(
            "material_corpus_scan",
            Json{{"folder", corpusFolder().string()}, {"pattern", "broken.map"}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK_FALSE(std::filesystem::exists(corpusPath));
    }

    SECTION("reports progress")
    {
      auto stream = fixture.post(
        fixture.sessionId(),
        jsonrpc::makeRequest(
          1100,
          "tools/call",
          Json{
            {"name", "material_corpus_scan"},
            {"arguments", Json{{"folder", corpusFolder().string()}}},
            {"_meta", Json{{"progressToken", "scan"}}},
          }));
      CHECK(!stream->response.has_value());

      fixture.scheduler().runPending();
      REQUIRE(stream->response.has_value());
      CHECK((*stream->response)["result"]["isError"] == false);
      REQUIRE(stream->notifications.size() == 5);
      for (const auto& notification : stream->notifications)
      {
        CHECK(notification["method"] == "notifications/progress");
        CHECK(notification["params"]["total"] == 3);
      }
      CHECK(stream->notifications.back()["params"]["progress"] == 3);
    }

    SECTION("can be cancelled")
    {
      auto stream = fixture.post(
        fixture.sessionId(),
        jsonrpc::makeRequest(
          1101,
          "tools/call",
          Json{
            {"name", "material_corpus_scan"},
            {"arguments", Json{{"folder", corpusFolder().string()}}},
          }));
      REQUIRE(!stream->response.has_value());

      fixture.post(
        fixture.sessionId(),
        jsonrpc::makeNotification("notifications/cancelled", Json{{"requestId", 1101}}));
      fixture.scheduler().runPending();

      REQUIRE(stream->response.has_value());
      const auto& result = (*stream->response)["result"];
      CHECK(result["isError"] == true);
      CHECK(result["structuredContent"]["error"]["code"] == "CANCELLED");
      CHECK_FALSE(std::filesystem::exists(corpusPath));
    }

    SECTION("needs a knowledge directory")
    {
      fixture.host().knowledgeDir = std::nullopt;
      CHECK(
        fixture
          .callExpectingError(
            "material_corpus_scan", Json{{"folder", corpusFolder().string()}})
          .code
        == ErrorCode::UnsupportedInHost);
    }
  }

  SECTION("material_notes_set and material_notes_get")
  {
    knowledgeDocument(fixture, "Valve");

    SECTION("set, get and remove")
    {
      const auto set = fixture.call(
        "material_notes_set",
        Json{
          {"notes",
           Json::array({
             Json{
               {"material", "K_PANEL"},
               {"kind", "panel"},
               {"scale", 0.5},
               {"text", "fit 1x1"},
             },
             Json{{"material", "k_trim"}, {"faceSize", Json{256, 16}}},
           })},
        });
      const auto& result = resultOf(set);
      CHECK(result["scope"]["level"] == "game");
      CHECK(result["total"] == 2);
      REQUIRE(result["set"].size() == 2);
      CHECK(result["set"][0]["scale"] == Json{0.5, 0.5});
      CHECK(result["removed"].empty());

      const auto get = fixture.call("material_notes_get");
      const auto& items = get["items"];
      REQUIRE(items.size() == 2);
      CHECK(items[0]["material"] == "K_PANEL");
      CHECK(items[0]["kind"] == "panel");
      CHECK(items[0]["text"] == "fit 1x1");
      CHECK(items[0]["scope"] == "game");
      CHECK(items[1]["faceSize"] == Json{256, 16});

      CHECK(fixture.call("material_notes_get", Json{{"filter", "*trim"}})["total"] == 1);
      CHECK(fixture.call("material_notes_get", Json{{"filter", "PAN"}})["total"] == 1);

      // updating keeps the other values; clear removes values
      fixture.call(
        "material_notes_set",
        Json{
          {"notes",
           Json::array({Json{
             {"material", "k_panel"}, {"scale", Json{0.5, 0.25}}, {"clear", {"text"}}}})},
        });
      const auto updated = fixture.call("material_notes_get")["items"][0];
      CHECK(updated["kind"] == "panel");
      CHECK(updated["scale"] == Json{0.5, 0.25});
      CHECK_FALSE(updated.contains("text"));

      // notes drive material_usage
      const auto usage = fixture.call("material_usage", Json{{"materials", {"k_panel"}}});
      const auto profile = profileOf(usage, "k_panel");
      CHECK(profile["kind"]["value"] == "panel");
      CHECK(profile["kind"]["source"] == "notes");
      CHECK(profile["typicalScale"]["value"] == Json{0.5, 0.25});
      CHECK(profile["note"]["scope"] == "game");

      const auto removed = fixture.call(
        "material_notes_set",
        Json{
          {"notes",
           Json::array({
             Json{{"material", "k_panel"}, {"remove", true}},
             Json{{"material", "k_nothing"}, {"remove", true}},
           })},
        });
      CHECK(resultOf(removed)["removed"] == Json{"k_panel"});
      CHECK(resultOf(removed)["total"] == 1);
      CHECK(hasWarning(removed, "NOTE_NOT_FOUND"));
    }

    SECTION("mod notes override game notes")
    {
      fixture.call(
        "material_notes_set",
        Json{{"notes", Json::array({Json{{"material", "k_tile"}, {"kind", "tile"}}})}});
      fixture.call("mods_set", Json{{"mods", Json{"mymod"}}});

      // the default scope is the mod now
      const auto set = fixture.call(
        "material_notes_set",
        Json{{"notes", Json::array({Json{{"material", "k_tile"}, {"kind", "trim"}}})}});
      CHECK(resultOf(set)["scope"]["level"] == "mod");
      CHECK(resultOf(set)["scope"]["mod"] == "mymod");
      CHECK(
        resultOf(set)["scope"]["path"] == (knowledgeDir / "Quake" / "mymod").string());

      const auto effective = fixture.call("material_notes_get")["items"];
      REQUIRE(effective.size() == 1);
      CHECK(effective[0]["kind"] == "trim");
      CHECK(effective[0]["scope"] == "mod");

      const auto game =
        fixture.call("material_notes_get", Json{{"scope", "game"}})["items"];
      REQUIRE(game.size() == 1);
      CHECK(game[0]["kind"] == "tile");

      const auto usage = fixture.call("material_usage", Json{{"materials", {"k_tile"}}});
      CHECK(profileOf(usage, "k_tile")["kind"]["value"] == "trim");
      CHECK(profileOf(usage, "k_tile")["note"]["scope"] == "mod");
    }

    SECTION("dry run")
    {
      const auto set = fixture.call(
        "material_notes_set",
        Json{
          {"notes", Json::array({Json{{"material", "k_tile"}, {"kind", "tile"}}})},
          {"dryRun", true},
        });
      CHECK(resultOf(set)["wouldDo"].get<std::string>().starts_with("write 1 note"));
      CHECK(fixture.call("material_notes_get")["total"] == 0);
    }

    SECTION("invalid input")
    {
      const auto notes = [](Json note) {
        return Json{{"notes", Json::array({std::move(note)})}};
      };
      CHECK(
        fixture
          .callExpectingError(
            "material_notes_set", notes(Json{{"material", "x"}, {"kind", "brick"}}))
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "material_notes_set", notes(Json{{"material", "x"}, {"scale", 0}}))
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "material_notes_set", notes(Json{{"material", "x"}, {"faceSize", {64, -1}}}))
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("material_notes_set", notes(Json{{"material", "x"}}))
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "material_notes_set",
            notes(Json{{"material", "x"}, {"kind", "tile"}, {"remove", true}}))
          .code
        == ErrorCode::InvalidArgument);
      // the document has no mod
      CHECK(
        fixture
          .callExpectingError(
            "material_notes_set",
            Json{
              {"notes", Json::array({Json{{"material", "x"}, {"kind", "tile"}}})},
              {"scope", "mod"},
            })
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("material_notes_get", Json{{"scope", "mod"}}).code
        == ErrorCode::InvalidArgument);
    }

    SECTION("an invalid notes file")
    {
      const auto path = knowledgeDir / "Quake" / "_game" / "notes.json";
      std::filesystem::create_directories(path.parent_path());
      {
        auto stream = std::ofstream{path};
        stream << "{\"notes\": [{\"material\": \"x\", \"kind\": \"brick\"}]}";
      }
      CHECK(fixture.callExpectingError("material_notes_get").code == ErrorCode::IoError);
      CHECK(
        fixture
          .callExpectingError(
            "material_notes_set",
            Json{{"notes", Json::array({Json{{"material", "x"}, {"kind", "tile"}}})}})
          .code
        == ErrorCode::IoError);

      const auto usage = fixture.call("material_usage", Json{{"materials", {"k_tile"}}});
      CHECK(hasWarning(usage, "KNOWLEDGE_FILE_INVALID"));
    }

    SECTION("needs a knowledge directory")
    {
      fixture.host().knowledgeDir = std::nullopt;
      CHECK(
        fixture.callExpectingError("material_notes_get").code
        == ErrorCode::UnsupportedInHost);
      CHECK(
        fixture
          .callExpectingError(
            "material_notes_set",
            Json{{"notes", Json::array({Json{{"material", "x"}, {"kind", "tile"}}})}})
          .code
        == ErrorCode::UnsupportedInHost);
    }
  }

  SECTION("material_usage")
  {
    auto& document = knowledgeDocument(fixture, "Valve");
    auto& map = document.map();

    SECTION("reports the panel and the tiling material after a corpus scan")
    {
      fixture.call("material_corpus_scan", Json{{"folder", corpusFolder().string()}});
      const auto usage = fixture.call(
        "material_usage",
        Json{
          {"materials", {"k_panel", "K_TILE", "k_trim"}}, {"includeStatistics", true}});
      CHECK(usage["materialsFrom"] == "arguments");
      CHECK(usage["truncated"] == false);
      CHECK(usage["scope"]["game"] == "Quake");

      const auto panel = profileOf(usage, "k_panel");
      CHECK(
        panel["kind"] == Json{{"value", "panel"}, {"source", "corpus"}, {"samples", 10}});
      CHECK(panel["typicalScale"]["value"] == Json{0.5, 0.5});
      CHECK(panel["typicalScale"]["source"] == "corpus");
      CHECK(panel["texelDensity"]["value"] == Json{0.5, 0.5});
      CHECK(panel["typicalFaceSize"]["value"] == Json{32, 32});
      CHECK(panel["typicalRepeats"]["value"] == Json{1, 1});
      CHECK(panel["alignedFraction"]["value"] == 1.0);
      CHECK(panel["image"]["suggestedKind"] == "panel");
      CHECK(panel["statistics"]["corpus"]["kind"] == "panel");
      CHECK(panel["statistics"]["map"].is_null());

      const auto tile = profileOf(usage, "k_tile");
      CHECK(tile["kind"]["value"] == "tile");
      CHECK(tile["kind"]["source"] == "corpus");
      CHECK(tile["typicalScale"]["value"] == Json{1, 1});
      CHECK(tile["wholeRepeatFraction"]["value"] == 0.0);
      CHECK(tile["image"]["tilesU"] == true);
      CHECK(tile["image"]["tilesV"] == true);

      const auto trim = profileOf(usage, "k_trim");
      CHECK(trim["kind"]["value"] == "trim");
      CHECK(trim["kind"]["source"] == "corpus");
    }

    SECTION("without knowledge: image, config and name")
    {
      fixture.host().knowledgeDir = std::nullopt;
      const auto usage = fixture.call(
        "material_usage",
        Json{{"materials", {"k_*", "*water", "sky1"}}, {"includeImage", false}});
      CHECK(usage["scope"].is_null());
      CHECK(
        profileNames(usage)
        == std::vector<std::string>{"k_panel", "k_tile", "k_trim", "*water", "sky1"});
      CHECK(profileOf(usage, "k_panel")["kind"]["source"] == "image");
      CHECK(profileOf(usage, "k_panel")["kind"]["value"] == "panel");
      CHECK(profileOf(usage, "k_panel")["typicalScale"]["source"] == "config");
      CHECK(profileOf(usage, "*water")["kind"]["value"] == "liquid");
      CHECK(profileOf(usage, "*water")["kind"]["source"] == "config");
      CHECK(profileOf(usage, "*water")["configTag"] == "Liquid");
      CHECK(profileOf(usage, "sky1")["kind"]["value"] == "sky");
      CHECK(profileOf(usage, "sky1")["kind"]["source"] == "name");
      CHECK(profileOf(usage, "sky1")["loaded"] == false);
    }

    SECTION("defaults: selection, then the most used materials")
    {
      const auto panelBox = fixture.call(
        "brush_create_box",
        Json{{"min", {0, 0, 0}}, {"max", {32, 32, 32}}, {"material", "k_panel"}});
      fixture.call(
        "brush_create_box",
        Json{{"min", {64, 0, 0}}, {"max", {96, 32, 32}}, {"material", "k_tile"}});
      mdl::deselectAll(map);

      const auto all = fixture.call("material_usage", Json{{"includeImage", false}});
      CHECK(all["materialsFrom"] == "map");
      // the new map's brush has no material, which is left out
      CHECK(profileNames(all) == std::vector<std::string>{"k_panel", "k_tile"});
      CHECK(profileOf(all, "k_panel")["mapUsage"] == 6);
      CHECK(profileOf(all, "k_panel")["kind"]["source"] == "map");

      auto* brush = fixture.node(resultOf(panelBox)["brush"].get<std::string>());
      mdl::selectNodes(map, {brush});
      const auto selected = fixture.call("material_usage");
      CHECK(selected["materialsFrom"] == "selection");
      CHECK(profileNames(selected) == std::vector<std::string>{"k_panel"});
    }

    SECTION("invalid input and unmatched patterns")
    {
      CHECK(
        fixture.callExpectingError("material_usage", Json{{"materials", Json::array()}})
          .code
        == ErrorCode::InvalidArgument);
      const auto usage = fixture.call("material_usage", Json{{"materials", {"zz*"}}});
      CHECK(profileNames(usage) == std::vector<std::string>{"zz*"});
      CHECK(profileOf(usage, "zz*")["loaded"] == false);
      CHECK(hasWarning(usage, "NO_MATCH"));
    }
  }
}

} // namespace tb::mcp
