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

#include <string>
#include <string_view>
#include <vector>

namespace tb::mcp
{

namespace ProtocolVersion
{
constexpr auto V2025_11_25 = std::string_view{"2025-11-25"};
constexpr auto V2025_06_18 = std::string_view{"2025-06-18"};
constexpr auto V2025_03_26 = std::string_view{"2025-03-26"};

/** The preferred revision. */
constexpr auto Latest = V2025_11_25;
} // namespace ProtocolVersion

/** All supported revisions, the preferred one first. */
const std::vector<std::string>& supportedProtocolVersions();

bool isSupportedProtocolVersion(std::string_view version);

/**
 * Returns the version the server answers to an `initialize` request: the requested
 * version if it is supported, or the latest one otherwise.
 */
std::string negotiateProtocolVersion(std::string_view requestedVersion);

/** Whether JSON-RPC batches are allowed (only in 2025-03-26). */
bool supportsBatches(std::string_view version);

/** Whether tool results carry `structuredContent` and tools publish `outputSchema`. */
bool supportsStructuredContent(std::string_view version);

/** Whether tools, resources and prompts may carry a `title`. */
bool supportsTitles(std::string_view version);

} // namespace tb::mcp
