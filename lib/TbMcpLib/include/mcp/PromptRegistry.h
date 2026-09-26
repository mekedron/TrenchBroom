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

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tb::mcp
{

struct PromptArgument
{
  std::string name;
  std::string description;
  bool required = false;
};

using PromptArguments = std::map<std::string, std::string>;

/**
 * Produces the `messages` array of a `prompts/get` result, e.g.
 * `[{"role": "user", "content": {"type": "text", "text": ...}}]`.
 */
using PromptGetter = std::function<Result<Json, ToolError>(const PromptArguments&)>;

struct PromptDef
{
  std::string name;
  std::string title;
  std::string description;
  std::vector<PromptArgument> arguments;
  PromptGetter get;
};

/** Returns a single user message with the given text. */
Json userTextMessage(const std::string& text);

class PromptRegistry
{
private:
  std::vector<PromptDef> m_prompts;

public:
  /** Precondition: the name is not yet registered. */
  void add(PromptDef prompt);

  const PromptDef* find(std::string_view name) const;
  const std::vector<PromptDef>& prompts() const;

  /** A `prompts/list` result. */
  Json list(
    std::string_view protocolVersion,
    const std::optional<std::string>& cursor,
    size_t pageSize = 100) const;

  /**
   * A `prompts/get` result. Fails with INVALID_ARGUMENT if the prompt does not exist or
   * a required argument is missing.
   */
  Result<Json, ToolError> get(
    std::string_view name, const PromptArguments& arguments) const;
};

} // namespace tb::mcp
