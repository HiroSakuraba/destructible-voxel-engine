#include <iostream>
#include <string>
#include <vector>

#include "dve/display_presentation.hpp"
#include "dve/local_multiplayer_presentation.hpp"

int main() {
    using namespace dve;
    using namespace dve::presentation;
    std::string error;
    DisplayTopology topology;
    std::vector<DisplayInfo> displays{
        {1U, "Primary 4K", {0, 0, 1920U, 1080U}, {0, 0, 1920U, 1040U},
         3840U, 2160U, 2.0F, 120.0F, true, true, true},
        {2U, "Spectator Ultrawide", {1920, 0, 2752U, 1152U}, {1920, 0, 2752U, 1112U},
         3440U, 1440U, 1.25F, 144.0F, false, false, true},
    };
    if (!topology.replace(std::move(displays), &error)) {
        std::cerr << error << '\n';
        return 1;
    }
    WindowPlacementRequest mainRequest;
    mainRequest.window = 1U;
    mainRequest.preferredDisplay = 1U;
    mainRequest.mode = WindowMode::BorderlessFullscreen;
    WindowPlacementRequest spectatorRequest = mainRequest;
    spectatorRequest.window = 2U;
    spectatorRequest.preferredDisplay = 2U;
    const auto mainWindow = resolve_window_placement(topology, mainRequest, &error);
    const auto spectatorWindow = resolve_window_placement(topology, spectatorRequest, &error);
    if (!mainWindow || !spectatorWindow) {
        std::cerr << error << '\n';
        return 1;
    }

    std::vector<ViewportOwner> owners;
    for (std::uint32_t player = 0; player < 4U; ++player) {
        owners.push_back({player, 100U + player, 200U + player, player,
                          300U + player, "desktop", player == 0U});
    }
    ViewportLayoutRequest layoutRequest;
    layoutRequest.kind = ViewportLayoutKind::QuadFourPlayer;
    layoutRequest.owners = owners;
    layoutRequest.outputWidth = mainWindow->drawableWidth;
    layoutRequest.outputHeight = mainWindow->drawableHeight;
    layoutRequest.gutterPixels = 8U;
    const auto layout = build_viewport_layout(layoutRequest, &error);
    if (!layout) {
        std::cerr << error << '\n';
        return 1;
    }
    const auto routes = build_local_player_routes(*layout);
    const std::vector<WindowPlacement> windows{*mainWindow, *spectatorWindow};
    const std::vector<ViewportLayoutPlan> layouts{*layout};
    const auto budget = evaluate_presentation_budget({}, windows, layouts);

    std::cout << "{\n"
              << "  \"version\": \"2.21\",\n"
              << "  \"main_display\": \"" << topology.find(1U)->name << "\",\n"
              << "  \"main_drawable\": [" << mainWindow->drawableWidth << ", "
              << mainWindow->drawableHeight << "],\n"
              << "  \"spectator_display\": \"" << topology.find(2U)->name << "\",\n"
              << "  \"spectator_drawable\": [" << spectatorWindow->drawableWidth << ", "
              << spectatorWindow->drawableHeight << "],\n"
              << "  \"layout\": \"quad-four-player\",\n"
              << "  \"viewports\": [\n";
    for (std::size_t index = 0; index < routes.size(); ++index) {
        const auto& route = routes[index];
        std::cout << "    {\"player\": " << route.playerIndex
                  << ", \"camera\": " << route.camera
                  << ", \"hud\": " << route.hudCanvas
                  << ", \"input\": " << route.inputSlot
                  << ", \"audio_listener\": " << route.audioListener
                  << ", \"rect\": [" << route.viewport.x << ", " << route.viewport.y
                  << ", " << route.viewport.width << ", " << route.viewport.height << "]}";
        std::cout << (index + 1U == routes.size() ? "\n" : ",\n");
    }
    std::cout << "  ],\n"
              << "  \"total_window_pixels\": " << budget.totalWindowPixels << ",\n"
              << "  \"total_viewport_pixels\": " << budget.totalViewportPixels << ",\n"
              << "  \"within_default_budget\": " << (budget.within_budget() ? "true" : "false") << "\n"
              << "}\n";
    return 0;
}
