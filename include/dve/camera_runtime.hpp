#pragma once

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dve/camera_system.hpp"

namespace dve::camera { struct CameraSequence; class CameraSequencePlayer; }

namespace dve {
class GameWorld;
using GameObjectId = std::uint64_t;

namespace camera {

using CameraViewportId = std::uint64_t;

enum class CameraAccessibilityPreset : std::uint8_t {
    Standard,
    ReducedMotion,
    Photosensitive,
};

struct CameraAccessibilitySettings {
    CameraAccessibilityPreset preset{CameraAccessibilityPreset::Standard};
    bool horizonLock{};
    float motionScale{1.0F};
    float shakeScale{1.0F};
    float bloomScale{1.0F};
    float maximumDepthOfFieldWeight{1.0F};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct CameraMatrix4 {
    std::array<float, 16> values{};
    [[nodiscard]] static CameraMatrix4 identity() noexcept;
};

enum class CameraToneMapCurve : std::uint8_t { Linear, Reinhard, AcesFilmic };
enum class CameraLensDistortionModel : std::uint8_t { Disabled, BrownConrady, FisheyeEquidistant };
enum class CameraFramingPreset : std::uint8_t {
    Native,
    Academy137,
    Imax143,
    Imax190,
    Widescreen185,
    Scope239,
    Custom,
};
enum class CameraCinematicPreset : std::uint8_t {
    Neutral,
    AcademyClassic,
    Imax143,
    Imax190,
    Scope239,
    VintageAnamorphic,
    FisheyeAction,
    SplitDiopter,
    BleachBypass,
    SeventiesFilm,
};

struct CameraColorGradeSettings {
    CameraToneMapCurve toneMap{CameraToneMapCurve::AcesFilmic};
    float temperatureKelvin{6500.0F};
    float tint{};
    Float3 lift{};
    Float3 gamma{1.0F, 1.0F, 1.0F};
    Float3 gain{1.0F, 1.0F, 1.0F};
    float lutBlend{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct CameraLensEffectSettings {
    CameraLensDistortionModel distortionModel{CameraLensDistortionModel::Disabled};
    float radialK1{};
    float radialK2{};
    float radialK3{};
    float fisheyeStrength{};
    float chromaticAberrationPixels{};
    float anamorphicSqueeze{1.0F};
    float lensBreathing{};
    float anamorphicFlareIntensity{};
    float anamorphicFlareThreshold{1.0F};
    float gateWeavePixels{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct CameraBokehSettings {
    std::uint32_t bladeCount{6U};
    float bladeRotationRadians{};
    float roundness{0.8F};
    float anamorphicRatio{1.0F};
    float catEye{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct CameraSplitDiopterSettings {
    bool enabled{};
    float nearFocusDistanceMeters{1.0F};
    float farFocusDistanceMeters{10.0F};
    float centerX{};
    float centerY{};
    float angleRadians{};
    float featherFraction{0.025F};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct CameraFilmSettings {
    float grainIntensity{};
    float halationIntensity{};
    float sharpenIntensity{};
    float motionBlurWeight{};
    float shutterAngleDegrees{180.0F};
    std::uint32_t grainSeed{1U};
    CameraFramingPreset framing{CameraFramingPreset::Native};
    float customAspectRatio{16.0F / 9.0F};
    float matteOpacity{1.0F};
    Float3 matteColor{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct CameraCinematicSettings {
    CameraColorGradeSettings colorGrade{};
    CameraLensEffectSettings lens{};
    CameraBokehSettings bokeh{};
    CameraSplitDiopterSettings splitDiopter{};
    CameraFilmSettings film{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct CameraPostProcessProfile {
    float exposure{1.0F};
    float bloomIntensity{1.0F};
    float saturation{1.0F};
    float contrast{1.0F};
    float vignette{};
    float depthOfFieldWeight{};
    float focusDistanceMeters{10.0F};
    float apertureFStop{2.8F};
    Float3 colorTint{1.0F, 1.0F, 1.0F};
    CameraCinematicSettings cinematic{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

enum CameraPostProcessField : std::uint32_t {
    CameraPostExposure = 1U << 0U,
    CameraPostBloom = 1U << 1U,
    CameraPostSaturation = 1U << 2U,
    CameraPostContrast = 1U << 3U,
    CameraPostVignette = 1U << 4U,
    CameraPostDepthOfField = 1U << 5U,
    CameraPostFocus = 1U << 6U,
    CameraPostAperture = 1U << 7U,
    CameraPostColorTint = 1U << 8U,
    CameraPostColorGrade = 1U << 9U,
    CameraPostLensEffects = 1U << 10U,
    CameraPostBokeh = 1U << 11U,
    CameraPostSplitDiopter = 1U << 12U,
    CameraPostFilm = 1U << 13U,
    CameraPostAll = (1U << 14U) - 1U,
};

struct CameraPostProcessLayer {
    std::string name;
    CameraPostProcessProfile profile{};
    float weight{1.0F};
    bool enabled{true};
    std::uint32_t overrideMask{CameraPostAll};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct CameraPostProcessStack {
    std::vector<CameraPostProcessLayer> layers;

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
    [[nodiscard]] CameraPostProcessProfile evaluate() const noexcept;
};

[[nodiscard]] CameraPostProcessProfile blend_camera_post_process(
    const CameraPostProcessProfile& from,
    const CameraPostProcessProfile& to,
    float weight) noexcept;
[[nodiscard]] float camera_framing_aspect_ratio(const CameraFilmSettings& film,
                                                        float nativeAspectRatio) noexcept;
[[nodiscard]] CameraPostProcessProfile camera_cinematic_preset(CameraCinematicPreset preset) noexcept;

struct CameraViewportDesc {
    CameraViewportId id{};
    std::string name;
    std::uint32_t outputChannelMask{1U};
    float normalizedX{};
    float normalizedY{};
    float normalizedWidth{1.0F};
    float normalizedHeight{1.0F};
    std::string renderTarget{"main"};
    bool enabled{true};
    std::uint32_t overrideMask{CameraPostAll};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct CameraGpuPacket {
    std::array<float, 16> view{};
    std::array<float, 16> projection{};
    std::array<float, 16> viewProjection{};
    std::array<float, 4> cameraPosition{};
    std::array<float, 4> viewportRect{};
    std::array<float, 4> lens{};
    std::array<float, 4> physicalLens0{};
    std::array<float, 4> physicalLens1{};
    std::array<float, 4> postProcess0{};
    std::array<float, 4> postProcess1{};
    std::array<float, 4> colorTint{};
    std::array<float, 4> colorGrade0{};
    std::array<float, 4> colorGradeLift{};
    std::array<float, 4> colorGradeGamma{};
    std::array<float, 4> colorGradeGain{};
    std::array<float, 4> lensEffects0{};
    std::array<float, 4> lensEffects1{};
    std::array<float, 4> lensEffects2{};
    std::array<float, 4> bokeh{};
    std::array<float, 4> splitDiopter0{};
    std::array<float, 4> splitDiopter1{};
    std::array<float, 4> film0{};
    std::array<float, 4> film1{};
    std::array<float, 4> matteColor{};
    std::uint32_t outputChannelMask{1U};
    std::uint32_t cameraCutGeneration{};
    std::uint32_t resetTemporalHistory{};
    std::uint32_t reserved{};
};
static_assert(sizeof(CameraGpuPacket) == 544U,
              "CameraGpuPacket must remain byte-identical to camera_cinematic.hlsli");

struct CameraDepthOfFieldRange {
    float hyperfocalDistanceMeters{};
    float nearFocusMeters{};
    float farFocusMeters{};
};

struct CameraFrameGuides {
    float insetX{};
    float insetY{};
    float safeActionInset{0.05F};
    float safeTitleInset{0.10F};
};

struct CameraTriggeredEvent {
    std::uint64_t id{};
    std::string name;
    std::string payload;
};

struct CameraViewportFrame {
    CameraViewportDesc viewport{};
    CameraPose pose{};
    CameraPostProcessProfile postProcess{};
    CameraGpuPacket gpu{};
    std::optional<std::uint64_t> activeShotId;
    std::string activeMarker;
    std::vector<CameraTriggeredEvent> triggeredEvents;
};

class GameWorldCameraCollisionWorld final : public ICameraCollisionWorld {
public:
    explicit GameWorldCameraCollisionWorld(const GameWorld& world) : world_(&world) {}
    [[nodiscard]] bool sweep_sphere(Float3 start, Float3 end, float radiusMeters,
                                    CameraCollisionHit& hit) const override;
private:
    const GameWorld* world_{};
};

class GameCameraRuntime {
public:
    GameCameraRuntime();
    ~GameCameraRuntime();

    [[nodiscard]] bool add_viewport(CameraViewportDesc viewport, std::string* error = nullptr);
    [[nodiscard]] bool remove_viewport(CameraViewportId id) noexcept;
    [[nodiscard]] bool update_viewport(CameraViewportDesc viewport, std::string* error = nullptr);
    [[nodiscard]] CameraDirector* director(CameraViewportId id) noexcept;
    [[nodiscard]] const CameraDirector* director(CameraViewportId id) const noexcept;
    [[nodiscard]] const CameraViewportFrame* frame(CameraViewportId id) const noexcept;
    [[nodiscard]] std::vector<CameraViewportId> viewport_ids() const;

    [[nodiscard]] bool bind_target(CameraViewportId viewportId, CameraTargetId targetId,
                                   GameObjectId objectId) noexcept;
    [[nodiscard]] bool unbind_target(CameraViewportId viewportId, CameraTargetId targetId) noexcept;
    [[nodiscard]] bool set_post_process(CameraViewportId viewportId, CameraRigId rigId,
                                        CameraPostProcessProfile profile,
                                        std::string* error = nullptr);
    [[nodiscard]] bool set_post_process_stack(CameraViewportId viewportId, CameraRigId rigId,
                                              CameraPostProcessStack stack,
                                              std::string* error = nullptr);
    [[nodiscard]] bool set_sequence(CameraViewportId viewportId, const CameraSequence& sequence,
                                    std::string* error = nullptr);
    [[nodiscard]] bool play_sequence(CameraViewportId viewportId, bool loop = false) noexcept;
    [[nodiscard]] bool pause_sequence(CameraViewportId viewportId) noexcept;
    [[nodiscard]] bool seek_sequence(CameraViewportId viewportId, float timeSeconds) noexcept;
    [[nodiscard]] bool stop_sequence(CameraViewportId viewportId) noexcept;
    [[nodiscard]] float sequence_time(CameraViewportId viewportId) const noexcept;
    [[nodiscard]] bool sequence_playing(CameraViewportId viewportId) const noexcept;
    void clear_sequence(CameraViewportId viewportId) noexcept;

    [[nodiscard]] std::string serialize_state() const;
    [[nodiscard]] bool restore_state(std::string_view text, std::string* error = nullptr);
    void set_reduced_motion(bool reduced) noexcept;
    [[nodiscard]] bool set_accessibility(CameraAccessibilitySettings settings,
                                         std::string* error = nullptr);
    [[nodiscard]] const CameraAccessibilitySettings& accessibility() const noexcept {
        return accessibility_;
    }
    void update(const GameWorld& world, float elapsedSeconds);

private:
    struct ViewportState {
        CameraViewportDesc desc{};
        CameraDirector director{};
        std::map<CameraTargetId, GameObjectId> targetBindings{};
        std::map<CameraRigId, CameraPostProcessStack> postProcessStacks{};
        CameraViewportFrame frame{};
        CameraRigId previousLiveRig{};
        std::uint32_t cutGeneration{};
        std::shared_ptr<CameraSequence> sequence{};
        std::unique_ptr<CameraSequencePlayer> sequencePlayer{};
        std::optional<std::uint64_t> previousShotId{};
    };
    std::map<CameraViewportId, ViewportState> viewports_;
    CameraAccessibilitySettings accessibility_{};
    bool reducedMotion_{};
};

[[nodiscard]] CameraMatrix4 camera_view_matrix(const CameraPose& pose) noexcept;
// anamorphicSqueeze widens the horizontal field of view by that factor, which is what an
// anamorphic front element actually buys you. There is no squeezed intermediate image in a
// real-time renderer and no projector to undo one, so neither the frustum nor a post-process
// horizontal remap may carry the squeeze: both produce non-square pixels on the display. The
// squeeze is expressed by the delivery aspect instead, via camera_anamorphic_filmback_aspect_ratio.
[[nodiscard]] CameraMatrix4 camera_projection_matrix(const CameraLens& lens) noexcept;
// Delivery aspect for an anamorphic taking lens: sensor width times squeeze, over sensor height.
// Anamorphic 35 (21.95 x 18.60 mm) at 2x gives 2.360, which is the scope frame.
[[nodiscard]] float camera_anamorphic_filmback_aspect_ratio(const CameraPhysicalLens& lens,
                                                            float anamorphicSqueeze) noexcept;
[[nodiscard]] CameraMatrix4 multiply(CameraMatrix4 a, CameraMatrix4 b) noexcept;
[[nodiscard]] CameraDepthOfFieldRange camera_depth_of_field_range(const CameraPhysicalLens& lens,
                                                                   float circleOfConfusionMillimeters = 0.0F) noexcept;
[[nodiscard]] CameraFrameGuides camera_frame_guides(float viewportAspect, float targetAspect) noexcept;
[[nodiscard]] bool camera_sphere_visible(const CameraPose& pose, Float3 center, float radiusMeters) noexcept;
[[nodiscard]] float camera_projected_sphere_radius_pixels(const CameraPose& pose, Float3 center,
                                                          float radiusMeters, float viewportHeightPixels) noexcept;
[[nodiscard]] CameraGpuPacket pack_camera_gpu_packet(const CameraViewportDesc& viewport,
                                                      const CameraPose& pose,
                                                      const CameraPostProcessProfile& post,
                                                      std::uint32_t cutGeneration,
                                                      bool resetTemporalHistory) noexcept;

} // namespace camera
} // namespace dve
