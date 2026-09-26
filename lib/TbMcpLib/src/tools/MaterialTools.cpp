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

#include "mcp/tools/MaterialTools.h"

#include "base/PreferenceManager.h"
#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/ToolRegistry.h"
#include "prefs/Preferences.h"

#include <string>

namespace tb::mcp
{
namespace
{
using namespace schema;

Json currentLocks()
{
  return Json{
    {"alignmentLock", pref(Preferences::AlignmentLock)},
    {"uvLock", pref(Preferences::UvLock)},
  };
}

Schema locksSchema()
{
  return object({
    field("alignmentLock", boolean())
      .required()
      .describe("Texture lock: transforms keep the texture alignment on brush faces"),
    field("uvLock", boolean())
      .required()
      .describe("UV lock: vertex editing keeps the UV coordinates"),
  });
}

ToolResult locksGet(CallContext&, const Args&)
{
  return currentLocks();
}

ToolResult locksSet(CallContext& context, const Args& args)
{
  const auto alignmentLock = args.getOptional<bool>("alignmentLock");
  const auto uvLock = args.getOptional<bool>("uvLock");
  if (!alignmentLock && !uvLock)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Nothing to change.",
      "Pass 'alignmentLock', 'uvLock' or both.");
  }

  const auto previous = currentLocks();
  auto result = previous;
  if (alignmentLock)
  {
    result["alignmentLock"] = *alignmentLock;
  }
  if (uvLock)
  {
    result["uvLock"] = *uvLock;
  }

  if (!context.dryRun())
  {
    // MapDocument applies the preferences to the editor context of every open document
    if (alignmentLock)
    {
      setPref(Preferences::AlignmentLock, *alignmentLock);
    }
    if (uvLock)
    {
      setPref(Preferences::UvLock, *uvLock);
    }
  }

  result["previous"] = previous;
  return result;
}

} // namespace

void registerMaterialTools(ToolRegistry& registry)
{
  registry.add(
    ToolDef{"locks_get"}
      .title("Get Locks")
      .description(
        "Returns the texture lock ('alignmentLock': moving, rotating or scaling brushes "
        "keeps their texture alignment) and the UV lock ('uvLock': vertex editing keeps "
        "UV coordinates). Both are editor preferences shared by all documents. "
        "Example: {}")
      .input(object({}))
      .output(locksSchema())
      .mutation(Mutation::None)
      .idempotent()
      .handler(locksGet));

  registry.add(
    ToolDef{"locks_set"}
      .title("Set Locks")
      .description(
        "Turns the texture lock (alignmentLock) and/or the UV lock (uvLock) on or off, "
        "like the toolbar buttons. These are editor preferences for all documents and "
        "are "
        "not undoable. Transform tools also accept a per-call 'alignmentLock' override. "
        "Returns the new and the previous values. Example: {\"alignmentLock\": false}")
      .input(object({
        field("alignmentLock", boolean()).describe("Texture lock"),
        field("uvLock", boolean()).describe("UV lock"),
      }))
      .output(object({
        field("alignmentLock", boolean()).required(),
        field("uvLock", boolean()).required(),
        field("previous", locksSchema()).required(),
      }))
      .mutation(Mutation::External)
      .idempotent()
      .handler(locksSet));
}

} // namespace tb::mcp
