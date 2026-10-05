#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "dve/physics_contact_sink.hpp"
#include "dve/rigid_body_adapter.hpp"

namespace dve {

struct Box3DWorldConfig {
    // Box3D can own an internal scheduler. One worker is the conservative deterministic
    // default and avoids oversubscribing DVE's own job system. Higher values are opt-in.
    std::uint32_t workerThreads{1U};
    std::uint32_t subStepCount{4U};
    std::uint32_t expectedBodyCount{32768U};
    std::uint32_t expectedDynamicBodyCount{16384U};
    std::uint32_t expectedContactCount{65536U};
    float hitEventThresholdMetersPerSecond{0.25F};
    bool enableSleeping{true};
    bool enableContinuousCollision{true};
};


struct Box3DRayHit {
    RigidBodyHandle body{kInvalidRigidBodyHandle};
    Float3 point{};
    Float3 normal{};
    float fraction{1.0F};
    std::uint16_t material{};
    int triangleIndex{-1};
    int childIndex{-1};
};

struct Box3DOverlapHit {
    RigidBodyHandle body{kInvalidRigidBodyHandle};
    std::uint16_t material{};
};

struct Box3DPhysicsTelemetry {
    std::uint32_t versionMajor{};
    std::uint32_t versionMinor{};
    std::uint32_t versionRevision{};
    std::uint32_t configuredWorkerThreads{};
    std::uint32_t configuredSubStepCount{};
    std::uint64_t stepCalls{};
    std::uint64_t invalidStepInputs{};
    std::uint64_t contactHitEvents{};
    std::uint64_t contactEventsQueued{};
    std::uint64_t contactEventsRejected{};
    std::size_t box3dAllocatedBytes{};
    RigidBodyCounts bodyCounts{};
    std::size_t constraintCount{};
};

// Optional production 3D rigid-body backend built on Box3D 0.1.x.
//
// The engine-facing body frame remains the authoritative center-of-mass frame. Each
// SolverBox is attached as an independently transformed convex hull, then Box3D's computed
// mass properties are replaced with DVE's validated mass, center, and inertia tensor.
class Box3DRigidBodyWorld final : public IRigidBodyWorld {
public:
    Box3DRigidBodyWorld();
    explicit Box3DRigidBodyWorld(const Box3DWorldConfig& config);
    ~Box3DRigidBodyWorld() override;

    Box3DRigidBodyWorld(const Box3DRigidBodyWorld&) = delete;
    Box3DRigidBodyWorld& operator=(const Box3DRigidBodyWorld&) = delete;
    Box3DRigidBodyWorld(Box3DRigidBodyWorld&&) = delete;
    Box3DRigidBodyWorld& operator=(Box3DRigidBodyWorld&&) = delete;

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
    [[nodiscard]] RigidBodyConstraintHandle create_constraint(
        const RigidBodyConstraintDesc& desc) override;
    bool destroy_constraint(RigidBodyConstraintHandle handle) override;
    void step(float fixedDeltaSeconds) override;

    bool teleport_body(
        RigidBodyHandle handle,
        const RigidTransform& transform,
        Float3 linearVelocity = {},
        Float3 angularVelocity = {});
    void set_gravity(Float3 acceleration) noexcept;
    bool set_damping(RigidBodyHandle handle, float linearDamping, float angularDamping);
    bool apply_impulse_at_point(
        RigidBodyHandle handle, Float3 impulse, Float3 worldPoint) override;
    bool apply_force_at_point(
        RigidBodyHandle handle, Float3 force, Float3 worldPoint) override;
    bool apply_angular_impulse(
        RigidBodyHandle handle, Float3 worldAngularImpulse) override;
    bool apply_torque(RigidBodyHandle handle, Float3 worldTorque) override;

    [[nodiscard]] std::vector<RigidBodyQueryHit> ray_cast_all(
        Float3 origin, Float3 direction, float maximumDistance,
        const RigidBodyQueryFilter& filter = {}) const override;
    [[nodiscard]] std::vector<RigidBodyQueryHit> overlap_aabb(
        RigidBodyWorldBounds bounds,
        const RigidBodyQueryFilter& filter = {}) const override;
    [[nodiscard]] std::vector<RigidBodyQueryHit> cast_sphere_all(
        Float3 origin, float radius, Float3 direction, float maximumDistance,
        const RigidBodyQueryFilter& filter = {}) const override;

    // The sink is consumed after b3World_Step, never from a Box3D worker callback. The sink
    // must remain alive while registered.
    void set_contact_sink(IPhysicsContactSink* sink) noexcept override;
    bool set_contact_material(
        RigidBodyHandle handle, std::uint16_t material) noexcept override;

    [[nodiscard]] RigidBodyHandle create_static_box(
        const RigidTransform& transform, Float3 halfExtents);
    [[nodiscard]] RigidBodyHandle create_static_triangle_mesh(
        const RigidTransform& transform,
        std::span<const Float3> vertices,
        std::span<const std::uint32_t> triangleIndices,
        Float3 scale = {1.0F, 1.0F, 1.0F},
        std::uint16_t material = 0U);
    [[nodiscard]] std::optional<Box3DRayHit> ray_cast_closest(
        Float3 origin, Float3 translation, std::uint64_t maskBits = ~std::uint64_t{0}) const;
    [[nodiscard]] std::size_t query_aabb(
        const RigidBodyWorldBounds& bounds,
        std::span<Box3DOverlapHit> output,
        std::uint64_t maskBits = ~std::uint64_t{0}) const;
    [[nodiscard]] std::optional<bool> is_active(RigidBodyHandle handle) const;
    [[nodiscard]] RigidTransform interpolated_transform(
        RigidBodyHandle handle, float alpha) const;
    [[nodiscard]] RigidBodyCounts body_counts() const;
    [[nodiscard]] std::size_t body_count() const;
    [[nodiscard]] std::size_t dynamic_body_count() const;
    [[nodiscard]] std::size_t awake_body_count() const;
    [[nodiscard]] std::size_t sleeping_body_count() const;
    [[nodiscard]] std::size_t constraint_count() const noexcept;
    [[nodiscard]] Box3DPhysicsTelemetry telemetry() const;
    [[nodiscard]] std::optional<Float3> solver_shape_center_of_mass_local(
        RigidBodyHandle handle) const;
    [[nodiscard]] std::optional<RigidBodyWorldBounds> solver_world_bounds(
        RigidBodyHandle handle) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dve
