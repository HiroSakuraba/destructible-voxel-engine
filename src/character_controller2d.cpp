#include "dve/character_controller2d.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace dve {
namespace {

constexpr float kPi = 3.14159265358979323846F;

[[nodiscard]] bool finite(TileVec2 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y);
}

[[nodiscard]] float clamp_axis(float value) noexcept {
    if (!std::isfinite(value)) return 0.0F;
    return std::clamp(value, -1.0F, 1.0F);
}

} // namespace

CharacterController2D::CharacterController2D(CharacterController2DSettings settings)
    : settings_(settings) {}

bool CharacterController2D::validate(std::string* error) const {
    const auto fail = [error](const char* message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (!finite(settings_.colliderCenterPixels) || !finite(settings_.colliderHalfExtentsPixels) ||
        !(settings_.colliderHalfExtentsPixels.x > 0.0F) ||
        !(settings_.colliderHalfExtentsPixels.y > 0.0F)) {
        return fail("character collider geometry must be finite and positive");
    }
    const std::array<float, 27> finiteValues{
        settings_.maximumRunSpeedPixelsPerSecond,
        settings_.groundAccelerationPixelsPerSecondSquared,
        settings_.groundDecelerationPixelsPerSecondSquared,
        settings_.airAccelerationPixelsPerSecondSquared,
        settings_.airDecelerationPixelsPerSecondSquared,
        settings_.jumpSpeedPixelsPerSecond,
        settings_.jumpReleaseVelocityFactor,
        settings_.riseGravityScale,
        settings_.fallGravityScale,
        settings_.maximumFallSpeedPixelsPerSecond,
        settings_.coyoteTimeSeconds,
        settings_.jumpBufferSeconds,
        settings_.oneWayDropSeconds,
        settings_.groundProbeDistancePixels,
        settings_.groundProbeInsetPixels,
        settings_.wallProbeDistancePixels,
        settings_.ceilingProbeDistancePixels,
        settings_.ledgeProbeForwardPixels,
        settings_.ledgeProbeDownPixels,
        settings_.maximumSlopeDegrees,
        settings_.ladderClimbSpeedPixelsPerSecond,
        settings_.ladderHorizontalSpeedPixelsPerSecond,
        settings_.ladderSnapSpeedPixelsPerSecond,
        settings_.crushMinimumClosingSpeedPixelsPerSecond,
        settings_.crushGraceSeconds,
        static_cast<float>(settings_.groundCategoryBits != 0U),
        static_cast<float>(settings_.groundMaskBits != 0U)};
    for (const float value : finiteValues) {
        if (!std::isfinite(value)) return fail("character settings contain a non-finite value");
    }
    if (settings_.maximumRunSpeedPixelsPerSecond < 0.0F ||
        settings_.groundAccelerationPixelsPerSecondSquared < 0.0F ||
        settings_.groundDecelerationPixelsPerSecondSquared < 0.0F ||
        settings_.airAccelerationPixelsPerSecondSquared < 0.0F ||
        settings_.airDecelerationPixelsPerSecondSquared < 0.0F ||
        settings_.jumpSpeedPixelsPerSecond < 0.0F ||
        settings_.maximumFallSpeedPixelsPerSecond < 0.0F ||
        settings_.coyoteTimeSeconds < 0.0F || settings_.jumpBufferSeconds < 0.0F ||
        settings_.oneWayDropSeconds < 0.0F || settings_.groundProbeDistancePixels < 0.0F ||
        settings_.groundProbeInsetPixels < 0.0F || settings_.wallProbeDistancePixels < 0.0F ||
        settings_.ceilingProbeDistancePixels < 0.0F || settings_.ledgeProbeForwardPixels < 0.0F ||
        settings_.ledgeProbeDownPixels < 0.0F || settings_.crushMinimumClosingSpeedPixelsPerSecond < 0.0F ||
        settings_.crushGraceSeconds < 0.0F || settings_.maximumSlopeDegrees < 0.0F ||
        settings_.maximumSlopeDegrees >= 89.0F || settings_.ladderClimbSpeedPixelsPerSecond < 0.0F ||
        settings_.ladderHorizontalSpeedPixelsPerSecond < 0.0F ||
        settings_.ladderSnapSpeedPixelsPerSecond < 0.0F) {
        return fail("character settings contain a negative or out-of-range value");
    }
    if (!(settings_.jumpReleaseVelocityFactor > 0.0F) ||
        settings_.jumpReleaseVelocityFactor > 1.0F || settings_.riseGravityScale < 0.0F ||
        settings_.fallGravityScale < 0.0F) {
        return fail("jump release and gravity scales are out of range");
    }
    if (settings_.groundProbeInsetPixels >= settings_.colliderHalfExtentsPixels.x) {
        return fail("ground probe inset must be smaller than the collider half width");
    }
    if (settings_.groundCategoryBits == 0U || settings_.groundMaskBits == 0U) {
        return fail("ground query category and mask bits must be non-zero");
    }
    return true;
}

void CharacterController2D::reset() noexcept {
    state_ = {};
    state_.facingRight = true;
    state_.groundNormal = {0.0F, -1.0F};
    ladderContact_ = false;
    ladderHasCenter_ = false;
    ladderCenterX_ = 0.0F;
    jumpWasHeld_ = false;
    appliedTransportVelocityPixelsPerSecond_ = {};
    crushCandidateSeconds_ = 0.0F;
}

CharacterController2DSnapshot CharacterController2D::capture_snapshot() const noexcept {
    CharacterController2DSnapshot snapshot;
    snapshot.state = state_;
    snapshot.ladderContact = ladderContact_;
    snapshot.ladderHasCenter = ladderHasCenter_;
    snapshot.ladderCenterX = ladderCenterX_;
    snapshot.jumpWasHeld = jumpWasHeld_;
    snapshot.appliedTransportVelocityPixelsPerSecond = appliedTransportVelocityPixelsPerSecond_;
    snapshot.crushCandidateSeconds = crushCandidateSeconds_;
    return snapshot;
}

bool CharacterController2D::restore_snapshot(const CharacterController2DSnapshot& snapshot) noexcept {
    if (!finite(snapshot.state.groundNormal) ||
        !finite(snapshot.state.supportVelocityPixelsPerSecond) ||
        !finite(snapshot.state.groundSurfaceVelocityPixelsPerSecond) ||
        !finite(snapshot.state.leftWallVelocityPixelsPerSecond) ||
        !finite(snapshot.state.rightWallVelocityPixelsPerSecond) ||
        !finite(snapshot.state.ceilingVelocityPixelsPerSecond) ||
        !finite(snapshot.appliedTransportVelocityPixelsPerSecond) ||
        !std::isfinite(snapshot.state.slopeDegrees) ||
        !std::isfinite(snapshot.state.crushClosingSpeedPixelsPerSecond) ||
        !std::isfinite(snapshot.crushCandidateSeconds) ||
        !std::isfinite(snapshot.state.coyoteRemainingSeconds) ||
        !std::isfinite(snapshot.state.jumpBufferRemainingSeconds) ||
        !std::isfinite(snapshot.ladderCenterX) || snapshot.state.coyoteRemainingSeconds < 0.0F ||
        snapshot.state.jumpBufferRemainingSeconds < 0.0F || snapshot.crushCandidateSeconds < 0.0F) {
        return false;
    }
    state_ = snapshot.state;
    ladderContact_ = snapshot.ladderContact;
    ladderHasCenter_ = snapshot.ladderContact && snapshot.ladderHasCenter;
    ladderCenterX_ = ladderHasCenter_ ? snapshot.ladderCenterX : 0.0F;
    jumpWasHeld_ = snapshot.jumpWasHeld;
    appliedTransportVelocityPixelsPerSecond_ = snapshot.appliedTransportVelocityPixelsPerSecond;
    crushCandidateSeconds_ = snapshot.crushCandidateSeconds;
    state_.ladderAvailable = ladderContact_;
    if (!ladderContact_) state_.climbingLadder = false;
    return true;
}

void CharacterController2D::set_ladder_contact(bool available, float centerX, bool hasCenter) noexcept {
    ladderContact_ = available;
    ladderHasCenter_ = available && hasCenter && std::isfinite(centerX);
    ladderCenterX_ = ladderHasCenter_ ? centerX : 0.0F;
    state_.ladderAvailable = available;
    if (!available) state_.climbingLadder = false;
}

float CharacterController2D::move_towards(float current, float target, float maximumDelta) noexcept {
    if (current < target) return std::min(current + maximumDelta, target);
    if (current > target) return std::max(current - maximumDelta, target);
    return target;
}

bool CharacterController2D::pre_step(Physics2DWorld& world, Physics2DBodyHandle body,
                                     const CharacterController2DInput& input,
                                     float frameDeltaSeconds) {
    state_.jumpedThisFrame = false;
    state_.droppedThroughThisFrame = false;
    if (!(frameDeltaSeconds > 0.0F) || !std::isfinite(frameDeltaSeconds)) return false;

    Physics2DBodyState bodyState;
    if (!world.body_state(body, bodyState)) return false;

    const float moveX = clamp_axis(input.moveX);
    const float moveY = clamp_axis(input.moveY);
    if (moveX > 0.001F) state_.facingRight = true;
    else if (moveX < -0.001F) state_.facingRight = false;

    state_.jumpBufferRemainingSeconds = std::max(0.0F, state_.jumpBufferRemainingSeconds - frameDeltaSeconds);
    state_.coyoteRemainingSeconds = std::max(0.0F, state_.coyoteRemainingSeconds - frameDeltaSeconds);
    if (input.jumpPressed) state_.jumpBufferRemainingSeconds = settings_.jumpBufferSeconds;

    state_.ladderAvailable = ladderContact_;
    if (ladderContact_) {
        if (!state_.climbingLadder && std::fabs(moveY) > 0.10F) state_.climbingLadder = true;
        if (state_.climbingLadder && input.jumpPressed && std::fabs(moveY) < 0.10F) {
            state_.climbingLadder = false;
            state_.jumpBufferRemainingSeconds = settings_.jumpBufferSeconds;
        }
    } else {
        state_.climbingLadder = false;
    }

    TileVec2 velocity = bodyState.linearVelocityPixelsPerSecond;
    velocity.x -= appliedTransportVelocityPixelsPerSecond_.x;
    velocity.y -= appliedTransportVelocityPixelsPerSecond_.y;
    appliedTransportVelocityPixelsPerSecond_ = {};

    if (state_.climbingLadder) {
        static_cast<void>(world.set_body_gravity_scale(body, 0.0F));
        const float targetX = moveX * settings_.ladderHorizontalSpeedPixelsPerSecond;
        velocity.x = move_towards(velocity.x, targetX,
                                  settings_.ladderSnapSpeedPixelsPerSecond * frameDeltaSeconds);
        if (settings_.snapToLadderCenter && ladderHasCenter_) {
            const float offset = ladderCenterX_ - bodyState.positionPixels.x;
            const float snapVelocity = std::clamp(offset / frameDeltaSeconds,
                                                  -settings_.ladderSnapSpeedPixelsPerSecond,
                                                  settings_.ladderSnapSpeedPixelsPerSecond);
            velocity.x = move_towards(velocity.x, snapVelocity,
                                      settings_.ladderSnapSpeedPixelsPerSecond * frameDeltaSeconds);
        }
        velocity.y = moveY * settings_.ladderClimbSpeedPixelsPerSecond;
        if (input.dropPressed && moveY > 0.0F) {
            state_.climbingLadder = false;
        } else {
            jumpWasHeld_ = input.jumpHeld;
            return world.set_body_linear_velocity(body, velocity);
        }
    }

    const bool canJump = state_.grounded || state_.coyoteRemainingSeconds > 0.0F;
    if (input.dropPressed && state_.grounded) {
        if (world.drop_through_one_way(body, settings_.oneWayDropSeconds)) {
            state_.grounded = false;
            state_.coyoteRemainingSeconds = 0.0F;
            state_.droppedThroughThisFrame = true;
            velocity.y = std::max(velocity.y, 30.0F);
        }
    } else if (state_.jumpBufferRemainingSeconds > 0.0F && canJump) {
        velocity.y = -settings_.jumpSpeedPixelsPerSecond;
        if (settings_.inheritSupportVelocity) {
            velocity.x += state_.supportVelocityPixelsPerSecond.x;
            if (settings_.inheritVerticalSupportVelocityOnJump) {
                velocity.y += std::min(0.0F, state_.supportVelocityPixelsPerSecond.y);
            }
        }
        state_.jumpBufferRemainingSeconds = 0.0F;
        state_.coyoteRemainingSeconds = 0.0F;
        state_.grounded = false;
        state_.onSlope = false;
        state_.supportBody = {};
        state_.supportVelocityPixelsPerSecond = {};
        state_.groundSurfaceVelocityPixelsPerSecond = {};
        state_.jumpedThisFrame = true;
    }

    const float supportHorizontal = (state_.grounded && settings_.inheritSupportVelocity)
                                        ? state_.supportVelocityPixelsPerSecond.x
                                        : 0.0F;
    const float surfaceHorizontal = state_.grounded
                                        ? state_.groundSurfaceVelocityPixelsPerSecond.x
                                        : 0.0F;
    const float transportHorizontal = supportHorizontal + surfaceHorizontal;
    const float targetHorizontal = moveX * settings_.maximumRunSpeedPixelsPerSecond +
                                   transportHorizontal;
    appliedTransportVelocityPixelsPerSecond_.x = transportHorizontal;
    const bool accelerating = std::fabs(targetHorizontal) > std::fabs(velocity.x) ||
                              (targetHorizontal != 0.0F && std::signbit(targetHorizontal) != std::signbit(velocity.x));
    float horizontalRate{};
    if (state_.grounded) {
        horizontalRate = accelerating ? settings_.groundAccelerationPixelsPerSecondSquared
                                      : settings_.groundDecelerationPixelsPerSecondSquared;
    } else {
        horizontalRate = accelerating ? settings_.airAccelerationPixelsPerSecondSquared
                                      : settings_.airDecelerationPixelsPerSecondSquared;
    }
    velocity.x = move_towards(velocity.x, targetHorizontal, horizontalRate * frameDeltaSeconds);

    if (state_.grounded && state_.onSlope && std::fabs(state_.groundNormal.y) > 0.05F &&
        !state_.jumpedThisFrame) {
        velocity.y = -(state_.groundNormal.x / state_.groundNormal.y) * velocity.x;
    }

    if (velocity.y < 0.0F) {
        static_cast<void>(world.set_body_gravity_scale(body, settings_.riseGravityScale));
        if (jumpWasHeld_ && !input.jumpHeld && !state_.jumpedThisFrame) {
            velocity.y *= settings_.jumpReleaseVelocityFactor;
        }
    } else {
        static_cast<void>(world.set_body_gravity_scale(body, settings_.fallGravityScale));
        velocity.y = std::min(velocity.y, settings_.maximumFallSpeedPixelsPerSecond);
    }

    jumpWasHeld_ = input.jumpHeld;
    return world.set_body_linear_velocity(body, velocity);
}

bool CharacterController2D::post_step(Physics2DWorld& world, Physics2DBodyHandle body,
                                      float frameDeltaSeconds) {
    if (!(frameDeltaSeconds > 0.0F) || !std::isfinite(frameDeltaSeconds)) return false;
    Physics2DBodyState bodyState;
    if (!world.body_state(body, bodyState)) return false;

    const bool wasGrounded = state_.grounded;
    const bool nowGrounded = !state_.climbingLadder && probe_ground(world, bodyState);
    state_.grounded = nowGrounded;
    if (nowGrounded) {
        state_.coyoteRemainingSeconds = settings_.coyoteTimeSeconds;
    } else if (wasGrounded) {
        state_.coyoteRemainingSeconds = std::max(state_.coyoteRemainingSeconds,
                                                 settings_.coyoteTimeSeconds);
    }
    probe_surroundings(world, bodyState, frameDeltaSeconds);
    return true;
}

bool CharacterController2D::apply_knockback(Physics2DWorld& world, Physics2DBodyHandle body,
                                             TileVec2 velocityPixelsPerSecond,
                                             bool clearJumpWindows) {
    if (!finite(velocityPixelsPerSecond)) return false;
    if (!world.set_body_linear_velocity(body, velocityPixelsPerSecond)) return false;
    state_.grounded = false;
    state_.onSlope = false;
    state_.climbingLadder = false;
    state_.supportBody = {};
    state_.supportVelocityPixelsPerSecond = {};
    state_.groundSurfaceVelocityPixelsPerSecond = {};
    appliedTransportVelocityPixelsPerSecond_ = {};
    crushCandidateSeconds_ = 0.0F;
    state_.crushed = false;
    if (clearJumpWindows) {
        state_.coyoteRemainingSeconds = 0.0F;
        state_.jumpBufferRemainingSeconds = 0.0F;
    }
    return true;
}

bool CharacterController2D::probe_ground(Physics2DWorld& world,
                                         const Physics2DBodyState& bodyState) {
    const float footY = bodyState.positionPixels.y + settings_.colliderCenterPixels.y +
                        settings_.colliderHalfExtentsPixels.y;
    const float halfWidth = settings_.colliderHalfExtentsPixels.x;
    const float inset = settings_.groundProbeInsetPixels;
    const std::array<float, 3> xOffsets{-halfWidth + inset, 0.0F, halfWidth - inset};
    const float minimumUpNormal = std::cos(settings_.maximumSlopeDegrees * kPi / 180.0F);

    Physics2DRayCastHit best;
    best.fraction = std::numeric_limits<float>::infinity();
    for (const float xOffset : xOffsets) {
        const TileVec2 origin{bodyState.positionPixels.x + settings_.colliderCenterPixels.x + xOffset,
                              footY - 0.05F};
        const Physics2DRayCastHit hit = world.ray_cast(
            origin, {0.0F, settings_.groundProbeDistancePixels + 0.05F},
            settings_.groundCategoryBits, settings_.groundMaskBits);
        if (!hit.hit || hit.normal.y > -minimumUpNormal || hit.fraction >= best.fraction) continue;
        best = hit;
    }

    if (!best.hit) {
        state_.onSlope = false;
        state_.groundNormal = {0.0F, -1.0F};
        state_.slopeDegrees = 0.0F;
        state_.supportBody = {};
        state_.supportVelocityPixelsPerSecond = {};
        state_.groundSurfaceVelocityPixelsPerSecond = {};
        return false;
    }

    state_.groundNormal = best.normal;
    state_.slopeDegrees = std::atan2(std::fabs(best.normal.x), std::max(0.0001F, -best.normal.y)) *
                          180.0F / kPi;
    state_.onSlope = state_.slopeDegrees > 0.25F;
    state_.supportBody = best.body;
    state_.supportVelocityPixelsPerSecond = {};
    state_.groundSurfaceVelocityPixelsPerSecond = best.surfaceVelocityPixelsPerSecond;
    if (settings_.inheritSupportVelocity && best.body) {
        Physics2DBodyState support;
        if (world.body_state(best.body, support)) {
            state_.supportVelocityPixelsPerSecond = support.linearVelocityPixelsPerSecond;
        }
    }
    return true;
}

void CharacterController2D::probe_surroundings(Physics2DWorld& world,
                                                const Physics2DBodyState& bodyState,
                                                float frameDeltaSeconds) {
    const TileVec2 center{bodyState.positionPixels.x + settings_.colliderCenterPixels.x,
                          bodyState.positionPixels.y + settings_.colliderCenterPixels.y};
    const TileVec2 half = settings_.colliderHalfExtentsPixels;
    const auto velocity_for = [&world](Physics2DBodyHandle handle) {
        Physics2DBodyState state;
        return handle && world.body_state(handle, state) ? state.linearVelocityPixelsPerSecond : TileVec2{};
    };
    const auto probe = [&](TileVec2 origin, TileVec2 translation) {
        return world.ray_cast(origin, translation, settings_.groundCategoryBits, settings_.groundMaskBits);
    };

    const float insetY = std::min(half.y * 0.5F, 3.0F);
    const Physics2DRayCastHit left = probe(
        {center.x - half.x - 0.05F, center.y - insetY},
        {-settings_.wallProbeDistancePixels, 0.0F});
    const Physics2DRayCastHit right = probe(
        {center.x + half.x + 0.05F, center.y - insetY},
        {settings_.wallProbeDistancePixels, 0.0F});
    const Physics2DRayCastHit ceilingLeft = probe(
        {center.x - half.x + settings_.groundProbeInsetPixels, center.y - half.y - 0.05F},
        {0.0F, -settings_.ceilingProbeDistancePixels});
    const Physics2DRayCastHit ceilingRight = probe(
        {center.x + half.x - settings_.groundProbeInsetPixels, center.y - half.y - 0.05F},
        {0.0F, -settings_.ceilingProbeDistancePixels});
    const Physics2DRayCastHit ceiling = ceilingLeft.hit &&
        (!ceilingRight.hit || ceilingLeft.fraction <= ceilingRight.fraction) ? ceilingLeft : ceilingRight;

    state_.touchingLeftWall = left.hit && left.normal.x > 0.5F;
    state_.touchingRightWall = right.hit && right.normal.x < -0.5F;
    state_.touchingCeiling = ceiling.hit && ceiling.normal.y > 0.5F;
    state_.leftWallBody = state_.touchingLeftWall ? left.body : Physics2DBodyHandle{};
    state_.rightWallBody = state_.touchingRightWall ? right.body : Physics2DBodyHandle{};
    state_.ceilingBody = state_.touchingCeiling ? ceiling.body : Physics2DBodyHandle{};
    state_.leftWallVelocityPixelsPerSecond = velocity_for(state_.leftWallBody);
    state_.rightWallVelocityPixelsPerSecond = velocity_for(state_.rightWallBody);
    state_.ceilingVelocityPixelsPerSecond = velocity_for(state_.ceilingBody);

    const float facing = state_.facingRight ? 1.0F : -1.0F;
    const TileVec2 ledgeOrigin{center.x + facing * (half.x + settings_.ledgeProbeForwardPixels),
                               center.y + half.y - 0.5F};
    const Physics2DRayCastHit ledgeFloor = probe(ledgeOrigin, {0.0F, settings_.ledgeProbeDownPixels});
    state_.nearLedge = state_.grounded && !ledgeFloor.hit;

    float closingSpeed = 0.0F;
    if (state_.grounded && state_.touchingCeiling) {
        closingSpeed = std::max(closingSpeed, state_.ceilingVelocityPixelsPerSecond.y -
            state_.supportVelocityPixelsPerSecond.y);
    }
    if (state_.touchingLeftWall && state_.touchingRightWall) {
        closingSpeed = std::max(closingSpeed, state_.leftWallVelocityPixelsPerSecond.x -
            state_.rightWallVelocityPixelsPerSecond.x);
    }
    state_.crushClosingSpeedPixelsPerSecond = std::max(0.0F, closingSpeed);
    const bool candidate = settings_.enableCrushDetection &&
        state_.crushClosingSpeedPixelsPerSecond >= settings_.crushMinimumClosingSpeedPixelsPerSecond;
    crushCandidateSeconds_ = candidate ? crushCandidateSeconds_ + frameDeltaSeconds : 0.0F;
    state_.crushed = candidate && crushCandidateSeconds_ >= settings_.crushGraceSeconds;
}

} // namespace dve
