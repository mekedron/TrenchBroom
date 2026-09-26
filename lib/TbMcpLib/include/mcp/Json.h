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

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace tb::mcp
{

/**
 * The JSON type used throughout the MCP server. Keys keep their insertion order, which
 * makes responses deterministic and readable.
 */
using Json = nlohmann::ordered_json;

/**
 * Rounds the given value to 6 decimals so that agents see 64 rather than 63.99999999997.
 */
double roundForOutput(double value);

/**
 * Parses the given text. Returns nullopt if the text is not valid JSON.
 */
std::optional<Json> parseJson(std::string_view text);

/**
 * Serializes the given value compactly. Invalid UTF-8 is replaced rather than causing an
 * exception.
 */
std::string dumpJson(const Json& value);

/**
 * Returns the value of the given key if the value is an object that contains the key, or
 * nullptr otherwise.
 */
const Json* findMember(const Json& value, std::string_view key);

} // namespace tb::mcp
