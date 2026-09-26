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

#include "mcp/ProtocolVersion.h"

#include <algorithm>

namespace tb::mcp
{

const std::vector<std::string>& supportedProtocolVersions()
{
  static const auto versions = std::vector<std::string>{
    std::string{ProtocolVersion::V2025_11_25},
    std::string{ProtocolVersion::V2025_06_18},
    std::string{ProtocolVersion::V2025_03_26},
  };
  return versions;
}

bool isSupportedProtocolVersion(const std::string_view version)
{
  return std::ranges::find(supportedProtocolVersions(), version)
         != supportedProtocolVersions().end();
}

std::string negotiateProtocolVersion(const std::string_view requestedVersion)
{
  return isSupportedProtocolVersion(requestedVersion)
           ? std::string{requestedVersion}
           : std::string{ProtocolVersion::Latest};
}

bool supportsBatches(const std::string_view version)
{
  return version == ProtocolVersion::V2025_03_26;
}

bool supportsStructuredContent(const std::string_view version)
{
  return version != ProtocolVersion::V2025_03_26;
}

bool supportsTitles(const std::string_view version)
{
  return version != ProtocolVersion::V2025_03_26;
}

} // namespace tb::mcp
