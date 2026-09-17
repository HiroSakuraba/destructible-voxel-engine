#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "dve/physics_contact_sink.hpp"
#include "dve/rigid_body_adapter.hpp"
#include "dve/soft_body.hpp"

namespace dve {

struct JoltWorldConfig {
    // Zero selects the conservative automatic policy (up to four workers), leaving CPU
    // capacity for the authority thread, renderer, and the engine's derived-work pool.
    std::uint32_t workerThreads{};
    std::uint32_t maxBodies{32768U};
    std::uint32_t maxBodyPairs{131072U};
    std::uint32_t maxContactConstraints{65536U};
    // Sized above Jolt 5.5's update-time allocation for maxContactConstraints at the default
    // capacity. Callers reducing this should reduce the body/contact capacities with it.
    std::size_t temporaryMemoryBytes{96U * 1024U * 1024U};

    // Explicit box rounding. The effective radius is the minimum of this value and the
    // configured fraction of the smallest half-extent. This avoids Jolt's 5 cm default from
    // turning one-voxel debris into nearly spherical collision shapes.
    float boxConvexRadiusMeters{0.005F};
    float boxConvexRadiusMaxFraction{0.10F};

    // Jolt solver tuning. The defaults match Jolt's production defaults while making the
    // determinism and sleep policy explicit in the project settings surface.
    std::uint32_t collisionSteps{1U};
    std::uint32_t velocityIterations{10U};
    std::uint32_t positionIterations{2U};
    float speculativeContactDistanceMeters{0.02F};
    float penetrationSlopMeters{0.02F};
    float timeBeforeSleepSeconds{0.5F};
    float pointVelocitySleepThreshold{0.03F};
    bool deterministicSimulation{true};
    bool allowSleeping{true};
};

// X11 reserves the token `None` as a macro. Public engine enums deliberately avoid that
// identifier so headers remain composable regardless of include order.
enum class JoltPhysicsUpdateErrorFlag : std::uint32_t {
    NoErrors = 0,
    ManifoldCacheFull = 1U << 0U,
    BodyPairCacheFull = 1U << 1U,
    ContactConstraintsFull = 1U << 2U,
};

using SolverWorldBounds = RigidBodyWorldBounds;

using JoltVirtualCharacterHandle = std::uint32_t;
constexpr JoltVirtualCharacterHandle kInvalidJoltVirtualCharacterHandle = 0xFFFFFFFFU;

using JoltSoftBodyHandle = std::uint32_t;
using JoltVehicleHandle = std::uint32_t;
using JoltMutableCompoundChildHandle = std::uint32_t;
constexpr JoltSoftBodyHandle kInvalidJoltSoftBodyHandle = 0xFFFFFFFFU;
constexpr JoltVehicleHandle kInvalidJoltVehicleHandle = 0xFFFFFFFFU;
constexpr JoltMutableCompoundChildHandle kInvalidJoltMutableCompoundChildHandle = 0xFFFFFFFFU;

enum class JoltCharacterGroundState : std::uint8_t {
    InAir,
    OnGround,
    OnSteepGround,
    NotSupported,
};

struct JoltRayHit {
    RigidBodyHandle body{kInvalidRigidBodyHandle};
    Float3 point{};
    Float3 normal{};
    float fraction{1.0F};
    std::uint16_t material{};
    std::uint32_t subShapeId{};
};

struct JoltShapeCastHit {
    RigidBodyHandle body{kInvalidRigidBodyHandle};
    Float3 point{};
    Float3 normal{};
    float fraction{1.0F};
    float penetrationDepth{};
    std::uint16_t material{};
};

struct JoltOverlapHit {
    RigidBodyHandle body{kInvalidRigidBodyHandle};
    std::uint16_t material{};
};

struct JoltVirtualCharacterDesc {
    RigidTransform transform{};
    float radiusMeters{0.35F};
    float cylinderHalfHeightMeters{0.55F};
    float massKilograms{70.0F};
    float maxStrengthNewtons{100.0F};
    float maxSlopeRadians{0.872665F};
    float characterPaddingMeters{0.02F};
    float predictiveContactDistanceMeters{0.1F};
    bool createInnerBody{true};
};

struct JoltVirtualCharacterInput {
    Float3 desiredHorizontalVelocity{};
    float jumpSpeedMetersPerSecond{};
    bool jump{};
    bool controlInAir{true};
};

struct JoltHeightFieldDesc {
    RigidTransform transform{};
    std::uint32_t sampleCount{};
    Float3 sampleOffset{};
    Float3 sampleScale{1.0F, 1.0F, 1.0F};
    std::uint32_t blockSize{2U};
    std::uint32_t bitsPerSample{12U};
    std::uint16_t material{};
};

struct JoltMutableBoxDesc {
    Float3 center{};
    Quaternion rotation{};
    Float3 halfExtents{0.5F, 0.5F, 0.5F};
    std::uint32_t userData{};
};

struct JoltMutableCompoundDesc {
    RigidTransform transform{};
    std::vector<JoltMutableBoxDesc> boxes{};
    bool dynamic{};
    double massKilograms{1.0};
    Float3 linearVelocity{};
    Float3 angularVelocity{};
    bool allowSleeping{true};
    RigidBodyCollisionClass collisionClass{RigidBodyCollisionClass::Full};
};

struct JoltSoftBodyDesc {
    RigidTransform transform{};
    std::uint32_t solverIterations{8U};
    float linearDamping{0.02F};
    float restitution{};
    float friction{0.35F};
    float pressure{};
    float gravityFactor{1.0F};
    float vertexRadiusMeters{0.01F};
    float maximumLinearVelocity{500.0F};
    bool updatePosition{true};
    bool allowSleeping{true};
    bool facesDoubleSided{true};
};

struct JoltSoftBodyVertexState {
    Float3 position{};
    Float3 velocity{};
    float inverseMass{};
};

struct JoltSoftBodyState {
    RigidTransform transform{};
    std::vector<JoltSoftBodyVertexState> vertices{};
    float volume{};
    bool sleeping{};
};

struct JoltHairStrandDesc {
    std::vector<Float3> points{};
    float vertexMassKilograms{0.01F};
    float stretchShearCompliance{1.0e-7F};
    float bendTwistCompliance{1.0e-5F};
    bool pinRoot{true};
};

struct JoltVehicleWheelDesc {
    Float3 position{};
    Float3 suspensionDirection{0.0F, -1.0F, 0.0F};
    Float3 steeringAxis{0.0F, 1.0F, 0.0F};
    Float3 wheelUp{0.0F, 1.0F, 0.0F};
    Float3 wheelForward{0.0F, 0.0F, 1.0F};
    float suspensionMinimumLength{0.20F};
    float suspensionMaximumLength{0.45F};
    float suspensionFrequencyHertz{2.0F};
    float suspensionDampingRatio{0.5F};
    float radiusMeters{0.35F};
    float widthMeters{0.20F};
    float maximumSteerAngleRadians{0.55F};
    float maximumBrakeTorque{1500.0F};
    float maximumHandBrakeTorque{};
};

struct JoltVehicleDifferentialDesc {
    std::int32_t leftWheel{-1};
    std::int32_t rightWheel{-1};
    float engineTorqueRatio{1.0F};
    float limitedSlipRatio{1.4F};
};

struct JoltWheeledVehicleDesc {
    RigidBodyCreateDesc chassis{};
    std::vector<JoltVehicleWheelDesc> wheels{};
    std::vector<JoltVehicleDifferentialDesc> differentials{};
    Float3 up{0.0F, 1.0F, 0.0F};
    Float3 forward{0.0F, 0.0F, 1.0F};
    float maximumPitchRollAngleRadians{1.047198F};
    float maximumEngineTorque{500.0F};
    float minimumEngineRpm{1000.0F};
    float maximumEngineRpm{6000.0F};
    float clutchStrength{10.0F};
};

struct JoltVehicleInput {
    float forward{};
    float steering{};
    float brake{};
    float handBrake{};
};

struct JoltVehicleWheelState {
    bool hasContact{};
    RigidBodyHandle contactBody{kInvalidRigidBodyHandle};
    Float3 contactPoint{};
    Float3 contactNormal{0.0F, 1.0F, 0.0F};
    float suspensionLength{};
    float angularVelocity{};
    float rotationAngle{};
    float steerAngle{};
};

struct JoltVehicleState {
    RigidBodyHandle chassis{kInvalidRigidBodyHandle};
    std::vector<JoltVehicleWheelState> wheels{};
};

struct JoltDebugLine { Float3 from{}; Float3 to{}; std::uint32_t color{}; };
struct JoltDebugTriangle { Float3 a{}; Float3 b{}; Float3 c{}; std::uint32_t color{}; };
struct JoltDebugText { Float3 position{}; std::string text{}; std::uint32_t color{}; float height{0.25F}; };
struct JoltDebugDrawSettings {
    bool shapes{true};
    bool wireframe{};
    bool bounds{};
    bool centerOfMass{};
    bool velocity{};
    bool constraints{true};
    bool constraintLimits{true};
    bool softBodyVertices{};
    bool softBodyConstraints{};
    bool softBodyRods{true};
};
struct JoltDebugFrame {
    bool supported{};
    std::vector<JoltDebugLine> lines{};
    std::vector<JoltDebugTriangle> triangles{};
    std::vector<JoltDebugText> text{};
};

struct JoltVirtualCharacterState {
    RigidTransform transform{};
    Float3 linearVelocity{};
    Float3 groundVelocity{};
    Float3 groundNormal{0.0F, 1.0F, 0.0F};
    RigidBodyHandle groundBody{kInvalidRigidBodyHandle};
    JoltCharacterGroundState groundState{JoltCharacterGroundState::InAir};
};

struct JoltWorldSnapshot {
    std::vector<std::byte> solverBytes{};
    std::vector<RigidBodyState> bodyStates{};
    std::vector<std::uint8_t> liveBodyMask{};
    std::vector<JoltVirtualCharacterState> characterStates{};
    std::vector<std::byte> characterSolverBytes{};
    std::vector<std::uint8_t> liveCharacterMask{};
    std::vector<std::uint8_t> liveSoftBodyMask{};
    std::vector<std::uint8_t> liveVehicleMask{};
    double contactTimeSeconds{};
};

struct JoltPhysicsTelemetry {
    std::uint32_t configuredWorkerThreads{};
    std::uint32_t configuredCollisionSteps{};
    std::uint32_t configuredVelocityIterations{};
    std::uint32_t configuredPositionIterations{};
    bool deterministicSimulation{};
    bool sleepingAllowed{};
    std::uint32_t lastUpdateErrorBits{};
    std::uint64_t updateCalls{};
    std::uint64_t invalidStepInputs{};
    std::uint64_t failedUpdateCalls{};
    std::uint64_t manifoldCacheFullEvents{};
    std::uint64_t bodyPairCacheFullEvents{};
    std::uint64_t contactConstraintsFullEvents{};
    std::uint64_t contactCallbacks{};
    std::uint64_t contactEventsQueued{};
    std::uint64_t contactEventsRejected{};
    std::uint64_t rayQueries{};
    std::uint64_t overlapQueries{};
    std::uint64_t shapeCasts{};
    std::uint64_t snapshotsSaved{};
    std::uint64_t snapshotsRestored{};
    std::uint64_t characterUpdates{};
    std::uint64_t heightFieldUpdates{};
    std::uint64_t mutableCompoundEdits{};
    std::uint64_t softBodyUpdates{};
    std::uint64_t vehicleUpdates{};
    std::uint64_t debugFramesCaptured{};
    std::uint32_t versionMajor{};
    std::uint32_t versionMinor{};
    std::uint32_t versionPatch{};
    std::size_t virtualCharacterCount{};
    std::size_t softBodyCount{};
    std::size_t vehicleCount{};
    RigidBodyCounts bodyCounts{};
};

// Production rigid-body backend built on Jolt Physics 5.5.x-5.6.x.
//
// The authoritative voxel/fragment pipeline remains solver-neutral. Every dynamic Jolt shape
// is recentered so its solver center of mass exactly matches RigidBodyCreateDesc::transform,
// including heterogeneous-density fragments whose physical center differs from the geometric
// center of their box compound.
class JoltRigidBodyWorld final : public IRigidBodyWorld {
public:
    JoltRigidBodyWorld();
    explicit JoltRigidBodyWorld(const JoltWorldConfig& config);
    ~JoltRigidBodyWorld() override;

    JoltRigidBodyWorld(const JoltRigidBodyWorld&) = delete;
    JoltRigidBodyWorld& operator=(const JoltRigidBodyWorld&) = delete;
    JoltRigidBodyWorld(JoltRigidBodyWorld&&) = delete;
    JoltRigidBodyWorld& operator=(JoltRigidBodyWorld&&) = delete;

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

    // Explicit teleport operation: unlike set_state(), this intentionally collapses the
    // interpolation history to the target transform and wakes the body.
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

    // The sink must remain alive while registered. Jolt may invoke it concurrently from worker
    // threads during step(); the supplied implementation must therefore be callback-safe.
    void set_contact_sink(IPhysicsContactSink* sink) noexcept override;
    bool set_contact_material(
        RigidBodyHandle handle, std::uint16_t material) noexcept override;

    // Production static geometry and scene queries.
    [[nodiscard]] RigidBodyHandle create_static_box(const RigidTransform& transform, Float3 halfExtents);
    [[nodiscard]] RigidBodyHandle create_static_triangle_mesh(
        const RigidTransform& transform,
        std::span<const Float3> vertices,
        std::span<const std::uint32_t> triangleIndices,
        std::uint16_t material = 0U,
        bool favorBuildSpeed = false);
    [[nodiscard]] RigidBodyHandle create_static_height_field(
        const JoltHeightFieldDesc& desc, std::span<const float> samples);
    bool update_height_field_region(
        RigidBodyHandle handle, std::uint32_t x, std::uint32_t y,
        std::uint32_t width, std::uint32_t height, std::span<const float> samples);

    [[nodiscard]] RigidBodyHandle create_mutable_compound(const JoltMutableCompoundDesc& desc);
    [[nodiscard]] JoltMutableCompoundChildHandle add_mutable_box(
        RigidBodyHandle body, const JoltMutableBoxDesc& desc);
    bool modify_mutable_box(
        RigidBodyHandle body, JoltMutableCompoundChildHandle child, const JoltMutableBoxDesc& desc);
    bool remove_mutable_box(RigidBodyHandle body, JoltMutableCompoundChildHandle child);

    [[nodiscard]] JoltSoftBodyHandle create_soft_body(
        const SoftBodyAsset& asset, const JoltSoftBodyDesc& desc = {});
    [[nodiscard]] JoltSoftBodyHandle create_hair_rods(
        std::span<const JoltHairStrandDesc> strands, const JoltSoftBodyDesc& desc = {});
    bool destroy_soft_body(JoltSoftBodyHandle handle);
    [[nodiscard]] std::optional<JoltSoftBodyState> soft_body_state(JoltSoftBodyHandle handle) const;
    bool set_soft_body_vertex_velocity(
        JoltSoftBodyHandle handle, std::uint32_t vertex, Float3 velocity);
    bool set_soft_body_vertex_inverse_mass(
        JoltSoftBodyHandle handle, std::uint32_t vertex, float inverseMass);
    bool apply_soft_body_impulse(JoltSoftBodyHandle handle, Float3 impulse);

    [[nodiscard]] JoltVehicleHandle create_wheeled_vehicle(const JoltWheeledVehicleDesc& desc);
    bool destroy_vehicle(JoltVehicleHandle handle);
    bool set_vehicle_input(JoltVehicleHandle handle, const JoltVehicleInput& input);
    [[nodiscard]] std::optional<JoltVehicleState> vehicle_state(JoltVehicleHandle handle) const;

    [[nodiscard]] JoltDebugFrame capture_debug_frame(const JoltDebugDrawSettings& settings = {}) const;

    void optimize_broad_phase();

    [[nodiscard]] std::optional<JoltRayHit> ray_cast_closest(
        Float3 origin,
        Float3 translation,
        bool collideWithBackFaces = true) const;
    [[nodiscard]] std::size_t query_aabb(
        const RigidBodyWorldBounds& bounds,
        std::span<JoltOverlapHit> output) const;
    [[nodiscard]] std::optional<JoltShapeCastHit> cast_sphere_closest(
        Float3 center,
        float radiusMeters,
        Float3 translation) const;

    // Full Jolt solver snapshots for replay/rollback. The topology (bodies and constraints)
    // must be unchanged between save and restore; the snapshot restores contacts, sleep state,
    // constraints, velocities, and DVE interpolation history.
    [[nodiscard]] std::optional<JoltWorldSnapshot> save_snapshot() const;
    bool restore_snapshot(const JoltWorldSnapshot& snapshot);

    // Jolt CharacterVirtual bridge. The character is updated explicitly and may optionally
    // create an inner rigid body so ordinary world ray casts and CCD bodies can see it.
    [[nodiscard]] JoltVirtualCharacterHandle create_virtual_character(
        const JoltVirtualCharacterDesc& desc);
    bool destroy_virtual_character(JoltVirtualCharacterHandle handle);
    bool update_virtual_character(
        JoltVirtualCharacterHandle handle,
        const JoltVirtualCharacterInput& input,
        float fixedDeltaSeconds);
    [[nodiscard]] std::optional<JoltVirtualCharacterState> virtual_character_state(
        JoltVirtualCharacterHandle handle) const;
    bool set_virtual_character_state(
        JoltVirtualCharacterHandle handle,
        const JoltVirtualCharacterState& state);

    [[nodiscard]] std::optional<bool> is_active(RigidBodyHandle handle) const;
    [[nodiscard]] RigidTransform interpolated_transform(RigidBodyHandle handle, float alpha) const;
    [[nodiscard]] RigidBodyCounts body_counts() const;
    [[nodiscard]] std::size_t body_count() const;
    [[nodiscard]] std::size_t dynamic_body_count() const;
    [[nodiscard]] std::size_t awake_body_count() const;
    [[nodiscard]] std::size_t sleeping_body_count() const;
    [[nodiscard]] std::size_t constraint_count() const noexcept;
    [[nodiscard]] std::size_t active_body_count() const { return awake_body_count(); }

    [[nodiscard]] JoltPhysicsTelemetry telemetry() const;
    [[nodiscard]] bool last_update_succeeded() const noexcept;

    // Validation-only inspection. A correctly recentered dynamic fragment reports zero.
    [[nodiscard]] std::optional<Float3> solver_shape_center_of_mass_local(RigidBodyHandle handle) const;
    [[nodiscard]] std::optional<SolverWorldBounds> solver_world_bounds(RigidBodyHandle handle) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dve
