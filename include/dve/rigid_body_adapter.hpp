#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "dve/fragment.hpp"

namespace dve {

class IPhysicsContactSink;

using RigidBodyHandle = std::uint32_t;
constexpr RigidBodyHandle kInvalidRigidBodyHandle = 0xFFFFFFFFU;
using RigidBodyConstraintHandle = std::uint32_t;
constexpr RigidBodyConstraintHandle kInvalidRigidBodyConstraintHandle = 0xFFFFFFFFU;

struct SolverInertiaTensor {
    double xx{};
    double yy{};
    double zz{};
    double xy{};
    double xz{};
    double yz{};
};

enum class RigidBodyCollisionClass : std::uint8_t {
    Full,          // collides with static world, full bodies, and debris
    DebrisNoSelf,  // collides with static world and full bodies, but not other DebrisNoSelf bodies
};

struct StaticRigidBodyCreateDesc {
    // World transform of the authored object-local collision frame.
    RigidTransform transform{};
    std::vector<SolverBox> boxes{}; // meters, relative to the object origin
    RigidBodyCollisionClass collisionClass{RigidBodyCollisionClass::Full};
};

struct RigidBodyCreateDesc {
    // World transform of the authoritative center-of-mass frame.
    RigidTransform transform{};
    double massKilograms{};
    SolverInertiaTensor inertiaKilogramMetersSquared{};
    std::vector<SolverBox> boxes{}; // meters, relative to the authoritative center of mass
    Float3 linearVelocity{};        // world-space center-of-mass velocity, m/s
    Float3 angularVelocity{};       // world-space angular velocity, rad/s
    bool allowSleeping{true};
    bool useContinuousCollision{};  // requests solver linear-cast / CCD motion quality
    RigidBodyCollisionClass collisionClass{RigidBodyCollisionClass::Full};
};

struct RigidBodyState {
    RigidTransform previousTransform{};
    RigidTransform currentTransform{};
    Float3 linearVelocity{};
    Float3 angularVelocity{};
    bool sleeping{};
};

struct RigidBodyCounts {
    std::size_t total{};
    std::size_t staticBodies{};
    std::size_t dynamicBodies{};
    std::size_t awakeDynamicBodies{};
    std::size_t sleepingDynamicBodies{};
};

struct RigidBodyWorldBounds {
    Float3 minimum{};
    Float3 maximum{};
};

struct RigidBodyQueryFilter {
    std::vector<RigidBodyHandle> ignoredBodies;
    bool includeStatic{true};
    bool includeDynamic{true};
};

struct RigidBodyQueryHit {
    RigidBodyHandle body{kInvalidRigidBodyHandle};
    Float3 point{};
    Float3 normal{};
    float fraction{1.0F};
    float distance{};
    std::uint16_t material{};
    std::uint32_t subShape{};
};

enum class RigidBodyConstraintKind : std::uint8_t { Ball, Hinge, ConeTwist, Fixed };

struct RigidBodyConstraintDesc {
    RigidBodyHandle parentBody{kInvalidRigidBodyHandle};
    RigidBodyHandle childBody{kInvalidRigidBodyHandle};
    RigidBodyConstraintKind kind{RigidBodyConstraintKind::ConeTwist};
    Float3 parentAnchorLocal{};
    Float3 childAnchorLocal{};
    Float3 parentAxisLocal{1.0F, 0.0F, 0.0F};
    Quaternion referenceRotation{}; // child rotation relative to parent at creation
    float minimumRadians{-0.785398F};
    float maximumRadians{0.785398F};
    float swingLimitRadians{0.785398F};
};

[[nodiscard]] bool validate_rigid_body_constraint_desc(
    const RigidBodyConstraintDesc& desc) noexcept;

struct FragmentSolverScale {
    double kilogramsPerDensityUnit{0.001};
    double metersPerVoxel{0.1};
};

enum class RigidBodyValidationError : std::uint8_t {
    NoError,
    EmptyCollisionShape,
    InvalidMass,
    InvalidTransform,
    InvalidVelocity,
    InvalidCollisionBox,
    InvalidInertia,
    SingularInertiaAfterFloatConversion,
};

struct RigidBodyValidationResult {
    RigidBodyValidationError error{RigidBodyValidationError::NoError};
    RigidTransform normalizedTransform{};
    float massKilogramsFloat{};
    SolverInertiaTensor inertiaAfterFloatConversion{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == RigidBodyValidationError::NoError;
    }
};

[[nodiscard]] bool inertia_is_positive_semidefinite(
    const SolverInertiaTensor& inertia,
    double tolerance = 1.0e-10) noexcept;

[[nodiscard]] bool inertia_is_positive_definite(
    const SolverInertiaTensor& inertia) noexcept;

// Central validation contract shared by every rigid-body backend. The returned transform is
// quaternion-normalized and the mass/inertia fields reflect the float precision that a game
// solver such as Jolt will actually consume. Dynamic three-dimensional bodies require a
// positive-definite (invertible) inertia tensor after that conversion.
[[nodiscard]] RigidBodyValidationResult validate_rigid_body_desc(
    const RigidBodyCreateDesc& desc) noexcept;

[[nodiscard]] bool rigid_body_state_is_finite(const RigidBodyState& state) noexcept;
[[nodiscard]] bool validate_static_rigid_body_desc(
    const StaticRigidBodyCreateDesc& desc) noexcept;

// Center-of-mass velocity inherited by a child fragment split from a moving parent:
// v_child = v_parent + omega_parent x (r_child - r_parent).
[[nodiscard]] Float3 inherited_child_center_of_mass_velocity(
    Float3 parentLinearVelocity,
    Float3 parentAngularVelocity,
    Float3 parentCenterOfMassWorld,
    Float3 childCenterOfMassWorld) noexcept;

// Converts the complete FragmentSolverPackage from voxel units to solver SI units,
// including the body-frame world translation. Callers must not post-scale the result.
[[nodiscard]] std::optional<RigidBodyCreateDesc> make_rigid_body_desc(
    const FragmentSolverPackage& package,
    const FragmentSolverScale& scale,
    Float3 inheritedLinearVelocity = {},
    Float3 inheritedAngularVelocity = {});

// Convenience path for a fragment split from a moving parent body. The child inherits the
// parent angular velocity and the correct tangential center-of-mass velocity.
[[nodiscard]] std::optional<RigidBodyCreateDesc> make_moving_split_rigid_body_desc(
    const FragmentSolverPackage& childPackage,
    const FragmentSolverScale& scale,
    const RigidBodyState& parentState);

class IRigidBodyWorld {
public:
    virtual ~IRigidBodyWorld() = default;
    [[nodiscard]] virtual RigidBodyHandle create_body(const RigidBodyCreateDesc& desc) = 0;
    [[nodiscard]] virtual std::vector<RigidBodyHandle> create_bodies(
        std::span<const RigidBodyCreateDesc> descs);

    [[nodiscard]] virtual RigidBodyHandle create_static_body(
        const StaticRigidBodyCreateDesc& desc);
    [[nodiscard]] virtual std::vector<RigidBodyHandle> create_static_bodies(
        std::span<const StaticRigidBodyCreateDesc> descs);

    virtual bool destroy_body(RigidBodyHandle handle) = 0;
    virtual bool destroy_bodies(std::span<const RigidBodyHandle> handles);
    [[nodiscard]] virtual std::optional<RigidBodyState> state(RigidBodyHandle handle) const = 0;

    // Restores the complete externally visible state, including interpolation history and
    // sleeping status. Backends may expose a separate teleport helper for the common case.
    virtual bool set_state(RigidBodyHandle handle, const RigidBodyState& state) = 0;
    // Center-of-mass impulse/force operations used by gameplay scripting. Backends wake the
    // body and reject static, unknown, or non-finite requests.
    virtual bool apply_impulse(RigidBodyHandle handle, Float3 worldImpulse) = 0;
    virtual bool apply_force(RigidBodyHandle handle, Float3 worldForce) = 0;
    // World-point loads preserve torque from off-center attachments. Backends without native
    // point-load support retain center-of-mass behavior as a conservative fallback.
    virtual bool apply_impulse_at_point(
        RigidBodyHandle handle, Float3 worldImpulse, Float3 worldPoint);
    virtual bool apply_force_at_point(
        RigidBodyHandle handle, Float3 worldForce, Float3 worldPoint);
    // Explicit angular loads. Angular impulse is measured in N*m*s and torque in N*m.
    // Backends that do not expose angular loads return false rather than discarding them.
    virtual bool apply_angular_impulse(RigidBodyHandle handle, Float3 worldAngularImpulse);
    virtual bool apply_torque(RigidBodyHandle handle, Float3 worldTorque);
    // Solver-neutral, deterministic query-all operations. Implementations sort by fraction,
    // distance, body, then sub-shape and honor ignored bodies before returning.
    [[nodiscard]] virtual std::vector<RigidBodyQueryHit> ray_cast_all(
        Float3 origin, Float3 direction, float maximumDistance,
        const RigidBodyQueryFilter& filter = {}) const;
    [[nodiscard]] virtual std::vector<RigidBodyQueryHit> overlap_aabb(
        RigidBodyWorldBounds bounds, const RigidBodyQueryFilter& filter = {}) const;
    [[nodiscard]] virtual std::vector<RigidBodyQueryHit> cast_sphere_all(
        Float3 origin, float radius, Float3 direction, float maximumDistance,
        const RigidBodyQueryFilter& filter = {}) const;
    // A null sink disconnects the consumer. The sink must remain alive while registered.
    virtual void set_contact_sink(IPhysicsContactSink* sink) noexcept;
    virtual bool set_contact_material(RigidBodyHandle handle, std::uint16_t material) noexcept;
    // Backends that do not implement constraints return the invalid handle. This keeps existing
    // body backends source-compatible while making unsupported live ragdolls fail explicitly.
    [[nodiscard]] virtual RigidBodyConstraintHandle create_constraint(
        const RigidBodyConstraintDesc& desc);
    virtual bool destroy_constraint(RigidBodyConstraintHandle handle);
    virtual bool destroy_constraints(std::span<const RigidBodyConstraintHandle> handles);
    virtual void step(float fixedDeltaSeconds) = 0;
};

// Deterministic adapter/reference world used to validate body creation,
// center-of-mass frames, interpolation, gravity integration, and sleep hooks.
// It deliberately does not claim to be the production contact solver.
class ReferenceRigidBodyWorld final : public IRigidBodyWorld {
public:
    [[nodiscard]] RigidBodyHandle create_body(const RigidBodyCreateDesc& desc) override;
    [[nodiscard]] std::vector<RigidBodyHandle> create_bodies(
        std::span<const RigidBodyCreateDesc> descs) override;
    [[nodiscard]] RigidBodyHandle create_static_body(
        const StaticRigidBodyCreateDesc& desc) override;
    [[nodiscard]] std::vector<RigidBodyHandle> create_static_bodies(
        std::span<const StaticRigidBodyCreateDesc> descs) override;
    bool destroy_body(RigidBodyHandle handle) override;
    [[nodiscard]] std::optional<RigidBodyState> state(RigidBodyHandle handle) const override;
    bool set_state(RigidBodyHandle handle, const RigidBodyState& state) override;
    bool apply_impulse(RigidBodyHandle handle, Float3 worldImpulse) override;
    bool apply_force(RigidBodyHandle handle, Float3 worldForce) override;
    bool apply_impulse_at_point(
        RigidBodyHandle handle, Float3 worldImpulse, Float3 worldPoint) override;
    bool apply_force_at_point(
        RigidBodyHandle handle, Float3 worldForce, Float3 worldPoint) override;
    bool apply_angular_impulse(RigidBodyHandle handle, Float3 worldAngularImpulse) override;
    bool apply_torque(RigidBodyHandle handle, Float3 worldTorque) override;
    [[nodiscard]] std::vector<RigidBodyQueryHit> ray_cast_all(
        Float3 origin, Float3 direction, float maximumDistance,
        const RigidBodyQueryFilter& filter = {}) const override;
    [[nodiscard]] std::vector<RigidBodyQueryHit> overlap_aabb(
        RigidBodyWorldBounds bounds, const RigidBodyQueryFilter& filter = {}) const override;
    [[nodiscard]] std::vector<RigidBodyQueryHit> cast_sphere_all(
        Float3 origin, float radius, Float3 direction, float maximumDistance,
        const RigidBodyQueryFilter& filter = {}) const override;
    bool set_contact_material(RigidBodyHandle handle, std::uint16_t material) noexcept override;
    [[nodiscard]] RigidBodyConstraintHandle create_constraint(
        const RigidBodyConstraintDesc& desc) override;
    bool destroy_constraint(RigidBodyConstraintHandle handle) override;
    void step(float fixedDeltaSeconds) override;

    void set_gravity(Float3 acceleration) noexcept { gravity_ = acceleration; }
    void set_sleep_thresholds(float linearSpeed, float angularSpeed, std::uint32_t quietSteps) noexcept;
    [[nodiscard]] RigidTransform interpolated_transform(RigidBodyHandle handle, float alpha) const;
    [[nodiscard]] RigidBodyCounts body_counts() const noexcept;
    [[nodiscard]] std::size_t body_count() const noexcept;
    [[nodiscard]] std::size_t dynamic_body_count() const noexcept;
    [[nodiscard]] std::size_t awake_body_count() const noexcept;
    [[nodiscard]] std::size_t sleeping_body_count() const noexcept;
    [[nodiscard]] std::size_t constraint_count() const noexcept;

    // Backward-compatible name. "Active" now consistently means awake dynamic bodies.
    [[nodiscard]] std::size_t active_body_count() const noexcept { return awake_body_count(); }

private:
    struct Body {
        bool alive{};
        bool isStatic{};
        RigidBodyCreateDesc desc{};
        StaticRigidBodyCreateDesc staticDesc{};
        RigidBodyState state{};
        std::uint32_t quietSteps{};
        std::uint16_t material{};
    };

    struct Constraint {
        bool alive{};
        RigidBodyConstraintDesc desc{};
    };

    std::vector<Body> bodies_{};
    std::vector<RigidBodyHandle> freeHandles_{};
    std::vector<Constraint> constraints_{};
    std::vector<RigidBodyConstraintHandle> freeConstraintHandles_{};
    Float3 gravity_{0.0F, -9.81F, 0.0F};
    float sleepLinearSpeed_{0.02F};
    float sleepAngularSpeed_{0.02F};
    std::uint32_t sleepQuietSteps_{30U};
};

} // namespace dve
