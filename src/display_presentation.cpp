#include "dve/display_presentation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

namespace dve::presentation {
namespace {

void set_error(std::string* error, std::string_view message) {
    if (error != nullptr) error->assign(message.begin(), message.end());
}

std::uint32_t scaled_extent(std::uint32_t logical, float scale) noexcept {
    const double result = std::round(static_cast<double>(logical) * static_cast<double>(scale));
    if (result <= 1.0) return 1U;
    if (result >= static_cast<double>(std::numeric_limits<std::uint32_t>::max()))
        return std::numeric_limits<std::uint32_t>::max();
    return static_cast<std::uint32_t>(result);
}

IntRect clamp_rect(IntRect rect, const IntRect& bounds, bool& clamped) noexcept {
    const std::uint32_t width = std::min(rect.width, bounds.width);
    const std::uint32_t height = std::min(rect.height, bounds.height);
    const std::int64_t maxX = bounds.right() - static_cast<std::int64_t>(width);
    const std::int64_t maxY = bounds.bottom() - static_cast<std::int64_t>(height);
    const auto x = static_cast<std::int32_t>(std::clamp<std::int64_t>(rect.x, bounds.x, maxX));
    const auto y = static_cast<std::int32_t>(std::clamp<std::int64_t>(rect.y, bounds.y, maxY));
    clamped = clamped || width != rect.width || height != rect.height || x != rect.x || y != rect.y;
    return {x, y, width, height};
}

ViewportRegion make_region(const ViewportOwner& owner, std::uint32_t outputWidth,
                           std::uint32_t outputHeight, float nx, float ny, float nw, float nh,
                           std::uint32_t gutter, float safeFraction, bool overlay) {
    const auto px = static_cast<std::uint32_t>(std::llround(static_cast<double>(outputWidth) * nx));
    const auto py = static_cast<std::uint32_t>(std::llround(static_cast<double>(outputHeight) * ny));
    const auto pr = static_cast<std::uint32_t>(std::llround(static_cast<double>(outputWidth) * (nx + nw)));
    const auto pb = static_cast<std::uint32_t>(std::llround(static_cast<double>(outputHeight) * (ny + nh)));
    IntRect rect{static_cast<std::int32_t>(px), static_cast<std::int32_t>(py),
                 pr > px ? pr - px : 1U, pb > py ? pb - py : 1U};
    const std::uint32_t halfGutter = gutter / 2U;
    if (rect.width > gutter) {
        rect.x += static_cast<std::int32_t>(halfGutter);
        rect.width -= gutter;
    }
    if (rect.height > gutter) {
        rect.y += static_cast<std::int32_t>(halfGutter);
        rect.height -= gutter;
    }
    const auto insetX = static_cast<std::uint32_t>(std::floor(static_cast<double>(rect.width) * safeFraction));
    const auto insetY = static_cast<std::uint32_t>(std::floor(static_cast<double>(rect.height) * safeFraction));
    IntRect safe = rect;
    if (safe.width > insetX * 2U) {
        safe.x += static_cast<std::int32_t>(insetX);
        safe.width -= insetX * 2U;
    }
    if (safe.height > insetY * 2U) {
        safe.y += static_cast<std::int32_t>(insetY);
        safe.height -= insetY * 2U;
    }
    return {owner, rect, safe, nx, ny, nw, nh, overlay};
}

bool unique_owners(std::span<const ViewportOwner> owners, std::string* error) {
    std::set<std::uint32_t> players;
    std::set<std::uint64_t> cameras;
    std::set<std::uint64_t> huds;
    std::set<InputSlotId> inputs;
    std::set<AudioListenerId> listeners;
    for (const auto& owner : owners) {
        if (!owner.validate(error)) return false;
        if (!players.insert(owner.playerIndex).second || !cameras.insert(owner.cameraId).second ||
            !huds.insert(owner.hudCanvasId).second || !inputs.insert(owner.inputSlot).second ||
            !listeners.insert(owner.audioListener).second) {
            set_error(error, "viewport owners must have unique player, camera, HUD, input, and audio identities");
            return false;
        }
    }
    return true;
}

} // namespace

std::int64_t IntRect::right() const noexcept {
    return static_cast<std::int64_t>(x) + static_cast<std::int64_t>(width);
}
std::int64_t IntRect::bottom() const noexcept {
    return static_cast<std::int64_t>(y) + static_cast<std::int64_t>(height);
}
bool IntRect::contains(std::int32_t px, std::int32_t py) const noexcept {
    return static_cast<std::int64_t>(px) >= x && static_cast<std::int64_t>(py) >= y &&
           static_cast<std::int64_t>(px) < right() && static_cast<std::int64_t>(py) < bottom();
}
bool IntRect::intersects(const IntRect& other) const noexcept {
    return static_cast<std::int64_t>(x) < other.right() && right() > other.x &&
           static_cast<std::int64_t>(y) < other.bottom() && bottom() > other.y;
}

bool ResolutionProfile::validate(std::string* error) const {
    if (id.empty() || label.empty()) { set_error(error, "resolution profile requires an ID and label"); return false; }
    if (width == 0U || height == 0U || width > 16384U || height > 16384U) {
        set_error(error, "resolution profile dimensions must be within 1..16384"); return false;
    }
    if (portrait != (height > width)) { set_error(error, "resolution portrait flag does not match dimensions"); return false; }
    return true;
}

const std::vector<ResolutionProfile>& built_in_resolution_profiles() {
    static const std::vector<ResolutionProfile> profiles{
        {"hd_720p", "HD 1280x720", 1280U, 720U, false, false, true},
        {"fhd_1080p", "Full HD 1920x1080", 1920U, 1080U, false, false, true},
        {"qhd_1440p", "QHD 2560x1440", 2560U, 1440U, false, false, true},
        {"uhd_4k", "4K UHD 3840x2160", 3840U, 2160U, false, false, true},
        {"uwqhd", "Ultrawide QHD 3440x1440", 3440U, 1440U, true, false, true},
        {"dual_qhd", "Super Ultrawide 5120x1440", 5120U, 1440U, true, false, false},
        {"portrait_fhd", "Portrait Full HD 1080x1920", 1080U, 1920U, false, true, true},
        {"portrait_4k", "Portrait 4K 2160x3840", 2160U, 3840U, false, true, false},
        {"5k", "5K 5120x2880", 5120U, 2880U, false, false, false},
        {"8k", "8K UHD 7680x4320", 7680U, 4320U, false, false, false},
    };
    return profiles;
}

const ResolutionProfile* find_resolution_profile(std::string_view id) noexcept {
    const auto& profiles = built_in_resolution_profiles();
    const auto found = std::find_if(profiles.begin(), profiles.end(),
                                    [id](const ResolutionProfile& value) { return value.id == id; });
    return found == profiles.end() ? nullptr : &*found;
}

bool DisplayInfo::validate(std::string* error) const {
    if (id == kInvalidDisplayId || name.empty()) { set_error(error, "display requires a nonzero ID and name"); return false; }
    if (logicalBounds.width == 0U || logicalBounds.height == 0U ||
        logicalWorkArea.width == 0U || logicalWorkArea.height == 0U) {
        set_error(error, "display bounds and work area must be nonempty"); return false;
    }
    if (!logicalBounds.intersects(logicalWorkArea) || nativePixelWidth == 0U || nativePixelHeight == 0U) {
        set_error(error, "display work area or native pixel dimensions are invalid"); return false;
    }
    if (!std::isfinite(contentScale) || contentScale < 0.5F || contentScale > 8.0F ||
        !std::isfinite(refreshRateHz) || refreshRateHz < 20.0F || refreshRateHz > 1000.0F) {
        set_error(error, "display content scale or refresh rate is outside supported bounds"); return false;
    }
    return true;
}

bool DisplayTopology::replace(std::vector<DisplayInfo> displays, std::string* error) {
    if (displays.empty()) { set_error(error, "display topology requires at least one display"); return false; }
    std::set<DisplayId> ids;
    std::size_t primaryCount = 0U;
    for (const auto& display : displays) {
        if (!display.validate(error)) return false;
        if (!ids.insert(display.id).second) { set_error(error, "display topology contains duplicate IDs"); return false; }
        if (display.primary && display.connected) ++primaryCount;
    }
    if (primaryCount != 1U) { set_error(error, "display topology requires exactly one connected primary display"); return false; }
    displays_ = std::move(displays);
    ++generation_;
    return true;
}

const DisplayInfo* DisplayTopology::find(DisplayId id) const noexcept {
    const auto found = std::find_if(displays_.begin(), displays_.end(),
                                    [id](const DisplayInfo& value) { return value.id == id && value.connected; });
    return found == displays_.end() ? nullptr : &*found;
}
const DisplayInfo* DisplayTopology::primary() const noexcept {
    const auto found = std::find_if(displays_.begin(), displays_.end(),
                                    [](const DisplayInfo& value) { return value.primary && value.connected; });
    return found == displays_.end() ? nullptr : &*found;
}
DisplayId DisplayTopology::recover(DisplayId preferred) const noexcept {
    if (find(preferred) != nullptr) return preferred;
    const auto* display = primary();
    return display != nullptr ? display->id : kInvalidDisplayId;
}

bool WindowPlacement::validate(std::string* error) const {
    if (window == kInvalidWindowId || display == kInvalidDisplayId || logicalRect.width == 0U ||
        logicalRect.height == 0U || drawableWidth == 0U || drawableHeight == 0U) {
        set_error(error, "window placement is missing identity or dimensions"); return false;
    }
    if (!std::isfinite(contentScale) || contentScale <= 0.0F ||
        !std::isfinite(refreshRateHz) || refreshRateHz <= 0.0F) {
        set_error(error, "window placement scale or refresh rate is invalid"); return false;
    }
    return true;
}

std::optional<WindowPlacement> resolve_window_placement(const DisplayTopology& topology,
                                                         const WindowPlacementRequest& request,
                                                         std::string* error) {
    if (request.window == kInvalidWindowId || request.logicalWidth == 0U || request.logicalHeight == 0U) {
        set_error(error, "window placement request requires identity and positive dimensions"); return std::nullopt;
    }
    const DisplayId recovered = topology.recover(request.preferredDisplay);
    const DisplayInfo* display = topology.find(recovered);
    if (display == nullptr) { set_error(error, "window placement cannot resolve a connected display"); return std::nullopt; }
    WindowPlacement result;
    result.window = request.window;
    result.display = display->id;
    result.mode = request.mode;
    result.contentScale = display->contentScale;
    result.refreshRateHz = display->refreshRateHz;
    result.recoveredDisplay = request.preferredDisplay != kInvalidDisplayId && request.preferredDisplay != display->id;
    if (result.recoveredDisplay) result.recoveryReason = "preferred display is unavailable; moved to primary display";

    if (request.mode == WindowMode::Windowed) {
        IntRect rect{};
        rect.width = request.logicalWidth;
        rect.height = request.logicalHeight;
        if (request.logicalX.has_value()) rect.x = *request.logicalX;
        else if (request.centerIfUnspecified)
            rect.x = display->logicalWorkArea.x + static_cast<std::int32_t>((display->logicalWorkArea.width - std::min(rect.width, display->logicalWorkArea.width)) / 2U);
        else rect.x = display->logicalWorkArea.x;
        if (request.logicalY.has_value()) rect.y = *request.logicalY;
        else if (request.centerIfUnspecified)
            rect.y = display->logicalWorkArea.y + static_cast<std::int32_t>((display->logicalWorkArea.height - std::min(rect.height, display->logicalWorkArea.height)) / 2U);
        else rect.y = display->logicalWorkArea.y;
        if (request.clampToWorkArea) rect = clamp_rect(rect, display->logicalWorkArea, result.clamped);
        result.logicalRect = rect;
        result.drawableWidth = scaled_extent(rect.width, display->contentScale);
        result.drawableHeight = scaled_extent(rect.height, display->contentScale);
    } else {
        result.logicalRect = display->logicalBounds;
        result.drawableWidth = display->nativePixelWidth;
        result.drawableHeight = display->nativePixelHeight;
    }
    if (!result.validate(error)) return std::nullopt;
    return result;
}

bool ViewportOwner::validate(std::string* error) const {
    if (cameraId == 0U || hudCanvasId == 0U || audioListener == 0U || budgetProfile.empty()) {
        set_error(error, "viewport owner requires camera, HUD, audio listener, and budget identities"); return false;
    }
    return true;
}

bool ViewportLayoutPlan::validate(std::string* error) const {
    if (outputWidth == 0U || outputHeight == 0U || regions.empty()) {
        set_error(error, "viewport layout plan requires output dimensions and regions"); return false;
    }
    std::vector<ViewportOwner> owners;
    owners.reserve(regions.size());
    for (const auto& region : regions) {
        owners.push_back(region.owner);
        if (region.pixelRect.width == 0U || region.pixelRect.height == 0U ||
            region.pixelRect.x < 0 || region.pixelRect.y < 0 ||
            region.pixelRect.right() > outputWidth || region.pixelRect.bottom() > outputHeight) {
            set_error(error, "viewport region is outside the output target"); return false;
        }
        if (!region.pixelRect.intersects(region.safeRect)) {
            set_error(error, "viewport safe region does not overlap its viewport"); return false;
        }
    }
    return unique_owners(owners, error);
}

std::optional<ViewportLayoutPlan> build_viewport_layout(const ViewportLayoutRequest& request,
                                                         std::string* error) {
    if (request.outputWidth == 0U || request.outputHeight == 0U || request.owners.empty() ||
        request.owners.size() > 4U || !std::isfinite(request.safeAreaFraction) ||
        request.safeAreaFraction < 0.0F || request.safeAreaFraction >= 0.25F ||
        !std::isfinite(request.pictureInPictureFraction) || request.pictureInPictureFraction < 0.15F ||
        request.pictureInPictureFraction > 0.5F) {
        set_error(error, "viewport layout request is invalid"); return std::nullopt;
    }
    if (!unique_owners(request.owners, error)) return std::nullopt;
    ViewportLayoutPlan plan;
    plan.outputWidth = request.outputWidth;
    plan.outputHeight = request.outputHeight;
    plan.resolvedKind = request.kind;
    if (plan.resolvedKind == ViewportLayoutKind::AutoTwoPlayer) {
        plan.resolvedKind = request.outputWidth >= request.outputHeight
            ? ViewportLayoutKind::VerticalTwoPlayer : ViewportLayoutKind::HorizontalTwoPlayer;
    }
    const auto add = [&](std::size_t ownerIndex, float x, float y, float w, float h, bool overlay = false) {
        plan.regions.push_back(make_region(request.owners[ownerIndex], request.outputWidth, request.outputHeight,
                                           x, y, w, h, request.gutterPixels,
                                           request.safeAreaFraction, overlay));
    };
    switch (plan.resolvedKind) {
        case ViewportLayoutKind::Single:
            if (request.owners.size() != 1U) { set_error(error, "single layout requires one owner"); return std::nullopt; }
            add(0U, 0.0F, 0.0F, 1.0F, 1.0F); break;
        case ViewportLayoutKind::HorizontalTwoPlayer:
            if (request.owners.size() != 2U) { set_error(error, "horizontal layout requires two owners"); return std::nullopt; }
            add(0U, 0.0F, 0.0F, 1.0F, 0.5F); add(1U, 0.0F, 0.5F, 1.0F, 0.5F); break;
        case ViewportLayoutKind::VerticalTwoPlayer:
            if (request.owners.size() != 2U) { set_error(error, "vertical layout requires two owners"); return std::nullopt; }
            add(0U, 0.0F, 0.0F, 0.5F, 1.0F); add(1U, 0.5F, 0.0F, 0.5F, 1.0F); break;
        case ViewportLayoutKind::ThreePlayerMainLeft:
            if (request.owners.size() != 3U) { set_error(error, "three-player layout requires three owners"); return std::nullopt; }
            add(0U, 0.0F, 0.0F, 0.5F, 1.0F); add(1U, 0.5F, 0.0F, 0.5F, 0.5F); add(2U, 0.5F, 0.5F, 0.5F, 0.5F); break;
        case ViewportLayoutKind::ThreePlayerMainTop:
            if (request.owners.size() != 3U) { set_error(error, "three-player layout requires three owners"); return std::nullopt; }
            add(0U, 0.0F, 0.0F, 1.0F, 0.5F); add(1U, 0.0F, 0.5F, 0.5F, 0.5F); add(2U, 0.5F, 0.5F, 0.5F, 0.5F); break;
        case ViewportLayoutKind::QuadFourPlayer:
            if (request.owners.size() != 4U) { set_error(error, "quad layout requires four owners"); return std::nullopt; }
            add(0U, 0.0F, 0.0F, 0.5F, 0.5F); add(1U, 0.5F, 0.0F, 0.5F, 0.5F);
            add(2U, 0.0F, 0.5F, 0.5F, 0.5F); add(3U, 0.5F, 0.5F, 0.5F, 0.5F); break;
        case ViewportLayoutKind::PictureInPicture: {
            if (request.owners.size() != 2U) { set_error(error, "picture-in-picture layout requires two owners"); return std::nullopt; }
            add(0U, 0.0F, 0.0F, 1.0F, 1.0F);
            const float f = request.pictureInPictureFraction;
            const float marginX = 24.0F / static_cast<float>(request.outputWidth);
            const float marginY = 24.0F / static_cast<float>(request.outputHeight);
            add(1U, 1.0F - f - marginX, marginY, f, f * static_cast<float>(request.outputWidth) /
                 static_cast<float>(request.outputHeight), true);
            break;
        }
        case ViewportLayoutKind::AutoTwoPlayer:
            set_error(error, "auto layout was not resolved"); return std::nullopt;
    }
    if (!plan.validate(error)) return std::nullopt;
    return plan;
}

std::optional<std::vector<ViewportPixelPresentation>> build_viewport_pixel_presentations(
    const ViewportLayoutPlan& layout, std::span<const PixelPresentationConfig> configs,
    std::string* error) {
    if (!layout.validate(error)) return std::nullopt;
    if (configs.size() != layout.regions.size()) {
        set_error(error, "pixel presentation config count must equal viewport count"); return std::nullopt;
    }
    std::vector<ViewportPixelPresentation> result;
    result.reserve(layout.regions.size());
    for (std::size_t index = 0; index < layout.regions.size(); ++index) {
        const auto& region = layout.regions[index];
        auto local = compute_pixel_presentation(configs[index], region.pixelRect.width,
                                                region.pixelRect.height, error);
        if (!local.has_value()) return std::nullopt;
        local->viewportX += region.pixelRect.x;
        local->viewportY += region.pixelRect.y;
        local->outputWidth = layout.outputWidth;
        local->outputHeight = layout.outputHeight;
        result.push_back({region, configs[index], *local});
    }
    return result;
}

bool SharedCameraSplitPolicy::validate(std::string* error) const {
    if (!std::isfinite(splitDistanceWorldUnits) || !std::isfinite(mergeDistanceWorldUnits) ||
        splitDistanceWorldUnits <= 0.0F || mergeDistanceWorldUnits <= 0.0F ||
        mergeDistanceWorldUnits >= splitDistanceWorldUnits || splitDelayTicks == 0U ||
        mergeDelayTicks == 0U || !std::isfinite(verticalBias) || verticalBias <= 0.0F) {
        set_error(error, "shared-camera split policy is invalid"); return false;
    }
    return true;
}

TwoPlayerSharedCameraController::TwoPlayerSharedCameraController(SharedCameraSplitPolicy policy)
    : policy_(policy) {
    if (!policy_.validate()) policy_ = {};
}

SharedCameraSplitState TwoPlayerSharedCameraController::update(Float3 playerA, Float3 playerB,
                                                                GameplayPlane2D plane) noexcept {
    state_.changed = false;
    const float dx = playerB.x - playerA.x;
    const float dy = plane == GameplayPlane2D::XY ? playerB.y - playerA.y : playerB.z - playerA.z;
    const float distance = std::sqrt(dx * dx + dy * dy);
    if (state_.layout == ViewportLayoutKind::Single) {
        state_.mergeTicks = 0U;
        if (distance >= policy_.splitDistanceWorldUnits) {
            ++state_.splitTicks;
            if (state_.splitTicks >= policy_.splitDelayTicks) {
                state_.layout = std::abs(dy) > std::abs(dx) * policy_.verticalBias
                    ? ViewportLayoutKind::HorizontalTwoPlayer
                    : ViewportLayoutKind::VerticalTwoPlayer;
                state_.splitTicks = 0U;
                state_.changed = true;
            }
        } else state_.splitTicks = 0U;
    } else {
        state_.splitTicks = 0U;
        if (distance <= policy_.mergeDistanceWorldUnits) {
            ++state_.mergeTicks;
            if (state_.mergeTicks >= policy_.mergeDelayTicks) {
                state_.layout = ViewportLayoutKind::Single;
                state_.mergeTicks = 0U;
                state_.changed = true;
            }
        } else state_.mergeTicks = 0U;
    }
    return state_;
}

void TwoPlayerSharedCameraController::reset() noexcept { state_ = {}; }


std::vector<AudioListenerMixEntry> build_audio_listener_mix(const ViewportLayoutPlan& layout,
                                                             AudioListenerMixPolicy policy) {
    std::vector<AudioListenerMixEntry> result;
    if (!layout.validate()) return result;
    result.reserve(layout.regions.size());
    const float equalGain = 1.0F / std::sqrt(static_cast<float>(layout.regions.size()));
    float weightedSquares = 0.0F;
    if (policy == AudioListenerMixPolicy::ViewportWeighted) {
        for (const auto& region : layout.regions) {
            const float area = region.normalizedWidth * region.normalizedHeight;
            weightedSquares += area * area;
        }
    }
    for (const auto& region : layout.regions) {
        float gain = 0.0F;
        if (policy == AudioListenerMixPolicy::PrimaryOnly) gain = region.owner.primary ? 1.0F : 0.0F;
        else if (policy == AudioListenerMixPolicy::EqualPower) gain = equalGain;
        else {
            const float area = region.normalizedWidth * region.normalizedHeight;
            gain = weightedSquares > 0.0F ? area / std::sqrt(weightedSquares) : 0.0F;
        }
        const float center = region.normalizedX + region.normalizedWidth * 0.5F;
        result.push_back({region.owner.audioListener, region.owner.playerIndex, gain,
                          std::clamp(center * 2.0F - 1.0F, -1.0F, 1.0F), region.owner.primary});
    }
    return result;
}

std::optional<MultiDisplayFramePacingPlan> build_multi_display_frame_pacing(
    const DisplayTopology& topology, std::span<const WindowPlacement> windows,
    MultiDisplayFramePacingPolicy policy, float requestedSimulationRateHz, std::string* error) {
    if (windows.empty() || !std::isfinite(requestedSimulationRateHz) || requestedSimulationRateHz < 0.0F ||
        requestedSimulationRateHz > 1000.0F) {
        set_error(error, "multi-display frame-pacing request is invalid");
        return std::nullopt;
    }
    const auto* primary = topology.primary();
    if (primary == nullptr) {
        set_error(error, "multi-display frame pacing requires a primary display");
        return std::nullopt;
    }
    float lowest = std::numeric_limits<float>::max();
    for (const auto& window : windows) {
        const auto* display = topology.find(window.display);
        if (display == nullptr) {
            set_error(error, "frame-pacing window references a disconnected display");
            return std::nullopt;
        }
        lowest = std::min(lowest, display->refreshRateHz);
    }
    MultiDisplayFramePacingPlan result;
    result.policy = policy;
    const float base = policy == MultiDisplayFramePacingPolicy::LowestRefresh ? lowest : primary->refreshRateHz;
    result.simulationRateHz = requestedSimulationRateHz > 0.0F ? requestedSimulationRateHz : base;
    for (const auto& window : windows) {
        const auto* display = topology.find(window.display);
        float target = display->refreshRateHz;
        bool independent = policy == MultiDisplayFramePacingPolicy::Independent;
        if (policy == MultiDisplayFramePacingPolicy::LockToPrimary) target = primary->refreshRateHz;
        else if (policy == MultiDisplayFramePacingPolicy::LowestRefresh) target = lowest;
        result.windows.push_back({window.window, window.display, display->refreshRateHz, target, independent});
    }
    return result;
}

PresentationBudgetReport evaluate_presentation_budget(const PresentationBudget& budget,
                                                        std::span<const WindowPlacement> windows,
                                                        std::span<const ViewportLayoutPlan> layouts) {
    PresentationBudgetReport report;
    report.profileId = budget.profileId;
    for (const auto& window : windows) {
        report.totalWindowPixels += static_cast<std::uint64_t>(window.drawableWidth) * window.drawableHeight;
        if (window.drawableWidth > budget.maximumDrawableWidth)
            report.violations.push_back({"drawable_width", window.drawableWidth, budget.maximumDrawableWidth,
                                         "window drawable width exceeds profile"});
        if (window.drawableHeight > budget.maximumDrawableHeight)
            report.violations.push_back({"drawable_height", window.drawableHeight, budget.maximumDrawableHeight,
                                         "window drawable height exceeds profile"});
    }
    std::uint64_t viewportCount = 0U;
    for (const auto& layout : layouts) {
        viewportCount += layout.regions.size();
        for (const auto& region : layout.regions)
            report.totalViewportPixels += static_cast<std::uint64_t>(region.pixelRect.width) * region.pixelRect.height;
    }
    if (windows.size() > budget.maximumWindows)
        report.violations.push_back({"windows", windows.size(), budget.maximumWindows,
                                     "window count exceeds profile"});
    if (viewportCount > budget.maximumViewports)
        report.violations.push_back({"viewports", viewportCount, budget.maximumViewports,
                                     "viewport count exceeds profile"});
    if (report.totalWindowPixels > budget.maximumTotalPixels)
        report.violations.push_back({"window_pixels", report.totalWindowPixels, budget.maximumTotalPixels,
                                     "total drawable pixels exceed profile"});
    if (report.totalViewportPixels > budget.maximumTotalViewportPixels)
        report.violations.push_back({"viewport_pixels", report.totalViewportPixels,
                                     budget.maximumTotalViewportPixels,
                                     "total viewport pixels exceed profile"});
    return report;
}

} // namespace dve::presentation
