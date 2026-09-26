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

#include "mcp/ResourceRegistry.h"

#include "mcp/Pagination.h"

#include "kd/contracts.h"

#include <algorithm>

namespace tb::mcp
{

Json jsonResourceContents(const std::string& uri, const Json& value)
{
  return Json::array({Json{
    {"uri", uri},
    {"mimeType", "application/json"},
    {"text", dumpJson(value)},
  }});
}

std::optional<ResourceVariables> matchUriTemplate(
  const std::string_view uriTemplate, const std::string_view uri)
{
  auto variables = ResourceVariables{};
  auto t = size_t{0};
  auto u = size_t{0};

  while (t < uriTemplate.size())
  {
    if (uriTemplate[t] == '{')
    {
      const auto close = uriTemplate.find('}', t);
      if (close == std::string_view::npos)
      {
        return std::nullopt;
      }
      const auto name = uriTemplate.substr(t + 1, close - t - 1);
      t = close + 1;

      // the variable extends to the next literal character of the template or '/'
      const auto terminator = t < uriTemplate.size() ? uriTemplate[t] : '\0';
      auto end = u;
      while (end < uri.size() && uri[end] != '/' && uri[end] != terminator)
      {
        ++end;
      }
      if (end == u)
      {
        return std::nullopt;
      }
      variables[std::string{name}] = std::string{uri.substr(u, end - u)};
      u = end;
    }
    else
    {
      if (u >= uri.size() || uri[u] != uriTemplate[t])
      {
        return std::nullopt;
      }
      ++t;
      ++u;
    }
  }

  if (u != uri.size())
  {
    return std::nullopt;
  }
  return variables;
}

void ResourceRegistry::add(ResourceDef resource)
{
  contract_pre(!exists(resource.uri));
  contract_pre(resource.read != nullptr);

  m_resources.push_back(std::move(resource));
}

void ResourceRegistry::addTemplate(ResourceTemplateDef resourceTemplate)
{
  contract_pre(resourceTemplate.read != nullptr);

  m_templates.push_back(std::move(resourceTemplate));
}

const std::vector<ResourceDef>& ResourceRegistry::resources() const
{
  return m_resources;
}

const std::vector<ResourceTemplateDef>& ResourceRegistry::templates() const
{
  return m_templates;
}

bool ResourceRegistry::exists(const std::string_view uri) const
{
  return std::ranges::any_of(
           m_resources, [&](const auto& resource) { return resource.uri == uri; })
         || std::ranges::any_of(m_templates, [&](const auto& resourceTemplate) {
              return matchUriTemplate(resourceTemplate.uriTemplate, uri).has_value();
            });
}

Json ResourceRegistry::list(
  ServerState& server,
  Session& session,
  const std::optional<std::string>& cursor,
  const size_t pageSize) const
{
  auto all = std::vector<Json>{};
  for (const auto& resource : m_resources)
  {
    auto entry = Json{{"uri", resource.uri}, {"name", resource.name}};
    if (!resource.title.empty())
    {
      entry["title"] = resource.title;
    }
    entry["description"] = resource.description;
    entry["mimeType"] = resource.mimeType;
    all.push_back(std::move(entry));
  }
  for (const auto& resourceTemplate : m_templates)
  {
    if (resourceTemplate.list)
    {
      for (auto& entry : resourceTemplate.list(server, session))
      {
        all.push_back(std::move(entry));
      }
    }
  }

  auto offset = size_t{0};
  if (cursor)
  {
    if (const auto decoded = decodeCursor(*cursor))
    {
      offset = decoded->offset;
    }
  }

  auto page = Json::array();
  for (size_t i = offset; i < all.size() && page.size() < pageSize; ++i)
  {
    page.push_back(all[i]);
  }

  const auto end = offset + page.size();
  auto result = Json{{"resources", std::move(page)}};
  if (end < all.size())
  {
    result["nextCursor"] = encodeCursor(end, 0);
  }
  return result;
}

Json ResourceRegistry::listTemplates() const
{
  auto templates = Json::array();
  for (const auto& resourceTemplate : m_templates)
  {
    auto entry = Json{
      {"uriTemplate", resourceTemplate.uriTemplate}, {"name", resourceTemplate.name}};
    if (!resourceTemplate.title.empty())
    {
      entry["title"] = resourceTemplate.title;
    }
    entry["description"] = resourceTemplate.description;
    entry["mimeType"] = resourceTemplate.mimeType;
    templates.push_back(std::move(entry));
  }
  return Json{{"resourceTemplates", std::move(templates)}};
}

Result<Json, ToolError> ResourceRegistry::read(
  ServerState& server, Session& session, const std::string& uri) const
{
  for (const auto& resource : m_resources)
  {
    if (resource.uri == uri)
    {
      return resource.read(server, session, uri, {});
    }
  }
  for (const auto& resourceTemplate : m_templates)
  {
    if (const auto variables = matchUriTemplate(resourceTemplate.uriTemplate, uri))
    {
      return resourceTemplate.read(server, session, uri, *variables);
    }
  }
  return makeError(
    ErrorCode::ObjectNotFound,
    "Resource '" + uri + "' does not exist.",
    "Use resources/list and resources/templates/list to find resources.");
}

} // namespace tb::mcp
