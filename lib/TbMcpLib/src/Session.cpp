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

#include "mcp/Session.h"

#include "mcp/Endpoint.h"

#include <algorithm>
#include <string>

namespace tb::mcp
{

bool Session::send(const Json& notification) const
{
  if (!initialized)
  {
    return false;
  }
  if (auto stream = notificationStream.lock())
  {
    stream->send(notification);
    return true;
  }
  return false;
}

std::string Session::clientDisplayName() const
{
  if (clientName.empty())
  {
    return "client " + id.substr(0, 8);
  }
  return clientVersion.empty() ? clientName : clientName + " " + clientVersion;
}

std::string Session::recordSnapshot(SnapshotRecord record)
{
  record.id = "snap:" + std::to_string(nextSnapshotNumber++);
  if (snapshotRecords.size() >= MaxSnapshotRecords)
  {
    snapshotRecords.erase(snapshotRecords.begin());
  }
  snapshotRecords.push_back(std::move(record));
  return snapshotRecords.back().id;
}

const SnapshotRecord* Session::findSnapshotRecord(const std::string& id) const
{
  const auto it = std::ranges::find_if(
    snapshotRecords, [&](const auto& record) { return record.id == id; });
  return it != snapshotRecords.end() ? &*it : nullptr;
}

} // namespace tb::mcp
