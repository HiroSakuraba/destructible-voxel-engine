#pragma once
// M7: deterministic, stateless two-link IK for visual mech legs.
// World is Z-up, joints are in metres, and input geometry has no dependency
// on GameWorld, rendering, editor assets or a specific rigid-body backend.
#include "dve/transform.hpp"
#include <optional>
namespace dve {
struct MechLegIkConfig {
    float thighLength{1.15F};
    float shinLength{1.10F};
    float minimumKneeBendRadians{0.04F};
};
struct MechLegIkPose {
    Float3 hip{}, knee{}, ankle{};
    Float3 thighCenter{}, shinCenter{};
    Quaternion thighRotation{}, shinRotation{};
    bool reachClamped{};
};
// Pole indicates the preferred forward-facing knee bend in world coordinates.
// Unreachable feet clamp to the nearest point in the joint's admissible range,
// rather than introducing stretch or NaNs.
// boneForward fixes the roll of the thigh/shin rotations about their own axes
// (local +Y is its projection perpendicular to the bone) so non-square limb
// meshes follow the chassis heading instead of the bone's tilt direction. A
// zero or non-finite boneForward falls back to the knee direction.
[[nodiscard]] std::optional<MechLegIkPose> solve_mech_leg_ik(
    Float3 hip, Float3 desiredAnkle, Float3 kneePole,
    const MechLegIkConfig& config={}, Float3 boneForward={}) noexcept;
[[nodiscard]] Quaternion mech_align_z_axis(Float3 direction) noexcept;
// Limit visual ankle roll/pitch on slopes to avoid impossible mechanical poses.
// The stance solver keeps its actual support normal for terrain and VFX.
[[nodiscard]] Float3 mech_limit_ankle_normal(Float3 contactNormal,
                                             float maximumTiltRadians=0.61F) noexcept;
} // namespace dve
