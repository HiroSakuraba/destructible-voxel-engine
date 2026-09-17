#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dve/transform.hpp"

namespace dve::camera {

using CameraRigId = std::uint64_t;
using CameraTargetId = std::uint64_t;
using CameraShakeId = std::uint64_t;

constexpr float kDegreesToRadians = 0.01745329251994329577F;
constexpr float kRadiansToDegrees = 57.295779513082320876F;

// Camera controls do not interpolate meaningfully in their display units.
//
// Focus distance: a follow focus is geometrically linear in diopters (1/metres), not metres.
// Blending 1 m to 100 m linearly in metres spends almost the whole move already at infinity.
// Aperture: stops are logarithmic, so an f/2.8 to f/22 blend in linear f-number is heavily back
// loaded and the exposure change lands in a rush at the end.
// Focal length: angular field is what the eye tracks, and it is roughly logarithmic in focal
// length. A 24 mm to 200 mm zoom interpolated linearly in millimetres spends most of the move at
// the telephoto end.
[[nodiscard]] inline float blend_camera_focus_distance(float a,float b,float weight) noexcept {
    const float da=1.0F/std::max(1.0e-4F,a),db=1.0F/std::max(1.0e-4F,b);
    return 1.0F/std::max(1.0e-4F,da+(db-da)*weight);
}
[[nodiscard]] inline float blend_camera_f_stop(float a,float b,float weight) noexcept {
    const float la=std::log2(std::max(1.0e-3F,a)),lb=std::log2(std::max(1.0e-3F,b));
    return std::exp2(la+(lb-la)*weight);
}
[[nodiscard]] inline float blend_camera_focal_length(float a,float b,float weight) noexcept {
    const float la=std::log2(std::max(1.0e-3F,a)),lb=std::log2(std::max(1.0e-3F,b));
    return std::exp2(la+(lb-la)*weight);
}
// 2026-07-25: vertical field of view has to blend in the same space as focal length, or the
// identical authored move behaves differently depending on whether the physical lens happens to
// be enabled: blend_lens log-blends focalLengthMillimeters but was lerping this in radians.
// Focal length is proportional to 1/tan(fov/2), so log-blending tan(fov/2) is the same curve.
// A 60 to 10 degree move lands at 35.0 degrees linearly and 25.3 degrees here.
[[nodiscard]] inline float blend_camera_vertical_fov(float a,float b,float weight) noexcept {
    const float ta=std::tan(std::clamp(a,0.001F,3.13F)*0.5F);
    const float tb=std::tan(std::clamp(b,0.001F,3.13F)*0.5F);
    const float la=std::log2(std::max(1.0e-4F,ta)),lb=std::log2(std::max(1.0e-4F,tb));
    return 2.0F*std::atan(std::exp2(la+(lb-la)*weight));
}
// Colour temperature is perceptually uniform in mireds, not in kelvin.
[[nodiscard]] inline float blend_camera_temperature_kelvin(float a,float b,float weight) noexcept {
    const float ma=1.0e6F/std::clamp(a,1000.0F,20000.0F);
    const float mb=1.0e6F/std::clamp(b,1000.0F,20000.0F);
    const float mixed=ma+(mb-ma)*weight;
    return std::clamp(1.0e6F/std::max(1.0e-3F,mixed),1000.0F,20000.0F);
}
// bladeCount is valid only as 0 (circular) or 3..16. Rounding a linear blend walked it through 1
// and 2, which produced a profile that failed its own validate(), and apply_camera_cinematic_pipeline
// bails out entirely on an invalid profile: the whole cinematic pass switched off mid-blend.
[[nodiscard]] inline std::uint32_t blend_camera_blade_count(std::uint32_t a,std::uint32_t b,float weight) noexcept {
    if(a==b)return a;
    if(a==0U||b==0U)return weight<0.5F?a:b;
    const float mixed=static_cast<float>(a)+(static_cast<float>(b)-static_cast<float>(a))*weight;
    const auto rounded=static_cast<std::uint32_t>(std::lround(mixed));
    return std::clamp(rounded,3U,16U);
}

enum class CameraProjection : std::uint8_t { Perspective, Orthographic };
enum class CameraRigMode : std::uint8_t {
    Fixed,
    FreeFly,
    Orbit,
    Follow,
    ThirdPerson,
    FirstPerson,
    Cinematic,
};
enum class CameraBlendCurve : std::uint8_t { Cut, Linear, EaseIn, EaseOut, EaseInOut, SmoothStep };
enum class CameraGateFit : std::uint8_t { Vertical, Horizontal, Fill, Overscan, Stretch };
enum class CameraFilmbackPreset : std::uint8_t {
    Custom,
    Super16,
    Super35,
    FullFrame35,
    Anamorphic35,
    Imax15Perf,
    ImaxDigital,
};
enum class CameraShakePattern : std::uint8_t { PerlinLikeNoise, SineWave };
enum class CameraShakeSpace : std::uint8_t { CameraLocal, World };
enum class CameraObstructionStrategy : std::uint8_t {
    PullForward,
    PreserveTargetFraming,
    FadeOccluders,
    ShoulderSwap,
};

struct CameraPhysicalLens {
    bool enabled{};
    float focalLengthMillimeters{35.0F};
    float sensorWidthMillimeters{36.0F};
    float sensorHeightMillimeters{24.0F};
    float lensShiftX{};
    float lensShiftY{};
    float apertureFStop{2.8F};
    float focusDistanceMeters{10.0F};
    CameraGateFit gateFit{CameraGateFit::Vertical};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
    [[nodiscard]] float vertical_field_of_view_radians(float viewportAspectRatio) const noexcept;
};

[[nodiscard]] CameraPhysicalLens camera_physical_lens_preset(
    CameraFilmbackPreset preset,
    float focalLengthMillimeters = 35.0F) noexcept;

struct CameraLens {
    CameraProjection projection{CameraProjection::Perspective};
    float verticalFieldOfViewRadians{60.0F * kDegreesToRadians};
    float orthographicHeightMeters{10.0F};
    float nearPlaneMeters{0.05F};
    float farPlaneMeters{5000.0F};
    float aspectRatio{16.0F / 9.0F};
    CameraPhysicalLens physical{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
    [[nodiscard]] float effective_vertical_field_of_view_radians() const noexcept;
};

struct CameraPose {
    Float3 position{0.0F, 1.5F, 4.0F};
    Float3 target{0.0F, 1.0F, 0.0F};
    Float3 worldUp{0.0F, 1.0F, 0.0F};
    CameraLens lens{};
    float postProcessWeight{1.0F};
};

struct CameraTargetState {
    Float3 position{};
    Float3 forward{0.0F, 0.0F, -1.0F};
    Float3 up{0.0F, 1.0F, 0.0F};
    Float3 velocity{};
};

struct CameraFramingSettings {
    Float3 localOffset{0.0F, 1.6F, 0.0F};
    float distanceMeters{4.5F};
    float heightMeters{0.4F};
    float shoulderOffsetMeters{0.45F};
    float lookAheadSeconds{0.15F};
    float screenOffsetX{};
    float screenOffsetY{};
    float positionDampingSeconds{0.12F};
    float aimDampingSeconds{0.08F};
    float deadZoneFraction{0.02F};
    float softZoneFraction{0.65F};
    float orbitYawRadians{};
    float orbitPitchRadians{15.0F * kDegreesToRadians};
    float minimumPitchRadians{-80.0F * kDegreesToRadians};
    float maximumPitchRadians{80.0F * kDegreesToRadians};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct CameraCollisionSettings {
    bool enabled{true};
    bool preserveLineOfSight{true};
    CameraObstructionStrategy strategy{CameraObstructionStrategy::PullForward};
    float probeRadiusMeters{0.22F};
    float minimumTargetDistanceMeters{0.35F};
    float collisionPullInSeconds{0.035F};
    float recoverySeconds{0.25F};
    float shoulderSwapSearchMeters{0.9F};
    float occluderFadeOpacity{0.18F};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct CameraVolumeConstraint {
    bool enabled{};
    Float3 minimum{-100000.0F, -100000.0F, -100000.0F};
    Float3 maximum{100000.0F, 100000.0F, 100000.0F};
    float softnessMeters{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct CameraRig {
    CameraRigId id{};
    std::string name;
    CameraRigMode mode{CameraRigMode::Fixed};
    bool enabled{true};
    std::int32_t priority{};
    std::uint32_t outputChannels{1U};
    CameraPose authoredPose{};
    CameraLens lens{};
    std::optional<CameraTargetId> followTarget;
    std::optional<CameraTargetId> lookAtTarget;
    CameraFramingSettings framing{};
    CameraCollisionSettings collision{};
    CameraVolumeConstraint volumeConstraint{};
    float postProcessWeight{1.0F};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct CameraBlend {
    CameraBlendCurve curve{CameraBlendCurve::EaseInOut};
    float durationSeconds{0.35F};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct CameraCollisionHit {
    Float3 position{};
    Float3 normal{};
    float fraction{1.0F};
    // Stable renderer/runtime identity when supplied by the collision adapter. Zero means the
    // adapter could detect geometry but could not identify a persistent scene object/material.
    std::uint64_t objectId{};
    std::uint32_t materialId{};
};

struct CameraOccluderFadeRequest {
    CameraRigId rigId{};
    std::uint64_t objectId{};
    std::uint32_t materialId{};
    float targetOpacity{1.0F};
    Float3 hitPosition{};
};

class ICameraCollisionWorld {
public:
    virtual ~ICameraCollisionWorld() = default;
    [[nodiscard]] virtual bool sweep_sphere(
        Float3 start,
        Float3 end,
        float radiusMeters,
        CameraCollisionHit& hit) const = 0;
};

struct CameraShake {
    CameraShakeId id{};
    CameraShakePattern pattern{CameraShakePattern::PerlinLikeNoise};
    CameraShakeSpace space{CameraShakeSpace::CameraLocal};
    Float3 positionAmplitudeMeters{0.03F, 0.03F, 0.03F};
    Float3 rotationAmplitudeRadians{0.4F * kDegreesToRadians, 0.4F * kDegreesToRadians,
                                    0.25F * kDegreesToRadians};
    float fieldOfViewAmplitudeRadians{};
    float frequencyHertz{12.0F};
    float durationSeconds{0.3F};
    float blendInSeconds{0.03F};
    float blendOutSeconds{0.08F};
    float scale{1.0F};
    std::optional<Float3> sourcePosition;
    float fullIntensityRadiusMeters{};
    float zeroIntensityRadiusMeters{};
    std::uint32_t seed{1U};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct CameraDirectorTelemetry {
    std::uint64_t updates{};
    std::uint64_t rigChanges{};
    std::uint64_t cuts{};
    std::uint64_t blendsStarted{};
    std::uint64_t collisionCorrections{};
    std::uint64_t shoulderSwaps{};
    std::uint64_t occluderFadeRequests{};
    std::uint64_t volumeCorrections{};
    std::uint64_t shakeSamples{};
    bool shoulderSwapActive{};
    bool occluderFadeActive{};
    bool volumeConstraintActive{};
    CameraRigId liveRig{};
    CameraRigId outgoingRig{};
    float blendWeight{1.0F};
};

class CameraDirector {
public:
    [[nodiscard]] bool add_or_replace_rig(CameraRig rig, std::string* error = nullptr);
    [[nodiscard]] bool remove_rig(CameraRigId id) noexcept;
    [[nodiscard]] const CameraRig* find_rig(CameraRigId id) const noexcept;
    [[nodiscard]] CameraRig* find_rig(CameraRigId id) noexcept;
    [[nodiscard]] const std::vector<CameraRig>& rigs() const noexcept { return rigs_; }

    void set_target(CameraTargetId id, CameraTargetState state) noexcept;
    void remove_target(CameraTargetId id) noexcept;
    void set_channel_mask(std::uint32_t mask) noexcept { channelMask_ = mask == 0U ? 1U : mask; }
    [[nodiscard]] std::uint32_t channel_mask() const noexcept { return channelMask_; }

    void set_default_blend(CameraBlend blend) noexcept { defaultBlend_ = blend; }
    [[nodiscard]] const CameraBlend& default_blend() const noexcept { return defaultBlend_; }
    void set_custom_blend(CameraRigId from, CameraRigId to, CameraBlend blend);
    void clear_custom_blends() noexcept { customBlends_.clear(); }

    void bind_state(std::string stateName, CameraRigId rigId);
    [[nodiscard]] bool set_state(std::string_view stateName) noexcept;
    [[nodiscard]] bool has_state_binding(std::string_view stateName) const noexcept {
        return stateBindings_.contains(std::string(stateName));
    }
    [[nodiscard]] std::string_view state() const noexcept { return state_; }
    void clear_state() noexcept { state_.clear(); }

    [[nodiscard]] bool force_live(CameraRigId id, bool cut = false) noexcept;
    void clear_forced_live(bool cut = false) noexcept;

    [[nodiscard]] bool start_shake(CameraShake shake, std::string* error = nullptr);
    [[nodiscard]] bool stop_shake(CameraShakeId id) noexcept;
    void stop_all_shakes() noexcept;
    void set_shake_scale(float scale) noexcept;
    void set_reduced_motion(bool reduced) noexcept { reducedMotion_ = reduced; }
    [[nodiscard]] float shake_scale() const noexcept { return shakeScale_; }
    [[nodiscard]] bool reduced_motion() const noexcept { return reducedMotion_; }

    [[nodiscard]] CameraPose update(float elapsedSeconds, const ICameraCollisionWorld* collisionWorld = nullptr);
    [[nodiscard]] const CameraPose& current_pose() const noexcept { return currentPose_; }
    [[nodiscard]] const CameraDirectorTelemetry& telemetry() const noexcept { return telemetry_; }
    [[nodiscard]] const std::vector<CameraOccluderFadeRequest>& occluder_fade_requests() const noexcept {
        return occluderFadeRequests_;
    }

private:
    struct ActiveShake { CameraShake definition; float elapsed{}; };
    [[nodiscard]] const CameraRig* choose_live_rig() const noexcept;
    [[nodiscard]] CameraPose evaluate_rig(const CameraRig& rig, float elapsedSeconds,
                                          const ICameraCollisionWorld* collisionWorld);
    [[nodiscard]] CameraBlend blend_for(CameraRigId from, CameraRigId to) const noexcept;
    void begin_transition(const CameraRig* next, bool forceCut = false) noexcept;
    void apply_shakes(CameraPose& pose, float elapsedSeconds);

    std::vector<CameraRig> rigs_;
    std::map<CameraTargetId, CameraTargetState> targets_;
    std::map<std::pair<CameraRigId, CameraRigId>, CameraBlend> customBlends_;
    std::map<CameraRigId, float> collisionDistances_;
    std::map<std::string, CameraRigId, std::less<>> stateBindings_;
    std::vector<ActiveShake> shakes_;
    std::vector<CameraOccluderFadeRequest> occluderFadeRequests_;
    std::optional<CameraRigId> forcedRig_;
    std::string state_;
    std::uint32_t channelMask_{1U};
    CameraBlend defaultBlend_{};
    CameraPose currentPose_{};
    CameraPose transitionFrom_{};
    CameraPose transitionTo_{};
    CameraRigId liveRig_{};
    CameraRigId outgoingRig_{};
    float transitionElapsed_{};
    float transitionDuration_{};
    CameraBlendCurve transitionCurve_{CameraBlendCurve::Cut};
    float shakeScale_{1.0F};
    bool reducedMotion_{};
    CameraDirectorTelemetry telemetry_{};
};

struct CameraRigLibrary {
    std::vector<CameraRig> rigs;
    CameraBlend defaultBlend{};
    std::map<std::pair<CameraRigId, CameraRigId>, CameraBlend> customBlends;
    std::map<std::string, CameraRigId, std::less<>> stateBindings;

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static std::optional<CameraRigLibrary> parse(std::string_view text,
                                                                std::string* error = nullptr);
};

} // namespace dve::camera
