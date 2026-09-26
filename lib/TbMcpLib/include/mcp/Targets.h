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
#include "mcp/ObjectIds.h"
#include "mcp/Schema.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace tb::mdl
{
class BrushFaceHandle;
class Node;
} // namespace tb::mdl

namespace tb::mcp
{
class Args;
class CallContext;

/**
 * The standard `ids` parameter: explicit object ids; when omitted, the tool acts on the
 * current selection (spec X7).
 */
schema::Field idsField(
  std::vector<ObjectKind> kinds = {},
  std::string description = "Object ids to act on. Default: the current selection");

/**
 * Resolves the targets of a call: the objects named by the given argument, or the current
 * selection if the argument is absent. Fails with NO_SELECTION if neither yields any
 * object, with OBJECT_NOT_FOUND / WRONG_OBJECT_KIND for bad ids, and with
 * OBJECT_NOT_EDITABLE for hidden or locked objects or objects in a closed group.
 */
Result<std::vector<mdl::Node*>, ToolError> resolveTargets(
  CallContext& context,
  const Args& args,
  std::string_view key = "ids",
  const std::vector<ObjectKind>& kinds = {});

/** Resolves a face id such as `brush:1042/face:3`. */
Result<mdl::BrushFaceHandle, ToolError> resolveFace(
  CallContext& context, std::string_view id);

enum class SelectionAfter
{
  /** Restore the selection the human had before the call. */
  Restore,
  /** Leave the targets (or whatever the operation selected) selected. */
  Result,
};

/**
 * Selects the given targets, runs the function, and restores the previous selection
 * (dropping removed objects) unless `after` is SelectionAfter::Result. Most Map_*
 * functions act on the selection; this makes tools independent of it. The selection
 * changes are part of the call's undo step.
 */
ToolResult withTargets(
  CallContext& context,
  const std::vector<mdl::Node*>& targets,
  const std::function<ToolResult()>& function,
  SelectionAfter after = SelectionAfter::Restore);

} // namespace tb::mcp
