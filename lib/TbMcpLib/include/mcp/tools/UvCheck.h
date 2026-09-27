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

#include "mcp/Json.h"
#include "mcp/tools/MaterialKnowledge.h"
#include "mdl/BrushFaceHandle.h"

#include "vm/vec.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tb::mdl
{
class Map;
}

namespace tb::mcp
{
class IdRegistry;

// UV quality checks (E11): texturing problems measured against the material profiles
// (MaterialKnowledge.h), for any game.

/** A kind of texturing problem; the finding code is the UPPER_SNAKE name. */
enum class UvIssue
{
  /** The texels are stretched: |scale U| / |scale V| is off the material's ratio. */
  AspectDistortion,
  /** A panel (or a trim across its short axis) does not repeat a whole number of times.
   */
  FractionalRepeat,
  /** A panel's texture edges do not lie on the face's edges. */
  PanelNotAligned,
  /** The scale is far outside the material's typical range. */
  UnusualScale,
  /** The texel density differs much from a neighbouring face. */
  TexelDensityMismatch,
  /** A coplanar neighbour with the same material does not continue the texture. */
  Seam,
};

/**
 * "UV_ASPECT_DISTORTION", "UV_FRACTIONAL_REPEAT", "UV_PANEL_NOT_ALIGNED",
 * "UV_UNUSUAL_SCALE", "UV_TEXEL_DENSITY_MISMATCH" or "UV_SEAM".
 */
std::string_view toString(UvIssue issue);
std::optional<UvIssue> uvIssueFromString(std::string_view code);
/** All finding codes, for schemas. */
std::vector<std::string> uvIssueCodes();

// Thresholds

/** The texel aspect ratio may deviate this much (10%) from the expected ratio. */
constexpr auto MaxAspectDeviation = 0.10;
/**
 * A scale is unusual if it is more than this factor below the low end (10th percentile)
 * or above the high end (90th percentile) of the material's scale range; for a scale
 * from notes, the range is that scale.
 */
constexpr auto UnusualScaleFactor = 1.25;
/** Corpus statistics need this many samples to define a typical scale range. */
constexpr auto MinCorpusScaleSamples = uint64_t(5);
/**
 * Statistics of the current map need this many samples to define a typical scale range,
 * a scale ratio or a kind for the checks: the checked faces are part of them.
 */
constexpr auto MinMapSamples = uint64_t(20);
/** Neighbouring faces whose mean texel density differs by more than this factor. */
constexpr auto DensityMismatchFactor = 1.5;

/** The texel aspect ratio (U density / V density) a material is expected to have. */
struct ExpectedAspect
{
  double ratio = 1.0;
  /** "notes" or "corpus" (their typical scale), or "default" (square texels). */
  std::string source = "default";
};

/**
 * The typical scale's U / V ratio from notes or the corpus; square texels otherwise. The
 * current map's statistics are not used: the checked faces shape them.
 */
ExpectedAspect expectedAspect(const MaterialProfile& profile);

/**
 * The kind the checks use: the profile's kind, except that a kind from the current map
 * with fewer than MinMapSamples samples gives way to the image analysis (if the profile
 * has one), since the checked faces shape the map's statistics.
 */
MaterialKind checkedKind(const MaterialProfile& profile);

/**
 * For a trim: the texture axis (0 = U, 1 = V) across the strip, i.e. the texture's short
 * axis, or for a square texture the axis that does not tile according to the image
 * analysis. nullopt if unknown.
 */
std::optional<size_t> trimAcrossAxis(const MaterialProfile& profile);

/** A suggested tool call that fixes a finding. */
struct UvFix
{
  std::string tool;
  /**
   * The arguments without the face: toJson(UvFinding) adds "ids" (or "face" for
   * material_fit_geometry) with the finding's face.
   */
  Json arguments = Json::object();
  /** Whether toJson adds "source": the neighbour face (face_attributes_copy). */
  bool neighbourAsSource = false;
  std::string description;
};

struct UvFinding
{
  UvIssue issue;
  mdl::BrushFaceHandle face;
  /** The neighbour face involved (density mismatch, seam). */
  std::optional<mdl::BrushFaceHandle> neighbour;
  std::string material;
  std::string message;
  /** The measured values, e.g. {"aspect", "expectedAspect", "deviation"}. */
  Json measured = Json::object();
  /** The suggested fix first, then alternatives. */
  std::vector<UvFix> fixes;
};

/**
 * {"code", "face", "brush", "material", "message", "measured", "neighbour"?,
 * "fix": {"tool", "arguments", "description"}, "alternatives": [...]}
 */
Json toJson(const UvFinding& finding, const IdRegistry& ids);

/** Returns the profile of a material (by name, case-insensitive). */
using ProfileProvider = std::function<const MaterialProfile&(const std::string&)>;

/**
 * Profiles from the knowledge; profiles whose kind comes from few current-map samples
 * get an image analysis (see checkedKind).
 */
ProfileProvider profileProvider(MaterialKnowledge& knowledge);

/**
 * Checks how the textures lie on the given faces and returns the findings about them
 * (grouped by face, in the order of the faces):
 * - UV_ASPECT_DISTORTION: the texel density ratio U / V deviates from expectedAspect by
 *   more than MaxAspectDeviation.
 * - UV_FRACTIONAL_REPEAT: panels (both axes) and trims (across, see trimAcrossAxis)
 *   whose texture does not repeat a whole number of times (at least once) within
 *   TexelTolerance.
 * - UV_PANEL_NOT_ALIGNED: panels whose texture edges are not on the face edges.
 * - UV_UNUSUAL_SCALE: |scale| more than UnusualScaleFactor outside the scale range from
 *   notes, the corpus (MinCorpusScaleSamples) or the map (MinMapSamples).
 * - UV_TEXEL_DENSITY_MISMATCH: tiles (and unknown kinds) whose mean texel density differs
 *   by more than DensityMismatchFactor from a neighbouring tile face (faces of the same
 *   brush sharing an edge, coplanar faces of other brushes sharing an edge); reported
 *   once per face (the worst neighbour), and for a pair of checked faces on the one
 *   whose scale is further from its material's typical scale.
 * - UV_SEAM: coplanar neighbours of other brushes with the same material (tiles, trims,
 *   unknown kinds) whose texture does not continue across the shared edge: a different
 *   scale or rotation, or an offset mismatch of more than TexelTolerance modulo the
 *   texture size; reported once per pair.
 * Tool, sky and liquid materials and faces without a material are skipped; checks that
 * need the texture size (repeats, alignment, seams) skip materials that are not loaded.
 * Neighbours are found through the world's node tree. `only` limits the checks (empty:
 * all).
 */
std::vector<UvFinding> checkUv(
  const std::vector<mdl::BrushFaceHandle>& faces,
  const mdl::Map& map,
  const ProfileProvider& profiles,
  const std::vector<UvIssue>& only = {});

} // namespace tb::mcp
