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

#pragma once

#include "base/Result.h"
#include "mcp/Errors.h"
#include "mcp/Image.h"
#include "mcp/Json.h"

#include "vm/vec.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tb
{
namespace gl
{
class Material;
}

namespace mdl
{
class BrushFace;
class Map;
} // namespace mdl
} // namespace tb

namespace tb::mcp
{
class MaterialImageLoader;

// Level-design knowledge about materials (E11): how a material's texture is meant to lie
// on faces. Knowledge comes from data only, for any game: explicit notes, a scanned
// corpus of reference maps, the current map, the game configuration's smart tags and an
// analysis of the texture image.

// Material kinds

/** What kind of texture a material is, which decides how it should be applied. */
enum class MaterialKind
{
  /** Nothing is known. */
  Unknown,
  /** A picture with a border (screen, sign, door): fitted to its face, not repeated. */
  Panel,
  /** A seamless tile (wall, floor): repeats freely, fractional repeats are fine. */
  Tile,
  /** Tiles along one axis only (a strip, a trim): fitted across, repeated along. */
  Trim,
  /** Mostly transparent (a decal, a grate, a sprite-like overlay). */
  Decal,
  Sky,
  Liquid,
  /** A compiler tool material (clip, skip, hint, trigger, origin, ...). */
  Tool,
};

/** "unknown", "panel", "tile", "trim", "decal", "sky", "liquid" or "tool". */
std::string_view toString(MaterialKind kind);

/** The kind with the given name (as returned by toString), or nullopt. */
std::optional<MaterialKind> materialKindFromString(std::string_view name);

/** The names of all kinds but "unknown", for schemas. */
std::vector<std::string> materialKindNames();

/**
 * The lowest-priority fallback: sky if the name contains "sky"; liquid if it starts with
 * '*' or '!' (Quake and Half-Life liquids) or contains "water", "lava" or "slime";
 * nullopt otherwise. Only used when no data source decided the kind.
 */
std::optional<MaterialKind> kindFromName(std::string_view materialName);

/** A kind given by a smart tag of the game configuration. */
struct ConfigKind
{
  MaterialKind kind;
  /** The tag's name. */
  std::string tag;
};

/**
 * The kind given by the game configuration's smart tags. Every face tag of the map's game
 * is tested on a probe face with the material (name and, if loaded, the material itself,
 * so that surfaceparm and flag matchers see its defaults). A matching tag named like
 * "sky" gives sky; like "liquid", "water", "lava" or "slime" gives liquid; a tool name
 * (clip, skip, hint, origin, null, nodraw, caulk, trigger, areaportal, ...) or the tag
 * attribute "transparent" (unless the name contains "trans", like Quake 2's Transparent
 * tag) gives tool. Returns nullopt if no tag decides the kind.
 */
std::optional<ConfigKind> kindFromConfig(
  const mdl::Map& map, std::string_view materialName);

// Face sampling

/** Tolerance in texels for "on a texture edge" and "a whole number of repeats". */
constexpr auto TexelTolerance = 1.0;

/** How a texture lies on one brush face, measured along the face's texture axes. */
struct FaceSample
{
  /** |scale| per axis (world units per texel for unit-length texture axes). */
  vm::vec2d scale;
  /** Whether the scale is negative (the texture is mirrored) per axis. */
  std::array<bool, 2> flipped = {false, false};
  /**
   * The rotation in degrees in [0, 360) as the face shows it (see faceRotation in
   * NodeJson.h: Valve faces derive it from their UV axes).
   */
  double rotation = 0.0;
  /** World units per texel along the U and V texture axes. */
  vm::vec2d texelDensity;
  /** The face's extent along the U and V texture axes in world units. */
  vm::vec2d worldSize;
  /** The face's extent along the U and V texture axes in texels. */
  vm::vec2d texelSize;
  /**
   * The texel coordinate at the face's minimum edge along U and V (the smallest texture
   * coordinate of its vertices), modulo the texture size if it is known. 0 (or the
   * texture size) means the texture starts exactly at that edge.
   */
  vm::vec2d texelStart;
  /** The texture size in pixels, if known. */
  std::optional<vm::vec2d> textureSize;
  /** texelSize / textureSize: how many times the texture repeats, if the size is known.
   */
  std::optional<vm::vec2d> repeats;
  /**
   * Per axis: whether a texture edge coincides (within TexelTolerance) with the face's
   * minimum or maximum edge. False if the texture size is unknown.
   */
  std::array<bool, 2> alignedAxes = {false, false};
  /**
   * Per axis: whether the texture repeats a whole number of times (at least once) within
   * TexelTolerance. False if the texture size is unknown.
   */
  std::array<bool, 2> wholeRepeatAxes = {false, false};

  bool aligned() const { return alignedAxes[0] && alignedAxes[1]; }
  bool wholeRepeats() const { return wholeRepeatAxes[0] && wholeRepeatAxes[1]; }
  /**
   * Whether the surface shows the image exactly once: one whole repeat on both axes,
   * aligned to the surface's edges. False if the texture size is unknown.
   */
  bool showsImageOnce() const;
};

/**
 * The outline and texture mapping of a brush face: what sampling needs, and what decides
 * whether neighbouring faces form one surface.
 */
struct FaceOutline
{
  /** The material name as written. */
  std::string material;
  vm::vec3d normal;
  double distance = 0.0;
  vm::vec3d uAxis;
  vm::vec3d vAxis;
  vm::vec2d scale;
  vm::vec2d offset;
  /** The rotation in degrees as the face shows it. */
  double rotation = 0.0;
  std::vector<vm::vec3d> vertices;
};

/**
 * The outline of the face, or nullopt for faces without geometry, with a zero scale or
 * degenerate texture axes.
 */
std::optional<FaceOutline> faceOutline(const mdl::BrushFace& face);

/**
 * Measures how the texture lies on a surface of one or more faces with the same texture
 * mapping (see mergeSurfaces): the extents are those of all their vertices. The mapping
 * (scale, rotation, texel density) is taken from the first face. Returns nullopt for an
 * empty list.
 */
std::optional<FaceSample> sampleSurface(
  const std::vector<const FaceOutline*>& faces, std::optional<vm::vec2d> textureSize);

/**
 * Per face: whether it is hidden, i.e. lies within a coplanar face of another brush that
 * faces the opposite way (the backs of brushes that touch, which a decompiler textures
 * like the visible faces). Faces with tool or liquid materials hide nothing.
 */
std::vector<bool> hiddenFaces(const std::vector<FaceOutline>& faces);

/**
 * Groups faces into surfaces: faces are joined when they are coplanar, have the same
 * material (case-insensitive) and a continuous texture mapping (the same texture axes and
 * scale, offsets equal modulo the texture size if it is known), and share a piece of an
 * edge. Faces split by a compiler or decompiler become one surface again. Returns the
 * indices of the faces of each surface, in order of their first face.
 */
std::vector<std::vector<size_t>> mergeSurfaces(
  const std::vector<FaceOutline>& faces,
  const std::function<std::optional<vm::vec2d>(const std::string&)>& textureSize);

/** The texture size of the material in pixels, or nullopt if it is not loaded. */
std::optional<vm::vec2d> textureSizeOf(const gl::Material* material);

/**
 * Measures how the texture lies on the face (Standard and Valve faces). The texture size
 * enables repeats and alignment; pass textureSizeOf(face.material()) for faces of a map
 * with loaded materials. Returns nullopt for faces without geometry, with a zero scale
 * or degenerate texture axes.
 */
std::optional<FaceSample> sampleFace(
  const mdl::BrushFace& face, std::optional<vm::vec2d> textureSize);

// Statistics

/**
 * A compact histogram over one- or two-dimensional keys (e.g. a scale pair). Keys are
 * rounded by the caller. The extremes are kept exactly even when entries are capped.
 */
class Histogram
{
public:
  using Key = std::array<double, 2>;

private:
  size_t m_dimensions = 2;
  std::map<Key, uint64_t> m_entries;
  uint64_t m_total = 0;
  Key m_min = {0, 0};
  Key m_max = {0, 0};

public:
  explicit Histogram(size_t dimensions = 2);

  size_t dimensions() const;
  const std::map<Key, uint64_t>& entries() const;
  /** The number of values added (also those merged into other entries by cap). */
  uint64_t total() const;
  bool empty() const;

  void add(Key key, uint64_t count = 1);
  void merge(const Histogram& other);
  /**
   * Keeps the maxEntries most frequent entries and adds the counts of the others to the
   * nearest kept entry, so that percentiles stay approximately right.
   */
  void cap(size_t maxEntries);

  /** The most frequent key (the smallest one on ties), or nullopt if empty. */
  std::optional<Key> mode() const;
  /** The value at the given fraction (0..1) of the distribution along the axis. */
  std::optional<double> percentile(size_t axis, double fraction) const;
  /** The fraction of values along the axis for which the predicate holds. */
  template <typename P>
  double fraction(const size_t axis, const P& predicate) const
  {
    if (m_total == 0)
    {
      return 0.0;
    }
    auto count = uint64_t(0);
    for (const auto& [key, n] : m_entries)
    {
      if (predicate(key[axis]))
      {
        count += n;
      }
    }
    return double(count) / double(m_total);
  }
  /** The smallest / largest value along the axis ever added, or nullopt if empty. */
  std::optional<double> min(size_t axis) const;
  std::optional<double> max(size_t axis) const;

  /** {"e": [[key..., count], ...], "lo": [...], "hi": [...]} */
  Json toJson() const;
  static std::optional<Histogram> fromJson(const Json& json, size_t dimensions);
};

/** The maximum number of entries per histogram kept in a MaterialStats. */
constexpr auto MaxHistogramEntries = size_t(16);

/** Statistics of how one material is used on brush faces. */
struct MaterialStats
{
  /** Sampled faces. */
  uint64_t samples = 0;
  /** Sampled faces whose texture size was known (repeats, alignment). */
  uint64_t sizedSamples = 0;
  /** Sized samples with whole repeats along U, along V, and along both. */
  std::array<uint64_t, 3> wholeRepeats = {0, 0, 0};
  /** Sized samples aligned to a face edge along U, along V, and along both. */
  std::array<uint64_t, 3> aligned = {0, 0, 0};
  /** Sized samples that show the image exactly once (FaceSample::showsImageOnce). */
  uint64_t once = 0;
  /** |scale| per axis, rounded to 0.001. */
  Histogram scale{2};
  /** Face extent in world units along U, V, rounded to 1. */
  Histogram worldSize{2};
  /** Face extent in texels along U, V, rounded to 1. */
  Histogram texelSize{2};
  /** Repeats along U, V (sized samples only), rounded to 0.01. */
  Histogram repeats{2};
  /** Rotation in degrees, rounded to 1. */
  Histogram rotation{1};

  void add(const FaceSample& sample);
  void merge(const MaterialStats& other);
  /** Caps every histogram at MaxHistogramEntries. */
  void cap();
};

/**
 * {"n", "sized", "whole": [u, v, both], "aligned": [u, v, both], "once", "scale", "size",
 * "texels", "repeats", "rotation"} with histograms as in Histogram::toJson ("once" is
 * optional when reading, for files written before it existed).
 */
Json toJson(const MaterialStats& stats);
std::optional<MaterialStats> materialStatsFromJson(const Json& json);

/** Percentiles of the |scale| per axis. */
struct ScaleRange
{
  vm::vec2d min;
  /** 10th percentile. */
  vm::vec2d low;
  /** 90th percentile. */
  vm::vec2d high;
  vm::vec2d max;
};

Json toJson(const ScaleRange& range);

/** What MaterialStats say about a material. */
struct StatsSummary
{
  uint64_t samples = 0;
  /** Samples that give repeats (sized at sampling time, or derived from texel sizes). */
  uint64_t sizedSamples = 0;
  /** The most frequent scale pair. */
  std::optional<vm::vec2d> typicalScale;
  std::optional<ScaleRange> scaleRange;
  /** The most frequent face extent in world units along U, V. */
  std::optional<vm::vec2d> typicalFaceSize;
  /** The most frequent repeats along U, V. */
  std::optional<vm::vec2d> typicalRepeats;
  /** The median repeats along U, V. */
  std::optional<vm::vec2d> medianRepeats;
  /** The fraction of sized samples with whole repeats on both axes, and per axis. */
  std::optional<double> wholeRepeatFraction;
  std::optional<vm::vec2d> wholeRepeatFractionPerAxis;
  /** The fraction of sized samples aligned to a face edge on both axes, and per axis. */
  std::optional<double> alignedFraction;
  std::optional<vm::vec2d> alignedFractionPerAxis;
  /** The fraction of sized samples with at most 1.05 repeats, per axis. */
  std::optional<vm::vec2d> singleRepeatFractionPerAxis;
  /**
   * The fraction of sized samples that show the image exactly once (unknown when the
   * repeats were derived from texel extents).
   */
  std::optional<double> onceFraction;
  /** The most frequent rotation. */
  std::optional<double> typicalRotation;
};

/**
 * Summarizes the statistics. If none of the samples had a known texture size but one is
 * given now, repeats and whole repeats are derived from the texel extents (alignment
 * stays unknown).
 */
StatsSummary summarize(
  const MaterialStats& stats, std::optional<vm::vec2d> textureSize = std::nullopt);

Json toJson(const StatsSummary& summary);

/** The minimum number of sized samples before statistics decide a kind. */
constexpr auto MinKindSamples = uint64_t(4);

/**
 * The kind that the statistics suggest (needs MinKindSamples sized samples):
 * - panel: whole repeats on both axes in >= 75% of the samples, median repeats <= 2 on
 *   both axes, and (if known) aligned to a face edge in >= 75%; or at least half of the
 *   samples show the image exactly once (the texture's own evidence: a tile rarely fits
 *   its surface exactly);
 * - trim: along one axis >= 75% of the samples have at most 1.05 repeats and (if known)
 *   are aligned, along the other axis fewer than 50% do;
 * - tile: whole repeats in fewer than 60% of the samples, or median repeats > 2;
 * - nullopt otherwise.
 */
std::optional<MaterialKind> kindFromStats(const StatsSummary& summary);

// Image analysis

/** A game-independent analysis of a texture image. */
struct ImageAnalysis
{
  size_t width = 0;
  size_t height = 0;
  /** width / height. */
  double aspectRatio = 1.0;
  /**
   * The mean color difference (0..1) between the left and right column (x) and between
   * the top and bottom row (y), i.e. across the seam when the texture repeats.
   */
  vm::vec2d edgeDifference;
  /** The mean color difference between adjacent columns (x) and adjacent rows (y). */
  vm::vec2d innerDifference;
  /** Whether the texture repeats without a visible seam along U / V. */
  bool tilesU = false;
  bool tilesV = false;
  /** The fraction of pixels with alpha < 128. */
  double transparentFraction = 0.0;
  /** decal, tile, trim or panel. */
  MaterialKind suggestedKind = MaterialKind::Unknown;
};

/**
 * Analyzes an image: an axis tiles if its edge difference is at most 1.5 times the inner
 * difference plus 0.03 (so noisy tiles count as seamless). Suggested kind: decal if at
 * least 25% of the pixels are transparent; else tile if both axes tile; trim if only one
 * axis tiles or the image is at least 4 times longer than wide; panel otherwise.
 */
ImageAnalysis analyzeImage(const RgbaImage& image);

Json toJson(const ImageAnalysis& analysis);

// Knowledge notes

/** Explicit facts about a material, written by the agent or the user. */
struct MaterialNote
{
  /** The material name as written. */
  std::string name;
  std::optional<MaterialKind> kind;
  /** The scale to use per axis. */
  std::optional<vm::vec2d> scale;
  /** The face size (world units along U, V) the material is made for. */
  std::optional<vm::vec2d> faceSize;
  std::optional<std::string> text;
  /** ISO time of the last change. */
  std::string updated;

  bool empty() const { return !kind && !scale && !faceSize && !text; }
};

/** {"material", "kind"?, "scale"?, "faceSize"?, "text"?, "updated"} */
Json toJson(const MaterialNote& note);
std::optional<MaterialNote> materialNoteFromJson(const Json& json);

/** The notes of one knowledge folder (notes.json), keyed by lower-case material name. */
struct NotesFile
{
  std::map<std::string, MaterialNote> notes;
};

Json toJson(const NotesFile& notes);
Result<NotesFile, std::string> notesFileFromJson(const Json& json);

// Reference corpus

/** The corpus statistics of one knowledge folder (corpus.json). */
struct CorpusFile
{
  struct Entry
  {
    /** The material name as first seen. */
    std::string name;
    /** The texture size known when the material was scanned. */
    std::optional<vm::vec2d> textureSize;
    MaterialStats stats;
  };

  std::string game;
  std::optional<std::string> mod;
  /** The scanned folders (several after merges). */
  std::vector<std::string> folders;
  /** The scanned files. */
  std::vector<std::string> files;
  std::string scannedAt;
  uint64_t faces = 0;
  /** Keyed by lower-case material name. */
  std::map<std::string, Entry> materials;

  /** Adds the other corpus: statistics of the same material are merged. */
  void merge(const CorpusFile& other);
};

Json toJson(const CorpusFile& corpus);
Result<CorpusFile, std::string> corpusFileFromJson(const Json& json);

// Knowledge store

/** The folder name used for the game itself (no mod). */
constexpr auto GameScopeFolder = std::string_view{"_game"};

/**
 * Where the knowledge of a map's game and mod lives:
 * `<knowledge>/<game>/<mod or _game>/`, with corpus.json and notes.json.
 */
struct KnowledgeScope
{
  std::string game;
  /** The document's most specific enabled mod, or nullopt for the game itself. */
  std::optional<std::string> mod;
  /** `<knowledge>/<game>/_game`: game-level notes and corpus. */
  std::filesystem::path gameDirectory;
  /** `<knowledge>/<game>/<mod>`, or gameDirectory if there is no mod. */
  std::filesystem::path directory;

  std::filesystem::path corpusPath() const;
  std::filesystem::path notesPath() const;
  std::filesystem::path gameCorpusPath() const;
  std::filesystem::path gameNotesPath() const;
};

/** {"game", "mod", "path"} */
Json toJson(const KnowledgeScope& scope);

/**
 * Replaces characters that are not letters, digits, '.', '-', '_' or ' ' by '_', and
 * names that consist of dots only; an empty name becomes "_".
 */
std::string sanitizeFolderName(std::string_view name);

KnowledgeScope knowledgeScope(
  const mdl::Map& map, const std::filesystem::path& knowledgeDirectory);

/**
 * Reads a corpus / notes file. Returns nullptr if the file does not exist, an error
 * message if it cannot be read or parsed. Parsed files are cached in memory by path, last
 * write time and size (main thread only), so repeated calls do not parse them again.
 */
Result<std::shared_ptr<const CorpusFile>, std::string> readCorpusFile(
  const std::filesystem::path& path);
Result<std::shared_ptr<const NotesFile>, std::string> readNotesFile(
  const std::filesystem::path& path);

/**
 * Writes the file (creating its folder; via a temporary file that replaces the old one)
 * and updates the cache. Fails with IO_ERROR.
 */
Result<void, ToolError> writeCorpusFile(
  const std::filesystem::path& path, const CorpusFile& corpus);
Result<void, ToolError> writeNotesFile(
  const std::filesystem::path& path, const NotesFile& notes);

// Material profiles

/** A value with where it came from. */
template <typename T>
struct Sourced
{
  T value;
  /** "notes", "corpus", "map", "image", "config" or "name". */
  std::string source;
  /** The number of samples behind the value (0 for notes, config, image and name). */
  uint64_t samples = 0;
};

/** Everything known about how a material should be applied. */
struct MaterialProfile
{
  /** The loaded material's spelling, else the name as requested. */
  std::string name;
  bool loaded = false;
  std::optional<vm::vec2d> textureSize;
  /** Brush faces using the material in the current map. */
  uint64_t mapUsage = 0;
  /** Unknown with source "none" if nothing decided it. */
  Sourced<MaterialKind> kind = {MaterialKind::Unknown, "none", 0};
  /** Notes > corpus > map > the game's default scale ("config"). */
  std::optional<Sourced<vm::vec2d>> typicalScale;
  /** Corpus > map. */
  std::optional<Sourced<ScaleRange>> scaleRange;
  /** World units per texel per axis; follows typicalScale. */
  std::optional<Sourced<vm::vec2d>> texelDensity;
  /** Notes > corpus > map. */
  std::optional<Sourced<vm::vec2d>> typicalFaceSize;
  /** Corpus > map. */
  std::optional<Sourced<vm::vec2d>> typicalRepeats;
  std::optional<Sourced<double>> wholeRepeatFraction;
  std::optional<Sourced<double>> alignedFraction;
  /** The analysis of the texture image, if it was needed or requested and loadable. */
  std::optional<ImageAnalysis> image;
  /** Why the image could not be analyzed, if it was needed or requested. */
  std::optional<std::string> imageError;
  /** The effective note (mod-level notes replace game-level notes). */
  std::optional<MaterialNote> note;
  /** "game" or "mod". */
  std::optional<std::string> noteScope;
  /** The smart tag that marks the material (sky, liquid, tool), if any. */
  std::optional<std::string> configTag;
  /** Summaries of the corpus and current-map statistics, if any. */
  std::optional<StatsSummary> corpusStats;
  std::optional<StatsSummary> mapStats;
};

/**
 * {"name", "loaded", "textureSize", "mapUsage", "kind": {value, source, samples},
 * "typicalScale", "scaleRange", "texelDensity", "typicalFaceSize", "typicalRepeats",
 * "wholeRepeatFraction", "alignedFraction" (each {value, source, samples} or null),
 * "image", "note", "configTag"}; with includeStatistics also "statistics": {corpus, map}.
 */
Json toJson(const MaterialProfile& profile, bool includeStatistics = false);

/**
 * Builds material profiles for one map, merging (highest priority first) notes, the
 * game's smart tags (sky, liquid and tool kinds), the reference corpus, statistics of the
 * current map, a name fallback for sky and liquid, and image analysis.
 *
 * Create one instance per tool call: current-map statistics are computed once over all
 * brush faces when first needed, knowledge files are read once (and cached across
 * instances), images are loaded lazily and once per material with one game file system.
 * The map must outlive the instance.
 */
class MaterialKnowledge
{
private:
  const mdl::Map* m_map;
  std::optional<KnowledgeScope> m_scope;
  std::vector<std::string> m_problems;

  std::shared_ptr<const NotesFile> m_gameNotes;
  std::shared_ptr<const NotesFile> m_modNotes;
  std::shared_ptr<const CorpusFile> m_gameCorpus;
  std::shared_ptr<const CorpusFile> m_modCorpus;

  std::optional<std::unordered_map<std::string, MaterialStats>> m_mapStats;
  std::unordered_map<std::string, uint64_t> m_mapUsage;
  std::unique_ptr<MaterialImageLoader> m_imageLoader;
  std::unordered_map<std::string, MaterialProfile> m_profiles;

public:
  /**
   * Without a knowledge directory, profiles come only from the map, the game
   * configuration, names and images.
   */
  MaterialKnowledge(
    const mdl::Map& map, std::optional<std::filesystem::path> knowledgeDirectory);
  ~MaterialKnowledge();

  MaterialKnowledge(MaterialKnowledge&&) noexcept;
  MaterialKnowledge& operator=(MaterialKnowledge&&) noexcept;

  /** The knowledge folders of the map's game and mod, or nullopt without a directory. */
  const std::optional<KnowledgeScope>& scope() const;

  /**
   * Knowledge files that could not be read or parsed (they are ignored), e.g. for a
   * KNOWLEDGE_FILE_INVALID warning.
   */
  const std::vector<std::string>& problems() const;

  /**
   * The profile of a material (case-insensitive), computed once. The image is analyzed
   * when no other source decides the kind, or always with withImage.
   */
  const MaterialProfile& profile(std::string_view materialName, bool withImage = false);

  /** The statistics of the material in the current map, or nullptr if it is unused. */
  const MaterialStats* mapStats(std::string_view materialName);

  /** The materials used on brush faces of the map with their face counts, most used
   * first. */
  std::vector<std::pair<std::string, uint64_t>> mapUsage();

  /** The effective note of a material and its scope ("game" or "mod"). */
  std::optional<std::pair<MaterialNote, std::string>> note(
    std::string_view materialName) const;

  /** The corpus entry of a material: the mod's corpus first, then the game's. */
  const CorpusFile::Entry* corpusEntry(std::string_view materialName) const;

private:
  void computeMapStats();
  void analyzeImage(MaterialProfile& profile);
};

} // namespace tb::mcp
