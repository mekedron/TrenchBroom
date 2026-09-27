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

#include "mcp/ToolRegistry.h"

#include "mcp/Pagination.h"
#include "mcp/ProtocolVersion.h"

#include "kd/contracts.h"

#include <algorithm>

namespace tb::mcp
{
namespace
{

void relaxAdditionalProperties(Json& schema)
{
  if (schema.is_object())
  {
    schema.erase("additionalProperties");
    for (auto& [key, value] : schema.items())
    {
      relaxAdditionalProperties(value);
    }
  }
  else if (schema.is_array())
  {
    for (auto& value : schema)
    {
      relaxAdditionalProperties(value);
    }
  }
}

void addFieldIfAbsent(schema::Schema& schema, schema::Field field)
{
  if (!schema.findField(field.name))
  {
    schema.fields.push_back(std::move(field));
  }
}

} // namespace

ToolDef::ToolDef(std::string name)
  : m_name{std::move(name)}
{
}

ToolDef& ToolDef::title(std::string title)
{
  m_title = std::move(title);
  return *this;
}

ToolDef& ToolDef::description(std::string description)
{
  m_description = std::move(description);
  return *this;
}

ToolDef& ToolDef::input(schema::Schema input)
{
  contract_pre(input.type == schema::Type::Object);
  m_input = std::move(input);
  return *this;
}

ToolDef& ToolDef::output(schema::Schema output)
{
  m_output = std::move(output);
  return *this;
}

ToolDef& ToolDef::mutation(const Mutation mutation)
{
  m_mutation = mutation;
  return *this;
}

ToolDef& ToolDef::documentUse(const DocumentUse documentUse)
{
  m_documentUse = documentUse;
  return *this;
}

ToolDef& ToolDef::transactional(const bool transactional)
{
  m_transactional = transactional;
  return *this;
}

ToolDef& ToolDef::keepsActiveTool(const bool keepsActiveTool)
{
  m_keepsActiveTool = keepsActiveTool;
  return *this;
}

ToolDef& ToolDef::paginated(const bool paginated)
{
  m_paginated = paginated;
  return *this;
}

ToolDef& ToolDef::destructive(const bool destructive)
{
  m_destructive = destructive;
  return *this;
}

ToolDef& ToolDef::idempotent(const bool idempotent)
{
  m_idempotent = idempotent;
  return *this;
}

ToolDef& ToolDef::openWorld(const bool openWorld)
{
  m_openWorld = openWorld;
  return *this;
}

ToolDef& ToolDef::handler(ToolHandler handler)
{
  m_handler = std::move(handler);
  return *this;
}

const std::string& ToolDef::name() const
{
  return m_name;
}

const std::string& ToolDef::title() const
{
  return m_title.empty() ? m_name : m_title;
}

const std::string& ToolDef::description() const
{
  return m_description;
}

Mutation ToolDef::mutation() const
{
  return m_mutation;
}

DocumentUse ToolDef::documentUse() const
{
  return m_documentUse.value_or(
    m_mutation == Mutation::Map ? DocumentUse::Required : DocumentUse::None);
}

bool ToolDef::transactional() const
{
  return m_mutation == Mutation::Map && m_transactional;
}

bool ToolDef::keepsActiveTool() const
{
  return m_keepsActiveTool;
}

bool ToolDef::paginated() const
{
  return m_paginated;
}

bool ToolDef::destructive() const
{
  return m_destructive;
}

bool ToolDef::idempotent() const
{
  return m_idempotent;
}

bool ToolDef::openWorld() const
{
  return m_openWorld;
}

ToolDef& ToolDef::asyncHandler(AsyncToolHandler handler)
{
  m_asyncHandler = std::move(handler);
  return *this;
}

const ToolHandler& ToolDef::handler() const
{
  return m_handler;
}

const AsyncToolHandler& ToolDef::asyncHandler() const
{
  return m_asyncHandler;
}

bool ToolDef::isAsync() const
{
  return m_asyncHandler != nullptr;
}

bool ToolDef::isModifying() const
{
  return m_mutation != Mutation::None;
}

schema::Schema ToolDef::inputSchema() const
{
  using namespace schema;

  auto result = m_input;
  if (documentUse() != DocumentUse::None)
  {
    addFieldIfAbsent(
      result,
      field("document", documentId())
        .describe("Target document, e.g. 'doc:1'. Default: the document chosen with "
                  "document_activate, else the focused window"));
  }
  if (isModifying())
  {
    addFieldIfAbsent(
      result,
      field("dryRun", boolean().defaultsTo(false))
        .describe("Validate and report what would happen (including changes and issues) "
                  "without changing anything"));
  }
  if (m_paginated)
  {
    for (auto& paginationField : paginationFields())
    {
      addFieldIfAbsent(result, std::move(paginationField));
    }
  }
  return result;
}

std::optional<schema::Schema> ToolDef::outputSchema() const
{
  using namespace schema;

  if (!isModifying())
  {
    return m_output;
  }

  return object(
           {
             field("ok", boolean()).required(),
             field("dryRun", boolean()).required(),
             field("undoStep", any())
               .describe("Name of the undo step this call created, or null"),
             field("result", m_output.value_or(object({}).allowAdditionalProperties()))
               .describe("Tool-specific result"),
             field("changes", object({}).allowAdditionalProperties())
               .describe("Ids of created, modified and removed objects"),
             field("selection", object({}).allowAdditionalProperties())
               .describe("The selection after the call"),
             field(
               "issuesIntroduced",
               array(object(
                       {
                         field("objectId", string()).required(),
                         field("type", string()).required(),
                         field("description", string()).required(),
                         field("code", string())
                           .describe(
                             "Machine code: Z_FIGHTING, ENTITY_OUTSIDE_HULL, MODEL_*, "
                             "UV_ASPECT_DISTORTION, or the editor validator in "
                             "UPPER_SNAKE case"),
                         field("source", enumOf({"editor", "mcp"})),
                         field("details", any())
                           .describe("MCP checks: face ids, positions, bounds, measures"),
                       })
                       .allowAdditionalProperties()))
               .describe(
                 "Problems the call introduced: editor validator issues of created and "
                 "modified objects, and MCP placement checks (z-fighting, entities "
                 "outside the hull, model placement, texture distortion)"),
             field("warnings", array(any())),
             field("console", array(any()))
               .describe("Console warnings and errors logged while the call ran"),
             field("grid", number()).describe("Grid size in effect"),
           })
    .allowAdditionalProperties();
}

Json ToolDef::toJson(const std::string_view protocolVersion) const
{
  auto result = Json{{"name", m_name}};
  if (supportsTitles(protocolVersion))
  {
    result["title"] = title();
  }
  result["description"] = m_description;
  result["inputSchema"] = inputSchema().toJsonSchema();

  if (supportsStructuredContent(protocolVersion))
  {
    if (const auto output = outputSchema())
    {
      auto outputJson = output->toJsonSchema();
      relaxAdditionalProperties(outputJson);
      result["outputSchema"] = std::move(outputJson);
    }
  }

  result["annotations"] = Json{
    {"title", title()},
    {"readOnlyHint", m_mutation == Mutation::None},
    {"destructiveHint", m_destructive},
    {"idempotentHint", m_idempotent},
    {"openWorldHint", m_openWorld},
  };
  return result;
}

void ToolRegistry::add(ToolDef tool)
{
  contract_pre(isValidToolName(tool.name()));
  contract_pre(find(tool.name()) == nullptr);
  contract_pre((tool.handler() != nullptr) != (tool.asyncHandler() != nullptr));
  contract_pre(!tool.isAsync() || tool.mutation() != Mutation::Map);

  m_tools.push_back(std::move(tool));
}

const ToolDef* ToolRegistry::find(const std::string_view name) const
{
  const auto it =
    std::ranges::find_if(m_tools, [&](const auto& tool) { return tool.name() == name; });
  return it != m_tools.end() ? &*it : nullptr;
}

const std::vector<ToolDef>& ToolRegistry::tools() const
{
  return m_tools;
}

Json ToolRegistry::list(
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

  auto tools = Json::array();
  for (size_t i = offset; i < m_tools.size() && tools.size() < pageSize; ++i)
  {
    tools.push_back(m_tools[i].toJson(protocolVersion));
  }

  auto result = Json{{"tools", std::move(tools)}};
  const auto end = offset + result["tools"].size();
  if (end < m_tools.size())
  {
    result["nextCursor"] = encodeCursor(end, 0);
  }
  return result;
}

bool isValidToolName(const std::string_view name)
{
  if (name.empty() || name.size() > 64 || name[0] < 'a' || name[0] > 'z')
  {
    return false;
  }
  return std::ranges::all_of(name, [](const char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
  });
}

} // namespace tb::mcp
