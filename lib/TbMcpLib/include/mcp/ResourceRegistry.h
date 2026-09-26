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
class ServerState;
class Session;

using ResourceVariables = std::map<std::string, std::string>;

/**
 * Reads a resource. Returns the `contents` array of a `resources/read` result, e.g.
 * `[{"uri": ..., "mimeType": "application/json", "text": ...}]`.
 */
using ResourceReader = std::function<Result<Json, ToolError>(
  ServerState&, Session&, const std::string& uri, const ResourceVariables&)>;

/** Lists the concrete resources of a template, e.g. one per open document. */
using ResourceLister = std::function<std::vector<Json>(ServerState&, Session&)>;

struct ResourceDef
{
  std::string uri;
  std::string name;
  std::string title;
  std::string description;
  std::string mimeType = "application/json";
  ResourceReader read;
};

struct ResourceTemplateDef
{
  /** An RFC 6570 level 1 template, e.g. `trenchbroom://documents/{doc}/info`. */
  std::string uriTemplate;
  std::string name;
  std::string title;
  std::string description;
  std::string mimeType = "application/json";
  ResourceReader read;
  /** Optional: contributes concrete entries to `resources/list`. */
  ResourceLister list;
};

/** Returns `[{"uri": uri, "mimeType": ..., "text": <serialized json>}]`. */
Json jsonResourceContents(const std::string& uri, const Json& value);

/**
 * Matches a URI against an RFC 6570 level 1 template. Variables match one path segment
 * (no '/').
 */
std::optional<ResourceVariables> matchUriTemplate(
  std::string_view uriTemplate, std::string_view uri);

class ResourceRegistry
{
private:
  std::vector<ResourceDef> m_resources;
  std::vector<ResourceTemplateDef> m_templates;

public:
  /** Precondition: the uri is not yet registered. */
  void add(ResourceDef resource);
  void addTemplate(ResourceTemplateDef resourceTemplate);

  const std::vector<ResourceDef>& resources() const;
  const std::vector<ResourceTemplateDef>& templates() const;

  /** Whether the uri names a static resource or matches a template. */
  bool exists(std::string_view uri) const;

  /** A `resources/list` result with the static resources and listed template entries. */
  Json list(
    ServerState& server,
    Session& session,
    const std::optional<std::string>& cursor,
    size_t pageSize = 100) const;

  /** A `resources/templates/list` result. */
  Json listTemplates() const;

  /**
   * Reads the resource. Returns an OBJECT_NOT_FOUND error if nothing matches the uri.
   */
  Result<Json, ToolError> read(
    ServerState& server, Session& session, const std::string& uri) const;
};

} // namespace tb::mcp
