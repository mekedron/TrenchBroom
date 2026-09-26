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

#include "mcp/PromptRegistry.h"

#include "mcp/Pagination.h"
#include "mcp/ProtocolVersion.h"

#include "kd/contracts.h"

#include <algorithm>

namespace tb::mcp
{

Json userTextMessage(const std::string& text)
{
  return Json::array({Json{
    {"role", "user"},
    {"content", Json{{"type", "text"}, {"text", text}}},
  }});
}

void PromptRegistry::add(PromptDef prompt)
{
  contract_pre(find(prompt.name) == nullptr);
  contract_pre(prompt.get != nullptr);

  m_prompts.push_back(std::move(prompt));
}

const PromptDef* PromptRegistry::find(const std::string_view name) const
{
  const auto it = std::ranges::find_if(
    m_prompts, [&](const auto& prompt) { return prompt.name == name; });
  return it != m_prompts.end() ? &*it : nullptr;
}

const std::vector<PromptDef>& PromptRegistry::prompts() const
{
  return m_prompts;
}

Json PromptRegistry::list(
  const std::string_view protocolVersion,
  const std::optional<std::string>& cursor,
  const size_t pageSize) const
{
  auto offset = size_t{0};
  if (cursor)
  {
    if (const auto decoded = decodeCursor(*cursor))
    {
      offset = decoded->offset;
    }
  }

  auto prompts = Json::array();
  for (size_t i = offset; i < m_prompts.size() && prompts.size() < pageSize; ++i)
  {
    const auto& prompt = m_prompts[i];
    auto entry = Json{{"name", prompt.name}};
    if (supportsTitles(protocolVersion) && !prompt.title.empty())
    {
      entry["title"] = prompt.title;
    }
    entry["description"] = prompt.description;

    auto arguments = Json::array();
    for (const auto& argument : prompt.arguments)
    {
      arguments.push_back(Json{
        {"name", argument.name},
        {"description", argument.description},
        {"required", argument.required},
      });
    }
    entry["arguments"] = std::move(arguments);
    prompts.push_back(std::move(entry));
  }

  const auto end = offset + prompts.size();
  auto result = Json{{"prompts", std::move(prompts)}};
  if (end < m_prompts.size())
  {
    result["nextCursor"] = encodeCursor(end, 0);
  }
  return result;
}

Result<Json, ToolError> PromptRegistry::get(
  const std::string_view name, const PromptArguments& arguments) const
{
  const auto* prompt = find(name);
  if (!prompt)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Prompt '" + std::string{name} + "' does not exist.",
      "Use prompts/list to find prompts.");
  }

  for (const auto& argument : prompt->arguments)
  {
    if (argument.required && !arguments.contains(argument.name))
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "Prompt '" + prompt->name + "' requires the argument '" + argument.name + "'.",
        argument.description);
    }
  }

  return prompt->get(arguments) | kdl::transform([&](Json messages) {
           return Json{
             {"description", prompt->description},
             {"messages", std::move(messages)},
           };
         });
}

} // namespace tb::mcp
