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

#include "ui/McpEngineHost.h"

#include "el/Interpolate.h"
#include "mdl/GameEngineProfile.h"
#include "ui/CompilationVariables.h"
#include "ui/LaunchGameEngine.h"
#include "ui/MapDocument.h"

namespace tb::ui
{

McpEngineHost::McpEngineHost(std::optional<std::filesystem::path> logFilePath)
  : m_logFilePath{std::move(logFilePath)}
{
}

McpEngineHost::~McpEngineHost() = default;

Result<std::string> McpEngineHost::engineParameters(
  MapDocument& document, const std::string& parameterSpec)
{
  return el::interpolate(LaunchGameEngineVariables{document.map()}, parameterSpec);
}

Result<int64_t> McpEngineHost::launchEngine(
  MapDocument& document,
  const mdl::GameEngineProfile& profile,
  std::optional<std::string> parameterSpec)
{
  auto launchedProfile = profile;
  if (parameterSpec)
  {
    launchedProfile.parameterSpec = std::move(*parameterSpec);
  }

  auto processId = int64_t{0};
  return launchGameEngineProfile(
           launchedProfile,
           LaunchGameEngineVariables{document.map()},
           m_logFilePath,
           &processId)
         | kdl::transform([&]() { return processId; });
}

} // namespace tb::ui
