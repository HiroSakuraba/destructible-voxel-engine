#include "dve/local_multiplayer_presentation.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace dve::presentation {
namespace {
void set_error(std::string* error, std::string_view message) {
    if (error != nullptr) error->assign(message.begin(), message.end());
}
}

std::optional<CameraViewportSyncReport> synchronize_camera_viewports(
    camera::GameCameraRuntime& runtime,
    const ViewportLayoutPlan& layout,
    const CameraViewportSyncOptions& options,
    std::string* error) {
    if (!layout.validate(error)) return std::nullopt;
    if (options.renderTarget.empty() || options.firstOutputChannelBit > 30U) {
        set_error(error, "camera viewport synchronization options are invalid");
        return std::nullopt;
    }
    CameraViewportSyncReport report;
    std::set<camera::CameraViewportId> requested;
    for (std::size_t index = 0; index < layout.regions.size(); ++index) {
        const auto& region = layout.regions[index];
        camera::CameraViewportDesc desc;
        desc.id = region.owner.cameraId;
        desc.name = "Player " + std::to_string(region.owner.playerIndex + 1U);
        const std::uint32_t bit = options.firstOutputChannelBit + static_cast<std::uint32_t>(index);
        desc.outputChannelMask = 1U << bit;
        desc.normalizedX = region.normalizedX;
        desc.normalizedY = region.normalizedY;
        desc.normalizedWidth = region.normalizedWidth;
        desc.normalizedHeight = region.normalizedHeight;
        desc.renderTarget = options.renderTarget;
        desc.enabled = true;
        requested.insert(desc.id);
        if (runtime.director(desc.id) == nullptr) {
            if (!runtime.add_viewport(desc, error)) return std::nullopt;
            ++report.added;
        } else {
            if (!runtime.update_viewport(desc, error)) return std::nullopt;
            ++report.updated;
        }
        report.activeViewports.push_back(desc.id);
    }
    if (options.removeUnlistedViewports) {
        const auto existing = runtime.viewport_ids();
        for (const auto id : existing) {
            if (!requested.contains(id) && runtime.remove_viewport(id)) ++report.removed;
        }
    }
    return report;
}

std::vector<LocalPlayerRoute> build_local_player_routes(const ViewportLayoutPlan& layout) {
    std::vector<LocalPlayerRoute> routes;
    routes.reserve(layout.regions.size());
    for (const auto& region : layout.regions) {
        routes.push_back({region.owner.playerIndex, region.owner.cameraId,
                          region.owner.hudCanvasId, region.owner.inputSlot,
                          region.owner.audioListener, region.pixelRect, region.safeRect});
    }
    std::sort(routes.begin(), routes.end(), [](const LocalPlayerRoute& left, const LocalPlayerRoute& right) {
        return left.playerIndex < right.playerIndex;
    });
    return routes;
}


std::vector<ui::UiDrawCommand> map_hud_draw_commands_to_viewport(
    std::span<const ui::UiDrawCommand> commands,
    const LocalPlayerRoute& route,
    ui::UiVec2 logicalCanvasSize,
    bool useSafeArea,
    std::string* error) {
    std::vector<ui::UiDrawCommand> result;
    if (!std::isfinite(logicalCanvasSize.x) || !std::isfinite(logicalCanvasSize.y) ||
        logicalCanvasSize.x <= 0.0F || logicalCanvasSize.y <= 0.0F) {
        set_error(error, "HUD logical canvas size is invalid");
        return result;
    }
    const IntRect target = useSafeArea ? route.safeArea : route.viewport;
    if (target.width == 0U || target.height == 0U) {
        set_error(error, "HUD viewport target is empty");
        return result;
    }
    const float scaleX = static_cast<float>(target.width) / logicalCanvasSize.x;
    const float scaleY = static_cast<float>(target.height) / logicalCanvasSize.y;
    const auto map_rect = [&](ui::UiRect rectangle) {
        return ui::UiRect{
            static_cast<float>(target.x) + rectangle.x * scaleX,
            static_cast<float>(target.y) + rectangle.y * scaleY,
            rectangle.width * scaleX,
            rectangle.height * scaleY};
    };
    const ui::UiRect targetClip{static_cast<float>(target.x), static_cast<float>(target.y),
                                static_cast<float>(target.width), static_cast<float>(target.height)};
    const auto intersect = [](ui::UiRect a, ui::UiRect b) {
        const float left = std::max(a.x, b.x);
        const float top = std::max(a.y, b.y);
        const float right = std::min(a.x + a.width, b.x + b.width);
        const float bottom = std::min(a.y + a.height, b.y + b.height);
        return ui::UiRect{left, top, std::max(0.0F, right - left), std::max(0.0F, bottom - top)};
    };
    result.reserve(commands.size());
    for (auto command : commands) {
        command.rectangle = map_rect(command.rectangle);
        command.clipRectangle = intersect(map_rect(command.clipRectangle), targetClip);
        command.textScale *= std::min(scaleX, scaleY);
        result.push_back(std::move(command));
    }
    return result;
}

std::optional<ui::UiVec2> output_pixel_to_hud_logical(
    const LocalPlayerRoute& route,
    ui::UiVec2 outputPixel,
    ui::UiVec2 logicalCanvasSize,
    bool useSafeArea) noexcept {
    if (!std::isfinite(outputPixel.x) || !std::isfinite(outputPixel.y) ||
        !std::isfinite(logicalCanvasSize.x) || !std::isfinite(logicalCanvasSize.y) ||
        logicalCanvasSize.x <= 0.0F || logicalCanvasSize.y <= 0.0F) return std::nullopt;
    const IntRect target = useSafeArea ? route.safeArea : route.viewport;
    if (target.width == 0U || target.height == 0U ||
        outputPixel.x < static_cast<float>(target.x) || outputPixel.y < static_cast<float>(target.y) ||
        outputPixel.x >= static_cast<float>(target.right()) || outputPixel.y >= static_cast<float>(target.bottom()))
        return std::nullopt;
    return ui::UiVec2{
        (outputPixel.x - static_cast<float>(target.x)) * logicalCanvasSize.x /
            static_cast<float>(target.width),
        (outputPixel.y - static_cast<float>(target.y)) * logicalCanvasSize.y /
            static_cast<float>(target.height)};
}

} // namespace dve::presentation
