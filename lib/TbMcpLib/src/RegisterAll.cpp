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

#include "mcp/RegisterAll.h"

#include "mcp/McpServer.h"
#include "mcp/Resources.h"
#include "mcp/tools/BrushEditTools.h"
#include "mcp/tools/ClipboardTools.h"
#include "mcp/tools/CompileTools.h"
#include "mcp/tools/ConsoleTools.h"
#include "mcp/tools/DocumentTools.h"
#include "mcp/tools/EntityClassTools.h"
#include "mcp/tools/EntityCreateTools.h"
#include "mcp/tools/EntityPropertyTools.h"
#include "mcp/tools/FaceTools.h"
#include "mcp/tools/GameTools.h"
#include "mcp/tools/GeometryTools.h"
#include "mcp/tools/GroupTools.h"
#include "mcp/tools/HistoryTools.h"
#include "mcp/tools/LayerTools.h"
#include "mcp/tools/MaterialTools.h"
#include "mcp/tools/SceneTools.h"
#include "mcp/tools/SelectionTools.h"
#include "mcp/tools/SessionTools.h"
#include "mcp/tools/SnapshotTools.h"
#include "mcp/tools/SpatialTools.h"
#include "mcp/tools/TagTools.h"
#include "mcp/tools/TransformTools.h"
#include "mcp/tools/ViewTools.h"

namespace tb::mcp
{

void registerAll(McpServer& server)
{
  registerSessionTools(server.tools());
  registerHistoryTools(server.tools());
  registerDocumentTools(server.tools());
  registerGameTools(server.tools());
  registerSceneTools(server.tools());
  registerSpatialTools(server.tools());
  registerSelectionTools(server.tools());
  registerGeometryTools(server.tools());
  registerTransformTools(server.tools());
  registerBrushEditTools(server.tools());
  registerViewTools(server.tools());
  registerMaterialTools(server.tools());
  registerFaceTools(server.tools());
  registerTagTools(server.tools());
  registerEntityClassTools(server.tools());
  registerEntityCreateTools(server.tools());
  registerEntityPropertyTools(server.tools());
  registerLayerTools(server.tools());
  registerGroupTools(server.tools());
  registerClipboardTools(server.tools());
  registerCompileTools(server.tools());
  registerSnapshotTools(server.tools());
  registerConsoleTools(server.tools());
  registerResources(server);
  registerCompileResources(server.resources());
  registerConsoleResources(server.resources());
}

} // namespace tb::mcp
