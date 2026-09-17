#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dve/sprite2d.hpp"
#include "dve/transform.hpp"

namespace dve::presentation {

using DisplayId = std::uint64_t;
using WindowId = std::uint64_t;
using AudioListenerId = std::uint64_t;
using InputSlotId = std::uint32_t;

inline constexpr DisplayId kInvalidDisplayId = 0U;
inline constexpr WindowId kInvalidWindowId = 0U;

struct IntRect {
    std::int32_t x{};
    std::int32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};

    [[nodiscard]] std::int64_t right() const noexcept;
    [[nodiscard]] std::int64_t bottom() const noexcept;
    [[nodiscard]] bool contains(std::int32_t px, std::int32_t py) const noexcept;
    [[nodiscard]] bool intersects(const IntRect& other) const noexcept;
};

struct ResolutionProfile {
    std::string id;
    std::string label;
    std::uint32_t width{};
    std::uint32_t height{};
    bool ultrawide{};
    bool portrait{};
    bool shippingReference{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

[[nodiscard]] const std::vector<ResolutionProfile>& built_in_resolution_profiles();
[[nodiscard]] const ResolutionProfile* find_resolution_profile(std::string_view id) noexcept;

struct DisplayInfo {
    DisplayId id{kInvalidDisplayId};
    std::string name;
    IntRect logicalBounds{};
    IntRect logicalWorkArea{};
    std::uint32_t nativePixelWidth{};
    std::uint32_t nativePixelHeight{};
    float contentScale{1.0F};
    float refreshRateHz{60.0F};
    bool primary{};
    bool hdrCapable{};
    bool connected{true};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

class DisplayTopology {
public:
    [[nodiscard]] bool replace(std::vector<DisplayInfo> displays, std::string* error = nullptr);
    [[nodiscard]] const std::vector<DisplayInfo>& displays() const noexcept { return displays_; }
    [[nodiscard]] const DisplayInfo* find(DisplayId id) const noexcept;
    [[nodiscard]] const DisplayInfo* primary() const noexcept;
    [[nodiscard]] DisplayId recover(DisplayId preferred) const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

private:
    std::vector<DisplayInfo> displays_;
    std::uint64_t generation_{};
};

enum class WindowMode : std::uint8_t {
    Windowed,
    BorderlessFullscreen,
    ExclusiveFullscreen,
};

struct WindowPlacementRequest {
    WindowId window{kInvalidWindowId};
    DisplayId preferredDisplay{kInvalidDisplayId};
    WindowMode mode{WindowMode::Windowed};
    std::uint32_t logicalWidth{1280U};
    std::uint32_t logicalHeight{720U};
    std::optional<std::int32_t> logicalX;
    std::optional<std::int32_t> logicalY;
    bool centerIfUnspecified{true};
    bool clampToWorkArea{true};
};

struct WindowPlacement {
    WindowId window{kInvalidWindowId};
    DisplayId display{kInvalidDisplayId};
    WindowMode mode{WindowMode::Windowed};
    IntRect logicalRect{};
    std::uint32_t drawableWidth{};
    std::uint32_t drawableHeight{};
    float contentScale{1.0F};
    float refreshRateHz{60.0F};
    bool recoveredDisplay{};
    bool clamped{};
    std::string recoveryReason;

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

[[nodiscard]] std::optional<WindowPlacement> resolve_window_placement(
    const DisplayTopology& topology,
    const WindowPlacementRequest& request,
    std::string* error = nullptr);

enum class ViewportLayoutKind : std::uint8_t {
    Single,
    AutoTwoPlayer,
    HorizontalTwoPlayer,
    VerticalTwoPlayer,
    ThreePlayerMainLeft,
    ThreePlayerMainTop,
    QuadFourPlayer,
    PictureInPicture,
};

struct ViewportOwner {
    std::uint32_t playerIndex{};
    std::uint64_t cameraId{};
    std::uint64_t hudCanvasId{};
    InputSlotId inputSlot{};
    AudioListenerId audioListener{};
    std::string budgetProfile{"desktop"};
    bool primary{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct ViewportLayoutRequest {
    ViewportLayoutKind kind{ViewportLayoutKind::Single};
    std::vector<ViewportOwner> owners;
    std::uint32_t outputWidth{1920U};
    std::uint32_t outputHeight{1080U};
    std::uint32_t gutterPixels{};
    float safeAreaFraction{0.035F};
    float pictureInPictureFraction{0.28F};
};

struct ViewportRegion {
    ViewportOwner owner{};
    IntRect pixelRect{};
    IntRect safeRect{};
    float normalizedX{};
    float normalizedY{};
    float normalizedWidth{1.0F};
    float normalizedHeight{1.0F};
    bool overlay{};
};

struct ViewportLayoutPlan {
    ViewportLayoutKind resolvedKind{ViewportLayoutKind::Single};
    std::uint32_t outputWidth{};
    std::uint32_t outputHeight{};
    std::vector<ViewportRegion> regions;

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

[[nodiscard]] std::optional<ViewportLayoutPlan> build_viewport_layout(
    const ViewportLayoutRequest& request,
    std::string* error = nullptr);

struct ViewportPixelPresentation {
    ViewportRegion region{};
    PixelPresentationConfig pixelConfig{};
    PixelPresentationLayout pixelLayout{};
};

[[nodiscard]] std::optional<std::vector<ViewportPixelPresentation>> build_viewport_pixel_presentations(
    const ViewportLayoutPlan& layout,
    std::span<const PixelPresentationConfig> configs,
    std::string* error = nullptr);

struct SharedCameraSplitPolicy {
    float splitDistanceWorldUnits{22.0F};
    float mergeDistanceWorldUnits{15.0F};
    std::uint32_t splitDelayTicks{12U};
    std::uint32_t mergeDelayTicks{24U};
    float verticalBias{1.15F};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct SharedCameraSplitState {
    ViewportLayoutKind layout{ViewportLayoutKind::Single};
    std::uint32_t splitTicks{};
    std::uint32_t mergeTicks{};
    bool changed{};
};

class TwoPlayerSharedCameraController {
public:
    explicit TwoPlayerSharedCameraController(SharedCameraSplitPolicy policy = {});

    [[nodiscard]] SharedCameraSplitState update(Float3 playerA, Float3 playerB,
                                                 GameplayPlane2D plane = GameplayPlane2D::XY) noexcept;
    void reset() noexcept;
    [[nodiscard]] const SharedCameraSplitState& state() const noexcept { return state_; }
    [[nodiscard]] const SharedCameraSplitPolicy& policy() const noexcept { return policy_; }

private:
    SharedCameraSplitPolicy policy_{};
    SharedCameraSplitState state_{};
};


enum class AudioListenerMixPolicy : std::uint8_t {
    PrimaryOnly,
    EqualPower,
    ViewportWeighted,
};

struct AudioListenerMixEntry {
    AudioListenerId listener{};
    std::uint32_t playerIndex{};
    float gain{};
    float stereoPan{};
    bool primary{};
};

[[nodiscard]] std::vector<AudioListenerMixEntry> build_audio_listener_mix(
    const ViewportLayoutPlan& layout,
    AudioListenerMixPolicy policy);

enum class MultiDisplayFramePacingPolicy : std::uint8_t {
    Independent,
    LockToPrimary,
    LowestRefresh,
};

struct WindowFramePacingEntry {
    WindowId window{kInvalidWindowId};
    DisplayId display{kInvalidDisplayId};
    float displayRefreshRateHz{60.0F};
    float targetPresentRateHz{60.0F};
    bool independentlyPaced{};
};

struct MultiDisplayFramePacingPlan {
    MultiDisplayFramePacingPolicy policy{MultiDisplayFramePacingPolicy::LockToPrimary};
    float simulationRateHz{60.0F};
    std::vector<WindowFramePacingEntry> windows;
};

[[nodiscard]] std::optional<MultiDisplayFramePacingPlan> build_multi_display_frame_pacing(
    const DisplayTopology& topology,
    std::span<const WindowPlacement> windows,
    MultiDisplayFramePacingPolicy policy,
    float requestedSimulationRateHz = 0.0F,
    std::string* error = nullptr);

struct PresentationBudget {
    std::string profileId{"desktop"};
    std::uint32_t maximumDrawableWidth{7680U};
    std::uint32_t maximumDrawableHeight{4320U};
    std::uint32_t maximumViewports{4U};
    std::uint32_t maximumWindows{4U};
    std::uint64_t maximumTotalPixels{33'177'600ULL};
    std::uint64_t maximumTotalViewportPixels{66'355'200ULL};
};

struct PresentationBudgetViolation {
    std::string field;
    std::uint64_t actual{};
    std::uint64_t limit{};
    std::string message;
};

struct PresentationBudgetReport {
    std::string profileId;
    std::uint64_t totalWindowPixels{};
    std::uint64_t totalViewportPixels{};
    std::vector<PresentationBudgetViolation> violations;

    [[nodiscard]] bool within_budget() const noexcept { return violations.empty(); }
};

[[nodiscard]] PresentationBudgetReport evaluate_presentation_budget(
    const PresentationBudget& budget,
    std::span<const WindowPlacement> windows,
    std::span<const ViewportLayoutPlan> layouts);

} // namespace dve::presentation
