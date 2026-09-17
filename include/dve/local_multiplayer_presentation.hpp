#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dve/camera_runtime.hpp"
#include "dve/display_presentation.hpp"
#include "dve/game_ui.hpp"

namespace dve::presentation {

struct CameraViewportSyncOptions {
    std::string renderTarget{"main"};
    std::uint32_t firstOutputChannelBit{};
    bool removeUnlistedViewports{true};
};

struct CameraViewportSyncReport {
    std::uint32_t added{};
    std::uint32_t updated{};
    std::uint32_t removed{};
    std::vector<camera::CameraViewportId> activeViewports;
};

[[nodiscard]] std::optional<CameraViewportSyncReport> synchronize_camera_viewports(
    camera::GameCameraRuntime& runtime,
    const ViewportLayoutPlan& layout,
    const CameraViewportSyncOptions& options = {},
    std::string* error = nullptr);

struct LocalPlayerRoute {
    std::uint32_t playerIndex{};
    camera::CameraViewportId camera{};
    std::uint64_t hudCanvas{};
    InputSlotId inputSlot{};
    AudioListenerId audioListener{};
    IntRect viewport{};
    IntRect safeArea{};
};

[[nodiscard]] std::vector<LocalPlayerRoute> build_local_player_routes(
    const ViewportLayoutPlan& layout);


[[nodiscard]] std::vector<ui::UiDrawCommand> map_hud_draw_commands_to_viewport(
    std::span<const ui::UiDrawCommand> commands,
    const LocalPlayerRoute& route,
    ui::UiVec2 logicalCanvasSize,
    bool useSafeArea = true,
    std::string* error = nullptr);

[[nodiscard]] std::optional<ui::UiVec2> output_pixel_to_hud_logical(
    const LocalPlayerRoute& route,
    ui::UiVec2 outputPixel,
    ui::UiVec2 logicalCanvasSize,
    bool useSafeArea = false) noexcept;

} // namespace dve::presentation
