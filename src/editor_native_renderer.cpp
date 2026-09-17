#include "dve/editor_native_renderer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstddef>
#include <string>

namespace dve::editor {
EditorColor rgb(unsigned r, unsigned g, unsigned b) noexcept {
    return ((r & 255U) << 16U) | ((g & 255U) << 8U) | (b & 255U);
}

unsigned byte(float value) noexcept {
    return static_cast<unsigned>(std::clamp(value, 0.0F, 1.0F) * 255.0F + 0.5F);
}

std::string tool_name(EditorToolId tool) {
    static constexpr std::array<std::string_view, 9> names{
        "Select", "Move", "Add", "Remove", "Paint", "Box", "Beam", "Anchor", "Rotate"
    };
    return std::string(names[static_cast<std::size_t>(tool)]);
}

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

void render_synth_panel(const IEditorCanvas& painter, NativeEditorController& controller,
                        EditorColor panel, EditorColor panel2, EditorColor border,
                        EditorColor text, EditorColor muted, EditorColor accent) {
    if (!controller.synth_panel().open()) return;
    const SynthPanelLayout& layout = controller.synth_panel().layout();
    const audio::SynthPreset preset = controller.synthesizer().preset();
    const audio::SynthMeters meters = controller.synthesizer().meters();
    const auto voices = controller.synthesizer().voices();
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
    button(layout.resetButton, "Reset preset"); button(layout.octaveDownButton, "-");
    painter.text(layout.octaveDownButton.x + 38, layout.octaveDownButton.y + 17,
                 "Oct " + std::to_string(controller.synth_panel().octave()), text);
    button(layout.octaveUpButton, "+"); button(layout.midiThruButton, "MIDI THRU", preset.midiThru);
    static constexpr std::array<std::string_view, kSynthPanelPageCount> pageNames{
        "OSC", "FILTER / ENV", "MOD MATRIX", "PERFORM", "EFFECTS", "PRESETS", "EXPRESSION"};
    for (std::size_t i = 0; i < layout.tabButtons.size(); ++i)
        button(layout.tabButtons[i], pageNames[i], static_cast<std::size_t>(controller.synth_panel().page()) == i);

    const SynthPanelPage page = controller.synth_panel().page();
    if (page == SynthPanelPage::Oscillators) {
        for (std::size_t i = 0; i < layout.oscillatorRows.size(); ++i) {
            const auto& osc = preset.oscillators[i];
            const bool selected = controller.synth_panel().selected_oscillator() == i;
            painter.fill(layout.oscillatorRows[i], selected ? rgb(39,58,82) : panel);
            painter.outline(layout.oscillatorRows[i], selected ? accent : border);
            button(layout.oscillatorEnableButtons[i], osc.enabled ? "ON" : "--", osc.enabled);
            painter.text(layout.oscillatorRows[i].x + 40, layout.oscillatorRows[i].y + 21,
                         "OSC " + std::to_string(i + 1U), text);
            button(layout.oscillatorWaveButtons[i], audio::oscillator_waveform_name(osc.waveform));
            painter.text(layout.oscillatorGainDownButtons[i].x - 38, layout.oscillatorGainDownButtons[i].y + 16, "Gain", muted);
            button(layout.oscillatorGainDownButtons[i], "-");
            painter.text(layout.oscillatorGainDownButtons[i].x + 31, layout.oscillatorGainDownButtons[i].y + 16, compact(osc.gain), text);
            button(layout.oscillatorGainUpButtons[i], "+");
            painter.text(layout.oscillatorTuneDownButtons[i].x - 38, layout.oscillatorTuneDownButtons[i].y + 16, "Semi", muted);
            button(layout.oscillatorTuneDownButtons[i], "-");
            painter.text(layout.oscillatorTuneDownButtons[i].x + 31, layout.oscillatorTuneDownButtons[i].y + 16,
                         std::to_string(static_cast<int>(osc.semitones)), text);
            button(layout.oscillatorTuneUpButtons[i], "+");
            painter.text(layout.oscillatorFineDownButtons[i].x - 42, layout.oscillatorFineDownButtons[i].y + 16, "Cents", muted);
            button(layout.oscillatorFineDownButtons[i], "-");
            painter.text(layout.oscillatorFineDownButtons[i].x + 31, layout.oscillatorFineDownButtons[i].y + 16,
                         std::to_string(static_cast<int>(osc.cents)), text);
            button(layout.oscillatorFineUpButtons[i], "+");
            painter.text(layout.oscillatorPwmDownButtons[i].x - 38, layout.oscillatorPwmDownButtons[i].y + 16, "PWM", muted);
            button(layout.oscillatorPwmDownButtons[i], "-");
            painter.text(layout.oscillatorPwmDownButtons[i].x + 31, layout.oscillatorPwmDownButtons[i].y + 16,
                         compact(osc.pwmDepth), text);
            button(layout.oscillatorPwmUpButtons[i], "+");
        }
        const auto& osc = preset.oscillators[controller.synth_panel().selected_oscillator()];
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
        } else if (osc.waveform == audio::OscillatorWaveform::Granular) {
            labels = {"Grain position", "Grain size ms", "Density Hz", "Spray", "Pitch semitones",
                      "Stereo spread", "Freeze", "Window", "Reverse", "Key tracking"};
            const std::array<std::string_view, 3> windows{"Hann", "Triangle", "Tukey"};
            values = {compact(osc.grainPosition), compact(osc.grainSizeMilliseconds), compact(osc.grainDensityHertz),
                      compact(osc.grainSpray), compact(osc.grainPitchSemitones), compact(osc.grainStereoSpread),
                      osc.grainFreeze ? "On" : "Off", std::string(windows[static_cast<std::size_t>(osc.grainWindow)]),
                      osc.sampleReverse ? "On" : "Off", osc.sampleKeyTrack ? "On" : "Off"};
        }
        for (std::size_t i = 0; i < layout.oscillatorAdvancedRows.size(); ++i) {
            painter.fill(layout.oscillatorAdvancedRows[i], panel); painter.outline(layout.oscillatorAdvancedRows[i], border);
            painter.text(layout.oscillatorAdvancedRows[i].x + 7, layout.oscillatorAdvancedRows[i].y + 18, labels[i], text);
            button(layout.oscillatorAdvancedDownButtons[i], "-");
            painter.text(layout.oscillatorAdvancedDownButtons[i].x + 32,
                         layout.oscillatorAdvancedDownButtons[i].y + 16, values[i], text);
            button(layout.oscillatorAdvancedUpButtons[i], "+");
        }
        if (osc.waveform == audio::OscillatorWaveform::Wavetable && preset.wavetable.enabled) {
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
                const bool selectedFrame = i == controller.synth_panel().selected_wavetable_frame();
                painter.fill(layout.wavetableFrameButtons[i], selectedFrame ? accent : panel2);
                painter.outline(layout.wavetableFrameButtons[i], border);
                painter.text(layout.wavetableFrameButtons[i].x + 8, layout.wavetableFrameButtons[i].y + 16,
                             "F" + std::to_string(i + 1U), text);
            }
            button(layout.wavetableNormalizeButton, "Normalize");
            button(layout.wavetableRemoveDcButton, "Remove DC");
            button(layout.wavetableAlignButton, "Align");
        }
        if ((osc.waveform == audio::OscillatorWaveform::Sample || osc.waveform == audio::OscillatorWaveform::Granular) &&
            preset.sampleBank.enabled && preset.sampleBank.frameCount > 1U) {
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
            "Filter delay", "Filter attack", "Filter hold", "Filter decay", "Filter sustain", "Filter release"};
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
            compact(preset.filter.envelope.sustainLevel), compact(preset.filter.envelope.releaseSeconds,3)};
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
            "Modulation slot", "Source", "Destination", "Amount", "Curve", "Route enabled"};
        const auto modulationInfo = controller.synthesizer().modulation_activity()[controller.synth_panel().selected_modulation_slot()];
        std::array<std::string, kSynthParameterRowCount> values{
            bool_text(lfo1.enabled), std::string(audio::lfo_waveform_name(lfo1.waveform)), compact(lfo1.rateHertz), compact(lfo1.depth),
            compact(lfo1.phase), compact(lfo1.fadeInSeconds), bool_text(lfo1.keySync), bool_text(lfo1.tempoSync), compact(lfo1.beatsPerCycle),
            bool_text(lfo2.enabled), std::string(audio::lfo_waveform_name(lfo2.waveform)), compact(lfo2.rateHertz), compact(lfo2.depth),
            compact(lfo2.phase), compact(lfo2.fadeInSeconds), bool_text(lfo2.keySync), bool_text(lfo2.tempoSync), compact(lfo2.beatsPerCycle),
            std::to_string(controller.synth_panel().selected_modulation_slot() + 1U),
            std::string(audio::modulation_source_name(slot.source)), std::string(audio::modulation_destination_name(slot.destination)),
            compact(slot.amount) + " | live " + compact(modulationInfo.currentValue),
            std::string(curve_text(slot.curve)), bool_text(slot.enabled)};
        std::array<bool, kSynthParameterRowCount> toggles{}; std::array<bool, kSynthParameterRowCount> active{};
        for (std::size_t i : {0U,6U,7U,9U,15U,16U,23U}) toggles[i] = true;
        active[0]=lfo1.enabled; active[6]=lfo1.keySync; active[7]=lfo1.tempoSync;
        active[9]=lfo2.enabled; active[15]=lfo2.keySync; active[16]=lfo2.tempoSync; active[23]=slot.enabled;
        draw_parameter_rows(labels, values, toggles, active);
        const UiRect activityTrack{layout.parameterRows[21].x + 116, layout.parameterRows[21].y + 24, 112, 4};
        painter.fill(activityTrack, rgb(28,35,46));
        const int activityCenter = activityTrack.x + activityTrack.width / 2;
        const int activityPixels = static_cast<int>(std::clamp(modulationInfo.currentValue, -1.0F, 1.0F) *
                                                    static_cast<float>(activityTrack.width / 2));
        painter.fill({std::min(activityCenter, activityCenter + activityPixels), activityTrack.y,
                      std::max(1, std::abs(activityPixels)), activityTrack.height}, accent);
        for (std::size_t i = 0; i < layout.macroRows.size(); ++i) {
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
            "Gate length", "Swing", "Octave range", "Step count", "Latch", "Retrigger envelopes"};
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
            bool_text(preset.arpeggiator.retriggerEnvelopes)};
        std::array<bool, kSynthParameterRowCount> toggles{}; std::array<bool, kSynthParameterRowCount> active{};
        for (std::size_t i : {4U,12U,22U,23U}) toggles[i] = true;
        active[4]=preset.chord.enabled; active[12]=preset.arpeggiator.enabled;
        active[22]=preset.arpeggiator.latch; active[23]=preset.arpeggiator.retriggerEnvelopes;
        draw_parameter_rows(labels, values, toggles, active);

        const std::size_t selectedStep = controller.synth_panel().selected_arpeggiator_step();
        for (std::size_t i = 0; i < layout.arpeggiatorStepButtons.size(); ++i) {
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
            "Enabled", "Condition", "Automation", "Accent", "Slide", "Transpose", "Octave", "Velocity",
            "Gate", "Probability", "Ratchets", "Tie", "Macro 1", "Macro 2", "Macro 3", "Macro 4"};
        const std::array<std::string, kSynthArpStepPropertyCount> stepValues{
            bool_text(step.enabled), std::string(condition_text(step.condition)), std::string(automation_text(step.automationCurve)),
            bool_text(step.accent), bool_text(step.slide), std::to_string(static_cast<int>(step.transpose)),
            std::to_string(static_cast<int>(step.octaveOffset)), compact(step.velocityScale), compact(step.gateScale),
            compact(step.probability), std::to_string(step.ratchets), bool_text(step.tie),
            step.macro1 < 0.0F ? "Keep" : compact(step.macro1), step.macro2 < 0.0F ? "Keep" : compact(step.macro2),
            step.macro3 < 0.0F ? "Keep" : compact(step.macro3), step.macro4 < 0.0F ? "Keep" : compact(step.macro4)};
        for (std::size_t i = 0; i < layout.arpeggiatorStepRows.size(); ++i) {
            painter.fill(layout.arpeggiatorStepRows[i], panel); painter.outline(layout.arpeggiatorStepRows[i], border);
            painter.text(layout.arpeggiatorStepRows[i].x + 7, layout.arpeggiatorStepRows[i].y + 18,
                         stepLabels[i], text);
            if (i == 0U || i == 3U || i == 4U || i == 11U) {
                const bool enabled = i == 0U ? step.enabled : (i == 3U ? step.accent : (i == 4U ? step.slide : step.tie));
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
        static constexpr std::array<std::string_view, 8> effectNames{
            "Distortion", "3-band EQ", "Chorus", "Phaser", "Delay", "Reverb", "Compressor", "Limiter"};
        const std::array<bool, 8> effects{
            preset.distortion.enabled, preset.eq.enabled, preset.chorus.enabled, preset.phaser.enabled,
            preset.delay.enabled, preset.reverb.enabled, preset.compressor.enabled, preset.limiter.enabled};
        for (std::size_t i = 0; i < layout.effectRows.size(); ++i) {
            painter.fill(layout.effectRows[i], panel); painter.outline(layout.effectRows[i], border);
            painter.text(layout.effectRows[i].x + 10, layout.effectRows[i].y + 24, effectNames[i], text);
            button(layout.effectToggleButtons[i], effects[i] ? "ON" : "OFF", effects[i]);
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
    } else {
        button(layout.presetScanButton, "Scan library");
        button(layout.presetPreviousButton, "<"); button(layout.presetNextButton, ">");
        button(layout.presetLoadButton, "Load"); button(layout.presetCaptureAButton, "Capture A");
        button(layout.presetCaptureBButton, "Capture B"); button(layout.presetMorphDownButton, "-");
        painter.text(layout.presetMorphDownButton.x + 43, layout.presetMorphDownButton.y + 18,
                     "Morph " + compact(controller.synth_panel().preset_morph_amount()), text);
        button(layout.presetMorphUpButton, "+");
        const auto& entries = controller.synth_panel().preset_library().entries();
        for (std::size_t i = 0; i < layout.presetEntryButtons.size(); ++i) {
            const std::string label = i < entries.size() ? entries[i].name : "--";
            button(layout.presetEntryButtons[i], label,
                   i < entries.size() && i == controller.synth_panel().selected_preset_entry());
        }
        painter.text(layout.panel.x + 14, layout.presetEntryButtons[3].y + 42,
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

    const int meterX = layout.panel.x + 14;
    const int meterY = layout.panel.y + layout.panel.height - 151;
    const int meterWidth = std::max(120, layout.panel.width - 28);
    painter.text(meterX, meterY, "Voices " + std::to_string(meters.activeVoices) + "/16", text);
    painter.fill({meterX, meterY + 8, meterWidth, 12}, rgb(20,25,31));
    painter.fill({meterX, meterY + 8, static_cast<int>(static_cast<float>(meterWidth) * std::min(1.0F, meters.peakLeft)), 5}, rgb(72,210,130));
    painter.fill({meterX, meterY + 15, static_cast<int>(static_cast<float>(meterWidth) * std::min(1.0F, meters.peakRight)), 5}, rgb(72,160,230));

    std::array<bool,128> activeNotes{};
    for (const auto& voice : voices) if (voice.active) activeNotes[voice.note] = true;
    const int baseNote = std::clamp(controller.synth_panel().octave() * 12 + 36, 0, 103);
    for (std::size_t i = 0; i < layout.pianoKeys.size(); ++i) {
        const int note = baseNote + static_cast<int>(i); const int pitchClass = note % 12;
        const bool black = pitchClass == 1 || pitchClass == 3 || pitchClass == 6 || pitchClass == 8 || pitchClass == 10;
        const bool active = activeNotes[static_cast<std::size_t>(note)];
        painter.fill(layout.pianoKeys[i], active ? accent : (black ? rgb(18,20,24) : rgb(220,225,232)));
        painter.outline(layout.pianoKeys[i], rgb(60,65,74));
        if (i % 12U == 0U) painter.text(layout.pianoKeys[i].x + 3, layout.pianoKeys[i].y + layout.pianoKeys[i].height - 7,
            "C" + std::to_string(note / 12 - 1), active || black ? text : rgb(30,34,40));
    }
    painter.text(layout.panel.x + 12, layout.panel.y + layout.panel.height - 9,
                 "Keys Z-M/Q-W | arrows octave | 1/3/4/5/6/7 pages | Ctrl+4 toggles", muted);
}


void render_chiptune_panel(const IEditorCanvas& painter, NativeEditorController& controller,
                           EditorColor panel, EditorColor panel2, EditorColor border,
                           EditorColor text, EditorColor muted, EditorColor accent) {
    if (!controller.chiptune_panel().open()) return;
    const auto& editor = controller.chiptune_panel();
    const auto& layout = editor.layout();
    const auto& session = editor.session();
    const auto& song = session.song();
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
        for(std::size_t i=0;i<layout.pianoKeys.size();++i){
            const int note=(session.octave()+1)*12+static_cast<int>(i);
            const int pc=note%12; const bool black=pc==1||pc==3||pc==6||pc==8||pc==10;
            painter.fill(layout.pianoKeys[i],black?rgb(18,20,24):rgb(220,225,232));
            painter.outline(layout.pianoKeys[i],rgb(60,65,74));
            if(i%12U==0U) painter.text(layout.pianoKeys[i].x+3,layout.pianoKeys[i].y+layout.pianoKeys[i].height-7,
                                      "C"+std::to_string(note/12-1),black?text:rgb(30,34,40));
        }
    }
    painter.text(layout.panel.x+12,layout.panel.y+layout.panel.height-12,
                 std::string(editor.status())+" | F1/F2/F3 pages | [] octave | Ctrl+9 toggles",muted);
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

void render_native_editor(const IEditorCanvas& painter, NativeEditorController& controller, int width, int height) {
    const bool highContrast = controller.workspace().preferences().highContrast;
    const EditorColor background = highContrast ? rgb(0,0,0) : rgb(24,27,32);
    const EditorColor panel = highContrast ? rgb(12,12,12) : rgb(34,38,45);
    const EditorColor panel2 = highContrast ? rgb(24,24,24) : rgb(42,47,56);
    const EditorColor border = highContrast ? rgb(255,255,255) : rgb(76,83,96);
    const EditorColor text = rgb(235,238,244);
    const EditorColor muted = highContrast ? rgb(210,210,210) : rgb(160,168,181);
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

    const int desiredMenuWidth = std::max(70, static_cast<int>(78.0F * controller.workspace().preferences().uiScale));
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

    for (std::size_t index = 0; index < layout.toolbarButtons.size(); ++index) {
        const UiRect rect = layout.toolbarButtons[index];
        const bool selected = static_cast<std::size_t>(controller.active_tool()) == index;
        painter.fill(rect, selected ? accent : panel2);
        painter.outline(rect, selected ? rgb(190,220,255) : border);
        const std::string label = std::to_string(index + 1) + " " + tool_name(static_cast<EditorToolId>(index));
        painter.text(rect.x + 6, rect.y + rect.height / 2 + 5, label, text);
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
    painter.fill(viewport, highContrast ? rgb(0,0,0) : rgb(19,23,29));
    if (controller.viewport_settings().showGrid) {
        for (int i = -20; i <= 20; ++i) {
            const ScreenPoint a = project_world_to_screen(controller.camera(), viewport, {static_cast<float>(i),0,-20});
            const ScreenPoint b = project_world_to_screen(controller.camera(), viewport, {static_cast<float>(i),0,20});
            const ScreenPoint c = project_world_to_screen(controller.camera(), viewport, {-20,0,static_cast<float>(i)});
            const ScreenPoint d = project_world_to_screen(controller.camera(), viewport, {20,0,static_cast<float>(i)});
            const EditorColor gridColor = i == 0 ? rgb(92,106,126) : rgb(45,51,61);
            if (a.visible && b.visible) painter.line(static_cast<int>(a.x),static_cast<int>(a.y),static_cast<int>(b.x),static_cast<int>(b.y),gridColor);
            if (c.visible && d.visible) painter.line(static_cast<int>(c.x),static_cast<int>(c.y),static_cast<int>(d.x),static_cast<int>(d.y),gridColor);
        }
    }

    const auto drawItems = controller.draw_items();
    for (const EditorVoxelDrawItem& item : drawItems) {
        const Float4 base = editor_material_display_color(controller.materials(), item.material);
        float shade = std::clamp(1.1F - item.depth * 0.018F, 0.42F, 1.0F);
        if (item.selected) shade = std::min(1.0F, shade + 0.14F);
        const EditorColor voxelColor = rgb(byte(base.x * shade), byte(base.y * shade), byte(base.z * shade));
        const int radius = std::max(1, static_cast<int>(item.pixelRadius));
        painter.fill({static_cast<int>(item.screenX) - radius, static_cast<int>(item.screenY) - radius,
                      radius * 2 + 1, radius * 2 + 1}, voxelColor);
        if (item.selected && radius >= 3)
            painter.outline({static_cast<int>(item.screenX) - radius, static_cast<int>(item.screenY) - radius,
                             radius * 2 + 1, radius * 2 + 1}, rgb(150,205,255));
        if (item.anchored && controller.viewport_settings().showAnchors) {
            painter.line(static_cast<int>(item.screenX)-3,static_cast<int>(item.screenY),
                         static_cast<int>(item.screenX)+3,static_cast<int>(item.screenY),rgb(255,218,72),2);
            painter.line(static_cast<int>(item.screenX),static_cast<int>(item.screenY)-3,
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
        painter.fill(sideRect, sideColor);
        painter.fill(item.screenBounds, faceColor);
        painter.outline(item.screenBounds, item.selected ? rgb(150,205,255) : border);
        const int available = std::max(0, item.screenBounds.width - 10);
        std::string label = item.text;
        while (!label.empty() && painter.text_width(label) > available) label.pop_back();
        if (label.size() < item.text.size() && label.size() > 3U) {
            label.resize(label.size() - 3U);
            label += "...";
        }
        if (!label.empty() && item.screenBounds.height >= 13) {
            const int labelX = item.screenBounds.x + std::max(5, (item.screenBounds.width - painter.text_width(label)) / 2);
            const int labelY = item.screenBounds.y + item.screenBounds.height / 2 + 4;
            painter.text(labelX, labelY, label, rgb(244,246,250));
        }
        if (item.selected) {
            painter.text(item.screenBounds.x + 4, item.screenBounds.y + 12,
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
        painter.outline(item.screenBounds, item.selected ? rgb(150,205,255) : tint);
        for (int ring = 1; ring <= 3; ++ring) {
            const int d = inset*ring;
            if (inner.width <= d*2 || inner.height <= d*2) break;
            painter.outline({inner.x+d, inner.y+d, inner.width-d*2, inner.height-d*2}, tint);
        }
        if (item.screenBounds.height >= 24) {
            painter.text(item.screenBounds.x+5, item.screenBounds.y+14,
                         "Gabor " + std::to_string(item.primitiveCount) +
                         " L" + std::to_string(item.maximumLodLevel),
                         item.selected ? rgb(220,235,255) : tint);
        }
    }

    for (EditorObjectId selectedId : controller.workspace().selected_objects()) {
        if (const EditorObject* selected = controller.workspace().document().find_object(selectedId)) {
            if (controller.viewport_settings().showObjectBounds)
                draw_world_box(painter, controller, object_world_bounds(*selected), rgb(100,188,255));
            if (controller.viewport_settings().showCollision)
                draw_world_box(painter, controller, object_world_bounds(*selected), rgb(255,140,58));
        }
    }
    if (controller.hover_pick()) {
        const ScreenPoint hover = project_world_to_screen(controller.camera(), viewport, controller.hover_pick()->worldPosition);
        if (hover.visible) {
            painter.outline({static_cast<int>(hover.x)-7,static_cast<int>(hover.y)-7,15,15},rgb(255,255,255));
        }
    }
    for (const GizmoScreenAxis& axis : controller.gizmo_axes()) {
        if (!axis.start.visible || !axis.end.visible) continue;
        const EditorColor axisColor = axis.axis == 1 ? rgb(244,79,83) : axis.axis == 2 ? rgb(84,220,121) : rgb(72,139,255);
        painter.line(static_cast<int>(axis.start.x),static_cast<int>(axis.start.y),
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
        painter.fill(viewport, rgb(18, 30, 48));
        const int horizon = viewport.y + viewport.height * 2 / 3;
        painter.fill({viewport.x, horizon, viewport.width, viewport.y + viewport.height - horizon},
                     rgb(29, 52, 45));
        for (int band = 0; band < 5; ++band) {
            const int offset = static_cast<int>(frame.parallaxOffset.x * static_cast<float>(band + 1)) % 180;
            const int y = viewport.y + 42 + band * 24;
            for (int x = viewport.x - 180 + offset; x < viewport.x + viewport.width; x += 180)
                painter.fill({x, y, 110, 10 + band * 3}, rgb(31 + band * 7, 55 + band * 6, 80 + band * 4));
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
            painter.fill({x, y, w, h}, rgb(shade, static_cast<std::uint8_t>(shade + 25U), 72));
            if (w >= 8 && h >= 8) painter.outline({x, y, w, h}, rgb(42, 68, 52));
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
                painter.fill(rect, color);
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
                    painter.outline(rect, outlineColor);
                }
            }
        };
        draw_sprite_items(frame.sprites, false);
        draw_sprite_items(frame.particles.renderList, true);
        if (controller.sprite_diagnostics_open()) {
            const UiRect diagnostics{viewport.x + viewport.width - 338, viewport.y + 10, 328, 238};
            painter.fill(diagnostics, rgb(11, 16, 23));
            painter.outline(diagnostics, rgb(87, 176, 245));
            painter.text(diagnostics.x + 10, diagnostics.y + 20, "SPRITE DIAGNOSTICS", rgb(220, 235, 248));
            painter.text(diagnostics.x + 10, diagnostics.y + 40,
                         "Draws " + std::to_string(diagnosticReport.drawCallCount) +
                         "  Batches " + std::to_string(diagnosticReport.batchCount) +
                         "  Breaks " + std::to_string(diagnosticReport.batchBreaks.size()), muted);
            painter.text(diagnostics.x + 10, diagnostics.y + 58,
                         "Sprites " + std::to_string(diagnosticReport.spriteCount) +
                         "  Tiles " + std::to_string(diagnosticReport.tileCount) +
                         "  Pixels " + std::to_string(diagnosticReport.visiblePixelCount), muted);
            painter.text(diagnostics.x + 10, diagnostics.y + 76,
                         "Textures " + std::to_string(diagnosticReport.textureCount) +
                         "  Palettes " + std::to_string(diagnosticReport.paletteCount) +
                         "  Missing " + std::to_string(diagnosticReport.referenceIssues.size()), muted);
            painter.text(diagnostics.x + 10, diagnostics.y + 94,
                         "Snap violations " + std::to_string(diagnosticReport.pixelSnapViolations.size()) +
                         "  Budget warnings " + std::to_string(diagnosticReport.budgetViolations.size()), muted);
            if (!diagnosticReport.atlases.empty()) {
                const SpriteAtlasDiagnostic& atlas = diagnosticReport.atlases.front();
                const int occupancy = static_cast<int>(std::lround(atlas.occupancy * 100.0));
                const int fragmentation = static_cast<int>(std::lround(atlas.packingFragmentation * 100.0));
                painter.text(diagnostics.x + 10, diagnostics.y + 112,
                             "Atlas " + std::to_string(occupancy) + "% used  " +
                             std::to_string(fragmentation) + "% fragmented", muted);
            }
            const int heatX = diagnostics.x + 10;
            const int heatY = diagnostics.y + 126;
            const int heatW = 160;
            const int heatH = 90;
            painter.fill({heatX, heatY, heatW, heatH}, rgb(6, 8, 12));
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
                    painter.fill({heatX + bx, heatY + by, 5, 5},
                        rgb(std::to_integer<unsigned>(diagnosticReport.overdraw.heatmapRgba8[offset]),
                            std::to_integer<unsigned>(diagnosticReport.overdraw.heatmapRgba8[offset + 1U]),
                            std::to_integer<unsigned>(diagnosticReport.overdraw.heatmapRgba8[offset + 2U])));
                }
            }
            painter.outline({heatX, heatY, heatW, heatH}, rgb(70, 80, 92));
            painter.text(diagnostics.x + 182, diagnostics.y + 145,
                         "Max overdraw " + std::to_string(diagnosticReport.overdraw.maximumOverdraw), muted);
            painter.text(diagnostics.x + 182, diagnostics.y + 164,
                         "Fragments " + std::to_string(diagnosticReport.fragmentCount), muted);
            painter.text(diagnostics.x + 182, diagnostics.y + 183,
                         "Sorting layers " + std::to_string(diagnosticReport.sortingLayers.size()), muted);
            painter.text(diagnostics.x + 182, diagnostics.y + 202, "Profile: Retro16Bit", muted);
        }
        const UiRect hud{viewport.x + 12, viewport.y + 12, 310, 32};
        painter.fill(hud, rgb(10, 18, 28));
        painter.outline(hud, rgb(90, 160, 225));
        painter.text(hud.x + 10, hud.y + 21, frame.hudText, text);
        painter.text(viewport.x + 12, viewport.y + viewport.height - 14,
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
                    painter.line(static_cast<int>(origin.x), static_cast<int>(origin.y),
                                 static_cast<int>(projected[i].x), static_cast<int>(projected[i].y), rgb(255,190,80));
            }
            for (std::size_t i = 0; i < projected.size(); ++i) {
                const ScreenPoint& a = projected[i];
                const ScreenPoint& b = projected[(i + 1U) % projected.size()];
                if (a.visible && b.visible)
                    painter.line(static_cast<int>(a.x), static_cast<int>(a.y),
                                 static_cast<int>(b.x), static_cast<int>(b.y), rgb(255,190,80));
            }
        }
        if (rig && settingBool("camera.preview_selected", true)) {
            const float ratio = std::clamp(settingFloat("camera.preview_size", 0.25F), 0.1F, 0.75F);
            const int previewWidth = std::max(180, static_cast<int>(static_cast<float>(viewport.width) * ratio));
            const int previewHeight = std::max(110, previewWidth * 9 / 16);
            const UiRect preview{viewport.x + viewport.width - previewWidth - 14, viewport.y + 14,
                                 previewWidth, std::min(previewHeight, viewport.height - 28)};
            painter.fill(preview, rgb(12,16,22));
            EditorCamera previewCamera;
            apply_camera_pose(previewCamera, rig->authoredPose);
            EditorViewportSettings previewSettings = controller.viewport_settings();
            previewSettings.maximumDrawVoxels = std::min<std::size_t>(previewSettings.maximumDrawVoxels, 25000U);
            const auto previewItems = build_voxel_draw_list(controller.workspace().document(), controller.materials(),
                                                            previewCamera, preview, previewSettings,
                                                            controller.workspace().selected_objects());
            for (const EditorVoxelDrawItem& item : previewItems) {
                const Float4 base = editor_material_display_color(controller.materials(), item.material);
                const float shade = std::clamp(1.08F - item.depth * 0.018F, 0.42F, 1.0F);
                const int radius = std::max(1, static_cast<int>(item.pixelRadius));
                painter.fill({static_cast<int>(item.screenX) - radius, static_cast<int>(item.screenY) - radius,
                              radius * 2 + 1, radius * 2 + 1},
                             rgb(byte(base.x * shade), byte(base.y * shade), byte(base.z * shade)));
            }
            painter.outline(preview, accent);
            painter.fill({preview.x, preview.y, preview.width, 22}, rgb(28,43,63));
            painter.text(preview.x + 7, preview.y + 16, rig->name + "  [Camera Preview]", text);
        }
    }
    const CinematicCameraOverlayState& cameraOverlays =
        controller.cinematic_camera_panel().overlays();
    const camera::CameraPostProcessProfile& overlayProfile =
        controller.cinematic_camera_panel().profile_for_scope(controller.selected_camera_rig());
    const int insetX = std::max(12, viewport.width / 20);
    const int insetY = std::max(10, viewport.height / 20);
    if (cameraOverlays.safeFrames) {
        painter.outline({viewport.x + insetX, viewport.y + insetY,
                         viewport.width - insetX * 2, viewport.height - insetY * 2}, rgb(238, 214, 110));
        painter.outline({viewport.x + insetX * 2, viewport.y + insetY * 2,
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
            painter.fill({viewport.x, viewport.y, viewport.width, matte}, rgb(3, 3, 4));
            painter.fill({viewport.x, viewport.y + viewport.height - matte, viewport.width, matte}, rgb(3, 3, 4));
        } else if (targetAspect < nativeAspect) {
            const int contentWidth = std::clamp(
                static_cast<int>(static_cast<float>(viewport.height) * targetAspect), 1, viewport.width);
            const int matte = (viewport.width - contentWidth) / 2;
            painter.fill({viewport.x, viewport.y, matte, viewport.height}, rgb(3, 3, 4));
            painter.fill({viewport.x + viewport.width - matte, viewport.y, matte, viewport.height}, rgb(3, 3, 4));
        }
    }
    if (cameraOverlays.focusPlanes) {
        const int y = viewport.y + viewport.height / 2;
        painter.line(viewport.x + insetX, y, viewport.x + viewport.width - insetX, y, rgb(90, 220, 170), 2);
        painter.text(viewport.x + insetX + 5, y - 7,
                     "FOCUS " + std::to_string(overlayProfile.focusDistanceMeters).substr(0, 6) + " m",
                     rgb(90, 220, 170));
    }
    if (cameraOverlays.splitDiopter) {
        const auto& split = overlayProfile.cinematic.splitDiopter;
        const float center = std::clamp(split.centerX, 0.0F, 1.0F);
        const int x = viewport.x + static_cast<int>(center * static_cast<float>(viewport.width));
        painter.line(x, viewport.y + insetY, x, viewport.y + viewport.height - insetY,
                     rgb(235, 130, 220), 2);
        painter.text(x + 6, viewport.y + insetY + 18, "SPLIT DIOPTER", rgb(235, 130, 220));
    }
    if (cameraOverlays.motionVectors) {
        for (int y = viewport.y + 70; y < viewport.y + viewport.height - 30; y += 72) {
            for (int x = viewport.x + 50; x < viewport.x + viewport.width - 50; x += 96)
                painter.line(x, y, x + 18, y - 7, rgb(100, 180, 255));
        }
        painter.text(viewport.x + 12, viewport.y + 42, "MOTION VECTORS", rgb(100, 180, 255));
    }
    if (cameraOverlays.exposurePreview) {
        painter.text(viewport.x + 12, viewport.y + 62,
                     "EXPOSURE PREVIEW  EV " + std::to_string(overlayProfile.exposure).substr(0, 5),
                     rgb(255, 195, 95));
    }
    if (cameraOverlays.compareUngraded) {
        const int x = viewport.x + viewport.width / 2;
        painter.line(x, viewport.y, x, viewport.y + viewport.height, rgb(245, 245, 245), 2);
        painter.text(viewport.x + 12, viewport.y + viewport.height - 12, "UNGRADED", text);
        painter.text(x + 12, viewport.y + viewport.height - 12, "GRADED", text);
    }

    painter.text(viewport.x + 12, viewport.y + 20,
                 controller.camera_mode() == camera::CameraRigMode::FreeFly
                     ? "RMB free look  WASDQE fly  Shift boost  MMB pan"
                     : "RMB orbit  MMB pan  Shift-click multi-select  T move  R rotate  X local/world",
                 muted);

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
        painter.fill(overlayRect, rgb(14, 20, 29));
        painter.outline(overlayRect, paused ? rgb(230, 174, 62) : accent);
        painter.text(overlayRect.x + 10, overlayRect.y + 19,
                     sessionMode + "  " + state + "  " + cameraState,
                     paused ? rgb(255, 216, 120) : text);
        painter.text(overlayRect.x + 10, overlayRect.y + 39,
                     "Tick " + std::to_string(telemetry.fixedTickCount) +
                         "   Runtime objects " + std::to_string(telemetry.runtimeObjectCount),
                     muted);
        if (!session.hud().interactionPrompt.empty()) {
            painter.text(overlayRect.x + 10, overlayRect.y + 61,
                         session.hud().interactionPrompt, text);
        }
        if (session.hud().selectedTool) {
            painter.text(overlayRect.x + 10, overlayRect.y + 81,
                         "Tool: " + session.hud().selectedTool->label, muted);
        }
    }

    if (layout.inspector.width > 0) {
        painter.text(layout.inspector.x + 12, layout.inspector.y + 20, "INSPECTOR", muted);
        if (controller.workspace().selected_object()) {
            if (const EditorObject* object = controller.workspace().document().find_object(*controller.workspace().selected_object())) {
                const bool renamingHere = controller.text_edit().kind == TextEditKind::ObjectName;
                painter.text(layout.inspector.x + 12, layout.inspector.y + 48,
                             renamingHere ? controller.text_edit().buffer + "_" : object->name,
                             renamingHere ? accent : text);
                painter.text(layout.inspector.x + 12, layout.inspector.y + 69,
                             "ID " + std::to_string(object->id) + "   Voxels " + std::to_string(object->voxels->occupied_voxel_count()), muted);
                const bool editingPosition = controller.text_edit().kind == TextEditKind::Position;
                if (editingPosition) painter.fill(layout.inspectorFields.empty() ? UiRect{} : layout.inspectorFields[0], rgb(40,54,74));
                painter.text(layout.inspector.x + 12, layout.inspector.y + 91,
                             editingPosition
                                 ? "Position  " + controller.text_edit().buffer + "_"
                                 : "Position  " + std::to_string(object->transform.position.x).substr(0,5) + "  " +
                                       std::to_string(object->transform.position.y).substr(0,5) + "  " +
                                       std::to_string(object->transform.position.z).substr(0,5),
                             editingPosition ? accent : muted);
                constexpr float degreesPerRadian = 57.29577951308232F;
                const Float3 euler = multiply(quaternion_to_euler_xyz(object->transform.rotation), degreesPerRadian);
                const bool editingRotation = controller.text_edit().kind == TextEditKind::Rotation;
                if (editingRotation) painter.fill(layout.inspectorFields.size() < 2 ? UiRect{} : layout.inspectorFields[1], rgb(40,54,74));
                painter.text(layout.inspector.x + 12, layout.inspector.y + 113,
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
                            painter.fill(layout.inspectorFields[fieldIndex], rgb(40,54,74));
                        painter.text(layout.inspector.x + 12, layout.inspector.y + y,
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
                    painter.text(layout.inspector.x + 12, layout.inspector.y + 333,
                                 "Glyphs " + std::to_string(asset.glyphInstances.size()) +
                                 "   Curves " + std::to_string(asset.atlas.curveTexels.size()/2U), muted);
                    const auto dependency = inspect_text3d_font_dependency(
                        controller.project_root(), object->textFontAsset);
                    painter.text(layout.inspector.x + 12, layout.inspector.y + 355,
                                 dependency.packageReady ? "Font dependency ready"
                                                         : "Font license/dependency warning",
                                 dependency.packageReady ? muted : rgb(235,180,80));
                    painter.text(layout.inspector.x + 12, layout.inspector.y + 377,
                                 "Hash " + std::to_string(asset.contentHash), muted);
                } else if (object->gaborVolume) {
                    const auto& asset = *object->gaborVolume;
                    const auto& material = asset.material;
                    const auto drawGaborField = [&](std::size_t fieldIndex, TextEditKind kind,
                                                    int y, std::string label, std::string value) {
                        const bool editing = controller.text_edit().kind == kind;
                        if (editing && fieldIndex < layout.inspectorFields.size())
                            painter.fill(layout.inspectorFields[fieldIndex], rgb(40,54,74));
                        painter.text(layout.inspector.x + 12, layout.inspector.y + y,
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
                    painter.text(layout.inspector.x + 12, layout.inspector.y + 267,
                                 "Primitives " + std::to_string(asset.primitives.size()) +
                                 "   Levels " + std::to_string(static_cast<unsigned>(maximumLod)+1U), muted);
                    painter.text(layout.inspector.x + 12, layout.inspector.y + 289,
                                 "Source " + object->sourceAsset.generic_string(), muted);
                    painter.text(layout.inspector.x + 12, layout.inspector.y + 311,
                                 "Hash " + std::to_string(asset.contentHash), muted);
                } else {
                    painter.text(layout.inspector.x + 12, layout.inspector.y + 135,
                                 "Selection " + std::to_string(controller.workspace().selection_count()) +
                                 "   Axes " + (controller.transform_space() == EditorTransformSpace::World ? "World" : "Local"), text);
                    const EditorSelectionDiagnostics diagnostics = controller.selection_diagnostics();
                    painter.text(layout.inspector.x + 12, layout.inspector.y + 157,
                                 "Mass " + std::to_string(diagnostics.massKilograms).substr(0,8) + " kg", muted);
                    painter.text(layout.inspector.x + 12, layout.inspector.y + 179,
                                 "Components " + std::to_string(diagnostics.connectedComponents) +
                                 "   Detached " + std::to_string(diagnostics.detachedComponents), muted);
                    painter.text(layout.inspector.x + 12, layout.inspector.y + 201,
                                 "Collision boxes " + std::to_string(diagnostics.collisionBoxes), muted);
                    painter.text(layout.inspector.x + 12, layout.inspector.y + 223,
                                 "Material " + std::to_string(controller.active_material()) + "  " +
                                 (controller.materials().find(controller.active_material()) ?
                                  controller.materials().find(controller.active_material())->definition.name : "Unknown"), text);
                    painter.text(layout.inspector.x + 12, layout.inspector.y + 245,
                                 "Object components " + std::to_string(object->components.size()), muted);
                    if (object->attachment && object->parent) {
                        const std::string socket = object->attachment->socket.empty()
                            ? std::string("default") : object->attachment->socket;
                        painter.text(layout.inspector.x + 12, layout.inspector.y + 267,
                                     "Attached to " + std::to_string(*object->parent) + "  socket " + socket, muted);
                    } else {
                        painter.text(layout.inspector.x + 12, layout.inspector.y + 267,
                                     "Attachment  none", muted);
                    }
                    if (object->prefabLink) {
                        painter.text(layout.inspector.x + 12, layout.inspector.y + 289,
                                     "Prefab " + object->prefabLink->prefabAsset.filename().generic_string() +
                                     "  overrides " + std::to_string(object->prefabLink->overrides.size()), accent);
                    } else {
                        painter.text(layout.inspector.x + 12, layout.inspector.y + 289,
                                     "Prefab  none", muted);
                    }
                    painter.text(layout.inspector.x + 12, layout.inspector.y + 311,
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
                        painter.text(layout.inspector.x + 12,
                                     layout.inspector.y + 333 + static_cast<int>(componentIndex) * 20,
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
                         "Draw items: " + std::to_string(controller.draw_items().size()) +
                         "   UI scale: " + std::to_string(controller.workspace().preferences().uiScale).substr(0,4), muted);
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
            const std::string summary = std::to_string(entries.size()) + " shown / " +
                std::to_string(controller.asset_database().records().size()) + " indexed";
            painter.text(layout.assetSearchBox.x,
                         layout.assetSearchBox.y + layout.assetSearchBox.height + 14,
                         summary, muted);
            break;
        }
    }

    const EditorStatusMessage& status = controller.status();
    painter.text(10, layout.statusBar.y + layout.statusBar.height - 6, status.text,
                 status.error ? rgb(255,105,105) : text);
    const std::string scale = "UI " + std::to_string(controller.workspace().preferences().uiScale).substr(0,3) + "x" +
                               "  Snap " + std::to_string(controller.workspace().preferences().translateSnapMeters).substr(0,5) + "m";
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
            painter.text(labelX, y + itemHeight - 7, actions[i].label, labelColor);
            std::string rightText = actions[i].shortcut;
            if (!actions[i].enabled && !actions[i].disabledReason.empty()) rightText = "Unavailable";
            else if (actions[i].visibility == MenuVisibility::Advanced && rightText.empty()) rightText = "Advanced";
            else if (rightText.empty()) rightText = actions[i].section;
            if (!rightText.empty()) {
                painter.text(popup.x + popup.width - painter.text_width(rightText) - 8,
                             y + itemHeight - 7, rightText, muted);
            }
        }
        if (popupLayout.canScrollUp) painter.text(popup.x + popup.width - 18, popup.y + 14, "^", muted);
        if (popupLayout.canScrollDown)
            painter.text(popup.x + popup.width - 18, popup.y + popup.height - 6, "v", muted);
    }

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
        const int itemHeight = std::max(22, static_cast<int>(24.0F * controller.workspace().preferences().uiScale));
        const UiRect popup{menu.x, menu.y, 190, itemHeight * static_cast<int>(menu.items.size())};
        painter.fill(popup, panel2);
        painter.outline(popup, border);
        for (std::size_t i = 0; i < menu.items.size(); ++i) {
            if (menu.hoveredItem && *menu.hoveredItem == i) painter.fill(menu.itemRects[i], rgb(48,88,142));
            painter.text(popup.x + 10, popup.y + static_cast<int>(i) * itemHeight + itemHeight - 7, menu.items[i].label, text);
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
    render_cinematic_camera_panel(painter, controller, panel, panel2, border, text, muted, accent);

    if (controller.settings_panel().open) {
        const NativeSettingsModalLayout settingsLayout = controller.settings_modal_layout();
        painter.fill({0, 0, width, height}, rgb(8,10,14));
        painter.fill(settingsLayout.panel, panel);
        painter.outline(settingsLayout.panel, accent);
        painter.text(settingsLayout.panel.x + 16, settingsLayout.panel.y + 31,
                     "Settings and Preferences", text);

        static constexpr std::array<SettingScope, 3> scopes{
            SettingScope::User, SettingScope::Project, SettingScope::Session};
        for (std::size_t index = 0; index < settingsLayout.scopeTabs.size(); ++index) {
            const UiRect tab = settingsLayout.scopeTabs[index];
            painter.fill(tab, controller.settings_panel().scope == scopes[index] ? accent : panel2);
            painter.outline(tab, border);
            painter.text(tab.x + 10, tab.y + tab.height - 8, setting_scope_name(scopes[index]), text);
        }

        painter.fill(settingsLayout.searchBox, panel2);
        painter.outline(settingsLayout.searchBox, border);
        const std::string search = controller.settings_panel().searchQuery.empty()
            ? std::string("Search every option by name, description, or keyword...")
            : controller.settings_panel().searchQuery + "_";
        painter.text(settingsLayout.searchBox.x + 8,
                     settingsLayout.searchBox.y + settingsLayout.searchBox.height - 8,
                     search, controller.settings_panel().searchQuery.empty() ? muted : text);

        painter.fill(settingsLayout.changedToggle,
                     controller.settings_panel().changedOnly ? accent : panel2);
        painter.outline(settingsLayout.changedToggle, border);
        painter.text(settingsLayout.changedToggle.x + 8,
                     settingsLayout.changedToggle.y + settingsLayout.changedToggle.height - 8,
                     controller.settings_panel().changedOnly ? "Changed: On" : "Changed: Off", text);

        painter.fill(settingsLayout.advancedToggle,
                     controller.settings_panel().includeAdvanced ? accent : panel2);
        painter.outline(settingsLayout.advancedToggle, border);
        painter.text(settingsLayout.advancedToggle.x + 8,
                     settingsLayout.advancedToggle.y + settingsLayout.advancedToggle.height - 8,
                     controller.settings_panel().includeAdvanced ? "Advanced: On" : "Advanced: Off", text);

        const auto categories = controller.settings_categories();
        for (std::size_t index = 0; index < settingsLayout.categoryRows.size() && index < categories.size(); ++index) {
            const UiRect row = settingsLayout.categoryRows[index];
            if (categories[index] == controller.settings_panel().selectedCategory &&
                controller.settings_panel().searchQuery.empty()) painter.fill(row, rgb(48,78,118));
            painter.text(row.x + 8, row.y + row.height - 8, categories[index], text);
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
            std::string valueText = setting_value_to_string(displayed);
            if (controller.settings_panel().valueEditing &&
                controller.settings_panel().valueEditId == definition.id)
                valueText = controller.settings_panel().valueEditBuffer + "_";
            const std::uint32_t rowText = availability.available ? text : muted;
            painter.text(row.x + 8, row.y + row.height - 8, definition.label, rowText);
            const int valueX = row.x + row.width * 2 / 3;
            painter.text(valueX, row.y + row.height - 8, valueText,
                         availability.available ? accent : muted);
            std::string marker;
            if (controller.settings_panel().stagedValues.contains(definition.id)) marker = "modified";
            else if (controller.settings_panel().stagedClears.contains(definition.id)) marker = "reset";
            else if (controller.workspace().settings().has_override(controller.settings_panel().scope, definition.id))
                marker = setting_scope_name(controller.settings_panel().scope);
            else if (inherited) marker = "from " + setting_scope_name(source);
            if (!marker.empty())
                painter.text(valueX - painter.text_width(marker) - 12, row.y + row.height - 8, marker, muted);
            std::string policy;
            if (definition.applyPolicy == SettingApplyPolicy::RestartRequired) policy = "restart";
            else if (definition.applyPolicy == SettingApplyPolicy::OnApply) policy = "apply";
            if (!policy.empty()) painter.text(row.x + row.width - painter.text_width(policy) - 8,
                                               row.y + row.height - 8, policy, muted);
        }
        if (rows.empty()) painter.text(settingsLayout.searchBox.x, settingsLayout.searchBox.y + 66,
                                       controller.settings_panel().changedOnly
                                           ? "No changed options match this view."
                                           : "No settings match this search.", muted);

        painter.fill(settingsLayout.detailPanel, panel2);
        painter.outline(settingsLayout.detailPanel, border);
        if (controller.settings_panel().selectedRow < rows.size()) {
            const SettingDefinition& definition = *rows[controller.settings_panel().selectedRow];
            painter.text(settingsLayout.detailPanel.x + 10, settingsLayout.detailPanel.y + 22,
                         definition.category + " > " + definition.section + " > " + definition.label, accent);
            painter.text(settingsLayout.detailPanel.x + 10, settingsLayout.detailPanel.y + 45,
                         definition.description, text);
            std::string details = "Default: " + setting_value_to_string(definition.defaultValue);
            if (definition.minimum) details += "  Min: " + std::to_string(*definition.minimum);
            if (definition.maximum) details += "  Max: " + std::to_string(*definition.maximum);
            painter.text(settingsLayout.detailPanel.x + 10, settingsLayout.detailPanel.y + 68, details, muted);
            const SettingAvailability availability = controller.workspace().settings().availability(
                definition.id, controller.settings_capabilities());
            if (!availability.available)
                painter.text(settingsLayout.detailPanel.x + 10, settingsLayout.detailPanel.y + 88,
                             availability.explanation, rgb(255,190,80));
        }
        if (!controller.settings_panel().status.empty())
            painter.text(settingsLayout.detailPanel.x + settingsLayout.detailPanel.width / 2,
                         settingsLayout.detailPanel.y + settingsLayout.detailPanel.height - 10,
                         controller.settings_panel().status, muted);

        painter.fill(settingsLayout.resetSettingButton, panel2);
        painter.outline(settingsLayout.resetSettingButton, border);
        painter.text(settingsLayout.resetSettingButton.x + 12,
                     settingsLayout.resetSettingButton.y + settingsLayout.resetSettingButton.height - 9,
                     "Reset Option", text);
        painter.fill(settingsLayout.resetCategoryButton, panel2);
        painter.outline(settingsLayout.resetCategoryButton, border);
        painter.text(settingsLayout.resetCategoryButton.x + 12,
                     settingsLayout.resetCategoryButton.y + settingsLayout.resetCategoryButton.height - 9,
                     "Reset Category", text);
        painter.fill(settingsLayout.discardButton, panel2);
        painter.outline(settingsLayout.discardButton, border);
        painter.text(settingsLayout.discardButton.x + 18,
                     settingsLayout.discardButton.y + settingsLayout.discardButton.height - 9, "Discard", text);
        painter.fill(settingsLayout.applyButton, controller.settings_panel().dirty ? accent : panel2);
        painter.outline(settingsLayout.applyButton, border);
        painter.text(settingsLayout.applyButton.x + 24,
                     settingsLayout.applyButton.y + settingsLayout.applyButton.height - 9, "Apply", text);
        painter.text(settingsLayout.panel.x + 310, settingsLayout.applyButton.y + 22,
                     "Enter/F2 edits | arrows adjust | Ctrl+R resets | F3 changed | Ctrl+Tab scope", muted);
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
