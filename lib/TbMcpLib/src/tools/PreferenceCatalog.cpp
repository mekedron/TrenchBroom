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

#include "mcp/tools/PreferenceCatalog.h"

#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/GameManager.h"
#include "prefs/Preferences.h"

#include <fmt/format.h>

#include <unordered_set>

namespace tb::mcp
{
namespace
{

template <typename T>
PreferenceInfo entry(
  Preference<T>& preference, std::string category, std::string description)
{
  return PreferenceInfo{
    .preference = &preference,
    .category = std::move(category),
    .description = std::move(description),
  };
}

// The OpenGL texture filter constants (GL_NEAREST, GL_LINEAR, GL_*_MIPMAP_*)
constexpr auto GlNearest = 0x2600;
constexpr auto GlLinear = 0x2601;
constexpr auto GlNearestMipmapNearest = 0x2700;
constexpr auto GlLinearMipmapNearest = 0x2701;
constexpr auto GlNearestMipmapLinear = 0x2702;
constexpr auto GlLinearMipmapLinear = 0x2703;

std::vector<PreferenceInfo> createEditorPreferences()
{
  using namespace Preferences;

  const auto color = [](Preference<Color>& preference, std::string description) {
    return entry(preference, "colors", std::move(description));
  };
  const auto alpha = [](Preference<float>& preference, std::string description) {
    return entry(preference, "colors", std::move(description)).withRange(0.0, 1.0);
  };
  const auto view = [](Preference<bool>& preference, std::string description) {
    return entry(preference, "view", std::move(description));
  };
  const auto key =
    [](Preference<std::vector<KeySequence>>& preference, std::string description) {
      return entry(preference, "keyboard", std::move(description));
    };

  return {
    // Updates
    entry(
      AskForAutoUpdates,
      "updater",
      "Ask on start-up whether TrenchBroom should check for updates automatically."),
    entry(AutoCheckForUpdates, "updater", "Check for updates when TrenchBroom starts."),
    entry(IncludePreReleaseUpdates, "updater", "Include pre-releases in update checks."),
    entry(
      EnableDraftReleaseUpdates,
      "updater",
      "Offer the option to include draft releases (not saved)."),
    entry(
      IncludeDraftReleaseUpdates,
      "updater",
      "Include draft releases in update checks (not saved)."),

    // User interface
    entry(
      MapViewLayout,
      "view",
      "Layout of the editing views: 0 = one pane (3D), 1 = two panes, 2 = three panes, "
      "3 = four panes.")
      .withValues({0, 1, 2, 3}),
    entry(Theme, "view", "User interface theme.")
      .withValues({SystemTheme, DarkTheme})
      .withNote("Takes effect after TrenchBroom is restarted."),

    // Renderer
    entry(ShowAxes, "renderer", "Show the coordinate system axes in the 3D view."),
    color(SoftMapBoundsColor, "Color of the soft map bounds."),
    color(BackgroundColor, "Background color of the map views."),
    entry(AxisLength, "renderer", "Length of the coordinate axes in the 3D view."),
    color(XAxisColor, "Color of the X axis (fixed)."),
    color(YAxisColor, "Color of the Y axis (fixed)."),
    color(ZAxisColor, "Color of the Z axis (fixed)."),
    color(PointFileColor, "Color of the leak trace of a loaded point file."),
    color(PortalFileBorderColor, "Border color of the portals of a loaded portal file."),
    color(PortalFileFillColor, "Fill color of the portals of a loaded portal file."),
    entry(ShowFPS, "renderer", "Show the frame rate in the map views."),
    color(CompassBackgroundColor, "Background color of the compass (fixed)."),
    color(CompassBackgroundOutlineColor, "Outline color of the compass (fixed)."),
    color(CompassAxisOutlineColor, "Outline color of the compass axes (fixed)."),
    color(CameraFrustumColor, "Color of the 3D camera frustum shown in the 2D views."),
    color(DefaultGroupColor, "Color of groups."),
    color(LinkedGroupColor, "Color of linked groups."),
    color(TutorialOverlayTextColor, "Text color of the tutorial overlay."),
    color(TutorialOverlayBackgroundColor, "Background color of the tutorial overlay."),
    color(FaceColor, "Color of brush faces without a material."),
    color(SelectedFaceColor, "Tint of selected brush faces."),
    color(LockedFaceColor, "Tint of brush faces in locked layers or groups."),
    alpha(TransparentFaceAlpha, "Opacity of transparent faces."),
    color(EdgeColor, "Color of brush edges."),
    color(SelectedEdgeColor, "Color of the edges of selected objects."),
    alpha(OccludedSelectedEdgeAlpha, "Opacity of occluded edges of selected objects."),
    color(LockedEdgeColor, "Color of the edges of locked objects."),
    color(UndefinedEntityColor, "Color of entities without a definition."),
    color(SelectionBoundsColor, "Color of the bounds of the selection."),
    color(InfoOverlayTextColor, "Text color of info overlays (e.g. classnames)."),
    color(GroupInfoOverlayTextColor, "Text color of group info overlays."),
    color(InfoOverlayBackgroundColor, "Background color of info overlays."),
    alpha(
      WeakInfoOverlayBackgroundAlpha, "Background opacity of less important overlays."),
    color(
      SelectedInfoOverlayTextColor, "Text color of info overlays of selected objects."),
    color(
      SelectedInfoOverlayBackgroundColor,
      "Background color of info overlays of selected objects."),
    color(LockedInfoOverlayTextColor, "Text color of info overlays of locked objects."),
    color(
      LockedInfoOverlayBackgroundColor,
      "Background color of info overlays of locked objects."),

    // Handles and tools
    entry(HandleRadius, "controls", "Radius of vertex and tool handles in pixels."),
    entry(
      MaximumHandleDistance,
      "controls",
      "Maximum distance from the camera at which handles can be picked."),
    color(HandleColor, "Color of vertex and tool handles."),
    color(OccludedHandleColor, "Color of occluded handles."),
    color(SelectedHandleColor, "Color of selected handles."),
    color(OccludedSelectedHandleColor, "Color of occluded selected handles."),
    color(ClipHandleColor, "Color of the clip tool's points."),
    color(ClipFaceColor, "Color of the clip tool's clip plane."),
    color(ExtrudeHandleColor, "Color of the extrude (resize) handles."),
    entry(
      RotateHandleRadius, "controls", "Radius of the rotate tool's handle in pixels."),
    color(RotateHandleColor, "Color of the rotate tool's handle."),
    color(ScaleHandleColor, "Color of the scale tool's handles."),
    color(ScaleFillColor, "Fill color of the scale tool's box."),
    color(ScaleOutlineColor, "Outline color of the scale tool's box."),
    alpha(ScaleOutlineDimAlpha, "Opacity of the scale tool's dimmed outlines."),
    color(ShearFillColor, "Fill color of the shear tool's box."),
    color(ShearOutlineColor, "Outline color of the shear tool's box."),
    color(
      MoveTraceColor,
      "Color of the trace shown while moving objects. The occluded part of the trace "
      "uses the same stored color (its default is translucent)."),
    color(MoveIndicatorOutlineColor, "Outline color of the move indicator."),
    color(MoveIndicatorFillColor, "Fill color of the move indicator."),
    color(AngleIndicatorColor, "Color of the angle indicator of the rotate tool."),
    color(TextureSeamColor, "Color of material seams in the UV editor."),

    // Materials and textures
    entry(Brightness, "renderer", "Brightness of materials and models in the 3D view.")
      .withRange(0.0, 2.0),
    entry(GridAlpha, "renderer", "Opacity of the grid lines in the 3D view.")
      .withRange(0.0, 1.0),
    color(GridColor2D, "Color of the grid lines in the 2D views."),
    entry(
      TextureMinFilter,
      "renderer",
      fmt::format(
        "Texture minification filter (OpenGL constant): {} nearest, {} nearest "
        "mipmapped, {} nearest mipmapped interpolated, {} linear, {} linear mipmapped, "
        "{} linear mipmapped interpolated. Set together with the magnification filter.",
        GlNearest,
        GlNearestMipmapNearest,
        GlNearestMipmapLinear,
        GlLinear,
        GlLinearMipmapNearest,
        GlLinearMipmapLinear))
      .withValues(
        {GlNearest,
         GlLinear,
         GlNearestMipmapNearest,
         GlLinearMipmapNearest,
         GlNearestMipmapLinear,
         GlLinearMipmapLinear}),
    entry(
      TextureMagFilter,
      "renderer",
      fmt::format(
        "Texture magnification filter (OpenGL constant): {} nearest, {} linear.",
        GlNearest,
        GlLinear))
      .withValues({GlNearest, GlLinear}),
    entry(EnableMSAA, "renderer", "Enable multisampling (antialiasing)."),

    // Editing
    entry(
      AlignmentLock,
      "editor",
      "Texture lock: keep the material alignment when brushes are moved or rotated."),
    entry(UvLock, "editor", "UV lock: keep UV coordinates when vertices are moved."),
    entry(
      GroupBrushesCreatedByShapeTool,
      "editor",
      "Group the brushes created by the shape tool."),

    // Fonts
    entry(
      RendererFontPath,
      "renderer",
      "Font of the labels in the map views and browsers, relative to the resources."),
    entry(RendererFontSize, "renderer", "Font size of the labels in the map views.")
      .withRange(1, 96),

    // Browsers
    entry(BrowserFontSize, "browser", "Font size of the entity and material browsers.")
      .withRange(1, 96),
    color(BrowserTextColor, "Text color of the browsers."),
    color(BrowserSubTextColor, "Secondary text color of the browsers."),
    color(
      BrowserGroupBackgroundColor, "Background color of group titles in the browsers."),
    color(BrowserBackgroundColor, "Background color of the browsers."),
    entry(
      MaterialBrowserIconSize,
      "browser",
      "Icon size of the material browser (0.25 to 3; the preferences offer 0.25, 0.5, 1, "
      "1.5, 2, 2.5 and 3).")
      .withRange(0.25, 3.0),
    color(
      MaterialBrowserDefaultColor, "Frame color of materials in the material browser."),
    color(MaterialBrowserSelectedColor, "Frame color of the selected material."),
    color(MaterialBrowserUsedColor, "Frame color of materials used in the map."),

    // Camera
    entry(CameraLookSpeed, "camera", "Mouse look speed.").withRange(0.0, 1.0),
    entry(CameraLookInvertH, "camera", "Invert horizontal mouse look."),
    entry(CameraLookInvertV, "camera", "Invert vertical mouse look."),
    entry(CameraPanSpeed, "camera", "Mouse pan speed.").withRange(0.0, 1.0),
    entry(CameraPanInvertH, "camera", "Invert horizontal panning."),
    entry(CameraPanInvertV, "camera", "Invert vertical panning."),
    entry(CameraMouseWheelInvert, "camera", "Invert the mouse wheel."),
    entry(CameraMoveSpeed, "camera", "Speed of moving the camera with the mouse.")
      .withRange(0.0, 1.0),
    entry(
      CameraEnableAltMove, "camera", "Move the camera horizontally while Alt is held."),
    entry(
      CameraAltMoveInvert,
      "camera",
      "Invert the zoom direction when moving the camera with Alt."),
    entry(
      CameraMoveInCursorDir,
      "camera",
      "Move the camera towards the mouse cursor when the mouse wheel is turned."),
    entry(CameraFov, "camera", "Field of vision of the 3D view in degrees.")
      .withRange(50.0, 150.0),
    entry(CameraFlyMoveSpeed, "camera", "Speed of fly mode (keyboard camera movement).")
      .withRange(double(MinCameraFlyMoveSpeed), double(MaxCameraFlyMoveSpeed)),
    entry(Link2DCameras, "camera", "Pan and zoom all 2D views together."),

    // Fly mode keys
    key(CameraFlyForward, "Fly mode: move forward."),
    key(CameraFlyBackward, "Fly mode: move backward."),
    key(CameraFlyLeft, "Fly mode: move left."),
    key(CameraFlyRight, "Fly mode: move right."),
    key(CameraFlyUp, "Fly mode: move up."),
    key(CameraFlyDown, "Fly mode: move down."),

    // Map view
    view(ShowEntityClassnames, "Show entity classnames in the map views."),
    view(ShowGroupBounds, "Show the bounds of groups."),
    view(ShowBrushEntityBounds, "Show the bounds of brush entities."),
    view(ShowPointEntityBounds, "Show the bounds of point entities."),
    view(ShowPointEntityModels, "Show the models of point entities."),
    entry(
      FaceRenderMode,
      "view",
      "How brush faces are drawn in the 3D view: textured, flat (colors only) or skip "
      "(faces hidden).")
      .withValues({FaceRenderModeTextured, FaceRenderModeFlat, FaceRenderModeSkip}),
    view(ShadeFaces, "Shade brush faces by their orientation."),
    view(ShowFog, "Show fog in the 3D view."),
    view(ShowEdges, "Show brush edges."),
    view(ShowSoftMapBounds, "Show the soft map bounds."),
    view(ShowPointEntities, "Show point entities."),
    view(ShowBrushes, "Show brushes."),
    view(ShowPatches, "Show patches."),
    entry(
      EntityLinkMode,
      "view",
      "Which entity links are drawn: all, transitive (of the selection, transitively), "
      "direct (of the selection) or none.")
      .withValues(
        {EntityLinkModeAll,
         EntityLinkModeTransitive,
         EntityLinkModeDirect,
         EntityLinkModeNone}),
  };
}

std::vector<PreferenceInfo> hostPreferences(McpHost& host, ui::MapDocument* document)
{
  auto result = std::vector<PreferenceInfo>{};
  if (auto* preferenceHost = host.preferenceHost())
  {
    for (auto& hostPreference : preferenceHost->preferences(document))
    {
      result.push_back(PreferenceInfo{
        .preference = hostPreference.preference,
        .category = std::move(hostPreference.category),
        .description = std::move(hostPreference.description),
        .source = PreferenceSource::Host,
        .minimum = hostPreference.minimum,
        .maximum = hostPreference.maximum,
        .lockedReason = std::move(hostPreference.lockedReason),
        .secret = hostPreference.secret,
      });
    }
  }
  return result;
}

} // namespace

const std::filesystem::path& PreferenceInfo::path() const
{
  return preferencePath(preference);
}

PreferenceInfo PreferenceInfo::withValues(std::vector<Json> values) const
{
  auto result = *this;
  result.allowedValues = std::move(values);
  return result;
}

PreferenceInfo PreferenceInfo::withRange(const double min, const double max) const
{
  auto result = *this;
  result.minimum = min;
  result.maximum = max;
  return result;
}

PreferenceInfo PreferenceInfo::withNote(std::string text) const
{
  auto result = *this;
  result.note = std::move(text);
  return result;
}

const std::vector<PreferenceInfo>& editorPreferences()
{
  static const auto preferences = createEditorPreferences();
  return preferences;
}

std::vector<PreferenceInfo> gamePreferences(mdl::GameManager& gameManager)
{
  auto names = std::vector<std::string>{};
  for (const auto& gameInfo : gameManager.gameInfos())
  {
    names.push_back(gameInfo.gameConfig.name);
  }

  auto result = std::vector<PreferenceInfo>{};
  for (const auto& name : names)
  {
    auto* gameInfo = gameManager.gameInfo(name);
    result.push_back(PreferenceInfo{
      .preference = &gameInfo->gamePathPreference,
      .category = "games",
      .description = fmt::format(
        "Installation folder of {} (the folder that contains the game's base folder, "
        "e.g. 'id1' for Quake). Open documents of the game reload their assets.",
        name),
      .source = PreferenceSource::Game,
    });
    result.push_back(PreferenceInfo{
      .preference = &gameInfo->defaultEnginePathPreference,
      .category = "games",
      .description = fmt::format(
        "Path of the engine that the Launch Engine dialog selects by default for {}.",
        name),
      .source = PreferenceSource::Game,
    });
    for (auto& tool : gameInfo->gameConfig.compilationTools)
    {
      result.push_back(PreferenceInfo{
        .preference = &tool.pathPreference,
        .category = "games",
        .description = fmt::format(
          "Path of the compilation tool '{}' of {}{}; compile profiles refer to it as "
          "${{{}}}.",
          tool.name,
          name,
          tool.description ? fmt::format(" ({})", *tool.description) : "",
          tool.name),
        .source = PreferenceSource::Game,
      });
    }
  }
  return result;
}

std::vector<PreferenceInfo> allPreferences(McpHost& host, ui::MapDocument* document)
{
  auto result = std::vector<PreferenceInfo>{};
  auto paths = std::unordered_set<std::filesystem::path>{};

  const auto add = [&](PreferenceInfo info) {
    if (paths.insert(info.path()).second)
    {
      result.push_back(std::move(info));
    }
  };

  for (const auto& info : editorPreferences())
  {
    add(info);
  }
  for (auto& info : gamePreferences(host.gameManager()))
  {
    add(std::move(info));
  }
  for (auto& info : hostPreferences(host, document))
  {
    add(std::move(info));
  }
  return result;
}

std::string preferenceTypeName(const AnyPreference& preference)
{
  return std::visit(
    [](const auto* p) -> std::string {
      using T = std::decay_t<decltype(p->defaultValue)>;
      if constexpr (std::is_same_v<T, bool>)
      {
        return "bool";
      }
      else if constexpr (std::is_same_v<T, int>)
      {
        return "int";
      }
      else if constexpr (std::is_same_v<T, float>)
      {
        return "float";
      }
      else if constexpr (std::is_same_v<T, std::string>)
      {
        return "string";
      }
      else if constexpr (std::is_same_v<T, std::filesystem::path>)
      {
        return "path";
      }
      else if constexpr (std::is_same_v<T, Color>)
      {
        return "color";
      }
      else
      {
        return "shortcuts";
      }
    },
    preference);
}

const std::filesystem::path& preferencePath(const AnyPreference& preference)
{
  return std::visit(
    [](const auto* p) -> const std::filesystem::path& { return p->path; }, preference);
}

PreferencePersistencePolicy preferencePersistence(const AnyPreference& preference)
{
  return std::visit([](const auto* p) { return p->persistencePolicy; }, preference);
}

} // namespace tb::mcp
