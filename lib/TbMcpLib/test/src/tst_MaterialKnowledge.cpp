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
#include "fs/TestEnvironment.h"
#include "gl/Material.h"
#include "gl/MaterialManager.h"
#include "gl/ResourceManager.h"
#include "gl/TestGl.h"
#include "gl/TestUtils.h"
#include "mcp/McpToolFixture.h"
#include "mcp/tools/AssetUtils.h"
#include "mcp/tools/GeometryUtils.h"
#include "mcp/tools/MaterialKnowledge.h"
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/UvAttributes.h"
#include "ui/MapDocument.h"

#include "vm/approx.h"
#include "vm/bbox.h"
#include "vm/vec.h"
#include "vm/vec_io.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

std::string knowledgeWad()
{
  return (getFixtureRoot() / "test" / "mcp" / "wads" / "knowledge.wad").string();
}

/** Loads and uploads all pending resources, like the editor does after loading. */
void processResources(mdl::Map& map)
{
  auto gl = gl::TestGl{};
  gl::processResourcesSync(
    map.resourceManager(), gl::ProcessContext{gl, [](auto, auto) {}});
}

/** A Quake document (Valve format) with the materials of knowledge.wad loaded. */
mdl::Map& knowledgeMap(McpToolFixture& fixture)
{
  const auto created =
    fixture.call("document_new", Json{{"game", "Quake"}, {"format", "Valve"}});
  const auto id = created["result"]["document"]["id"].get<std::string>();
  auto* document = fixture.host().documentList.back().document;
  REQUIRE(fixture.documentId(*document) == id);
  fixture.call("materials_collections_set", Json{{"wads", Json{knowledgeWad()}}});
  processResources(document->map());
  REQUIRE(document->map().materialManager().materials().size() == 4);
  return document->map();
}

/** Adds a cuboid with the given material and UV attributes on all faces. */
mdl::BrushNode* addCuboid(
  mdl::Map& map,
  const vm::bbox3d& box,
  const std::string& material,
  const mdl::UvAttributes& attributes = {})
{
  auto brush = brushBuilder(map).createCuboid(box, material).value();
  for (auto& face : brush.faces())
  {
    REQUIRE(face.setUvAttributes(attributes).is_success());
  }
  const auto added = addBrushes(map, {std::move(brush)});
  REQUIRE(added.size() == 1);
  return static_cast<mdl::BrushNode*>(added.front());
}

const mdl::BrushFace& faceWithNormal(const mdl::Brush& brush, const vm::vec3d& normal)
{
  const auto index = brush.findFace(normal);
  REQUIRE(index);
  return brush.face(*index);
}

/** A sample as sampleFace computes it for an axis-aligned face. */
FaceSample makeSample(
  const vm::vec2d& scale,
  const vm::vec2d& texels,
  const std::optional<vm::vec2d>& size,
  const std::array<bool, 2>& aligned)
{
  auto sample = FaceSample{};
  sample.scale = scale;
  sample.texelDensity = scale;
  sample.texelSize = texels;
  sample.worldSize = vm::vec2d{texels.x() * scale.x(), texels.y() * scale.y()};
  if (size)
  {
    sample.textureSize = size;
    sample.repeats = vm::vec2d{texels.x() / size->x(), texels.y() / size->y()};
    for (size_t axis = 0; axis < 2; ++axis)
    {
      const auto count = std::round(texels[axis] / (*size)[axis]);
      sample.wholeRepeatAxes[axis] =
        count >= 1 && std::abs(texels[axis] - count * (*size)[axis]) <= 1.0;
    }
    sample.alignedAxes = aligned;
  }
  return sample;
}

MaterialStats panelStats(const size_t count)
{
  auto stats = MaterialStats{};
  for (size_t i = 0; i < count; ++i)
  {
    stats.add(makeSample({0.5, 0.5}, {64, 64}, vm::vec2d{64, 64}, {true, true}));
  }
  return stats;
}

MaterialStats tileStats()
{
  auto stats = MaterialStats{};
  for (const auto& texels : std::vector<vm::vec2d>{
         {100, 72}, {200, 40}, {150, 150}, {90, 130}, {300, 20}, {48, 150}})
  {
    stats.add(makeSample({1, 1}, texels, vm::vec2d{64, 64}, {false, false}));
  }
  return stats;
}

} // namespace

TEST_CASE("MaterialKnowledge functions")
{
  SECTION("materialKindFromString")
  {
    for (const auto& name : materialKindNames())
    {
      const auto kind = materialKindFromString(name);
      REQUIRE(kind);
      CHECK(toString(*kind) == name);
    }
    CHECK(materialKindFromString("PANEL") == MaterialKind::Panel);
    CHECK(materialKindFromString("unknown") == MaterialKind::Unknown);
    CHECK(materialKindFromString("brick") == std::nullopt);
    CHECK(materialKindNames().size() == 7);
  }

  SECTION("kindFromName")
  {
    CHECK(kindFromName("sky4") == MaterialKind::Sky);
    CHECK(kindFromName("e1u1/SKY1") == MaterialKind::Sky);
    CHECK(kindFromName("*water1") == MaterialKind::Liquid);
    CHECK(kindFromName("!toxic") == MaterialKind::Liquid);
    CHECK(kindFromName("base_lava") == MaterialKind::Liquid);
    CHECK(kindFromName("lab1_gad2") == std::nullopt);
  }

  SECTION("kindFromConfig")
  {
    // the real Quake configuration
    auto fixture = McpToolFixture{};
    auto& map = knowledgeMap(fixture);
    // Quake's tags: Clip, Skip, Hint (material), Liquid ("*" prefix), Trigger (class)
    const auto clip = kindFromConfig(map, "clip");
    REQUIRE(clip);
    CHECK(clip->kind == MaterialKind::Tool);
    CHECK(clip->tag == "Clip");
    CHECK(kindFromConfig(map, "hint_skip")->kind == MaterialKind::Tool);
    const auto water = kindFromConfig(map, "*water1");
    REQUIRE(water);
    CHECK(water->kind == MaterialKind::Liquid);
    CHECK(water->tag == "Liquid");
    CHECK(kindFromConfig(map, "wall_brick") == std::nullopt);
    // Quake has no sky tag: sky comes from the name fallback only
    CHECK(kindFromConfig(map, "sky4") == std::nullopt);
  }

  SECTION("sampleFace")
  {
    SECTION("Standard face")
    {
      auto fixture = mdl::MapFixture{};
      auto& map = fixture.create();
      auto brush =
        brushBuilder(map).createCuboid({{0, 0, 0}, {128, 64, 32}}, "m").value();
      auto& face = brush.face(*brush.findFace(vm::vec3d{0, -1, 0}));
      REQUIRE(
        face.setUvAttributes({.offset = {16, 0}, .scale = {0.5f, 2.0f}}).is_success());

      const auto sized = sampleFace(face, vm::vec2d{64, 64});
      REQUIRE(sized);
      CHECK(sized->scale == vm::approx{vm::vec2d{0.5, 2}});
      CHECK(sized->texelDensity == vm::approx{vm::vec2d{0.5, 2}});
      CHECK(sized->worldSize == vm::approx{vm::vec2d{128, 32}});
      CHECK(sized->texelSize == vm::approx{vm::vec2d{256, 16}});
      REQUIRE(sized->repeats);
      CHECK(*sized->repeats == vm::approx{vm::vec2d{4, 0.25}});
      // U starts 16 texels into the texture; the top edge of V is on a texture edge
      CHECK(sized->texelStart == vm::approx{vm::vec2d{16, 48}});
      CHECK(sized->alignedAxes == std::array{false, true});
      CHECK(sized->wholeRepeatAxes == std::array{true, false});
      CHECK_FALSE(sized->aligned());
      CHECK_FALSE(sized->wholeRepeats());
      CHECK(sized->rotation == 0.0);

      const auto unsized = sampleFace(face, std::nullopt);
      REQUIRE(unsized);
      CHECK(unsized->texelSize == vm::approx{vm::vec2d{256, 16}});
      CHECK(unsized->repeats == std::nullopt);
      CHECK(unsized->alignedAxes == std::array{false, false});

      REQUIRE(
        face.setUvAttributes({.scale = {-1.0f, 1.0f}, .rotation = 30.0f}).is_success());
      const auto rotated = sampleFace(face, vm::vec2d{64, 64});
      REQUIRE(rotated);
      CHECK(rotated->scale == vm::approx{vm::vec2d{1, 1}});
      CHECK(rotated->flipped == std::array{true, false});
      CHECK(rotated->rotation == Catch::Approx(30.0));

      REQUIRE(face.setUvAttributes({.scale = {0.0f, 1.0f}}).is_success());
      CHECK(sampleFace(face, vm::vec2d{64, 64}) == std::nullopt);
    }

    SECTION("Valve face")
    {
      auto fixture = mdl::MapFixture{};
      auto& map = fixture.create(mdl::QuakeFixtureConfig);
      auto brush =
        brushBuilder(map).createCuboid({{0, 0, 0}, {128, 64, 32}}, "m").value();
      auto& face = brush.face(*brush.findFace(vm::vec3d{0, -1, 0}));

      const auto sample = sampleFace(face, vm::vec2d{64, 32});
      REQUIRE(sample);
      CHECK(sample->worldSize == vm::approx{vm::vec2d{128, 32}});
      CHECK(sample->texelSize == vm::approx{vm::vec2d{128, 32}});
      CHECK(*sample->repeats == vm::approx{vm::vec2d{2, 1}});
      CHECK(sample->aligned());
      CHECK(sample->wholeRepeats());
      CHECK(sample->rotation == 0.0);

      // rotating turns the Valve UV axes: U now runs along the face's height
      REQUIRE(face.setUvAttributes({.rotation = 90.0f}).is_success());
      const auto rotated = sampleFace(face, vm::vec2d{64, 32});
      REQUIRE(rotated);
      CHECK(rotated->rotation == Catch::Approx(90.0));
      CHECK(rotated->worldSize == vm::approx{vm::vec2d{32, 128}});
      CHECK(*rotated->repeats == vm::approx{vm::vec2d{0.5, 4}});
    }
  }

  SECTION("summarize and kindFromStats")
  {
    const auto panel = summarize(panelStats(5));
    CHECK(panel.samples == 5);
    CHECK(panel.sizedSamples == 5);
    CHECK(panel.typicalScale == vm::vec2d{0.5, 0.5});
    CHECK(panel.typicalFaceSize == vm::vec2d{32, 32});
    CHECK(panel.typicalRepeats == vm::vec2d{1, 1});
    CHECK(panel.wholeRepeatFraction == 1.0);
    CHECK(panel.alignedFraction == 1.0);
    CHECK(kindFromStats(panel) == MaterialKind::Panel);

    // too few samples
    CHECK(kindFromStats(summarize(panelStats(3))) == std::nullopt);

    const auto tile = summarize(tileStats());
    CHECK(tile.typicalScale == vm::vec2d{1, 1});
    CHECK(tile.wholeRepeatFraction == 0.0);
    CHECK(kindFromStats(tile) == MaterialKind::Tile);
    REQUIRE(tile.scaleRange);
    CHECK(tile.scaleRange->min == vm::vec2d{1, 1});
    CHECK(tile.scaleRange->max == vm::vec2d{1, 1});

    auto trim = MaterialStats{};
    for (const auto length : {256.0, 192.0, 320.0, 512.0})
    {
      trim.add(makeSample({1, 1}, {length, 16}, vm::vec2d{64, 16}, {true, true}));
    }
    CHECK(kindFromStats(summarize(trim)) == MaterialKind::Trim);

    // the scale range reports percentiles and extremes
    auto scales = MaterialStats{};
    for (const auto scale : {0.25, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 1.0, 2.0})
    {
      scales.add(makeSample({scale, scale}, {64, 64}, std::nullopt, {false, false}));
    }
    const auto range = summarize(scales).scaleRange;
    REQUIRE(range);
    CHECK(range->min == vm::vec2d{0.25, 0.25});
    CHECK(range->low == vm::vec2d{0.25, 0.25});
    CHECK(range->high == vm::vec2d{1, 1});
    CHECK(range->max == vm::vec2d{2, 2});

    // without sizes at sampling time, repeats are derived from the texel extents
    auto unsized = MaterialStats{};
    for (size_t i = 0; i < 4; ++i)
    {
      unsized.add(makeSample({0.5, 0.5}, {64, 64}, std::nullopt, {false, false}));
    }
    CHECK(summarize(unsized).typicalRepeats == std::nullopt);
    const auto derived = summarize(unsized, vm::vec2d{64, 64});
    CHECK(derived.sizedSamples == 4);
    CHECK(derived.typicalRepeats == vm::vec2d{1, 1});
    CHECK(derived.wholeRepeatFraction == 1.0);
    CHECK(derived.alignedFraction == std::nullopt);
    CHECK(kindFromStats(derived) == MaterialKind::Panel);
  }

  SECTION("analyzeImage")
  {
    // a checkerboard of 1 pixel squares repeats seamlessly on both axes
    auto checker = makeImage(8, 8, {0, 0, 0, 255});
    for (size_t y = 0; y < 8; ++y)
    {
      for (size_t x = 0; x < 8; ++x)
      {
        if ((x + y) % 2 == 0)
        {
          std::fill_n(checker.pixels.begin() + long((y * 8 + x) * 4), 3, 255);
        }
      }
    }
    const auto tile = analyzeImage(checker);
    CHECK(tile.tilesU);
    CHECK(tile.tilesV);
    CHECK(tile.suggestedKind == MaterialKind::Tile);

    // a horizontal gradient tiles vertically only
    auto gradient = makeImage(16, 16, {0, 0, 0, 255});
    for (size_t y = 0; y < 16; ++y)
    {
      for (size_t x = 0; x < 16; ++x)
      {
        std::fill_n(gradient.pixels.begin() + long((y * 16 + x) * 4), 3, x * 16);
      }
    }
    const auto trim = analyzeImage(gradient);
    CHECK_FALSE(trim.tilesU);
    CHECK(trim.tilesV);
    CHECK(trim.edgeDifference.x() == Catch::Approx(240.0 / 255.0));
    CHECK(trim.suggestedKind == MaterialKind::Trim);

    const auto flat = analyzeImage(makeImage(64, 8, {10, 20, 30, 255}));
    CHECK(flat.aspectRatio == 8.0);
    CHECK(flat.suggestedKind == MaterialKind::Trim);

    const auto transparent = analyzeImage(makeImage(4, 4, {0, 0, 0, 0}));
    CHECK(transparent.transparentFraction == 1.0);
    CHECK(transparent.suggestedKind == MaterialKind::Decal);
  }

  SECTION("notes JSON")
  {
    const auto note = MaterialNote{
      "LAB1_GAD2",
      MaterialKind::Panel,
      vm::vec2d{0.5, 0.5},
      vm::vec2d{64, 64},
      "fit 1x1",
      "2026-09-27T10:15:00Z"};
    const auto json = toJson(note);
    CHECK(json["material"] == "LAB1_GAD2");
    CHECK(json["kind"] == "panel");
    const auto parsed = materialNoteFromJson(json);
    REQUIRE(parsed);
    CHECK(toJson(*parsed) == json);

    // a single number is a scale for both axes
    const auto scalar = materialNoteFromJson(Json{{"material", "x"}, {"scale", 0.25}});
    REQUIRE(scalar);
    CHECK(scalar->scale == vm::vec2d{0.25, 0.25});
    CHECK(
      materialNoteFromJson(Json{{"material", "x"}, {"kind", "brick"}}) == std::nullopt);
    CHECK(materialNoteFromJson(Json{{"kind", "panel"}}) == std::nullopt);

    auto file = NotesFile{};
    file.notes["lab1_gad2"] = note;
    const auto fileJson = toJson(file);
    const auto parsedFile = notesFileFromJson(fileJson);
    REQUIRE(parsedFile.is_success());
    CHECK(toJson(parsedFile.value()) == fileJson);
    CHECK(notesFileFromJson(Json{{"notes", 3}}).is_error());
  }

  SECTION("corpus JSON and merge")
  {
    auto corpus = CorpusFile{};
    corpus.game = "Quake";
    corpus.folders = {"/maps"};
    corpus.files = {"/maps/a.map"};
    corpus.faces = 5;
    corpus.materials["k_panel"] = {"K_PANEL", vm::vec2d{64, 64}, panelStats(5)};

    const auto json = toJson(corpus);
    CHECK(json["mod"].is_null());
    const auto parsed = corpusFileFromJson(json);
    REQUIRE(parsed.is_success());
    CHECK(toJson(parsed.value()) == json);
    CHECK(corpusFileFromJson(Json{{"materials", Json::array()}}).is_error());

    auto other = CorpusFile{};
    other.folders = {"/maps2"};
    other.files = {"/maps2/b.map", "/maps/a.map"};
    other.faces = 6;
    other.materials["k_panel"] = {"k_panel", std::nullopt, panelStats(1)};
    other.materials["k_tile"] = {"k_tile", vm::vec2d{64, 64}, tileStats()};
    corpus.merge(other);
    CHECK(corpus.folders == std::vector<std::string>{"/maps", "/maps2"});
    CHECK(corpus.files == std::vector<std::string>{"/maps/a.map", "/maps2/b.map"});
    CHECK(corpus.faces == 11);
    CHECK(corpus.materials.size() == 2);
    CHECK(corpus.materials["k_panel"].name == "K_PANEL");
    CHECK(corpus.materials["k_panel"].textureSize == vm::vec2d{64, 64});
    CHECK(corpus.materials["k_panel"].stats.samples == 6);
  }

  SECTION("sanitizeFolderName")
  {
    CHECK(sanitizeFolderName("Half-Life") == "Half-Life");
    CHECK(sanitizeFolderName("Quake 2") == "Quake 2");
    CHECK(sanitizeFolderName("a/b:c*") == "a_b_c_");
    CHECK(sanitizeFolderName("..") == "__");
    CHECK(sanitizeFolderName("") == "_");
  }

  SECTION("knowledge files")
  {
    auto env = fs::TestEnvironment{};
    const auto path = env.dir() / "Quake" / "_game" / "notes.json";
    CHECK(readNotesFile(path).value() == nullptr);

    auto notes = NotesFile{};
    notes.notes["k_panel"] =
      MaterialNote{"k_panel", MaterialKind::Panel, {}, {}, {}, "t"};
    REQUIRE(writeNotesFile(path, notes).is_success());
    const auto read = readNotesFile(path);
    REQUIRE(read.is_success());
    REQUIRE(read.value());
    CHECK(read.value()->notes.at("k_panel").kind == MaterialKind::Panel);
    // cached: the same object is returned while the file is unchanged
    CHECK(readNotesFile(path).value() == read.value());

    env.createFile(path, "{ not json");
    CHECK(readNotesFile(path).is_error());

    const auto corpusPath = env.dir() / "Quake" / "_game" / "corpus.json";
    auto corpus = CorpusFile{};
    corpus.materials["k_tile"] = {"k_tile", std::nullopt, tileStats()};
    REQUIRE(writeCorpusFile(corpusPath, corpus).is_success());
    const auto readCorpus = readCorpusFile(corpusPath);
    REQUIRE(readCorpus.is_success());
    CHECK(readCorpus.value()->materials.at("k_tile").stats.samples == 6);
  }
}

TEST_CASE("Histogram")
{
  SECTION("add, mode and percentile")
  {
    auto histogram = Histogram{2};
    histogram.add({1, 1}, 3);
    histogram.add({0.5, 0.5});
    histogram.add({2, 4});
    CHECK(histogram.total() == 5);
    CHECK(histogram.mode() == Histogram::Key{1, 1});
    CHECK(histogram.percentile(0, 0.1) == 0.5);
    CHECK(histogram.percentile(0, 0.5) == 1.0);
    CHECK(histogram.percentile(1, 1.0) == 4.0);
    CHECK(histogram.min(1) == 0.5);
    CHECK(histogram.max(1) == 4.0);
    CHECK(histogram.fraction(0, [](const double v) { return v >= 1.0; }) == 0.8);
    CHECK(Histogram{1}.mode() == std::nullopt);
  }

  SECTION("merge")
  {
    auto lhs = Histogram{1};
    lhs.add({1, 0});
    auto rhs = Histogram{1};
    rhs.add({1, 0}, 2);
    rhs.add({-3, 0});
    lhs.merge(rhs);
    CHECK(lhs.total() == 4);
    CHECK(lhs.entries().at({1, 0}) == 3);
    CHECK(lhs.min(0) == -3.0);
  }

  SECTION("cap")
  {
    auto histogram = Histogram{1};
    for (auto i = 0; i < 20; ++i)
    {
      histogram.add({double(i), 0}, i == 5 ? 10 : 1);
    }
    histogram.cap(4);
    CHECK(histogram.entries().size() == 4);
    CHECK(histogram.total() == 29);
    CHECK(histogram.mode() == Histogram::Key{5, 0});
    // the extremes survive
    CHECK(histogram.min(0) == 0.0);
    CHECK(histogram.max(0) == 19.0);
    auto sum = uint64_t(0);
    for (const auto& [key, count] : histogram.entries())
    {
      sum += count;
    }
    CHECK(sum == 29);
  }

  SECTION("JSON round trip")
  {
    auto histogram = Histogram{2};
    histogram.add({0.35, 0.35}, 2);
    histogram.add({1, 1});
    const auto json = histogram.toJson();
    const auto parsed = Histogram::fromJson(json, 2);
    REQUIRE(parsed);
    CHECK(parsed->toJson() == json);
    CHECK(parsed->total() == 3);
    CHECK(
      Histogram::fromJson(Json{{"e", Json::array({Json::array({1, 2})})}}, 2)
      == std::nullopt);
  }
}

TEST_CASE("MaterialStats")
{
  SECTION("add")
  {
    auto stats = MaterialStats{};
    stats.add(makeSample({0.5, 0.5}, {64, 64}, vm::vec2d{64, 64}, {true, false}));
    stats.add(makeSample({1, 1}, {100, 64}, std::nullopt, {false, false}));
    CHECK(stats.samples == 2);
    CHECK(stats.sizedSamples == 1);
    CHECK(stats.wholeRepeats == std::array<uint64_t, 3>{1, 1, 1});
    CHECK(stats.aligned == std::array<uint64_t, 3>{1, 0, 0});
    CHECK(stats.scale.total() == 2);
    CHECK(stats.repeats.total() == 1);
  }

  SECTION("merge")
  {
    auto stats = panelStats(2);
    stats.merge(tileStats());
    CHECK(stats.samples == 8);
    CHECK(stats.sizedSamples == 8);
    CHECK(stats.aligned[2] == 2);
    CHECK(stats.scale.entries().size() == 2);
  }

  SECTION("JSON round trip")
  {
    auto stats = panelStats(3);
    stats.merge(tileStats());
    stats.cap();
    const auto json = toJson(stats);
    const auto parsed = materialStatsFromJson(json);
    REQUIRE(parsed);
    CHECK(toJson(*parsed) == json);
    CHECK(materialStatsFromJson(Json{{"n", 1}}) == std::nullopt);
  }

  SECTION("cap keeps the corpus compact")
  {
    auto stats = MaterialStats{};
    for (auto i = 0; i < 200; ++i)
    {
      stats.add(makeSample(
        {0.25 + 0.01 * i, 1}, {double(10 + i), 64}, vm::vec2d{64, 64}, {false, true}));
    }
    stats.cap();
    CHECK(stats.scale.entries().size() == MaxHistogramEntries);
    CHECK(stats.worldSize.entries().size() <= MaxHistogramEntries);
    CHECK(stats.scale.total() == 200);
    CHECK(dumpJson(toJson(stats)).size() < 2000);
  }
}

TEST_CASE("MaterialKnowledge")
{
  auto fixture = McpToolFixture{};
  auto& map = knowledgeMap(fixture);
  const auto knowledgeDir = *fixture.host().knowledgeDir;

  SECTION("image analysis of the fixture textures")
  {
    const auto analysis = [&](const std::string& name) {
      const auto* material = map.materialManager().material(name);
      REQUIRE(material);
      const auto image = loadMaterialImage(map, *material);
      REQUIRE(image.is_success());
      return analyzeImage(image.value().image);
    };

    const auto tile = analysis("k_tile");
    CHECK(tile.tilesU);
    CHECK(tile.tilesV);
    CHECK(tile.suggestedKind == MaterialKind::Tile);

    const auto panel = analysis("k_panel");
    CHECK_FALSE(panel.tilesU);
    CHECK_FALSE(panel.tilesV);
    CHECK(panel.suggestedKind == MaterialKind::Panel);

    const auto trim = analysis("k_trim");
    CHECK(trim.width == 64);
    CHECK(trim.height == 16);
    CHECK(trim.tilesU);
    CHECK_FALSE(trim.tilesV);
    CHECK(trim.suggestedKind == MaterialKind::Trim);

    const auto decal = analysis("{k_decal");
    CHECK(decal.transparentFraction > 0.5);
    CHECK(decal.suggestedKind == MaterialKind::Decal);
  }

  SECTION("profile without knowledge")
  {
    auto knowledge = MaterialKnowledge{map, std::nullopt};
    CHECK(knowledge.scope() == std::nullopt);

    const auto& tile = knowledge.profile("K_TILE");
    CHECK(tile.name == "k_tile");
    CHECK(tile.loaded);
    CHECK(tile.textureSize == vm::vec2d{64, 64});
    CHECK(tile.kind.value == MaterialKind::Tile);
    CHECK(tile.kind.source == "image");
    REQUIRE(tile.typicalScale);
    CHECK(tile.typicalScale->value == vm::vec2d{1, 1});
    CHECK(tile.typicalScale->source == "config");
    CHECK(tile.scaleRange == std::nullopt);

    const auto& sky = knowledge.profile("sky4");
    CHECK_FALSE(sky.loaded);
    CHECK(sky.kind.value == MaterialKind::Sky);
    CHECK(sky.kind.source == "name");
    CHECK(sky.image == std::nullopt);

    const auto& water = knowledge.profile("*water1");
    CHECK(water.kind.value == MaterialKind::Liquid);
    CHECK(water.kind.source == "config");
    CHECK(water.configTag == "Liquid");

    const auto& unknown = knowledge.profile("nothing_here");
    CHECK(unknown.kind.value == MaterialKind::Unknown);
    CHECK(unknown.kind.source == "none");
    CHECK(unknown.imageError);

    // the image is only analyzed when needed, unless requested
    CHECK(knowledge.profile("*water1").image == std::nullopt);
  }

  SECTION("merge precedence and sources")
  {
    // six 32 x 32 faces with k_panel at scale 0.5: fitted 1 x 1 and aligned in the map
    addCuboid(map, {{0, 0, 0}, {32, 32, 32}}, "k_panel", {.scale = {0.5f, 0.5f}});
    addCuboid(map, {{64, 0, 0}, {96, 32, 32}}, "k_tile", {.scale = {0.35f, 0.35f}});

    {
      auto knowledge = MaterialKnowledge{map, knowledgeDir};
      const auto& panel = knowledge.profile("k_panel");
      CHECK(panel.mapUsage == 6);
      CHECK(panel.kind.value == MaterialKind::Panel);
      CHECK(panel.kind.source == "map");
      CHECK(panel.kind.samples == 6);
      CHECK(panel.typicalScale->value == vm::vec2d{0.5, 0.5});
      CHECK(panel.typicalScale->source == "map");
      CHECK(panel.texelDensity->value == vm::vec2d{0.5, 0.5});
      CHECK(panel.typicalFaceSize->value == vm::vec2d{32, 32});
      CHECK(panel.typicalRepeats->value == vm::vec2d{1, 1});
      CHECK(panel.alignedFraction->value == 1.0);
      CHECK(panel.image == std::nullopt);
      CHECK(knowledge.profile("k_panel", true).image);

      CHECK(knowledge.mapUsage().front().second == 6);
      CHECK(knowledge.mapStats("K_PANEL")->samples == 6);
      CHECK(knowledge.mapStats("k_trim") == nullptr);
    }

    const auto scope = knowledgeScope(map, knowledgeDir);
    CHECK(scope.game == "Quake");
    CHECK(scope.mod == std::nullopt);
    CHECK(scope.directory == knowledgeDir / "Quake" / "_game");

    // the corpus overrides the map
    auto corpus = CorpusFile{};
    corpus.materials["k_panel"] = {"k_panel", vm::vec2d{64, 64}, tileStats()};
    REQUIRE(writeCorpusFile(scope.corpusPath(), corpus).is_success());
    {
      auto knowledge = MaterialKnowledge{map, knowledgeDir};
      const auto& panel = knowledge.profile("k_panel");
      CHECK(panel.kind.value == MaterialKind::Tile);
      CHECK(panel.kind.source == "corpus");
      CHECK(panel.typicalScale->value == vm::vec2d{1, 1});
      CHECK(panel.typicalScale->source == "corpus");
      CHECK(panel.typicalScale->samples == 6);
      CHECK(panel.scaleRange->source == "corpus");
      // the map still gives values the corpus does not have
      CHECK(knowledge.profile("k_tile").typicalScale->value == vm::vec2d{0.35, 0.35});
      CHECK(knowledge.profile("k_tile").typicalScale->source == "map");
    }

    // notes override everything, also config tags
    auto notes = NotesFile{};
    notes.notes["k_panel"] =
      MaterialNote{"K_PANEL", MaterialKind::Panel, vm::vec2d{0.5, 0.5}, {}, "fit", "t"};
    notes.notes["*water1"] =
      MaterialNote{"*water1", MaterialKind::Tile, {}, vm::vec2d{128, 128}, {}, "t"};
    REQUIRE(writeNotesFile(scope.notesPath(), notes).is_success());
    {
      auto knowledge = MaterialKnowledge{map, knowledgeDir};
      const auto& panel = knowledge.profile("k_panel");
      CHECK(panel.kind.value == MaterialKind::Panel);
      CHECK(panel.kind.source == "notes");
      CHECK(panel.typicalScale->value == vm::vec2d{0.5, 0.5});
      CHECK(panel.typicalScale->source == "notes");
      CHECK(panel.noteScope == "game");
      CHECK(panel.note->text == "fit");
      // the corpus still gives the face size, which the note does not have
      CHECK(panel.typicalFaceSize->source == "corpus");

      const auto& water = knowledge.profile("*water1");
      CHECK(water.kind.value == MaterialKind::Tile);
      CHECK(water.kind.source == "notes");
      CHECK(water.configTag == "Liquid");
      CHECK(water.typicalFaceSize->value == vm::vec2d{128, 128});

      const auto json = toJson(panel, true);
      CHECK(
        json["kind"] == Json{{"value", "panel"}, {"source", "notes"}, {"samples", 0}});
      CHECK(json["note"]["scope"] == "game");
      CHECK(json["statistics"]["corpus"]["samples"] == 6);
      CHECK(json["statistics"]["map"]["samples"] == 6);
    }
  }

  SECTION("notes of the mod override notes of the game")
  {
    const auto gameScope = knowledgeScope(map, knowledgeDir);
    auto gameNotes = NotesFile{};
    gameNotes.notes["k_tile"] =
      MaterialNote{"k_tile", MaterialKind::Tile, vm::vec2d{2, 2}, {}, {}, "t"};
    gameNotes.notes["k_trim"] =
      MaterialNote{"k_trim", MaterialKind::Trim, {}, {}, "game trim", "t"};
    REQUIRE(writeNotesFile(gameScope.notesPath(), gameNotes).is_success());

    fixture.call("mods_set", Json{{"mods", Json{"base", "My Mod"}}});
    processResources(map);
    const auto modScope = knowledgeScope(map, knowledgeDir);
    CHECK(modScope.mod == "My Mod");
    CHECK(modScope.gameDirectory == gameScope.directory);
    CHECK(modScope.directory == knowledgeDir / "Quake" / "My Mod");

    auto modNotes = NotesFile{};
    modNotes.notes["k_tile"] =
      MaterialNote{"k_tile", MaterialKind::Panel, vm::vec2d{0.5, 0.5}, {}, {}, "t"};
    REQUIRE(writeNotesFile(modScope.notesPath(), modNotes).is_success());

    auto knowledge = MaterialKnowledge{map, knowledgeDir};
    const auto& tile = knowledge.profile("k_tile");
    CHECK(tile.kind.value == MaterialKind::Panel);
    CHECK(tile.noteScope == "mod");
    CHECK(tile.typicalScale->value == vm::vec2d{0.5, 0.5});

    const auto& trim = knowledge.profile("k_trim");
    CHECK(trim.kind.value == MaterialKind::Trim);
    CHECK(trim.noteScope == "game");
  }

  SECTION("invalid knowledge files are reported and ignored")
  {
    const auto scope = knowledgeScope(map, knowledgeDir);
    std::filesystem::create_directories(scope.directory);
    {
      auto stream = std::ofstream{scope.notesPath()};
      stream << "[1, 2";
    }
    auto knowledge = MaterialKnowledge{map, knowledgeDir};
    REQUIRE(knowledge.problems().size() == 1);
    CHECK(knowledge.problems().front().find("notes.json") != std::string::npos);
    CHECK(knowledge.profile("k_tile").kind.source == "image");
  }
}

} // namespace tb::mcp
