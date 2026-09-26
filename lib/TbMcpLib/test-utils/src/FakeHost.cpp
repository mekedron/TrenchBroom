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

#include "mcp/FakeHost.h"

#include <algorithm>

namespace tb::mcp
{

std::string FakeHost::addDocument(ui::MapDocument& document, std::string title)
{
  for (auto& info : documentList)
  {
    info.focused = false;
  }

  auto id = "doc:" + std::to_string(m_nextDocumentId++);
  documentList.push_back(DocumentInfo{id, &document, std::move(title), true});
  documentsDidChangeNotifier();
  return id;
}

void FakeHost::removeDocument(ui::MapDocument& document)
{
  documentWillCloseNotifier(document);
  std::erase_if(
    documentList, [&](const auto& info) { return info.document == &document; });
  documentsDidChangeNotifier();
}

void FakeHost::setFocused(const std::string& documentId)
{
  for (auto& info : documentList)
  {
    info.focused = info.id == documentId;
  }
  documentsDidChangeNotifier();
}

std::string FakeHost::applicationVersion() const
{
  return version;
}

std::vector<DocumentInfo> FakeHost::documents()
{
  return documentList;
}

BusyState FakeHost::busyState(ui::MapDocument&)
{
  return busy;
}

std::vector<std::string> FakeHost::prepareForAgentEdit(ui::MapDocument&)
{
  ++prepareCount;
  return prepareNotes;
}

std::optional<std::string> FakeHost::currentToolName(ui::MapDocument&)
{
  return toolName;
}

bool FakeHost::isCompileRunning(ui::MapDocument&)
{
  return compileRunning;
}

} // namespace tb::mcp
