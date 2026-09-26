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

#include "mcp/tools/ViewTools.h"

#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/ToolRegistry.h"
#include "mdl/Grid.h"
#include "mdl/Map.h"

#include <cmath>
#include <optional>
#include <string>

namespace tb::mcp
{
namespace
{
using namespace schema;

struct GridState
{
  int exponent;
  bool visible;
  bool snap;
};

GridState gridState(const mdl::Grid& grid)
{
  return {grid.size(), grid.visible(), grid.snap()};
}

Json toJson(const GridState& state)
{
  const auto size = mdl::Grid::actualSize(state.exponent);
  return Json{
    {"size", size},
    {"exponent", state.exponent},
    {"effectiveSize", state.snap ? size : 1.0},
    {"visible", state.visible},
    {"snap", state.snap},
    {"angle", 15.0},
  };
}

std::string validSizes()
{
  auto result = std::string{};
  for (auto exponent = mdl::Grid::MinSize; exponent <= mdl::Grid::MaxSize; ++exponent)
  {
    const auto size = mdl::Grid::actualSize(exponent);
    auto text = std::to_string(size);
    text.erase(text.find_last_not_of('0') + 1);
    if (text.back() == '.')
    {
      text.pop_back();
    }
    result += (result.empty() ? "" : ", ") + text;
  }
  return result;
}

Schema gridSchema()
{
  return object({
    field("size", number()).required().describe("Grid size in map units"),
    field("exponent", integer()).required().describe("size = 2^exponent"),
    field("effectiveSize", number())
      .required()
      .describe("The size tools snap to: size, or 1 if snapping is off"),
    field("visible", boolean()).required(),
    field("snap", boolean()).required(),
    field("angle", number()).required().describe("Rotation snap angle in degrees"),
  });
}

ToolResult gridGet(CallContext& context, const Args&)
{
  return toJson(gridState(context.map().grid()));
}

ToolResult gridSet(CallContext& context, const Args& args)
{
  auto& grid = context.map().grid();
  const auto size = args.getOptional<double>("size");
  const auto exponent = args.getOptional<int>("exponent");
  const auto visible = args.getOptional<bool>("visible");
  const auto snap = args.getOptional<bool>("snap");

  if (size && exponent)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Pass either 'size' or 'exponent', not both.",
      "E.g. {\"size\": 16} or {\"exponent\": 4}.");
  }
  if (!size && !exponent && !visible && !snap)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Nothing to change.",
      "Pass at least one of 'size', 'exponent', 'visible' and 'snap'.");
  }

  const auto previous = gridState(grid);
  auto next = previous;
  if (size)
  {
    const auto e = std::log2(*size);
    if (
      !(*size > 0.0) || std::abs(e - std::round(e)) > 1e-9 || e < mdl::Grid::MinSize
      || e > mdl::Grid::MaxSize)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "Grid size " + std::to_string(*size) + " is not a valid grid size.",
        "Use a power of two: " + validSizes() + ".");
    }
    next.exponent = int(std::round(e));
  }
  if (exponent)
  {
    next.exponent = *exponent;
  }
  next.visible = visible.value_or(previous.visible);
  next.snap = snap.value_or(previous.snap);

  if (!context.dryRun())
  {
    if (next.exponent != previous.exponent)
    {
      grid.setSize(next.exponent);
    }
    if (next.visible != grid.visible())
    {
      grid.toggleVisible();
    }
    if (next.snap != grid.snap())
    {
      grid.toggleSnap();
    }
  }

  auto result = toJson(next);
  result["previous"] = toJson(previous);
  return result;
}

} // namespace

void registerViewTools(ToolRegistry& registry)
{
  registry.add(
    ToolDef{"grid_get"}
      .title("Get Grid")
      .description(
        "Returns the grid of the document: size in map units (a power of two from 0.125 "
        "to 256), exponent (size = 2^exponent), whether it is visible and whether "
        "snapping is on, and the rotation snap angle. Example: {}")
      .input(object({}))
      .output(gridSchema())
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(gridGet));

  registry.add(
    ToolDef{"grid_set"}
      .title("Set Grid")
      .description(
        "Changes the grid of the document, like the View > Grid menu: 'size' in map "
        "units "
        "(0.125, 0.25, ..., 256) or 'exponent' (-3 to 8), and whether the grid is "
        "'visible' and 'snap'ping is on. Not undoable. Returns the new and the previous "
        "grid. Example: {\"size\": 16} or {\"snap\": false}")
      .input(object({
        field("size", number()).describe("Grid size in map units, a power of two"),
        field("exponent", integer().min(mdl::Grid::MinSize).max(mdl::Grid::MaxSize))
          .describe("Alternative to size: size = 2^exponent"),
        field("visible", boolean()).describe("Show the grid"),
        field("snap", boolean()).describe("Snap to the grid"),
      }))
      .output(object({
        field("size", number()).required(),
        field("exponent", integer()).required(),
        field("effectiveSize", number()).required(),
        field("visible", boolean()).required(),
        field("snap", boolean()).required(),
        field("angle", number()).required(),
        field("previous", gridSchema()).required(),
      }))
      .mutation(Mutation::External)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(gridSet));
}

} // namespace tb::mcp
