#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "dve/display_presentation.hpp"
#include "dve/game_ui.hpp"
#include "dve/local_multiplayer_presentation.hpp"

namespace {
using namespace dve;
using namespace dve::presentation;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

ViewportOwner owner(std::uint32_t player) {
    ViewportOwner result;
    result.playerIndex = player;
    result.cameraId = 100U + player;
    result.hudCanvasId = 200U + player;
    result.inputSlot = player;
    result.audioListener = 300U + player;
    result.budgetProfile = player == 0U ? "desktop" : "mobile";
    result.primary = player == 0U;
    return result;
}

void test_profiles_and_4k_integer_scaling() {
    const auto& profiles = built_in_resolution_profiles();
    require(profiles.size() >= 8U, "built-in resolution profile inventory is incomplete");
    for (const auto& profile : profiles) require(profile.validate(), "built-in resolution profile is invalid");
    const auto* profile4k = find_resolution_profile("uhd_4k");
    require(profile4k != nullptr && profile4k->width == 3840U && profile4k->height == 2160U,
            "4K profile is missing or incorrect");

    const std::vector<std::pair<std::uint32_t, std::uint32_t>> logical{
        {320U, 180U}, {640U, 360U}, {960U, 540U}, {1280U, 720U}, {1920U, 1080U}};
    const std::vector<float> expected{12.0F, 6.0F, 4.0F, 3.0F, 2.0F};
    for (std::size_t index = 0; index < logical.size(); ++index) {
        PixelPresentationConfig config;
        config.logicalWidth = logical[index].first;
        config.logicalHeight = logical[index].second;
        config.scaleMode = PixelScaleMode::IntegerFit;
        const auto layout = compute_pixel_presentation(config, 3840U, 2160U);
        require(layout.has_value(), "4K pixel presentation failed");
        require(layout->integerScale && std::abs(layout->scaleX - expected[index]) < 0.0001F &&
                std::abs(layout->scaleY - expected[index]) < 0.0001F,
                "4K pixel presentation did not choose the exact integer scale");
        require(layout->viewportX == 0 && layout->viewportY == 0 &&
                layout->viewportWidth == 3840U && layout->viewportHeight == 2160U,
                "4K exact-aspect presentation unexpectedly letterboxed");
    }
}

DisplayTopology make_topology() {
    DisplayTopology topology;
    std::string error;
    std::vector<DisplayInfo> displays{
        {1U, "Primary 4K", {0, 0, 1920U, 1080U}, {0, 0, 1920U, 1040U},
         3840U, 2160U, 2.0F, 120.0F, true, true, true},
        {2U, "Ultrawide", {1920, 0, 2752U, 1152U}, {1920, 0, 2752U, 1112U},
         3440U, 1440U, 1.25F, 144.0F, false, false, true},
    };
    require(topology.replace(std::move(displays), &error), error.c_str());
    return topology;
}

void test_topology_placement_and_recovery() {
    auto topology = make_topology();
    require(topology.primary() != nullptr && topology.primary()->id == 1U, "primary display lookup failed");

    WindowPlacementRequest windowed;
    windowed.window = 10U;
    windowed.preferredDisplay = 2U;
    windowed.logicalWidth = 1600U;
    windowed.logicalHeight = 1000U;
    windowed.logicalX = 4500;
    windowed.logicalY = 900;
    std::string error;
    const auto placed = resolve_window_placement(topology, windowed, &error);
    require(placed.has_value(), error.c_str());
    require(placed->display == 2U && placed->clamped, "windowed placement was not clamped to the ultrawide work area");
    require(placed->drawableWidth == 2000U && placed->drawableHeight == 1250U,
            "mixed-DPI drawable dimensions are incorrect");

    WindowPlacementRequest fullscreen;
    fullscreen.window = 11U;
    fullscreen.preferredDisplay = 2U;
    fullscreen.mode = WindowMode::BorderlessFullscreen;
    const auto full = resolve_window_placement(topology, fullscreen, &error);
    require(full.has_value(), error.c_str());
    require(full->logicalRect.width == 2752U && full->drawableWidth == 3440U &&
            full->drawableHeight == 1440U, "borderless ultrawide placement is incorrect");

    std::vector<DisplayInfo> disconnected{
        {1U, "Primary 4K", {0, 0, 1920U, 1080U}, {0, 0, 1920U, 1040U},
         3840U, 2160U, 2.0F, 120.0F, true, true, true},
        {2U, "Ultrawide", {1920, 0, 2752U, 1152U}, {1920, 0, 2752U, 1112U},
         3440U, 1440U, 1.25F, 144.0F, false, false, false},
    };
    require(topology.replace(std::move(disconnected), &error), error.c_str());
    const auto recovered = resolve_window_placement(topology, fullscreen, &error);
    require(recovered.has_value() && recovered->display == 1U && recovered->recoveredDisplay,
            "disconnected display did not recover to the primary display");
    require(recovered->drawableWidth == 3840U && recovered->drawableHeight == 2160U,
            "recovered 4K fullscreen dimensions are incorrect");
}

void test_viewport_layouts_and_ownership() {
    std::string error;
    ViewportLayoutRequest request;
    request.kind = ViewportLayoutKind::VerticalTwoPlayer;
    request.owners = {owner(0U), owner(1U)};
    request.outputWidth = 3840U;
    request.outputHeight = 2160U;
    request.gutterPixels = 8U;
    const auto two = build_viewport_layout(request, &error);
    require(two.has_value(), error.c_str());
    require(two->regions.size() == 2U && two->regions[0].pixelRect.width == 1912U &&
            two->regions[1].pixelRect.x == 1924, "4K vertical split rectangles are incorrect");
    require(two->regions[0].owner.inputSlot != two->regions[1].owner.inputSlot &&
            two->regions[0].owner.audioListener != two->regions[1].owner.audioListener,
            "per-player input or audio ownership collided");

    request.kind = ViewportLayoutKind::ThreePlayerMainLeft;
    request.owners = {owner(0U), owner(1U), owner(2U)};
    request.gutterPixels = 0U;
    const auto three = build_viewport_layout(request, &error);
    require(three.has_value() && three->regions.size() == 3U, error.c_str());
    require(three->regions[0].pixelRect.width == 1920U && three->regions[0].pixelRect.height == 2160U,
            "three-player primary viewport is wrong");

    request.kind = ViewportLayoutKind::QuadFourPlayer;
    request.owners = {owner(0U), owner(1U), owner(2U), owner(3U)};
    const auto quad = build_viewport_layout(request, &error);
    require(quad.has_value() && quad->regions.size() == 4U, error.c_str());
    for (const auto& region : quad->regions)
        require(region.pixelRect.width == 1920U && region.pixelRect.height == 1080U,
                "4K four-player viewport is not a 1080p quadrant");

    request.kind = ViewportLayoutKind::PictureInPicture;
    request.owners = {owner(0U), owner(1U)};
    const auto pip = build_viewport_layout(request, &error);
    require(pip.has_value() && pip->regions[1].overlay && pip->regions[1].pixelRect.width > 900U,
            "picture-in-picture layout is missing its overlay view");

    std::vector<PixelPresentationConfig> configs(quad->regions.size());
    for (auto& config : configs) {
        config.logicalWidth = 320U;
        config.logicalHeight = 180U;
        config.scaleMode = PixelScaleMode::IntegerFit;
    }
    const auto presentations = build_viewport_pixel_presentations(*quad, configs, &error);
    require(presentations.has_value() && presentations->size() == 4U, error.c_str());
    for (const auto& presentation : *presentations)
        require(presentation.pixelLayout.integerScale && std::abs(presentation.pixelLayout.scaleX - 6.0F) < 0.0001F,
                "4K quad view did not preserve a 6x 320x180 pixel scale");
}


void test_camera_runtime_and_route_bridge() {
    std::string error;
    ViewportLayoutRequest request;
    request.kind = ViewportLayoutKind::QuadFourPlayer;
    request.owners = {owner(0U), owner(1U), owner(2U), owner(3U)};
    request.outputWidth = 3840U;
    request.outputHeight = 2160U;
    const auto layout = build_viewport_layout(request, &error);
    require(layout.has_value(), error.c_str());
    camera::GameCameraRuntime runtime;
    const auto first = synchronize_camera_viewports(runtime, *layout, {}, &error);
    require(first.has_value() && first->added == 4U && first->updated == 0U &&
            runtime.viewport_ids().size() == 4U, error.c_str());
    const auto routes = build_local_player_routes(*layout);
    require(routes.size() == 4U && routes[3].playerIndex == 3U &&
            routes[3].viewport.width == 1920U && routes[3].safeArea.width < routes[3].viewport.width,
            "local-player route table is incomplete");
    ui::UiDrawCommand command;
    command.rectangle = {0.0F, 0.0F, 320.0F, 180.0F};
    command.clipRectangle = command.rectangle;
    command.textScale = 1.0F;
    const std::vector<ui::UiDrawCommand> commands{command};
    const auto mapped = map_hud_draw_commands_to_viewport(commands, routes[0], {320.0F, 180.0F}, true, &error);
    require(mapped.size() == 1U && std::abs(mapped[0].rectangle.x - static_cast<float>(routes[0].safeArea.x)) < 0.001F &&
            std::abs(mapped[0].rectangle.width - static_cast<float>(routes[0].safeArea.width)) < 0.001F,
            "per-player HUD draw commands were not mapped into the safe area");
    const auto hudPoint = output_pixel_to_hud_logical(
        routes[0], {static_cast<float>(routes[0].viewport.x) + static_cast<float>(routes[0].viewport.width) * 0.5F,
                    static_cast<float>(routes[0].viewport.y) + static_cast<float>(routes[0].viewport.height) * 0.5F},
        {320.0F, 180.0F});
    require(hudPoint.has_value() && std::abs(hudPoint->x - 160.0F) < 0.001F &&
            std::abs(hudPoint->y - 90.0F) < 0.001F,
            "per-player pointer input did not map into HUD logical coordinates");

    request.kind = ViewportLayoutKind::VerticalTwoPlayer;
    request.owners = {owner(0U), owner(1U)};
    const auto two = build_viewport_layout(request, &error);
    require(two.has_value(), error.c_str());
    const auto second = synchronize_camera_viewports(runtime, *two, {}, &error);
    require(second.has_value() && second->updated == 2U && second->removed == 2U &&
            runtime.viewport_ids().size() == 2U, error.c_str());
    const auto* frame = runtime.frame(100U);
    require(frame != nullptr && std::abs(frame->viewport.normalizedWidth - 0.5F) < 0.0001F,
            "camera runtime did not preserve updated split-screen viewport state");
}

void test_dynamic_shared_camera() {
    SharedCameraSplitPolicy policy;
    policy.splitDelayTicks = 3U;
    policy.mergeDelayTicks = 2U;
    TwoPlayerSharedCameraController controller(policy);
    for (int tick = 0; tick < 2; ++tick)
        require(controller.update({0, 0, 0}, {30, 0, 0}).layout == ViewportLayoutKind::Single,
                "shared camera split before hysteresis delay");
    auto split = controller.update({0, 0, 0}, {30, 0, 0});
    require(split.layout == ViewportLayoutKind::VerticalTwoPlayer && split.changed,
            "horizontal player separation did not choose a left/right split");
    require(controller.update({0, 0, 0}, {10, 0, 0}).layout == ViewportLayoutKind::VerticalTwoPlayer,
            "shared camera merged before hysteresis delay");
    auto merged = controller.update({0, 0, 0}, {10, 0, 0});
    require(merged.layout == ViewportLayoutKind::Single && merged.changed,
            "shared camera did not merge after hysteresis delay");

    controller.reset();
    for (int tick = 0; tick < 3; ++tick) split = controller.update({0, 0, 0}, {0, 30, 0});
    require(split.layout == ViewportLayoutKind::HorizontalTwoPlayer,
            "vertical player separation did not choose a top/bottom split");
}

void test_budgets_and_game_settings() {
    auto topology = make_topology();
    std::string error;
    WindowPlacementRequest mainRequest;
    mainRequest.window = 1U;
    mainRequest.mode = WindowMode::BorderlessFullscreen;
    mainRequest.preferredDisplay = 1U;
    WindowPlacementRequest spectatorRequest = mainRequest;
    spectatorRequest.window = 2U;
    spectatorRequest.preferredDisplay = 2U;
    const auto main = resolve_window_placement(topology, mainRequest, &error);
    const auto spectator = resolve_window_placement(topology, spectatorRequest, &error);
    require(main.has_value() && spectator.has_value(), error.c_str());

    ViewportLayoutRequest layoutRequest;
    layoutRequest.kind = ViewportLayoutKind::QuadFourPlayer;
    layoutRequest.owners = {owner(0U), owner(1U), owner(2U), owner(3U)};
    layoutRequest.outputWidth = main->drawableWidth;
    layoutRequest.outputHeight = main->drawableHeight;
    const auto layout = build_viewport_layout(layoutRequest, &error);
    require(layout.has_value(), error.c_str());
    const std::vector<WindowPlacement> windows{*main, *spectator};
    const std::vector<ViewportLayoutPlan> layouts{*layout};
    const auto equalMix = build_audio_listener_mix(*layout, AudioListenerMixPolicy::EqualPower);
    require(equalMix.size() == 4U && std::abs(equalMix[0].gain - 0.5F) < 0.0001F &&
            equalMix[0].stereoPan < 0.0F && equalMix[1].stereoPan > 0.0F,
            "equal-power per-viewport audio routing is incorrect");
    const auto primaryMix = build_audio_listener_mix(*layout, AudioListenerMixPolicy::PrimaryOnly);
    require(primaryMix[0].gain == 1.0F && primaryMix[1].gain == 0.0F,
            "primary-listener audio policy is incorrect");
    const auto pacing = build_multi_display_frame_pacing(
        topology, windows, MultiDisplayFramePacingPolicy::LowestRefresh, 0.0F, &error);
    require(pacing.has_value() && std::abs(pacing->simulationRateHz - 120.0F) < 0.0001F &&
            pacing->windows.size() == 2U &&
            std::abs(pacing->windows[1].targetPresentRateHz - 120.0F) < 0.0001F,
            "mixed-refresh frame-pacing plan is incorrect");
    const auto independentPacing = build_multi_display_frame_pacing(
        topology, windows, MultiDisplayFramePacingPolicy::Independent, 60.0F, &error);
    require(independentPacing.has_value() && independentPacing->windows[1].independentlyPaced &&
            std::abs(independentPacing->windows[1].targetPresentRateHz - 144.0F) < 0.0001F,
            "independent multi-display pacing is incorrect");
    PresentationBudget desktop;
    desktop.maximumWindows = 2U;
    desktop.maximumViewports = 4U;
    const auto pass = evaluate_presentation_budget(desktop, windows, layouts);
    require(pass.within_budget(), "4K plus ultrawide presentation unexpectedly exceeded the desktop budget");
    desktop.maximumTotalPixels = 4'000'000ULL;
    const auto fail = evaluate_presentation_budget(desktop, windows, layouts);
    require(!fail.within_budget() && !fail.violations.empty(), "presentation budget did not report an over-budget topology");

    ui::GameSettings settings;
    settings.width = 3840U;
    settings.height = 2160U;
    settings.preferredDisplayId = 2U;
    settings.localPlayerCount = 4U;
    settings.displayMode = "borderless";
    settings.fullscreen = true;
    settings.resolutionProfile = "uhd_4k";
    settings.splitScreenLayout = "quad";
    settings.spectatorWindow = true;
    const auto parsed = ui::GameSettings::parse(settings.serialize(), &error);
    require(parsed.has_value(), error.c_str());
    require(parsed->width == 3840U && parsed->preferredDisplayId == 2U &&
            parsed->localPlayerCount == 4U && parsed->displayMode == "borderless" &&
            parsed->splitScreenLayout == "quad" && parsed->spectatorWindow,
            "version-2 game display settings did not round trip");

    const std::string legacy = "DVE_GAME_SETTINGS=1\nwidth=1920\nheight=1080\nfullscreen=1\n";
    const auto migrated = ui::GameSettings::parse(legacy, &error);
    require(migrated.has_value() && migrated->displayMode == "borderless" && migrated->fullscreen,
            "legacy fullscreen setting did not migrate to borderless mode");
}

} // namespace

int main() {
    try {
        test_profiles_and_4k_integer_scaling();
        test_topology_placement_and_recovery();
        test_viewport_layouts_and_ownership();
        test_camera_runtime_and_route_bridge();
        test_dynamic_shared_camera();
        test_budgets_and_game_settings();
        std::cout << "dve_v221_display_presentation_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_v221_display_presentation_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
