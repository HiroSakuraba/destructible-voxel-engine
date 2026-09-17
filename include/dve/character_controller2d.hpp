#pragma once

#include "dve/physics2d.hpp"

#include <cstdint>
#include <string>

namespace dve {

// Input sampled once per rendered frame. jumpPressed/dropPressed are edge-triggered; jumpHeld is
// level-triggered. Axis values are clamped to [-1, 1].
struct CharacterController2DInput {
    float moveX{};
    float moveY{};
    bool jumpPressed{false};
    bool jumpHeld{false};
    bool dropPressed{false};
};

// A practical side-view controller layered over Physics2DWorld. The controller deliberately owns
// character feel (acceleration, coyote time, buffered jumps, ladder behavior, slope following)
// while the selected physics backend owns collision and integration.
struct CharacterController2DSettings {
    // Collider geometry used by the ground probes. It should match the character's primary box or
    // capsule collider. Coordinates are relative to the body's authored origin.
    TileVec2 colliderCenterPixels{};
    TileVec2 colliderHalfExtentsPixels{7.0F, 14.0F};

    float maximumRunSpeedPixelsPerSecond{160.0F};
    float groundAccelerationPixelsPerSecondSquared{1800.0F};
    float groundDecelerationPixelsPerSecondSquared{2400.0F};
    float airAccelerationPixelsPerSecondSquared{900.0F};
    float airDecelerationPixelsPerSecondSquared{500.0F};

    float jumpSpeedPixelsPerSecond{360.0F};
    float jumpReleaseVelocityFactor{0.48F};
    float riseGravityScale{1.0F};
    float fallGravityScale{1.45F};
    float maximumFallSpeedPixelsPerSecond{720.0F};

    float coyoteTimeSeconds{0.10F};
    float jumpBufferSeconds{0.12F};
    float oneWayDropSeconds{0.18F};

    float groundProbeDistancePixels{4.0F};
    float groundProbeInsetPixels{1.0F};
    float wallProbeDistancePixels{2.0F};
    float ceilingProbeDistancePixels{2.0F};
    float ledgeProbeForwardPixels{6.0F};
    float ledgeProbeDownPixels{12.0F};
    float maximumSlopeDegrees{50.0F};
    std::uint64_t groundCategoryBits{1};
    std::uint64_t groundMaskBits{~std::uint64_t{0}};

    float ladderClimbSpeedPixelsPerSecond{110.0F};
    float ladderHorizontalSpeedPixelsPerSecond{70.0F};
    float ladderSnapSpeedPixelsPerSecond{360.0F};
    bool snapToLadderCenter{true};

    bool inheritSupportVelocity{true};
    bool inheritVerticalSupportVelocityOnJump{true};
    bool enableCrushDetection{true};
    float crushMinimumClosingSpeedPixelsPerSecond{20.0F};
    float crushGraceSeconds{0.08F};
};

struct CharacterController2DState {
    bool grounded{false};
    bool onSlope{false};
    bool ladderAvailable{false};
    bool climbingLadder{false};
    bool facingRight{true};
    bool jumpedThisFrame{false};
    bool droppedThroughThisFrame{false};
    bool touchingLeftWall{false};
    bool touchingRightWall{false};
    bool touchingCeiling{false};
    bool nearLedge{false};
    bool crushed{false};

    TileVec2 groundNormal{0.0F, -1.0F};
    float slopeDegrees{};
    Physics2DBodyHandle supportBody{};
    TileVec2 supportVelocityPixelsPerSecond{};
    TileVec2 groundSurfaceVelocityPixelsPerSecond{};
    Physics2DBodyHandle leftWallBody{};
    Physics2DBodyHandle rightWallBody{};
    Physics2DBodyHandle ceilingBody{};
    TileVec2 leftWallVelocityPixelsPerSecond{};
    TileVec2 rightWallVelocityPixelsPerSecond{};
    TileVec2 ceilingVelocityPixelsPerSecond{};
    float crushClosingSpeedPixelsPerSecond{};

    float coyoteRemainingSeconds{};
    float jumpBufferRemainingSeconds{};
};

struct CharacterController2DSnapshot {
    CharacterController2DState state{};
    bool ladderContact{false};
    bool ladderHasCenter{false};
    float ladderCenterX{};
    bool jumpWasHeld{false};
    TileVec2 appliedTransportVelocityPixelsPerSecond{};
    float crushCandidateSeconds{};
};

class CharacterController2D {
public:
    explicit CharacterController2D(CharacterController2DSettings settings = {});

    [[nodiscard]] const CharacterController2DSettings& settings() const noexcept { return settings_; }
    [[nodiscard]] const CharacterController2DState& state() const noexcept { return state_; }

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
    void reset() noexcept;
    [[nodiscard]] CharacterController2DSnapshot capture_snapshot() const noexcept;
    bool restore_snapshot(const CharacterController2DSnapshot& snapshot) noexcept;

    // Ladders are usually represented by sensor colliders. Gameplay code feeds the aggregate sensor
    // state here after processing Physics2DEvent::SensorBegin/SensorEnd. centerX is optional and is
    // used for smooth horizontal centering while climbing.
    void set_ladder_contact(bool available, float centerX = 0.0F, bool hasCenter = false) noexcept;

    // Call before Physics2DWorld::step. This consumes input, updates timers, and applies the desired
    // velocity/gravity policy. Returns false when the body handle is invalid.
    bool pre_step(Physics2DWorld& world, Physics2DBodyHandle body,
                  const CharacterController2DInput& input, float frameDeltaSeconds);

    // Call after Physics2DWorld::step. This performs three foot probes, records slope/support state,
    // and refreshes coyote time. Returns false when the body handle is invalid.
    bool post_step(Physics2DWorld& world, Physics2DBodyHandle body, float frameDeltaSeconds);

    // Immediate gameplay response for damage, recoil, launch pads, or scripted motion.
    bool apply_knockback(Physics2DWorld& world, Physics2DBodyHandle body,
                         TileVec2 velocityPixelsPerSecond, bool clearJumpWindows = true);

private:
    [[nodiscard]] bool probe_ground(Physics2DWorld& world, const Physics2DBodyState& bodyState);
    void probe_surroundings(Physics2DWorld& world, const Physics2DBodyState& bodyState,
                            float frameDeltaSeconds);
    [[nodiscard]] static float move_towards(float current, float target, float maximumDelta) noexcept;

    CharacterController2DSettings settings_{};
    CharacterController2DState state_{};
    bool ladderContact_{false};
    bool ladderHasCenter_{false};
    float ladderCenterX_{};
    bool jumpWasHeld_{false};
    TileVec2 appliedTransportVelocityPixelsPerSecond_{};
    float crushCandidateSeconds_{};
};

} // namespace dve
