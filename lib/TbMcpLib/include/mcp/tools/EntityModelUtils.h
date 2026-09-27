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
#include "mdl/ModelSpecification.h"

#include "vm/bbox.h"
#include "vm/mat.h"

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tb
{
class Logger;

namespace el
{
class VariableStore;
}

namespace mdl
{
class Entity;
class EntityModel;
class EntityModelData;
class EntityModelFrame;
class EntityNode;
class GameFileSystem;
class Map;
class ModelDefinition;
class Node;
struct EntityDefinition;
} // namespace mdl
} // namespace tb

namespace tb::mcp
{
class CallContext;
class IdRegistry;

// Helpers of model-aware placement (epic E11): entity models loaded on demand, the
// property that selects a model's frame (animation), the model bounds of each animation
// and placement checks against brushes with the model bounds.

/**
 * Loads entity models from the game file system for the duration of one call. The editor
 * loads models asynchronously, so a model is often not loaded yet (e.g. right after an
 * entity was created); this loader reads it synchronously instead. Every path is loaded
 * at most once, failures included; the game file system is created on first use.
 */
class EntityModelLoader
{
private:
  struct Entry
  {
    std::unique_ptr<mdl::EntityModel> model;
    std::string error;
  };

  const mdl::Map& m_map;
  std::unique_ptr<Logger> m_logger;
  std::unique_ptr<mdl::GameFileSystem> m_fileSystem;
  std::map<std::filesystem::path, Entry> m_models;

public:
  explicit EntityModelLoader(const mdl::Map& map);
  ~EntityModelLoader();

  EntityModelLoader(const EntityModelLoader&) = delete;
  EntityModelLoader& operator=(const EntityModelLoader&) = delete;

  const mdl::Map& map() const;

  /** The model at the given path, or an error message if it cannot be loaded. */
  Result<const mdl::EntityModel*> load(const std::filesystem::path& path);
};

/** The point entities among the given nodes and their descendants, without duplicates. */
std::vector<mdl::EntityNode*> pointEntities(const std::vector<mdl::Node*>& nodes);

/** The model of a point entity as the editor would show it. */
struct EntityModelState
{
  mdl::ModelSpecification specification;
  /** The loaded model; never null. */
  const mdl::EntityModel* model = nullptr;
  /** Whether the model is the entity's own model loaded by the editor. */
  bool loadedByEditor = false;
  /** Model space to world space: origin, rotation and scale of the entity. */
  vm::mat4x4d transformation;

  const mdl::EntityModelData& data() const;
  /** The frame the entity shows, or null if its index is out of range. */
  const mdl::EntityModelFrame* frame() const;
  /** The world bounds of the given frame. */
  vm::bbox3d worldBounds(const mdl::EntityModelFrame& frame) const;
  /** The world bounds of the frame the entity shows, if that frame exists. */
  std::optional<vm::bbox3d> worldBounds() const;
};

/**
 * The model of a point entity: the entity's own model if the editor has loaded it,
 * otherwise the model loaded by the given loader. Fails if the entity has no definition
 * with a model, the model expression cannot be evaluated or the model cannot be loaded.
 */
Result<EntityModelState> resolveEntityModel(
  const mdl::Entity& entity, EntityModelLoader& loader);

/**
 * The variable of a model expression that selects the model frame, found by evaluating
 * the expression with the given variables: the first variable the evaluation reads such
 * that setting it to a number n makes the expression select frame n (checked for several
 * values) without changing the model path. Nullopt if the frame is fixed or depends on
 * variables only indirectly (e.g. a spawnflag that picks another frame).
 */
std::optional<std::string> findFrameProperty(
  const mdl::ModelDefinition& definition, const el::VariableStore& variables);

/** findFrameProperty for the model definition and properties of a point entity. */
std::optional<std::string> findFrameProperty(const mdl::Entity& entity);

/**
 * The value of the frame property that makes the model expression select the given
 * frame, or nullopt if no such value is found.
 */
std::optional<std::string> framePropertyValue(
  const mdl::ModelDefinition& definition,
  const el::VariableStore& variables,
  const std::string& property,
  size_t frameIndex);

/** One frame of a model: a Quake frame or an animation (e.g. a studio model sequence). */
struct ModelAnimation
{
  size_t index;
  /** The frame or animation name; nullopt if the format has no names. */
  std::optional<std::string> name;
  /** Bounds in model space, relative to the entity's origin before rotation and scale. */
  vm::bbox3d bounds;
  /** Bounds in the world with the entity's origin, rotation and scale. */
  vm::bbox3d worldBounds;
};

/** The frames (animations) of an entity's model, at most `limit` of them. */
std::vector<ModelAnimation> modelAnimations(
  const EntityModelState& state, size_t limit = size_t(-1));

/** `{"index", "name", "bounds", "worldBounds"}` */
Json animationJson(const ModelAnimation& animation);

/**
 * Finds an animation of the model by name (ignoring case) or by index: a JSON string or a
 * non-negative integer. A string of digits that is no animation name is read as an index.
 */
std::optional<size_t> findAnimation(
  const mdl::EntityModelData& data, const Json& animation);

/** The names of the first animations of the model, for hints ("0" etc. without names). */
std::vector<std::string> animationNames(const mdl::EntityModelData& data, size_t limit);

/**
 * The model fields of entity_model_info for a point entity: modelLoaded, modelBounds,
 * modelLoadError (if the model cannot be loaded), frameProperty, currentAnimation {index,
 * name, bounds, worldBounds} (or null), animationCount, animations (at most
 * maxAnimations) and animationsTruncated.
 */
Json entityModelAnimationsJson(
  const mdl::Entity& entity, EntityModelLoader& loader, size_t maxAnimations);

/** A problem with the placement of a model found by checkModelPlacement. */
struct PlacementFinding
{
  /** MODEL_BELOW_FLOOR, MODEL_FLOATING, MODEL_PENETRATES_BRUSHES or MODEL_NO_FLOOR. */
  std::string code;
  std::string message;
  std::vector<std::string> objectIds;
  /** Depth below the floor or gap above it, if applicable. */
  std::optional<double> distance;
  /** A move that fixes the finding, if there is a simple one. */
  std::optional<vm::vec3d> suggestedMove;
};

/** The surface below a model. */
struct PlacementSurface
{
  double z;
  const mdl::Node* node;
  std::optional<size_t> faceIndex;
};

struct PlacementCheck
{
  vm::bbox3d modelBounds;
  std::optional<PlacementSurface> surface;
  std::vector<PlacementFinding> findings;
};

/** Models whose bottom is within this distance of the surface below stand on it. */
constexpr double PlacementTolerance = 1.0;

/**
 * Checks world model bounds against the visible brushes and patches of the map that are
 * not triggers, with vertical rays cast down from the top of the bounds at their center
 * and inset corners. A surface hit within PlacementTolerance of the bottom of the bounds
 * supports the model (also below a higher surface, e.g. the floor under a chair seat).
 * Without support, the surface below is the highest first hit (as for dropToFloor): if
 * the bottom is more than PlacementTolerance below it, the model reaches into the floor
 * (MODEL_BELOW_FLOOR), if it is more than PlacementTolerance above it, the model floats
 * (MODEL_FLOATING); without any surface, MODEL_NO_FLOOR. Brushes other than the surface
 * whose interior intersects the bounds shrunk by PlacementTolerance (so touching and
 * overlaps up to the tolerance are fine) are reported as MODEL_PENETRATES_BRUSHES. The
 * findings name the entity `entityId` first; `subject` starts each message, e.g. "The
 * model of entity:5 (animation 'sit')".
 */
PlacementCheck checkModelPlacement(
  const vm::bbox3d& modelBounds,
  mdl::Map& map,
  const IdRegistry& ids,
  const std::string& entityId,
  const std::string& subject);

/** "The model of <id> (animation '<name>')" or "... (frame <n>)". */
std::string placementSubject(const std::string& entityId, const EntityModelState& state);

/** `{"code", "message", "objectIds", "distance"?, "suggestedMove"?}` */
Json placementFindingJson(const PlacementFinding& finding);

/** `{"z", "object", "face"}` or null. */
Json placementSurfaceJson(
  const std::optional<PlacementSurface>& surface, const IdRegistry& ids);

/**
 * Checks the placement of the point entities among the given nodes and their
 * descendants whose models can be loaded (other entities are skipped) and adds a warning
 * for each finding. At most 20 warnings are added; the rest are summarized in a
 * MORE_PLACEMENT_FINDINGS warning that points to entity_placement_check.
 */
void warnModelPlacement(
  CallContext& context, const std::vector<mdl::Node*>& nodes, EntityModelLoader& loader);

/** warnModelPlacement with a loader of its own. */
void warnModelPlacement(CallContext& context, const std::vector<mdl::Node*>& nodes);

/** The bounds that dropToFloor rests on the floor, relative to the entity's origin. */
struct DropBounds
{
  vm::bbox3d bounds;
  /** Whether these are model bounds (else the definition's bounds). */
  bool model = false;
};

/**
 * The bounds that dropToFloor uses for a new point entity with the given definition (may
 * be null) and properties: with `dropUsing` "model", the model bounds of the animation
 * the properties select (INVALID_ARGUMENT if the class has no model or the model cannot
 * be loaded); with "definition", the definition's bounds (or the default bounds without
 * definition); with "auto", the model bounds if the model can be loaded, else the
 * definition's bounds. With `applyDefaults`, the definition's defaults of missing
 * properties are taken into account.
 */
Result<DropBounds, ToolError> dropToFloorBounds(
  const mdl::EntityDefinition* definition,
  const std::string& classname,
  const std::vector<std::pair<std::string, std::string>>& properties,
  bool applyDefaults,
  std::string_view dropUsing,
  EntityModelLoader& loader);

} // namespace tb::mcp
