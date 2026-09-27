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

#include "mcp/tools/ClipboardTools.h"

#include "NodeJson.h"
#include "ToolUtils.h"
#include "base/Logger.h"
#include "base/ParserStatus.h"
#include "fs/DiskIO.h"
#include "fs/PathInfo.h"
#include "gl/MaterialManager.h"
#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/JsonVm.h"
#include "mcp/ObjectIds.h"
#include "mcp/ServerState.h"
#include "mcp/Targets.h"
#include "mcp/ToolRegistry.h"
#include "mcp/tools/GeometryUtils.h"
#include "mcp/tools/UvTools.h"
#include "mdl/BezierPatch.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/Grid.h"
#include "mdl/GroupNode.h"
#include "mdl/Layer.h"
#include "mdl/LayerNode.h"
#include "mdl/LinkedGroupUtils.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/MapHeader.h"
#include "mdl/MapReader.h"
#include "mdl/Map_CopyPaste.h"
#include "mdl/Map_Geometry.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_Layers.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/ModelUtils.h"
#include "mdl/Node.h"
#include "mdl/NodeWriter.h"
#include "mdl/PasteType.h"
#include "mdl/PatchNode.h"
#include "mdl/Selection.h"
#include "mdl/WorldNode.h"

#include "kd/overload.h"
#include "kd/string_compare.h"
#include "kd/string_utils.h"

#include "vm/bbox.h"
#include "vm/scalar.h"
#include "vm/vec.h"

#include <fmt/format.h>
#include <fmt/std.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace tb::mcp
{
namespace
{

using namespace schema;

constexpr size_t MaxSummaries = 100;
constexpr size_t MaxListItems = 500;

const auto ClipboardKinds = std::vector<ObjectKind>{
  ObjectKind::Group, ObjectKind::Entity, ObjectKind::Brush, ObjectKind::Patch};

// Shared helpers

/** Drops duplicates and nodes whose ancestor is in the list as well. */
std::vector<mdl::Node*> withoutDescendants(std::vector<mdl::Node*> nodes)
{
  auto seen = std::set<const mdl::Node*>{};
  std::erase_if(nodes, [&](const auto* node) { return !seen.insert(node).second; });
  std::erase_if(nodes, [&](const auto* node) {
    return std::ranges::any_of(
      nodes, [&](const auto* other) { return node->isDescendantOf(*other); });
  });
  return nodes;
}

std::string serializeNodes(
  const mdl::WorldNode& world,
  const std::vector<mdl::Node*>& nodes,
  kdl::task_manager& taskManager)
{
  auto stream = std::stringstream{};
  auto writer = mdl::NodeWriter{world, stream};
  writer.writeNodes(nodes, taskManager);
  return stream.str();
}

size_t lineCount(const std::string& text)
{
  return size_t(std::ranges::count(text, '\n'))
         + (text.empty() || text.back() == '\n' ? 0 : 1);
}

/**
 * Whether the text holds brush faces rather than objects: the first character that is
 * not whitespace or part of a `//` comment is `(` (a face line) rather than `{`.
 */
bool isFaceText(std::string_view text)
{
  size_t i = 0;
  while (i < text.size())
  {
    if (std::isspace(static_cast<unsigned char>(text[i])))
    {
      ++i;
    }
    else if (text.substr(i, 2) == "//")
    {
      const auto end = text.find('\n', i);
      i = end == std::string_view::npos ? text.size() : end + 1;
    }
    else
    {
      return text[i] == '(';
    }
  }
  return false;
}

void collectMaterials(const mdl::Node& node, std::set<std::string>& materials)
{
  for (auto& material : nodeMaterials(node))
  {
    materials.insert(std::move(material));
  }
  for (const auto* child : node.children())
  {
    collectMaterials(*child, materials);
  }
}

/** The distinct material names used by the given nodes and their descendants, sorted. */
template <typename Nodes>
std::vector<std::string> materialsOf(const Nodes& nodes)
{
  auto materials = std::set<std::string>{};
  for (const auto* node : nodes)
  {
    collectMaterials(*node, materials);
  }
  materials.erase(mdl::BrushFace::NoMaterialName);
  return {materials.begin(), materials.end()};
}

/** The given materials that none of the map's material collections provides. */
std::vector<std::string> missingMaterials(
  const mdl::Map& map, const std::vector<std::string>& materials)
{
  auto result = std::vector<std::string>{};
  std::ranges::copy_if(materials, std::back_inserter(result), [&](const auto& name) {
    return map.materialManager().material(name) == nullptr;
  });
  return result;
}

/** Warns with MISSING_MATERIALS if any of the given materials is not loaded. */
void warnMissingMaterials(CallContext& context, const std::vector<std::string>& missing)
{
  if (!missing.empty())
  {
    context.warn(
      "MISSING_MATERIALS",
      fmt::format(
        "{} material(s) are not in the map's material collections: {}. Add a collection "
        "with materials_collections_set or replace them with material_replace.",
        missing.size(),
        kdl::str_join(missing, ", ")));
  }
}

std::optional<vm::bbox3d> boundsOf(const std::vector<mdl::Node*>& nodes)
{
  auto result = std::optional<vm::bbox3d>{};
  for (const auto* node : nodes)
  {
    const auto& bounds = node->logicalBounds();
    result = result ? vm::merge(*result, bounds) : bounds;
  }
  return result;
}

Json summaries(const std::vector<mdl::Node*>& nodes, const IdRegistry& ids)
{
  auto result = Json::array();
  for (size_t i = 0; i < nodes.size() && i < MaxSummaries; ++i)
  {
    result.push_back(nodeSummary(*nodes[i], ids));
  }
  return result;
}

// Placement of pasted and imported objects

/** Where pasted or imported objects go: their original position, a point or an offset.
 */
struct Placement
{
  std::optional<vm::vec3d> position;
  std::string anchor = "min";
  std::optional<vm::vec3d> offset;
  bool snapToGrid = false;
};

Result<Placement, ToolError> placementArgument(const Args& args)
{
  auto placement = Placement{
    args.getOptional<vm::vec3d>("position"),
    args.get<std::string>("anchor"),
    args.getOptional<vm::vec3d>("offset"),
    args.get<bool>("snapToGrid"),
  };
  if (placement.position && placement.offset)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "'position' and 'offset' cannot be combined.",
      "Pass 'position' to place the objects' bounds anchor at a point, or 'offset' to "
      "move "
      "them relative to their original position.");
  }
  return placement;
}

vm::vec3d anchorPoint(const vm::bbox3d& bounds, const std::string& anchor)
{
  if (anchor == "center")
  {
    return bounds.center();
  }
  if (anchor == "max")
  {
    return bounds.max;
  }
  if (anchor == "bottomCenter")
  {
    const auto center = bounds.center();
    return {center.x(), center.y(), bounds.min.z()};
  }
  return bounds.min;
}

/** The translation that moves objects with the given bounds to their placement. */
vm::vec3d placementDelta(
  const mdl::Map& map, const vm::bbox3d& bounds, const Placement& placement)
{
  auto delta = placement.position
                 ? *placement.position - anchorPoint(bounds, placement.anchor)
               : placement.offset ? *placement.offset
                                  : vm::vec3d{0, 0, 0};
  if (placement.snapToGrid)
  {
    // like pasting in the editor: the bounds' min corner lands on the grid
    const auto gridSize = map.grid().actualSize();
    const auto min = bounds.min + delta;
    for (size_t i = 0; i < 3; ++i)
    {
      delta[i] += vm::round(min[i] / gridSize) * gridSize - min[i];
    }
  }
  return delta;
}

Json placementJson(const Placement& placement)
{
  if (placement.position)
  {
    return Json{
      {"mode", "point"},
      {"position", toJson(*placement.position)},
      {"anchor", placement.anchor},
      {"snapToGrid", placement.snapToGrid},
    };
  }
  if (placement.offset)
  {
    return Json{
      {"mode", "offset"},
      {"offset", toJson(*placement.offset)},
      {"snapToGrid", placement.snapToGrid},
    };
  }
  return Json{{"mode", "original"}, {"snapToGrid", placement.snapToGrid}};
}

Result<mdl::LayerNode*, ToolError> targetLayerArgument(
  CallContext& context, const Args& args)
{
  const auto id = args.getOptional<std::string>("targetLayer");
  if (!id)
  {
    return nullptr;
  }
  auto resolved = context.ids().resolve(*id);
  if (resolved.is_error())
  {
    return errorOf(resolved);
  }
  return dynamic_cast<mdl::LayerNode*>(resolved.value());
}

UvMode uvModeArgument(const Args& args)
{
  return uvModeFromString(args.getOr<std::string>("uv", "keep")).value_or(UvMode::Keep);
}

/**
 * Pastes map text as objects (mdl::paste, the editor's paste), moves them to their
 * placement and into the target layer, aligns their faces with the UV mode, and leaves
 * them selected. Returns the paste result.
 */
ToolResult pasteObjects(
  CallContext& context,
  const std::string& text,
  const Placement& placement,
  mdl::LayerNode* targetLayer,
  const UvMode uvMode)
{
  auto& map = context.map();
  auto& ids = context.ids();

  if (mdl::paste(map, text) != mdl::PasteType::Node)
  {
    return context.operationFailed(
      "The text could not be pasted as objects.",
      "Pass map text in the document's format (or a compatible one: Standard and Valve, "
      "Quake 2 and Quake 2 Valve, the Quake 3 formats), e.g. from clipboard_copy or "
      "map_text_get. Use map_import for map files in other formats.");
  }

  auto pasted = map.selection().nodes;
  if (pasted.empty())
  {
    return makeError(
      ErrorCode::ObjectNotEditable,
      "The objects were pasted into a hidden or locked layer or group, so they cannot be "
      "selected, moved or reported.",
      "Show and unlock the current layer (layer_set_state) or pass 'targetLayer'.");
  }

  const auto bounds = *boundsOf(pasted);
  const auto delta = placementDelta(map, bounds, placement);
  if (delta != vm::vec3d{0, 0, 0})
  {
    if (!mdl::translateSelection(map, delta))
    {
      return makeError(
        ErrorCode::OutOfWorldBounds,
        fmt::format(
          "The objects could not be moved by {}; they would reach the world bounds.",
          toJson(delta).dump()),
        "Choose a position inside the world bounds.");
    }
  }

  if (targetLayer && &mdl::parentForNodes(map) != targetLayer)
  {
    if (!mdl::canMoveSelectedNodesToLayer(map, targetLayer))
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "The objects cannot be moved to layer " + ids.format(*targetLayer) + ".",
        "Pass an editable layer, or close the open group first (group_close).",
        {ids.format(*targetLayer)});
    }
    const auto moved = pasted;
    mdl::moveSelectedNodesToLayer(map, targetLayer);
    // objects moved into a hidden or locked layer are not selected
    pasted = map.selection().nodes.empty() ? moved : map.selection().nodes;
  }
  else
  {
    pasted = map.selection().nodes;
  }
  if (
    auto error = checkInsideWorldBounds(
      pasted, map, ids, "Choose a position inside the world bounds."))
  {
    return *error;
  }

  const auto missing = missingMaterials(map, materialsOf(pasted));
  warnMissingMaterials(context, missing);

  auto uv = Json(nullptr);
  if (uvMode != UvMode::Keep)
  {
    auto aligned = alignNodeUvs(context, pasted, uvMode);
    if (aligned.is_error())
    {
      return errorOf(aligned);
    }
    uv = std::move(aligned).value();
  }

  const auto finalBounds = boundsOf(pasted);
  auto result = Json{
    {"pasteType", "objects"},
    {"ids", formatIds(pasted, ids)},
    {"count", pasted.size()},
    {"objects", summaries(pasted, ids)},
    {"truncated", pasted.size() > MaxSummaries},
    {"bounds", finalBounds ? toJson(*finalBounds) : Json{}},
    {"offset", toJson(delta)},
    {"placement", placementJson(placement)},
    {"layer", layerIdOf(*pasted.front(), ids)},
    {"missingMaterials", missing},
  };
  if (!uv.is_null())
  {
    result["uv"] = std::move(uv);
  }
  return result;
}

// clipboard_copy / clipboard_cut

struct CopiedText
{
  std::string mode;
  std::string text;
  std::vector<std::string> ids;
};

Result<CopiedText, ToolError> copyObjects(
  CallContext& context, const std::vector<mdl::Node*>& targets)
{
  auto& map = context.map();
  const auto nodes = withoutDescendants(targets);
  return CopiedText{
    "objects",
    serializeNodes(map.worldNode(), nodes, map.taskManager()),
    formatIds(nodes, context.ids()),
  };
}

Result<CopiedText, ToolError> copyFaces(
  CallContext& context, const std::vector<mdl::BrushFaceHandle>& handles)
{
  auto& map = context.map();
  auto faces = std::vector<mdl::BrushFace>{};
  auto faceIds = std::vector<std::string>{};
  for (const auto& handle : handles)
  {
    faces.push_back(handle.face());
    faceIds.push_back(context.ids().formatFace(*handle.node(), handle.faceIndex()));
  }

  auto stream = std::stringstream{};
  auto writer = mdl::NodeWriter{map.worldNode(), stream};
  writer.writeBrushFaces(faces, map.taskManager());
  return CopiedText{"faces", stream.str(), std::move(faceIds)};
}

Json copyResult(const CopiedText& copied, const bool includeText)
{
  auto result = Json{
    {"mode", copied.mode},
    {"ids", copied.ids},
    {"count", copied.ids.size()},
    {"lineCount", lineCount(copied.text)},
    {"bytes", copied.text.size()},
  };
  if (includeText)
  {
    result["text"] = copied.text;
  }
  return result;
}

ToolResult clipboardCopy(CallContext& context, const Args& args)
{
  if (args.has("ids") && args.has("faces"))
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "'ids' and 'faces' cannot be combined.",
      "Copy objects with 'ids' or face attributes with 'faces'.");
  }

  const auto copyFacesMode =
    args.has("faces") || (!args.has("ids") && context.map().selection().hasBrushFaces());

  auto copied = [&]() -> Result<CopiedText, ToolError> {
    if (copyFacesMode)
    {
      auto faces = resolveFaceTargets(context, args, "faces");
      if (faces.is_error())
      {
        return errorOf(faces);
      }
      return copyFaces(context, faces.value());
    }
    auto targets = resolveTargets(context, args, "ids", ClipboardKinds);
    if (targets.is_error())
    {
      return errorOf(targets);
    }
    return copyObjects(context, targets.value());
  }();
  if (copied.is_error())
  {
    return errorOf(copied);
  }

  context.server().clipboard = copied.value().text;
  return copyResult(copied.value(), args.get<bool>("includeText"));
}

ToolResult clipboardCut(CallContext& context, const Args& args)
{
  auto targets = resolveTargets(context, args, "ids", ClipboardKinds);
  if (targets.is_error())
  {
    return errorOf(targets);
  }

  auto copied = copyObjects(context, targets.value());
  if (copied.is_error())
  {
    return errorOf(copied);
  }

  auto& map = context.map();
  auto result = withTargets(context, targets.value(), [&]() -> ToolResult {
    mdl::removeSelectedNodes(map);
    return copyResult(copied.value(), args.get<bool>("includeText"));
  });

  if (result.is_success() && !context.dryRun())
  {
    context.server().clipboard = copied.value().text;
  }
  return result;
}

// clipboard_paste

ToolResult pasteFaces(CallContext& context, const Args& args, const std::string& text)
{
  for (const auto* key : {"position", "offset", "targetLayer"})
  {
    if (args.has(key))
    {
      return makeError(
        ErrorCode::InvalidArgument,
        fmt::format(
          "The text holds brush faces, which apply to existing faces, so '{}' does not "
          "apply.",
          key),
        "Pass the target faces in 'faces', or paste object text to use a placement.");
    }
  }

  auto faces = resolveFaceTargets(context, args, "faces");
  if (faces.is_error())
  {
    return errorOf(faces);
  }

  auto& map = context.map();
  auto& ids = context.ids();
  auto faceIds = std::vector<std::string>{};
  for (const auto& handle : faces.value())
  {
    faceIds.push_back(ids.formatFace(*handle.node(), handle.faceIndex()));
  }

  return withFaces(context, faces.value(), [&]() -> ToolResult {
    if (mdl::paste(map, text) != mdl::PasteType::BrushFace)
    {
      return context.operationFailed(
        "The face text could not be applied.",
        "Pass brush face lines in the document's format, e.g. from clipboard_copy with "
        "'faces'.");
    }

    const auto material = faces.value().front().face().materialName();
    const auto missing = missingMaterials(map, {material});
    warnMissingMaterials(context, missing);
    return Json{
      {"pasteType", "faces"},
      {"faces", faceIds},
      {"count", faceIds.size()},
      {"material", material},
      {"missingMaterials", missing},
    };
  });
}

ToolResult clipboardPaste(CallContext& context, const Args& args)
{
  const auto text = args.getOptional<std::string>("text");
  if (!text && context.server().clipboard.empty())
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "The server's clipboard is empty.",
      "Copy objects or faces with clipboard_copy / clipboard_cut first, or pass 'text'.");
  }
  const auto& pasteText = text ? *text : context.server().clipboard;

  if (isFaceText(pasteText))
  {
    if (uvModeArgument(args) != UvMode::Keep)
    {
      context.warn(
        "IGNORED_ARGUMENT", "'uv' has no effect on face text, which carries its UVs.");
    }
    return pasteFaces(context, args, pasteText);
  }

  if (args.has("faces"))
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "The text holds objects, so 'faces' does not apply.",
      "Omit 'faces', or paste face text (clipboard_copy with 'faces').");
  }

  auto placement = placementArgument(args);
  if (placement.is_error())
  {
    return errorOf(placement);
  }
  auto targetLayer = targetLayerArgument(context, args);
  if (targetLayer.is_error())
  {
    return errorOf(targetLayer);
  }

  return pasteObjects(
    context, pasteText, placement.value(), targetLayer.value(), uvModeArgument(args));
}

// map_import and map_file_inspect

/** Collects the parser's warnings and errors instead of logging them. */
class CollectingParserStatus : public ParserStatus
{
public:
  std::vector<std::string> problems;

  explicit CollectingParserStatus(Logger& logger)
    : ParserStatus{logger, ""}
  {
  }

private:
  void doProgress(double) override {}

  void doLog(const LogLevel level, const std::string& str) override
  {
    if (level == LogLevel::Warn || level == LogLevel::Error)
    {
      problems.push_back(str);
    }
  }
};

/**
 * Reads a whole map file in its source format and converts brush faces to the target
 * format while parsing (the same conversion as pasting Valve text into a Standard map).
 * Like mdl::WorldReader, which cannot convert because it uses one format for both.
 */
class ImportReader : public mdl::MapReader
{
private:
  std::unique_ptr<mdl::WorldNode> m_world;

public:
  ImportReader(
    const std::string_view str,
    const mdl::MapFormat sourceFormat,
    const mdl::MapFormat targetFormat,
    const mdl::EntityPropertyConfig& entityPropertyConfig)
    : MapReader{str, sourceFormat, targetFormat, entityPropertyConfig}
    , m_world{std::make_unique<mdl::WorldNode>(
        entityPropertyConfig, mdl::Entity{}, targetFormat)}
  {
    m_world->disableNodeTreeUpdates();
  }

  Result<std::unique_ptr<mdl::WorldNode>> read(
    const vm::bbox3d& worldBounds, ParserStatus& status, kdl::task_manager& taskManager)
  {
    return readEntities(worldBounds, status, taskManager) | kdl::transform([&]() {
             for (const auto& error : mdl::initializeLinkIds({m_world.get()}))
             {
               status.error("Could not restore linked groups: " + error.msg);
             }
             m_world->rebuildNodeTree();
             m_world->enableNodeTreeUpdates();
             return std::move(m_world);
           });
  }

private:
  mdl::Node* onWorldNode(
    std::unique_ptr<mdl::WorldNode> worldNode, ParserStatus&) override
  {
    m_world->setEntity(worldNode->entity());
    auto* defaultLayer = m_world->defaultLayer();
    defaultLayer->setLayer(worldNode->defaultLayer()->layer());
    return defaultLayer;
  }

  void onLayerNode(std::unique_ptr<mdl::Node> layerNode, ParserStatus&) override
  {
    m_world->addChild(layerNode.release());
  }

  void onNode(
    mdl::Node* parentNode, std::unique_ptr<mdl::Node> node, ParserStatus&) override
  {
    auto* parent = parentNode ? parentNode : m_world->defaultLayer();
    parent->addChild(node.release());
  }
};

const auto AllFormats = std::vector<mdl::MapFormat>{
  mdl::MapFormat::Standard,
  mdl::MapFormat::Valve,
  mdl::MapFormat::Quake2,
  mdl::MapFormat::Quake2_Valve,
  mdl::MapFormat::Hexen2,
  mdl::MapFormat::Daikatana,
  mdl::MapFormat::Quake3_Legacy,
  mdl::MapFormat::Quake3_Valve,
  mdl::MapFormat::Quake3,
};

struct SourceMap
{
  std::filesystem::path path;
  std::unique_ptr<mdl::WorldNode> world;
  mdl::MapFormat sourceFormat = mdl::MapFormat::Unknown;
  mdl::MapFormat targetFormat = mdl::MapFormat::Unknown;
  std::string formatSource;
  std::optional<std::string> game;
  std::vector<std::string> problems;
  size_t droppedPatches = 0;
};

/**
 * The formats to try for a file without a format comment: the formats compatible with the
 * document's format, then the game's formats, then all others.
 */
std::vector<mdl::MapFormat> formatsToTry(const mdl::Map& map)
{
  auto result = mdl::compatibleFormats(map.worldNode().mapFormat());
  for (const auto& formatConfig : map.gameInfo().gameConfig.fileFormats)
  {
    result.push_back(mdl::formatFromName(formatConfig.format));
  }
  std::ranges::copy(AllFormats, std::back_inserter(result));

  auto seen = std::set<mdl::MapFormat>{};
  std::erase_if(result, [&](const auto format) {
    return format == mdl::MapFormat::Unknown || !seen.insert(format).second;
  });
  return result;
}

void collectPatches(mdl::Node& node, std::vector<mdl::Node*>& patches)
{
  if (dynamic_cast<mdl::PatchNode*>(&node))
  {
    patches.push_back(&node);
  }
  for (auto* child : node.children())
  {
    collectPatches(*child, patches);
  }
}

/**
 * Reads the map file at the path argument, detects its format and converts it to the
 * document's format. Patches are dropped if the document's format has none.
 */
Result<SourceMap, ToolError> readSourceMap(CallContext& context, const Args& args)
{
  auto pathResult = absolutePathArgument(args, "path");
  if (pathResult.is_error())
  {
    return errorOf(pathResult);
  }

  auto source = SourceMap{};
  source.path = pathResult.value();
  if (fs::Disk::pathInfo(source.path) != fs::PathInfo::File)
  {
    return makeError(
      ErrorCode::IoError,
      fmt::format("There is no map file at {}.", source.path),
      "Use map_files_list {\"folder\": ...} to find map files.");
  }

  auto stream = std::ifstream{source.path, std::ios::binary};
  auto text = std::string{std::istreambuf_iterator<char>{stream}, {}};
  if (stream.bad())
  {
    return makeError(
      ErrorCode::IoError,
      fmt::format("{} could not be read.", source.path),
      "Check that the file is readable.");
  }

  auto headerStream = std::istringstream{text};
  const auto [headerGame, headerFormat] =
    mdl::readMapHeader(headerStream)
    | kdl::value_or(std::pair{std::optional<std::string>{}, mdl::MapFormat::Unknown});
  source.game = headerGame;

  auto& map = context.map();
  source.targetFormat = map.worldNode().mapFormat();
  source.formatSource = headerFormat != mdl::MapFormat::Unknown ? "header" : "detected";
  const auto candidates = headerFormat != mdl::MapFormat::Unknown
                            ? std::vector<mdl::MapFormat>{headerFormat}
                            : formatsToTry(map);

  auto errors = std::vector<std::string>{};
  for (const auto format : candidates)
  {
    auto status = CollectingParserStatus{map.logger()};
    auto reader = ImportReader{
      text, format, source.targetFormat, map.worldNode().entityPropertyConfig()};
    if (auto world = reader.read(map.worldBounds(), status, map.taskManager()))
    {
      source.world = std::move(world).value();
      source.sourceFormat = format;
      source.problems = std::move(status.problems);
      break;
    }
    else
    {
      errors.push_back(mdl::formatName(format) + ": " + errorMessage(world));
    }
  }

  if (!source.world)
  {
    auto error = makeError(
      ErrorCode::InvalidArgument,
      fmt::format(
        "{} could not be parsed as a map file ({}).",
        source.path,
        kdl::str_join(errors, "; ")),
      "Check that the file is a map file of a game TrenchBroom supports.");
    error.details["parseErrors"] = errors;
    return error;
  }

  if (!mdl::hasPatchSupport(source.targetFormat))
  {
    auto patches = std::vector<mdl::Node*>{};
    collectPatches(*source.world, patches);
    for (auto* patch : patches)
    {
      patch->parent()->removeChild(patch);
      delete patch;
    }
    source.droppedPatches = patches.size();
  }

  return source;
}

void warnSourceProblems(CallContext& context, const SourceMap& source)
{
  if (source.droppedPatches > 0)
  {
    context.warn(
      "PATCHES_DROPPED",
      fmt::format(
        "{} patch(es) were left out because the {} format has no patches.",
        source.droppedPatches,
        mdl::formatName(source.targetFormat)));
  }
  if (
    source.game
    && !kdl::ci::str_is_equal(*source.game, context.map().gameInfo().gameConfig.name))
  {
    context.warn(
      "GAME_MISMATCH",
      fmt::format(
        "The file was made for {}, the document is a {} map; entity classnames and "
        "materials may not exist.",
        *source.game,
        context.map().gameInfo().gameConfig.name));
  }
  for (size_t i = 0; i < source.problems.size() && i < 20; ++i)
  {
    context.warn("PARSE_WARNING", source.problems[i]);
  }
}

std::string classnameOf(const mdl::Node& node)
{
  if (const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(&node))
  {
    return entityNode->entity().classname();
  }
  return "worldspawn";
}

bool isGroup(const mdl::Node& node)
{
  return dynamic_cast<const mdl::GroupNode*>(&node) != nullptr;
}

/** The layers of the source map in the editor's order; index 0 is the default layer. */
std::vector<mdl::LayerNode*> sourceLayers(mdl::WorldNode& world)
{
  return world.allLayersUserSorted();
}

/** Selects the layers named by the `layer` filter (a name or an index). */
Result<std::vector<mdl::LayerNode*>, ToolError> filterLayers(
  const SourceMap& source, const Args& args)
{
  const auto layers = sourceLayers(*source.world);
  const auto filter = args.getOptional<Json>("layer");
  if (!filter)
  {
    return layers;
  }

  auto result = std::vector<mdl::LayerNode*>{};
  if (filter->is_number_integer())
  {
    const auto index = filter->get<int64_t>();
    if (index >= 0 && size_t(index) < layers.size())
    {
      result.push_back(layers[size_t(index)]);
    }
  }
  else
  {
    const auto name = filter->get<std::string>();
    std::ranges::copy_if(layers, std::back_inserter(result), [&](const auto* layer) {
      return kdl::ci::str_is_equal(layer->name(), name);
    });
  }

  if (result.empty())
  {
    auto names = std::vector<std::string>{};
    for (size_t i = 0; i < layers.size(); ++i)
    {
      names.push_back(fmt::format("{} '{}'", i, layers[i]->name()));
    }
    return makeError(
      ErrorCode::InvalidArgument,
      fmt::format(
        "{} has no layer {}. Its layers are: {}.",
        source.path,
        filter->dump(),
        kdl::str_join(names, ", ")),
      "Pass a layer name or index from map_file_inspect.");
  }
  return result;
}

void findGroups(mdl::Node& node, const std::string& name, std::vector<mdl::Node*>& result)
{
  if (auto* groupNode = dynamic_cast<mdl::GroupNode*>(&node);
      groupNode && kdl::ci::str_is_equal(groupNode->name(), name))
  {
    result.push_back(groupNode);
    return;
  }
  for (auto* child : node.children())
  {
    findGroups(*child, name, result);
  }
}

void findByClassname(
  mdl::Node& node, const std::string& pattern, std::vector<mdl::Node*>& result)
{
  if (isGroup(node) || dynamic_cast<mdl::LayerNode*>(&node))
  {
    for (auto* child : node.children())
    {
      findByClassname(*child, pattern, result);
    }
  }
  else if (kdl::ci::str_matches_glob(classnameOf(node), pattern))
  {
    // an entity with its brushes, or a brush or patch of the world
    result.push_back(&node);
  }
}

bool overlaps(const vm::bbox3d& lhs, const vm::bbox3d& rhs)
{
  for (size_t i = 0; i < 3; ++i)
  {
    if (lhs.max[i] <= rhs.min[i] || lhs.min[i] >= rhs.max[i])
    {
      return false;
    }
  }
  return true;
}

/**
 * The objects to import: the children of the chosen layers, narrowed to the named groups
 * (outermost matches), then to entities (or world brushes for "worldspawn") matching the
 * classname, then to objects whose bounds intersect or lie inside the region.
 */
Result<std::vector<mdl::Node*>, ToolError> filterObjects(
  const SourceMap& source, const Args& args)
{
  auto layers = filterLayers(source, args);
  if (layers.is_error())
  {
    return errorOf(layers);
  }

  auto nodes = std::vector<mdl::Node*>{};
  for (auto* layer : layers.value())
  {
    std::ranges::copy(layer->children(), std::back_inserter(nodes));
  }

  auto filters = std::vector<std::string>{};
  if (const auto group = args.getOptional<std::string>("group"))
  {
    auto groups = std::vector<mdl::Node*>{};
    for (auto* node : nodes)
    {
      findGroups(*node, *group, groups);
    }
    nodes = std::move(groups);
    filters.push_back(fmt::format("group '{}'", *group));
  }

  if (const auto classname = args.getOptional<std::string>("classname"))
  {
    auto matches = std::vector<mdl::Node*>{};
    for (auto* node : nodes)
    {
      findByClassname(*node, *classname, matches);
    }
    nodes = std::move(matches);
    filters.push_back(fmt::format("classname '{}'", *classname));
  }

  if (const auto region = args.getOptional<vm::bbox3d>("region"))
  {
    const auto inside = args.get<std::string>("regionMode") == "inside";
    std::erase_if(nodes, [&](const auto* node) {
      const auto& bounds = node->logicalBounds();
      return inside ? !region->contains(bounds) : !overlaps(*region, bounds);
    });
    filters.push_back(fmt::format(
      "{} region {}", inside ? "inside" : "intersecting", toJson(*region).dump()));
  }

  if (nodes.empty())
  {
    return makeError(
      ErrorCode::InvalidArgument,
      fmt::format(
        "Nothing in {} matches the filter{}.",
        source.path,
        filters.empty() ? "" : " (" + kdl::str_join(filters, ", ") + ")"),
      "Use map_file_inspect to list the file's layers, groups and classnames.");
  }
  return nodes;
}

// map_file_inspect

struct Counts
{
  size_t brushes = 0;
  size_t patches = 0;
  size_t entities = 0;
  size_t groups = 0;
};

void count(const mdl::Node& node, Counts& counts)
{
  node.accept(kdl::overload(
    [&](auto&& thisLambda, const mdl::WorldNode& worldNode) {
      worldNode.visitChildren(thisLambda);
    },
    [&](auto&& thisLambda, const mdl::LayerNode& layerNode) {
      layerNode.visitChildren(thisLambda);
    },
    [&](auto&& thisLambda, const mdl::GroupNode& groupNode) {
      ++counts.groups;
      groupNode.visitChildren(thisLambda);
    },
    [&](auto&& thisLambda, const mdl::EntityNode& entityNode) {
      ++counts.entities;
      entityNode.visitChildren(thisLambda);
    },
    [&](const mdl::BrushNode&) { ++counts.brushes; },
    [&](const mdl::PatchNode&) { ++counts.patches; }));
}

Json countsJson(const mdl::Node& node)
{
  auto counts = Counts{};
  count(node, counts);
  return Json{
    {"brushes", counts.brushes},
    {"patches", counts.patches},
    {"entities", counts.entities},
    {"groups", counts.groups},
  };
}

void collectGroupsJson(
  const mdl::Node& node,
  const std::string& layerName,
  const std::string& parentName,
  Json& groups)
{
  for (const auto* child : node.children())
  {
    if (const auto* groupNode = dynamic_cast<const mdl::GroupNode*>(child))
    {
      if (groups.size() < MaxListItems)
      {
        groups.push_back(Json{
          {"name", groupNode->name()},
          {"layer", layerName},
          {"parent", parentName.empty() ? Json{} : Json(parentName)},
          {"bounds", toJson(groupNode->logicalBounds())},
          {"contents", countsJson(*groupNode)},
          {"materials",
           materialsOf(std::vector{static_cast<const mdl::Node*>(groupNode)})},
        });
      }
      collectGroupsJson(*groupNode, layerName, groupNode->name(), groups);
    }
  }
}

void collectClassnames(const mdl::Node& node, std::map<std::string, size_t>& classnames)
{
  for (const auto* child : node.children())
  {
    if (const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(child))
    {
      ++classnames[entityNode->entity().classname()];
    }
    else if (isGroup(*child))
    {
      collectClassnames(*child, classnames);
    }
  }
}

ToolResult mapFileInspect(CallContext& context, const Args& args)
{
  auto source = readSourceMap(context, args);
  if (source.is_error())
  {
    return errorOf(source);
  }
  auto& sourceMap = source.value();
  warnSourceProblems(context, sourceMap);

  auto layers = Json::array();
  auto groups = Json::array();
  auto classnames = std::map<std::string, size_t>{};
  const auto sourceLayerNodes = sourceLayers(*sourceMap.world);
  for (size_t i = 0; i < sourceLayerNodes.size(); ++i)
  {
    const auto* layer = sourceLayerNodes[i];
    const auto children = layer->children();
    layers.push_back(Json{
      {"index", i},
      {"name", layer->name()},
      {"default", layer == sourceMap.world->defaultLayer()},
      {"bounds", children.empty() ? Json{} : toJson(*boundsOf(children))},
      {"contents", countsJson(*layer)},
    });
    collectGroupsJson(*layer, layer->name(), "", groups);
    collectClassnames(*layer, classnames);
  }

  auto classnamesJson = Json::array();
  for (const auto& [classname, count] : classnames)
  {
    classnamesJson.push_back(Json{{"classname", classname}, {"count", count}});
  }

  const auto materials = materialsOf(std::vector{sourceMap.world.get()});
  const auto missing = missingMaterials(context.map(), materials);
  auto allNodes = std::vector<mdl::Node*>{};
  for (auto* layer : sourceLayerNodes)
  {
    std::ranges::copy(layer->children(), std::back_inserter(allNodes));
  }

  return Json{
    {"path", sourceMap.path.string()},
    {"format", mdl::formatName(sourceMap.sourceFormat)},
    {"formatSource", sourceMap.formatSource},
    {"game", sourceMap.game ? Json(*sourceMap.game) : Json{}},
    {"documentFormat", mdl::formatName(sourceMap.targetFormat)},
    {"converted", sourceMap.sourceFormat != sourceMap.targetFormat},
    {"bounds", allNodes.empty() ? Json{} : toJson(*boundsOf(allNodes))},
    {"contents", countsJson(*sourceMap.world)},
    {"layers", std::move(layers)},
    {"groups", std::move(groups)},
    {"classnames", std::move(classnamesJson)},
    {"materials", materials},
    {"missingMaterials", missing},
  };
}

// map_import with replaceLayer / replaceGroup, layer_replace

/** What an import replaces: the objects of a layer, or a group. */
struct Replacement
{
  ObjectKind kind = ObjectKind::Layer;
  /** The name of the layer or group. */
  std::string name;
  /** The layer that receives the imported objects (nullptr: create it). */
  mdl::LayerNode* layer = nullptr;
  /** replaceGroup: the group that contains the replaced group, if any. */
  mdl::GroupNode* parentGroup = nullptr;
  /** The objects that are deleted. */
  std::vector<mdl::Node*> removed;
  /** No layer or group had the name; the import creates it. */
  bool created = false;
};

/**
 * Resolves the layer or group to replace: an id, a name address or a plain name. A plain
 * name that no layer or group has is created by the import, so that a replacement can be
 * repeated from the first run on.
 */
Result<std::optional<Replacement>, ToolError> replacementArgument(
  CallContext& context, const Args& args)
{
  const auto layerValue = args.getOptional<std::string>("replaceLayer");
  const auto groupValue = args.getOptional<std::string>("replaceGroup");
  if (!layerValue && !groupValue)
  {
    return std::nullopt;
  }
  if (layerValue && groupValue)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Pass either 'replaceLayer' or 'replaceGroup', not both.",
      "replaceLayer replaces the objects of a layer, replaceGroup one group.");
  }

  const auto& ids = context.ids();
  const auto kind = layerValue ? ObjectKind::Layer : ObjectKind::Group;
  const auto& value = layerValue ? *layerValue : *groupValue;
  const auto key = layerValue ? "replaceLayer" : "replaceGroup";

  auto* node = static_cast<mdl::Node*>(nullptr);
  const auto& name = value;
  if (parseNameAddress(value) || parseObjectRef(value))
  {
    auto resolved = ids.resolve(value);
    if (resolved.is_error())
    {
      return errorOf(resolved);
    }
    node = resolved.value();
    if (objectKindOf(*node) != kind)
    {
      return makeError(
        ErrorCode::WrongObjectKind,
        fmt::format("'{}' of {} is not a {}.", value, key, toString(kind)),
        fmt::format("Pass a {} id, a name address or a name.", toString(kind)),
        {ids.format(*node)});
    }
  }
  else
  {
    auto resolved = ids.resolve(NameAddress{kind, value});
    if (resolved.is_success())
    {
      node = resolved.value();
    }
    else if (errorOf(resolved).code != ErrorCode::ObjectNotFound)
    {
      return errorOf(resolved);
    }
  }

  auto replacement = Replacement{};
  replacement.kind = kind;
  if (auto* layerNode = dynamic_cast<mdl::LayerNode*>(node))
  {
    replacement.name = layerNode->name();
    replacement.layer = layerNode;
    replacement.removed = layerNode->children();
  }
  else if (auto* groupNode = dynamic_cast<mdl::GroupNode*>(node))
  {
    replacement.name = groupNode->name();
    replacement.layer = mdl::findContainingLayer(groupNode);
    replacement.parentGroup = dynamic_cast<mdl::GroupNode*>(groupNode->parent());
    replacement.removed = {groupNode};
  }
  else
  {
    // created by the import; a new group goes into the target layer
    replacement.name = name;
    replacement.created = true;
    if (kind == ObjectKind::Group)
    {
      auto targetLayer = targetLayerArgument(context, args);
      if (targetLayer.is_error())
      {
        return errorOf(targetLayer);
      }
      replacement.layer = targetLayer.value()
                            ? targetLayer.value()
                            : context.map().editorContext().currentLayer();
    }
  }

  if (node && args.has("targetLayer"))
  {
    return makeError(
      ErrorCode::InvalidArgument,
      fmt::format("'targetLayer' cannot be combined with an existing {}.", key),
      "The imported objects replace the old ones where they are; omit targetLayer.");
  }
  if (replacement.layer && replacement.layer->locked())
  {
    return makeError(
      ErrorCode::ObjectNotEditable,
      fmt::format("Layer '{}' is locked.", replacement.layer->name()),
      fmt::format(
        "Unlock it with layer_set_state {{\"layer\": \"{}\", \"locked\": false}}.",
        ids.format(*replacement.layer)),
      {ids.format(*replacement.layer)});
  }
  return replacement;
}

/** Whether the node is one of the nodes or inside one of them. */
bool isInside(const mdl::Node* node, const std::vector<mdl::Node*>& nodes)
{
  for (; node; node = node->parent())
  {
    if (std::ranges::find(nodes, node) != nodes.end())
    {
      return true;
    }
  }
  return false;
}

/**
 * Prepares the import of a replacement: closes open groups so that the imported objects
 * go into the layer, and creates the layer if needed.
 */
Result<void, ToolError> prepareReplacement(CallContext& context, Replacement& replacement)
{
  auto& map = context.map();

  if (const auto* openGroup = map.editorContext().currentGroup())
  {
    const auto closedReplaced = isInside(openGroup, replacement.removed);
    while (map.editorContext().currentGroup())
    {
      mdl::closeGroup(map);
    }
    if (!closedReplaced)
    {
      context.warn(
        "GROUP_CLOSED",
        "The open groups were closed so that the imported objects go into the layer.");
    }
  }

  if (!replacement.layer)
  {
    auto& worldNode = map.worldNode();
    const auto customLayers = worldNode.customLayersUserSorted();
    auto layer = mdl::Layer{replacement.name};
    layer.setSortIndex(
      !customLayers.empty() ? customLayers.back()->layer().sortIndex() + 1 : 0);
    auto* layerNode = new mdl::LayerNode{std::move(layer)};
    if (mdl::addNodes(map, {{&worldNode, {layerNode}}}).empty())
    {
      return context.operationFailed(
        "The layer '" + replacement.name + "' could not be created.");
    }
    replacement.layer = layerNode;
  }
  return Result<void, ToolError>{};
}

/**
 * Deletes the replaced objects after the import (so that a parent group of a replaced
 * group, which the new group has joined, is not removed as empty) and selects the
 * imported objects again. Returns the number of deleted objects including their contents.
 */
size_t removeReplaced(
  CallContext& context,
  const Replacement& replacement,
  const std::vector<mdl::Node*>& imported)
{
  auto& map = context.map();
  auto count = size_t(0);
  for (const auto* node : replacement.removed)
  {
    count += 1 + node->descendantCount();
  }
  if (!replacement.removed.empty())
  {
    mdl::deselectAll(map);
    mdl::removeNodes(map, replacement.removed);
    const auto& editorContext = map.editorContext();
    auto selectable = std::vector<mdl::Node*>{};
    std::ranges::copy_if(imported, std::back_inserter(selectable), [&](const auto* node) {
      return editorContext.selectable(*node);
    });
    mdl::selectNodes(map, selectable);
  }
  return count;
}

/**
 * Puts the imported (selected) objects in their place: into one group with the replaced
 * group's name (an imported single group is renamed instead), inside the replaced group's
 * parent group.
 */
Result<mdl::GroupNode*, ToolError> groupImported(
  CallContext& context, const Replacement& replacement, std::vector<mdl::Node*>& imported)
{
  auto& map = context.map();
  auto* groupNode = static_cast<mdl::GroupNode*>(nullptr);
  if (imported.size() == 1 && dynamic_cast<mdl::GroupNode*>(imported.front()))
  {
    groupNode = static_cast<mdl::GroupNode*>(imported.front());
    mdl::deselectAll(map);
    mdl::selectNodes(map, {groupNode});
    mdl::renameSelectedGroups(map, replacement.name);
  }
  else
  {
    mdl::deselectAll(map);
    mdl::selectNodes(map, imported);
    groupNode = mdl::groupSelectedNodes(map, replacement.name);
    if (!groupNode)
    {
      return context.operationFailed(
        "The imported objects could not be grouped as '" + replacement.name + "'.");
    }
  }

  if (replacement.parentGroup && groupNode->parent() != replacement.parentGroup)
  {
    mdl::deselectAll(map);
    if (!mdl::reparentNodes(map, {{replacement.parentGroup, {groupNode}}}))
    {
      return context.operationFailed(
        "The group could not be moved into "
        + context.ids().format(*replacement.parentGroup) + ".");
    }
  }
  imported = {groupNode};
  return groupNode;
}

// map_import

ToolResult mapImport(CallContext& context, const Args& args)
{
  auto placement = placementArgument(args);
  if (placement.is_error())
  {
    return errorOf(placement);
  }
  auto replacement = replacementArgument(context, args);
  if (replacement.is_error())
  {
    return errorOf(replacement);
  }
  auto targetLayer = targetLayerArgument(context, args);
  if (targetLayer.is_error())
  {
    return errorOf(targetLayer);
  }

  auto source = readSourceMap(context, args);
  if (source.is_error())
  {
    return errorOf(source);
  }
  auto& sourceMap = source.value();
  warnSourceProblems(context, sourceMap);

  auto nodes = filterObjects(sourceMap, args);
  if (nodes.is_error())
  {
    return errorOf(nodes);
  }

  // the source world already has the document's format, so the text can be pasted
  auto& map = context.map();
  const auto text = serializeNodes(*sourceMap.world, nodes.value(), map.taskManager());

  // a replacement deletes the old objects in the same undo step
  auto plan = std::move(replacement).value();
  if (plan)
  {
    if (auto prepared = prepareReplacement(context, *plan); prepared.is_error())
    {
      return errorOf(prepared);
    }
  }

  auto result = pasteObjects(
    context,
    text,
    placement.value(),
    plan ? plan->layer : targetLayer.value(),
    uvModeArgument(args));
  if (result.is_error())
  {
    return result;
  }

  auto json = std::move(result).value();
  if (plan)
  {
    auto& ids = context.ids();
    auto imported = std::vector<mdl::Node*>{};
    for (const auto& id : json["ids"])
    {
      imported.push_back(ids.resolve(id.get<std::string>()).value());
    }

    auto replaced = Json{
      {"kind", toString(plan->kind)},
      {"created", plan->created},
      {"removedObjects", 0},
      {"importedObjects", json["count"]},
    };
    if (plan->kind == ObjectKind::Group)
    {
      auto group = groupImported(context, *plan, imported);
      if (group.is_error())
      {
        return errorOf(group);
      }
      replaced["removedObjects"] = removeReplaced(context, *plan, imported);
      replaced["group"] = ids.format(*group.value());
      replaced["address"] = *nameAddressOf(*group.value());
      json["ids"] = formatIds(imported, ids);
      json["objects"] = summaries(imported, ids);
      json["count"] = imported.size();
    }
    else
    {
      replaced["removedObjects"] = removeReplaced(context, *plan, imported);
      replaced["layer"] = ids.format(*plan->layer);
      replaced["address"] = *nameAddressOf(*plan->layer);
    }
    json["replaced"] = std::move(replaced);
  }
  json["path"] = sourceMap.path.string();
  json["sourceFormat"] = mdl::formatName(sourceMap.sourceFormat);
  json["formatSource"] = sourceMap.formatSource;
  json["documentFormat"] = mdl::formatName(sourceMap.targetFormat);
  json["converted"] = sourceMap.sourceFormat != sourceMap.targetFormat;
  json["sourceObjects"] = nodes.value().size();
  json.erase("pasteType");
  return json;
}

ToolResult layerReplace(CallContext& context, const Args& args)
{
  // map_import with replaceLayer; 'sourceLayer' is map_import's 'layer' filter
  auto json = args.json();
  json["replaceLayer"] = json["layer"];
  json.erase("layer");
  if (auto sourceLayer = json.find("sourceLayer"); sourceLayer != json.end())
  {
    json["layer"] = *sourceLayer;
    json.erase(sourceLayer);
  }
  return mapImport(context, Args{std::move(json)});
}

// Schemas

/** Adds the placement arguments of clipboard_paste and map_import. */
void addPlacementFields(std::vector<Field>& fields)
{
  fields.push_back(
    field("position", vec3())
      .describe("Place the objects so that the anchor of their bounds lands on "
                "this point (map units). Default: keep the original coordinates"));
  fields.push_back(
    field("anchor", enumOf({"min", "center", "max", "bottomCenter"}).defaultsTo("min"))
      .describe("With position: the point of the bounds that lands on it: 'min' corner, "
                "'center', 'max' corner, or 'bottomCenter' (center of the bottom face)"));
  fields.push_back(
    field("offset", vec3())
      .describe("Move the objects by this vector (map units) instead of 'position'"));
  fields.push_back(
    field("snapToGrid", boolean().defaultsTo(false))
      .describe(
        "Snap the bounds' min corner to the current grid after placing, like pasting in "
        "the editor"));
  fields.push_back(
    field("targetLayer", objectId({ObjectKind::Layer}))
      .describe("Layer to put the objects in. Default: the current layer"));
  fields.push_back(
    field("uv", enumOf({"keep", "typical", "fit", "world"}).defaultsTo("keep"))
      .describe(
        "Texture alignment of the new faces, from the material profiles "
        "(material_usage): keep (as in the source); typical (the material's typical "
        "scale, justified to each face's edge, like uv_align typical); fit (the typical "
        "scale rounded to whole repeats per face); world (world-aligned axes at the "
        "typical scale, offset 0: textures continue across brushes). Faces whose "
        "material is not loaded keep their UVs"));
}

std::vector<Field> pasteOutputFields()
{
  return {
    field("ids", array(string())).describe("The new objects (selected)"),
    field("count", integer()).describe("Number of new objects"),
    field("objects", array(any())).describe("Summaries of the new objects (at most 100)"),
    field("truncated", boolean()).describe("Whether 'objects' was cut at 100"),
    field("bounds", any()).describe("Bounds of the new objects"),
    field("offset", vec3()).describe("The translation applied to the objects"),
    field("placement", any())
      .describe("{mode: 'original' | 'point' | 'offset', position, anchor, offset, "
                "snapToGrid}"),
    field("layer", any()).describe("The layer of the new objects"),
    field("missingMaterials", array(string()))
      .describe("Materials of the new objects that no material collection provides"),
    field("uv", any())
      .describe("With uv other than keep: {mode, faces (aligned), skipped (material not "
                "loaded), typicalScales: {material: {scale, source}}}"),
  };
}

} // namespace

void registerClipboardTools(ToolRegistry& registry)
{
  registry.add(
    ToolDef{"clipboard_copy"}
      .title("Copy")
      .description(
        "Copies objects or faces as map text in the document's format, like Edit > Copy, "
        "into the server's clipboard and returns the text. The map is not changed; the "
        "clipboard is one for all sessions and documents, and the operating system's "
        "clipboard is not touched. Objects: 'ids' or the selected objects. Faces: "
        "'faces' or the selected faces; face text carries the material and alignment, "
        "and clipboard_paste applies the last copied face to other faces. Returns mode, "
        "ids, count, lineCount, bytes and text. "
        "Examples: {\"ids\": [\"group:12\"]}; {\"faces\": [\"brush:12/face:3\"], "
        "\"includeText\": false}")
      .input(object({
        idsField(ClipboardKinds, "Objects to copy. Default: the selection"),
        field("faces", faceTargetsField().schema)
          .describe(
            "Faces to copy instead of objects (face ids, or brush / group / entity ids "
            "for all their faces). Default: the selected faces if faces are selected"),
        field("includeText", boolean().defaultsTo(true))
          .describe("Return the text; false returns only its size"),
      }))
      .output(object({
        field("mode", enumOf({"objects", "faces"})).required(),
        field("ids", array(string())).required().describe("The copied objects or faces"),
        field("count", integer())
          .required()
          .describe("Number of copied objects or faces"),
        field("lineCount", integer()).required().describe("Lines of the map text"),
        field("bytes", integer()).required().describe("Size of the map text in bytes"),
        field("text", string())
          .describe("The map text (omitted if includeText is false)"),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .handler(clipboardCopy));

  registry.add(
    ToolDef{"clipboard_cut"}
      .title("Cut")
      .description(
        "Copies objects to the server's clipboard (see clipboard_copy) and deletes them, "
        "like Edit > Cut, in one undo step. A dry run leaves the clipboard unchanged. "
        "Example: {\"ids\": [\"brush:12\", \"entity:40\"]}")
      .input(object({
        idsField(ClipboardKinds, "Objects to cut. Default: the selection"),
        field("includeText", boolean().defaultsTo(true))
          .describe("Return the text; false returns only its size"),
      }))
      .output(object({
        field("mode", enumOf({"objects"})).required(),
        field("ids", array(string())).required().describe("The cut objects"),
        field("count", integer()).required().describe("Number of cut objects"),
        field("lineCount", integer()).required().describe("Lines of the map text"),
        field("bytes", integer()).required().describe("Size of the map text in bytes"),
        field("text", string())
          .describe("The map text (omitted if includeText is false)"),
      }))
      .mutation(Mutation::Map)
      .destructive()
      .handler(clipboardCut));

  auto pasteInput = std::vector<Field>{
    field("text", string().nonEmpty())
      .describe("Map text to paste. Default: the server's clipboard"),
    field("faces", faceTargetsField().schema)
      .describe(
        "Face text only: the faces that get the attributes. Default: the selected faces, "
        "or all faces of the selected objects"),
  };
  addPlacementFields(pasteInput);
  auto pasteOutput = pasteOutputFields();
  pasteOutput.insert(
    pasteOutput.begin(), field("pasteType", enumOf({"objects", "faces"})).required());
  pasteOutput.push_back(
    field("faces", array(string())).describe("Face paste: the changed faces"));
  pasteOutput.push_back(
    field("material", string()).describe("Face paste: the material applied"));

  registry.add(
    ToolDef{"clipboard_paste"}
      .title("Paste")
      .description(
        "Pastes map text (default: the server's clipboard) like Edit > Paste, in one "
        "undo step. Object text (entities, brushes, groups in the document's format or a "
        "compatible one) "
        "becomes new objects in the current layer (or 'targetLayer'), at their original "
        "coordinates, at 'position' (map units; the 'anchor' of their bounds lands on "
        "the point) or moved by 'offset' (not both); they are selected. Face text "
        "(brush face lines from clipboard_copy with 'faces') applies the material and "
        "alignment of its last face to 'faces' or the selected faces. Materials missing "
        "from the material collections are reported (MISSING_MATERIALS). 'uv' aligns "
        "the textures of the new faces from the material profiles (typical, fit, world; "
        "default keep). Use map_import to paste from a map file. Examples: "
        "{\"position\": [512, 0, 0], \"anchor\": \"min\"}; {\"offset\": [0, 256, 0], "
        "\"uv\": \"typical\"}; {\"faces\": [\"brush:12/face:3\"]}")
      .input(object(std::move(pasteInput)))
      .output(object(std::move(pasteOutput)))
      .mutation(Mutation::Map)
      .handler(clipboardPaste));

  registry.add(
    ToolDef{"map_file_inspect"}
      .title("Inspect Map File")
      .description(
        "Reads another map file (absolute path) without opening it and lists what "
        "map_import can take from it; read-only. Lists "
        "its format (from the header comment or detected), "
        "layers (index 0 is the default layer), groups (name, layer, bounds, contents, "
        "materials), entity classnames, materials and the materials missing from the "
        "document's collections. Example: {\"path\": \"/maps/prefabs/rooms.map\"}")
      .input(object({
        field("path", string().nonEmpty())
          .required()
          .describe("Absolute path of the map"),
      }))
      .output(object({
        field("path", string()).required(),
        field("format", string()).required().describe("The file's map format"),
        field("formatSource", enumOf({"header", "detected"}))
          .required()
          .describe("Whether the format comes from the header comment or was detected"),
        field("game", any()).describe("The game named in the header, or null"),
        field("documentFormat", string())
          .required()
          .describe("The document's map format"),
        field("converted", boolean())
          .required()
          .describe("Whether map_import converts the objects to the document's format"),
        field("bounds", any()).required().describe("Bounds of all objects in the file"),
        field("contents", any()).required().describe("Counts by kind"),
        field("layers", array(any()))
          .required()
          .describe("The file's layers; index 0 is the default layer"),
        field("groups", array(any()))
          .required()
          .describe("Groups {name, layer, bounds, contents, materials}; at most 500"),
        field("classnames", array(any())).required().describe("Entity classnames"),
        field("materials", array(string()))
          .required()
          .describe("Materials the file uses"),
        field("missingMaterials", array(string()))
          .required()
          .describe("Materials the document's collections do not provide"),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(mapFileInspect));

  const auto sourceLayerSchema = oneOf({
    string().nonEmpty().describe("Layer name"),
    integer().min(0).describe("Layer index (0 = default layer)"),
  });
  auto sourceFilters = std::vector<Field>{
    field("group", string().nonEmpty())
      .describe("Only groups with this name (outermost matches, with their contents)"),
    field("classname", string().nonEmpty())
      .describe(
        "Only entities with this classname glob, with their brushes ('worldspawn': "
        "world brushes); inside groups too, without the groups"),
    field("region", box())
      .describe("Only objects whose bounds meet this box (map units, see regionMode)"),
    field("regionMode", enumOf({"intersects", "inside"}).defaultsTo("intersects"))
      .describe("'intersects' (overlap) or 'inside' (bounds within the region)"),
  };
  auto importInput = std::vector<Field>{
    field("path", string().nonEmpty()).required().describe("Absolute path of the map"),
    field("layer", sourceLayerSchema)
      .describe(
        "Only objects in this layer of the file: name or index (0 = default layer)"),
  };
  importInput.insert(importInput.end(), sourceFilters.begin(), sourceFilters.end());
  importInput.push_back(
    field("replaceLayer", string().nonEmpty())
      .describe(
        "Replace the objects of this layer of the document (id, 'layer:@name' or a "
        "name; a name no layer has creates the layer) with the imported ones, in the "
        "same undo step"));
  importInput.push_back(
    field("replaceGroup", string().nonEmpty())
      .describe(
        "Replace this group of the document (id, 'group:@name' or a name; a name no "
        "group has creates the group) with a group of the same name holding the "
        "imported objects, in the same undo step"));
  addPlacementFields(importInput);
  auto replacedOutput =
    field("replaced", any())
      .describe(
        "With replaceLayer / replaceGroup: {kind, layer | group, address, created, "
        "removedObjects (including contents), importedObjects}");
  auto importOutput = pasteOutputFields();
  importOutput.push_back(replacedOutput);
  importOutput.push_back(field("path", string()).required());
  importOutput.push_back(
    field("sourceFormat", string()).required().describe("The file's map format"));
  importOutput.push_back(field("formatSource", enumOf({"header", "detected"}))
                           .required()
                           .describe("Whether the format comes from the header comment "
                                     "or was detected"));
  importOutput.push_back(field("documentFormat", string()).required());
  importOutput.push_back(
    field("converted", boolean())
      .required()
      .describe("Whether the objects were converted to the document's format"));
  importOutput.push_back(
    field("sourceObjects", integer()).required().describe("Objects taken from the file"));

  auto replaceOutput = importOutput;
  registry.add(
    ToolDef{"map_import"}
      .title("Import Map")
      .description(
        "Imports all or part of another map file (absolute path, any format TrenchBroom "
        "reads; the format comes from the header comment or is detected) into the "
        "document in one undo step. Filters (combined): 'layer', 'group', 'classname', "
        "'region'; use map_file_inspect to list them. Faces are converted to the "
        "document's format while reading (e.g. Valve 220 to Standard); patches are "
        "dropped for formats without patches. Layers are not imported: the objects go "
        "into the current layer or 'targetLayer'. Placement as in clipboard_paste: "
        "original coordinates, 'position' + 'anchor' or 'offset' (map units). Materials "
        "missing "
        "from the document's collections are listed in missingMaterials "
        "(MISSING_MATERIALS warning). 'uv' aligns the imported faces in the same undo "
        "step from the material profiles: typical (typical scale, justified per face), "
        "fit (typical scale rounded to whole repeats), world (world-aligned at the "
        "typical scale, continuous across brushes); default keep, so generated maps "
        "need not compute UVs. For bulk imports into an unfinished map, pass checks: "
        "\"defer\" (or detail: \"summary\" for issuesSummary). The imported objects are "
        "selected. replaceLayer / replaceGroup delete a layer's objects or a group and "
        "import in their place in one undo step (safe to repeat after regenerating the "
        "file; see layer_replace); dryRun shows the counts. Examples: "
        "{\"path\": \"/maps/prefabs/rooms.map\", \"group\": \"Armory\", \"position\": "
        "[512, 0, 0], \"anchor\": \"min\"}; {\"path\": \"/tmp/shell.map\", \"uv\": "
        "\"world\", \"checks\": \"defer\"}; {\"path\": \"/maps/e1m1.map\", "
        "\"region\": {\"min\": [0, 0, 0], \"max\": [512, 512, 256]}, \"regionMode\": "
        "\"inside\"}; {\"path\": \"/tmp/gen/bar.map\", \"replaceGroup\": \"Bar\", "
        "\"detail\": \"summary\"}")
      .input(object(std::move(importInput)))
      .output(object(std::move(importOutput)))
      .mutation(Mutation::Map)
      .handler(mapImport));

  auto replaceInput = std::vector<Field>{
    field("layer", string().nonEmpty())
      .required()
      .describe(
        "The document's layer to replace: id, 'layer:@name' or a name (a name no layer "
        "has creates the layer)"),
    field("path", string().nonEmpty()).required().describe("Absolute path of the map"),
    field("sourceLayer", sourceLayerSchema)
      .describe(
        "Only objects in this layer of the file: name or index (0 = default layer)"),
  };
  replaceInput.insert(replaceInput.end(), sourceFilters.begin(), sourceFilters.end());
  addPlacementFields(replaceInput);
  std::erase_if(replaceInput, [](const auto& f) { return f.name == "targetLayer"; });

  registry.add(
    ToolDef{"layer_replace"}
      .title("Replace Layer")
      .description(
        "Replaces the objects of a layer with the objects of a map file in one undo "
        "step: deletes everything in the layer, then imports the file into it like "
        "map_import (same filters, placement and uv; sourceLayer is map_import's "
        "layer). The layer keeps its id, name and state; a layer name that does not "
        "exist yet creates the layer, so a script can regenerate a part of the map and "
        "call it again and again without duplicates. A failure changes nothing. Returns "
        "replaced {kind, layer, address, created, removedObjects, importedObjects} and "
        "the new ids (lists cut per detail; detail: \"summary\" keeps large results "
        "small); dryRun reports the counts without changing the map. Fails with "
        "OBJECT_NOT_EDITABLE for a locked layer and AMBIGUOUS_NAME if several layers "
        "have the name. Examples: {\"layer\": \"Bar\", \"path\": \"/tmp/gen/bar.map\"}; "
        "{\"layer\": \"layer:@Lights\", \"path\": \"/tmp/gen/level.map\", "
        "\"sourceLayer\": \"Lights\", \"detail\": \"summary\", \"checks\": \"defer\"}")
      .input(object(std::move(replaceInput)))
      .output(object(std::move(replaceOutput)))
      .mutation(Mutation::Map)
      .idempotent()
      .destructive()
      .handler(layerReplace));
}

} // namespace tb::mcp
