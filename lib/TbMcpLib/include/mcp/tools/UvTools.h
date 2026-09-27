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
#include "mcp/Json.h"
#include "mcp/tools/MaterialKnowledge.h"
#include "mcp/tools/UvCheck.h"

#include "vm/vec.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tb::mdl
{
class BrushFace;
class BrushFaceHandle;
class Node;
struct UpdateBrushFaceAttributes;
} // namespace tb::mdl

namespace tb::mcp
{
class Args;
class CallContext;
class IdRegistry;
class ToolRegistry;

// uv_check findings as warnings of the material and UV tools (E11.7)

/** At most this many UV findings are added as warnings to a tool result. */
constexpr auto MaxUvWarnings = size_t(10);

/**
 * Runs checkUv on the given faces (after a tool changed them) with the call's material
 * knowledge and adds each finding as a warning: code = the finding code, the message
 * with the suggested fix, objectIds = the face (and the neighbour). After MaxUvWarnings
 * findings, one UV_CHECK_MORE warning counts the rest and points to uv_check.
 * Precondition: context.hasDocument()
 */
void warnUvFindings(CallContext& context, const std::vector<mdl::BrushFaceHandle>& faces);

// uv_align fit with a kept aspect ratio and the "typical" operation (E11.8)

/** The repeats of uv_align "fit" (without trimSheet). */
struct UvFitRequest
{
  std::optional<double> repeatU;
  std::optional<double> repeatV;
  /** The axis without a repeat count follows with the material's texel aspect ratio. */
  bool keepAspect = false;
  /** Rounds the repeats (given and following) to whole numbers, at least 1. */
  bool round = false;
};

/**
 * Reads repeatU, repeatV, keepAspect and round. Without repeats: 1 x 1, or repeatU 1 with
 * keepAspect. Given repeats are rounded with round. Fails with INVALID_ARGUMENT if
 * keepAspect comes with both repeats.
 */
Result<UvFitRequest, ToolError> uvFitRequest(const Args& args);

/** What the fit operations need to know about the materials of the faces. */
struct UvFitProfiles
{
  /** Keyed by lower-case material name. */
  std::map<std::string, ExpectedAspect> aspects;
  /**
   * The scale "typical" applies: notes > corpus > the current map (only if faces other
   * than the fitted ones use the material) > the game's default ("config").
   */
  std::map<std::string, Sourced<vm::vec2d>> typicalScales;
};

/**
 * The profiles of the faces' materials. With warnDefaultScales, warns with
 * TYPICAL_SCALE_DEFAULT for materials without notes, corpus or map data.
 */
UvFitProfiles uvFitProfiles(
  CallContext& context,
  const std::vector<mdl::BrushFaceHandle>& faces,
  bool warnDefaultScales);

/**
 * keepAspect fit: the given axis repeats as requested, the other axis gets the texel
 * density that keeps the material's aspect ratio (square texels by default), optionally
 * rounded to whole repeats; the texture is justified to the face's edge like fit. nullopt
 * if the face has no extent along an axis or the material is not loaded.
 */
std::optional<mdl::UpdateBrushFaceAttributes> aspectFitUpdate(
  const mdl::BrushFace& face, const UvFitRequest& request, const UvFitProfiles& profiles);

/**
 * "typical": the material's typical scale (keeping the signs of the current scale),
 * justified to the face's edge like fit. nullopt if the material is not loaded.
 */
std::optional<mdl::UpdateBrushFaceAttributes> typicalFitUpdate(
  const mdl::BrushFace& face, const UvFitProfiles& profiles);

/**
 * The material's typical scale adjusted to whole repeats (at least one) on both axes,
 * keeping the signs of the current scale, justified to the face's edge. nullopt if the
 * material is not loaded or the face has no extent.
 */
std::optional<mdl::UpdateBrushFaceAttributes> wholeRepeatsFitUpdate(
  const mdl::BrushFace& face, const UvFitProfiles& profiles);

/**
 * World-aligned (paraxial) UV axes at the material's typical scale, offset 0 and rotation
 * 0, so that the textures of adjacent faces continue across them. nullopt if the material
 * is not loaded.
 */
std::optional<mdl::UpdateBrushFaceAttributes> worldAlignedUpdate(
  const mdl::BrushFace& face, const UvFitProfiles& profiles);

/**
 * Applies an update computed per face in one command for all faces, without changing
 * the selection; faces for which the function returns nothing are left unchanged.
 * Fails with OPERATION_FAILED if an update is invalid (e.g. scale 0).
 */
ToolResult applyUvUpdates(
  CallContext& context,
  const std::vector<mdl::BrushFaceHandle>& faces,
  const std::function<
    std::optional<mdl::UpdateBrushFaceAttributes>(const mdl::BrushFace&)>& updateFor);

/** How the faces of new objects are aligned (the `uv` argument of the import tools). */
enum class UvMode
{
  /** The UVs of the source. */
  Keep,
  /** uv_align typical: the material's typical scale, justified to each face's edge. */
  Typical,
  /** The typical scale rounded to whole repeats per face, justified. */
  Fit,
  /** World-aligned axes at the typical scale: textures continue across brushes. */
  World,
};

std::optional<UvMode> uvModeFromString(std::string_view name);

/**
 * Aligns all brush faces of the given nodes (and their descendants) with the given mode
 * in one command. Faces whose material is not loaded keep their UVs
 * (MATERIAL_NOT_LOADED); materials with only the game's default scale warn with
 * TYPICAL_SCALE_DEFAULT. Returns {mode, faces, skipped, typicalScales: {material:
 * {scale, source}}}.
 */
Result<Json, ToolError> alignNodeUvs(
  CallContext& context, const std::vector<mdl::Node*>& nodes, UvMode mode);

/**
 * [{"id", "material", "scale", "repeats", "typicalScale"?}] for the first 50 faces:
 * the resulting scale and repeats; with profiles, the applied typical scale with its
 * source.
 */
Json uvFitReport(
  const std::vector<mdl::BrushFaceHandle>& faces,
  const IdRegistry& ids,
  const UvFitProfiles* typicalProfiles);

/**
 * Registers the tools uv_check and material_fit_geometry.
 */
void registerUvTools(ToolRegistry& registry);

} // namespace tb::mcp
