#include "dve/editor_native_renderer.hpp"
#include "dve/editor_midi.hpp"
#include "dve/editor_ui_zoom.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace dve::editor {
EditorColor rgb(unsigned r, unsigned g, unsigned b) noexcept {
    return ((r & 255U) << 16U) | ((g & 255U) << 8U) | (b & 255U);
}

unsigned byte(float value) noexcept {
    return static_cast<unsigned>(std::clamp(value, 0.0F, 1.0F) * 255.0F + 0.5F);
}

<<<<<<< HEAD
std::string tool_name(EditorToolId tool) { return std::string(editor_tool_info(tool).name); }
=======
std::string tool_name(EditorToolId tool) {
    static constexpr std::array<std::string_view, 10> names{
        "Select", "Move", "Add", "Remove", "Paint", "Box", "Beam", "Anchor", "Rotate", "Scale"
    };
    return std::string(names[static_cast<std::size_t>(tool)]);
}
>>>>>>> origin/main

std::string mode_name(EditorMode mode) {
    switch (mode) {
        case EditorMode::Edit: return "EDIT";
        case EditorMode::Simulate: return "SIMULATE";
        case EditorMode::Play: return "PLAY";
    }
    return "EDIT";
}

std::array<Float3, 8> bounds_corners(const EditorObjectBounds& bounds) {
    return {{
        {bounds.minimum.x,bounds.minimum.y,bounds.minimum.z}, {bounds.maximum.x,bounds.minimum.y,bounds.minimum.z},
        {bounds.minimum.x,bounds.maximum.y,bounds.minimum.z}, {bounds.maximum.x,bounds.maximum.y,bounds.minimum.z},
        {bounds.minimum.x,bounds.minimum.y,bounds.maximum.z}, {bounds.maximum.x,bounds.minimum.y,bounds.maximum.z},
        {bounds.minimum.x,bounds.maximum.y,bounds.maximum.z}, {bounds.maximum.x,bounds.maximum.y,bounds.maximum.z},
    }};
}

void draw_world_box(const IEditorCanvas& painter, const NativeEditorController& controller,
                    const EditorObjectBounds& bounds, EditorColor color) {
    if (!bounds.valid) return;
    const auto corners = bounds_corners(bounds);
    std::array<ScreenPoint,8> points{};
    for (std::size_t i = 0; i < points.size(); ++i)
        points[i] = project_world_to_screen(controller.camera(), controller.layout().viewport, corners[i]);
    static constexpr std::array<std::array<int,2>,12> edges{{
        {{0,1}},{{0,2}},{{0,4}},{{1,3}},{{1,5}},{{2,3}},{{2,6}},{{3,7}},{{4,5}},{{4,6}},{{5,7}},{{6,7}}
    }};
    for (const auto edge : edges) {
        const auto& a = points[static_cast<std::size_t>(edge[0])];
        const auto& b = points[static_cast<std::size_t>(edge[1])];
        if (a.visible && b.visible) painter.line(static_cast<int>(a.x), static_cast<int>(a.y),
                                                 static_cast<int>(b.x), static_cast<int>(b.y), color, 2);
    }
}



void render_cinematic_camera_panel(const IEditorCanvas& painter,
                                    const NativeEditorController& controller,
                                    EditorColor panel, EditorColor panel2,
                                    EditorColor border, EditorColor text,
                                    EditorColor muted, EditorColor accent) {
    const EditorCinematicCameraPanel& cameraPanel = controller.cinematic_camera_panel();
    if (!cameraPanel.is_open()) return;
    const int windowWidth = controller.layout().menuBar.width;
    const int windowHeight = controller.layout().statusBar.y + controller.layout().statusBar.height;
    const CinematicCameraPanelLayout layout = cameraPanel.layout(windowWidth, windowHeight);
    painter.fill(layout.panel, panel);
    painter.outline(layout.panel, accent);
    painter.text(layout.panel.x + 18, layout.panel.y + 28, "CINEMATIC CAMERA", text);
    painter.text(layout.panel.x + 190, layout.panel.y + 28,
                 std::string(cameraPanel.status()), muted);
    painter.fill(layout.closeButton, panel2);
    painter.outline(layout.closeButton, border);
    painter.text(layout.closeButton.x + 18, layout.closeButton.y + 20, "CLOSE", text);

    static constexpr std::array<CinematicCameraScope, 4> scopes{
        CinematicCameraScope::CameraInstance, CinematicCameraScope::ShotOverride,
        CinematicCameraScope::ProjectDefault, CinematicCameraScope::ViewportPreview};
    for (std::size_t i = 0; i < scopes.size(); ++i) {
        const bool active = cameraPanel.scope() == scopes[i];
        painter.fill(layout.scopeTabs[i], active ? rgb(43, 87, 136) : panel2);
        painter.outline(layout.scopeTabs[i], active ? accent : border);
        const std::string label(EditorCinematicCameraPanel::scope_name(scopes[i]));
        painter.text(layout.scopeTabs[i].x + 10, layout.scopeTabs[i].y + 21, label,
                     active ? text : muted);
    }

    static constexpr std::array<CinematicCameraSection, 8> sections{
        CinematicCameraSection::Composition, CinematicCameraSection::Lens,
        CinematicCameraSection::FocusAndBokeh, CinematicCameraSection::SplitDiopter,
        CinematicCameraSection::ColorGrade, CinematicCameraSection::Film,
        CinematicCameraSection::Framing, CinematicCameraSection::Accessibility};
    for (std::size_t i = 0; i < sections.size(); ++i) {
        const bool active = cameraPanel.section() == sections[i];
        painter.fill(layout.sectionRows[i], active ? rgb(40, 74, 112) : panel2);
        painter.outline(layout.sectionRows[i], active ? accent : border);
        painter.text(layout.sectionRows[i].x + 8, layout.sectionRows[i].y + 21,
                     std::string(EditorCinematicCameraPanel::section_name(sections[i])),
                     active ? text : muted);
    }

    static constexpr std::array<camera::CameraCinematicPreset, 10> presets{
        camera::CameraCinematicPreset::Neutral, camera::CameraCinematicPreset::AcademyClassic,
        camera::CameraCinematicPreset::Imax143, camera::CameraCinematicPreset::Imax190,
        camera::CameraCinematicPreset::Scope239, camera::CameraCinematicPreset::VintageAnamorphic,
        camera::CameraCinematicPreset::FisheyeAction, camera::CameraCinematicPreset::SplitDiopter,
        camera::CameraCinematicPreset::BleachBypass, camera::CameraCinematicPreset::SeventiesFilm};
    painter.text(layout.presetRows.front().x, layout.presetRows.front().y - 9,
                 "CAMERA / FILM PRESETS", accent);
    for (std::size_t i = 0; i < presets.size(); ++i) {
        painter.fill(layout.presetRows[i], panel2);
        painter.outline(layout.presetRows[i], border);
        painter.text(layout.presetRows[i].x + 9, layout.presetRows[i].y + 21,
                     std::string(EditorCinematicCameraPanel::preset_name(presets[i])), text);
    }

    static constexpr std::array<camera::CameraFilmbackPreset, 7> filmbacks{
        camera::CameraFilmbackPreset::Super16, camera::CameraFilmbackPreset::Super35,
        camera::CameraFilmbackPreset::FullFrame35, camera::CameraFilmbackPreset::Anamorphic35,
        camera::CameraFilmbackPreset::Imax15Perf, camera::CameraFilmbackPreset::ImaxDigital,
        camera::CameraFilmbackPreset::Custom};
    painter.text(layout.filmbackRows.front().x, layout.filmbackRows.front().y - 9,
                 "FILMBACK / GATE", accent);
    for (std::size_t i = 0; i < filmbacks.size(); ++i) {
        painter.fill(layout.filmbackRows[i], panel2);
        painter.outline(layout.filmbackRows[i], border);
        painter.text(layout.filmbackRows[i].x + 9, layout.filmbackRows[i].y + 21,
                     std::string(EditorCinematicCameraPanel::filmback_name(filmbacks[i])), text);
    }

    const camera::CameraPostProcessProfile& profile =
        cameraPanel.profile_for_scope(controller.selected_camera_rig());
    const auto& cinematic = profile.cinematic;
    const int infoX = layout.presetRows.front().x;
    const int infoY = layout.filmbackRows.back().y + 48;
    painter.text(infoX, infoY, "ACTIVE PROFILE", accent);
    painter.text(infoX, infoY + 22,
                 "Exposure " + std::to_string(profile.exposure).substr(0, 5) +
                 "   Saturation " + std::to_string(profile.saturation).substr(0, 5) +
                 "   Contrast " + std::to_string(profile.contrast).substr(0, 5), text);
    painter.text(infoX, infoY + 42,
                 "Focus " + std::to_string(profile.focusDistanceMeters).substr(0, 6) + " m" +
                 "   f/" + std::to_string(profile.apertureFStop).substr(0, 4) +
                 "   Bokeh blades " + std::to_string(cinematic.bokeh.bladeCount), muted);
    painter.text(infoX, infoY + 62,
                 std::string(cinematic.splitDiopter.enabled ? "Split diopter ON" : "Split diopter off") +
                 "   Fisheye " + std::to_string(cinematic.lens.fisheyeStrength).substr(0, 5) +
                 "   Anamorphic " + std::to_string(cinematic.lens.anamorphicSqueeze).substr(0, 5), muted);
    painter.text(infoX, infoY + 82,
                 "Grain " + std::to_string(cinematic.film.grainIntensity).substr(0, 5) +
                 "   Halation " + std::to_string(cinematic.film.halationIntensity).substr(0, 5) +
                 "   Motion blur " + std::to_string(cinematic.film.motionBlurWeight).substr(0, 5), muted);

    const std::array<std::pair<UiRect, std::string_view>, 5> buttons{{
        {layout.copyButton, "COPY"}, {layout.pasteButton, "PASTE"},
        {layout.resetButton, "RESET"}, {layout.keyframeButton, "ADD KEYFRAME"},
        {layout.sequencerButton, "SEQUENCER"}}};
    for (const auto& [rect, label] : buttons) {
        painter.fill(rect, panel2);
        painter.outline(rect, border);
        painter.text(rect.x + 9, rect.y + 21, std::string(label), text);
    }
    if (cameraPanel.sequencer_open()) {
        const auto& sequence = cameraPanel.sequencer().sequence();
        painter.text(layout.panel.x + layout.panel.width - 275,
                     layout.panel.y + layout.panel.height - 66,
                     "Sequence: " + (sequence.name.empty() ? std::string("Untitled") : sequence.name) +
                     "  Keys " + std::to_string(sequence.dollies.empty() ? 0U : sequence.dollies.front().keys.size()),
                     accent);
    }
}

void render_render3d_diagnostics_panel(const IEditorCanvas& painter,
                                       const NativeEditorController& controller,
                                       EditorColor panel, EditorColor panel2,
                                       EditorColor border, EditorColor text,
                                       EditorColor muted, EditorColor accent) {
    if (!controller.render3d_diagnostics_open()) return;
    const Render3DDiagnosticsReport& report = controller.render3d_diagnostics_report();
    const UiRect viewport = controller.layout().viewport;
    const UiRect box{viewport.x + 18, viewport.y + 18,
                     std::max(420, viewport.width - 36),
                     std::max(320, viewport.height - 36)};
    painter.fill(box, panel);
    painter.outline(box, accent);
    painter.text(box.x + 14, box.y + 25, "3D RENDERING DIAGNOSTICS", text);
    painter.text(box.x + box.width - 245, box.y + 25,
                 report.cameraName + " / " + report.renderPassName, muted);

    const int metricY = box.y + 42;
    const int metricHeight = 52;
    const int metricGap = 8;
    const int metricWidth = std::max(100, (box.width - 28 - metricGap * 3) / 4);
    const auto metric = [&](int column, std::string label, std::string value) {
        const UiRect rect{box.x + 14 + column * (metricWidth + metricGap), metricY,
                          metricWidth, metricHeight};
        painter.fill(rect, panel2);
        painter.outline(rect, border);
        painter.text(rect.x + 8, rect.y + 18, std::move(label), muted);
        painter.text(rect.x + 8, rect.y + 41, std::move(value), text);
    };
    metric(0, "Draw calls", std::to_string(report.drawCallCount));
    metric(1, "Visible objects", std::to_string(report.visibleObjectCount));
    metric(2, "Pipeline breaks", std::to_string(report.pipelineBreaks.size()));
    metric(3, "Budget warnings", std::to_string(report.budgetViolations.size()));

    const int leftX = box.x + 14;
    const int rightX = box.x + box.width / 2 + 8;
    int leftY = metricY + metricHeight + 22;
    int rightY = leftY;
    painter.text(leftX, leftY, "GEOMETRY / PIPELINES", accent); leftY += 22;
    painter.text(leftX, leftY, "Triangles " + std::to_string(report.triangleCount) +
                 "  Vertices " + std::to_string(report.vertexCount), text); leftY += 19;
    painter.text(leftX, leftY, "Meshes " + std::to_string(report.meshCount) +
                 "  Submeshes " + std::to_string(report.submeshCount) +
                 "  Instances " + std::to_string(report.instanceCount), muted); leftY += 19;
    painter.text(leftX, leftY, "Materials " + std::to_string(report.materialCount) +
                 "  Shaders " + std::to_string(report.shaderCount) +
                 "  Pipelines " + std::to_string(report.pipelineCount), muted); leftY += 28;

    painter.text(leftX, leftY, "RESIDENCY / REFERENCES", accent); leftY += 22;
    painter.text(leftX, leftY, "Textures " + std::to_string(report.textureCount) +
                 "  Resident bytes " + std::to_string(report.residency.residentTextureBytes), text); leftY += 19;
    painter.text(leftX, leftY, "Missing mesh/material/shader/pipeline/texture " +
                 std::to_string(report.residency.missingMeshes) + "/" +
                 std::to_string(report.residency.missingMaterials) + "/" +
                 std::to_string(report.residency.missingShaders) + "/" +
                 std::to_string(report.residency.missingPipelines) + "/" +
                 std::to_string(report.residency.missingTextures), muted); leftY += 19;
    painter.text(leftX, leftY, "Reference issues " + std::to_string(report.referenceIssues.size()), muted); leftY += 28;

    painter.text(leftX, leftY, "LOD / SKINNING", accent); leftY += 22;
    painter.text(leftX, leftY, "LOD decisions " + std::to_string(report.lodDecisions.size()), text); leftY += 19;
    painter.text(leftX, leftY, "Skinned vertices " + std::to_string(report.skinning.totalSkinnedVertices) +
                 "  GPU " + std::to_string(report.skinning.gpuSkinnedVertices) +
                 "  CPU " + std::to_string(report.skinning.cpuSkinnedVertices), text); leftY += 19;
    painter.text(leftX, leftY, "GPU dispatches " + std::to_string(report.skinning.gpuDispatches) +
                 "  Max bones " + std::to_string(report.skinning.maximumBonesPerObject), muted);

    painter.text(rightX, rightY, "SHADOWS / LIGHTING / OCCLUSION", accent); rightY += 22;
    painter.text(rightX, rightY, "Shadow casters " + std::to_string(report.shadows.casterCount) +
                 "  Shadow draws " + std::to_string(report.shadows.drawCount), text); rightY += 19;
    painter.text(rightX, rightY, "Visible lights " + std::to_string(report.lighting.visibleLightCount) +
                 "  Cluster refs " + std::to_string(report.lighting.clusterReferenceCount), text); rightY += 19;
    painter.text(rightX, rightY, "Occlusion culled " + std::to_string(report.occlusion.culledObjectCount) +
                 " of " + std::to_string(report.occlusion.testedObjectCount), muted); rightY += 28;

    painter.text(rightX, rightY, "VOXEL MATERIAL POLICY", accent); rightY += 22;
    painter.text(rightX, rightY, "Voxel material modes B/S/P2/P4 " +
                 std::to_string(report.voxelMaterials.bakedBrickCount) + "/" +
                 std::to_string(report.voxelMaterials.singleMaterialBrickCount) + "/" +
                 std::to_string(report.voxelMaterials.deferredPalette2BrickCount) + "/" +
                 std::to_string(report.voxelMaterials.deferredPalette4BrickCount), text); rightY += 19;
    painter.text(rightX, rightY, "Fallbacks " + std::to_string(report.voxelMaterials.fallbackBrickCount) +
                 "  recooks " + std::to_string(report.voxelMaterials.recookRequestCount) +
                 "  extra samples " + std::to_string(report.voxelMaterials.additionalTextureSamples), muted);
    rightY += 28;

    painter.text(rightX, rightY, "DEPTH COMPLEXITY (CPU REFERENCE)", accent); rightY += 20;
    painter.text(rightX, rightY, "Depth complexity max " +
                 std::to_string(report.depthComplexity.maximumDepthComplexity) +
                 "  fragments " + std::to_string(report.depthComplexity.fragmentCount), text); rightY += 18;
    const int heatW = std::max(160, box.width / 2 - 40);
    const int heatH = std::max(90, std::min(170, box.y + box.height - rightY - 28));
    const UiRect heat{rightX, rightY + 4, heatW, heatH};
    painter.fill(heat, rgb(6, 8, 12));
    const std::uint32_t sourceW = report.depthComplexity.width;
    const std::uint32_t sourceH = report.depthComplexity.height;
    for (int by = 0; by < heat.height; by += 5) {
        for (int bx = 0; bx < heat.width; bx += 5) {
            if (sourceW == 0U || sourceH == 0U || report.depthComplexity.heatmapRgba8.empty()) continue;
            const std::uint32_t sx = std::min(sourceW - 1U,
                static_cast<std::uint32_t>(bx) * sourceW / static_cast<std::uint32_t>(heat.width));
            const std::uint32_t sy = std::min(sourceH - 1U,
                static_cast<std::uint32_t>(by) * sourceH / static_cast<std::uint32_t>(heat.height));
            const std::size_t index = (static_cast<std::size_t>(sy) * sourceW + sx) * 4U;
            const unsigned red = std::to_integer<unsigned char>(report.depthComplexity.heatmapRgba8[index]);
            const unsigned green = std::to_integer<unsigned char>(report.depthComplexity.heatmapRgba8[index + 1U]);
            const unsigned blue = std::to_integer<unsigned char>(report.depthComplexity.heatmapRgba8[index + 2U]);
            painter.fill({heat.x + bx, heat.y + by, std::min(5, heat.width - bx),
                          std::min(5, heat.height - by)}, rgb(red, green, blue));
        }
    }
    painter.outline(heat, border);
    painter.text(box.x + 14, box.y + box.height - 10,
                 "Renderer-neutral capture; physical GPU counters remain a platform validation task.", muted);
}


EditorColor graph_color(const std::array<float, 4>& color) noexcept {
    return rgb(byte(color[0]), byte(color[1]), byte(color[2]));
}

UiRect graph_rect(const SpriteAnimationGraphRect& rect) noexcept {
    return {static_cast<int>(std::lround(rect.x)), static_cast<int>(std::lround(rect.y)),
            std::max(1, static_cast<int>(std::lround(rect.width))),
            std::max(1, static_cast<int>(std::lround(rect.height)))};
}

std::string graph_interruption_name(SpriteAnimationInterruptionSource value) {
    switch (value) {
    case SpriteAnimationInterruptionSource::NoInterruption: return "None";
    case SpriteAnimationInterruptionSource::CurrentState: return "Current";
    case SpriteAnimationInterruptionSource::PreviousState: return "Previous";
    case SpriteAnimationInterruptionSource::CurrentThenPrevious: return "Current -> Previous";
    case SpriteAnimationInterruptionSource::PreviousThenCurrent: return "Previous -> Current";
    }
    return "Unknown";
}

std::string graph_curve_name(SpriteAnimationBlendCurve value) {
    switch (value) {
    case SpriteAnimationBlendCurve::Linear: return "Linear";
    case SpriteAnimationBlendCurve::SmoothStep: return "SmoothStep";
    case SpriteAnimationBlendCurve::EaseIn: return "EaseIn";
    case SpriteAnimationBlendCurve::EaseOut: return "EaseOut";
    }
    return "Unknown";
}

std::string graph_track_policy_name(SpriteAnimationBlendTrackPolicy value) {
    switch (value) {
    case SpriteAnimationBlendTrackPolicy::DestinationOnly: return "Destination";
    case SpriteAnimationBlendTrackPolicy::SourceOnly: return "Source";
    case SpriteAnimationBlendTrackPolicy::HighestWeight: return "Highest weight";
    case SpriteAnimationBlendTrackPolicy::BothWeighted: return "Both weighted";
    }
    return "Unknown";
}

std::string graph_event_policy_name(SpriteAnimationBlendEventPolicy value) {
    switch (value) {
    case SpriteAnimationBlendEventPolicy::DestinationOnly: return "Destination";
    case SpriteAnimationBlendEventPolicy::SourceOnly: return "Source";
    case SpriteAnimationBlendEventPolicy::Both: return "Both";
    }
    return "Unknown";
}

std::string graph_root_policy_name(SpriteAnimationBlendRootMotionPolicy value) {
    switch (value) {
    case SpriteAnimationBlendRootMotionPolicy::DestinationOnly: return "Destination";
    case SpriteAnimationBlendRootMotionPolicy::SourceOnly: return "Source";
    case SpriteAnimationBlendRootMotionPolicy::Weighted: return "Weighted";
    }
    return "Unknown";
}

UiRect tile_rect(const TileCanvasRect& rect) noexcept {
    return {static_cast<int>(std::lround(rect.x)), static_cast<int>(std::lround(rect.y)),
            std::max(1, static_cast<int>(std::lround(rect.width))),
            std::max(1, static_cast<int>(std::lround(rect.height)))};
}

std::string tile_tool_name(TileMapTool tool) {
    switch (tool) {
    case TileMapTool::Pencil: return "Pencil";
    case TileMapTool::Eraser: return "Eraser";
    case TileMapTool::Rectangle: return "Rectangle";
    case TileMapTool::FloodFill: return "Flood";
    case TileMapTool::Terrain: return "Terrain";
    case TileMapTool::Object: return "Object";
    case TileMapTool::CollisionShape: return "Collision";
    case TileMapTool::Pan: return "Pan";
    }
    return "Tool";
}

EditorColor tile_layer_color(TileLayerKind kind, bool active) noexcept {
    const unsigned boost = active ? 38U : 0U;
    switch (kind) {
    case TileLayerKind::Visual: return rgb(62U + boost, 100U + boost, 132U + boost);
    case TileLayerKind::Collision: return rgb(80U + boost, 126U + boost, 88U + boost);
    case TileLayerKind::Hazard: return rgb(150U + boost, 72U, 62U);
    case TileLayerKind::Trigger: return rgb(118U + boost, 78U, 146U + boost);
    }
    return rgb(90U, 90U, 90U);
}


void render_sprite_pixel_art_panel(const IEditorCanvas& painter,
                                   NativeEditorController& controller,
                                   EditorColor panel, EditorColor panel2,
                                   EditorColor border, EditorColor text,
                                   EditorColor muted, EditorColor accent) {
    if (!controller.sprite_pixel_art_open()) return;
    const UiRect canvas = controller.sprite_pixel_art_canvas();
    const auto& session = controller.sprite_pixel_art();
    const auto& document = session.document();
    const UiRect outer{32, 42, canvas.x + canvas.width + 300 - 32, canvas.y + canvas.height + 54 - 42};
    painter.fill(outer, panel);
    painter.outline(outer, accent);
    painter.text(52, 70, "Pixel Art Studio", text);
    painter.text(52, 92, "1 Pencil  2 Eraser  3 Fill  N Frame  L Layer  W Wrap  Ctrl+S Save  Ctrl+P Publish", muted);
    painter.fill(canvas, panel2);
    painter.outline(canvas, border);
    std::vector<std::byte> rgba;
    std::string error;
    if (session.onion_rgba8(session.selected_frame(), true, true, rgba, &error)) {
        const int scale = std::max(1, std::min(canvas.width / static_cast<int>(document.width),
                                               canvas.height / static_cast<int>(document.height)));
        const bool fits = static_cast<int>(document.width) * scale <= canvas.width &&
                          static_cast<int>(document.height) * scale <= canvas.height;
        if (fits) {
            const int imageWidth = static_cast<int>(document.width) * scale;
            const int imageHeight = static_cast<int>(document.height) * scale;
            const int originX = canvas.x + (canvas.width - imageWidth) / 2;
            const int originY = canvas.y + (canvas.height - imageHeight) / 2;
            for (std::uint32_t y = 0; y < document.height; ++y) {
                for (std::uint32_t x = 0; x < document.width; ++x) {
                    const std::size_t offset = (static_cast<std::size_t>(y) * document.width + x) * 4U;
                    const unsigned a = static_cast<unsigned>(rgba[offset + 3U]);
                    const EditorColor checker = ((x + y) & 1U) ? rgb(46,48,56) : rgb(56,58,66);
                    const int px = originX + static_cast<int>(x) * scale;
                    const int py = originY + static_cast<int>(y) * scale;
                    painter.fill({px, py, scale, scale}, checker);
                    if (a != 0U) painter.fill({px, py, scale, scale}, rgb(
                        static_cast<unsigned>(rgba[offset + 0U]),
                        static_cast<unsigned>(rgba[offset + 1U]),
                        static_cast<unsigned>(rgba[offset + 2U])));
                    if (scale >= 6) painter.outline({px, py, scale, scale}, rgb(34,36,42));
                }
            }
        } else {
            const std::uint32_t stepX = std::max(1U, (document.width + static_cast<std::uint32_t>(canvas.width) - 1U) /
                                                       static_cast<std::uint32_t>(canvas.width));
            const std::uint32_t stepY = std::max(1U, (document.height + static_cast<std::uint32_t>(canvas.height) - 1U) /
                                                       static_cast<std::uint32_t>(canvas.height));
            for (std::uint32_t y = 0; y < document.height; y += stepY)
                for (std::uint32_t x = 0; x < document.width; x += stepX) {
                    const std::size_t offset = (static_cast<std::size_t>(y) * document.width + x) * 4U;
                    if (static_cast<unsigned>(rgba[offset + 3U]) == 0U) continue;
                    painter.fill({canvas.x + static_cast<int>(x / stepX), canvas.y + static_cast<int>(y / stepY), 1, 1},
                                 rgb(static_cast<unsigned>(rgba[offset + 0U]),
                                     static_cast<unsigned>(rgba[offset + 1U]),
                                     static_cast<unsigned>(rgba[offset + 2U])));
                }
        }
    }
    const int sideX = canvas.x + canvas.width + 18;
    painter.text(sideX, 124, "Document", text);
    painter.text(sideX, 146, document.name, muted);
    painter.text(sideX, 168, std::to_string(document.width) + " x " + std::to_string(document.height), muted);
    painter.text(sideX, 190, "Frames: " + std::to_string(document.frames.size()), muted);
    painter.text(sideX, 212, "Frame: " + std::to_string(session.selected_frame()), muted);
    painter.text(sideX, 234, "Layers: " + std::to_string(document.frames[session.selected_frame()].layers.size()), muted);
    painter.text(sideX, 256, session.wrap_painting() ? "Wrap: On" : "Wrap: Off", session.wrap_painting() ? accent : muted);
    painter.text(sideX, 278, "Hash: " + std::to_string(document.contentHash), muted);
}

void render_sprite_rig2d_panel(const IEditorCanvas& painter,
                               NativeEditorController& controller,
                               EditorColor panel, EditorColor panel2,
                               EditorColor border, EditorColor text,
                               EditorColor muted, EditorColor accent) {
    if (!controller.sprite_rig2d_open()) return;
    const UiRect canvas = controller.sprite_rig2d_canvas();
    const auto& asset = controller.sprite_rig2d().asset();
    const UiRect outer{32, 42, canvas.x + canvas.width + 300 - 32, canvas.y + canvas.height + 54 - 42};
    painter.fill(outer, panel);
    painter.outline(outer, accent);
    painter.text(52, 70, "Multi-Part Sprite Rig", text);
    painter.text(52, 92, "Space Preview  B Bone  P Part  I Two-Bone IK  V Variant  Ctrl+S Save", muted);
    painter.fill(canvas, panel2);
    painter.outline(canvas, border);
    SpriteRigPose2D pose;
    std::string error;
    const std::string clip = asset.clips.empty() ? std::string{} : asset.clips.front().name;
    if (sample_sprite_rig2d(asset, clip, controller.sprite_rig2d_preview_time(), {}, pose, &error)) {
        const float zoom = 4.0F;
        const int centerX = canvas.x + canvas.width / 2;
        const int centerY = canvas.y + canvas.height / 2;
        auto screen = [&](SpriteVec2 value) {
            return std::pair<int,int>{centerX + static_cast<int>(std::lround(value.x * zoom)),
                                      centerY - static_cast<int>(std::lround(value.y * zoom))};
        };
        for (const auto& bone : asset.bones) {
            const auto poseIt = std::find_if(pose.bones.begin(), pose.bones.end(), [&](const auto& candidate) { return candidate.id == bone.id; });
            if (poseIt == pose.bones.end()) continue;
            const auto [x0,y0] = screen(poseIt->world.translation);
            const float angle = poseIt->world.rotationDegrees * 3.14159265358979323846F / 180.0F;
            const SpriteVec2 endpoint{poseIt->world.translation.x + std::cos(angle) * bone.lengthPixels,
                                      poseIt->world.translation.y + std::sin(angle) * bone.lengthPixels};
            const auto [x1,y1] = screen(endpoint);
            painter.line(x0, y0, x1, y1, rgb(100,190,255), 3);
            painter.fill({x0 - 4, y0 - 4, 8, 8}, accent);
            painter.text(x0 + 6, y0 - 6, bone.name, muted);
        }
        for (const auto& part : pose.parts) {
            if (!part.visible) continue;
            const auto [x,y] = screen(part.world.translation);
            painter.fill({x - 10, y - 10, 20, 20}, rgb(byte(part.tint[0]), byte(part.tint[1]), byte(part.tint[2])));
            painter.outline({x - 10, y - 10, 20, 20}, text);
            painter.text(x + 12, y + 4, part.name, text);
        }
        for (const auto& constraint : asset.constraints) {
            const auto [x,y] = screen(constraint.targetPixels);
            painter.line(x - 6, y, x + 6, y, rgb(255,190,80), 2);
            painter.line(x, y - 6, x, y + 6, rgb(255,190,80), 2);
        }
    } else painter.text(canvas.x + 20, canvas.y + 30, error, rgb(255,100,100));
    const int sideX = canvas.x + canvas.width + 18;
    painter.text(sideX, 124, asset.name, text);
    painter.text(sideX, 148, "Bones: " + std::to_string(asset.bones.size()), muted);
    painter.text(sideX, 170, "Parts: " + std::to_string(asset.parts.size()), muted);
    painter.text(sideX, 192, "Clips: " + std::to_string(asset.clips.size()), muted);
    painter.text(sideX, 214, "IK: " + std::to_string(asset.constraints.size()), muted);
    painter.text(sideX, 236, "Variants: " + std::to_string(asset.variants.size()), muted);
    painter.text(sideX, 258, controller.sprite_rig2d_preview_playing() ? "Preview: Playing" : "Preview: Paused",
                 controller.sprite_rig2d_preview_playing() ? accent : muted);
    painter.text(sideX, 280, "Hash: " + std::to_string(asset.contentHash), muted);
}

void render_tile_world_editor_panel(const IEditorCanvas& painter,
                                    NativeEditorController& controller,
                                    EditorColor panel, EditorColor panel2,
                                    EditorColor border, EditorColor text,
                                    EditorColor muted, EditorColor accent) {
    if (!controller.tile_world_editor_open()) return;
    const int width = controller.window_width();
    const int height = controller.window_height();
    painter.fill({0, 0, width, height}, rgb(12, 16, 22));
    painter.fill({0, 0, width, 42}, panel);
    painter.text(18, 28, "TILE WORLD EDITOR", text);
    painter.text(194, 28, "Transactional map, semantic inspectors, regional recook and play validation", muted);

    const UiRect playButton{24, 52, 92, 28};
    const UiRect saveButton{124, 52, 80, 28};
    painter.fill(playButton, rgb(50, 132, 86)); painter.outline(playButton, border);
    painter.text(playButton.x + 13, playButton.y + 19, "Play Test", text);
    painter.fill(saveButton, panel2); painter.outline(saveButton, border);
    painter.text(saveButton.x + 21, saveButton.y + 19, "Save", text);

    const TileCanvasRect canvasViewport = controller.tile_world_canvas_viewport();
    const TileCanvasRect paletteViewport = controller.tile_world_palette_viewport();
    const TileWorldDesktopFrame frame = controller.tile_world_editor().frame(
        canvasViewport, paletteViewport, controller.tile_world_editor().canvas().session().map().content_hash());

    for (int tool = 0; tool < 7; ++tool) {
        const TileMapTool value = static_cast<TileMapTool>(tool);
        const UiRect button{static_cast<int>(canvasViewport.x) + tool * 58, 52, 54, 28};
        const bool selected = controller.tile_world_editor().canvas().tool() == value;
        painter.fill(button, selected ? accent : panel2); painter.outline(button, border);
        painter.text(button.x + 4, button.y + 18,
                     std::to_string(tool + 1) + " " + tile_tool_name(value).substr(0, 5), text);
    }

    painter.fill(tile_rect(paletteViewport), rgb(19, 24, 31));
    painter.outline(tile_rect(paletteViewport), border);
    painter.text(static_cast<int>(paletteViewport.x) + 7, static_cast<int>(paletteViewport.y) - 8,
                 "TILE PALETTE", muted);
    for (const TileSetPaletteCell& cell : frame.palette.cells) {
        const UiRect rect = tile_rect(cell.rect);
        const unsigned shade = 55U + static_cast<unsigned>((cell.atlasIndex * 23U) % 105U);
        painter.fill(rect, rgb(shade, std::min(220U, shade + 28U), std::max(45U, shade - 10U)));
        painter.outline(rect, cell.selected ? accent : border);
        if (rect.width >= 22) painter.text(rect.x + 4, rect.y + 14, std::to_string(cell.tileIndex + 1U), text);
    }

    painter.fill(tile_rect(canvasViewport), rgb(18, 25, 31));
    painter.outline(tile_rect(canvasViewport), border);
    for (const TileCanvasCellFrame& cell : frame.canvas.cells) {
        const UiRect rect = tile_rect(cell.rect);
        painter.fill(rect, tile_layer_color(cell.kind, cell.activeLayer));
        if (cell.collision != TileCollision::Empty) painter.outline(rect, rgb(245, 210, 75));
        else if (rect.width >= 8 && rect.height >= 8) painter.outline(rect, rgb(39, 52, 62));
    }
    for (const TileCanvasObjectFrame& object : frame.canvas.objects) {
        const UiRect rect = tile_rect(object.rect);
        painter.outline(rect, object.selected ? rgb(255, 206, 72) : rgb(220, 105, 225));
        if (object.selected) {
            for (const TileCanvasRect& handle : object.resizeHandles)
                painter.fill(tile_rect(handle), rgb(255, 206, 72));
        }
    }
    if (frame.canvas.brushPreview) {
        const TileRegion region = *frame.canvas.brushPreview;
        const float tileWidth = static_cast<float>(controller.tile_world_editor().canvas().session().map().tileWidth) * frame.canvas.zoom;
        const float tileHeight = static_cast<float>(controller.tile_world_editor().canvas().session().map().tileHeight) * frame.canvas.zoom;
        const UiRect preview{
            static_cast<int>(std::lround(canvasViewport.x + frame.canvas.pan.x + static_cast<float>(region.minCol) * tileWidth)),
            static_cast<int>(std::lround(canvasViewport.y + frame.canvas.pan.y + static_cast<float>(region.minRow) * tileHeight)),
            std::max(1, static_cast<int>(std::lround(static_cast<float>(region.maxCol - region.minCol + 1U) * tileWidth))),
            std::max(1, static_cast<int>(std::lround(static_cast<float>(region.maxRow - region.minRow + 1U) * tileHeight)))};
        painter.outline(preview, accent);
    }

    const int sideX = static_cast<int>(canvasViewport.x + canvasViewport.width) + 16;
    const int sideW = std::max(330, width - sideX - 18);
    painter.fill({sideX, 52, sideW, height - 70}, panel);
    painter.outline({sideX, 52, sideW, height - 70}, border);
    painter.text(sideX + 10, 72, "LAYERS", text);
    int rowY = 80;
    for (const TileWorldLayerRow& layer : frame.layers) {
        rowY += 22;
        if (rowY > 250) break;
        const UiRect row{sideX + 8, rowY - 16, sideW - 16, 20};
        painter.fill(row, layer.selected ? rgb(48, 74, 104) : panel2);
        painter.text(row.x + 5, row.y + 14,
                     std::string(layer.visible ? "V " : "- ") + (layer.locked ? "L " : "  ") +
                     layer.name, layer.selected ? text : muted);
        painter.text(row.x + row.width - 92, row.y + 14,
                     std::to_string(layer.parallaxX) + "," + std::to_string(layer.parallaxY), muted);
    }

    int infoY = 278;
    painter.text(sideX + 10, infoY, frame.inspector.title.empty() ? "INSPECTOR" : frame.inspector.title, text);
    for (const TileWorldInspectorField& field : frame.inspector.fields) {
        infoY += 20;
        if (infoY > height - 225) break;
        painter.text(sideX + 12, infoY, field.name, muted);
        painter.text(sideX + sideW / 2, infoY, field.value, field.editable ? text : muted);
    }

    int overlayY = height - 210;
    painter.text(sideX + 10, overlayY, "AUTOTILE / PARALLAX / CHUNKS", text);
    overlayY += 20;
    if (controller.tile_world_editor().show_autotile_rules()) {
        painter.text(sideX + 12, overlayY, "Autotile rules: " + std::to_string(frame.autotileRules.size()) + "  [A]", muted);
        overlayY += 18;
        for (std::size_t index = 0U; index < std::min<std::size_t>(3U, frame.autotileRules.size()); ++index) {
            const auto& rule = frame.autotileRules[index];
            painter.text(sideX + 20, overlayY, rule.terrain + " -> tile " + std::to_string(rule.tileValue) +
                         "  priority " + std::to_string(rule.priority), text);
            overlayY += 17;
        }
    }
    if (controller.tile_world_editor().show_parallax_preview()) {
        painter.text(sideX + 12, overlayY, "Parallax layers: " + std::to_string(frame.parallaxLayers.size()) + "  [V]", muted);
        overlayY += 18;
    }
    if (controller.tile_world_editor().show_chunk_diagnostics()) {
        painter.text(sideX + 12, overlayY, "Chunk diagnostics: " + std::to_string(frame.chunks.size()) + "  [C]", muted);
        overlayY += 18;
    }
    if (frame.recookOverlay) {
        const TileRegionalRecookOverlay& recook = *frame.recookOverlay;
        painter.text(sideX + 12, overlayY,
                     "Recook #" + std::to_string(recook.generation) + " region " +
                     std::to_string(recook.region.minCol) + "," + std::to_string(recook.region.minRow) + "-" +
                     std::to_string(recook.region.maxCol) + "," + std::to_string(recook.region.maxRow),
                     rgb(255, 196, 75));
        overlayY += 18;
    }
    const auto errors = std::count_if(frame.validation.begin(), frame.validation.end(),
        [](const TileWorldDiagnostic& diagnostic) {
            return diagnostic.severity == TileWorldDiagnosticSeverity::Error;
        });
    painter.text(sideX + 12, overlayY,
                 frame.validForPlay ? "LEVEL VALID - F5 launches play test"
                                    : "LEVEL INVALID - " + std::to_string(errors) + " errors",
                 frame.validForPlay ? rgb(90, 220, 135) : rgb(245, 90, 85));
}

void render_sprite_animation_graph_panel(const IEditorCanvas& painter,
                                         NativeEditorController& controller,
                                         EditorColor panel, EditorColor panel2,
                                         EditorColor border, EditorColor text,
                                         EditorColor muted, EditorColor accent) {
    if (!controller.sprite_animation_graph_open()) return;

    const SpriteAnimationGraphRect viewport = controller.sprite_animation_graph_viewport();
    const SpriteAnimationGraphFrame frame = controller.sprite_animation_graph().frame(viewport);
    const SpriteAnimationGraphDrawList drawList = build_sprite_animation_graph_draw_list(frame);
    const UiRect panelRect{static_cast<int>(std::lround(viewport.x)) - 12,
                           static_cast<int>(std::lround(viewport.y)) - 44,
                           static_cast<int>(std::lround(viewport.width)) + 24,
                           static_cast<int>(std::lround(viewport.height)) + 84};
    painter.fill(panelRect, rgb(16, 19, 25));
    painter.outline(panelRect, accent);
    painter.fill({panelRect.x, panelRect.y, panelRect.width, 34}, panel);
    painter.text(panelRect.x + 12, panelRect.y + 23, "Sprite Animation State Graph", text);

    std::string breadcrumb;
    for (std::size_t index = 0U; index < frame.breadcrumbs.size(); ++index) {
        if (index != 0U) breadcrumb += " / ";
        breadcrumb += frame.breadcrumbs[index];
    }
    if (!breadcrumb.empty()) {
        painter.text(panelRect.x + 260, panelRect.y + 23, breadcrumb, muted);
    }

    for (const SpriteAnimationGraphDrawCommand& command : drawList.commands) {
        const EditorColor color = graph_color(command.color);
        switch (command.kind) {
        case SpriteAnimationGraphDrawKind::FilledRect:
            painter.fill(graph_rect(command.rect), color);
            break;
        case SpriteAnimationGraphDrawKind::OutlineRect:
            painter.outline(graph_rect(command.rect), color);
            break;
        case SpriteAnimationGraphDrawKind::Polyline:
            for (std::size_t point = 1U; point < command.points.size(); ++point) {
                const SpriteVec2& a = command.points[point - 1U];
                const SpriteVec2& b = command.points[point];
                painter.line(static_cast<int>(std::lround(a.x)), static_cast<int>(std::lround(a.y)),
                             static_cast<int>(std::lround(b.x)), static_cast<int>(std::lround(b.y)),
                             color, std::max(1, static_cast<int>(std::lround(command.thickness))));
            }
            break;
        case SpriteAnimationGraphDrawKind::Circle: {
            const int radius = std::max(1, static_cast<int>(std::lround(command.radius)));
            const int cx = static_cast<int>(std::lround(command.center.x));
            const int cy = static_cast<int>(std::lround(command.center.y));
            painter.fill({cx - radius, cy - radius, radius * 2, radius * 2}, color);
            break;
        }
        case SpriteAnimationGraphDrawKind::Text:
            painter.text(static_cast<int>(std::lround(command.rect.x)),
                         static_cast<int>(std::lround(command.rect.y)), command.text, color);
            break;
        }
    }

    const int inspectorWidth = 258;
    const UiRect inspector{panelRect.x + panelRect.width - inspectorWidth - 8,
                           panelRect.y + 42, inspectorWidth,
                           std::min(254, panelRect.height - 88)};
    painter.fill(inspector, panel2);
    painter.outline(inspector, border);
    painter.text(inspector.x + 10, inspector.y + 22, "Transition Inspector", text);

    int rowY = inspector.y + 44;
    const auto selected = controller.sprite_animation_graph().selected_transition();
    const auto& transitions = controller.sprite_animation_graph().session().asset().transitions;
    if (selected && *selected < transitions.size()) {
        const SpriteAnimationTransition& transition = transitions[*selected];
        const std::array<std::string, 10> rows{
            transition.fromState + " -> " + transition.toState,
            "Priority: " + std::to_string(transition.priority) + "   [ / ]",
            "Blend: " + graph_curve_name(transition.blendCurve) + "   B",
            "Duration: " + std::to_string(transition.blendDurationSeconds).substr(0, 5) + " s",
            "Interrupt: " + graph_interruption_name(transition.interruptionSource),
            "Tracks: " + graph_track_policy_name(transition.trackPolicy) + "   P",
            "Events: " + graph_event_policy_name(transition.eventPolicy) + "   E",
            "Root motion: " + graph_root_policy_name(transition.rootMotionPolicy) + "   M",
            "Conditions: " + std::to_string(transition.conditions.size()),
            transition.hasExitTime ? "Exit time enabled" : "No exit time"};
        for (const std::string& row : rows) {
            painter.text(inspector.x + 10, rowY, row, rowY == inspector.y + 44 ? accent : text);
            rowY += 19;
        }
    } else {
        painter.text(inspector.x + 10, rowY, "Select an edge to inspect it.", muted);
        rowY += 22;
        painter.text(inspector.x + 10, rowY, "Drag from output to input", muted);
        rowY += 18;
        painter.text(inspector.x + 10, rowY, "to create a transition.", muted);
    }

    const std::string metrics = std::to_string(drawList.nodeCount) + " states  |  " +
        std::to_string(drawList.transitionCount) + " transitions  |  " +
        std::to_string(drawList.validationBadgeCount) + " diagnostics";
    painter.text(panelRect.x + 12, panelRect.y + panelRect.height - 30, metrics, muted);
    painter.text(panelRect.x + 12, panelRect.y + panelRect.height - 11,
                 "Drag nodes/ports | Ctrl+C/V | C comment | G group | S subgraph | Esc close", muted);
}

namespace {
std::vector<FittedTextRecord>* g_fittedTextSink = nullptr;

bool utf8_continuation(unsigned char byte) noexcept { return (byte & 0xC0U) == 0x80U; }
} // namespace

std::string elide_text_to_width(const IEditorCanvas& canvas, std::string_view value, int maxWidth) {
    if (canvas.text_width(value) <= maxWidth) return std::string(value);
    const std::string_view marker = canvas.ellipsis();
    if (canvas.text_width(marker) > maxWidth) return {};
    // Candidate cut points: every UTF-8 code point boundary.
    std::vector<std::size_t> cuts;
    cuts.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i)
        if (!utf8_continuation(static_cast<unsigned char>(value[i]))) cuts.push_back(i);
    std::size_t lo = 0, hi = cuts.size();  // cuts[lo] (prefix length) is known to fit
    std::string candidate;
    auto fits = [&](std::size_t index) {
        std::size_t length = cuts[index];
        while (length > 0U && value[length - 1U] == ' ') --length;  // no "abc …"
        candidate.assign(value.substr(0, length));
        candidate.append(marker);
        return canvas.text_width(candidate) <= maxWidth;
    };
    while (lo + 1U < hi) {
        const std::size_t mid = (lo + hi) / 2U;
        if (fits(mid)) lo = mid; else hi = mid;
    }
    (void)fits(lo);
    return candidate;
}

CellFitCanvas::CellFitCanvas(const IEditorCanvas& inner, UiRect bounds, UiRect screen, int hoverX, int hoverY)
    : inner_(inner), bounds_(bounds), screen_(screen), hoverX_(hoverX), hoverY_(hoverY) {
    cells_.reserve(512);
}

void CellFitCanvas::set_record_sink(std::vector<FittedTextRecord>* sink) noexcept { g_fittedTextSink = sink; }

void CellFitCanvas::add_cell(UiRect cell, bool focused) {
    if (cell.width > 0 && cell.height > 0) cells_.push_back({cell, focused});
}

void CellFitCanvas::text(int x, int y, std::string_view value, EditorColor color) const {
    const Cell* cell = nullptr;
    for (const Cell& candidate : cells_)
        if (candidate.rect.contains(x, y - 4)) { cell = &candidate; break; }
    const UiRect rect = cell != nullptr ? cell->rect : bounds_;
    const int maxWidth = rect.x + rect.width - 3 - x;
    const std::string fitted = elide_text_to_width(inner_, value, maxWidth);
    if (!fitted.empty()) inner_.text(x, y, fitted, color);
    const bool elided = fitted != value;
    if (elided) {
        const bool hovered = rect.contains(hoverX_, hoverY_);
        const bool focused = cell != nullptr && cell->focused;
        if ((hovered || focused) && (!tip_ || (hovered && !tip_->hovered)))
            tip_ = Tip{rect, std::string(value), hovered};
    }
    if (g_fittedTextSink != nullptr) {
        FittedTextRecord record{rect, x, y, fitted, std::string(value), elided};
        records_.push_back(record);
        g_fittedTextSink->push_back(std::move(record));
    }
}

std::optional<std::string> CellFitCanvas::tooltip_text() const {
    if (!tip_) return std::nullopt;
    return tip_->text;
}

void CellFitCanvas::draw_tooltip(EditorColor background, EditorColor border, EditorColor color) const {
    if (!tip_) return;
    const std::string label = elide_text_to_width(inner_, tip_->text, std::max(0, screen_.width - 12));
    const int width = std::min(screen_.width, inner_.text_width(label) + 12);
    constexpr int height = 22;
    const UiRect cell = tip_->cell;
    const int x = std::clamp(cell.x, screen_.x, screen_.x + screen_.width - width);
    int y = cell.y + cell.height + 2;
    if (y + height > screen_.y + screen_.height) y = std::max(screen_.y, cell.y - height - 2);
    inner_.fill({x, y, width, height}, background);
    inner_.outline({x, y, width, height}, border);
    inner_.text(x + 6, y + 16, label, color);
}

namespace {
// A multi-line hover tooltip for the main editor chrome (toolbar, menus, camera preview).
// The first line is drawn in the text colour, the rest muted. Lines are elided to the screen.
struct ChromeTooltip {
    UiRect anchor{};
    std::vector<std::string> lines;
    bool beside{};  // to the right of the anchor (menu rows) instead of below it
};

void draw_chrome_tooltip(const IEditorCanvas& painter, const ChromeTooltip& tip, UiRect screen,
                         EditorColor background, EditorColor border, EditorColor text, EditorColor muted) {
    if (tip.lines.empty()) return;
    constexpr int kLineHeight = 16;
    std::vector<std::string> lines;
    int width = 0;
    for (const std::string& line : tip.lines) {
        lines.push_back(elide_text_to_width(painter, line, std::max(0, screen.width - 16)));
        width = std::max(width, painter.text_width(lines.back()) + 12);
    }
    width = std::min(width, screen.width);
    const int height = static_cast<int>(lines.size()) * kLineHeight + 8;
    int x = tip.anchor.x;
    int y = tip.anchor.y + tip.anchor.height + 2;
    if (tip.beside) {
        x = tip.anchor.x + tip.anchor.width + 4;
        y = tip.anchor.y;
        if (x + width > screen.x + screen.width) x = tip.anchor.x - width - 4;
    }
    x = std::clamp(x, screen.x, std::max(screen.x, screen.x + screen.width - width));
    if (y + height > screen.y + screen.height) y = std::max(screen.y, tip.anchor.y - height - 2);
    painter.fill({x, y, width, height}, background);
    painter.outline({x, y, width, height}, border);
    for (std::size_t i = 0; i < lines.size(); ++i)
        painter.text(x + 6, y + 4 + static_cast<int>(i + 1) * kLineHeight - 4, lines[i], i == 0 ? text : muted);
}

// Registers `down .. up` value slots and label slots for a row of stepper buttons.
void add_stepper_cells(CellFitCanvas& canvas, UiRect row, UiRect down, UiRect up, UiRect toggle = {}) {
    if (row.width <= 0) return;
    canvas.add_cell(down);
    canvas.add_cell(up);
    canvas.add_cell(toggle);
    if (down.width > 0 && up.width > 0)
        canvas.add_cell({down.x + down.width, row.y, up.x - (down.x + down.width), row.height});
    int labelRight = row.x + row.width;
    for (const UiRect& control : {down, toggle})
        if (control.width > 0) labelRight = std::min(labelRight, control.x - 2);
    canvas.add_cell({row.x, row.y, labelRight - row.x, row.height});
    canvas.add_cell(row);
}

void draw_piano_keyboard(const IEditorCanvas& painter, const PianoKeyboard& keyboard,
                         const std::array<bool, 128>& activeNotes, EditorColor accent, EditorColor text) {
    const UiRect keysArea = keyboard.keys_area();
    const int areaRight = keysArea.x + keysArea.width;
    const int computerFirst = keyboard.computer_key_base();
    const int computerLast = computerFirst + 19;
    for (const PianoKey& key : keyboard.visible_keys()) {
        const bool active = key.midi >= 0 && key.midi < 128 && activeNotes[static_cast<std::size_t>(key.midi)];
        const EditorColor color = active ? accent : (key.black ? rgb(18,20,24) : rgb(220,225,232));
        painter.fill(key.rect, color);
        painter.outline(key.rect, rgb(60,65,74));
        if (key.black) continue;
        if (key.midi >= computerFirst && key.midi <= computerLast)  // computer-keyboard window (Z = first)
            painter.fill({key.rect.x + 1, key.rect.y + key.rect.height - 3, std::max(1, key.rect.width - 2), 2},
                         rgb(96,150,220));
    }
    // C-octave labels last: a label may run into the next (always white) D key, whose
    // fill would otherwise paint over it. Black keys never reach the label row.
    for (const PianoKey& key : keyboard.visible_keys()) {
        if (key.black || key.midi % 12 != 0) continue;
        const bool active = key.midi >= 0 && key.midi < 128 && activeNotes[static_cast<std::size_t>(key.midi)];
        const std::string label = piano_note_name(key.midi);
        const auto full = keyboard.key_rect(key.midi);
        const int labelX = (full ? full->x : key.rect.x) + 3;
        const int labelWidth = painter.text_width(label);
        const int room = 2 * keyboard.white_key_width() - 4;
        if (labelWidth > room || labelX < keysArea.x || labelX + labelWidth > areaRight) continue;
        painter.text(labelX, key.rect.y + key.rect.height - 7, label, active ? text : rgb(30,34,40));
    }
    if (keyboard.scrollable()) {
        painter.fill(keyboard.scrollbar_track(), rgb(20,25,31));
        painter.fill(keyboard.scrollbar_thumb(), rgb(96,110,130));
    }
}
} // namespace

UiRect editor_screen_rect(const NativeEditorController& controller) noexcept {
    const NativeEditorLayout& layout = controller.layout();
    return {0, 0, std::max(1, layout.menuBar.width), std::max(1, layout.statusBar.y + layout.statusBar.height)};
}

void register_synth_text_cells(CellFitCanvas& canvas, const EditorSynthPanel& synthPanel) {
    const SynthPanelLayout& layout = synthPanel.layout();
    if (synthPanel.search_panel().open()) canvas.add_cell(synthPanel.search_panel().layout().panel);
    // Fixed chrome first, so page content that overflows cannot capture its text.
    canvas.add_cell(layout.keyboardKeysButton);
    canvas.add_cell(layout.meterArea);
    canvas.add_cell(layout.pianoArea);
    const int helpY = layout.pianoArea.y + layout.pianoArea.height;
    canvas.add_cell({layout.panel.x, helpY, layout.keyboardKeysButton.x - 4 - layout.panel.x,
                     layout.panel.y + layout.panel.height - helpY});
    for (const UiRect& rect : {layout.closeButton, layout.panicButton, layout.searchButton, layout.resetButton,
                               layout.octaveDownButton, layout.octaveUpButton, layout.midiThruButton,
                               layout.midiInputButton, layout.keyboardKeysButton})
        canvas.add_cell(rect);
    canvas.add_cell({layout.octaveDownButton.x + layout.octaveDownButton.width, layout.octaveDownButton.y,
                     layout.octaveUpButton.x - layout.octaveDownButton.x - layout.octaveDownButton.width,
                     layout.octaveDownButton.height});
    for (const UiRect& tab : layout.tabButtons) canvas.add_cell(tab);
    const SynthPanelPage page = synthPanel.page();
    auto value_slot = [&](UiRect down, UiRect up) {
        canvas.add_cell(down); canvas.add_cell(up);
        canvas.add_cell({down.x + down.width, down.y, up.x - down.x - down.width, down.height});
    };
    if (page == SynthPanelPage::Oscillators) {
        if (layout.oscillatorCompact && layout.oscillatorHeader.width > 0) {
            // Column labels: each spans its value column (wave button, or "-" .. "+").
            const UiRect& header = layout.oscillatorHeader;
            const UiRect wave = layout.oscillatorWaveButtons[0];
            canvas.add_cell({wave.x, header.y, wave.width, header.height});
            const std::array<std::pair<UiRect, UiRect>, 4> groups{{
                {layout.oscillatorGainDownButtons[0], layout.oscillatorGainUpButtons[0]},
                {layout.oscillatorTuneDownButtons[0], layout.oscillatorTuneUpButtons[0]},
                {layout.oscillatorFineDownButtons[0], layout.oscillatorFineUpButtons[0]},
                {layout.oscillatorPwmDownButtons[0], layout.oscillatorPwmUpButtons[0]}}};
            for (const auto& [down, up] : groups)
                canvas.add_cell({down.x, header.y, up.x + up.width - down.x, header.height});
            canvas.add_cell(header);
        }
        for (std::size_t i = 0; i < layout.oscillatorRows.size(); ++i) {
            const UiRect row = layout.oscillatorRows[i];
            if (row.width <= 0) continue;
            canvas.add_cell(layout.oscillatorEnableButtons[i]);
            canvas.add_cell(layout.oscillatorWaveButtons[i]);
            value_slot(layout.oscillatorGainDownButtons[i], layout.oscillatorGainUpButtons[i]);
            value_slot(layout.oscillatorTuneDownButtons[i], layout.oscillatorTuneUpButtons[i]);
            value_slot(layout.oscillatorFineDownButtons[i], layout.oscillatorFineUpButtons[i]);
            value_slot(layout.oscillatorPwmDownButtons[i], layout.oscillatorPwmUpButtons[i]);
            const int nameX = layout.oscillatorEnableButtons[i].x + layout.oscillatorEnableButtons[i].width + 2;
            canvas.add_cell({nameX, row.y, layout.oscillatorWaveButtons[i].x - 2 - nameX, row.height});
        }
        for (std::size_t i = 0; i < layout.oscillatorAdvancedRows.size(); ++i)
            add_stepper_cells(canvas, layout.oscillatorAdvancedRows[i], layout.oscillatorAdvancedDownButtons[i],
                              layout.oscillatorAdvancedUpButtons[i]);
        for (const UiRect& rect : layout.wavetableFrameButtons) canvas.add_cell(rect);
        for (const UiRect& rect : {layout.wavetableNormalizeButton, layout.wavetableRemoveDcButton,
                                   layout.wavetableAlignButton, layout.wavetableCanvas})
            canvas.add_cell(rect);
        for (const UiRect& row : layout.oscillatorRows) canvas.add_cell(row);
    } else if (page == SynthPanelPage::Effects) {
        canvas.add_cell(layout.effectParamTitle);
        for (std::size_t i = 0; i < layout.effectRows.size(); ++i) {
            const UiRect row = layout.effectRows[i];
            if (row.width <= 0) continue;
            canvas.add_cell(layout.effectToggleButtons[i]);
            canvas.add_cell({row.x, row.y, layout.effectToggleButtons[i].x - 2 - row.x, row.height});
        }
        for (std::size_t i = 0; i < layout.effectParamRows.size(); ++i)
            add_stepper_cells(canvas, layout.effectParamRows[i], layout.effectParamDownButtons[i],
                              layout.effectParamUpButtons[i], layout.effectParamToggleButtons[i]);
    } else if (page == SynthPanelPage::Presets) {
        for (const UiRect& rect : {layout.presetScanButton, layout.presetPreviousButton, layout.presetNextButton,
                                   layout.presetLoadButton, layout.presetCaptureAButton, layout.presetCaptureBButton})
            canvas.add_cell(rect);
        value_slot(layout.presetMorphDownButton, layout.presetMorphUpButton);
        for (const UiRect& rect : layout.presetEntryButtons) canvas.add_cell(rect);
        canvas.add_cell(layout.presetStatusRow);
        for (std::size_t i = 0; i < 7U; ++i)
            add_stepper_cells(canvas, layout.parameterRows[i], layout.parameterDownButtons[i],
                              layout.parameterUpButtons[i], layout.parameterToggleButtons[i]);
    } else {
        for (std::size_t i = 0; i < layout.parameterRows.size(); ++i)
            add_stepper_cells(canvas, layout.parameterRows[i], layout.parameterDownButtons[i],
                              layout.parameterUpButtons[i], layout.parameterToggleButtons[i]);
        if (page == SynthPanelPage::Modulation)
            for (std::size_t i = 0; i < layout.macroRows.size(); ++i)
                add_stepper_cells(canvas, layout.macroRows[i], layout.macroDownButtons[i], layout.macroUpButtons[i]);
        if (page == SynthPanelPage::Performance) {
            for (const UiRect& rect : layout.arpeggiatorStepButtons) canvas.add_cell(rect);
            for (std::size_t i = 0; i < layout.arpeggiatorStepRows.size(); ++i)
                add_stepper_cells(canvas, layout.arpeggiatorStepRows[i], layout.arpeggiatorStepDownButtons[i],
                                  layout.arpeggiatorStepUpButtons[i], layout.arpeggiatorStepToggleButtons[i]);
        }
    }
    {
        // Title text ends before the first title-bar button (MIDI input on compact panels).
        const int titleRight = layout.midiInputButton.y == layout.titleBar.y + 5
            ? std::min(layout.searchButton.x, layout.midiInputButton.x) : layout.searchButton.x;
        canvas.add_cell({layout.titleBar.x, layout.titleBar.y, titleRight - 4 - layout.titleBar.x, layout.titleBar.height});
    }
    canvas.add_cell(layout.titleBar);
}

void render_synth_panel(const IEditorCanvas& outerPainter, NativeEditorController& controller,
                        EditorColor panel, EditorColor panel2, EditorColor border,
                        EditorColor text, EditorColor muted, EditorColor accent) {
    if (!controller.synth_panel().open()) return;
    const SynthPanelLayout& layout = controller.synth_panel().layout();
    const audio::SynthPreset preset = controller.synthesizer().preset();
    const audio::SynthMeters meters = controller.synthesizer().meters();
    const auto voices = controller.synthesizer().voices();
    // Every label/value is fitted to its cell (elided with an ellipsis); the
    // hovered cell's full text is shown as a tooltip after the panel.
    CellFitCanvas painter(outerPainter, layout.panel, editor_screen_rect(controller),
                          controller.hover_x(), controller.hover_y());
    register_synth_text_cells(painter, controller.synth_panel());
    auto compact = [](float value, int digits = 2) {
        std::string result = std::to_string(value);
        const auto dot = result.find('.');
        if (dot != std::string::npos && dot + 1U + static_cast<std::size_t>(digits) < result.size())
            result.resize(dot + 1U + static_cast<std::size_t>(digits));
        return result;
    };
    auto bool_text = [](bool value) -> std::string { return value ? "On" : "Off"; };
    auto source_text = [](std::int8_t source) {
        return source < 0 ? std::string("Off") : "Osc " + std::to_string(static_cast<int>(source) + 1);
    };
    auto filter_oversampling_text = [](audio::FilterOversampling value) {
        if (value == audio::FilterOversampling::Auto) return std::string("Auto");
        return std::to_string(static_cast<unsigned>(value)) + "x";
    };
    auto fm_mode_text = [](audio::FrequencyModulationMode value) -> std::string_view {
        switch (value) {
            case audio::FrequencyModulationMode::Off: return "Off";
            case audio::FrequencyModulationMode::Linear: return "Linear";
            case audio::FrequencyModulationMode::Exponential: return "Exponential";
        }
        return "Off";
    };
    auto curve_text = [](audio::ModulationCurve value) -> std::string_view {
        switch (value) {
            case audio::ModulationCurve::Linear: return "Linear";
            case audio::ModulationCurve::Quadratic: return "Quadratic";
            case audio::ModulationCurve::Cubic: return "Cubic";
        }
        return "Linear";
    };
    auto scale_text = [](audio::ChordScale value) -> std::string_view {
        switch (value) {
            case audio::ChordScale::Chromatic: return "Chromatic";
            case audio::ChordScale::Major: return "Major";
            case audio::ChordScale::NaturalMinor: return "Natural minor";
            case audio::ChordScale::HarmonicMinor: return "Harmonic minor";
            case audio::ChordScale::Dorian: return "Dorian";
            case audio::ChordScale::Mixolydian: return "Mixolydian";
            case audio::ChordScale::Pentatonic: return "Pentatonic";
        }
        return "Chromatic";
    };
    auto clock_text = [](audio::ArpeggiatorClockSource value) -> std::string_view {
        switch (value) {
            case audio::ArpeggiatorClockSource::Internal: return "Internal";
            case audio::ArpeggiatorClockSource::GameClock: return "Game";
            case audio::ArpeggiatorClockSource::MidiClock: return "MIDI";
        }
        return "Internal";
    };
    auto button = [&](UiRect rect, std::string_view label, bool active = false) {
        painter.fill(rect, active ? accent : panel2); painter.outline(rect, border);
        painter.text(rect.x + 6, rect.y + 17, label, active ? rgb(255,255,255) : text);
    };
    auto draw_parameter_rows = [&](const std::array<std::string, kSynthParameterRowCount>& labels,
                                   const std::array<std::string, kSynthParameterRowCount>& values,
                                   const std::array<bool, kSynthParameterRowCount>& toggles,
                                   const std::array<bool, kSynthParameterRowCount>& active) {
        for (std::size_t i = 0; i < layout.parameterRows.size(); ++i) {
            if (layout.parameterRows[i].width <= 0) continue;  // scrolled out of view
            painter.fill(layout.parameterRows[i], panel); painter.outline(layout.parameterRows[i], border);
            painter.text(layout.parameterRows[i].x + 7, layout.parameterRows[i].y + 18, labels[i], text);
            if (toggles[i]) button(layout.parameterToggleButtons[i], values[i], active[i]);
            else {
                button(layout.parameterDownButtons[i], "-");
                painter.text(layout.parameterDownButtons[i].x + 32,
                             layout.parameterDownButtons[i].y + 16, values[i], text);
                button(layout.parameterUpButtons[i], "+");
            }
        }
    };

    painter.fill(layout.panel, rgb(12,17,26)); painter.outline(layout.panel, accent);
    painter.fill(layout.titleBar, rgb(27,44,67));
    painter.text(layout.titleBar.x + 12, layout.titleBar.y + 23,
                 "DVE Eightfold Modular - synthesis, sequencing, modulation, MIDI and presets", text);
    painter.fill(layout.closeButton, rgb(100,42,48)); painter.outline(layout.closeButton, border);
    painter.text(layout.closeButton.x + 7, layout.closeButton.y + 17, "X", text);
    painter.fill(layout.panicButton, rgb(104,48,42)); painter.outline(layout.panicButton, border);
    painter.text(layout.panicButton.x + 8, layout.panicButton.y + 17, "ALL NOTES OFF", text);
    button(layout.searchButton, "SEARCH", controller.synth_panel().search_panel().open());
    button(layout.resetButton, painter.text_width("Reset preset") + 9 <= layout.resetButton.width ? "Reset preset" : "Reset");
    button(layout.octaveDownButton, "-");
    painter.text(layout.octaveDownButton.x + layout.octaveDownButton.width + 4, layout.octaveDownButton.y + 17,
                 "Oct " + std::to_string(controller.synth_panel().octave()), text);
    button(layout.octaveUpButton, "+"); button(layout.midiThruButton, "MIDI THRU", preset.midiThru);
    {
        // MIDI input port + connection status (green when connected, amber otherwise).
        const UiRect rect = layout.midiInputButton;
        const bool connected = controller.synth_panel().midi_input_connected();
        painter.fill(rect, connected ? rgb(28,74,52) : rgb(74,58,28)); painter.outline(rect, border);
        painter.text(rect.x + 6, rect.y + 17, controller.synth_panel().midi_input_label(), text);
    }
    static constexpr std::array<std::string_view, kSynthPanelPageCount> pageNames{
        "OSC", "FILTER / ENV", "MOD MATRIX", "PERFORM", "EFFECTS", "PRESETS", "EXPRESSION", "GENERATIVE"};
    static constexpr std::array<std::string_view, kSynthPanelPageCount> shortPageNames{
        "OSC", "FLT", "MOD", "PRF", "FX", "PRE", "EXP", "GEN"};
    for (std::size_t i = 0; i < layout.tabButtons.size(); ++i) {
        // Narrow panels use short tab names (still elided / tooltipped if even those do not fit).
        const bool fits = painter.text_width(pageNames[i]) + 9 <= layout.tabButtons[i].width;
        button(layout.tabButtons[i], fits ? pageNames[i] : shortPageNames[i],
               static_cast<std::size_t>(controller.synth_panel().page()) == i);
    }

    const SynthPanelPage page = controller.synth_panel().page();
    if (page == SynthPanelPage::Oscillators) {
        const bool compactOsc = layout.oscillatorCompact;
        if (compactOsc && layout.oscillatorHeader.width > 0 && layout.oscillatorRows[0].width > 0) {
            // Compact columns: one label per value column instead of one per row.
            const std::size_t first = [&] {
                for (std::size_t i = 0; i < layout.oscillatorRows.size(); ++i)
                    if (layout.oscillatorRows[i].width > 0) return i;
                return std::size_t{0};
            }();
            const int baseline = layout.oscillatorHeader.y + 14;
            painter.text(layout.oscillatorWaveButtons[first].x + 2, baseline, "Wave", muted);
            painter.text(layout.oscillatorGainDownButtons[first].x + 2, baseline, "Gain", muted);
            painter.text(layout.oscillatorTuneDownButtons[first].x + 2, baseline, "Semi", muted);
            painter.text(layout.oscillatorFineDownButtons[first].x + 2, baseline, "Cents", muted);
            painter.text(layout.oscillatorPwmDownButtons[first].x + 2, baseline, "PWM", muted);
        }
        for (std::size_t i = 0; i < layout.oscillatorRows.size(); ++i) {
            if (layout.oscillatorRows[i].width <= 0) continue;  // scrolled out of view
            const auto& osc = preset.oscillators[i];
            const bool selected = controller.synth_panel().selected_oscillator() == i;
            painter.fill(layout.oscillatorRows[i], selected ? rgb(39,58,82) : panel);
            painter.outline(layout.oscillatorRows[i], selected ? accent : border);
            button(layout.oscillatorEnableButtons[i], osc.enabled ? "ON" : "--", osc.enabled);
            painter.text(layout.oscillatorRows[i].x + 40, layout.oscillatorRows[i].y + 21,
                         "OSC " + std::to_string(i + 1U), text);
            button(layout.oscillatorWaveButtons[i], audio::oscillator_waveform_name(osc.waveform));
            if (!compactOsc) painter.text(layout.oscillatorGainDownButtons[i].x - 38, layout.oscillatorGainDownButtons[i].y + 16, "Gain", muted);
            button(layout.oscillatorGainDownButtons[i], "-");
            painter.text(layout.oscillatorGainDownButtons[i].x + 31, layout.oscillatorGainDownButtons[i].y + 16, compact(osc.gain), text);
            button(layout.oscillatorGainUpButtons[i], "+");
            if (!compactOsc) painter.text(layout.oscillatorTuneDownButtons[i].x - 38, layout.oscillatorTuneDownButtons[i].y + 16, "Semi", muted);
            button(layout.oscillatorTuneDownButtons[i], "-");
            painter.text(layout.oscillatorTuneDownButtons[i].x + 31, layout.oscillatorTuneDownButtons[i].y + 16,
                         std::to_string(static_cast<int>(osc.semitones)), text);
            button(layout.oscillatorTuneUpButtons[i], "+");
            if (!compactOsc) painter.text(layout.oscillatorFineDownButtons[i].x - 42, layout.oscillatorFineDownButtons[i].y + 16, "Cents", muted);
            button(layout.oscillatorFineDownButtons[i], "-");
            painter.text(layout.oscillatorFineDownButtons[i].x + 31, layout.oscillatorFineDownButtons[i].y + 16,
                         std::to_string(static_cast<int>(osc.cents)), text);
            button(layout.oscillatorFineUpButtons[i], "+");
            if (!compactOsc) painter.text(layout.oscillatorPwmDownButtons[i].x - 38, layout.oscillatorPwmDownButtons[i].y + 16, "PWM", muted);
            button(layout.oscillatorPwmDownButtons[i], "-");
            painter.text(layout.oscillatorPwmDownButtons[i].x + 31, layout.oscillatorPwmDownButtons[i].y + 16,
                         compact(osc.pwmDepth), text);
            button(layout.oscillatorPwmUpButtons[i], "+");
        }
        const auto& osc = preset.oscillators[controller.synth_panel().selected_oscillator()];
        // Phase 4/5: quality-tier text shared by the granular and spectral
        // advanced sections.
        auto quality_tier_text = [](audio::FilterQuality value) -> std::string_view {
            switch (value) {
                case audio::FilterQuality::Eco: return "Eco";
                case audio::FilterQuality::Standard: return "Standard";
                case audio::FilterQuality::High: return "High";
                case audio::FilterQuality::Offline: return "Offline";
            }
            return "Standard";
        };
        std::array<std::string, kSynthOscillatorAdvancedPropertyCount> labels{
            "PWM rate", "Shape / table", "Hard-sync source", "FM source", "FM mode",
            "FM amount", "Ring source", "Ring depth", "Sub level", "Sub octaves"};
        std::array<std::string, kSynthOscillatorAdvancedPropertyCount> values{
            compact(osc.pwmRateHertz), osc.waveform == audio::OscillatorWaveform::Wavetable ? compact(osc.wavetablePosition) : compact(osc.shape),
            source_text(osc.hardSyncSource), source_text(osc.frequencyModSource), std::string(fm_mode_text(osc.frequencyModMode)),
            compact(osc.frequencyModAmount), source_text(osc.ringModSource), compact(osc.ringModDepth),
            compact(osc.subOscillatorLevel), std::to_string(osc.subOscillatorOctaves)};
        if (osc.waveform == audio::OscillatorWaveform::Sample) {
            labels = {"Sample start", "Sample end", "Loop start", "Loop end", "Loop",
                      "Reverse", "Key tracking", "Velocity gain", "One shot", "Root MIDI note"};
            values = {compact(osc.sampleStart), compact(osc.sampleEnd), compact(osc.sampleLoopStart),
                      compact(osc.sampleLoopEnd), osc.sampleLoop ? "On" : "Off", osc.sampleReverse ? "On" : "Off",
                      osc.sampleKeyTrack ? "On" : "Off", compact(osc.sampleVelocityToGain),
                      osc.sampleOneShot ? "On" : "Off", std::to_string(preset.sampleBank.rootNote)};
        } else if (osc.waveform == audio::OscillatorWaveform::Sampler) {
            labels = {"Playback mode", "Direction", "Pitch tracking", "Start offset s", "Gain",
                      "Loop start s", "Loop end s", "Loop xfade s", "Sampler enabled", "Root MIDI note"};
            values = {std::string(audio::sampler_playback_mode_name(preset.sampler.playbackMode)),
                      std::string(audio::sampler_direction_name(preset.sampler.direction)),
                      preset.sampler.pitchTracking ? "On" : "Off", compact(preset.sampler.startOffsetSeconds),
                      compact(preset.sampler.gain), compact(preset.sampler.loopStartSeconds),
                      compact(preset.sampler.loopEndSeconds), compact(preset.sampler.loopCrossfadeSeconds),
                      preset.sampler.enabled ? "On" : "Off", std::to_string(preset.sampleBank.rootNote)};
        } else if (osc.waveform == audio::OscillatorWaveform::ModalResonator) {
            const auto& modal = osc.modalResonator;
            labels = {"Excitation", "Base freq Hz", "Damping", "Inharmonicity", "Brightness",
                      "Excitation level", "Noise burst ms", "Transient ms", "Mode count", "Fundamental ratio"};
            values = {std::string(audio::modal_excitation_source_name(modal.excitation)),
                      modal.baseFrequency <= 0.0F ? "Note" : compact(modal.baseFrequency, 0),
                      compact(modal.damping), compact(modal.inharmonicity), compact(modal.brightness),
                      compact(modal.excitationLevel), compact(modal.noiseBurstMilliseconds),
                      compact(modal.transientMilliseconds), std::to_string(modal.modeCount),
                      compact(modal.modes[0].frequencyRatio)};
        } else if (osc.waveform == audio::OscillatorWaveform::Granular) {
            // Phase 4: dedicated granular generator (preset-level GranularParameters).
            const auto& granular = preset.granular;
            labels = {"Granular", "Density Hz", "Duration ms", "Position", "Position jitter",
                      "Envelope", "Cloud", "Scatter", "Dust", "Freeze", "Smear", "Width",
                      "Pitch semitones", "Gain", "Pan scatter", "Reverse prob",
                      "Freeze position", "Quality"};
            static constexpr std::array<std::string_view, 4> envelopes{
                "Hann", "Triangle", "Exponential", "Planck"};
            values = {granular.enabled ? "On" : "Off", compact(granular.densityHz, 1),
                      compact(granular.durationMs, 0), compact(granular.position01),
                      compact(granular.positionJitter01),
                      std::string(envelopes[static_cast<std::size_t>(granular.envelopeShape)]),
                      compact(granular.cloud01), compact(granular.scatter01),
                      compact(granular.dust01), compact(granular.freeze01),
                      compact(granular.smear01), compact(granular.width01),
                      compact(granular.pitchSemitones, 0), compact(granular.gain),
                      compact(granular.panScatter01), compact(granular.reverseProbability01),
                      compact(granular.freezePosition01),
                      std::string(quality_tier_text(granular.granularQuality))};
        } else if (osc.waveform == audio::OscillatorWaveform::Spectral) {
            // Phase 5: spectral/resynthesis oscillator (preset-level
            // SpectralParameters, spectral.hpp contract). 14 rows of the
            // 18-row advanced budget.
            const auto& spectral = preset.spectral;
            labels = {"Spectral", "Gain", "Stretch", "Freeze", "Formant st",
                      "Harmonic stretch", "Tilt dB/oct", "Threshold", "Blur",
                      "Quantize", "Inharmonicity", "Phase rnd", "Stereo spread",
                      "Quality"};
            values = {spectral.enabled ? "On" : "Off", compact(spectral.gain),
                      compact(spectral.timeStretch, 2), compact(spectral.freeze01),
                      compact(spectral.formantShiftSemitones, 1), compact(spectral.harmonicStretch, 2),
                      compact(spectral.spectralTiltDbPerOct, 1), compact(spectral.partialThreshold01),
                      compact(spectral.spectralBlur01), compact(spectral.frequencyQuantize01),
                      compact(spectral.inharmonicity01), compact(spectral.phaseRandom01),
                      compact(spectral.stereoSpread01),
                      std::string(quality_tier_text(spectral.spectralQuality))};
        }
        // Other waveforms only define the original 10 advanced rows; the
        // extra rows are the Phase 4 granular section (18 rows) and the
        // Phase 5 spectral section (14 of the same 18-row budget, so no ABI
        // break).
        const std::size_t advancedRowCount =
            osc.waveform == audio::OscillatorWaveform::Granular
                ? kSynthOscillatorAdvancedPropertyCount
                : (osc.waveform == audio::OscillatorWaveform::Spectral
                       ? kSynthSpectralAdvancedRowCount
                       : 10U);
        for (std::size_t i = 0; i < advancedRowCount; ++i) {
            if (layout.oscillatorAdvancedRows[i].width <= 0) continue;  // scrolled out of view
            painter.fill(layout.oscillatorAdvancedRows[i], panel); painter.outline(layout.oscillatorAdvancedRows[i], border);
            painter.text(layout.oscillatorAdvancedRows[i].x + 7, layout.oscillatorAdvancedRows[i].y + 18, labels[i], text);
            button(layout.oscillatorAdvancedDownButtons[i], "-");
            painter.text(layout.oscillatorAdvancedDownButtons[i].x + 32,
                         layout.oscillatorAdvancedDownButtons[i].y + 16, values[i], text);
            button(layout.oscillatorAdvancedUpButtons[i], "+");
        }
        if (osc.waveform == audio::OscillatorWaveform::Wavetable && preset.wavetable.enabled &&
            layout.wavetableCanvas.width > 0) {
            const UiRect graph = layout.wavetableCanvas;
            painter.fill(graph, rgb(16,22,31)); painter.outline(graph, border);
            painter.text(graph.x + 7, graph.y + 16,
                         "Wavetable: " + preset.wavetable.name + " | position " + compact(osc.wavetablePosition), muted);
            const std::size_t frames = std::max<std::size_t>(1U, preset.wavetable.frameCount);
            const float position = std::clamp(osc.wavetablePosition, 0.0F, 1.0F) * static_cast<float>(frames - 1U);
            const std::size_t frame = std::min<std::size_t>(static_cast<std::size_t>(position), frames - 1U);
            for (std::size_t i = 1; i < audio::kWavetableSampleCount; ++i) {
                const float a = preset.wavetable.samples[frame * audio::kWavetableSampleCount + i - 1U];
                const float b = preset.wavetable.samples[frame * audio::kWavetableSampleCount + i];
                const int x0 = graph.x + static_cast<int>((i - 1U) * static_cast<std::size_t>(graph.width - 2) /
                                                          (audio::kWavetableSampleCount - 1U));
                const int x1 = graph.x + static_cast<int>(i * static_cast<std::size_t>(graph.width - 2) /
                                                          (audio::kWavetableSampleCount - 1U));
                const int y0 = graph.y + 39 - static_cast<int>(a * 22.0F);
                const int y1 = graph.y + 39 - static_cast<int>(b * 22.0F);
                painter.line(x0, y0, x1, y1, accent, 1);
            }
        }
        if (osc.waveform == audio::OscillatorWaveform::Wavetable) {
            for (std::size_t i = 0; i < layout.wavetableFrameButtons.size(); ++i) {
                if (layout.wavetableFrameButtons[i].width <= 0) continue;
                const bool selectedFrame = i == controller.synth_panel().selected_wavetable_frame();
                painter.fill(layout.wavetableFrameButtons[i], selectedFrame ? accent : panel2);
                painter.outline(layout.wavetableFrameButtons[i], border);
                painter.text(layout.wavetableFrameButtons[i].x + 8, layout.wavetableFrameButtons[i].y + 16,
                             "F" + std::to_string(i + 1U), text);
            }
            if (layout.wavetableNormalizeButton.width > 0) {
                button(layout.wavetableNormalizeButton, "Normalize");
                button(layout.wavetableRemoveDcButton, "Remove DC");
                button(layout.wavetableAlignButton, "Align");
            }
        }
        if ((osc.waveform == audio::OscillatorWaveform::Sample || osc.waveform == audio::OscillatorWaveform::Granular) &&
            preset.sampleBank.enabled && preset.sampleBank.frameCount > 1U && layout.wavetableCanvas.width > 0) {
            const UiRect graph = layout.wavetableCanvas;
            painter.fill(graph, rgb(16,22,31)); painter.outline(graph, border);
            painter.text(graph.x + 7, graph.y + 16,
                         "Sample: " + preset.sampleBank.name + " | " + std::to_string(preset.sampleBank.frameCount) +
                         " frames | root " + std::to_string(preset.sampleBank.rootNote), muted);
            const std::size_t count = preset.sampleBank.frameCount;
            const std::size_t stride = std::max<std::size_t>(1U, count / static_cast<std::size_t>(graph.width - 2));
            std::size_t previous = 0U;
            for (std::size_t i = stride; i < count; i += stride) {
                const int x0 = graph.x + static_cast<int>(previous * static_cast<std::size_t>(graph.width - 2) / (count - 1U));
                const int x1 = graph.x + static_cast<int>(i * static_cast<std::size_t>(graph.width - 2) / (count - 1U));
                const int y0 = graph.y + 42 - static_cast<int>(preset.sampleBank.samples[previous] * 20.0F);
                const int y1 = graph.y + 42 - static_cast<int>(preset.sampleBank.samples[i] * 20.0F);
                painter.line(x0, y0, x1, y1, accent, 1);
                previous = i;
            }
        }
    } else if (page == SynthPanelPage::FilterEnvelope) {
        std::array<std::string, kSynthParameterRowCount> labels{
            "Filter model", "Filter output", "Cutoff Hz", "Resonance", "Drive", "Envelope depth",
            "Ladder bass comp", "SEM morph", "Oversampling", "MS-20 high-pass", "Self oscillation", "Key tracking",
            "Amp delay", "Amp attack", "Amp hold", "Amp decay", "Amp sustain", "Amp release",
            "Filter delay", "Filter attack", "Filter hold", "Filter decay", "Filter sustain", "Filter release",
            "Comb damping", "Comb mix", "Comb feedback", "Formant dry mix"};
        std::array<std::string, kSynthParameterRowCount> values{
            std::string(audio::filter_topology_name(preset.filter.topology)), std::string(audio::filter_mode_name(preset.filter.mode)),
            compact(preset.filter.cutoffHertz,0), compact(preset.filter.resonance), compact(preset.filter.drive),
            compact(preset.filter.envelopeAmountOctaves), compact(preset.filter.bassCompensation), compact(preset.filter.morph),
            filter_oversampling_text(preset.filter.oversampling), compact(preset.filter.ms20HighPassCutoffHertz,0),
            compact(preset.filter.selfOscillation), compact(preset.filter.keyTrack), compact(preset.ampEnvelope.delaySeconds,3),
            compact(preset.ampEnvelope.attackSeconds,3), compact(preset.ampEnvelope.holdSeconds,3), compact(preset.ampEnvelope.decaySeconds,3),
            compact(preset.ampEnvelope.sustainLevel), compact(preset.ampEnvelope.releaseSeconds,3),
            compact(preset.filter.envelope.delaySeconds,3), compact(preset.filter.envelope.attackSeconds,3),
            compact(preset.filter.envelope.holdSeconds,3), compact(preset.filter.envelope.decaySeconds,3),
            compact(preset.filter.envelope.sustainLevel), compact(preset.filter.envelope.releaseSeconds,3),
            compact(preset.filter.comb.damping), compact(preset.filter.comb.mix),
            compact(preset.filter.comb.feedbackScale), compact(preset.filter.formant.dryMix)};
        std::array<bool, kSynthParameterRowCount> toggles{}; std::array<bool, kSynthParameterRowCount> active{};
        toggles[0] = true; active[0] = preset.filter.enabled; values[0] = preset.filter.enabled ? values[0] : "Filter off";
        toggles[1] = true; active[1] = preset.filter.alternateRevision; values[1] = preset.filter.alternateRevision ? "Revision B" : "Revision A";
        draw_parameter_rows(labels, values, toggles, active);
    } else if (page == SynthPanelPage::Modulation) {
        const auto& lfo1 = preset.lfos[0]; const auto& lfo2 = preset.lfos[1];
        const auto& slot = preset.modulation[controller.synth_panel().selected_modulation_slot()];
        std::array<std::string, kSynthParameterRowCount> labels{
            "LFO 1", "LFO 1 wave", "LFO 1 rate", "LFO 1 depth", "LFO 1 phase", "LFO 1 fade",
            "LFO 1 key sync", "LFO 1 tempo sync", "LFO 1 beats/cycle",
            "LFO 2", "LFO 2 wave", "LFO 2 rate", "LFO 2 depth", "LFO 2 phase", "LFO 2 fade",
            "LFO 2 key sync", "LFO 2 tempo sync", "LFO 2 beats/cycle",
            "Modulation slot", "Source", "Destination", "Amount", "Curve", "Route enabled", "Bias"};
        const auto modulationInfo = controller.synthesizer().modulation_activity()[controller.synth_panel().selected_modulation_slot()];
        std::array<std::string, kSynthParameterRowCount> values{
            bool_text(lfo1.enabled), std::string(audio::lfo_waveform_name(lfo1.waveform)), compact(lfo1.rateHertz), compact(lfo1.depth),
            compact(lfo1.phase), compact(lfo1.fadeInSeconds), bool_text(lfo1.keySync), bool_text(lfo1.tempoSync), compact(lfo1.beatsPerCycle),
            bool_text(lfo2.enabled), std::string(audio::lfo_waveform_name(lfo2.waveform)), compact(lfo2.rateHertz), compact(lfo2.depth),
            compact(lfo2.phase), compact(lfo2.fadeInSeconds), bool_text(lfo2.keySync), bool_text(lfo2.tempoSync), compact(lfo2.beatsPerCycle),
            std::to_string(controller.synth_panel().selected_modulation_slot() + 1U),
            std::string(audio::modulation_source_name(slot.source)), std::string(audio::modulation_destination_name(slot.destination)),
            compact(slot.amount) + " | live " + compact(modulationInfo.currentValue),
            std::string(curve_text(slot.curve)), bool_text(slot.enabled), compact(slot.bias)};
        std::array<bool, kSynthParameterRowCount> toggles{}; std::array<bool, kSynthParameterRowCount> active{};
        for (std::size_t i : {0U,6U,7U,9U,15U,16U,23U}) toggles[i] = true;
        active[0]=lfo1.enabled; active[6]=lfo1.keySync; active[7]=lfo1.tempoSync;
        active[9]=lfo2.enabled; active[15]=lfo2.keySync; active[16]=lfo2.tempoSync; active[23]=slot.enabled;
        draw_parameter_rows(labels, values, toggles, active);
        const UiRect activityTrack = layout.parameterRows[21].width <= 0 ? UiRect{} : UiRect{layout.parameterRows[21].x + 116, layout.parameterRows[21].y + layout.parameterRows[21].height - 3, 112, 3};
        if (activityTrack.width > 0) {
        painter.fill(activityTrack, rgb(28,35,46));
        const int activityCenter = activityTrack.x + activityTrack.width / 2;
        const int activityPixels = static_cast<int>(std::clamp(modulationInfo.currentValue, -1.0F, 1.0F) *
                                                    static_cast<float>(activityTrack.width / 2));
        painter.fill({std::min(activityCenter, activityCenter + activityPixels), activityTrack.y,
                      std::max(1, std::abs(activityPixels)), activityTrack.height}, accent);
        }
        for (std::size_t i = 0; i < layout.macroRows.size(); ++i) {
            if (layout.macroRows[i].width <= 0) continue;
            painter.fill(layout.macroRows[i], panel); painter.outline(layout.macroRows[i], border);
            painter.text(layout.macroRows[i].x + 7, layout.macroRows[i].y + 19,
                         preset.macros.names[i], text);
            button(layout.macroDownButtons[i], "-");
            painter.text(layout.macroDownButtons[i].x + 31, layout.macroDownButtons[i].y + 16,
                         compact(preset.macros.values[i]), text);
            button(layout.macroUpButtons[i], "+");
        }
    } else if (page == SynthPanelPage::Performance) {
        std::array<std::string, kSynthParameterRowCount> labels{
            "Reference tuning", "Global transpose", "Fine tune cents", "Analog drift", "Chord engine", "Chord type",
            "Chord inversion", "Chord spread", "Chord scale", "Scale root", "Strum ms", "Chord velocity",
            "Arpeggiator", "Arp direction", "Beat division", "Internal BPM", "Clock source", "External BPM",
            "Gate length", "Swing", "Octave range", "Step count", "Latch", "Retrigger envelopes",
            "Humanize timing", "Humanize velocity", "Arp scale", "Arp scale root",
            "Phrase vel start", "Phrase vel end"};
        static constexpr std::array<std::string_view, 12> pitchNames{
            "C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
        std::array<std::string, kSynthParameterRowCount> values{
            compact(preset.tuning.referenceHertz,1) + " Hz", compact(preset.tuning.transposeSemitones,0),
            compact(preset.tuning.fineCents,1), compact(preset.tuning.analogDriftCents,1), bool_text(preset.chord.enabled),
            std::string(audio::chord_type_name(preset.chord.type)), std::to_string(static_cast<int>(preset.chord.inversion)),
            std::to_string(preset.chord.spreadOctaves), std::string(scale_text(preset.chord.scale)),
            std::string(pitchNames[preset.chord.scaleRoot % 12U]), compact(preset.chord.strumMilliseconds,0),
            compact(preset.chord.velocityScale), bool_text(preset.arpeggiator.enabled),
            std::string(audio::arpeggiator_mode_name(preset.arpeggiator.mode)),
            std::string(audio::arpeggiator_division_name(preset.arpeggiator.division)), compact(preset.arpeggiator.tempoBpm,0),
            std::string(clock_text(preset.arpeggiator.clockSource)), compact(preset.arpeggiator.externalTempoBpm,0),
            compact(preset.arpeggiator.gate), compact(preset.arpeggiator.swing), std::to_string(preset.arpeggiator.octaveRange),
            std::to_string(preset.arpeggiator.stepCount), bool_text(preset.arpeggiator.latch),
            bool_text(preset.arpeggiator.retriggerEnvelopes), compact(preset.arpeggiator.humanizeTiming),
            compact(preset.arpeggiator.humanizeVelocity), std::string(scale_text(preset.arpeggiator.scale)),
            std::string(pitchNames[preset.arpeggiator.scaleRoot % 12U]),
            compact(preset.arpeggiator.phraseVelocityStart), compact(preset.arpeggiator.phraseVelocityEnd)};
        std::array<bool, kSynthParameterRowCount> toggles{}; std::array<bool, kSynthParameterRowCount> active{};
        for (std::size_t i : {4U,12U,22U,23U}) toggles[i] = true;
        active[4]=preset.chord.enabled; active[12]=preset.arpeggiator.enabled;
        active[22]=preset.arpeggiator.latch; active[23]=preset.arpeggiator.retriggerEnvelopes;
        draw_parameter_rows(labels, values, toggles, active);

        const std::size_t selectedStep = controller.synth_panel().selected_arpeggiator_step();
        for (std::size_t i = 0; i < layout.arpeggiatorStepButtons.size(); ++i) {
            if (layout.arpeggiatorStepButtons[i].width <= 0) continue;
            const auto& candidate = preset.arpeggiator.steps[i];
            const bool selected = i == selectedStep;
            const bool playing = i == meters.arpeggiatorStep;
            std::string label = std::to_string(i + 1U);
            if (!candidate.enabled) label += " -";
            else if (playing) label += " >";
            if (candidate.ratchets > 1U) label += "x" + std::to_string(candidate.ratchets);
            button(layout.arpeggiatorStepButtons[i], label, selected || playing);
        }
        const auto& step = preset.arpeggiator.steps[selectedStep];
        auto condition_text = [](audio::ArpeggiatorCondition value) -> std::string_view {
            switch (value) {
                case audio::ArpeggiatorCondition::Unconditional: return "Always";
                case audio::ArpeggiatorCondition::Every2: return "Every 2";
                case audio::ArpeggiatorCondition::Every3: return "Every 3";
                case audio::ArpeggiatorCondition::Every4: return "Every 4";
                case audio::ArpeggiatorCondition::FirstOf4: return "First / 4";
                case audio::ArpeggiatorCondition::Fill: return "Fill";
                case audio::ArpeggiatorCondition::AB: return "A:B";
            }
            return "Always";
        };
        auto automation_text = [](audio::StepAutomationCurve value) -> std::string_view {
            switch (value) {
                case audio::StepAutomationCurve::Step: return "Step";
                case audio::StepAutomationCurve::Linear: return "Linear";
                case audio::StepAutomationCurve::Smooth: return "Smooth";
            }
            return "Step";
        };
        const std::array<std::string, kSynthArpStepPropertyCount> stepLabels{
            "Enabled", "Condition", "Cond A", "Cond B", "Automation", "Accent", "Slide", "Transpose", "Octave", "Velocity",
            "Gate", "Probability", "Ratchets", "Tie", "Macro 1", "Macro 2", "Macro 3", "Macro 4"};
        const std::array<std::string, kSynthArpStepPropertyCount> stepValues{
            bool_text(step.enabled), std::string(condition_text(step.condition)),
            std::to_string(step.conditionA), std::to_string(step.conditionB),
            std::string(automation_text(step.automationCurve)),
            bool_text(step.accent), bool_text(step.slide), std::to_string(static_cast<int>(step.transpose)),
            std::to_string(static_cast<int>(step.octaveOffset)), compact(step.velocityScale), compact(step.gateScale),
            compact(step.probability), std::to_string(step.ratchets), bool_text(step.tie),
            step.macro1 < 0.0F ? "Keep" : compact(step.macro1), step.macro2 < 0.0F ? "Keep" : compact(step.macro2),
            step.macro3 < 0.0F ? "Keep" : compact(step.macro3), step.macro4 < 0.0F ? "Keep" : compact(step.macro4)};
        for (std::size_t i = 0; i < layout.arpeggiatorStepRows.size(); ++i) {
            if (layout.arpeggiatorStepRows[i].width <= 0) continue;
            painter.fill(layout.arpeggiatorStepRows[i], panel); painter.outline(layout.arpeggiatorStepRows[i], border);
            painter.text(layout.arpeggiatorStepRows[i].x + 7, layout.arpeggiatorStepRows[i].y + 18,
                         stepLabels[i], text);
            if (i == 0U || i == 5U || i == 6U || i == 13U) {
                const bool enabled = i == 0U ? step.enabled : (i == 5U ? step.accent : (i == 6U ? step.slide : step.tie));
                button(layout.arpeggiatorStepToggleButtons[i], stepValues[i], enabled);
            }
            else {
                button(layout.arpeggiatorStepDownButtons[i], "-");
                painter.text(layout.arpeggiatorStepDownButtons[i].x + 32,
                             layout.arpeggiatorStepDownButtons[i].y + 16, stepValues[i], text);
                button(layout.arpeggiatorStepUpButtons[i], "+");
            }
        }
    } else if (page == SynthPanelPage::Effects) {
        static constexpr std::array<std::string_view, 13> effectNames{
            "Distortion", "Bitcrusher", "Harmonizer", "3-band EQ", "Chorus", "Flanger",
            "Ensemble", "Phaser", "Delay", "Reverb", "Compressor", "Limiter", "Diffusion"};
        const std::array<bool, 13> effects{
            preset.distortion.enabled, preset.bitcrusher.enabled, preset.harmonizer.enabled,
            preset.eq.enabled, preset.chorus.enabled, preset.flanger.enabled,
            preset.ensemble.enabled, preset.phaser.enabled, preset.delay.enabled,
            preset.reverb.enabled, preset.compressor.enabled, preset.limiter.enabled,
            preset.diffusionDelay.enabled};
        const std::size_t selectedEffect = controller.synth_panel().selected_effect();
        for (std::size_t i = 0; i < layout.effectRows.size(); ++i) {
            if (layout.effectRows[i].width <= 0) continue;  // scrolled out of view
            painter.fill(layout.effectRows[i], panel); painter.outline(layout.effectRows[i], border);
            if (i == selectedEffect) painter.outline(layout.effectRows[i], accent);
            painter.text(layout.effectRows[i].x + 10, layout.effectRows[i].y + 20, effectNames[i], text);
            button(layout.effectToggleButtons[i], effects[i] ? "ON" : "OFF", effects[i]);
        }
        // Parameter editor for the selected effect.
        const auto paramTitle = std::string(effectNames[selectedEffect]) + " parameters";
        if (layout.effectParamTitle.width > 0)
            painter.text(layout.effectParamTitle.x + 7, layout.effectParamTitle.y + 15, paramTitle, text);
        auto effect_params = [&](std::size_t effect) -> std::vector<std::tuple<std::string, std::string, bool>> {
            // Returns (label, value, isToggle) for each parameter row.
            std::vector<std::tuple<std::string, std::string, bool>> rows;
            const auto f2 = [&](float v, int d) { return compact(v, d); };
            switch (effect) {
                case 0: {
                    auto distortion_mode_text = [](audio::DistortionMode mode) -> std::string_view {
                        switch (mode) {
                            case audio::DistortionMode::Classic: return "Classic";
                            case audio::DistortionMode::Fuzz: return "Fuzz";
                            case audio::DistortionMode::SoftClip: return "Soft Clip";
                            case audio::DistortionMode::Foldback: return "Foldback";
                        }
                        return "Classic";
                    };
                    rows.emplace_back("Drive", f2(preset.distortion.drive, 1), false);
                    rows.emplace_back("Mix", f2(preset.distortion.mix, 2), false);
                    rows.emplace_back("Mode", distortion_mode_text(preset.distortion.mode), true);
                    break;
                }
                case 1:
                    rows.emplace_back("Bits", std::to_string(preset.bitcrusher.bits), false);
                    rows.emplace_back("Downsample", std::to_string(preset.bitcrusher.downsample), false);
                    rows.emplace_back("Mix", f2(preset.bitcrusher.mix, 2), false);
                    break;
                case 2:
                    rows.emplace_back("Sub level", f2(preset.harmonizer.subLevel, 2), false);
                    rows.emplace_back("Up level", f2(preset.harmonizer.upLevel, 2), false);
                    rows.emplace_back("Mix", f2(preset.harmonizer.mix, 2), false);
                    break;
                case 3:
                    rows.emplace_back("Low dB", f2(preset.eq.lowGainDb, 1), false);
                    rows.emplace_back("Mid dB", f2(preset.eq.midGainDb, 1), false);
                    rows.emplace_back("High dB", f2(preset.eq.highGainDb, 1), false);
                    break;
                case 4:
                    rows.emplace_back("Rate Hz", f2(preset.chorus.rateHertz, 2), false);
                    rows.emplace_back("Depth ms", f2(preset.chorus.depthMilliseconds, 1), false);
                    rows.emplace_back("Mix", f2(preset.chorus.mix, 2), false);
                    break;
                case 5:
                    rows.emplace_back("Rate Hz", f2(preset.flanger.rateHertz, 2), false);
                    rows.emplace_back("Depth ms", f2(preset.flanger.depthMilliseconds, 1), false);
                    rows.emplace_back("Feedback", f2(preset.flanger.feedback, 2), false);
                    rows.emplace_back("Mix", f2(preset.flanger.mix, 2), false);
                    break;
                case 6: {
                    const char* modeName = preset.ensemble.mode == audio::EnsembleMode::I ? "I" :
                        preset.ensemble.mode == audio::EnsembleMode::II ? "II" : "I+II";
                    rows.emplace_back("Mode", modeName, true);
                    rows.emplace_back("Mix", f2(preset.ensemble.mix, 2), false);
                    break;
                }
                case 7:
                    rows.emplace_back("Rate Hz", f2(preset.phaser.rateHertz, 2), false);
                    rows.emplace_back("Depth", f2(preset.phaser.depth, 2), false);
                    rows.emplace_back("Feedback", f2(preset.phaser.feedback, 2), false);
                    rows.emplace_back("Mix", f2(preset.phaser.mix, 2), false);
                    break;
                case 8:
                    rows.emplace_back("Time s", f2(preset.delay.timeSeconds, 2), false);
                    rows.emplace_back("Feedback", f2(preset.delay.feedback, 2), false);
                    rows.emplace_back("Mix", f2(preset.delay.mix, 2), false);
                    rows.emplace_back("Ping-pong", preset.delay.pingPong ? "On" : "Off", true);
                    rows.emplace_back("Tempo sync", preset.delay.tempoSync ? "On" : "Off", true);
                    rows.emplace_back("Sync beats", f2(preset.delay.syncBeats, 2), false);
                    break;
                case 9:
                    rows.emplace_back("Room size", f2(preset.reverb.roomSize, 2), false);
                    rows.emplace_back("Damping", f2(preset.reverb.damping, 2), false);
                    rows.emplace_back("Width", f2(preset.reverb.width, 2), false);
                    rows.emplace_back("Mix", f2(preset.reverb.mix, 2), false);
                    break;
                case 10:
                    rows.emplace_back("Threshold dB", f2(preset.compressor.thresholdDb, 1), false);
                    rows.emplace_back("Ratio", f2(preset.compressor.ratio, 1), false);
                    rows.emplace_back("Attack ms", f2(preset.compressor.attackMilliseconds, 1), false);
                    rows.emplace_back("Release ms", f2(preset.compressor.releaseMilliseconds, 0), false);
                    rows.emplace_back("Makeup dB", f2(preset.compressor.makeupDb, 1), false);
                    break;
                case 11:
                    rows.emplace_back("Ceiling dB", f2(preset.limiter.ceilingDb, 1), false);
                    rows.emplace_back("Release ms", f2(preset.limiter.releaseMilliseconds, 0), false);
                    break;
                case 12:
                    rows.emplace_back("Time s", f2(preset.diffusionDelay.timeSeconds, 2), false);
                    rows.emplace_back("Feedback", f2(preset.diffusionDelay.feedback, 2), false);
                    rows.emplace_back("Mix", f2(preset.diffusionDelay.mix, 2), false);
                    rows.emplace_back("Diffusion", f2(preset.diffusionDelay.diffusion, 2), false);
                    break;
                default: break;
            }
            return rows;
        };
        const auto params = effect_params(selectedEffect);
        for (std::size_t i = 0; i < layout.effectParamRows.size(); ++i) {
            if (i >= params.size()) break;
            if (layout.effectParamRows[i].width <= 0) continue;  // scrolled out of view
            const auto& [label, value, isToggle] = params[i];
            painter.fill(layout.effectParamRows[i], panel); painter.outline(layout.effectParamRows[i], border);
            painter.text(layout.effectParamRows[i].x + 7, layout.effectParamRows[i].y + 18, label, text);
            if (isToggle) button(layout.effectParamToggleButtons[i], value, true);
            else {
                button(layout.effectParamDownButtons[i], "-");
                painter.text(layout.effectParamDownButtons[i].x + 32,
                             layout.effectParamDownButtons[i].y + 16, value, text);
                button(layout.effectParamUpButtons[i], "+");
            }
        }
    } else if (page == SynthPanelPage::Expression) {
        auto mpe_text = [](audio::MpeZoneMode value) -> std::string_view {
            switch (value) {
                case audio::MpeZoneMode::Off: return "Off";
                case audio::MpeZoneMode::Lower: return "Lower";
                case audio::MpeZoneMode::Upper: return "Upper";
                case audio::MpeZoneMode::Dual: return "Dual";
            }
            return "Off";
        };
        auto oscillator_quality_text = [](audio::OscillatorQuality value) -> std::string_view {
            switch (value) {
                case audio::OscillatorQuality::Normal: return "Normal";
                case audio::OscillatorQuality::High: return "High";
                case audio::OscillatorQuality::Offline: return "Offline";
            }
            return "Normal";
        };
        auto filter_quality_text = [](audio::FilterQuality value) -> std::string_view {
            switch (value) {
                case audio::FilterQuality::Eco: return "Eco";
                case audio::FilterQuality::Standard: return "Standard";
                case audio::FilterQuality::High: return "High";
                case audio::FilterQuality::Offline: return "Offline";
            }
            return "Standard";
        };
        const auto& route = preset.modulation[controller.synth_panel().selected_modulation_slot()];
        const std::array<std::string, kSynthParameterRowCount> labels{
            "MPE zone", "Lower master channel", "Lower member channels", "Upper master channel",
            "Upper member channels", "Master bend range", "Member bend range", "Timbre controller",
            "Master sustain to members", "Microtuning", "Tuning reference Hz", "Reference MIDI note",
            "Unison", "Unison voices", "Unison detune cents", "Unison stereo spread",
            "Unison phase spread", "Preserve unison level", "Oscillator quality", "Filter quality",
            "Modulation slot", "Modulation polarity", "Route smoothing ms", "Favorite preset"};
        const std::array<std::string, kSynthParameterRowCount> values{
            std::string(mpe_text(preset.mpe.zoneMode)), std::to_string(preset.mpe.lowerMasterChannel + 1U),
            std::to_string(preset.mpe.lowerMemberCount), std::to_string(preset.mpe.upperMasterChannel + 1U),
            std::to_string(preset.mpe.upperMemberCount), compact(preset.mpe.masterPitchBendRangeSemitones,1),
            compact(preset.mpe.memberPitchBendRangeSemitones,1), "CC " + std::to_string(preset.mpe.timbreController),
            bool_text(preset.mpe.masterSustainToMembers), bool_text(preset.microtuning.enabled) + " " + preset.microtuning.name,
            compact(preset.microtuning.referenceHertz,1), std::to_string(preset.microtuning.referenceNote),
            bool_text(preset.unison.enabled), std::to_string(preset.unison.voices), compact(preset.unison.detuneCents,1),
            compact(preset.unison.stereoSpread), compact(preset.unison.phaseSpread), bool_text(preset.unison.preserveLevel),
            std::string(oscillator_quality_text(preset.oscillatorQuality)), std::string(filter_quality_text(preset.filterQuality)),
            std::to_string(controller.synth_panel().selected_modulation_slot() + 1U),
            route.polarity == audio::ModulationPolarity::Bipolar ? "Bipolar" : "Unipolar",
            compact(route.smoothingMilliseconds,1), bool_text(preset.metadata.favorite)};
        std::array<bool, kSynthParameterRowCount> toggles{};
        std::array<bool, kSynthParameterRowCount> active{};
        for (std::size_t i : {8U,9U,12U,17U,23U}) toggles[i] = true;
        active[8]=preset.mpe.masterSustainToMembers; active[9]=preset.microtuning.enabled;
        active[12]=preset.unison.enabled; active[17]=preset.unison.preserveLevel;
        active[23]=preset.metadata.favorite;
        draw_parameter_rows(labels, values, toggles, active);
    } else if (page == SynthPanelPage::Generative) {
        // Phase 3: generative sequencer / patch genetics / attractor conductor.
        static constexpr std::array<std::string_view, 7> laneNames{
            "Pitch", "Velocity", "Gate", "Timbre", "Probability", "Morph", "Pan"};
        static constexpr std::array<std::string_view, 6> seqScaleNames{
            "Chromatic", "Major", "Minor", "Pent major", "Pent minor", "Dorian"};
        static constexpr std::array<std::string_view, 4> directionNames{
            "Forward", "Reverse", "Ping-pong", "Random"};
        static constexpr std::array<std::string_view, 12> pitchNames{
            "C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
        const auto& seq = preset.sequencer;
        const std::size_t selectedLane = controller.synth_panel().selected_sequencer_lane();
        std::array<std::string, kSynthParameterRowCount> labels{
            "Sequencer",
            "Pitch steps", "Velocity steps", "Gate steps", "Timbre steps",
            "Probability steps", "Morph steps", "Pan steps",
            "Edit lane", "Lane direction", "Scale", "Scale root", "Octave range", "Seq seed",
            "Mutation intensity", "Mutation seed", "Mutate now", "Breed A x B",
            "Lock oscillators", "Lock spectral", "Lock filters", "Lock envelopes",
            "Lock modulation", "Lock stereo", "Lock sequencer", "Lock FX",
            "Attractor", "Attractor BPM", "", ""};
        std::array<std::string, kSynthParameterRowCount> values{
            bool_text(seq.enabled),
            std::to_string(seq.lanes[0].stepCount), std::to_string(seq.lanes[1].stepCount),
            std::to_string(seq.lanes[2].stepCount), std::to_string(seq.lanes[3].stepCount),
            std::to_string(seq.lanes[4].stepCount), std::to_string(seq.lanes[5].stepCount),
            std::to_string(seq.lanes[6].stepCount),
            std::string(laneNames[selectedLane]),
            std::string(directionNames[static_cast<std::size_t>(seq.lanes[selectedLane].direction)]),
            std::string(seqScaleNames[static_cast<std::size_t>(seq.scale)]),
            std::string(pitchNames[seq.rootNote % 12U]) + std::to_string(static_cast<int>(seq.rootNote / 12U) - 1),
            std::to_string(seq.octaveRange), std::to_string(seq.randomSeed),
            compact(preset.genetics.mutationIntensity), std::to_string(preset.genetics.mutationSeed),
            "Go", "Go", "", "", "", "", "", "", "", "",
            bool_text(preset.attractor.enabled), compact(static_cast<float>(preset.attractor.config.bpm), 0),
            "", ""};
        for (std::size_t g = 0; g < 8U; ++g)
            values[18U + g] = (preset.genetics.lockedGroups & (1U << g)) != 0U ? "Locked" : "Free";
        std::array<bool, kSynthParameterRowCount> toggles{}; std::array<bool, kSynthParameterRowCount> active{};
        for (std::size_t i : {0U,16U,17U,18U,19U,20U,21U,22U,23U,24U,25U,26U}) toggles[i] = true;
        active[0] = seq.enabled;
        for (std::size_t g = 0; g < 8U; ++g)
            active[18U + g] = (preset.genetics.lockedGroups & (1U << g)) != 0U;
        active[26] = preset.attractor.enabled;
        draw_parameter_rows(labels, values, toggles, active);
    } else {
        if (layout.presetScanButton.width > 0) {
            button(layout.presetScanButton, "Scan library");
            button(layout.presetPreviousButton, "<"); button(layout.presetNextButton, ">");
            button(layout.presetLoadButton, "Load");
        }
        if (layout.presetCaptureAButton.width > 0) {
            button(layout.presetCaptureAButton, "Capture A");
            button(layout.presetCaptureBButton, "Capture B"); button(layout.presetMorphDownButton, "-");
            painter.text(layout.presetMorphDownButton.x + 43, layout.presetMorphDownButton.y + 18,
                         "Morph " + compact(controller.synth_panel().preset_morph_amount()), text);
            button(layout.presetMorphUpButton, "+");
        }
        const auto& entries = controller.synth_panel().preset_library().entries();
        for (std::size_t i = 0; i < layout.presetEntryButtons.size(); ++i) {
            if (layout.presetEntryButtons[i].width <= 0) continue;  // scrolled out of view
            const std::string label = i < entries.size() ? entries[i].name : "--";
            button(layout.presetEntryButtons[i], label,
                   i < entries.size() && i == controller.synth_panel().selected_preset_entry());
        }
        if (layout.presetStatusRow.width > 0)
            painter.text(layout.presetStatusRow.x + 2, layout.presetStatusRow.y + 14,
                         std::string(controller.synth_panel().preset_status()), muted);
        const auto& mapping = preset.midiLearn[controller.synth_panel().selected_midi_learn_mapping()];
        std::array<std::string, kSynthParameterRowCount> labels{};
        std::array<std::string, kSynthParameterRowCount> values{};
        labels[0]="MIDI learn slot"; values[0]=std::to_string(controller.synth_panel().selected_midi_learn_mapping()+1U);
        labels[1]="Mapping enabled"; values[1]=bool_text(mapping.enabled);
        labels[2]="Controller CC"; values[2]=std::to_string(mapping.controller);
        labels[3]="Target macro"; values[3]=std::to_string(mapping.macroIndex+1U);
        labels[4]="Minimum"; values[4]=compact(mapping.minimum);
        labels[5]="Maximum"; values[5]=compact(mapping.maximum);
        labels[6]="Inverted"; values[6]=bool_text(mapping.inverted);
        std::array<bool, kSynthParameterRowCount> toggles{}; std::array<bool, kSynthParameterRowCount> active{};
        toggles[1]=true; active[1]=mapping.enabled; toggles[6]=true; active[6]=mapping.inverted;
        for (std::size_t i = 0; i < 7U; ++i) {
            if (layout.parameterRows[i].width <= 0) continue;  // scrolled out of view
            painter.fill(layout.parameterRows[i], panel); painter.outline(layout.parameterRows[i], border);
            painter.text(layout.parameterRows[i].x + 7, layout.parameterRows[i].y + 18, labels[i], text);
            if (toggles[i]) button(layout.parameterToggleButtons[i], values[i], active[i]);
            else {
                button(layout.parameterDownButtons[i], "-");
                painter.text(layout.parameterDownButtons[i].x + 32, layout.parameterDownButtons[i].y + 16, values[i], text);
                button(layout.parameterUpButtons[i], "+");
            }
        }
    }

    const int meterX = layout.meterArea.x + 2;
    const int meterY = layout.meterArea.y + 13;  // text baseline (== panel bottom - 151)
    const int meterWidth = std::max(120, layout.meterArea.width - 4);
    painter.text(meterX, meterY, "Voices " + std::to_string(meters.activeVoices) + "/16", text);
    painter.fill({meterX, meterY + 8, meterWidth, 12}, rgb(20,25,31));
    painter.fill({meterX, meterY + 8, static_cast<int>(static_cast<float>(meterWidth) * std::min(1.0F, meters.peakLeft)), 5}, rgb(72,210,130));
    painter.fill({meterX, meterY + 15, static_cast<int>(static_cast<float>(meterWidth) * std::min(1.0F, meters.peakRight)), 5}, rgb(72,160,230));

    std::array<bool,128> activeNotes{};
    for (const auto& voice : voices) if (voice.active && voice.note < 128U) activeNotes[voice.note] = true;
    draw_piano_keyboard(painter, controller.synth_panel().keyboard(), activeNotes, accent, text);
    {
        const PianoKeyboard& keyboard = controller.synth_panel().keyboard();
        button(layout.keyboardKeysButton, "Keys " + std::to_string(keyboard.key_count()) + " >");
    }
    painter.text(layout.panel.x + 12, layout.panel.y + layout.panel.height - 9,
                 "Keys Z-M/Q-W | arrows octave | 1/3/4/5/6/7 pages | wheel scrolls | Ctrl+4 toggles", muted);
    if (layout.gridScrollTrack.width > 0) {
        painter.fill(layout.gridScrollTrack, rgb(20,25,31));
        painter.fill(layout.gridScrollThumb, rgb(96,110,130));
    }

    // Phase 6: patch-search browser overlay.
    {
        const auto& searchPanel = controller.synth_panel().search_panel();
        if (searchPanel.open()) {
            const auto& sl = searchPanel.layout();
            const auto& session = searchPanel.session();
            painter.fill(sl.panel, rgb(10, 14, 22)); painter.outline(sl.panel, accent);
            painter.fill(sl.titleBar, rgb(27, 44, 67));
            painter.text(sl.titleBar.x + 12, sl.titleBar.y + 23, "Patch Search - audio-to-synth", text);
            button(sl.closeButton, "X");
            painter.fill(sl.targetNameField, panel2); painter.outline(sl.targetNameField, border);
            painter.text(sl.targetNameField.x + 7, sl.targetNameField.y + 18,
                         session.targetName.empty() ? "(no target)" : session.targetName, text);
            const std::size_t first = searchPanel.visible_first();
            for (std::size_t r = 0; r < searchPanel.visible_count(); ++r) {
                const auto& cand = session.candidates[first + r];
                const bool selected = (first + r) == searchPanel.selected_candidate();
                painter.fill(sl.candidateRows[r], selected ? rgb(39, 58, 82) : panel);
                painter.outline(sl.candidateRows[r], selected ? accent : border);
                char dist[32];
                std::snprintf(dist, sizeof(dist), "%.4f", cand.distance);
                painter.text(sl.candidateRows[r].x + 7, sl.candidateRows[r].y + 18,
                             cand.label + "  (" + dist + ")", selected ? rgb(255, 255, 255) : text);
            }
            button(sl.pagePrevButton, "<"); button(sl.pageNextButton, ">");
            painter.text(sl.pageLabel.x + 4, sl.pageLabel.y + 17, searchPanel.page_text(), muted);
            button(sl.auditionButton, "AUDITION"); button(sl.promoteButton, "PROMOTE");
            painter.text(sl.statusField.x + 7, sl.statusField.y + 18, std::string(searchPanel.status()), muted);
        }
    }
    painter.draw_tooltip(rgb(16,20,28), accent, text);
}


void register_chiptune_text_cells(CellFitCanvas& canvas, const EditorChiptunePanel& editor) {
    const auto& layout = editor.layout();
    const auto& session = editor.session();
    if (editor.page() == ChiptunePanelPage::Sfx) canvas.add_cell(layout.pianoArea);
    for (const UiRect& rect : {layout.closeButton, layout.playSongButton, layout.playInstrumentButton, layout.stopButton,
                               layout.saveButton, layout.openButton, layout.undoButton, layout.redoButton,
                               layout.copyButton, layout.cutButton, layout.pasteButton, layout.songBusButton,
                               layout.instrumentBusButton})
        canvas.add_cell(rect);
    for (const UiRect& tab : layout.tabs) canvas.add_cell(tab);
    const int panelRight = layout.panel.x + layout.panel.width - 12;
    auto value_slot = [&](UiRect down, UiRect up) {
        canvas.add_cell(down); canvas.add_cell(up);
        canvas.add_cell({down.x + down.width, down.y, up.x - down.x - down.width, down.height});
    };
    if (editor.page() == ChiptunePanelPage::Pattern) {
        for (const UiRect& rect : layout.orderRows) canvas.add_cell(rect);
        for (const UiRect& rect : {layout.addOrderButton, layout.deleteOrderButton, layout.duplicatePatternButton,
                                   layout.addPatternButton, layout.deletePatternButton, layout.effectPreviousButton,
                                   layout.effectNextButton, layout.effectParamDownButton, layout.effectParamUpButton})
            canvas.add_cell(rect);
        const auto cursor = session.cursor();
        for (std::size_t row = 0; row < kChiptuneVisibleRows; ++row)
            for (std::size_t channel = 0; channel < audio::kChiptuneMaxChannels; ++channel) {
                const bool focused = editor.first_visible_row() + row == cursor.row && channel == cursor.channel;
                canvas.add_cell(layout.cells[row][channel], focused);
            }
        const UiRect up = layout.effectParamUpButton;
        canvas.add_cell({up.x + up.width, up.y, layout.patternGrid.x + layout.patternGrid.width - up.x - up.width, up.height});
        canvas.add_cell(layout.orderList);
    } else if (editor.page() == ChiptunePanelPage::Instrument) {
        for (const UiRect& rect : {layout.instrumentPreviousButton, layout.instrumentNextButton,
                                   layout.addInstrumentButton, layout.deleteInstrumentButton,
                                   layout.wavePreviousButton, layout.waveNextButton,
                                   layout.normalizeWavetableButton, layout.removeDcButton})
            canvas.add_cell(rect);
        for (const UiRect& rect : layout.wavetableShapeButtons) canvas.add_cell(rect);
        const UiRect next = layout.waveNextButton;
        canvas.add_cell({next.x + next.width, next.y, panelRight - next.x - next.width, next.height});
        canvas.add_cell(layout.envelopeCanvas);
        canvas.add_cell(layout.wavetableCanvas);
    } else {
        for (const UiRect& rect : layout.sfxPresetButtons) canvas.add_cell(rect);
        value_slot(layout.sfxBaseDownButton, layout.sfxBaseUpButton);
        value_slot(layout.sfxDurationDownButton, layout.sfxDurationUpButton);
        value_slot(layout.sfxGainDownButton, layout.sfxGainUpButton);
        value_slot(layout.sfxPanDownButton, layout.sfxPanUpButton);
        canvas.add_cell(layout.applySfxButton);
        canvas.add_cell(layout.auditionSfxButton);
        canvas.add_cell(layout.keyboardKeysButton);
        canvas.add_cell(layout.pianoArea);
    }
    canvas.add_cell(layout.titleBar);
}

void render_chiptune_panel(const IEditorCanvas& outerPainter, NativeEditorController& controller,
                           EditorColor panel, EditorColor panel2, EditorColor border,
                           EditorColor text, EditorColor muted, EditorColor accent) {
    if (!controller.chiptune_panel().open()) return;
    const auto& editor = controller.chiptune_panel();
    const auto& layout = editor.layout();
    const auto& session = editor.session();
    const auto& song = session.song();
    CellFitCanvas painter(outerPainter, layout.panel, editor_screen_rect(controller),
                          controller.hover_x(), controller.hover_y());
    register_chiptune_text_cells(painter, editor);
    painter.fill(layout.panel, rgb(10,15,23));
    painter.outline(layout.panel, accent);
    painter.fill(layout.titleBar, rgb(27,44,67));
    painter.text(layout.titleBar.x + 12, layout.titleBar.y + 23,
                 "DVE Chiptune Tracker - " + song.name, text);
    painter.fill(layout.closeButton, rgb(100,42,48));
    painter.outline(layout.closeButton, border);
    painter.text(layout.closeButton.x + 7, layout.closeButton.y + 17, "X", text);
    auto button = [&](UiRect rect, std::string_view label, bool active = false, bool enabled = true) {
        painter.fill(rect, enabled ? (active ? accent : panel2) : rgb(24,27,32));
        painter.outline(rect, enabled ? border : rgb(48,52,60));
        painter.text(rect.x + 5, rect.y + 17, label, enabled ? (active ? rgb(255,255,255) : text) : muted);
    };
    static constexpr std::array<std::string_view,3> tabs{"PATTERN","INSTRUMENT","SFX"};
    for (std::size_t i=0;i<tabs.size();++i)
        button(layout.tabs[i], tabs[i], static_cast<std::size_t>(editor.page())==i);
    button(layout.playSongButton,"PLAY");
    button(layout.playInstrumentButton,"AUDITION");
    button(layout.stopButton,"STOP");
    button(layout.saveButton,"SAVE");
    button(layout.openButton,"OPEN");
    button(layout.undoButton,"UNDO",false,session.can_undo());
    button(layout.redoButton,"REDO",false,session.can_redo());
    button(layout.copyButton,"COPY");
    button(layout.cutButton,"CUT");
    button(layout.pasteButton,"PASTE",false,!session.clipboard().empty());
    button(layout.songBusButton, std::string("Song -> ") + std::string(audio::audio_bus_name(session.song_bus())));
    button(layout.instrumentBusButton, std::string("Preview -> ") + std::string(audio::audio_bus_name(session.instrument_bus())));

    if (editor.page()==ChiptunePanelPage::Pattern) {
        painter.fill(layout.orderList,panel); painter.outline(layout.orderList,border);
        painter.text(layout.orderList.x+7,layout.orderList.y+18,"ORDER",text);
        for(std::size_t i=0;i<layout.orderRows.size();++i){
            const std::uint32_t order=editor.first_visible_order()+static_cast<std::uint32_t>(i);
            if(order>=song.order.size()) continue;
            const bool selected=order==session.cursor().order;
            painter.fill(layout.orderRows[i],selected?rgb(42,86,132):panel2);
            painter.outline(layout.orderRows[i],selected?accent:border);
            painter.text(layout.orderRows[i].x+7,layout.orderRows[i].y+17,
                         std::to_string(order)+" : P"+std::to_string(song.order[order]),selected?rgb(255,255,255):text);
        }
        button(layout.addOrderButton,"+ORD"); button(layout.deleteOrderButton,"-ORD");
        button(layout.duplicatePatternButton,"DUP"); button(layout.addPatternButton,"+PAT");
        button(layout.deletePatternButton,"-PAT");
        const std::uint32_t patternIndex = song.order.empty()?0U:song.order[session.cursor().order];
        if(patternIndex<song.patterns.size()){
            const auto& pattern=song.patterns[patternIndex];
            for(std::size_t row=0;row<kChiptuneVisibleRows;++row){
                const std::uint32_t sourceRow=editor.first_visible_row()+static_cast<std::uint32_t>(row);
                if(sourceRow>=pattern.rowCount) continue;
                for(std::size_t channel=0;channel<audio::kChiptuneMaxChannels;++channel){
                    if(channel>=song.channelCount) continue;
                    const auto rect=layout.cells[row][channel];
                    const bool cursorSelected=sourceRow==session.cursor().row&&channel==session.cursor().channel;
                    bool rangeSelected=false;
                    if(session.selection()&&session.selection()->anchor.order==session.cursor().order){
                        const auto& selection=*session.selection();
                        rangeSelected=sourceRow>=selection.first_row()&&sourceRow<=selection.last_row()&&
                                      channel>=selection.first_channel()&&channel<=selection.last_channel();
                    }
                    painter.fill(rect,cursorSelected?rgb(49,98,145):(rangeSelected?rgb(36,65,91):(sourceRow%4U==0U?rgb(24,32,43):panel)));
                    painter.outline(rect,cursorSelected?accent:(rangeSelected?rgb(76,126,170):border));
                    const auto& cell=pattern.at(sourceRow,static_cast<std::uint32_t>(channel),song.channelCount);
                    painter.text(rect.x+4,rect.y+16,chip_cell_display(cell),cursorSelected?rgb(255,255,255):text);
                }
            }
        }
        const auto cursor=session.cursor();
        const auto& cell=song.patterns[song.order[cursor.order]].at(cursor.row,cursor.channel,song.channelCount);
        button(layout.effectPreviousButton,"<FX"); button(layout.effectNextButton,"FX>");
        button(layout.effectParamDownButton,"-P"); button(layout.effectParamUpButton,"+P");
        painter.text(layout.effectParamUpButton.x+38,layout.effectParamUpButton.y+17,
                     std::string(audio::chip_effect_name(cell.effect))+" "+std::to_string(cell.effectParam),text);
    } else if (editor.page()==ChiptunePanelPage::Instrument) {
        const auto instrumentIndex=session.selected_instrument();
        const auto& instrument=song.instruments[instrumentIndex];
        button(layout.instrumentPreviousButton,"<"); button(layout.instrumentNextButton,">");
        button(layout.addInstrumentButton,"+ INST"); button(layout.deleteInstrumentButton,"- INST",false,song.instruments.size()>1U);
        button(layout.wavePreviousButton,"<"); button(layout.waveNextButton,">");
        painter.text(layout.waveNextButton.x+38,layout.waveNextButton.y+18,
                     "I"+std::to_string(instrumentIndex+1U)+" "+instrument.name+" | "+std::string(audio::chip_wave_name(instrument.wave)),text);
        painter.fill(layout.envelopeCanvas,panel); painter.outline(layout.envelopeCanvas,border);
        painter.text(layout.envelopeCanvas.x+7,layout.envelopeCanvas.y+18,"VOLUME ENVELOPE - drag to draw",text);
        if(!instrument.volume.values.empty()){
            const int count=static_cast<int>(instrument.volume.values.size());
            for(int i=0;i<count;++i){
                const int px=layout.envelopeCanvas.x+static_cast<int>((static_cast<float>(i)+0.5F)*static_cast<float>(layout.envelopeCanvas.width)/static_cast<float>(count));
                const int ph=static_cast<int>(instrument.volume.values[static_cast<std::size_t>(i)]*static_cast<float>(layout.envelopeCanvas.height-28));
                painter.fill({px-2,layout.envelopeCanvas.y+layout.envelopeCanvas.height-ph-3,5,5},accent);
            }
        }
        painter.fill(layout.wavetableCanvas,panel); painter.outline(layout.wavetableCanvas,border);
        painter.text(layout.wavetableCanvas.x+7,layout.wavetableCanvas.y+18,"WAVETABLE - drag to draw",text);
        if(!instrument.wavetable.empty()){
            const int count=static_cast<int>(instrument.wavetable.size());
            for(int i=0;i<count;++i){
                const float value=instrument.wavetable[static_cast<std::size_t>(i)];
                const int px=layout.wavetableCanvas.x+static_cast<int>((static_cast<float>(i)+0.5F)*static_cast<float>(layout.wavetableCanvas.width)/static_cast<float>(count));
                const int py=layout.wavetableCanvas.y+layout.wavetableCanvas.height/2-static_cast<int>(value*static_cast<float>(layout.wavetableCanvas.height/2-18));
                painter.fill({px-2,py-2,5,5},accent);
            }
        }
        button(layout.normalizeWavetableButton,"NORMALIZE"); button(layout.removeDcButton,"REMOVE DC");
        static constexpr std::array<std::string_view,5> shapes{"SINE","TRIANGLE","SAW","PULSE","SILENCE"};
        for(std::size_t i=0;i<shapes.size();++i) button(layout.wavetableShapeButtons[i],shapes[i]);
    } else {
        static constexpr std::array<std::string_view,8> names{"COIN","JUMP","LASER","EXPLOSION","HIT","POWER UP","UI OK","UI CANCEL"};
        const auto request=session.sfx_request();
        for(std::size_t i=0;i<names.size();++i) button(layout.sfxPresetButtons[i],names[i],static_cast<std::size_t>(request.preset)==i);
        button(layout.sfxBaseDownButton,"-"); button(layout.sfxBaseUpButton,"+");
        painter.text(layout.sfxBaseDownButton.x+38,layout.sfxBaseDownButton.y+17,"Note "+std::to_string(request.baseMidi),text);
        button(layout.sfxDurationDownButton,"-"); button(layout.sfxDurationUpButton,"+");
        painter.text(layout.sfxDurationDownButton.x+38,layout.sfxDurationDownButton.y+17,"Time "+std::to_string(request.durationSeconds).substr(0,4),text);
        button(layout.sfxGainDownButton,"-"); button(layout.sfxGainUpButton,"+");
        painter.text(layout.sfxGainDownButton.x+38,layout.sfxGainDownButton.y+17,"Gain "+std::to_string(request.gain).substr(0,4),text);
        button(layout.sfxPanDownButton,"-"); button(layout.sfxPanUpButton,"+");
        painter.text(layout.sfxPanDownButton.x+38,layout.sfxPanDownButton.y+17,"Pan "+std::to_string(request.pan).substr(0,4),text);
        button(layout.applySfxButton,"APPLY TO SONG"); button(layout.auditionSfxButton,"AUDITION SFX");
        button(layout.keyboardKeysButton, "Keys " + std::to_string(editor.keyboard().key_count()) + " >");
        std::array<bool,128> activeNotes{};
        if (request.baseMidi >= 0 && request.baseMidi < 128) activeNotes[static_cast<std::size_t>(request.baseMidi)] = true;
        draw_piano_keyboard(painter, editor.keyboard(), activeNotes, accent, text);
    }
    painter.text(layout.panel.x+12,layout.panel.y+layout.panel.height-12,
                 std::string(editor.status())+" | F1/F2/F3 pages | [] octave | Ctrl+9 toggles",muted);
    painter.draw_tooltip(rgb(16,20,28), accent, text);
}

void render_audio_panel(const IEditorCanvas& painter, NativeEditorController& controller,
                        EditorColor panel, EditorColor panel2, EditorColor border,
                        EditorColor text, EditorColor muted, EditorColor accent) {
    if (!controller.audio_panel().open()) return;
    const AudioPanelLayout& layout = controller.audio_panel().layout();
    const audio::AudioMixerMeters meters = controller.audio_mixer().meters();
    painter.fill(layout.panel, rgb(12,17,26));
    painter.outline(layout.panel, accent);
    painter.fill(layout.titleBar, rgb(27,44,67));
    painter.text(layout.titleBar.x + 12, layout.titleBar.y + 23,
                 "DVE Audio Mixer - buses, virtualization, and spatial runtime", text);
    painter.fill(layout.closeButton, rgb(100,42,48));
    painter.outline(layout.closeButton, border);
    painter.text(layout.closeButton.x + 7, layout.closeButton.y + 17, "X", text);
    auto button = [&](UiRect rect, std::string_view label, bool active = false) {
        painter.fill(rect, active ? accent : panel2);
        painter.outline(rect, border);
        painter.text(rect.x + 6, rect.y + 17, label, active ? rgb(255,255,255) : text);
    };
    button(layout.panicButton, "STOP ALL");
    button(layout.testEventButton, "TEST EVENT");
    for (std::size_t i = 0; i < audio::kAudioBusCount; ++i) {
        const auto bus = static_cast<audio::AudioBusId>(i);
        const audio::AudioBusParameters parameters = controller.audio_mixer().bus_parameters(bus);
        const audio::AudioBusMeter& meter = meters.buses[i];
        painter.fill(layout.busRows[i], panel);
        painter.outline(layout.busRows[i], border);
        painter.text(layout.busRows[i].x + 8, layout.busRows[i].y + 20,
                     audio::audio_bus_name(bus), text);
        const int meterX = layout.busRows[i].x + 92;
        const int meterWidth = std::max(60, layout.muteButtons[i].x - meterX - 8);
        painter.fill({meterX, layout.busRows[i].y + 6, meterWidth, 8}, rgb(20,25,31));
        painter.fill({meterX, layout.busRows[i].y + 6,
                      static_cast<int>(static_cast<float>(meterWidth) * std::min(1.0F, meter.peakLeft)), 4},
                     rgb(72,210,130));
        painter.fill({meterX, layout.busRows[i].y + 11,
                      static_cast<int>(static_cast<float>(meterWidth) * std::min(1.0F, meter.peakRight)), 4},
                     rgb(72,160,230));
        button(layout.muteButtons[i], parameters.mute ? "MUTE" : "LIVE", parameters.mute);
        button(layout.gainDownButtons[i], "-");
        painter.text(layout.gainDownButtons[i].x + 34, layout.gainDownButtons[i].y + 17,
                     std::to_string(parameters.gain).substr(0,4), text);
        button(layout.gainUpButtons[i], "+");
    }
    painter.text(layout.panel.x + 12, layout.panel.y + layout.panel.height - 48,
                 "Sample voices " + std::to_string(meters.logicalSampleVoices) +
                 " | mixed " + std::to_string(meters.physicalSampleVoices) +
                 " | virtual " + std::to_string(meters.virtualSampleVoices) +
                 " | synth " + std::to_string(meters.synthVoices), text);
    painter.text(layout.panel.x + 12, layout.panel.y + layout.panel.height - 24,
                 "Ctrl+5 toggles | fixed-capacity callback | dialogue ducking | shared reverb", muted);
}


void render_audio_event_panel(const IEditorCanvas& painter, NativeEditorController& controller,
                              EditorColor panel, EditorColor panel2, EditorColor border,
                              EditorColor text, EditorColor muted, EditorColor accent) {
    if (!controller.audio_event_panel().open()) return;
    const auto& eventPanel = controller.audio_event_panel();
    const auto& layout = eventPanel.layout();
    const auto& asset = eventPanel.asset();
    const auto& report = eventPanel.report();
    painter.fill(layout.panel, rgb(10,15,24));
    painter.outline(layout.panel, accent);
    painter.fill(layout.titleBar, rgb(27,44,67));
    painter.text(layout.titleBar.x + 12, layout.titleBar.y + 23,
                 "DVE Audio Event Graph - " + asset.graph.name, text);
    painter.fill(layout.closeButton, rgb(100,42,48));
    painter.outline(layout.closeButton, border);
    painter.text(layout.closeButton.x + 7, layout.closeButton.y + 17, "X", text);
    auto button = [&](UiRect rect, std::string_view label, bool enabled = true) {
        painter.fill(rect, enabled ? panel2 : rgb(24,27,32));
        painter.outline(rect, enabled ? border : rgb(48,52,60));
        painter.text(rect.x + 6, rect.y + 18, label, enabled ? text : muted);
    };
    button(layout.compileButton, "COMPILE");
    button(layout.auditionButton, "AUDITION", report.succeeded());
    button(layout.addNoteButton, "ADD NOTE");
    button(layout.saveButton, "SAVE");
    button(layout.openButton, "OPEN");
    button(layout.undoButton, "UNDO", eventPanel.can_undo());
    button(layout.redoButton, "REDO", eventPanel.can_redo());
    button(layout.copyButton, "COPY", !asset.graph.nodes.empty());
    button(layout.pasteButton, "PASTE");
    button(layout.deleteButton, "DELETE", eventPanel.selected_node() != asset.graph.root);
    button(layout.reseedButton, "RESEED");

    painter.fill(layout.palette, panel);
    painter.outline(layout.palette, border);
    painter.text(layout.palette.x + 8, layout.palette.y + 20, "NODE PALETTE", text);
    painter.fill(layout.paletteSearch, eventPanel.search_active() ? rgb(36,70,105) : panel2);
    painter.outline(layout.paletteSearch, eventPanel.search_active() ? accent : border);
    const std::string searchText = eventPanel.palette_filter().empty()
        ? std::string("Search nodes...")
        : std::string(eventPanel.palette_filter()) + (eventPanel.search_active() ? "_" : "");
    painter.text(layout.paletteSearch.x + 6, layout.paletteSearch.y + 18, searchText,
                 eventPanel.palette_filter().empty() ? muted : text);
    static constexpr std::array<audio::AudioEventNodeType, kAudioEventPaletteEntries> paletteTypes{
        audio::AudioEventNodeType::Sample, audio::AudioEventNodeType::Stream,
        audio::AudioEventNodeType::SynthNote, audio::AudioEventNodeType::Layer,
        audio::AudioEventNodeType::RandomNoRepeat, audio::AudioEventNodeType::Sequence,
        audio::AudioEventNodeType::Delay, audio::AudioEventNodeType::Gain,
        audio::AudioEventNodeType::Bus, audio::AudioEventNodeType::Switch,
        audio::AudioEventNodeType::Scatter, audio::AudioEventNodeType::Cooldown,
        audio::AudioEventNodeType::Blend, audio::AudioEventNodeType::Loop};
    std::size_t visible{};
    std::string filter(eventPanel.palette_filter());
    std::transform(filter.begin(), filter.end(), filter.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    for (const auto type : paletteTypes) {
        std::string label(audio::audio_event_node_type_name(type));
        std::string lower = label;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (!filter.empty() && lower.find(filter) == std::string::npos) continue;
        if (visible >= layout.paletteRowCount) break;
        painter.fill(layout.paletteRows[visible], panel2);
        painter.outline(layout.paletteRows[visible], border);
        painter.text(layout.paletteRows[visible].x + 7, layout.paletteRows[visible].y + 18, label, text);
        ++visible;
    }

    painter.fill(layout.canvas, rgb(15,20,29));
    painter.outline(layout.canvas, border);
    painter.fill(layout.inspector, panel);
    painter.outline(layout.inspector, border);

    for (std::size_t i = 0; i < layout.nodeCount; ++i) {
        const auto& node = asset.graph.nodes[i];
        const auto& from = layout.nodes[i];
        for (const auto child : node.children) {
            if (child >= layout.nodeCount) continue;
            const auto& to = layout.nodes[child];
            const int midX = (from.x + from.width + to.x) / 2;
            painter.line(from.x + from.width, from.y + from.height / 2, midX,
                         from.y + from.height / 2, muted, 2);
            painter.line(midX, from.y + from.height / 2, midX,
                         to.y + to.height / 2, muted, 2);
            painter.line(midX, to.y + to.height / 2, to.x,
                         to.y + to.height / 2, muted, 2);
        }
    }
    if (eventPanel.connecting() && eventPanel.connection_source() &&
        *eventPanel.connection_source() < layout.nodeCount) {
        const auto& port = layout.outputPorts[*eventPanel.connection_source()];
        painter.line(port.x + port.width / 2, port.y + port.height / 2,
                     eventPanel.pointer_x(), eventPanel.pointer_y(), accent, 2);
    }
    for (std::size_t i = 0; i < layout.nodeCount; ++i) {
        const auto& node = asset.graph.nodes[i];
        const auto rect = layout.nodes[i];
        const bool selected = i == eventPanel.selected_node();
        painter.fill(rect, selected ? rgb(40,95,145) : panel2);
        painter.outline(rect, selected ? accent : border);
        painter.fill(layout.inputPorts[i], rgb(90,175,235));
        painter.outline(layout.inputPorts[i], border);
        painter.fill(layout.outputPorts[i], rgb(245,170,72));
        painter.outline(layout.outputPorts[i], border);
        painter.text(rect.x + 9, rect.y + 19,
                     std::to_string(i) + "  " + std::string(audio::audio_event_node_type_name(node.type)), text);
        painter.text(rect.x + 9, rect.y + 41,
                     node.type == audio::AudioEventNodeType::SynthNote
                         ? "note " + std::to_string(node.note)
                         : std::to_string(node.children.size()) + " outgoing links", muted);
    }

    int y = layout.inspector.y + 24;
    painter.text(layout.inspector.x + 10, y, "NODE INSPECTOR", text); y += 25;
    if (!asset.graph.nodes.empty()) {
        const std::size_t selected = std::min(eventPanel.selected_node(), asset.graph.nodes.size() - 1U);
        const auto& node = asset.graph.nodes[selected];
        painter.text(layout.inspector.x + 10, y, "Index: " + std::to_string(selected), muted); y += 20;
        painter.text(layout.inspector.x + 10, y,
                     "Type: " + std::string(audio::audio_event_node_type_name(node.type)), muted); y += 20;
        painter.text(layout.inspector.x + 10, y, "Children: " + std::to_string(node.children.size()), muted); y += 20;
        painter.text(layout.inspector.x + 10, y,
                     selected == asset.graph.root ? "ROOT NODE" : "Delete/Copy enabled", muted);
    }
    const auto properties = eventPanel.selected_properties();
    for (std::size_t row = 0; row < properties.size() && row < layout.propertyRowCount; ++row) {
        const auto& property = properties[row];
        const auto minus = layout.propertyMinusButtons[row];
        const auto plus = layout.propertyPlusButtons[row];
        const int rowY = minus.y + 16;
        std::string value = property.label + ": " + property.value;
        if (!property.unit.empty()) value += " " + property.unit;
        painter.text(layout.inspector.x + 10, rowY, value, text);
        painter.fill(minus, panel2); painter.outline(minus, border);
        painter.fill(plus, panel2); painter.outline(plus, border);
        painter.text(minus.x + 7, minus.y + 16, "-", text);
        painter.text(plus.x + 7, plus.y + 16, "+", text);
    }
    y = layout.inspector.y + 126 + static_cast<int>(properties.size()) * 29;
    painter.text(layout.inspector.x + 10, y, "Seed: " + std::to_string(asset.deterministicSeed), muted); y += 25;
    painter.text(layout.inspector.x + 10, y,
                 report.succeeded() ? "COMPILE: READY" : "COMPILE: ERROR",
                 report.succeeded() ? rgb(80,220,135) : rgb(245,105,105)); y += 23;
    painter.text(layout.inspector.x + 10, y, std::string(eventPanel.status_message()), muted); y += 24;
    std::size_t shown{};
    for (const auto& diagnostic : report.diagnostics) {
        if (shown++ >= 8U) break;
        const std::string nodeText = diagnostic.node == UINT32_MAX ? "Graph" : "N" + std::to_string(diagnostic.node);
        painter.text(layout.inspector.x + 10, y, nodeText + ": " + diagnostic.message,
                     diagnostic.severity == audio::AudioEventDiagnosticSeverity::Error
                         ? rgb(245,105,105) : rgb(235,190,80));
        y += 18;
    }
    painter.text(layout.panel.x + 14, layout.panel.y + layout.panel.height - 10,
                 "Ctrl+6 toggles | drag nodes | drag orange output to blue input | .dveaudio v4", muted);
}

void render_control_rig_panel(const IEditorCanvas& painter, NativeEditorController& controller,
                              EditorColor panel, EditorColor panel2, EditorColor border,
                              EditorColor text, EditorColor muted, EditorColor accent) {
    if (!controller.control_rig_panel().open()) return;
    const ControlRigEditorFrame frame = controller.control_rig_panel().frame();
    painter.fill({0, 0, frame.panel.x * 2 + frame.panel.width,
                  frame.panel.y * 2 + frame.panel.height}, rgb(8, 10, 14));
    painter.fill(frame.panel, rgb(22, 26, 33));
    painter.outline(frame.panel, accent);
    painter.fill(frame.titleBar, rgb(31, 52, 78));
    painter.text(frame.titleBar.x + 13, frame.titleBar.y + 24,
                 "CONTROL RIG  |  " + controller.control_rig_panel().session().document().rig.name, text);
    painter.fill(frame.closeButton, rgb(108, 45, 50));
    painter.outline(frame.closeButton, border);
    painter.text(frame.closeButton.x + 7, frame.closeButton.y + 17, "X", text);
    painter.fill(frame.tabBar, rgb(25, 29, 37));
    painter.line(frame.tabBar.x, frame.tabBar.y + frame.tabBar.height - 1,
                 frame.tabBar.x + frame.tabBar.width, frame.tabBar.y + frame.tabBar.height - 1, border);
    for (const auto& tab : frame.tabs) {
        painter.fill(tab.rect, tab.active ? rgb(44, 64, 88) : panel2);
        painter.outline(tab.rect, tab.active ? accent : border);
        std::string label = tab.title;
        if (tab.dirty) label += " *";
        if (tab.recovered) label += " [Recovered]";
        if (label.size() > 26U) label.resize(26U), label += "...";
        painter.text(tab.rect.x + 8, tab.rect.y + 19, label, tab.active ? text : muted);
    }
    painter.fill(frame.toolbar, panel);
    painter.line(frame.toolbar.x, frame.toolbar.y + frame.toolbar.height - 1,
                 frame.toolbar.x + frame.toolbar.width, frame.toolbar.y + frame.toolbar.height - 1, border);
    painter.text(frame.toolbar.x + 12, frame.toolbar.y + 22,
                 "Ctrl+S save  |  Ctrl+W close tab  |  RMB create  |  MMB/Alt drag pan  |  wheel zoom  |  F frame", muted);
    const std::string zoom = std::to_string(static_cast<int>(frame.zoom * 100.0F)) + "%";
    painter.text(frame.toolbar.x + frame.toolbar.width - painter.text_width(zoom) - 12,
                 frame.toolbar.y + 22, zoom, accent);

    const UiRect palette{frame.panel.x, frame.canvas.y, frame.canvas.x - frame.panel.x, frame.canvas.height};
    painter.fill(palette, panel);
    painter.outline(palette, border);
    painter.text(palette.x + 10, palette.y + 22, "RIG PALETTE", text);
    painter.text(palette.x + 10, palette.y + 48, "Controls", accent);
    painter.text(palette.x + 14, palette.y + 70, "Transform", muted);
    painter.text(palette.x + 14, palette.y + 90, "Translation", muted);
    painter.text(palette.x + 14, palette.y + 110, "Rotation", muted);
    painter.text(palette.x + 10, palette.y + 144, "Solve Nodes", accent);
    painter.text(palette.x + 14, palette.y + 166, "Set / Copy Bone", muted);
    painter.text(palette.x + 14, palette.y + 186, "Constraints", muted);
    painter.text(palette.x + 14, palette.y + 206, "Two Bone IK", muted);
    painter.text(palette.x + 14, palette.y + 226, "FABRIK", muted);

    painter.fill(frame.canvas, rgb(15, 19, 26));
    painter.outline(frame.canvas, border);
    const int gridStep = std::max(14, static_cast<int>(32.0F * frame.zoom));
    for (int x = frame.canvas.x; x < frame.canvas.x + frame.canvas.width; x += gridStep)
        painter.line(x, frame.canvas.y, x, frame.canvas.y + frame.canvas.height, rgb(28, 34, 44));
    for (int y = frame.canvas.y; y < frame.canvas.y + frame.canvas.height; y += gridStep)
        painter.line(frame.canvas.x, y, frame.canvas.x + frame.canvas.width, y, rgb(28, 34, 44));

    for (const auto& comment : frame.comments) {
        const EditorColor color = rgb(byte(comment.color.r * 0.8F), byte(comment.color.g * 0.8F), byte(comment.color.b * 0.8F));
        painter.fill(comment.rect, color);
        painter.outline(comment.rect, rgb(byte(comment.color.r), byte(comment.color.g), byte(comment.color.b)));
        painter.text(comment.rect.x + 8, comment.rect.y + 20, comment.text, text);
    }
    const auto pin_color = [&](ControlRigGraphPinType type) {
        switch (type) {
            case ControlRigGraphPinType::Execute: return rgb(225, 225, 225);
            case ControlRigGraphPinType::Transform: return rgb(245, 170, 72);
            case ControlRigGraphPinType::Position: return rgb(80, 205, 132);
            case ControlRigGraphPinType::Rotation: return rgb(165, 110, 235);
        }
        return text;
    };
    for (const auto& link : frame.links) {
        const int midX = (link.fromX + link.toX) / 2;
        const EditorColor color = link.hovered ? accent : pin_color(link.type);
        const int width = link.hovered ? 3 : 2;
        painter.line(link.fromX, link.fromY, midX, link.fromY, color, width);
        painter.line(midX, link.fromY, midX, link.toY, color, width);
        painter.line(midX, link.toY, link.toX, link.toY, color, width);
    }
    for (const auto& card : frame.cards) {
        const EditorColor body = card.selected ? rgb(42, 78, 116) : panel2;
        const EditorColor outline = card.selected ? accent : (card.hovered ? rgb(120, 160, 205) : border);
        painter.fill(card.rect, body);
        painter.outline(card.rect, outline);
        const UiRect titleRect{card.rect.x, card.rect.y, card.rect.width, std::min(25, card.rect.height)};
        painter.fill(titleRect, card.kind == ControlRigGraphEntityKind::Control ? rgb(55, 89, 70) : rgb(58, 62, 91));
        painter.text(card.rect.x + 8, card.rect.y + 18, card.title, text);
        painter.text(card.rect.x + 8, card.rect.y + 42, card.subtitle, muted);
        if (card.diagnosticCount > 0U) {
            const EditorColor badge = card.diagnostic == ControlRigDiagnosticSeverity::Error
                ? rgb(230, 78, 78) : card.diagnostic == ControlRigDiagnosticSeverity::Warning
                    ? rgb(224, 170, 65) : rgb(75, 155, 225);
            const UiRect badgeRect{card.rect.x + card.rect.width - 24, card.rect.y + 4, 18, 16};
            painter.fill(badgeRect, badge);
            painter.text(badgeRect.x + 5, badgeRect.y + 13, std::to_string(card.diagnosticCount), text);
        }
        for (const auto& pin : card.pins) {
            painter.fill(pin.rect, pin_color(pin.type));
            painter.outline(pin.rect, rgb(10, 12, 16));
            const int labelWidth = painter.text_width(pin.label);
            const int labelX = pin.direction == ControlRigGraphPinDirection::Input
                ? pin.rect.x + 14 : pin.rect.x - labelWidth - 5;
            painter.text(labelX, pin.rect.y + 10, pin.label, muted);
        }
    }
    if (frame.draggedPin) {
        const int fromX = frame.draggedPin->rect.x + frame.draggedPin->rect.width / 2;
        const int fromY = frame.draggedPin->rect.y + frame.draggedPin->rect.height / 2;
        painter.line(fromX, fromY, frame.draggedPinX, frame.draggedPinY,
                     pin_color(frame.draggedPin->type), 3);
    }
    if (frame.marquee) {
        painter.outline(*frame.marquee, accent);
    }

    painter.fill(frame.inspector, panel);
    painter.outline(frame.inspector, border);
    painter.text(frame.inspector.x + 10, frame.inspector.y + 22, "VIEWPORT + INSPECTOR", text);
    painter.fill(frame.preview, rgb(10, 14, 20));
    painter.outline(frame.preview, border);
    for (const auto& line : frame.previewLines) {
        EditorColor color = rgb(byte(line.color.r), byte(line.color.g), byte(line.color.b));
        if (line.selected) color = accent;
        painter.line(line.fromX, line.fromY, line.toX, line.toY, color, line.selected ? 3 : 2);
    }
    if (frame.previewSelection) {
        painter.line(frame.gizmoOriginX, frame.gizmoOriginY, frame.gizmoOriginX + 42,
                     frame.gizmoOriginY, frame.gizmoAxis == 1 ? rgb(255, 235, 235) : rgb(230, 75, 75), 3);
        painter.line(frame.gizmoOriginX, frame.gizmoOriginY, frame.gizmoOriginX,
                     frame.gizmoOriginY - 42, frame.gizmoAxis == 2 ? rgb(235, 255, 235) : rgb(75, 220, 105), 3);
        painter.line(frame.gizmoOriginX, frame.gizmoOriginY, frame.gizmoOriginX - 30,
                     frame.gizmoOriginY + 30, frame.gizmoAxis == 3 ? rgb(235, 235, 255) : rgb(80, 125, 245), 3);
    }
    if (frame.inspectorProperties.empty()) {
        const int infoY = frame.preview.y + frame.preview.height + 28;
        const auto diagnostics = controller.control_rig_panel().session().document().diagnostics(
            controller.control_rig_panel().skeleton());
        painter.text(frame.inspector.x + 10, infoY, "Select controls or nodes to inspect", muted);
        painter.text(frame.inspector.x + 10, infoY + 22,
                     "Diagnostics: " + std::to_string(diagnostics.size()),
                     diagnostics.empty() ? rgb(80, 220, 135) : rgb(235, 190, 80));
    }
    for (const auto& property : frame.inspectorProperties) {
        painter.fill(property.row, property.activeEdit ? rgb(44, 78, 116) : panel2);
        painter.outline(property.row, property.activeEdit ? accent : border);
        painter.text(property.row.x + 6, property.row.y + 16, property.label, muted);
        std::string value = property.value;
        if (value.size() > 20U) value.resize(20U), value += "...";
        const int valueX = property.row.x + std::max(92, property.row.width / 2);
        painter.text(valueX, property.row.y + 16, value, property.mixed ? rgb(235, 190, 80) : text);
        painter.fill(property.decrementButton, rgb(46, 52, 63));
        painter.fill(property.incrementButton, rgb(46, 52, 63));
        painter.outline(property.decrementButton, border);
        painter.outline(property.incrementButton, border);
        painter.text(property.decrementButton.x + 7, property.decrementButton.y + 14, "-", text);
        painter.text(property.incrementButton.x + 6, property.incrementButton.y + 14, "+", text);
    }
    painter.text(frame.inspector.x + 10, frame.inspector.y + frame.inspector.height - 14,
                 frame.status, muted);

    if (!frame.contextActions.empty()) {
        const UiRect popup{frame.contextActions.front().rect.x, frame.contextActions.front().rect.y,
                           frame.contextActions.front().rect.width,
                           static_cast<int>(frame.contextActions.size()) * frame.contextActions.front().rect.height};
        painter.fill(popup, panel2);
        painter.outline(popup, border);
        for (const auto& action : frame.contextActions) {
            if (action.hovered) painter.fill(action.rect, rgb(48, 88, 142));
            painter.text(action.rect.x + 8, action.rect.y + 18, action.label, text);
        }
    }
}


namespace {

UiRect intersect_rect(UiRect a, UiRect b) noexcept {
    const int left = std::max(a.x, b.x);
    const int top = std::max(a.y, b.y);
    const int right = std::min(a.x + a.width, b.x + b.width);
    const int bottom = std::min(a.y + a.height, b.y + b.height);
    return {left, top, std::max(0, right - left), std::max(0, bottom - top)};
}

std::string_view sprite_loop_name(SpriteLoopMode mode) noexcept {
    switch (mode) {
        case SpriteLoopMode::Once: return "Once";
        case SpriteLoopMode::Loop: return "Loop";
        case SpriteLoopMode::PingPong: return "PingPong";
    }
    return "Loop";
}

EditorColor sprite_pixel_color(const SpriteDecodedImage& image, std::uint32_t x,
                               std::uint32_t y, const SpritePaletteBank* sourcePalette,
                               const SpritePalettePacket* palette, bool indexedTransport,
                               EditorColor fallback) noexcept {
    if (x >= image.width || y >= image.height) return fallback;
    const std::size_t index = (static_cast<std::size_t>(y) * image.width + x) * 4U;
    if (index + 3U >= image.rgba8.size()) return fallback;
    unsigned r = std::to_integer<unsigned char>(image.rgba8[index]);
    unsigned g = std::to_integer<unsigned char>(image.rgba8[index + 1U]);
    unsigned b = std::to_integer<unsigned char>(image.rgba8[index + 2U]);
    unsigned a = std::to_integer<unsigned char>(image.rgba8[index + 3U]);
    if (palette != nullptr && sourcePalette != nullptr) {
        std::optional<std::size_t> paletteIndex;
        if (indexedTransport && r < palette->colors.size()) {
            paletteIndex = r;
        } else {
            SpriteColor8 sourceColor{static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g),
                                     static_cast<std::uint8_t>(b), static_cast<std::uint8_t>(a)};
            if (sourceColor.a == 0U) sourceColor = {0U, 0U, 0U, 0U};
            const auto found = std::find(sourcePalette->colors.begin(), sourcePalette->colors.end(),
                                         sourceColor);
            if (found != sourcePalette->colors.end()) {
                paletteIndex = static_cast<std::size_t>(
                    std::distance(sourcePalette->colors.begin(), found));
            }
        }
        if (paletteIndex && *paletteIndex < palette->colors.size()) {
            const SpriteColor8 color = palette->colors[*paletteIndex];
            r = color.r;
            g = color.g;
            b = color.b;
            a = color.a;
        }
    }
    if (a == 0U) return fallback;
    if (a == 255U) return rgb(r, g, b);
    const unsigned checker = 48U;
    return rgb((r * a + checker * (255U - a)) / 255U,
               (g * a + checker * (255U - a)) / 255U,
               (b * a + checker * (255U - a)) / 255U);
}

void render_sprite_thumbnail(const IEditorCanvas& painter, const SpriteDecodedImage& image,
                             const SpriteFrame& frame, const SpritePaletteBank* sourcePalette,
                             const SpritePalettePacket* palette, bool indexedTransport,
                             UiRect area, EditorColor background, EditorColor border) {
    painter.fill(area, background);
    painter.outline(area, border);
    if (image.width == 0U || frame.atlasRect.width == 0U || frame.atlasRect.height == 0U) return;
    const int inset = 5;
    const int availableWidth = std::max(1, area.width - inset * 2);
    const int availableHeight = std::max(1, area.height - 24);
    const float scale = std::min(static_cast<float>(availableWidth) / static_cast<float>(frame.atlasRect.width),
                                 static_cast<float>(availableHeight) / static_cast<float>(frame.atlasRect.height));
    const int drawWidth = std::max(1, static_cast<int>(std::floor(static_cast<float>(frame.atlasRect.width) * scale)));
    const int drawHeight = std::max(1, static_cast<int>(std::floor(static_cast<float>(frame.atlasRect.height) * scale)));
    const int originX = area.x + (area.width - drawWidth) / 2;
    const int originY = area.y + 4 + (availableHeight - drawHeight) / 2;
    const int stride = std::max(1, static_cast<int>(std::ceil(1.0F / std::max(scale, 0.001F))));
    for (std::uint32_t sy = 0U; sy < frame.atlasRect.height; sy += static_cast<std::uint32_t>(stride)) {
        for (std::uint32_t sx = 0U; sx < frame.atlasRect.width; sx += static_cast<std::uint32_t>(stride)) {
            const int x0 = originX + static_cast<int>(std::floor(static_cast<float>(sx) * scale));
            const int y0 = originY + static_cast<int>(std::floor(static_cast<float>(sy) * scale));
            const int x1 = originX + static_cast<int>(std::ceil(static_cast<float>(std::min(frame.atlasRect.width, sx + static_cast<std::uint32_t>(stride))) * scale));
            const int y1 = originY + static_cast<int>(std::ceil(static_cast<float>(std::min(frame.atlasRect.height, sy + static_cast<std::uint32_t>(stride))) * scale));
            const EditorColor color = sprite_pixel_color(image, frame.atlasRect.x + sx,
                                                         frame.atlasRect.y + sy, sourcePalette,
                                                         palette, indexedTransport, background);
            painter.fill({x0, y0, std::max(1, x1 - x0), std::max(1, y1 - y0)}, color);
        }
    }
}

void render_sprite_authoring_panel(const IEditorCanvas& painter, NativeEditorController& controller,
                                   EditorColor panel, EditorColor panel2, EditorColor border,
                                   EditorColor text, EditorColor muted, EditorColor accent) {
    if (!controller.sprite_authoring_panel().open()) return;
    const EditorSpriteAuthoringPanel& editor = controller.sprite_authoring_panel();
    const SpriteAuthoringPanelFrame layout = editor.frame();
    const SpriteAuthoringWorkspace& workspace = editor.workspace();
    const SpriteAsset& asset = workspace.session().asset();
    const SpriteDecodedImage& image = workspace.source_image();
    const SpriteCanvasViewState& view = workspace.canvas();
    const SpriteTimelineState& timeline = workspace.timeline();
    const SpriteClip* clip = find_sprite_clip(asset, timeline.clip);
    SpritePalettePacket palettePacket;
    const SpritePalettePacket* palette = nullptr;
    const SpritePaletteBank* sourcePalette = nullptr;
    const bool indexedTransport = editor.palette_texture_is_index_transport();
    std::string paletteError;
    if (editor.has_palette_document() &&
        editor.palette_session().resolve_preview(palettePacket, &paletteError)) {
        palette = &palettePacket;
        const SpritePaletteAuthoringSelection selection = editor.palette_session().selection();
        sourcePalette = &editor.palette_session().asset().banks[selection.bank];
    }

    auto button = [&](UiRect rect, std::string_view label, bool active = false) {
        painter.fill(rect, active ? accent : panel2);
        painter.outline(rect, active ? rgb(190,220,255) : border);
        painter.text(rect.x + 6, rect.y + rect.height - 8, label, text);
    };

    painter.fill({0,0,controller.layout().menuBar.width,
                  controller.layout().statusBar.y + controller.layout().statusBar.height}, rgb(8,10,14));
    painter.fill(layout.panel, panel);
    painter.outline(layout.panel, accent);
    painter.fill(layout.titleBar, rgb(27,44,67));
    painter.text(layout.titleBar.x + 12, layout.titleBar.y + 22,
                 "SPRITE EDITOR  |  " + asset.name + (workspace.session().dirty() ? " *" : ""), text);
    painter.fill(layout.closeButton, rgb(100,42,48));
    painter.outline(layout.closeButton, border);
    painter.text(layout.closeButton.x + 7, layout.closeButton.y + 16, "X", text);

    button(layout.saveButton, "Save");
    button(layout.reimportButton, "Reimport");
    button(layout.gridSliceButton, "Grid Slice");
    button(layout.freeSliceButton, "Free Slice", editor.free_slice_mode());
    button(layout.repackButton, "Repack");
    button(layout.previousButton, "<");
    button(layout.playButton, timeline.playing ? "Pause" : "Play", timeline.playing);
    button(layout.nextButton, ">");
    button(layout.loopButton, clip ? sprite_loop_name(clip->loopMode) : "Loop");
    button(layout.gridButton, "Grid", view.showPixelGrid);
    button(layout.onionPreviousButton, "Onion -", view.onionSkinPrevious);
    button(layout.onionNextButton, "Onion +", view.onionSkinNext);

    painter.fill(layout.canvas, rgb(18,21,27));
    painter.outline(layout.canvas, border);
    constexpr int checkerSize = 12;
    for (int y = layout.canvas.y; y < layout.canvas.y + layout.canvas.height; y += checkerSize) {
        for (int x = layout.canvas.x; x < layout.canvas.x + layout.canvas.width; x += checkerSize) {
            const bool alternate = (((x - layout.canvas.x) / checkerSize) + ((y - layout.canvas.y) / checkerSize)) % 2 != 0;
            painter.fill(intersect_rect({x,y,checkerSize,checkerSize}, layout.canvas),
                         alternate ? rgb(42,45,52) : rgb(34,37,43));
        }
    }
    if (image.width != 0U && image.height != 0U) {
        const int originX = layout.canvas.x + static_cast<int>(std::lround(view.panPixels.x));
        const int originY = layout.canvas.y + static_cast<int>(std::lround(view.panPixels.y));
        const double estimated = static_cast<double>(image.width) * image.height;
        const std::uint32_t stride = static_cast<std::uint32_t>(std::max(1.0, std::ceil(std::sqrt(estimated / 65536.0))));
        for (std::uint32_t sy = 0U; sy < image.height; sy += stride) {
            for (std::uint32_t sx = 0U; sx < image.width; sx += stride) {
                const int x0 = originX + static_cast<int>(std::floor(static_cast<float>(sx) * view.zoom));
                const int y0 = originY + static_cast<int>(std::floor(static_cast<float>(sy) * view.zoom));
                const int x1 = originX + static_cast<int>(std::ceil(static_cast<float>(std::min(image.width, sx + stride)) * view.zoom));
                const int y1 = originY + static_cast<int>(std::ceil(static_cast<float>(std::min(image.height, sy + stride)) * view.zoom));
                UiRect pixel = intersect_rect({x0,y0,std::max(1,x1-x0),std::max(1,y1-y0)}, layout.canvas);
                if (pixel.width <= 0 || pixel.height <= 0) continue;
                painter.fill(pixel, sprite_pixel_color(image, sx, sy, sourcePalette, palette, indexedTransport, rgb(45,45,50)));
            }
        }
        if (view.showPixelGrid && view.zoom >= 4.0F && stride == 1U) {
            const EditorColor grid = rgb(22,25,31);
            const std::uint32_t firstX = static_cast<std::uint32_t>(std::max(0.0F,
                std::floor(static_cast<float>(layout.canvas.x - originX) / view.zoom)));
            const std::uint32_t lastX = std::min(image.width, static_cast<std::uint32_t>(std::max(0.0F,
                std::ceil(static_cast<float>(layout.canvas.x + layout.canvas.width - originX) / view.zoom))));
            const std::uint32_t firstY = static_cast<std::uint32_t>(std::max(0.0F,
                std::floor(static_cast<float>(layout.canvas.y - originY) / view.zoom)));
            const std::uint32_t lastY = std::min(image.height, static_cast<std::uint32_t>(std::max(0.0F,
                std::ceil(static_cast<float>(layout.canvas.y + layout.canvas.height - originY) / view.zoom))));
            for (std::uint32_t x = firstX; x <= lastX; ++x) {
                const int screenX = originX + static_cast<int>(std::lround(static_cast<float>(x) * view.zoom));
                painter.line(screenX, layout.canvas.y, screenX, layout.canvas.y + layout.canvas.height, grid);
            }
            for (std::uint32_t y = firstY; y <= lastY; ++y) {
                const int screenY = originY + static_cast<int>(std::lround(static_cast<float>(y) * view.zoom));
                painter.line(layout.canvas.x, screenY, layout.canvas.x + layout.canvas.width, screenY, grid);
            }
        }

        if (clip != nullptr) {
            for (std::size_t sequence = 0U; sequence < clip->frames.size(); ++sequence) {
                const SpriteFrame& frame = asset.frames[clip->frames[sequence]];
                const UiRect rect{
                    originX + static_cast<int>(std::lround(static_cast<float>(frame.atlasRect.x) * view.zoom)),
                    originY + static_cast<int>(std::lround(static_cast<float>(frame.atlasRect.y) * view.zoom)),
                    std::max(1, static_cast<int>(std::lround(static_cast<float>(frame.atlasRect.width) * view.zoom))),
                    std::max(1, static_cast<int>(std::lround(static_cast<float>(frame.atlasRect.height) * view.zoom)))};
                const bool selected = std::find(timeline.selectedSequenceIndices.begin(),
                                                timeline.selectedSequenceIndices.end(), sequence) !=
                                      timeline.selectedSequenceIndices.end();
                const UiRect clipped = intersect_rect(rect, layout.canvas);
                if (clipped.width > 0 && clipped.height > 0)
                    painter.outline(clipped, selected ? accent : rgb(235,190,80));
            }
            if (timeline.primarySequenceIndex && *timeline.primarySequenceIndex < clip->frames.size() && view.showPivot) {
                const SpriteFrame& selected = asset.frames[clip->frames[*timeline.primarySequenceIndex]];
                const float localPivotX = selected.pivotPixels.x - static_cast<float>(selected.sourceOffsetX);
                const float localPivotYFromTop = static_cast<float>(selected.atlasRect.height) -
                    (selected.pivotPixels.y - static_cast<float>(selected.sourceOffsetY));
                const int px = originX + static_cast<int>(std::lround((static_cast<float>(selected.atlasRect.x) + localPivotX) * view.zoom));
                const int py = originY + static_cast<int>(std::lround((static_cast<float>(selected.atlasRect.y) + localPivotYFromTop) * view.zoom));
                if (layout.canvas.contains(px, py)) {
                    painter.line(px - 7, py, px + 7, py, rgb(255,80,120), 2);
                    painter.line(px, py - 7, px, py + 7, rgb(255,80,120), 2);
                }
            }
            if (layout.tracksMode && timeline.primarySequenceIndex &&
                *timeline.primarySequenceIndex < clip->frames.size()) {
                const std::size_t sequence = *timeline.primarySequenceIndex;
                const SpriteFrame& selected = asset.frames[clip->frames[sequence]];
                const auto local_to_screen = [&](SpriteVec2 local) {
                    const float atlasX = static_cast<float>(selected.atlasRect.x) +
                        selected.pivotPixels.x + local.x - static_cast<float>(selected.sourceOffsetX);
                    const float atlasY = static_cast<float>(selected.atlasRect.y) +
                        static_cast<float>(selected.atlasRect.height) -
                        (selected.pivotPixels.y + local.y - static_cast<float>(selected.sourceOffsetY));
                    return std::pair<int, int>{
                        originX + static_cast<int>(std::lround(atlasX * view.zoom)),
                        originY + static_cast<int>(std::lround(atlasY * view.zoom))};
                };
                SpriteTrackProductionEditor productionTools(
                    const_cast<SpriteAuthoringSession&>(workspace.session()));
                const auto onionOverlays = productionTools.onion_overlays(
                    clip->name, sequence, view.onionSkinPrevious, view.onionSkinNext, 0.35F);
                for (const SpriteTrackOverlay& overlay : onionOverlays) {
                    const auto [ox, oy] = local_to_screen(overlay.centerPixels);
                    if (overlay.kind == SpriteTrackItemKind::CombatWindow) {
                        const int overlayWidth = std::max(2, static_cast<int>(std::lround(
                            overlay.sizePixels.x * view.zoom)));
                        const int overlayHeight = std::max(2, static_cast<int>(std::lround(
                            overlay.sizePixels.y * view.zoom)));
                        const EditorColor onionColor = overlay.relativeFrame < 0
                            ? rgb(90, 125, 210) : rgb(215, 125, 90);
                        painter.outline(intersect_rect({ox - overlayWidth / 2, oy - overlayHeight / 2,
                                                       overlayWidth, overlayHeight}, layout.canvas), onionColor);
                    } else if (overlay.kind == SpriteTrackItemKind::SocketKey &&
                               layout.canvas.contains(ox, oy)) {
                        const EditorColor onionColor = overlay.relativeFrame < 0
                            ? rgb(80, 125, 205) : rgb(220, 130, 80);
                        painter.line(ox - 4, oy, ox + 4, oy, onionColor);
                        painter.line(ox, oy - 4, ox, oy + 4, onionColor);
                    }
                }
                for (const SpriteCombatWindow& window : clip->combatWindows) {
                    if (sequence < window.firstSequenceIndex || sequence > window.lastSequenceIndex) continue;
                    const auto [cx, cy] = local_to_screen(window.volume.centerPixels);
                    const int width = std::max(2, static_cast<int>(std::lround(window.volume.sizePixels.x * view.zoom)));
                    const int height = std::max(2, static_cast<int>(std::lround(window.volume.sizePixels.y * view.zoom)));
                    const UiRect bounds = intersect_rect({cx - width / 2, cy - height / 2, width, height}, layout.canvas);
                    if (bounds.width <= 0 || bounds.height <= 0) continue;
                    const EditorColor roleColor = window.volume.role == SpriteCombatRole::Hitbox
                        ? rgb(255,72,72) : window.volume.role == SpriteCombatRole::Hurtbox
                        ? rgb(80,210,120) : rgb(255,190,70);
                    painter.outline(bounds, roleColor);
                    const auto selectedTrack = workspace.session().document().selectedTrack;
                    if (selectedTrack && selectedTrack->kind == SpriteTrackItemKind::CombatWindow &&
                        selectedTrack->id == window.id) {
                        painter.outline({bounds.x - 2, bounds.y - 2, bounds.width + 4, bounds.height + 4},
                                        rgb(255,255,255));
                        const std::array<std::pair<int,int>, 8> handles{{
                            {bounds.x, bounds.y}, {bounds.x + bounds.width / 2, bounds.y},
                            {bounds.x + bounds.width, bounds.y},
                            {bounds.x, bounds.y + bounds.height / 2},
                            {bounds.x + bounds.width, bounds.y + bounds.height / 2},
                            {bounds.x, bounds.y + bounds.height},
                            {bounds.x + bounds.width / 2, bounds.y + bounds.height},
                            {bounds.x + bounds.width, bounds.y + bounds.height}}};
                        for (const auto& [hx, hy] : handles) {
                            painter.fill({hx - 3, hy - 3, 7, 7}, rgb(245,245,245));
                            painter.outline({hx - 3, hy - 3, 7, 7}, roleColor);
                        }
                    }
                    painter.text(bounds.x + 3, bounds.y + 13, window.volume.name, roleColor);
                }
                for (const SpriteSocketKey& key : clip->socketKeys) {
                    if (key.sequenceIndex != sequence) continue;
                    const auto [sx, sy] = local_to_screen(key.positionPixels);
                    if (!layout.canvas.contains(sx, sy)) continue;
                    painter.line(sx - 6, sy, sx + 6, sy, rgb(90,190,255), 2);
                    painter.line(sx, sy - 6, sx, sy + 6, rgb(90,190,255), 2);
                    const auto selectedTrack = workspace.session().document().selectedTrack;
                    if (selectedTrack && selectedTrack->kind == SpriteTrackItemKind::SocketKey &&
                        selectedTrack->id == key.id) {
                        painter.outline({sx - 8, sy - 8, 16, 16}, rgb(255,255,255));
                    }
                    painter.text(sx + 7, sy - 4, key.name, rgb(90,190,255));
                }
            }
        }
    } else {
        painter.text(layout.canvas.x + 20, layout.canvas.y + 34,
                     "Select a PNG/JPEG texture or .dvesprite in Assets, then choose Open.", muted);
    }
    if (layout.freeSlicePreview) {
        const UiRect preview = intersect_rect(*layout.freeSlicePreview, layout.canvas);
        if (preview.width > 0 && preview.height > 0) painter.outline(preview, rgb(100,230,150));
    }
    painter.text(layout.canvas.x + 8, layout.canvas.y + layout.canvas.height - 8,
                 "Zoom " + std::to_string(view.zoom).substr(0,4) +
                 "x | wheel zoom | Alt+drag or middle drag pans | click sets pivot", muted);

    painter.fill(layout.timeline, panel2);
    painter.outline(layout.timeline, border);
    painter.text(layout.timeline.x + 8, layout.timeline.y + 18,
                 "TIMELINE  " + timeline.clip + "  | Ctrl multi-select | Shift range | drag reorder", muted);
    if (clip != nullptr) {
        for (std::size_t local = 0U; local < layout.timelineFrames.size(); ++local) {
            const std::size_t index = layout.timelineFrameIndices[local];
            if (index >= clip->frames.size()) continue;
            const bool selected = std::find(timeline.selectedSequenceIndices.begin(),
                                            timeline.selectedSequenceIndices.end(), index) !=
                                  timeline.selectedSequenceIndices.end();
            render_sprite_thumbnail(painter, image, asset.frames[clip->frames[index]], sourcePalette,
                                    palette, indexedTransport, layout.timelineFrames[local], selected ? rgb(39,69,105) : rgb(27,31,38),
                                    selected ? accent : border);
            const SpriteFrame& frame = asset.frames[clip->frames[index]];
            painter.text(layout.timelineFrames[local].x + 5,
                         layout.timelineFrames[local].y + layout.timelineFrames[local].height - 6,
                         std::to_string(index) + " " + frame.name, selected ? text : muted);
            if (!frame.event.empty())
                painter.text(layout.timelineFrames[local].x + layout.timelineFrames[local].width - 15,
                             layout.timelineFrames[local].y + 15, "!", rgb(255,190,80));
        }
        for (const UiRect& marker : layout.timelineCombatMarkers) painter.fill(marker, rgb(255,72,72));
        for (const UiRect& marker : layout.timelineSocketMarkers) painter.fill(marker, rgb(90,190,255));
        for (const UiRect& marker : layout.timelinePropertyMarkers) painter.fill(marker, rgb(210,120,255));
        for (const UiRect& marker : layout.timelineRootMarkers) painter.fill(marker, rgb(255,205,80));
    }

    painter.fill(layout.inspector, panel);
    painter.outline(layout.inspector, border);
    button(layout.sliceTabButton, "Slice", !layout.paletteMode && !layout.tracksMode);
    button(layout.paletteTabButton,
           editor.has_palette_document() ? "Palette" : "+Palette",
           layout.paletteMode);
    button(layout.tracksTabButton, "Tracks", layout.tracksMode);
    if (!layout.paletteMode && !layout.tracksMode) {
        const SpriteGridSliceSettings& grid = editor.grid_settings();
        std::array<std::string,5> labels{
            "Cell width: " + std::to_string(grid.cellWidth),
            "Cell height: " + std::to_string(grid.cellHeight),
            "Duration: " + std::to_string(grid.durationSeconds).substr(0,6) + " s",
            "Pivot X", "Pivot Y"};
        const std::array<UiRect,5> downs{layout.cellWidthDown, layout.cellHeightDown,
            layout.durationDown, layout.pivotXDown, layout.pivotYDown};
        const std::array<UiRect,5> ups{layout.cellWidthUp, layout.cellHeightUp,
            layout.durationUp, layout.pivotXUp, layout.pivotYUp};
        for (std::size_t index = 0U; index < labels.size(); ++index) {
            painter.text(layout.inspector.x + 10, downs[index].y + 16, labels[index], muted);
            button(downs[index], "-");
            button(ups[index], "+");
        }
        painter.text(layout.inspector.x + 10, layout.trimInButton.y + 16, "Trim bounds", muted);
        button(layout.trimInButton, "In");
        button(layout.trimOutButton, "Out");
        button(layout.trimTransparentButton,
               grid.trimTransparent ? "Trim transparent: On" : "Trim transparent: Off",
               grid.trimTransparent);
        button(layout.skipTransparentButton,
               grid.skipTransparent ? "Skip empty: On" : "Skip empty: Off",
               grid.skipTransparent);
        painter.text(layout.inspector.x + 10, layout.inspector.y + layout.inspector.height - 58,
                     "Frames: " + std::to_string(asset.frames.size()) +
                     " | Atlas: " + std::to_string(asset.textureWidth) + "x" +
                     std::to_string(asset.textureHeight), muted);
        painter.text(layout.inspector.x + 10, layout.inspector.y + layout.inspector.height - 35,
                     "Undo/redo: Ctrl+Z / Ctrl+Y", muted);
    } else if (editor.has_palette_document()) {
        const SpritePaletteAuthoringSession& paletteEditor = editor.palette_session();
        const SpritePaletteAsset& paletteAsset = paletteEditor.asset();
        const SpritePaletteAuthoringSelection selection = paletteEditor.selection();
        const SpritePaletteBank& bank = paletteAsset.banks[selection.bank];
        const SpriteColor8 selectedColor = bank.colors[selection.color];
        painter.text(layout.palettePanel.x + 2, layout.palettePreviousBankButton.y - 4,
                     paletteAsset.name + (paletteEditor.dirty() ? " *" : ""), text);
        button(layout.palettePreviousBankButton, "<");
        button(layout.paletteNextBankButton, ">");
        button(layout.paletteAddBankButton, "+Bank");
        button(layout.paletteRemoveBankButton, "-Bank");
        button(layout.paletteSaveButton, "Save", paletteEditor.dirty());
        button(layout.palettePlayButton, paletteEditor.preview_playing() ? "Pause" : "Play",
               paletteEditor.preview_playing());
        button(layout.paletteAddCycleButton, "+Cycle");
        button(layout.paletteRemoveCycleButton, "-Cycle");
        const bool selectedTransparent = paletteAsset.transparentIndex &&
            *paletteAsset.transparentIndex == selection.color;
        button(layout.paletteTransparentButton,
               selectedTransparent ? "Transparent" : "Set Alpha0", selectedTransparent);
        painter.text(layout.palettePanel.x + 2, layout.palettePlayButton.y - 4,
                     "Bank " + std::to_string(selection.bank + 1U) + "/" +
                     std::to_string(paletteAsset.banks.size()) + ": " + bank.name, muted);

        const std::array<std::string_view, 4> channels{"R", "G", "B", "A"};
        const std::array<unsigned, 4> values{selectedColor.r, selectedColor.g,
                                             selectedColor.b, selectedColor.a};
        for (std::size_t channel = 0U; channel < channels.size(); ++channel) {
            painter.text(layout.palettePanel.x + 2,
                         layout.paletteChannelDown[channel].y + 15,
                         std::string(channels[channel]) + ": " + std::to_string(values[channel]), muted);
            button(layout.paletteChannelDown[channel], "-");
            button(layout.paletteChannelUp[channel], "+");
        }
        const SpritePaletteCycleTrack* cycle = selection.cycle
            ? &paletteAsset.cycles[*selection.cycle] : nullptr;
        const std::array<std::string, 3> cycleLabels{
            cycle ? "Cycle first: " + std::to_string(cycle->firstIndex) : "Cycle first: -",
            cycle ? "Cycle last: " + std::to_string(cycle->lastIndex) : "Cycle last: -",
            cycle ? "Cycle ticks: " + std::to_string(cycle->ticksPerStep) : "Cycle ticks: -"};
        const std::array<UiRect, 3> cycleDown{layout.paletteCycleFirstDown,
            layout.paletteCycleLastDown, layout.paletteCycleTicksDown};
        const std::array<UiRect, 3> cycleUp{layout.paletteCycleFirstUp,
            layout.paletteCycleLastUp, layout.paletteCycleTicksUp};
        for (std::size_t field = 0U; field < cycleLabels.size(); ++field) {
            painter.text(layout.palettePanel.x + 2, cycleDown[field].y + 15,
                         cycleLabels[field], cycle ? muted : rgb(105,112,124));
            button(cycleDown[field], "-");
            button(cycleUp[field], "+");
        }
        if (cycle != nullptr) {
            painter.text(layout.palettePanel.x + 2, layout.paletteSwatchArea.y - 5,
                         cycle->name + " | 240 Hz deterministic preview", muted);
        } else {
            painter.text(layout.palettePanel.x + 2, layout.paletteSwatchArea.y - 5,
                         "Select +Cycle to animate a free entry range", muted);
        }
        for (std::size_t local = 0U; local < layout.paletteSwatches.size(); ++local) {
            const std::size_t colorIndex = layout.paletteSwatchIndices[local];
            SpriteColor8 color = bank.colors[colorIndex];
            if (palette != nullptr && colorIndex < palette->colors.size()) color = palette->colors[colorIndex];
            const unsigned checker = 48U;
            const EditorColor swatch = color.a == 0U ? rgb(checker, checker, checker)
                : color.a == 255U ? rgb(color.r, color.g, color.b)
                : rgb((static_cast<unsigned>(color.r) * color.a + checker * (255U - color.a)) / 255U,
                      (static_cast<unsigned>(color.g) * color.a + checker * (255U - color.a)) / 255U,
                      (static_cast<unsigned>(color.b) * color.a + checker * (255U - color.a)) / 255U);
            painter.fill(layout.paletteSwatches[local], swatch);
            const bool selected = colorIndex == selection.color;
            const bool transparent = paletteAsset.transparentIndex &&
                *paletteAsset.transparentIndex == colorIndex;
            painter.outline(layout.paletteSwatches[local], selected ? accent :
                            transparent ? rgb(255,190,80) : border);
        }
        painter.text(layout.inspector.x + 10, layout.inspector.y + layout.inspector.height - 12,
                     "Swatch " + std::to_string(selection.color) + "/" +
                     std::to_string(paletteAsset.entry_count() - 1U) +
                     " | wheel scroll | Ctrl+Z/Y palette history", muted);
    } else if (layout.tracksMode && clip != nullptr) {
        button(layout.trackAddHitboxButton, "+ Hitbox");
        button(layout.trackAddHurtboxButton, "+ Hurtbox");
        button(layout.trackAddSocketButton, "+ Socket");
        button(layout.trackAddPropertyButton, "+ Property");
        const std::size_t sequence = timeline.primarySequenceIndex.value_or(0U);
        const bool hasRoot = std::any_of(clip->rootMotionKeys.begin(), clip->rootMotionKeys.end(),
            [sequence](const SpriteRootMotionKey& key) { return key.sequenceIndex == sequence; });
        button(layout.trackAddSpawnButton, "+ Spawn");
        button(layout.trackSetRootMotionButton, hasRoot ? "- Root" : "+ Root", hasRoot);
        button(layout.trackDeleteButton, "Delete");
        button(layout.trackPresetButton, "Preset");
        button(layout.trackCopyRangeButton, "Copy Range");
        button(layout.trackPasteRangeButton, "Paste Range");
        button(layout.trackDuplicateButton, "Duplicate");
        button(layout.trackMirrorButton, "Mirror X");
        button(layout.trackRetimeButton, "Retime (-1 / Shift +1)");
        for (std::size_t index = 0U; index < layout.trackNumericDescriptors.size(); ++index) {
            const auto& descriptor = layout.trackNumericDescriptors[index];
            painter.text(layout.inspector.x + 10, layout.trackNumericDown[index].y + 15,
                         descriptor.label + ": " + descriptor.valueText, muted);
            button(layout.trackNumericDown[index], "-");
            button(layout.trackNumericUp[index], "+");
        }
        painter.text(layout.inspector.x + 10,
                     layout.trackRetimeButton.y + layout.trackRetimeButton.height + 16,
                     "Search: " + (layout.trackSearchQuery.empty() ? std::string("Ctrl+F")
                                                                  : layout.trackSearchQuery),
                     layout.trackSearchQuery.empty() ? muted : accent);
        const auto selectedTrack = workspace.session().document().selectedTrack;
        for (std::size_t row = 0U; row < layout.trackRows.size(); ++row) {
            const SpriteTrackItemKind kind = layout.trackRowKinds[row];
            const SpriteTrackId id = layout.trackRowIds[row];
            const bool selected = selectedTrack && selectedTrack->kind == kind && selectedTrack->id == id;
            painter.fill(layout.trackRows[row], selected ? rgb(55,88,125) : rgb(32,37,45));
            painter.outline(layout.trackRows[row], selected ? accent : border);
            std::string label;
            if (kind == SpriteTrackItemKind::CombatWindow) {
                const auto item = std::find_if(clip->combatWindows.begin(), clip->combatWindows.end(),
                    [id](const SpriteCombatWindow& candidate) { return candidate.id == id; });
                if (item != clip->combatWindows.end()) label = "Combat  " + item->volume.name +
                    "  [" + std::to_string(item->firstSequenceIndex) + "-" +
                    std::to_string(item->lastSequenceIndex) + "]";
            } else if (kind == SpriteTrackItemKind::SocketKey) {
                const auto item = std::find_if(clip->socketKeys.begin(), clip->socketKeys.end(),
                    [id](const SpriteSocketKey& candidate) { return candidate.id == id; });
                if (item != clip->socketKeys.end()) label = "Socket  " + item->name;
            } else if (kind == SpriteTrackItemKind::PropertyKey) {
                const auto item = std::find_if(clip->propertyKeys.begin(), clip->propertyKeys.end(),
                    [id](const SpritePropertyKey& candidate) { return candidate.id == id; });
                if (item != clip->propertyKeys.end()) label = "Property  " + item->name;
            } else {
                label = "Root motion";
            }
            painter.text(layout.trackRows[row].x + 5, layout.trackRows[row].y + 16,
                         label.empty() ? "Track item" : label, selected ? text : muted);
        }
        const std::size_t activeCombat = static_cast<std::size_t>(std::count_if(
            clip->combatWindows.begin(), clip->combatWindows.end(),
            [sequence](const SpriteCombatWindow& window) {
                return sequence >= window.firstSequenceIndex && sequence <= window.lastSequenceIndex;
            }));
        const std::size_t frameSockets = static_cast<std::size_t>(std::count_if(
            clip->socketKeys.begin(), clip->socketKeys.end(),
            [sequence](const SpriteSocketKey& key) { return key.sequenceIndex == sequence; }));
        const std::size_t frameProperties = static_cast<std::size_t>(std::count_if(
            clip->propertyKeys.begin(), clip->propertyKeys.end(),
            [sequence](const SpritePropertyKey& key) { return key.sequenceIndex == sequence; }));
        int infoY = layout.trackRows.empty()
            ? layout.trackMirrorButton.y + 34
            : layout.trackRows.back().y + layout.trackRows.back().height + 18;
        painter.text(layout.inspector.x + 10, infoY,
                     "Frame " + std::to_string(sequence), text);
        infoY += 22;
        painter.text(layout.inspector.x + 10, infoY,
                     "Combat: " + std::to_string(activeCombat) + " active / " +
                     std::to_string(clip->combatWindows.size()) + " total", muted);
        infoY += 20;
        painter.text(layout.inspector.x + 10, infoY,
                     "Sockets: " + std::to_string(frameSockets) + " here / " +
                     std::to_string(clip->socketKeys.size()) + " keys", muted);
        infoY += 20;
        painter.text(layout.inspector.x + 10, infoY,
                     "Properties: " + std::to_string(frameProperties) + " here / " +
                     std::to_string(clip->propertyKeys.size()) + " keys", muted);
        infoY += 28;
        painter.text(layout.inspector.x + 10, infoY,
                     "Red combat | Blue sockets", rgb(255,150,150));
        infoY += 20;
        painter.text(layout.inspector.x + 10, infoY,
                     "Purple properties | Gold root", muted);
        infoY += 30;
        painter.text(layout.inspector.x + 10, infoY,
                     "Drag white handles | arrows precision", muted);
        infoY += 20;
        painter.text(layout.inspector.x + 10, infoY,
                     "Ctrl+C/V range | Ctrl+F search", muted);
    }

    painter.fill(layout.statusBar, rgb(22,27,34));
    painter.text(layout.statusBar.x + 10, layout.statusBar.y + 17, editor.status(), muted);
}

} // namespace

namespace {
// Liang-Barsky: cuts the segment to the closed box [minX, maxX] x [minY, maxY].
// Returns false when no part of it lies inside.
bool clip_segment_to_box(float& x1, float& y1, float& x2, float& y2,
                         float minX, float minY, float maxX, float maxY) noexcept {
    if (maxX < minX || maxY < minY) return false;
    const float dx = x2 - x1;
    const float dy = y2 - y1;
    float enter = 0.0F;
    float leave = 1.0F;
    const auto edge = [&](float p, float q) {
        if (p == 0.0F) return q >= 0.0F;
        const float t = q / p;
        if (p < 0.0F) {
            if (t > leave) return false;
            enter = std::max(enter, t);
        } else {
            if (t < enter) return false;
            leave = std::min(leave, t);
        }
        return true;
    };
    if (!edge(-dx, x1 - minX) || !edge(dx, maxX - x1) || !edge(-dy, y1 - minY) || !edge(dy, maxY - y1)) return false;
    const float startX = x1;
    const float startY = y1;
    x1 = std::clamp(startX + enter * dx, minX, maxX);
    y1 = std::clamp(startY + enter * dy, minY, maxY);
    x2 = std::clamp(startX + leave * dx, minX, maxX);
    y2 = std::clamp(startY + leave * dy, minY, maxY);
    return true;
}

// Forwards to another canvas but drops text whose glyphs would reach clipY and
// fills/outlines that extend past it. Used for the inspector's fixed-offset
// detail lines so they never draw under the flag toggles.
class VerticalClipCanvas final : public IEditorCanvas {
public:
    VerticalClipCanvas(const IEditorCanvas& inner, int clipY) : inner_(inner), clipY_(clipY) {}
    void fill(UiRect rect, EditorColor color) const override { if (rect.y + rect.height <= clipY_) inner_.fill(rect, color); }
    void outline(UiRect rect, EditorColor color) const override { if (rect.y + rect.height <= clipY_) inner_.outline(rect, color); }
    void line(int x1, int y1, int x2, int y2, EditorColor color, int width) const override {
        if (std::max(y1, y2) <= clipY_) inner_.line(x1, y1, x2, y2, color, width);
    }
    // y is the text baseline; allow ~4 px of descent.
    void text(int x, int y, std::string_view value, EditorColor color) const override {
        if (y + 4 <= clipY_) inner_.text(x, y, value, color);
    }
    [[nodiscard]] int text_width(std::string_view value) const override { return inner_.text_width(value); }
    [[nodiscard]] std::string_view ellipsis() const override { return inner_.ellipsis(); }
private:
    const IEditorCanvas& inner_;
    int clipY_;
};
} // namespace

void RectClipCanvas::fill(UiRect rect, EditorColor color) const {
    const UiRect clipped = intersect_rect(rect, clip_);
    if (clipped.width > 0 && clipped.height > 0) inner_.fill(clipped, color);
}

void RectClipCanvas::outline(UiRect rect, EditorColor color) const {
    if (rect.width <= 0 || rect.height <= 0) return;
    if (rect.x >= clip_.x && rect.y >= clip_.y && rect.x + rect.width <= clip_.x + clip_.width &&
        rect.y + rect.height <= clip_.y + clip_.height) {
        inner_.outline(rect, color);
        return;
    }
    // Partly outside: draw the visible parts of its edges, not a new edge along the clip.
    const int right = rect.x + rect.width - 1;
    const int bottom = rect.y + rect.height - 1;
    line(rect.x, rect.y, right, rect.y, color, 1);
    line(rect.x, bottom, right, bottom, color, 1);
    line(rect.x, rect.y, rect.x, bottom, color, 1);
    line(right, rect.y, right, bottom, color, 1);
}

void RectClipCanvas::line(int x1, int y1, int x2, int y2, EditorColor color, int width) const {
    // Canvases widen a line by up to (width - 1) / 2 pixels before and width / 2 after
    // its centre, so the centre line is clipped to a box inset by that much.
    const int stroke = std::max(1, width);
    float ax = static_cast<float>(x1);
    float ay = static_cast<float>(y1);
    float bx = static_cast<float>(x2);
    float by = static_cast<float>(y2);
    if (!clip_segment_to_box(ax, ay, bx, by,
                             static_cast<float>(clip_.x + (stroke - 1) / 2),
                             static_cast<float>(clip_.y + (stroke - 1) / 2),
                             static_cast<float>(clip_.x + clip_.width - 1 - stroke / 2),
                             static_cast<float>(clip_.y + clip_.height - 1 - stroke / 2))) return;
    inner_.line(static_cast<int>(std::lround(ax)), static_cast<int>(std::lround(ay)),
                static_cast<int>(std::lround(bx)), static_cast<int>(std::lround(by)), color, width);
}

void RectClipCanvas::text(int x, int y, std::string_view value, EditorColor color) const {
    // Glyphs cannot be cut, so text that would cross the top or bottom is dropped and text
    // that would run past the right edge is elided to fit (labels of objects partly off
    // screen, and long labels anchored near the edge, used to spill onto the panels).
    const int right = clip_.x + clip_.width;
    if (x < clip_.x || x >= right) return;
    if (y - kEditorTextAscent < clip_.y || y + kEditorTextDescent > clip_.y + clip_.height) return;
    const std::string fitted = elide_text_to_width(inner_, value, right - x);
    if (!fitted.empty()) inner_.text(x, y, fitted, color);
}

void render_native_editor(const IEditorCanvas& painter, NativeEditorController& controller, int width, int height) {
    const bool highContrast = controller.workspace().preferences().highContrast;
    const auto theme = std::get<std::string>(controller.workspace().settings().value("editor.theme"));
    const bool light = theme == "light" || (theme == "system" && controller.system_theme_light());
    const EditorColor background = highContrast ? rgb(0,0,0) : light ? rgb(243,245,248) : rgb(24,27,32);
    const EditorColor panel = highContrast ? rgb(12,12,12) : light ? rgb(255,255,255) : rgb(34,38,45);
    const EditorColor panel2 = highContrast ? rgb(24,24,24) : light ? rgb(228,233,240) : rgb(42,47,56);
    const EditorColor border = highContrast ? rgb(255,255,255) : light ? rgb(130,140,154) : rgb(76,83,96);
    const EditorColor text = highContrast || !light ? rgb(235,238,244) : rgb(25,32,44);
    const EditorColor muted = highContrast ? rgb(210,210,210) : light ? rgb(78,89,105) : rgb(160,168,181);
    const EditorColor accent = rgb(64,147,255);

    painter.fill({0,0,width,height}, background);
    const NativeEditorLayout& layout = controller.layout();
    painter.fill(layout.menuBar, panel2);
    painter.fill(layout.toolbar, panel);
    painter.fill(layout.hierarchy, panel);
    painter.fill(layout.inspector, panel);
    painter.fill(layout.bottomPanel, panel2);
    painter.fill(layout.statusBar, panel);
    painter.outline(layout.hierarchy, border);
    painter.outline(layout.viewport, border);
    painter.outline(layout.inspector, border);
    painter.outline(layout.bottomPanel, border);


    const int desiredMenuWidth = 78;
    const int menuWidth = std::max(1, std::min(desiredMenuWidth, width / static_cast<int>(kMenuBarNames.size())));
    static constexpr std::array<std::string_view, 8> kCompactMenuNames{
        "File","Edit","New","View","Tools","Build","Win","Help"};
    for (std::size_t index = 0; index < kMenuBarNames.size(); ++index) {
        const int x = static_cast<int>(index) * menuWidth;
        if (controller.open_menu() && *controller.open_menu() == kMenuBarNames[index])
            painter.fill({x,0,menuWidth,layout.menuBar.height}, accent);
        const std::string_view label = menuWidth < 64 ? kCompactMenuNames[index] : kMenuBarNames[index];
        painter.text(x + (menuWidth < 64 ? 5 : 10), layout.menuBar.height - 9, label, text);
    }

    std::vector<ChromeTooltip> tooltips;
    const bool chromeHoverAllowed = !controller.open_menu() && !controller.context_menu().open;
    for (std::size_t index = 0; index < layout.toolbarButtons.size(); ++index) {
        const UiRect rect = layout.toolbarButtons[index];
        const auto tool = static_cast<EditorToolId>(index);
        const EditorToolInfo& info = editor_tool_info(tool);
        const bool selected = controller.active_tool() == tool;
        painter.fill(rect, selected ? accent : panel2);
        painter.outline(rect, selected ? rgb(190,220,255) : border);
        // Full name, then the short name, then whatever of the short name fits; never past the
        // button. The tooltip always has the full name, shortcut and description.
        const int room = rect.width - 8;
        std::string label(info.name);
        if (painter.text_width(label) > room) label = std::string(info.shortName);
        if (painter.text_width(label) > room) label = elide_text_to_width(painter, info.shortName, room);
        if (!label.empty()) {
            const int labelX = rect.x + std::max(4, (rect.width - painter.text_width(label)) / 2);
            painter.text(labelX, rect.y + rect.height / 2 + 5, label, text);
        }
        if (chromeHoverAllowed && rect.contains(controller.hover_x(), controller.hover_y())) {
            const std::string key = controller.tool_shortcut_text(tool);
            tooltips.push_back({rect,
                                {std::string(info.name) + (key.empty() ? "  (no shortcut)" : "  (" + key + ")"),
                                 std::string(info.description),
                                 "Acts on: " + std::string(info.accepts)}});
        }
    }
    const std::string mode = mode_name(controller.workspace().mode());
    painter.text(width - painter.text_width(mode) - 18, layout.toolbar.y + layout.toolbar.height / 2 + 5,
                 mode, controller.workspace().mode() == EditorMode::Edit ? rgb(102,220,144) : rgb(255,191,74));

    if (layout.hierarchy.width > 0) {
        painter.fill(layout.hierarchyFilterBox, panel2);
        painter.outline(layout.hierarchyFilterBox, border);
        const bool filtering = controller.text_edit().kind == TextEditKind::HierarchyFilter;
        const std::string filterText = filtering ? controller.text_edit().buffer + "_"
                                                   : (controller.hierarchy_filter().empty()
                                                          ? std::string("Filter...")
                                                          : std::string(controller.hierarchy_filter()));
        painter.text(layout.hierarchyFilterBox.x + 6, layout.hierarchyFilterBox.y + layout.hierarchyFilterBox.height - 6,
                     filterText, filtering || !controller.hierarchy_filter().empty() ? text : muted);
        const auto order = controller.hierarchy_order();
        for (std::size_t index = 0; index < order.size() && index < layout.hierarchyRows.size(); ++index) {
            const UiRect row = layout.hierarchyRows[index];
            const EditorObject* object = controller.workspace().document().find_object(order[index]);
            if (!object) continue;
            const bool selected = controller.workspace().is_selected(object->id);
            const bool dragTarget = controller.hierarchy_drag().active && controller.hierarchy_drag().hoverTarget &&
                                     *controller.hierarchy_drag().hoverTarget == object->id;
            if (dragTarget) painter.fill(row, rgb(90,140,90));
            else if (selected) painter.fill(row, rgb(55,92,140));
            std::string prefix = object->flags.visible ? "[x] " : "[ ] ";
            if (object->prefabLink) prefix += "P ";
            else if (object->attachment) prefix += "> ";
            const bool renaming = controller.text_edit().kind == TextEditKind::ObjectName &&
                                   controller.text_edit().objectId == object->id;
            const std::string label = renaming ? controller.text_edit().buffer + "_" : (prefix + object->name);
            painter.text(row.x + 8, row.y + row.height - 7, label, renaming ? accent : (object->flags.locked ? muted : text));
        }
        if (controller.hierarchy_drag().active && !controller.hierarchy_drag().hoverTarget) {
            painter.text(layout.hierarchy.x + 10, layout.hierarchy.y + layout.hierarchy.height - 10,
                         "Release to move to root", muted);
        }
    }

    const UiRect viewport = layout.viewport;
    // Everything painted into the 3D viewport goes through a clip: the voxel, frustum, box and
    // grid draw lists keep items out to 1.2x the viewport, which would otherwise land on the
    // outliner, inspector and bottom panel next to it.
    const RectClipCanvas viewportPainter(painter, viewport);
    viewportPainter.fill(viewport, highContrast ? rgb(0,0,0) : rgb(19,23,29));
    if (controller.viewport_settings().showGrid) {
        for (int i = -20; i <= 20; ++i) {
            const ScreenPoint a = project_world_to_screen(controller.camera(), viewport, {static_cast<float>(i),0,-20});
            const ScreenPoint b = project_world_to_screen(controller.camera(), viewport, {static_cast<float>(i),0,20});
            const ScreenPoint c = project_world_to_screen(controller.camera(), viewport, {-20,0,static_cast<float>(i)});
            const ScreenPoint d = project_world_to_screen(controller.camera(), viewport, {20,0,static_cast<float>(i)});
            const EditorColor gridColor = i == 0 ? rgb(92,106,126) : rgb(45,51,61);
            if (a.visible && b.visible) viewportPainter.line(static_cast<int>(a.x),static_cast<int>(a.y),static_cast<int>(b.x),static_cast<int>(b.y),gridColor);
            if (c.visible && d.visible) viewportPainter.line(static_cast<int>(c.x),static_cast<int>(c.y),static_cast<int>(d.x),static_cast<int>(d.y),gridColor);
        }
    }

    const auto& drawItems = controller.draw_items();
    for (const EditorVoxelDrawItem& item : drawItems) {
        const Float4 base = editor_material_display_color(controller.materials(), item.material);
        float shade = std::clamp(1.1F - item.depth * 0.018F, 0.42F, 1.0F);
        if (item.selected) shade = std::min(1.0F, shade + 0.14F);
        const EditorColor voxelColor = rgb(byte(base.x * shade), byte(base.y * shade), byte(base.z * shade));
        const int radius = std::max(1, static_cast<int>(item.pixelRadius));
        viewportPainter.fill({static_cast<int>(item.screenX) - radius, static_cast<int>(item.screenY) - radius,
                              radius * 2 + 1, radius * 2 + 1}, voxelColor);
        if (item.selected && radius >= 3)
            viewportPainter.outline({static_cast<int>(item.screenX) - radius, static_cast<int>(item.screenY) - radius,
                                     radius * 2 + 1, radius * 2 + 1}, rgb(150,205,255));
        if (item.anchored && controller.viewport_settings().showAnchors) {
            viewportPainter.line(static_cast<int>(item.screenX)-3,static_cast<int>(item.screenY),
                                 static_cast<int>(item.screenX)+3,static_cast<int>(item.screenY),rgb(255,218,72),2);
            viewportPainter.line(static_cast<int>(item.screenX),static_cast<int>(item.screenY)-3,
                                 static_cast<int>(item.screenX),static_cast<int>(item.screenY)+3,rgb(255,218,72),2);
        }
    }

    // The software editor uses a bounded text proxy while production rendering consumes the
    // same .dtext asset through Text3DGpuCache and the Slug HLSL face passes. This proxy keeps
    // selection, framing, and authoring usable on systems without a physical graphics backend.
    const auto textItems = controller.text3d_draw_items();
    for (const EditorText3DDrawItem& item : textItems) {
        const EditorObject* object = controller.workspace().document().find_object(item.objectId);
        if (!object || !object->text3d) continue;
        const Float4 face = object->text3d->style.faceColor;
        const Float4 side = object->text3d->style.sideColor;
        const float shade = std::clamp(1.08F - item.depth * 0.012F, 0.48F, 1.0F);
        const EditorColor faceColor = rgb(byte(face.x * shade), byte(face.y * shade), byte(face.z * shade));
        const EditorColor sideColor = rgb(byte(side.x * shade), byte(side.y * shade), byte(side.z * shade));
        const int extrusionOffset = std::clamp(item.screenBounds.height / 14, 2, 7);
        UiRect sideRect = item.screenBounds;
        sideRect.x += extrusionOffset;
        sideRect.y += extrusionOffset;
        viewportPainter.fill(sideRect, sideColor);
        viewportPainter.fill(item.screenBounds, faceColor);
        viewportPainter.outline(item.screenBounds, item.selected ? rgb(150,205,255) : border);
        const int available = std::max(0, item.screenBounds.width - 10);
        std::string label = item.text;
        while (!label.empty() && viewportPainter.text_width(label) > available) label.pop_back();
        if (label.size() < item.text.size() && label.size() > 3U) {
            label.resize(label.size() - 3U);
            label += "...";
        }
        if (!label.empty() && item.screenBounds.height >= 13) {
            const int labelX = item.screenBounds.x + std::max(5, (item.screenBounds.width - viewportPainter.text_width(label)) / 2);
            const int labelY = item.screenBounds.y + item.screenBounds.height / 2 + 4;
            viewportPainter.text(labelX, labelY, label, rgb(244,246,250));
        }
        if (item.selected) {
            viewportPainter.text(item.screenBounds.x + 4, item.screenBounds.y + 12,
                                 "Slug M" + std::to_string(item.faceMaterialId) + "/" +
                                 std::to_string(item.sideMaterialId), rgb(220,235,255));
        }
    }

    // Gabor volumes use a bounded translucent proxy in the software editor. The production
    // compute path consumes the same .dgabor primitive data through GaborGpuCache.
    const auto gaborItems = controller.gabor_volume_draw_items();
    for (const EditorGaborVolumeDrawItem& item : gaborItems) {
        const float shade = std::clamp(1.05F-item.depth*0.01F, 0.35F, 1.0F);
        const EditorColor tint = rgb(byte(item.albedoTint.x*shade),
                                     byte(item.albedoTint.y*shade),
                                     byte(item.albedoTint.z*shade));
        UiRect inner = item.screenBounds;
        const int inset = std::max(2, std::min(inner.width, inner.height)/10);
        viewportPainter.outline(item.screenBounds, item.selected ? rgb(150,205,255) : tint);
        for (int ring = 1; ring <= 3; ++ring) {
            const int d = inset*ring;
            if (inner.width <= d*2 || inner.height <= d*2) break;
            viewportPainter.outline({inner.x+d, inner.y+d, inner.width-d*2, inner.height-d*2}, tint);
        }
        if (item.screenBounds.height >= 24) {
            viewportPainter.text(item.screenBounds.x+5, item.screenBounds.y+14,
                                 "Gabor " + std::to_string(item.primitiveCount) +
                                 " L" + std::to_string(item.maximumLodLevel),
                                 item.selected ? rgb(220,235,255) : tint);
        }
    }

    for (EditorObjectId selectedId : controller.workspace().selected_objects()) {
        if (const EditorObject* selected = controller.workspace().document().find_object(selectedId)) {
            if (controller.viewport_settings().showObjectBounds)
                draw_world_box(viewportPainter, controller, object_world_bounds(*selected), rgb(100,188,255));
            if (controller.viewport_settings().showCollision)
                draw_world_box(viewportPainter, controller, object_world_bounds(*selected), rgb(255,140,58));
        }
    }
    if (controller.hover_pick()) {
        const ScreenPoint hover = project_world_to_screen(controller.camera(), viewport, controller.hover_pick()->worldPosition);
        if (hover.visible) {
            viewportPainter.outline({static_cast<int>(hover.x)-7,static_cast<int>(hover.y)-7,15,15},rgb(255,255,255));
        }
    }
    if (const auto point = controller.placement_target()) {
        const ScreenPoint target = project_world_to_screen(controller.camera(), viewport, *point);
        if (target.visible) {
            const int x = static_cast<int>(target.x), y = static_cast<int>(target.y);
            viewportPainter.line(x - 9, y, x + 9, y, rgb(255,219,89), 2);
            viewportPainter.line(x, y - 9, x, y + 9, rgb(255,219,89), 2);
        }
    }
    if (const auto bounds = controller.scale_preview_bounds())
        draw_world_box(viewportPainter, controller, *bounds, rgb(255,219,89));
    if (controller.voxel_boolean().active()) {
        // Boolean preview (ART-060): label A/B bounds, then mark every cell the commit would
        // change in the target grid. Nothing in the document changes until Enter.
        const EditorDocument& document = controller.workspace().document();
        if (const EditorObject* target = document.find_object(controller.voxel_boolean().target()))
            draw_world_box(viewportPainter, controller, object_world_bounds(*target), rgb(96,170,255));
        for (const EditorObjectId operandId : controller.voxel_boolean().operands())
            if (const EditorObject* operand = document.find_object(operandId))
                draw_world_box(viewportPainter, controller, object_world_bounds(*operand), rgb(255,166,64));
        const auto label_object = [&](EditorObjectId id, const std::string& label, EditorColor color) {
            const EditorObject* object = document.find_object(id);
            if (!object) return;
            const EditorObjectBounds bounds = object_world_bounds(*object);
            if (!bounds.valid) return;
            const ScreenPoint corner = project_world_to_screen(controller.camera(), viewport,
                {bounds.minimum.x, bounds.maximum.y, bounds.minimum.z});
            if (!corner.visible) return;
            const int x = static_cast<int>(corner.x);
            const int y = static_cast<int>(corner.y) - 6;
            const int w = viewportPainter.text_width(label) + 8;
            viewportPainter.fill({x - 2, y - 13, w, 17}, rgb(16,20,26));
            viewportPainter.outline({x - 2, y - 13, w, 17}, color);
            viewportPainter.text(x + 2, y, label, color);
        };
        label_object(controller.voxel_boolean().target(), "A target", rgb(150,200,255));
        const auto& operandIds = controller.voxel_boolean().operands();
        for (std::size_t i = 0; i < operandIds.size(); ++i)
            label_object(operandIds[i], operandIds.size() == 1 ? std::string("B operand") : "B" + std::to_string(i + 1),
                         rgb(255,190,110));
        for (const VoxelBooleanPreviewMarker& marker : controller.voxel_boolean_preview_markers()) {
            const int radius = std::max(2, static_cast<int>(marker.pixelRadius));
            const UiRect cell{static_cast<int>(marker.screenX) - radius, static_cast<int>(marker.screenY) - radius,
                              radius * 2 + 1, radius * 2 + 1};
            switch (marker.kind) {
            case VoxelBooleanPreviewMarker::Kind::Added:
                viewportPainter.fill(cell, rgb(70,205,120));
                viewportPainter.outline(cell, rgb(190,255,205));
                break;
            case VoxelBooleanPreviewMarker::Kind::Removed:
                viewportPainter.outline(cell, rgb(255,82,82));
                viewportPainter.line(cell.x, cell.y, cell.x + cell.width - 1, cell.y + cell.height - 1, rgb(255,82,82));
                break;
            case VoxelBooleanPreviewMarker::Kind::Repainted:
                viewportPainter.outline(cell, rgb(255,206,72));
                break;
            case VoxelBooleanPreviewMarker::Kind::Overlap:
                viewportPainter.outline(cell, rgb(96,220,255));
                break;
            }
        }
        const std::vector<std::string> lines = controller.voxel_boolean_preview_lines();
        int panelWidth = 0;
        for (const std::string& line : lines) panelWidth = std::max(panelWidth, viewportPainter.text_width(line));
        // Keep clear of the viewport hint line (top) and the camera preview inset (top right);
        // long diagnostics are elided here and stay complete in the status/console output.
        const int panelMaxWidth = viewport.width > 520 ? viewport.width - 200 : viewport.width - 16;
        panelWidth = std::min(panelWidth + 20, std::max(120, panelMaxWidth));
        const int lineHeight = 17;
        const UiRect panel{viewport.x + 8, viewport.y + 28, panelWidth, static_cast<int>(lines.size()) * lineHeight + 10};
        viewportPainter.fill(panel, rgb(18,23,31));
        viewportPainter.outline(panel, controller.voxel_boolean().can_commit() ? rgb(96,170,255) : rgb(255,110,90));
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const std::string& line = lines[i];
            EditorColor color = i == 0 ? rgb(235,242,250) : rgb(190,200,214);
            if (line.starts_with("Cannot commit")) color = rgb(255,130,110);
            else if (line.starts_with("Warning")) color = rgb(255,206,72);
            else if (line.starts_with("A target")) color = rgb(150,200,255);
            else if (line.starts_with("B")) color = rgb(255,190,110);
            viewportPainter.text(panel.x + 10, panel.y + 18 + static_cast<int>(i) * lineHeight,
                                 elide_text_to_width(viewportPainter, line, panel.width - 20), color);
        }
    }
    for (const GizmoScreenAxis& axis : controller.gizmo_axes()) {
        if (!axis.start.visible || !axis.end.visible) continue;
        const EditorColor axisColor = axis.axis == 1 ? rgb(244,79,83) : axis.axis == 2 ? rgb(84,220,121) : rgb(72,139,255);
        viewportPainter.line(static_cast<int>(axis.start.x),static_cast<int>(axis.start.y),
                             static_cast<int>(axis.end.x),static_cast<int>(axis.end.y),axisColor,4);
    }

    if (controller.sprite_level_playing() && controller.sprite_level() &&
        controller.sprite_level_presentation()) {
        const gameplay::SpriteVerticalSlicePresentationFrame frame =
            controller.sprite_level_presentation()->build_frame(*controller.sprite_level());
        SpriteRenderList diagnosticRender = frame.sprites;
        const std::size_t particleFirst = diagnosticRender.items.size();
        diagnosticRender.items.insert(diagnosticRender.items.end(),
                                      frame.particles.renderList.items.begin(),
                                      frame.particles.renderList.items.end());
        for (SpriteBatch batch : frame.particles.renderList.batches) {
            batch.firstItem += particleFirst;
            diagnosticRender.batches.push_back(std::move(batch));
        }
        const SpriteAsset* diagnosticAsset = controller.sprite_level_presentation()->sprites().asset(1U);
        std::vector<SpriteAssetCatalogEntry> diagnosticAssets;
        if (diagnosticAsset != nullptr) diagnosticAssets.push_back({1U, diagnosticAsset});
        std::vector<std::string> residentTextures;
        std::vector<std::string> residentPalettes;
        if (diagnosticAsset != nullptr) {
            residentTextures.push_back(diagnosticAsset->textureAsset);
            if (!diagnosticAsset->paletteAsset.empty()) residentPalettes.push_back(diagnosticAsset->paletteAsset);
        }
        if (!frame.particles.renderList.items.empty())
            residentTextures.push_back(frame.particles.renderList.items.front().textureAsset);
        SpriteDiagnosticsInput diagnosticInput;
        diagnosticInput.cameraName = "Original Sprite Level";
        diagnosticInput.sprites = &diagnosticRender;
        diagnosticInput.tiles = frame.tiles;
        diagnosticInput.assets = diagnosticAssets;
        diagnosticInput.residentTextures = residentTextures;
        diagnosticInput.residentPalettes = residentPalettes;
        diagnosticInput.presentation.logicalWidth = 320U;
        diagnosticInput.presentation.logicalHeight = 180U;
        diagnosticInput.presentation.pixelsPerWorldUnit = 16.0F;
        diagnosticInput.logicalWidth = 320U;
        diagnosticInput.logicalHeight = 180U;
        diagnosticInput.cameraOrigin = {(frame.camera.center.x + frame.screenShakePixels) / 16.0F,
                                        frame.camera.center.y / 16.0F, 0.0F};
        diagnosticInput.tileCameraCenterPixels = {frame.camera.center.x + frame.screenShakePixels,
                                                  frame.camera.center.y};
        diagnosticInput.profile = SpriteBudgetProfile::Retro16Bit;
        if (diagnosticAsset != nullptr) {
            diagnosticInput.textureBytes[diagnosticAsset->textureAsset] =
                static_cast<std::uint64_t>(diagnosticAsset->textureWidth) *
                diagnosticAsset->textureHeight * 4U;
            if (!diagnosticAsset->paletteAsset.empty())
                diagnosticInput.paletteBytes[diagnosticAsset->paletteAsset] = 1024U;
        }
        const SpriteDiagnosticsReport diagnosticReport = build_sprite_diagnostics(diagnosticInput);
        viewportPainter.fill(viewport, rgb(18, 30, 48));
        const int horizon = viewport.y + viewport.height * 2 / 3;
        viewportPainter.fill({viewport.x, horizon, viewport.width, viewport.y + viewport.height - horizon},
                             rgb(29, 52, 45));
        for (int band = 0; band < 5; ++band) {
            const int offset = static_cast<int>(frame.parallaxOffset.x * static_cast<float>(band + 1)) % 180;
            const int y = viewport.y + 42 + band * 24;
            for (int x = viewport.x - 180 + offset; x < viewport.x + viewport.width; x += 180)
                viewportPainter.fill({x, y, 110, 10 + band * 3}, rgb(31 + band * 7, 55 + band * 6, 80 + band * 4));
        }
        const float cameraX = frame.camera.center.x + frame.screenShakePixels;
        const float cameraY = frame.camera.center.y;
        const auto pixel_to_screen = [&](float x, float y) {
            return std::pair<int, int>{
                viewport.x + viewport.width / 2 + static_cast<int>(std::lround(x - cameraX)),
                viewport.y + viewport.height / 2 - static_cast<int>(std::lround(y - cameraY))};
        };
        for (const TileRenderItem& tile : frame.tiles) {
            const auto [left, bottom] = pixel_to_screen(tile.worldBounds.min.x, tile.worldBounds.min.y);
            const auto [right, top] = pixel_to_screen(tile.worldBounds.max_x(), tile.worldBounds.max_y());
            const int x = std::min(left, right);
            const int y = std::min(top, bottom);
            const int w = std::max(1, std::abs(right - left));
            const int h = std::max(1, std::abs(bottom - top));
            const std::uint8_t shade = static_cast<std::uint8_t>(70U + (tile.atlasIndex * 17U) % 80U);
            viewportPainter.fill({x, y, w, h}, rgb(shade, static_cast<std::uint8_t>(shade + 25U), 72));
            if (w >= 8 && h >= 8) viewportPainter.outline({x, y, w, h}, rgb(42, 68, 52));
        }
        const auto draw_sprite_items = [&](const SpriteRenderList& list, bool particles) {
            for (const SpriteDrawItem& item : list.items) {
                float minX = item.vertices[0].position.x;
                float maxX = minX;
                float minY = item.vertices[0].position.y;
                float maxY = minY;
                for (const SpriteVertex& vertex : item.vertices) {
                    minX = std::min(minX, vertex.position.x);
                    maxX = std::max(maxX, vertex.position.x);
                    minY = std::min(minY, vertex.position.y);
                    maxY = std::max(maxY, vertex.position.y);
                }
                const auto [left, bottom] = pixel_to_screen(minX * 16.0F, minY * 16.0F);
                const auto [right, top] = pixel_to_screen(maxX * 16.0F, maxY * 16.0F);
                UiRect rect{std::min(left, right), std::min(top, bottom),
                            std::max(2, std::abs(right - left)), std::max(2, std::abs(bottom - top))};
                EditorColor color = particles ? rgb(255, 203, 70) : rgb(220, 230, 240);
                if (!particles) {
                    switch (item.owner) {
                        case 1001U: color = rgb(72, 190, 245); break;
                        case 1002U: color = rgb(238, 92, 84); break;
                        case 1003U: color = rgb(255, 222, 90); break;
                        case 1004U: color = rgb(255, 191, 55); break;
                        case 1005U: color = rgb(85, 230, 155); break;
                        case 1006U: color = rgb(220, 72, 62); break;
                        default: break;
                    }
                }
                viewportPainter.fill(rect, color);
                if (!particles) {
                    EditorColor outlineColor = rgb(18, 24, 31);
                    if (controller.sprite_diagnostics_open()) {
                        const auto layer = std::find_if(diagnosticReport.sortingLayers.begin(),
                            diagnosticReport.sortingLayers.end(), [&](const SpriteSortingLayerDiagnostic& entry) {
                                return entry.sortingLayer == item.sortingLayer;
                            });
                        if (layer != diagnosticReport.sortingLayers.end())
                            outlineColor = rgb(layer->displayColor[0], layer->displayColor[1], layer->displayColor[2]);
                    }
                    viewportPainter.outline(rect, outlineColor);
                }
            }
        };
        draw_sprite_items(frame.sprites, false);
        draw_sprite_items(frame.particles.renderList, true);
        if (controller.sprite_diagnostics_open()) {
            const UiRect diagnostics{viewport.x + viewport.width - 338, viewport.y + 10, 328, 238};
            viewportPainter.fill(diagnostics, rgb(11, 16, 23));
            viewportPainter.outline(diagnostics, rgb(87, 176, 245));
            viewportPainter.text(diagnostics.x + 10, diagnostics.y + 20, "SPRITE DIAGNOSTICS", rgb(220, 235, 248));
            viewportPainter.text(diagnostics.x + 10, diagnostics.y + 40,
                                 "Draws " + std::to_string(diagnosticReport.drawCallCount) +
                                 "  Batches " + std::to_string(diagnosticReport.batchCount) +
                                 "  Breaks " + std::to_string(diagnosticReport.batchBreaks.size()), muted);
            viewportPainter.text(diagnostics.x + 10, diagnostics.y + 58,
                                 "Sprites " + std::to_string(diagnosticReport.spriteCount) +
                                 "  Tiles " + std::to_string(diagnosticReport.tileCount) +
                                 "  Pixels " + std::to_string(diagnosticReport.visiblePixelCount), muted);
            viewportPainter.text(diagnostics.x + 10, diagnostics.y + 76,
                                 "Textures " + std::to_string(diagnosticReport.textureCount) +
                                 "  Palettes " + std::to_string(diagnosticReport.paletteCount) +
                                 "  Missing " + std::to_string(diagnosticReport.referenceIssues.size()), muted);
            viewportPainter.text(diagnostics.x + 10, diagnostics.y + 94,
                                 "Snap violations " + std::to_string(diagnosticReport.pixelSnapViolations.size()) +
                                 "  Budget warnings " + std::to_string(diagnosticReport.budgetViolations.size()), muted);
            if (!diagnosticReport.atlases.empty()) {
                const SpriteAtlasDiagnostic& atlas = diagnosticReport.atlases.front();
                const int occupancy = static_cast<int>(std::lround(atlas.occupancy * 100.0));
                const int fragmentation = static_cast<int>(std::lround(atlas.packingFragmentation * 100.0));
                viewportPainter.text(diagnostics.x + 10, diagnostics.y + 112,
                                     "Atlas " + std::to_string(occupancy) + "% used  " +
                                     std::to_string(fragmentation) + "% fragmented", muted);
            }
            const int heatX = diagnostics.x + 10;
            const int heatY = diagnostics.y + 126;
            const int heatW = 160;
            const int heatH = 90;
            viewportPainter.fill({heatX, heatY, heatW, heatH}, rgb(6, 8, 12));
            const std::uint32_t sourceW = diagnosticReport.overdraw.width;
            const std::uint32_t sourceH = diagnosticReport.overdraw.height;
            for (int by = 0; by < heatH; by += 5) {
                for (int bx = 0; bx < heatW; bx += 5) {
                    const std::uint32_t sx = sourceW == 0U ? 0U :
                        std::min(sourceW - 1U, static_cast<std::uint32_t>(bx) * sourceW / static_cast<std::uint32_t>(heatW));
                    const std::uint32_t sy = sourceH == 0U ? 0U :
                        std::min(sourceH - 1U, static_cast<std::uint32_t>(by) * sourceH / static_cast<std::uint32_t>(heatH));
                    if (sourceW == 0U || sourceH == 0U) continue;
                    const std::size_t offset = (static_cast<std::size_t>(sy) * sourceW + sx) * 4U;
                    const unsigned alpha = std::to_integer<unsigned>(diagnosticReport.overdraw.heatmapRgba8[offset + 3U]);
                    if (alpha == 0U) continue;
                    viewportPainter.fill({heatX + bx, heatY + by, 5, 5},
                                rgb(std::to_integer<unsigned>(diagnosticReport.overdraw.heatmapRgba8[offset]),
                                    std::to_integer<unsigned>(diagnosticReport.overdraw.heatmapRgba8[offset + 1U]),
                                    std::to_integer<unsigned>(diagnosticReport.overdraw.heatmapRgba8[offset + 2U])));
                }
            }
            viewportPainter.outline({heatX, heatY, heatW, heatH}, rgb(70, 80, 92));
            viewportPainter.text(diagnostics.x + 182, diagnostics.y + 145,
                                 "Max overdraw " + std::to_string(diagnosticReport.overdraw.maximumOverdraw), muted);
            viewportPainter.text(diagnostics.x + 182, diagnostics.y + 164,
                                 "Fragments " + std::to_string(diagnosticReport.fragmentCount), muted);
            viewportPainter.text(diagnostics.x + 182, diagnostics.y + 183,
                                 "Sorting layers " + std::to_string(diagnosticReport.sortingLayers.size()), muted);
            viewportPainter.text(diagnostics.x + 182, diagnostics.y + 202, "Profile: Retro16Bit", muted);
        }
        const UiRect hud{viewport.x + 12, viewport.y + 12, 310, 32};
        viewportPainter.fill(hud, rgb(10, 18, 28));
        viewportPainter.outline(hud, rgb(90, 160, 225));
        viewportPainter.text(hud.x + 10, hud.y + 21, frame.hudText, text);
        viewportPainter.text(viewport.x + 12, viewport.y + viewport.height - 14,
                             "SPRITE LEVEL  |  A/D or arrows move  Space jump  X fire  R restart  Esc stop", muted);
    }

    const auto settingBool = [&](std::string_view id, bool fallback) {
        const SettingValue value = controller.workspace().settings().value(id);
        if (const auto* item = std::get_if<bool>(&value)) return *item;
        return fallback;
    };
    const auto settingFloat = [&](std::string_view id, float fallback) {
        const SettingValue value = controller.workspace().settings().value(id);
        if (const auto* item = std::get_if<double>(&value)) return static_cast<float>(*item);
        return fallback;
    };
    std::optional<UiRect> cameraPreviewRect;
    if (controller.selected_camera_rig()) {
        const camera::CameraRig* rig = controller.camera_director().find_rig(*controller.selected_camera_rig());
        if (rig && settingBool("camera.show_frustum", true)) {
            EditorCamera rigCamera;
            apply_camera_pose(rigCamera, rig->authoredPose);
            const CameraBasis rigBasis = camera_basis(rigCamera);
            const float distance = std::clamp(length(subtract(rigCamera.target, rigCamera.position)), 1.5F, 12.0F);
            const float aspect = static_cast<float>(std::max(1, viewport.width)) /
                                 static_cast<float>(std::max(1, viewport.height));
            const float halfHeight = rigCamera.projection == EditorProjection::Perspective
                ? std::tan(editor_camera_vertical_fov(rigCamera, aspect) * 0.5F) * distance
                : rigCamera.orthographicHeight * 0.5F;
            const float halfWidth = halfHeight * aspect;
            const Float3 center = add(rigCamera.position, multiply(rigBasis.forward, distance));
            const std::array<Float3, 4> corners{
                add(add(center, multiply(rigBasis.right, -halfWidth)), multiply(rigBasis.up, halfHeight)),
                add(add(center, multiply(rigBasis.right, halfWidth)), multiply(rigBasis.up, halfHeight)),
                add(add(center, multiply(rigBasis.right, halfWidth)), multiply(rigBasis.up, -halfHeight)),
                add(add(center, multiply(rigBasis.right, -halfWidth)), multiply(rigBasis.up, -halfHeight))};
            const ScreenPoint origin = project_world_to_screen(controller.camera(), viewport, rigCamera.position);
            std::array<ScreenPoint, 4> projected{};
            for (std::size_t i = 0; i < corners.size(); ++i) {
                projected[i] = project_world_to_screen(controller.camera(), viewport, corners[i]);
                if (origin.visible && projected[i].visible)
                    viewportPainter.line(static_cast<int>(origin.x), static_cast<int>(origin.y),
                                         static_cast<int>(projected[i].x), static_cast<int>(projected[i].y), rgb(255,190,80));
            }
            for (std::size_t i = 0; i < projected.size(); ++i) {
                const ScreenPoint& a = projected[i];
                const ScreenPoint& b = projected[(i + 1U) % projected.size()];
                if (a.visible && b.visible)
                    viewportPainter.line(static_cast<int>(a.x), static_cast<int>(a.y),
                                         static_cast<int>(b.x), static_cast<int>(b.y), rgb(255,190,80));
            }
        }
        if (rig && settingBool("camera.preview_selected", true)) {
            const float ratio = std::clamp(settingFloat("camera.preview_size", 0.25F), 0.1F, 0.75F);
            const int previewWidth = std::max(180, static_cast<int>(static_cast<float>(viewport.width) * ratio));
            const int previewHeight = std::max(110, previewWidth * 9 / 16);
            const UiRect preview{viewport.x + viewport.width - previewWidth - 14, viewport.y + 14,
                                 previewWidth, std::min(previewHeight, viewport.height - 28)};
            viewportPainter.fill(preview, rgb(12,16,22));
            EditorCamera previewCamera;
            apply_camera_pose(previewCamera, rig->authoredPose);
            EditorViewportSettings previewSettings = controller.viewport_settings();
            previewSettings.maximumDrawVoxels = std::min<std::size_t>(previewSettings.maximumDrawVoxels, 25000U);
            const auto& previewItems = controller.camera_preview_draw_items(previewCamera, preview, previewSettings);
            // The preview list has the same 1.2x margin around the preview rect.
            const RectClipCanvas previewPainter(viewportPainter, preview);
            for (const EditorVoxelDrawItem& item : previewItems) {
                const Float4 base = editor_material_display_color(controller.materials(), item.material);
                const float shade = std::clamp(1.08F - item.depth * 0.018F, 0.42F, 1.0F);
                const int radius = std::max(1, static_cast<int>(item.pixelRadius));
                previewPainter.fill({static_cast<int>(item.screenX) - radius, static_cast<int>(item.screenY) - radius,
                                     radius * 2 + 1, radius * 2 + 1},
                                    rgb(byte(base.x * shade), byte(base.y * shade), byte(base.z * shade)));
            }
            viewportPainter.outline(preview, accent);
            const UiRect titleBar{preview.x, preview.y, preview.width, 22};
            viewportPainter.fill(titleBar, rgb(28,43,63));
            // The rig name is elided first so the "Preview" tag stays; the full name is in a tooltip.
            const std::string tag = "  [Preview]";
            const int titleRoom = preview.width - 14;
            std::string title = rig->name + tag;
            bool elided = false;
            if (painter.text_width(title) > titleRoom) {
                const std::string name = elide_text_to_width(painter, rig->name, titleRoom - painter.text_width(tag));
                elided = true;
                title = name.empty() ? elide_text_to_width(painter, rig->name, titleRoom) : name + tag;
            }
            viewportPainter.text(preview.x + 7, preview.y + 16, title, text);
            cameraPreviewRect = preview;
            if (chromeHoverAllowed && elided && titleBar.contains(controller.hover_x(), controller.hover_y()))
                tooltips.push_back({titleBar, {rig->name, "Camera preview of the selected rig"}});
        }
    }
    const CinematicCameraOverlayState& cameraOverlays =
        controller.cinematic_camera_panel().overlays();
    const camera::CameraPostProcessProfile& overlayProfile =
        controller.cinematic_camera_panel().profile_for_scope(controller.selected_camera_rig());
    const int insetX = std::max(12, viewport.width / 20);
    const int insetY = std::max(10, viewport.height / 20);
    if (cameraOverlays.safeFrames) {
        viewportPainter.outline({viewport.x + insetX, viewport.y + insetY,
                                 viewport.width - insetX * 2, viewport.height - insetY * 2}, rgb(238, 214, 110));
        viewportPainter.outline({viewport.x + insetX * 2, viewport.y + insetY * 2,
                                 viewport.width - insetX * 4, viewport.height - insetY * 4}, rgb(198, 177, 86));
    }
    if (cameraOverlays.aspectMattes) {
        const float nativeAspect = static_cast<float>(std::max(1, viewport.width)) /
                                   static_cast<float>(std::max(1, viewport.height));
        const float targetAspect = camera::camera_framing_aspect_ratio(
            overlayProfile.cinematic.film, nativeAspect);
        if (targetAspect > nativeAspect) {
            const int contentHeight = std::clamp(
                static_cast<int>(static_cast<float>(viewport.width) / targetAspect), 1, viewport.height);
            const int matte = (viewport.height - contentHeight) / 2;
            viewportPainter.fill({viewport.x, viewport.y, viewport.width, matte}, rgb(3, 3, 4));
            viewportPainter.fill({viewport.x, viewport.y + viewport.height - matte, viewport.width, matte}, rgb(3, 3, 4));
        } else if (targetAspect < nativeAspect) {
            const int contentWidth = std::clamp(
                static_cast<int>(static_cast<float>(viewport.height) * targetAspect), 1, viewport.width);
            const int matte = (viewport.width - contentWidth) / 2;
            viewportPainter.fill({viewport.x, viewport.y, matte, viewport.height}, rgb(3, 3, 4));
            viewportPainter.fill({viewport.x + viewport.width - matte, viewport.y, matte, viewport.height}, rgb(3, 3, 4));
        }
    }
    if (cameraOverlays.focusPlanes) {
        const int y = viewport.y + viewport.height / 2;
        viewportPainter.line(viewport.x + insetX, y, viewport.x + viewport.width - insetX, y, rgb(90, 220, 170), 2);
        viewportPainter.text(viewport.x + insetX + 5, y - 7,
                             "FOCUS " + std::to_string(overlayProfile.focusDistanceMeters).substr(0, 6) + " m",
                             rgb(90, 220, 170));
    }
    if (cameraOverlays.splitDiopter) {
        const auto& split = overlayProfile.cinematic.splitDiopter;
        const float center = std::clamp(split.centerX, 0.0F, 1.0F);
        const int x = viewport.x + static_cast<int>(center * static_cast<float>(viewport.width));
        viewportPainter.line(x, viewport.y + insetY, x, viewport.y + viewport.height - insetY,
                             rgb(235, 130, 220), 2);
        viewportPainter.text(x + 6, viewport.y + insetY + 18, "SPLIT DIOPTER", rgb(235, 130, 220));
    }
    if (cameraOverlays.motionVectors) {
        for (int y = viewport.y + 70; y < viewport.y + viewport.height - 30; y += 72) {
            for (int x = viewport.x + 50; x < viewport.x + viewport.width - 50; x += 96)
                viewportPainter.line(x, y, x + 18, y - 7, rgb(100, 180, 255));
        }
        viewportPainter.text(viewport.x + 12, viewport.y + 42, "MOTION VECTORS", rgb(100, 180, 255));
    }
    if (cameraOverlays.exposurePreview) {
        viewportPainter.text(viewport.x + 12, viewport.y + 62,
                             "EXPOSURE PREVIEW  EV " + std::to_string(overlayProfile.exposure).substr(0, 5),
                             rgb(255, 195, 95));
    }
    if (cameraOverlays.compareUngraded) {
        const int x = viewport.x + viewport.width / 2;
        viewportPainter.line(x, viewport.y, x, viewport.y + viewport.height, rgb(245, 245, 245), 2);
        viewportPainter.text(viewport.x + 12, viewport.y + viewport.height - 12, "UNGRADED", text);
        viewportPainter.text(x + 12, viewport.y + viewport.height - 12, "GRADED", text);
    }

    {
        // Only the active tool's gestures, and only as many whole gestures as fit left of the
        // camera preview (they used to run underneath it). Everything else is behind the
        // "? Shortcuts" control in the corner.
        int hintRight = viewport.x + viewport.width - 12;
        if (cameraPreviewRect) hintRight = std::min(hintRight, cameraPreviewRect->x - 10);
        if (controller.play_session().active()) hintRight = std::min(hintRight, viewport.x + viewport.width - 286 - 22);
        const int hintX = viewport.x + 12;
        std::string hint;
        for (const std::string& gesture : controller.viewport_tool_gestures()) {
            const std::string candidate = hint.empty() ? gesture : hint + "   " + gesture;
            if (hintX + viewportPainter.text_width(candidate) > hintRight) break;
            hint = candidate;
        }
        if (!hint.empty()) viewportPainter.text(hintX, viewport.y + 20, hint, muted);
        const UiRect help = layout.viewportHelpButton;
        if (help.width > 0) {
            const bool hovered = help.contains(controller.hover_x(), controller.hover_y());
            viewportPainter.fill(help, hovered ? rgb(48,88,142) : rgb(28,34,44));
            viewportPainter.outline(help, border);
            const std::string label = elide_text_to_width(painter, "? Shortcuts", help.width - 10);
            viewportPainter.text(help.x + std::max(5, (help.width - painter.text_width(label)) / 2),
                                 help.y + help.height / 2 + 5, label, text);
            if (chromeHoverAllowed && hovered) {
                const std::string key = controller.workspace().shortcuts().display_binding("help.shortcuts");
                tooltips.push_back({help, {"Keyboard and mouse shortcuts" + (key.empty() ? std::string() : "  (" + key + ")"),
                                           "Every binding for the viewport and the active tool"}});
            }
        }
    }

    if (controller.play_session().active()) {
        const EditorPlaySession& session = controller.play_session();
        const EditorPlaySessionTelemetry& telemetry = session.telemetry();
        const bool paused = session.paused();
        const std::string sessionMode = session.mode() == EditorMode::Play ? "PLAY" : "SIMULATE";
        const std::string state = paused ? "PAUSED" : "RUNNING";
        const std::string cameraState = session.mode() == EditorMode::Play
            ? (controller.play_camera_possessed() ? "POSSESSED" : "EJECTED")
            : "EDITOR CAMERA";
        const int overlayWidth = 286;
        const int overlayHeight = session.hud().interactionPrompt.empty() && !session.hud().selectedTool
            ? 58 : 94;
        const UiRect overlayRect{
            viewport.x + viewport.width - overlayWidth - 12,
            viewport.y + 12,
            overlayWidth,
            overlayHeight};
        viewportPainter.fill(overlayRect, rgb(14, 20, 29));
        viewportPainter.outline(overlayRect, paused ? rgb(230, 174, 62) : accent);
        viewportPainter.text(overlayRect.x + 10, overlayRect.y + 19,
                             sessionMode + "  " + state + "  " + cameraState,
                             paused ? rgb(255, 216, 120) : text);
        viewportPainter.text(overlayRect.x + 10, overlayRect.y + 39,
                             "Tick " + std::to_string(telemetry.fixedTickCount) +
                                 "   Runtime objects " + std::to_string(telemetry.runtimeObjectCount),
                             muted);
        if (!session.hud().interactionPrompt.empty()) {
            viewportPainter.text(overlayRect.x + 10, overlayRect.y + 61,
                                 session.hud().interactionPrompt, text);
        }
        if (session.hud().selectedTool) {
            viewportPainter.text(overlayRect.x + 10, overlayRect.y + 81,
                                 "Tool: " + session.hud().selectedTool->label, muted);
        }
    }

    if (layout.inspector.width > 0) {
        painter.text(layout.inspector.x + 12, layout.inspector.y + 20, "INSPECTOR", muted);
        const int detailTop = layout.inspector.y + kInspectorHeaderHeight;
        const RectClipCanvas inspectorPainter(
            painter, {layout.inspector.x + 1, detailTop, layout.inspector.width - 2,
                      std::max(0, layout.inspectorContentClipY - detailTop)});
        // Detail lines are laid out from this origin; it moves up as the details scroll.
        const int inspectorTop = layout.inspector.y - layout.inspectorScroll;
        if (layout.inspectorScrollMax > 0) {
            const int track = std::max(1, layout.inspectorContentClipY - detailTop);
            const int thumb = std::max(16, track * track / (track + layout.inspectorScrollMax));
            const int thumbY = detailTop + (track - thumb) * layout.inspectorScroll / layout.inspectorScrollMax;
            painter.fill({layout.inspector.x + layout.inspector.width - 5, detailTop, 3, track}, panel2);
            painter.fill({layout.inspector.x + layout.inspector.width - 5, thumbY, 3, thumb}, muted);
        }
        if (controller.workspace().selected_object()) {
            if (const EditorObject* object = controller.workspace().document().find_object(*controller.workspace().selected_object())) {
                const bool renamingHere = controller.text_edit().kind == TextEditKind::ObjectName;
                inspectorPainter.text(layout.inspector.x + 12, inspectorTop + 48,
                             renamingHere ? controller.text_edit().buffer + "_" : object->name,
                             renamingHere ? accent : text);
                inspectorPainter.text(layout.inspector.x + 12, inspectorTop + 69,
                             "ID " + std::to_string(object->id) + "   Voxels " + std::to_string(object->voxels->occupied_voxel_count()), muted);
                const bool editingPosition = controller.text_edit().kind == TextEditKind::Position;
                if (editingPosition) inspectorPainter.fill(layout.inspectorFields.empty() ? UiRect{} : layout.inspectorFields[0], rgb(40,54,74));
                inspectorPainter.text(layout.inspector.x + 12, inspectorTop + 91,
                             editingPosition
                                 ? "Position  " + controller.text_edit().buffer + "_"
                                 : "Position  " + std::to_string(object->transform.position.x).substr(0,5) + "  " +
                                       std::to_string(object->transform.position.y).substr(0,5) + "  " +
                                       std::to_string(object->transform.position.z).substr(0,5),
                             editingPosition ? accent : muted);
                constexpr float degreesPerRadian = 57.29577951308232F;
                const Float3 euler = multiply(quaternion_to_euler_xyz(object->transform.rotation), degreesPerRadian);
                const bool editingRotation = controller.text_edit().kind == TextEditKind::Rotation;
                if (editingRotation) inspectorPainter.fill(layout.inspectorFields.size() < 2 ? UiRect{} : layout.inspectorFields[1], rgb(40,54,74));
                inspectorPainter.text(layout.inspector.x + 12, inspectorTop + 113,
                             editingRotation
                                 ? "Rotation  " + controller.text_edit().buffer + "_"
                                 : "Rotation  " + std::to_string(euler.x).substr(0,6) + "  " +
                                       std::to_string(euler.y).substr(0,6) + "  " + std::to_string(euler.z).substr(0,6),
                             editingRotation ? accent : muted);
                if (object->text3d) {
                    const auto& asset = *object->text3d;
                    const auto drawTextField = [&](std::size_t fieldIndex, TextEditKind kind,
                                                   int y, std::string label, std::string value,
                                                   EditorColor normalColor) {
                        const bool editing = controller.text_edit().kind == kind;
                        if (editing && fieldIndex < layout.inspectorFields.size())
                            inspectorPainter.fill(layout.inspectorFields[fieldIndex], rgb(40,54,74));
                        inspectorPainter.text(layout.inspector.x + 12, inspectorTop + y,
                                     label + "  " + (editing ? controller.text_edit().buffer + "_" : value),
                                     editing ? accent : normalColor);
                    };
                    drawTextField(2U, TextEditKind::Text3DContent, 135, "Text", asset.textUtf8, text);
                    drawTextField(3U, TextEditKind::Text3DSize, 157, "Size",
                                  std::to_string(asset.style.emSizeMeters).substr(0,7) + " m", muted);
                    drawTextField(4U, TextEditKind::Text3DDepth, 179, "Depth",
                                  std::to_string(asset.style.extrusionDepthMeters).substr(0,7) + " m", muted);
                    drawTextField(5U, TextEditKind::Text3DLetterSpacing, 201, "Spacing",
                                  std::to_string(asset.style.letterSpacingEm).substr(0,7) + " em", muted);
                    drawTextField(6U, TextEditKind::Text3DFaceMaterial, 223, "Face material",
                                  std::to_string(asset.style.faceMaterialId), text);
                    drawTextField(7U, TextEditKind::Text3DSideMaterial, 245, "Side material",
                                  std::to_string(asset.style.sideMaterialId), text);
                    drawTextField(8U, TextEditKind::Text3DFontPath, 267, "Font",
                                  object->textFontAsset.generic_string(), muted);
                    drawTextField(9U, TextEditKind::Text3DAlignment, 289, "Alignment",
                                  asset.style.alignment == Text3DHorizontalAlignment::Left ? "left" :
                                  asset.style.alignment == Text3DHorizontalAlignment::Center ? "center" : "right", muted);
                    drawTextField(10U, TextEditKind::Text3DFillRule, 311, "Fill",
                                  asset.style.fillRule == Text3DFillRule::NonZero ? "nonzero" : "evenodd", muted);
                    inspectorPainter.text(layout.inspector.x + 12, inspectorTop + 333,
                                 "Glyphs " + std::to_string(asset.glyphInstances.size()) +
                                 "   Curves " + std::to_string(asset.atlas.curveTexels.size()/2U), muted);
                    const auto dependency = inspect_text3d_font_dependency(
                        controller.project_root(), object->textFontAsset);
                    inspectorPainter.text(layout.inspector.x + 12, inspectorTop + 355,
                                 dependency.packageReady ? "Font dependency ready"
                                                         : "Font license/dependency warning",
                                 dependency.packageReady ? muted : rgb(235,180,80));
                    inspectorPainter.text(layout.inspector.x + 12, inspectorTop + 377,
                                 "Hash " + std::to_string(asset.contentHash), muted);
                } else if (object->gaborVolume) {
                    const auto& asset = *object->gaborVolume;
                    const auto& material = asset.material;
                    const auto drawGaborField = [&](std::size_t fieldIndex, TextEditKind kind,
                                                    int y, std::string label, std::string value) {
                        const bool editing = controller.text_edit().kind == kind;
                        if (editing && fieldIndex < layout.inspectorFields.size())
                            inspectorPainter.fill(layout.inspectorFields[fieldIndex], rgb(40,54,74));
                        inspectorPainter.text(layout.inspector.x + 12, inspectorTop + y,
                                     label + "  " + (editing ? controller.text_edit().buffer + "_" : value),
                                     editing ? accent : muted);
                    };
                    drawGaborField(2U, TextEditKind::GaborDensity, 135, "Density",
                                   std::to_string(material.densityMultiplier).substr(0,8));
                    drawGaborField(3U, TextEditKind::GaborTint, 157, "Tint",
                                   std::to_string(material.albedoTint.x).substr(0,5) + " " +
                                   std::to_string(material.albedoTint.y).substr(0,5) + " " +
                                   std::to_string(material.albedoTint.z).substr(0,5));
                    drawGaborField(4U, TextEditKind::GaborEmission, 179, "Emission",
                                   std::to_string(material.emissionIntensity).substr(0,8));
                    drawGaborField(5U, TextEditKind::GaborAnisotropy, 201, "Anisotropy",
                                   std::to_string(material.anisotropy).substr(0,8));
                    drawGaborField(6U, TextEditKind::GaborShadowStrength, 223, "Shadow",
                                   std::to_string(material.shadowStrength).substr(0,8));
                    drawGaborField(7U, TextEditKind::GaborLodBias, 245, "LOD bias",
                                   std::to_string(material.lodBias).substr(0,8));
                    std::uint16_t maximumLod{};
                    for (const auto& primitive : asset.primitives)
                        maximumLod = std::max(maximumLod, primitive.lodLevel);
                    inspectorPainter.text(layout.inspector.x + 12, inspectorTop + 267,
                                 "Primitives " + std::to_string(asset.primitives.size()) +
                                 "   Levels " + std::to_string(static_cast<unsigned>(maximumLod)+1U), muted);
                    inspectorPainter.text(layout.inspector.x + 12, inspectorTop + 289,
                                 "Source " + object->sourceAsset.generic_string(), muted);
                    inspectorPainter.text(layout.inspector.x + 12, inspectorTop + 311,
                                 "Hash " + std::to_string(asset.contentHash), muted);
                } else {
                    inspectorPainter.text(layout.inspector.x + 12, inspectorTop + 135,
                                 "Selection " + std::to_string(controller.workspace().selection_count()) +
                                 "   Axes " + (controller.transform_space() == EditorTransformSpace::World ? "World" : "Local"), text);
                    const EditorSelectionDiagnostics& diagnostics = controller.selection_diagnostics();
                    inspectorPainter.text(layout.inspector.x + 12, inspectorTop + 157,
                                 "Mass " + std::to_string(diagnostics.massKilograms).substr(0,8) + " kg", muted);
                    inspectorPainter.text(layout.inspector.x + 12, inspectorTop + 179,
                                 "Components " + std::to_string(diagnostics.connectedComponents) +
                                 "   Detached " + std::to_string(diagnostics.detachedComponents), muted);
                    inspectorPainter.text(layout.inspector.x + 12, inspectorTop + 201,
                                 "Collision boxes " + std::to_string(diagnostics.collisionBoxes), muted);
                    inspectorPainter.text(layout.inspector.x + 12, inspectorTop + 223,
                                 "Material " + std::to_string(controller.active_material()) + "  " +
                                 (controller.materials().find(controller.active_material()) ?
                                  controller.materials().find(controller.active_material())->definition.name : "Unknown"), text);
                    inspectorPainter.text(layout.inspector.x + 12, inspectorTop + 245,
                                 "Object components " + std::to_string(object->components.size()), muted);
                    if (object->attachment && object->parent) {
                        const std::string socket = object->attachment->socket.empty()
                            ? std::string("default") : object->attachment->socket;
                        inspectorPainter.text(layout.inspector.x + 12, inspectorTop + 267,
                                     "Attached to " + std::to_string(*object->parent) + "  socket " + socket, muted);
                    } else {
                        inspectorPainter.text(layout.inspector.x + 12, inspectorTop + 267,
                                     "Attachment  none", muted);
                    }
                    if (object->prefabLink) {
                        inspectorPainter.text(layout.inspector.x + 12, inspectorTop + 289,
                                     "Prefab " + object->prefabLink->prefabAsset.filename().generic_string() +
                                     "  overrides " + std::to_string(object->prefabLink->overrides.size()), accent);
                    } else {
                        inspectorPainter.text(layout.inspector.x + 12, inspectorTop + 289,
                                     "Prefab  none", muted);
                    }
                    inspectorPainter.text(layout.inspector.x + 12, inspectorTop + 311,
                                 "Layer " + std::to_string(object->layer) + "  Tags " +
                                 std::to_string(object->tags.size()) + "  Groups " +
                                 std::to_string(object->groups.size()), muted);
                    const auto componentSections = controller.primary_component_sections();
                    for (std::size_t componentIndex = 0;
                         componentIndex < std::min<std::size_t>(componentSections.size(), 3U);
                         ++componentIndex) {
                        const auto& section = componentSections[componentIndex];
                        std::string line = (section.enabled ? "[x] " : "[ ] ") + section.displayName;
                        if (!section.properties.empty())
                            line += "  " + section.properties.front().displayName + "=" +
                                    format_component_value(section.properties.front().value);
                        inspectorPainter.text(layout.inspector.x + 12,
                                     inspectorTop + 333 + static_cast<int>(componentIndex) * 20,
                                     line, section.enabled ? text : muted);
                    }
                }
                static constexpr std::array<std::string_view,5> labels{"Visible","Locked","Anchored","Structural","Collision"};
                const std::array<bool,5> values{object->flags.visible,object->flags.locked,object->flags.anchored,
                                                object->flags.structural,object->flags.collisionEnabled};
                for (std::size_t i = 0; i < layout.inspectorToggles.size(); ++i) {
                    const UiRect row = layout.inspectorToggles[i];
                    painter.fill({row.x,row.y,18,18}, values[i] ? accent : panel2);
                    painter.outline({row.x,row.y,18,18}, border);
                    if (values[i]) painter.text(row.x+4,row.y+14,"x",text);
                    painter.text(row.x+27,row.y+15,labels[i],text);
                }
            }
        } else {
            painter.text(layout.inspector.x + 12, layout.inspector.y + 48, "No selection", muted);
        }
    }

    static constexpr std::array<std::string_view, 6> kBottomTabNames{"Problems", "Tasks", "Console", "Profiler", "Assets", "AI Assistant"};
    for (std::size_t i = 0; i < layout.bottomTabs.size() && i < kBottomTabNames.size(); ++i) {
        const UiRect tabRect = layout.bottomTabs[i];
        if (static_cast<int>(controller.bottom_tab()) == static_cast<int>(i)) painter.fill(tabRect, rgb(55,92,140));
        painter.text(tabRect.x + 6, tabRect.y + tabRect.height - 6, kBottomTabNames[i], text);
    }
    int bottomY = layout.bottomTabs.empty() ? (layout.bottomPanel.y + 20)
                                             : (layout.bottomTabs.front().y + layout.bottomTabs.front().height + 14);
    switch (controller.bottom_tab()) {
        case BottomPanelTab::Problems: {
            if (controller.workspace().problems().problems().empty()) {
                painter.text(layout.bottomPanel.x + 14, bottomY, "No blocking problems.", text);
            } else {
                for (const EditorProblem& problem : controller.workspace().problems().problems()) {
                    const EditorColor problemColor = problem.severity == ProblemSeverity::Error ? rgb(255,95,95) :
                                                       problem.severity == ProblemSeverity::Warning ? rgb(255,190,80) : text;
                    painter.text(layout.bottomPanel.x + 14, bottomY, problem.code + ": " + problem.summary, problemColor);
                    bottomY += 20;
                }
            }
            break;
        }
        case BottomPanelTab::Tasks: {
            painter.text(layout.bottomPanel.x + 14, bottomY,
                         "Active tool: " + tool_name(controller.active_tool()) +
                         "   Selected: " + std::to_string(controller.workspace().selection_count()) +
                         "   Undo: " + std::string(controller.workspace().commands().undo_label()), muted);
            break;
        }
        case BottomPanelTab::Console: {
            const auto& entries = controller.workspace().log().entries();
            std::size_t shown = 0;
            const std::size_t maxLines = 6;
            const std::size_t start = entries.size() > maxLines ? entries.size() - maxLines : 0;
            for (std::size_t i = start; i < entries.size(); ++i) {
                const EditorColor logColor = entries[i].level == EditorLogLevel::Error ? rgb(255,95,95) :
                                               entries[i].level == EditorLogLevel::Warning ? rgb(255,190,80) : muted;
                painter.text(layout.bottomPanel.x + 14, bottomY, entries[i].text, logColor);
                bottomY += 20;
                ++shown;
            }
            if (shown == 0) painter.text(layout.bottomPanel.x + 14, bottomY, "(no messages yet)", muted);
            break;
        }
        case BottomPanelTab::Profiler: {
            painter.text(layout.bottomPanel.x + 14, bottomY,
                         "Draw items: " + std::to_string(controller.draw_item_count()) +
                         "   UI zoom: " + format_ui_zoom_percent(controller.effective_ui_zoom()), muted);
            break;
        }
        case BottomPanelTab::Assistant: {
            const auto& assistantPanel = controller.workspace().ai_assistant();
            const auto& messages = assistantPanel.messages();
            const std::size_t maxLines = 4;
            const std::size_t start = messages.size() > maxLines ? messages.size() - maxLines : 0;
            for (std::size_t i = start; i < messages.size(); ++i) {
                std::string prefix;
                switch (messages[i].role) {
                    case AiPanelRole::User: prefix = "You: "; break;
                    case AiPanelRole::Assistant: prefix = "ChatGPT: "; break;
                    case AiPanelRole::Tool: prefix = "Tool: "; break;
                    case AiPanelRole::System: prefix = "System: "; break;
                }
                std::string line = prefix + messages[i].text;
                if (line.size() > 150U) line.resize(150U), line += "...";
                painter.text(layout.bottomPanel.x + 14, bottomY, line, messages[i].role == AiPanelRole::System ? muted : text);
                bottomY += 20;
            }
            if (messages.empty()) painter.text(layout.bottomPanel.x + 14, bottomY,
                "ChatGPT is ready. OPENAI_API_KEY is read from the process environment.", muted);
            painter.fill(layout.aiPromptBox, panel2);
            painter.outline(layout.aiPromptBox, border);
            const std::string prompt = controller.text_edit().kind == TextEditKind::AiPrompt
                ? controller.text_edit().buffer : std::string("Ask ChatGPT to inspect or change the project...");
            painter.text(layout.aiPromptBox.x + 7, layout.aiPromptBox.y + layout.aiPromptBox.height - 6, prompt,
                         controller.text_edit().kind == TextEditKind::AiPrompt ? text : muted);
            painter.fill(layout.aiSendButton, accent);
            painter.text(layout.aiSendButton.x + 15, layout.aiSendButton.y + layout.aiSendButton.height - 6, "Send", text);
            if (!assistantPanel.pending_approvals().empty()) {
                const auto& proposal = assistantPanel.pending_approvals().front();
                painter.text(layout.aiPromptBox.x, layout.aiApproveButton.y - 7,
                    "Approval required [" + proposal.actor + "]: " + proposal.summary, rgb(255,190,80));
                painter.fill(layout.aiApproveButton, accent);
                painter.text(layout.aiApproveButton.x + 11, layout.aiApproveButton.y + layout.aiApproveButton.height - 6,
                             "Approve", text);
                painter.fill(layout.aiDenyButton, rgb(105,55,55));
                painter.text(layout.aiDenyButton.x + 19, layout.aiDenyButton.y + layout.aiDenyButton.height - 6,
                             "Deny", text);
            }
            break;
        }
        case BottomPanelTab::Assets: {
            painter.fill(layout.assetSearchBox, panel2);
            painter.outline(layout.assetSearchBox, border);
            const bool editingSearch = controller.text_edit().kind == TextEditKind::AssetSearch;
            const std::string search = editingSearch ? controller.text_edit().buffer
                : controller.asset_browser_state().query.text;
            painter.text(layout.assetSearchBox.x + 7,
                         layout.assetSearchBox.y + layout.assetSearchBox.height - 6,
                         search.empty() ? std::string("Search assets, paths, kinds, or tags...") : search,
                         search.empty() && !editingSearch ? muted : text);
            painter.fill(layout.assetFilterButton,
                         controller.asset_browser_state().query.staleOnly ? rgb(122,82,42) : panel2);
            painter.outline(layout.assetFilterButton, border);
            painter.text(layout.assetFilterButton.x + 9,
                         layout.assetFilterButton.y + layout.assetFilterButton.height - 6,
                         controller.asset_browser_state().query.staleOnly ? "Issues" : "All", text);
            painter.fill(layout.assetRefreshButton, accent);
            painter.text(layout.assetRefreshButton.x + 10,
                         layout.assetRefreshButton.y + layout.assetRefreshButton.height - 6,
                         "Refresh", text);

            const auto entries = controller.asset_browser_rows();
            for (std::size_t visible = 0; visible < layout.assetRows.size(); ++visible) {
                const std::size_t index = controller.asset_browser_state().firstVisible + visible;
                if (index >= entries.size()) break;
                const EditorAssetRecord& asset = *entries[index];
                const UiRect row = layout.assetRows[visible];
                if (controller.asset_browser_state().selectedId &&
                    *controller.asset_browser_state().selectedId == asset.id) painter.fill(row, rgb(55,92,140));
                const unsigned kindValue = static_cast<unsigned>(asset.kind);
                const EditorColor icon = rgb(static_cast<unsigned char>(70U + (kindValue * 37U) % 130U),
                                             static_cast<unsigned char>(85U + (kindValue * 53U) % 120U),
                                             static_cast<unsigned char>(95U + (kindValue * 29U) % 110U));
                painter.fill({row.x, row.y + 2, 14, 14}, icon);
                painter.outline({row.x, row.y + 2, 14, 14}, border);
                std::string name = asset.displayName;
                if (controller.text_edit().kind == TextEditKind::AssetRename &&
                    controller.asset_browser_state().selectedId &&
                    *controller.asset_browser_state().selectedId == asset.id)
                    name = controller.text_edit().buffer;
                painter.text(row.x + 22, row.y + row.height - 6, name, text);
                const std::string kind = std::string(to_string(asset.kind));
                const std::string health = std::string(to_string(asset.health));
                const bool issue = asset.health == EditorAssetHealth::StaleImport ||
                    asset.health == EditorAssetHealth::MissingSource ||
                    asset.health == EditorAssetHealth::MissingCooked ||
                    asset.health == EditorAssetHealth::MissingDependency ||
                    asset.health == EditorAssetHealth::Unreadable;
                const std::string badge = kind + "  " + health +
                    (asset.dependencies.empty() ? std::string{} : "  deps " + std::to_string(asset.dependencies.size())) +
                    (asset.unresolvedDependencies.empty() ? std::string{} : "  missing " + std::to_string(asset.unresolvedDependencies.size()));
                painter.text(row.x + row.width - painter.text_width(badge) - 7,
                             row.y + row.height - 6, badge,
                             issue ? rgb(255,190,80) : muted);
            }
            // Const access: the mutable accessor would wait for a background scan.
            const std::string summary = std::to_string(entries.size()) + " shown / " +
                std::to_string(std::as_const(controller).asset_database().records().size()) + " indexed" +
                (controller.asset_scan_in_flight() ? "  (scanning...)" : "");
            painter.text(layout.assetSearchBox.x,
                         layout.assetSearchBox.y + layout.assetSearchBox.height + 14,
                         summary, muted);
            break;
        }
    }

    const EditorStatusMessage& status = controller.status();
    painter.text(10, layout.statusBar.y + layout.statusBar.height - 6, status.text,
                 status.error ? rgb(255,105,105) : text);
    const auto& prefs = controller.workspace().preferences();
    const std::string scale = "UI " + format_ui_zoom_percent(controller.effective_ui_zoom()) +
        "  M " + (prefs.translateSnapEnabled ? std::to_string(prefs.translateSnapMeters).substr(0,4) : "off") +
        "m  R " + (prefs.rotateSnapEnabled ? std::to_string(prefs.rotateSnapDegrees).substr(0,4) : "off") +
        "°  S " + (prefs.scaleSnapEnabled ? std::to_string(prefs.scaleSnapStep).substr(0,4) : "off") +
        (prefs.absoluteGridSnap ? "  Grid" : "  Delta");
    painter.text(width - painter.text_width(scale) - 12, layout.statusBar.y + layout.statusBar.height - 6, scale, muted);

    if (controller.open_menu()) {
        const auto actions = controller.menu_actions(*controller.open_menu());
        const NativeMenuPopupLayout popupLayout = controller.menu_popup_layout();
        const UiRect popup = popupLayout.popup;
        painter.fill(popup, panel2);
        painter.outline(popup, border);
        for (std::size_t visible = 0; visible < popupLayout.rows.size(); ++visible) {
            const std::size_t i = popupLayout.firstVisibleAction + visible;
            if (i >= actions.size()) break;
            const UiRect row = popupLayout.rows[visible];
            const int y = row.y;
            const int itemHeight = row.height;
            if (controller.menu_hovered_action() && *controller.menu_hovered_action() == i)
                painter.fill(row, actions[i].enabled ? rgb(48,88,142) : rgb(54,58,66));
            if (i > 0U && actions[i].section != actions[i - 1U].section)
                painter.line(popup.x + 5, y, popup.x + popup.width - 5, y, border);
            if (actions[i].checkable) {
                painter.outline({popup.x + 8, y + 5, 14, 14}, border);
                if (actions[i].checked) painter.text(popup.x + 10, y + itemHeight - 8, "x", accent);
            }
            const int labelX = popup.x + (actions[i].checkable ? 30 : 10);
            const std::uint32_t labelColor = !actions[i].enabled ? muted
                : actions[i].dangerous ? rgb(255,125,125) : text;
            std::string rightText = actions[i].shortcut;
            if (!actions[i].enabled && !actions[i].disabledReason.empty()) rightText = "Unavailable";
            else if (actions[i].visibility == MenuVisibility::Advanced && rightText.empty()) rightText = "Advanced";
            else if (rightText.empty()) rightText = actions[i].section;
            // Label and right column get separate space: the right text keeps its width (up to
            // half the row) and the label is elided before it, with the full label in a tooltip.
            const int rowRight = popup.x + popup.width - 8;
            if (!rightText.empty())
                rightText = elide_text_to_width(painter, rightText, std::max(0, (rowRight - labelX) / 2));
            const int rightX = rightText.empty() ? rowRight : rowRight - painter.text_width(rightText);
            const std::string label = elide_text_to_width(painter, actions[i].label, std::max(0, rightX - 12 - labelX));
            painter.text(labelX, y + itemHeight - 7, label, labelColor);
            if (!rightText.empty()) painter.text(rightX, y + itemHeight - 7, rightText, muted);
            const bool hovered = controller.menu_hovered_action() && *controller.menu_hovered_action() == i;
            const bool explain = !actions[i].enabled && !actions[i].disabledReason.empty();
            if (hovered && (label != actions[i].label || explain)) {
                ChromeTooltip tip{row, {actions[i].label}, true};
                if (!actions[i].shortcut.empty()) tip.lines.front() += "  (" + actions[i].shortcut + ")";
                if (explain) tip.lines.push_back("Unavailable: " + actions[i].disabledReason);
                tooltips.push_back(std::move(tip));
            }
        }
        if (popupLayout.canScrollUp) painter.text(popup.x + popup.width - 18, popup.y + 14, "^", muted);
        if (popupLayout.canScrollDown)
            painter.text(popup.x + popup.width - 18, popup.y + popup.height - 6, "v", muted);
    }

    for (const ChromeTooltip& tip : tooltips)
        draw_chrome_tooltip(painter, tip, {0, 0, width, height}, rgb(16,22,31), border, text, muted);

    if (controller.marquee().active) {
        const MarqueeState& marquee = controller.marquee();
        const int left = std::min(marquee.startX, marquee.currentX);
        const int top = std::min(marquee.startY, marquee.currentY);
        const int w = std::abs(marquee.currentX - marquee.startX);
        const int h = std::abs(marquee.currentY - marquee.startY);
        painter.outline({left, top, w, h}, accent);
    }

    if (controller.context_menu().open) {
        const ContextMenuState& menu = controller.context_menu();
        const int itemHeight = 24;
        const UiRect popup{menu.x, menu.y, menu.width, itemHeight * static_cast<int>(menu.items.size())};
        painter.fill(popup, panel2);
        painter.outline(popup, border);
        for (std::size_t i = 0; i < menu.items.size(); ++i) {
            if (menu.hoveredItem && *menu.hoveredItem == i) painter.fill(menu.itemRects[i], rgb(48,88,142));
            painter.text(popup.x + 10, popup.y + static_cast<int>(i) * itemHeight + itemHeight - 7,
                         elide_text_to_width(painter, menu.items[i].label, popup.width - 20), text);
        }
    }

    render_synth_panel(painter, controller, panel, panel2, border, text, muted, accent);
    render_chiptune_panel(painter, controller, panel, panel2, border, text, muted, accent);
    render_audio_panel(painter, controller, panel, panel2, border, text, muted, accent);
    render_audio_event_panel(painter, controller, panel, panel2, border, text, muted, accent);
    render_sprite_authoring_panel(painter, controller, panel, panel2, border, text, muted, accent);
    render_sprite_pixel_art_panel(painter, controller, panel, panel2, border, text, muted, accent);
    render_sprite_rig2d_panel(painter, controller, panel, panel2, border, text, muted, accent);
    render_control_rig_panel(painter, controller, panel, panel2, border, text, muted, accent);
    render_sprite_animation_graph_panel(painter, controller, panel, panel2, border, text, muted, accent);
    render_tile_world_editor_panel(painter, controller, panel, panel2, border, text, muted, accent);
    render_render3d_diagnostics_panel(painter, controller, panel, panel2, border, text, muted, accent);
    if (std::get<bool>(controller.workspace().settings().value("accessibility.focus_indicators"))) {
        UiRect focused = layout.viewport;
        switch (controller.focus_region()) {
        case EditorFocusRegion::MenuBar: focused = layout.menuBar; break;
        case EditorFocusRegion::Toolbar: focused = layout.toolbar; break;
        case EditorFocusRegion::SceneHierarchy: focused = layout.hierarchy; break;
        case EditorFocusRegion::Inspector: focused = layout.inspector; break;
        case EditorFocusRegion::BottomPanel: focused = layout.bottomPanel; break;
        case EditorFocusRegion::Viewport: break;
        }
        painter.outline(focused, accent);
    }

    int diagnosticY = layout.viewport.y + 24;
    if (std::get<bool>(controller.workspace().settings().value("diagnostics.render_stats"))) {
        painter.text(layout.viewport.x + 12, diagnosticY,
            "Render: " + std::to_string(controller.draw_items().size()) + " voxel draw items", text);
        diagnosticY += 20;
    }
    if (std::get<bool>(controller.workspace().settings().value("diagnostics.audio_stats"))) {
        const auto meters = controller.audio_mixer().meters();
        painter.text(layout.viewport.x + 12, diagnosticY,
            "Audio: " + std::to_string(meters.physicalSampleVoices) + " voices / " +
            std::to_string(meters.streamUnderrunFrames) + " underrun frames / " +
            std::to_string(meters.droppedCommands) + " dropped commands", text);
        diagnosticY += 20;
    }
    if (std::get<bool>(controller.workspace().settings().value("diagnostics.camera_debug"))) {
        const auto& pose = controller.camera();
        painter.text(layout.viewport.x + 12, diagnosticY,
            "Camera: " + std::to_string(pose.position.x) + ", " + std::to_string(pose.position.y) + ", " +
            std::to_string(pose.position.z) + " / rig " + std::to_string(controller.camera_director().telemetry().liveRig), text);
    }
    render_cinematic_camera_panel(painter, controller, panel, panel2, border, text, muted, accent);

    if (controller.settings_panel().open) {
        const NativeSettingsModalLayout settingsLayout = controller.settings_modal_layout();
        // Every settings text is fitted to its cell (setting rows add label, marker, value and
        // policy cells as they are drawn); long values are elided and the hovered or selected
        // value is shown in full in a tooltip.
        CellFitCanvas settingsText(painter, settingsLayout.panel, UiRect{0, 0, width, height},
                                   controller.hover_x(), controller.hover_y());
        settingsText.add_cell(settingsLayout.title);
        for (const UiRect& tab : settingsLayout.scopeTabs) settingsText.add_cell(tab);
        for (const UiRect& rect : {settingsLayout.searchBox, settingsLayout.changedToggle, settingsLayout.advancedToggle,
                                   settingsLayout.resetSettingButton, settingsLayout.resetCategoryButton,
                                   settingsLayout.discardButton, settingsLayout.applyButton})
            settingsText.add_cell(rect);
        for (const UiRect& row : settingsLayout.categoryRows) settingsText.add_cell(row);
        {
            const int hintX = settingsLayout.panel.x + 308;
            const int hintRight = std::min(settingsLayout.discardButton.x, settingsLayout.applyButton.x) - 6;
            settingsText.add_cell({hintX, settingsLayout.applyButton.y, hintRight - hintX, settingsLayout.applyButton.height});
        }
        settingsText.add_cell(settingsLayout.detailPanel);
        painter.fill({0, 0, width, height}, rgb(8,10,14));
        painter.fill(settingsLayout.panel, panel);
        painter.outline(settingsLayout.panel, accent);
        settingsText.text(settingsLayout.title.x, settingsLayout.title.y + 20, "Settings and Preferences", text);

        static constexpr std::array<SettingScope, 3> scopes{
            SettingScope::User, SettingScope::Project, SettingScope::Session};
        for (std::size_t index = 0; index < settingsLayout.scopeTabs.size(); ++index) {
            const UiRect tab = settingsLayout.scopeTabs[index];
            painter.fill(tab, controller.settings_panel().scope == scopes[index] ? accent : panel2);
            painter.outline(tab, border);
            settingsText.text(tab.x + 10, tab.y + tab.height - 8, setting_scope_name(scopes[index]), text);
        }

        painter.fill(settingsLayout.searchBox, panel2);
        painter.outline(settingsLayout.searchBox, border);
        const std::string search = controller.settings_panel().searchQuery.empty()
            ? std::string("Search every option by name, description, or keyword...")
            : controller.settings_panel().searchQuery + "_";
        settingsText.text(settingsLayout.searchBox.x + 8,
                     settingsLayout.searchBox.y + settingsLayout.searchBox.height - 8,
                     search, controller.settings_panel().searchQuery.empty() ? muted : text);

        painter.fill(settingsLayout.changedToggle,
                     controller.settings_panel().changedOnly ? accent : panel2);
        painter.outline(settingsLayout.changedToggle, border);
        settingsText.text(settingsLayout.changedToggle.x + 8,
                     settingsLayout.changedToggle.y + settingsLayout.changedToggle.height - 8,
                     controller.settings_panel().changedOnly ? "Changed: On" : "Changed: Off", text);

        painter.fill(settingsLayout.advancedToggle,
                     controller.settings_panel().includeAdvanced ? accent : panel2);
        painter.outline(settingsLayout.advancedToggle, border);
        settingsText.text(settingsLayout.advancedToggle.x + 8,
                     settingsLayout.advancedToggle.y + settingsLayout.advancedToggle.height - 8,
                     controller.settings_panel().includeAdvanced ? "Advanced: On" : "Advanced: Off", text);

        const auto categories = controller.settings_categories();
        for (std::size_t index = 0; index < settingsLayout.categoryRows.size() && index < categories.size(); ++index) {
            const UiRect row = settingsLayout.categoryRows[index];
            if (categories[index] == controller.settings_panel().selectedCategory &&
                controller.settings_panel().searchQuery.empty()) painter.fill(row, rgb(48,78,118));
            settingsText.text(row.x + 8, row.y + row.height - 8, categories[index], text);
        }

        const auto rows = controller.settings_rows();
        std::string previousSection;
        for (std::size_t visible = 0; visible < settingsLayout.settingRows.size(); ++visible) {
            const std::size_t rowIndex = settingsLayout.firstVisibleSetting + visible;
            if (rowIndex >= rows.size()) break;
            const SettingDefinition& definition = *rows[rowIndex];
            const UiRect row = settingsLayout.settingRows[visible];
            const SettingAvailability availability = controller.workspace().settings().availability(
                definition.id, controller.settings_capabilities());
            if (rowIndex == controller.settings_panel().selectedRow) painter.fill(row, rgb(45,68,96));
            else if ((visible & 1U) != 0U) painter.fill(row, rgb(24,29,37));
            if (!previousSection.empty() && definition.section != previousSection)
                painter.line(row.x + 4, row.y, row.x + row.width - 4, row.y, border);
            previousSection = definition.section;
            const SettingValue displayed = controller.settings_panel().displayed_value(
                controller.workspace().settings(), definition.id);
            SettingScope source = SettingScope::User;
            bool inherited = false;
            (void)controller.workspace().settings().value(definition.id, &source, &inherited);
            std::string valueText = controller.settings_panel().value_label(definition, displayed);
            if (definition.id == kUiZoomSettingId)
                if (const auto* zoom = std::get_if<double>(&displayed))
                {
                    const float shown = snap_ui_zoom(static_cast<float>(*zoom));
                    valueText = format_ui_zoom_percent(shown);
                    if (shown > controller.ui_zoom_window_limit() + 1.0e-3F)
                        valueText += "  (window fits " + format_ui_zoom_percent(controller.ui_zoom_window_limit()) + ")";
                    valueText += "  Ctrl+= / Ctrl+- / Ctrl+0";
                }
            if (controller.settings_panel().valueEditing &&
                controller.settings_panel().valueEditId == definition.id)
                valueText = controller.settings_panel().valueEditBuffer + "_";
            const std::uint32_t rowText = availability.available ? text : muted;
            const int valueX = row.x + row.width * 2 / 3;
            std::string marker;
            if (controller.settings_panel().stagedValues.contains(definition.id)) marker = "modified";
            else if (controller.settings_panel().stagedClears.contains(definition.id)) marker = "reset";
            else if (controller.workspace().settings().has_override(controller.settings_panel().scope, definition.id))
                marker = setting_scope_name(controller.settings_panel().scope);
            else if (inherited) marker = "from " + setting_scope_name(source);
            std::string policy;
            if (definition.applyPolicy == SettingApplyPolicy::RestartRequired) policy = "restart";
            else if (definition.applyPolicy == SettingApplyPolicy::OnApply) policy = "apply";
            if (!definition.applied) policy = "no effect yet";
            const int policyX = policy.empty() ? row.x + row.width : row.x + row.width - painter.text_width(policy) - 8;
            const int markerX = marker.empty() ? valueX - 12 : std::max(row.x + row.width / 3, valueX - painter.text_width(marker) - 12);
            if (!policy.empty()) settingsText.add_cell({policyX - 2, row.y, row.x + row.width - policyX + 2, row.height});
            settingsText.add_cell({valueX - 2, row.y, policyX - 6 - valueX + 2, row.height},
                                  rowIndex == controller.settings_panel().selectedRow);
            if (!marker.empty()) settingsText.add_cell({markerX - 2, row.y, valueX - 4 - markerX, row.height});
            settingsText.add_cell({row.x, row.y, markerX - 6 - row.x, row.height});
            const int baseline = row.y + row.height - 8;
            settingsText.text(row.x + 8, baseline, definition.label, rowText);
            settingsText.text(valueX, baseline, valueText, availability.available ? accent : muted);
            if (!marker.empty()) settingsText.text(markerX, baseline, marker, muted);
            if (!policy.empty()) settingsText.text(policyX, baseline, policy, muted);
        }
        if (rows.empty()) settingsText.text(settingsLayout.searchBox.x, settingsLayout.searchBox.y + 66,
                                       controller.settings_panel().changedOnly
                                           ? "No changed options match this view."
                                           : "No settings match this search.", muted);

        painter.fill(settingsLayout.detailPanel, panel2);
        painter.outline(settingsLayout.detailPanel, border);
        if (controller.settings_panel().selectedRow < rows.size()) {
            const SettingDefinition& definition = *rows[controller.settings_panel().selectedRow];
            settingsText.text(settingsLayout.detailPanel.x + 10, settingsLayout.detailPanel.y + 22,
                         definition.category + " > " + definition.section + " > " + definition.label, accent);
            settingsText.text(settingsLayout.detailPanel.x + 10, settingsLayout.detailPanel.y + 45,
                         definition.description, text);
            std::string details = "Default: " + controller.settings_panel().value_label(definition, definition.defaultValue);
            if (definition.minimum) details += "  Min: " + std::to_string(*definition.minimum);
            if (definition.maximum) details += "  Max: " + std::to_string(*definition.maximum);
            if (definition.id == kUiZoomSettingId)
                details = "Default: 100%  Min: 100%  Max: 200%  Step: 25%  Current window allows up to " +
                          format_ui_zoom_percent(controller.ui_zoom_window_limit());
            if (!definition.applied) details += "  |  Not applied yet: changing this has no effect in this build.";
            settingsText.text(settingsLayout.detailPanel.x + 10, settingsLayout.detailPanel.y + 68, details, muted);
            const SettingAvailability availability = controller.workspace().settings().availability(
                definition.id, controller.settings_capabilities());
            if (!availability.available)
                settingsText.text(settingsLayout.detailPanel.x + 10, settingsLayout.detailPanel.y + 88,
                             availability.explanation, rgb(255,190,80));
            else if (definition.id == dve::editor::kMidiOutputPortSettingId)
                settingsText.text(settingsLayout.detailPanel.x + 10, settingsLayout.detailPanel.y + 88,
                             "Status: " + controller.midi_output_summary(),
                             controller.midi_output_status().state == audio::MidiConnectionState::Connected
                                 ? rgb(120,220,150) : rgb(255,190,80));
            else if (definition.id.starts_with("midi."))
                settingsText.text(settingsLayout.detailPanel.x + 10, settingsLayout.detailPanel.y + 88,
                             "Status: " + controller.midi_input_summary(),
                             controller.midi_input_status().state == audio::MidiConnectionState::Connected
                                 ? rgb(120,220,150) : rgb(255,190,80));
        }
        if (!controller.settings_panel().status.empty())
            settingsText.text(settingsLayout.detailPanel.x + settingsLayout.detailPanel.width / 2,
                         settingsLayout.detailPanel.y + settingsLayout.detailPanel.height - 10,
                         controller.settings_panel().status, muted);

        painter.fill(settingsLayout.resetSettingButton, panel2);
        painter.outline(settingsLayout.resetSettingButton, border);
        settingsText.text(settingsLayout.resetSettingButton.x + 12,
                     settingsLayout.resetSettingButton.y + settingsLayout.resetSettingButton.height - 9,
                     "Reset Option", text);
        painter.fill(settingsLayout.resetCategoryButton, panel2);
        painter.outline(settingsLayout.resetCategoryButton, border);
        settingsText.text(settingsLayout.resetCategoryButton.x + 12,
                     settingsLayout.resetCategoryButton.y + settingsLayout.resetCategoryButton.height - 9,
                     "Reset Category", text);
        painter.fill(settingsLayout.discardButton, panel2);
        painter.outline(settingsLayout.discardButton, border);
        settingsText.text(settingsLayout.discardButton.x + 18,
                     settingsLayout.discardButton.y + settingsLayout.discardButton.height - 9, "Discard", text);
        painter.fill(settingsLayout.applyButton, controller.settings_panel().dirty ? accent : panel2);
        painter.outline(settingsLayout.applyButton, border);
        settingsText.text(settingsLayout.applyButton.x + 24,
                     settingsLayout.applyButton.y + settingsLayout.applyButton.height - 9, "Apply", text);
        // Key hint between Reset Category and Discard; dropped when the footer has no room.
        if (std::min(settingsLayout.discardButton.x, settingsLayout.applyButton.x) - 6 - (settingsLayout.panel.x + 308) >= 60)
            settingsText.text(settingsLayout.panel.x + 310, settingsLayout.applyButton.y + 22,
                              "Enter/F2 edits | arrows adjust | Ctrl+R resets | F3 changed | Ctrl+Tab scope", muted);
        settingsText.draw_tooltip(rgb(16,20,28), accent, text);
    }

    if (controller.shortcut_panel().open) {
        const NativeShortcutModalLayout shortcutLayout = controller.shortcut_modal_layout();
        painter.fill({0, 0, width, height}, rgb(8,10,14));
        painter.fill(shortcutLayout.panel, panel);
        painter.outline(shortcutLayout.panel, accent);
        painter.text(shortcutLayout.panel.x + 16, shortcutLayout.panel.y + 31,
                     "Keyboard and Mouse Shortcuts", text);

        painter.fill(shortcutLayout.profilePrevious, panel2); painter.outline(shortcutLayout.profilePrevious, border);
        painter.text(shortcutLayout.profilePrevious.x + 10, shortcutLayout.profilePrevious.y + 19, "<", text);
        painter.fill(shortcutLayout.profileNext, panel2); painter.outline(shortcutLayout.profileNext, border);
        painter.text(shortcutLayout.profileNext.x + 10, shortcutLayout.profileNext.y + 19, ">", text);
        const std::string profileText = std::string(controller.workspace().shortcuts().active_profile());
        painter.text(shortcutLayout.profilePrevious.x + 40, shortcutLayout.profilePrevious.y + 19,
                     profileText, accent);

        painter.fill(shortcutLayout.contextPrevious, panel2); painter.outline(shortcutLayout.contextPrevious, border);
        painter.text(shortcutLayout.contextPrevious.x + 10, shortcutLayout.contextPrevious.y + 19, "<", text);
        painter.fill(shortcutLayout.contextNext, panel2); painter.outline(shortcutLayout.contextNext, border);
        painter.text(shortcutLayout.contextNext.x + 10, shortcutLayout.contextNext.y + 19, ">", text);
        const std::string contextText = controller.shortcut_panel().contextFilter
            ? shortcut_context_name(*controller.shortcut_panel().contextFilter) : std::string("All Contexts");
        const int contextX = shortcutLayout.contextPrevious.x + 40;
        painter.text(contextX, shortcutLayout.contextPrevious.y + 19, contextText, accent);

        painter.fill(shortcutLayout.searchBox, panel2); painter.outline(shortcutLayout.searchBox, border);
        const std::string searchText = controller.shortcut_panel().searchQuery.empty()
            ? std::string("Search commands, keys, or categories...")
            : controller.shortcut_panel().searchQuery + "_";
        painter.text(shortcutLayout.searchBox.x + 8,
                     shortcutLayout.searchBox.y + shortcutLayout.searchBox.height - 7,
                     searchText, controller.shortcut_panel().searchQuery.empty() ? muted : text);

        const auto rows = controller.shortcut_rows();
        for (std::size_t visible = 0; visible < shortcutLayout.rows.size(); ++visible) {
            const std::size_t rowIndex = shortcutLayout.firstVisibleRow + visible;
            if (rowIndex >= rows.size()) break;
            const ShortcutSearchResult& result = rows[rowIndex];
            const UiRect row = shortcutLayout.rows[visible];
            if (rowIndex == controller.shortcut_panel().selectedRow) painter.fill(row, rgb(45,68,96));
            if ((visible & 1U) != 0U && rowIndex != controller.shortcut_panel().selectedRow)
                painter.fill(row, rgb(24,29,37));
            const int categoryWidth = row.width / 6;
            const int contextWidth = row.width / 7;
            const int bindingWidth = row.width / 6;
            painter.text(row.x + 8, row.y + row.height - 7, result.command->category, muted);
            painter.text(row.x + categoryWidth, row.y + row.height - 7, result.command->label, text);
            painter.text(row.x + row.width - bindingWidth * 2 - contextWidth,
                         row.y + row.height - 7, shortcut_context_name(result.context), muted);
            const std::string primary = result.bindings.primary
                ? format_shortcut_gesture(*result.bindings.primary) : std::string("-");
            const std::string secondary = result.bindings.secondary
                ? format_shortcut_gesture(*result.bindings.secondary) : std::string("-");
            painter.text(row.x + row.width - bindingWidth * 2,
                         row.y + row.height - 7, primary, accent);
            painter.text(row.x + row.width - bindingWidth,
                         row.y + row.height - 7, secondary, muted);
            painter.line(row.x, row.y + row.height - 1, row.x + row.width, row.y + row.height - 1, border);
        }
        if (rows.empty()) painter.text(shortcutLayout.searchBox.x, shortcutLayout.searchBox.y + 58,
                                       "No shortcut commands match this search.", muted);

        painter.fill(shortcutLayout.resetButton, panel2); painter.outline(shortcutLayout.resetButton, border);
        painter.text(shortcutLayout.resetButton.x + 14,
                     shortcutLayout.resetButton.y + shortcutLayout.resetButton.height - 9,
                     "Reset Profile", text);
        painter.fill(shortcutLayout.closeButton, accent); painter.outline(shortcutLayout.closeButton, border);
        painter.text(shortcutLayout.closeButton.x + 25,
                     shortcutLayout.closeButton.y + shortcutLayout.closeButton.height - 9,
                     "Close", text);
        const auto conflicts = controller.workspace().shortcuts().conflicts(
            controller.workspace().shortcuts().active_profile());
        std::string footer = controller.shortcut_panel().capturing
            ? "CAPTURE: " + controller.shortcut_panel().status
            : "Enter: bind primary | Shift+Enter: secondary | Delete: unbind | Tab: context | F5: reset";
        if (!controller.shortcut_panel().status.empty() && !controller.shortcut_panel().capturing)
            footer = controller.shortcut_panel().status + " | " + footer;
        if (!conflicts.empty()) footer += " | Conflicts: " + std::to_string(conflicts.size());
        painter.text(shortcutLayout.panel.x + 164, shortcutLayout.resetButton.y + 21, footer,
                     conflicts.empty() ? muted : rgb(255,190,80));
    }

    if (controller.command_palette_open()) {
        const NativeCommandPaletteLayout paletteLayout = controller.command_palette_layout();
        painter.fill({0, 0, width, height}, rgb(8,10,14));
        painter.fill(paletteLayout.panel, panel);
        painter.outline(paletteLayout.panel, accent);
        painter.fill(paletteLayout.searchBox, panel2);
        painter.outline(paletteLayout.searchBox, accent);
        const std::string query = controller.command_palette_query().empty()
            ? std::string("Search commands and settings, panels, assets, objects, and docs...  > @ / # : ?")
            : std::string(controller.command_palette_query()) + "_";
        painter.text(paletteLayout.searchBox.x + 12,
                     paletteLayout.searchBox.y + paletteLayout.searchBox.height - 10,
                     query, controller.command_palette_query().empty() ? muted : text);

        const auto results = controller.command_palette_results(64);
        for (std::size_t visible = 0; visible < paletteLayout.rows.size(); ++visible) {
            const std::size_t rowIndex = paletteLayout.firstVisibleRow + visible;
            if (rowIndex >= results.size()) break;
            const CommandPaletteResult& result = results[rowIndex];
            const UiRect row = paletteLayout.rows[visible];
            if (rowIndex == controller.command_palette_selection()) painter.fill(row, rgb(45,68,96));
            else if ((visible & 1U) != 0U) painter.fill(row, rgb(24,29,37));
            std::string kind;
            std::uint32_t kindColor = accent;
            switch (result.kind) {
                case CommandPaletteResultKind::Command: kind = "COMMAND"; break;
                case CommandPaletteResultKind::Setting: kind = "SETTING"; kindColor = rgb(135,200,150); break;
                case CommandPaletteResultKind::Panel: kind = "PANEL"; kindColor = rgb(130,185,255); break;
                case CommandPaletteResultKind::Asset: kind = "ASSET"; kindColor = rgb(225,180,95); break;
                case CommandPaletteResultKind::SceneObject: kind = "OBJECT"; kindColor = rgb(205,145,230); break;
                case CommandPaletteResultKind::Documentation: kind = "DOC"; kindColor = rgb(130,210,205); break;
            }
            painter.text(row.x + 8, row.y + 17, kind, kindColor);
            const std::string displayLabel = result.favorite ? "* " + result.label : result.label;
            painter.text(row.x + 86, row.y + 20, displayLabel, result.enabled ? text : muted);
            painter.text(row.x + 86, row.y + row.height - 8, result.breadcrumb, muted);
            if (!result.shortcut.empty())
                painter.text(row.x + row.width - painter.text_width(result.shortcut) - 10,
                             row.y + 20, result.shortcut, muted);
            const std::string detail = !result.enabled && !result.disabledReason.empty()
                ? result.disabledReason : result.description;
            if (!detail.empty())
                painter.text(row.x + row.width / 2, row.y + row.height - 8,
                             detail, !result.enabled ? rgb(255,150,120) : muted);
            painter.line(row.x, row.y + row.height - 1, row.x + row.width,
                         row.y + row.height - 1, border);
        }
        if (results.empty())
            painter.text(paletteLayout.searchBox.x, paletteLayout.searchBox.y + 68,
                         "No command-center result matches this search.", muted);
        painter.fill(paletteLayout.hintBar, panel2);
        painter.text(paletteLayout.hintBar.x + 8,
                     paletteLayout.hintBar.y + paletteLayout.hintBar.height - 9,
                     "Up/Down select  Enter open/run  Ctrl+D favorite command  Prefix filters available  Esc close", muted);
    }

    if (controller.pending_destructive_confirmation()) {
        painter.fill({0, 0, width, height}, rgb(0,0,0));
        const UiRect box{width/2 - 300, height/2 - 50, 600, 100};
        painter.fill(box, panel2);
        painter.outline(box, rgb(255,190,80));
        painter.text(box.x + 16, box.y + 42, std::string(controller.pending_confirmation_message()), text);
    }
}

} // namespace dve::editor
