#include "dve/physics_jolt_backend.hpp"

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/IssueReporting.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Geometry/AABox.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/MassProperties.h>
#include <Jolt/Physics/Body/MotionProperties.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/EstimateCollisionResponse.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MutableCompoundShape.h>
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>
#include <Jolt/Physics/SoftBody/SoftBodySharedSettings.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>
#include <Jolt/Physics/Constraints/Constraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/EPhysicsUpdateError.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/StateRecorder.h>
#include <Jolt/RegisterTypes.h>
#ifdef JPH_DEBUG_RENDERER
#include <Jolt/Renderer/DebugRendererSimple.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <string_view>
#include <utility>
#include <vector>

static_assert(JPH_VERSION_MAJOR == 5 && (JPH_VERSION_MINOR == 5 || JPH_VERSION_MINOR == 6),
    "The DVE Jolt backend supports Jolt Physics 5.5.x and 5.6.x");

namespace dve {
namespace {

void jolt_trace_callback(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
}

#ifdef JPH_ENABLE_ASSERTS
bool jolt_assert_failed_callback(const char* expression, const char* message, const char* file, JPH::uint line) {
    std::fprintf(
        stderr, "%s:%u: Jolt assert failed (%s) %s\n", file, line, expression,
        message != nullptr ? message : "");
    return true;
}
#endif

void ensure_jolt_types_registered() {
    static std::once_flag once;
    std::call_once(once, [] {
        JPH::Trace = jolt_trace_callback;
        JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = jolt_assert_failed_callback;)
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    });
}

constexpr JPH::uint64 kCharacterUserDataBit = JPH::uint64{1} << 63U;
constexpr JPH::uint64 kSoftBodyUserDataBit = JPH::uint64{1} << 62U;

class MemoryStateRecorder final : public JPH::StateRecorder {
public:
    MemoryStateRecorder() = default;
    explicit MemoryStateRecorder(std::span<const std::byte> bytes) : bytes_(bytes.begin(), bytes.end()) {}

    void WriteBytes(const void* data, std::size_t count) override {
        if (failed_ || data == nullptr || count == 0U) return;
        const auto* first = static_cast<const std::byte*>(data);
        try {
            bytes_.insert(bytes_.end(), first, first + count);
        } catch (...) {
            failed_ = true;
        }
    }

    void ReadBytes(void* output, std::size_t count) override {
        if (failed_ || output == nullptr) { failed_ = true; return; }
        if (count > bytes_.size() - std::min(offset_, bytes_.size())) {
            failed_ = true;
            std::memset(output, 0, count);
            return;
        }
        std::memcpy(output, bytes_.data() + offset_, count);
        offset_ += count;
    }

    bool IsEOF() const override { return offset_ >= bytes_.size(); }
    bool IsFailed() const override { return failed_; }
    void rewind() noexcept { offset_ = 0U; failed_ = false; }
    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept { return bytes_; }

private:
    std::vector<std::byte> bytes_{};
    std::size_t offset_{};
    bool failed_{};
};

[[nodiscard]] bool finite_float3(Float3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] bool query_allows(
    RigidBodyHandle handle,
    bool isStatic,
    const RigidBodyQueryFilter& filter) noexcept {
    if ((isStatic && !filter.includeStatic) || (!isStatic && !filter.includeDynamic)) return false;
    return std::find(filter.ignoredBodies.begin(), filter.ignoredBodies.end(), handle) ==
        filter.ignoredBodies.end();
}

void sort_query_hits(std::vector<RigidBodyQueryHit>& hits) {
    std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) {
        return std::tie(a.fraction, a.distance, a.body, a.subShape) <
            std::tie(b.fraction, b.distance, b.body, b.subShape);
    });
}

namespace Layers {
constexpr JPH::ObjectLayer kNonMoving = 0;
constexpr JPH::ObjectLayer kMoving = 1;
constexpr JPH::ObjectLayer kDebrisNoSelf = 2;
constexpr JPH::uint kCount = 3;
} // namespace Layers

namespace BroadPhaseLayers {
constexpr JPH::BroadPhaseLayer kNonMoving(0);
constexpr JPH::BroadPhaseLayer kMoving(1);
constexpr JPH::uint kCount = 2;
} // namespace BroadPhaseLayers

class ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        if (a == Layers::kNonMoving) return b != Layers::kNonMoving;
        if (b == Layers::kNonMoving) return a != Layers::kNonMoving;
        if (a == Layers::kDebrisNoSelf && b == Layers::kDebrisNoSelf) return false;
        return true;
    }
};

class BroadPhaseLayerInterface final : public JPH::BroadPhaseLayerInterface {
public:
    BroadPhaseLayerInterface() {
        map_[Layers::kNonMoving] = BroadPhaseLayers::kNonMoving;
        map_[Layers::kMoving] = BroadPhaseLayers::kMoving;
        map_[Layers::kDebrisNoSelf] = BroadPhaseLayers::kMoving;
    }
    JPH::uint GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::kCount; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override { return map_[layer]; }

private:
    JPH::BroadPhaseLayer map_[Layers::kCount];
};

class ObjectVsBroadPhaseLayerFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broadPhase) const override {
        if (layer == Layers::kNonMoving) return broadPhase == BroadPhaseLayers::kMoving;
        return true;
    }
};

[[nodiscard]] JPH::RVec3 to_jolt_position(Float3 value) noexcept {
    return JPH::RVec3(value.x, value.y, value.z);
}

[[nodiscard]] JPH::Vec3 to_jolt_vec3(Float3 value) noexcept {
    return JPH::Vec3(value.x, value.y, value.z);
}

[[nodiscard]] JPH::Quat to_jolt_quat(Quaternion value) noexcept {
    return JPH::Quat(value.x, value.y, value.z, value.w);
}

[[nodiscard]] Float3 from_jolt(JPH::Vec3Arg value) noexcept {
    return {value.GetX(), value.GetY(), value.GetZ()};
}

[[nodiscard]] Quaternion from_jolt(JPH::QuatArg value) noexcept {
    return {value.GetX(), value.GetY(), value.GetZ(), value.GetW()};
}

[[nodiscard]] JPH::Mat44 to_jolt_inertia(const SolverInertiaTensor& inertia) noexcept {
    const auto xx = static_cast<float>(inertia.xx);
    const auto yy = static_cast<float>(inertia.yy);
    const auto zz = static_cast<float>(inertia.zz);
    const auto xy = static_cast<float>(inertia.xy);
    const auto xz = static_cast<float>(inertia.xz);
    const auto yz = static_cast<float>(inertia.yz);
    return JPH::Mat44(
        JPH::Vec4(xx, xy, xz, 0.0F),
        JPH::Vec4(xy, yy, yz, 0.0F),
        JPH::Vec4(xz, yz, zz, 0.0F),
        JPH::Vec4(0.0F, 0.0F, 0.0F, 1.0F));
}

[[nodiscard]] std::uint32_t automatic_worker_count() noexcept {
    const unsigned hardware = std::max(1U, std::thread::hardware_concurrency());
    if (hardware <= 2U) return 1U;
    return std::min(4U, hardware - 2U);
}

[[nodiscard]] JoltWorldConfig sanitize_config(JoltWorldConfig config) noexcept {
    if (config.workerThreads == 0U) config.workerThreads = automatic_worker_count();
    config.workerThreads = std::clamp(config.workerThreads, 1U, 31U);
    config.maxBodies = std::max(config.maxBodies, 1024U);
    config.maxBodyPairs = std::max(config.maxBodyPairs, config.maxBodies);
    config.maxContactConstraints = std::max(config.maxContactConstraints, 1024U);
    config.temporaryMemoryBytes = std::max<std::size_t>(config.temporaryMemoryBytes, 4U * 1024U * 1024U);
    config.temporaryMemoryBytes = std::min<std::size_t>(
        config.temporaryMemoryBytes,
        static_cast<std::size_t>(std::numeric_limits<JPH::uint>::max()));
    if (!std::isfinite(config.boxConvexRadiusMeters) || config.boxConvexRadiusMeters < 0.0F) {
        config.boxConvexRadiusMeters = 0.005F;
    }
    if (!std::isfinite(config.boxConvexRadiusMaxFraction) ||
        config.boxConvexRadiusMaxFraction < 0.0F || config.boxConvexRadiusMaxFraction > 1.0F) {
        config.boxConvexRadiusMaxFraction = 0.10F;
    }
    config.collisionSteps = std::clamp(config.collisionSteps, 1U, 8U);
    config.velocityIterations = std::clamp(config.velocityIterations, 2U, 64U);
    config.positionIterations = std::clamp(config.positionIterations, 1U, 32U);
    if (!std::isfinite(config.speculativeContactDistanceMeters) ||
        config.speculativeContactDistanceMeters < 0.0F) config.speculativeContactDistanceMeters = 0.02F;
    if (!std::isfinite(config.penetrationSlopMeters) || config.penetrationSlopMeters < 0.0F)
        config.penetrationSlopMeters = 0.02F;
    if (!std::isfinite(config.timeBeforeSleepSeconds) || config.timeBeforeSleepSeconds < 0.0F)
        config.timeBeforeSleepSeconds = 0.5F;
    if (!std::isfinite(config.pointVelocitySleepThreshold) || config.pointVelocitySleepThreshold <= 0.0F)
        config.pointVelocitySleepThreshold = 0.03F;
    return config;
}

[[nodiscard]] float effective_convex_radius(Float3 halfExtents, const JoltWorldConfig& config) noexcept {
    const float minimumHalfExtent = std::min({halfExtents.x, halfExtents.y, halfExtents.z});
    return std::min(config.boxConvexRadiusMeters, minimumHalfExtent * config.boxConvexRadiusMaxFraction);
}


} // namespace

struct JoltRigidBodyWorld::Impl final : public JPH::BodyActivationListener, public JPH::ContactListener {
    explicit Impl(JoltWorldConfig inputConfig)
        : config(sanitize_config(inputConfig)),
          tempAllocator(static_cast<JPH::uint>(config.temporaryMemoryBytes)),
          jobSystem(
              JPH::cMaxPhysicsJobs,
              JPH::cMaxPhysicsBarriers,
              static_cast<int>(config.workerThreads)) {
        ensure_jolt_types_registered();
        system.Init(
            config.maxBodies,
            0,
            config.maxBodyPairs,
            config.maxContactConstraints,
            broadPhaseLayerInterface,
            objectVsBroadPhaseFilter,
            objectLayerPairFilter);
        JPH::PhysicsSettings physicsSettings = system.GetPhysicsSettings();
        physicsSettings.mNumVelocitySteps = config.velocityIterations;
        physicsSettings.mNumPositionSteps = config.positionIterations;
        physicsSettings.mSpeculativeContactDistance = config.speculativeContactDistanceMeters;
        physicsSettings.mPenetrationSlop = config.penetrationSlopMeters;
        physicsSettings.mTimeBeforeSleep = config.timeBeforeSleepSeconds;
        physicsSettings.mPointVelocitySleepThreshold = config.pointVelocitySleepThreshold;
        physicsSettings.mDeterministicSimulation = config.deterministicSimulation;
        physicsSettings.mAllowSleeping = config.allowSleeping;
        system.SetPhysicsSettings(physicsSettings);
        system.SetBodyActivationListener(this);
        system.SetContactListener(this);
    }

    ~Impl() override;

    void OnBodyActivated(const JPH::BodyID& id, JPH::uint64 /*userData*/) override { (void)id; }
    void OnBodyDeactivated(const JPH::BodyID& id, JPH::uint64 /*userData*/) override { (void)id; }

    [[nodiscard]] static RigidBodyHandle handle_from_body(const JPH::Body& body) noexcept {
        const JPH::uint64 userData = body.GetUserData();
        if (userData == 0U || userData > static_cast<JPH::uint64>(kInvalidRigidBodyHandle))
            return kInvalidRigidBodyHandle;
        return static_cast<RigidBodyHandle>(userData - 1U);
    }

    void record_contact(const JPH::Body& body1, const JPH::Body& body2,
                        const JPH::ContactManifold& manifold,
                        const JPH::ContactSettings& settings, bool persistent) noexcept {
        contactCallbacks.fetch_add(1U, std::memory_order_relaxed);
        IPhysicsContactSink* sink = contactSink.load(std::memory_order_acquire);
        if (!sink || manifold.mRelativeContactPointsOn1.empty()) return;

        JPH::RVec3 position = JPH::RVec3::sZero();
        for (JPH::uint index = 0; index < manifold.mRelativeContactPointsOn1.size(); ++index)
            position += manifold.GetWorldSpaceContactPointOn1(index);
        position /= static_cast<float>(manifold.mRelativeContactPointsOn1.size());
        const JPH::Vec3 relativeVelocity = body2.GetPointVelocity(position) - body1.GetPointVelocity(position);

        JPH::CollisionEstimationResult estimation;
        JPH::EstimateCollisionResponse(body1, body2, manifold, estimation,
                                       settings.mCombinedFriction,
                                       settings.mCombinedRestitution, 1.0F, 1U);
        float normalImpulse = 0.0F;
#if JPH_VERSION_MINOR >= 6
        for (const float impulse : estimation.mContactImpulse)
            normalImpulse += std::max(0.0F, impulse);
#else
        for (const auto& impulse : estimation.mImpulses)
            normalImpulse += std::max(0.0F, impulse.mContactImpulse);
#endif
        const float normalSpeed = std::abs(relativeVelocity.Dot(manifold.mWorldSpaceNormal));
        const float effectiveMass = normalSpeed > 1.0e-5F ? normalImpulse / normalSpeed : 0.0F;

        const RigidBodyHandle handle1 = handle_from_body(body1);
        const RigidBodyHandle handle2 = handle_from_body(body2);
        PhysicsContactEvent event;
        event.bodyA = handle1;
        event.bodyB = handle2;
        event.materialA = handle1 < slots.size() ? slots[handle1].contactMaterial : 0U;
        event.materialB = handle2 < slots.size() ? slots[handle2].contactMaterial : 0U;
        event.position = from_jolt(position);
        event.relativeVelocity = from_jolt(relativeVelocity);
        event.contactNormal = from_jolt(manifold.mWorldSpaceNormal);
        event.normalImpulse = normalImpulse;
        event.effectiveMass = effectiveMass;
        event.persistent = persistent;
        event.timeSeconds = contactTimeSeconds.load(std::memory_order_relaxed);
        if (sink->record_contact(event))
            contactEventsQueued.fetch_add(1U, std::memory_order_relaxed);
        else
            contactEventsRejected.fetch_add(1U, std::memory_order_relaxed);
    }

    void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2,
                        const JPH::ContactManifold& manifold,
                        JPH::ContactSettings& settings) override {
        record_contact(body1, body2, manifold, settings, false);
    }
    void OnContactPersisted(const JPH::Body& body1, const JPH::Body& body2,
                            const JPH::ContactManifold& manifold,
                            JPH::ContactSettings& settings) override {
        record_contact(body1, body2, manifold, settings, true);
    }

    struct MutableChildRecord {
        bool alive{};
        JPH::uint shapeIndex{};
    };

    struct Slot {
        bool alive{};
        bool isStatic{};
        JPH::BodyID bodyId{};
        RigidTransform previousTransform{};
        RigidTransform currentTransform{};
        std::uint16_t contactMaterial{};
        JPH::Ref<JPH::HeightFieldShape> heightField{};
        JoltHeightFieldDesc heightFieldDesc{};
        std::vector<float> heightFieldSamples{};
        JPH::Ref<JPH::MutableCompoundShape> mutableCompound{};
        std::vector<MutableChildRecord> mutableChildren{};
        std::vector<JoltMutableCompoundChildHandle> freeMutableChildren{};
    };

    struct ConstraintSlot {
        bool alive{};
        RigidBodyHandle parentBody{kInvalidRigidBodyHandle};
        RigidBodyHandle childBody{kInvalidRigidBodyHandle};
        JPH::Ref<JPH::Constraint> constraint{};
    };

    struct CharacterSlot {
        bool alive{};
        JPH::Ref<JPH::CharacterVirtual> character{};
    };

    struct SoftBodySlot {
        bool alive{};
        JPH::BodyID bodyId{};
        JPH::Ref<JPH::SoftBodySharedSettings> sharedSettings{};
    };

    struct VehicleSlot {
        bool alive{};
        RigidBodyHandle chassis{kInvalidRigidBodyHandle};
        JPH::Ref<JPH::VehicleConstraint> constraint{};
        JPH::Ref<JPH::VehicleCollisionTester> collisionTester{};
    };

    JoltWorldConfig config{};
    BroadPhaseLayerInterface broadPhaseLayerInterface{};
    ObjectVsBroadPhaseLayerFilter objectVsBroadPhaseFilter{};
    ObjectLayerPairFilter objectLayerPairFilter{};
    JPH::TempAllocatorImpl tempAllocator;
    JPH::JobSystemThreadPool jobSystem;
    JPH::PhysicsSystem system{};

    std::vector<Slot> slots{};
    std::vector<RigidBodyHandle> freeHandles{};
    std::vector<ConstraintSlot> constraintSlots{};
    std::vector<RigidBodyConstraintHandle> freeConstraintHandles{};
    std::vector<CharacterSlot> characterSlots{};
    std::vector<JoltVirtualCharacterHandle> freeCharacterHandles{};
    std::vector<SoftBodySlot> softBodySlots{};
    std::vector<JoltSoftBodyHandle> freeSoftBodyHandles{};
    std::vector<VehicleSlot> vehicleSlots{};
    std::vector<JoltVehicleHandle> freeVehicleHandles{};
    JoltPhysicsTelemetry telemetry{};
    std::atomic<IPhysicsContactSink*> contactSink{};
    std::atomic<double> contactTimeSeconds{};
    std::atomic<std::uint64_t> contactCallbacks{};
    std::atomic<std::uint64_t> contactEventsQueued{};
    std::atomic<std::uint64_t> contactEventsRejected{};
    mutable std::atomic<std::uint64_t> rayQueries{};
    mutable std::atomic<std::uint64_t> overlapQueries{};
    mutable std::atomic<std::uint64_t> shapeCasts{};
    mutable std::atomic<std::uint64_t> snapshotsSaved{};
    std::atomic<std::uint64_t> snapshotsRestored{};
    std::atomic<std::uint64_t> characterUpdates{};
    std::atomic<std::uint64_t> heightFieldUpdates{};
    std::atomic<std::uint64_t> mutableCompoundEdits{};
    std::atomic<std::uint64_t> softBodyUpdates{};
    std::atomic<std::uint64_t> vehicleUpdates{};
    mutable std::atomic<std::uint64_t> debugFramesCaptured{};

    [[nodiscard]] RigidBodyHandle allocate_handle() {
        if (!freeHandles.empty()) {
            const RigidBodyHandle handle = freeHandles.back();
            freeHandles.pop_back();
            return handle;
        }
        if (slots.size() >= static_cast<std::size_t>(kInvalidRigidBodyHandle)) {
            return kInvalidRigidBodyHandle;
        }
        slots.emplace_back();
        return static_cast<RigidBodyHandle>(slots.size() - 1U);
    }

    [[nodiscard]] bool valid(RigidBodyHandle handle) const noexcept {
        return handle != kInvalidRigidBodyHandle && handle < slots.size() && slots[handle].alive;
    }

    [[nodiscard]] RigidBodyConstraintHandle allocate_constraint_handle() {
        if (!freeConstraintHandles.empty()) {
            const RigidBodyConstraintHandle handle = freeConstraintHandles.back();
            freeConstraintHandles.pop_back();
            return handle;
        }
        if (constraintSlots.size() >= static_cast<std::size_t>(kInvalidRigidBodyConstraintHandle))
            return kInvalidRigidBodyConstraintHandle;
        constraintSlots.emplace_back();
        return static_cast<RigidBodyConstraintHandle>(constraintSlots.size() - 1U);
    }

    [[nodiscard]] bool valid_constraint(RigidBodyConstraintHandle handle) const noexcept {
        return handle != kInvalidRigidBodyConstraintHandle && handle < constraintSlots.size() &&
            constraintSlots[handle].alive;
    }

    [[nodiscard]] JoltVirtualCharacterHandle allocate_character_handle() {
        if (!freeCharacterHandles.empty()) {
            const JoltVirtualCharacterHandle handle = freeCharacterHandles.back();
            freeCharacterHandles.pop_back();
            return handle;
        }
        if (characterSlots.size() >= static_cast<std::size_t>(kInvalidJoltVirtualCharacterHandle))
            return kInvalidJoltVirtualCharacterHandle;
        characterSlots.emplace_back();
        return static_cast<JoltVirtualCharacterHandle>(characterSlots.size() - 1U);
    }

    [[nodiscard]] bool valid_character(JoltVirtualCharacterHandle handle) const noexcept {
        return handle != kInvalidJoltVirtualCharacterHandle && handle < characterSlots.size() &&
            characterSlots[handle].alive && characterSlots[handle].character != nullptr;
    }

    [[nodiscard]] JoltSoftBodyHandle allocate_soft_body_handle() {
        if (!freeSoftBodyHandles.empty()) {
            const JoltSoftBodyHandle handle = freeSoftBodyHandles.back();
            freeSoftBodyHandles.pop_back();
            return handle;
        }
        if (softBodySlots.size() >= static_cast<std::size_t>(kInvalidJoltSoftBodyHandle))
            return kInvalidJoltSoftBodyHandle;
        softBodySlots.emplace_back();
        return static_cast<JoltSoftBodyHandle>(softBodySlots.size() - 1U);
    }

    [[nodiscard]] bool valid_soft_body(JoltSoftBodyHandle handle) const noexcept {
        return handle != kInvalidJoltSoftBodyHandle && handle < softBodySlots.size() &&
            softBodySlots[handle].alive && !softBodySlots[handle].bodyId.IsInvalid();
    }

    [[nodiscard]] JoltVehicleHandle allocate_vehicle_handle() {
        if (!freeVehicleHandles.empty()) {
            const JoltVehicleHandle handle = freeVehicleHandles.back();
            freeVehicleHandles.pop_back();
            return handle;
        }
        if (vehicleSlots.size() >= static_cast<std::size_t>(kInvalidJoltVehicleHandle))
            return kInvalidJoltVehicleHandle;
        vehicleSlots.emplace_back();
        return static_cast<JoltVehicleHandle>(vehicleSlots.size() - 1U);
    }

    [[nodiscard]] bool valid_vehicle(JoltVehicleHandle handle) const noexcept {
        return handle != kInvalidJoltVehicleHandle && handle < vehicleSlots.size() &&
            vehicleSlots[handle].alive && vehicleSlots[handle].constraint != nullptr;
    }

    [[nodiscard]] RigidBodyHandle handle_from_body_id(JPH::BodyID bodyId) const noexcept {
        if (bodyId.IsInvalid()) return kInvalidRigidBodyHandle;
        const JPH::uint64 userData = system.GetBodyInterfaceNoLock().GetUserData(bodyId);
        if ((userData & (kCharacterUserDataBit | kSoftBodyUserDataBit)) != 0U || userData == 0U ||
            userData > static_cast<JPH::uint64>(kInvalidRigidBodyHandle)) return kInvalidRigidBodyHandle;
        const RigidBodyHandle handle = static_cast<RigidBodyHandle>(userData - 1U);
        return valid(handle) ? handle : kInvalidRigidBodyHandle;
    }

    [[nodiscard]] std::uint16_t material_for_body(JPH::BodyID bodyId) const noexcept {
        const RigidBodyHandle handle = handle_from_body_id(bodyId);
        return handle != kInvalidRigidBodyHandle ? slots[handle].contactMaterial : 0U;
    }

    [[nodiscard]] JPH::RefConst<JPH::Shape> build_shape(const RigidBodyCreateDesc& desc) const {
        JPH::RefConst<JPH::Shape> shape;
        if (desc.boxes.size() == 1U && desc.boxes.front().center.x == 0.0F &&
            desc.boxes.front().center.y == 0.0F && desc.boxes.front().center.z == 0.0F) {
            const SolverBox& box = desc.boxes.front();
            shape = new JPH::BoxShape(
                to_jolt_vec3(box.halfExtents),
                effective_convex_radius(box.halfExtents, config));
        } else {
            JPH::StaticCompoundShapeSettings compound;
            for (const SolverBox& box : desc.boxes) {
                auto* boxShape = new JPH::BoxShape(
                    to_jolt_vec3(box.halfExtents),
                    effective_convex_radius(box.halfExtents, config));
                compound.AddShape(to_jolt_vec3(box.center), JPH::Quat::sIdentity(), boxShape);
            }
            JPH::Shape::ShapeResult result = compound.Create();
            if (result.HasError()) return {};
            shape = result.Get();
        }

        // The collision boxes are expressed relative to the voxel engine's authoritative
        // physical center of mass. Jolt's compound computes a geometric center of mass from
        // child volume/density; those differ for heterogeneous-material fragments. Shift only
        // the shape's reported COM so BodyCreationSettings::mPosition remains our COM while
        // the underlying collision geometry stays in the original coordinates.
        const JPH::Vec3 calculatedCenter = shape->GetCenterOfMass();
        if (calculatedCenter.LengthSq() > 1.0e-12F) {
            shape = new JPH::OffsetCenterOfMassShape(shape.GetPtr(), -calculatedCenter);
        }
        return shape;
    }

    [[nodiscard]] JPH::RefConst<JPH::Shape> build_static_shape(
        const StaticRigidBodyCreateDesc& desc) const {
        if (!validate_static_rigid_body_desc(desc)) return {};
        if (desc.boxes.size() == 1U && desc.boxes.front().center.x == 0.0F &&
            desc.boxes.front().center.y == 0.0F && desc.boxes.front().center.z == 0.0F) {
            const SolverBox& box = desc.boxes.front();
            return new JPH::BoxShape(
                to_jolt_vec3(box.halfExtents),
                effective_convex_radius(box.halfExtents, config));
        }
        JPH::StaticCompoundShapeSettings compound;
        for (const SolverBox& box : desc.boxes) {
            auto* boxShape = new JPH::BoxShape(
                to_jolt_vec3(box.halfExtents),
                effective_convex_radius(box.halfExtents, config));
            compound.AddShape(to_jolt_vec3(box.center), JPH::Quat::sIdentity(), boxShape);
        }
        JPH::Shape::ShapeResult result = compound.Create();
        if (result.HasError()) return {};
        return result.Get();
    }

    [[nodiscard]] JPH::Body* build_body(
        JPH::BodyInterface& bodyInterface,
        const RigidBodyCreateDesc& desc,
        const RigidBodyValidationResult& validation) const {
        if (!validation) return nullptr;
        const JPH::RefConst<JPH::Shape> shape = build_shape(desc);
        if (shape == nullptr) return nullptr;

        JPH::BodyCreationSettings settings(
            shape,
            to_jolt_position(validation.normalizedTransform.position),
            to_jolt_quat(validation.normalizedTransform.rotation),
            JPH::EMotionType::Dynamic,
            desc.collisionClass == RigidBodyCollisionClass::DebrisNoSelf
                ? Layers::kDebrisNoSelf
                : Layers::kMoving);
        settings.mLinearVelocity = to_jolt_vec3(desc.linearVelocity);
        settings.mAngularVelocity = to_jolt_vec3(desc.angularVelocity);
        settings.mAllowSleeping = desc.allowSleeping;
        settings.mApplyGyroscopicForce = true;
        settings.mMotionQuality = desc.useContinuousCollision
            ? JPH::EMotionQuality::LinearCast
            : JPH::EMotionQuality::Discrete;
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
        settings.mMassPropertiesOverride.mMass = validation.massKilogramsFloat;
        settings.mMassPropertiesOverride.mInertia = to_jolt_inertia(validation.inertiaAfterFloatConversion);
        return bodyInterface.CreateBody(settings);
    }

    [[nodiscard]] JPH::Body* build_static_body(
        JPH::BodyInterface& bodyInterface,
        const StaticRigidBodyCreateDesc& desc,
        const RigidTransform& normalizedTransform) const {
        const JPH::RefConst<JPH::Shape> shape = build_static_shape(desc);
        if (shape == nullptr) return nullptr;
        JPH::BodyCreationSettings settings(
            shape,
            to_jolt_position(normalizedTransform.position),
            to_jolt_quat(normalizedTransform.rotation),
            JPH::EMotionType::Static,
            Layers::kNonMoving);
        return bodyInterface.CreateBody(settings);
    }

    [[nodiscard]] JPH::Ref<JPH::Constraint> build_constraint(
        const RigidBodyConstraintDesc& desc) {
        if (!validate_rigid_body_constraint_desc(desc) || !valid(desc.parentBody) ||
            !valid(desc.childBody) || slots[desc.parentBody].isStatic ||
            slots[desc.childBody].isStatic) return {};

        JPH::Body* parent = system.GetBodyLockInterfaceNoLock().TryGetBody(
            slots[desc.parentBody].bodyId);
        JPH::Body* child = system.GetBodyLockInterfaceNoLock().TryGetBody(
            slots[desc.childBody].bodyId);
        if (parent == nullptr || child == nullptr) return {};

        const Float3 parentAxis = normalize(desc.parentAxisLocal);
        const Quaternion reference = normalize(desc.referenceRotation);
        const Float3 childAxis = rotate(conjugate(reference), parentAxis);
        const JPH::Vec3 axis1 = to_jolt_vec3(parentAxis);
        const JPH::Vec3 axis2 = to_jolt_vec3(childAxis);
        const JPH::Vec3 normal1 = axis1.GetNormalizedPerpendicular();
        const Float3 normal1Engine = from_jolt(normal1);
        const JPH::Vec3 normal2 = to_jolt_vec3(rotate(conjugate(reference), normal1Engine));

        switch (desc.kind) {
        case RigidBodyConstraintKind::Ball: {
            JPH::PointConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
            settings.mPoint1 = to_jolt_position(desc.parentAnchorLocal);
            settings.mPoint2 = to_jolt_position(desc.childAnchorLocal);
            return settings.Create(*parent, *child);
        }
        case RigidBodyConstraintKind::Hinge: {
            JPH::HingeConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
            settings.mPoint1 = to_jolt_position(desc.parentAnchorLocal);
            settings.mPoint2 = to_jolt_position(desc.childAnchorLocal);
            settings.mHingeAxis1 = axis1;
            settings.mHingeAxis2 = axis2;
            settings.mNormalAxis1 = normal1;
            settings.mNormalAxis2 = normal2;
            settings.mLimitsMin = desc.minimumRadians;
            settings.mLimitsMax = desc.maximumRadians;
            return settings.Create(*parent, *child);
        }
        case RigidBodyConstraintKind::ConeTwist: {
            JPH::SwingTwistConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
            settings.mPosition1 = to_jolt_position(desc.parentAnchorLocal);
            settings.mPosition2 = to_jolt_position(desc.childAnchorLocal);
            settings.mTwistAxis1 = axis1;
            settings.mTwistAxis2 = axis2;
            settings.mPlaneAxis1 = normal1;
            settings.mPlaneAxis2 = normal2;
            settings.mSwingType = JPH::ESwingType::Cone;
            settings.mNormalHalfConeAngle = desc.swingLimitRadians;
            settings.mPlaneHalfConeAngle = desc.swingLimitRadians;
            settings.mTwistMinAngle = desc.minimumRadians;
            settings.mTwistMaxAngle = desc.maximumRadians;
            return settings.Create(*parent, *child);
        }
        case RigidBodyConstraintKind::Fixed: {
            JPH::FixedConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
            settings.mAutoDetectPoint = false;
            settings.mPoint1 = to_jolt_position(desc.parentAnchorLocal);
            settings.mPoint2 = to_jolt_position(desc.childAnchorLocal);
            settings.mAxisX1 = axis1;
            settings.mAxisX2 = axis2;
            settings.mAxisY1 = normal1;
            settings.mAxisY2 = normal2;
            return settings.Create(*parent, *child);
        }
        }
        return {};
    }

    void record_update_error(JPH::EPhysicsUpdateError error) noexcept {
        const std::uint32_t bits = static_cast<std::uint32_t>(error);
        telemetry.lastUpdateErrorBits = bits;
        ++telemetry.updateCalls;
        if (bits == 0U) return;
        ++telemetry.failedUpdateCalls;
        if ((bits & static_cast<std::uint32_t>(JPH::EPhysicsUpdateError::ManifoldCacheFull)) != 0U) {
            ++telemetry.manifoldCacheFullEvents;
        }
        if ((bits & static_cast<std::uint32_t>(JPH::EPhysicsUpdateError::BodyPairCacheFull)) != 0U) {
            ++telemetry.bodyPairCacheFullEvents;
        }
        if ((bits & static_cast<std::uint32_t>(JPH::EPhysicsUpdateError::ContactConstraintsFull)) != 0U) {
            ++telemetry.contactConstraintsFullEvents;
        }
    }
};

JoltRigidBodyWorld::Impl::~Impl() {
    // Tear down objects explicitly before PhysicsSystem and its body manager are destroyed.
    // This is required for mutable/query-safe shapes whose internal synchronization must not
    // be released while the body manager itself holds its global destruction lock.
    for (VehicleSlot& slot : vehicleSlots) {
        if (!slot.alive || slot.constraint == nullptr) continue;
        system.RemoveStepListener(slot.constraint);
        system.RemoveConstraint(slot.constraint);
        slot = {};
    }
    for (ConstraintSlot& slot : constraintSlots) {
        if (!slot.alive || slot.constraint == nullptr) continue;
        system.RemoveConstraint(slot.constraint);
        slot = {};
    }
    for (CharacterSlot& slot : characterSlots) slot = {};

    JPH::BodyInterface& bodyInterface = system.GetBodyInterface();
    for (SoftBodySlot& slot : softBodySlots) {
        if (!slot.alive || slot.bodyId.IsInvalid()) continue;
        bodyInterface.RemoveBody(slot.bodyId);
        bodyInterface.DestroyBody(slot.bodyId);
        slot = {};
    }
    for (Slot& slot : slots) {
        if (!slot.alive || slot.bodyId.IsInvalid()) continue;
        bodyInterface.RemoveBody(slot.bodyId);
        bodyInterface.DestroyBody(slot.bodyId);
        slot = {};
    }
}

JoltRigidBodyWorld::JoltRigidBodyWorld() : JoltRigidBodyWorld(JoltWorldConfig{}) {}

JoltRigidBodyWorld::JoltRigidBodyWorld(const JoltWorldConfig& config) {
    ensure_jolt_types_registered();
    impl_ = std::make_unique<Impl>(config);
    impl_->telemetry.configuredWorkerThreads = impl_->config.workerThreads;
    impl_->telemetry.configuredCollisionSteps = impl_->config.collisionSteps;
    impl_->telemetry.configuredVelocityIterations = impl_->config.velocityIterations;
    impl_->telemetry.configuredPositionIterations = impl_->config.positionIterations;
    impl_->telemetry.deterministicSimulation = impl_->config.deterministicSimulation;
    impl_->telemetry.sleepingAllowed = impl_->config.allowSleeping;
    impl_->telemetry.versionMajor = JPH_VERSION_MAJOR;
    impl_->telemetry.versionMinor = JPH_VERSION_MINOR;
    impl_->telemetry.versionPatch = JPH_VERSION_PATCH;
    impl_->system.SetGravity(JPH::Vec3(0.0F, -9.81F, 0.0F));
    impl_->system.OptimizeBroadPhase();
}

JoltRigidBodyWorld::~JoltRigidBodyWorld() = default;

RigidBodyHandle JoltRigidBodyWorld::create_body(const RigidBodyCreateDesc& desc) {
    const RigidBodyValidationResult validation = validate_rigid_body_desc(desc);
    if (!validation) return kInvalidRigidBodyHandle;

    JPH::BodyInterface& bodyInterface = impl_->system.GetBodyInterface();
    JPH::Body* body = impl_->build_body(bodyInterface, desc, validation);
    if (body == nullptr) return kInvalidRigidBodyHandle;

    const RigidBodyHandle handle = impl_->allocate_handle();
    if (handle == kInvalidRigidBodyHandle) {
        bodyInterface.DestroyBody(body->GetID());
        return kInvalidRigidBodyHandle;
    }

    body->SetUserData(static_cast<JPH::uint64>(handle) + 1U);
    bodyInterface.AddBody(body->GetID(), JPH::EActivation::Activate);
    Impl::Slot& slot = impl_->slots[handle];
    slot.alive = true;
    slot.isStatic = false;
    slot.bodyId = body->GetID();
    slot.previousTransform = validation.normalizedTransform;
    slot.currentTransform = validation.normalizedTransform;
    return handle;
}

std::vector<RigidBodyHandle> JoltRigidBodyWorld::create_bodies(
    std::span<const RigidBodyCreateDesc> descs) {
    if (descs.empty()) return {};
    if (descs.size() == 1U) {
        const RigidBodyHandle handle = create_body(descs.front());
        return handle == kInvalidRigidBodyHandle ? std::vector<RigidBodyHandle>{}
                                                 : std::vector<RigidBodyHandle>{handle};
    }

    JPH::BodyInterface& bodyInterface = impl_->system.GetBodyInterface();
    std::vector<JPH::BodyID> ids;
    ids.reserve(descs.size());
    std::unordered_map<JPH::BodyID, std::size_t> idToDescIndex;
    idToDescIndex.reserve(descs.size());
    std::vector<RigidBodyValidationResult> validations(descs.size());

    for (std::size_t i = 0; i < descs.size(); ++i) {
        validations[i] = validate_rigid_body_desc(descs[i]);
        if (!validations[i]) {
            for (const JPH::BodyID id : ids) bodyInterface.DestroyBody(id);
            return {};
        }
        JPH::Body* body = impl_->build_body(bodyInterface, descs[i], validations[i]);
        if (body == nullptr) {
            for (const JPH::BodyID id : ids) bodyInterface.DestroyBody(id);
            return {};
        }
        idToDescIndex.emplace(body->GetID(), i);
        ids.push_back(body->GetID());
    }

    JPH::BodyInterface::AddState addState =
        bodyInterface.AddBodiesPrepare(ids.data(), static_cast<int>(ids.size()));
    bodyInterface.AddBodiesFinalize(
        ids.data(), static_cast<int>(ids.size()), addState, JPH::EActivation::Activate);

    std::vector<RigidBodyHandle> handles(descs.size(), kInvalidRigidBodyHandle);
    std::vector<RigidBodyHandle> allocated;
    allocated.reserve(descs.size());
    for (const JPH::BodyID& id : ids) {
        const std::size_t descIndex = idToDescIndex.at(id);
        const RigidBodyHandle handle = impl_->allocate_handle();
        if (handle == kInvalidRigidBodyHandle) {
            for (const JPH::BodyID rollbackId : ids) {
                bodyInterface.RemoveBody(rollbackId);
                bodyInterface.DestroyBody(rollbackId);
            }
            for (const RigidBodyHandle allocatedHandle : allocated) {
                impl_->slots[allocatedHandle] = Impl::Slot{};
                impl_->freeHandles.push_back(allocatedHandle);
            }
            return {};
        }
        if (JPH::Body* body = impl_->system.GetBodyLockInterfaceNoLock().TryGetBody(id))
            body->SetUserData(static_cast<JPH::uint64>(handle) + 1U);
        Impl::Slot& slot = impl_->slots[handle];
        slot.alive = true;
        slot.isStatic = false;
        slot.bodyId = id;
        slot.previousTransform = validations[descIndex].normalizedTransform;
        slot.currentTransform = validations[descIndex].normalizedTransform;
        handles[descIndex] = handle;
        allocated.push_back(handle);
    }
    return handles;
}

RigidBodyHandle JoltRigidBodyWorld::create_static_body(
    const StaticRigidBodyCreateDesc& desc) {
    if (!validate_static_rigid_body_desc(desc)) return kInvalidRigidBodyHandle;
    const RigidTransform normalized = make_rigid_transform(desc.transform.position, desc.transform.rotation);
    JPH::BodyInterface& bodyInterface = impl_->system.GetBodyInterface();
    JPH::Body* body = impl_->build_static_body(bodyInterface, desc, normalized);
    if (body == nullptr) return kInvalidRigidBodyHandle;
    const RigidBodyHandle handle = impl_->allocate_handle();
    if (handle == kInvalidRigidBodyHandle) {
        bodyInterface.DestroyBody(body->GetID());
        return kInvalidRigidBodyHandle;
    }
    body->SetUserData(static_cast<JPH::uint64>(handle) + 1U);
    bodyInterface.AddBody(body->GetID(), JPH::EActivation::DontActivate);
    Impl::Slot& slot = impl_->slots[handle];
    slot.alive = true;
    slot.isStatic = true;
    slot.bodyId = body->GetID();
    slot.previousTransform = normalized;
    slot.currentTransform = normalized;
    return handle;
}

std::vector<RigidBodyHandle> JoltRigidBodyWorld::create_static_bodies(
    std::span<const StaticRigidBodyCreateDesc> descs) {
    if (descs.empty()) return {};
    if (descs.size() == 1U) {
        const RigidBodyHandle handle = create_static_body(descs.front());
        return handle == kInvalidRigidBodyHandle ? std::vector<RigidBodyHandle>{}
                                                 : std::vector<RigidBodyHandle>{handle};
    }

    JPH::BodyInterface& bodyInterface = impl_->system.GetBodyInterface();
    std::vector<JPH::BodyID> ids;
    ids.reserve(descs.size());
    std::unordered_map<JPH::BodyID, std::size_t> idToDescIndex;
    idToDescIndex.reserve(descs.size());
    std::vector<RigidTransform> transforms(descs.size());

    for (std::size_t i = 0; i < descs.size(); ++i) {
        if (!validate_static_rigid_body_desc(descs[i])) {
            for (const JPH::BodyID id : ids) bodyInterface.DestroyBody(id);
            return {};
        }
        transforms[i] = make_rigid_transform(descs[i].transform.position, descs[i].transform.rotation);
        JPH::Body* body = impl_->build_static_body(bodyInterface, descs[i], transforms[i]);
        if (body == nullptr) {
            for (const JPH::BodyID id : ids) bodyInterface.DestroyBody(id);
            return {};
        }
        idToDescIndex.emplace(body->GetID(), i);
        ids.push_back(body->GetID());
    }

    JPH::BodyInterface::AddState addState =
        bodyInterface.AddBodiesPrepare(ids.data(), static_cast<int>(ids.size()));
    bodyInterface.AddBodiesFinalize(
        ids.data(), static_cast<int>(ids.size()), addState, JPH::EActivation::DontActivate);

    std::vector<RigidBodyHandle> handles(descs.size(), kInvalidRigidBodyHandle);
    std::vector<RigidBodyHandle> allocated;
    allocated.reserve(descs.size());
    for (const JPH::BodyID& id : ids) {
        const std::size_t descIndex = idToDescIndex.at(id);
        const RigidBodyHandle handle = impl_->allocate_handle();
        if (handle == kInvalidRigidBodyHandle) {
            for (const JPH::BodyID rollbackId : ids) {
                bodyInterface.RemoveBody(rollbackId);
                bodyInterface.DestroyBody(rollbackId);
            }
            for (const RigidBodyHandle allocatedHandle : allocated) {
                impl_->slots[allocatedHandle] = Impl::Slot{};
                impl_->freeHandles.push_back(allocatedHandle);
            }
            return {};
        }
        if (JPH::Body* body = impl_->system.GetBodyLockInterfaceNoLock().TryGetBody(id))
            body->SetUserData(static_cast<JPH::uint64>(handle) + 1U);
        Impl::Slot& slot = impl_->slots[handle];
        slot.alive = true;
        slot.isStatic = true;
        slot.bodyId = id;
        slot.previousTransform = transforms[descIndex];
        slot.currentTransform = transforms[descIndex];
        handles[descIndex] = handle;
        allocated.push_back(handle);
    }
    return handles;
}

void JoltRigidBodyWorld::set_contact_sink(IPhysicsContactSink* sink) noexcept {
    impl_->contactSink.store(sink, std::memory_order_release);
}

bool JoltRigidBodyWorld::set_contact_material(
    RigidBodyHandle handle, std::uint16_t material) noexcept {
    if (!impl_->valid(handle)) return false;
    impl_->slots[handle].contactMaterial = material;
    return true;
}

RigidBodyHandle JoltRigidBodyWorld::create_static_box(
    const RigidTransform& transform,
    Float3 halfExtents) {
    StaticRigidBodyCreateDesc desc;
    desc.transform = transform;
    desc.boxes.push_back({{}, halfExtents});
    return create_static_body(desc);
}

RigidBodyHandle JoltRigidBodyWorld::create_static_triangle_mesh(
    const RigidTransform& transform,
    std::span<const Float3> vertices,
    std::span<const std::uint32_t> triangleIndices,
    std::uint16_t material,
    bool favorBuildSpeed) {
    if (vertices.size() < 3U || triangleIndices.size() < 3U ||
        triangleIndices.size() % 3U != 0U || !finite_float3(transform.position)) {
        return kInvalidRigidBodyHandle;
    }
    for (const Float3 vertex : vertices) {
        if (!finite_float3(vertex)) return kInvalidRigidBodyHandle;
    }

    JPH::VertexList meshVertices;
    meshVertices.reserve(vertices.size());
    for (const Float3 vertex : vertices) {
        meshVertices.emplace_back(vertex.x, vertex.y, vertex.z);
    }
    JPH::IndexedTriangleList meshTriangles;
    meshTriangles.reserve(triangleIndices.size() / 3U);
    for (std::size_t index = 0U; index < triangleIndices.size(); index += 3U) {
        const std::uint32_t a = triangleIndices[index];
        const std::uint32_t b = triangleIndices[index + 1U];
        const std::uint32_t c = triangleIndices[index + 2U];
        if (a >= vertices.size() || b >= vertices.size() || c >= vertices.size())
            return kInvalidRigidBodyHandle;
        meshTriangles.emplace_back(a, b, c);
    }

    JPH::MeshShapeSettings meshSettings(std::move(meshVertices), std::move(meshTriangles));
    meshSettings.mPerTriangleUserData = true;
    meshSettings.mBuildQuality = favorBuildSpeed
        ? JPH::MeshShapeSettings::EBuildQuality::FavorBuildSpeed
        : JPH::MeshShapeSettings::EBuildQuality::FavorRuntimePerformance;
    JPH::Shape::ShapeResult shapeResult = meshSettings.Create();
    if (shapeResult.HasError()) return kInvalidRigidBodyHandle;

    const RigidTransform normalized = make_rigid_transform(transform.position, transform.rotation);
    JPH::BodyCreationSettings bodySettings(
        shapeResult.Get(),
        to_jolt_position(normalized.position),
        to_jolt_quat(normalized.rotation),
        JPH::EMotionType::Static,
        Layers::kNonMoving);
    bodySettings.mEnhancedInternalEdgeRemoval = true;
    JPH::BodyInterface& bodyInterface = impl_->system.GetBodyInterface();
    JPH::Body* body = bodyInterface.CreateBody(bodySettings);
    if (body == nullptr) return kInvalidRigidBodyHandle;

    const RigidBodyHandle handle = impl_->allocate_handle();
    if (handle == kInvalidRigidBodyHandle) {
        bodyInterface.DestroyBody(body->GetID());
        return handle;
    }
    body->SetUserData(static_cast<JPH::uint64>(handle) + 1U);
    bodyInterface.AddBody(body->GetID(), JPH::EActivation::DontActivate);
    Impl::Slot& slot = impl_->slots[handle];
    slot.alive = true;
    slot.isStatic = true;
    slot.bodyId = body->GetID();
    slot.previousTransform = normalized;
    slot.currentTransform = normalized;
    slot.contactMaterial = material;
    return handle;
}

RigidBodyHandle JoltRigidBodyWorld::create_static_height_field(
    const JoltHeightFieldDesc& desc,
    std::span<const float> samples) {
    if (desc.sampleCount < 4U || samples.size() != static_cast<std::size_t>(desc.sampleCount) * desc.sampleCount ||
        !finite_float3(desc.transform.position) || !finite_float3(desc.sampleOffset) ||
        !finite_float3(desc.sampleScale) || desc.sampleScale.x == 0.0F ||
        desc.sampleScale.y == 0.0F || desc.sampleScale.z == 0.0F) return kInvalidRigidBodyHandle;
    for (float value : samples) {
        if (!std::isfinite(value) && value != JPH::HeightFieldShapeConstants::cNoCollisionValue)
            return kInvalidRigidBodyHandle;
    }
    JPH::HeightFieldShapeSettings settings(
        samples.data(), to_jolt_vec3(desc.sampleOffset), to_jolt_vec3(desc.sampleScale), desc.sampleCount);
    settings.mBlockSize = std::clamp(desc.blockSize, 2U, 8U);
    settings.mBitsPerSample = std::clamp(desc.bitsPerSample, 1U, 16U);
    JPH::Shape::ShapeResult shapeResult = settings.Create();
    if (shapeResult.HasError()) return kInvalidRigidBodyHandle;
    JPH::Ref<JPH::Shape> baseShape = shapeResult.Get();
    JPH::Ref<JPH::HeightFieldShape> shape = static_cast<JPH::HeightFieldShape*>(baseShape.GetPtr());
    const RigidTransform normalized = make_rigid_transform(desc.transform.position, desc.transform.rotation);
    JPH::BodyCreationSettings bodySettings(
        shape, to_jolt_position(normalized.position), to_jolt_quat(normalized.rotation),
        JPH::EMotionType::Static, Layers::kNonMoving);
    JPH::BodyInterface& bodyInterface = impl_->system.GetBodyInterface();
    JPH::Body* body = bodyInterface.CreateBody(bodySettings);
    if (body == nullptr) return kInvalidRigidBodyHandle;
    const RigidBodyHandle handle = impl_->allocate_handle();
    if (handle == kInvalidRigidBodyHandle) {
        bodyInterface.DestroyBody(body->GetID());
        return handle;
    }
    body->SetUserData(static_cast<JPH::uint64>(handle) + 1U);
    bodyInterface.AddBody(body->GetID(), JPH::EActivation::DontActivate);
    Impl::Slot& slot = impl_->slots[handle];
    slot.alive = true;
    slot.isStatic = true;
    slot.bodyId = body->GetID();
    slot.previousTransform = normalized;
    slot.currentTransform = normalized;
    slot.contactMaterial = desc.material;
    slot.heightField = std::move(shape);
    slot.heightFieldDesc = desc;
    slot.heightFieldSamples.assign(samples.begin(), samples.end());
    return handle;
}

bool JoltRigidBodyWorld::update_height_field_region(
    RigidBodyHandle handle,
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t width,
    std::uint32_t height,
    std::span<const float> samples) {
    if (!impl_->valid(handle) || width == 0U || height == 0U ||
        samples.size() != static_cast<std::size_t>(width) * height) return false;
    Impl::Slot& slot = impl_->slots[handle];
    if (slot.heightField == nullptr || slot.heightFieldSamples.empty()) return false;
    const std::uint32_t side = slot.heightFieldDesc.sampleCount;
    const std::uint64_t endX = static_cast<std::uint64_t>(x) + width;
    const std::uint64_t endY = static_cast<std::uint64_t>(y) + height;
    if (endX > side || endY > side) return false;
    for (float value : samples) {
        if (!std::isfinite(value) && value != JPH::HeightFieldShapeConstants::cNoCollisionValue) return false;
    }

    std::vector<float> rebuiltSamples = slot.heightFieldSamples;
    for (std::uint32_t row = 0U; row < height; ++row) {
        const std::size_t destination = static_cast<std::size_t>(y + row) * side + x;
        const std::size_t source = static_cast<std::size_t>(row) * width;
        std::copy_n(samples.begin() + static_cast<std::ptrdiff_t>(source), width,
                    rebuiltSamples.begin() + static_cast<std::ptrdiff_t>(destination));
    }

    const JoltHeightFieldDesc& desc = slot.heightFieldDesc;
    JPH::HeightFieldShapeSettings settings(
        rebuiltSamples.data(), to_jolt_vec3(desc.sampleOffset), to_jolt_vec3(desc.sampleScale), side);
    settings.mBlockSize = std::clamp(desc.blockSize, 2U, 8U);
    settings.mBitsPerSample = std::clamp(desc.bitsPerSample, 1U, 16U);
    JPH::Shape::ShapeResult result = settings.Create();
    if (result.HasError()) return false;
    JPH::Ref<JPH::Shape> baseShape = result.Get();
    JPH::Ref<JPH::HeightFieldShape> replacement =
        static_cast<JPH::HeightFieldShape*>(baseShape.GetPtr());
    if (replacement == nullptr) return false;

    impl_->system.GetBodyInterface().SetShape(
        slot.bodyId, replacement, false, JPH::EActivation::DontActivate);
    slot.heightField = std::move(replacement);
    slot.heightFieldSamples = std::move(rebuiltSamples);
    impl_->heightFieldUpdates.fetch_add(1U, std::memory_order_relaxed);
    return true;
}

RigidBodyHandle JoltRigidBodyWorld::create_mutable_compound(const JoltMutableCompoundDesc& desc) {
    if (desc.boxes.empty() || !finite_float3(desc.transform.position) ||
        (desc.dynamic && (!std::isfinite(desc.massKilograms) || desc.massKilograms <= 0.0))) return kInvalidRigidBodyHandle;
    JPH::MutableCompoundShapeSettings settings;
    for (const JoltMutableBoxDesc& box : desc.boxes) {
        if (!finite_float3(box.center) || !finite_float3(box.halfExtents) ||
            box.halfExtents.x <= 0.0F || box.halfExtents.y <= 0.0F || box.halfExtents.z <= 0.0F) return kInvalidRigidBodyHandle;
        const float radius = effective_convex_radius(box.halfExtents, impl_->config);
        settings.AddShape(to_jolt_vec3(box.center), to_jolt_quat(normalize(box.rotation)),
                          new JPH::BoxShape(to_jolt_vec3(box.halfExtents), radius), box.userData);
    }
    JPH::Shape::ShapeResult result = settings.Create();
    if (result.HasError()) return kInvalidRigidBodyHandle;
    JPH::Ref<JPH::Shape> baseShape = result.Get();
    JPH::Ref<JPH::MutableCompoundShape> shape = static_cast<JPH::MutableCompoundShape*>(baseShape.GetPtr());
    const RigidTransform normalized = make_rigid_transform(desc.transform.position, desc.transform.rotation);
    const JPH::ObjectLayer layer = desc.dynamic && desc.collisionClass == RigidBodyCollisionClass::DebrisNoSelf
        ? Layers::kDebrisNoSelf : (desc.dynamic ? Layers::kMoving : Layers::kNonMoving);
    JPH::BodyCreationSettings bodySettings(
        shape, to_jolt_position(normalized.position), to_jolt_quat(normalized.rotation),
        desc.dynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static, layer);
    bodySettings.mAllowSleeping = desc.allowSleeping;
    bodySettings.mLinearVelocity = to_jolt_vec3(desc.linearVelocity);
    bodySettings.mAngularVelocity = to_jolt_vec3(desc.angularVelocity);
    if (desc.dynamic) {
        bodySettings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        bodySettings.mMassPropertiesOverride.mMass = static_cast<float>(desc.massKilograms);
    }
    JPH::BodyInterface& bodyInterface = impl_->system.GetBodyInterface();
    JPH::Body* body = bodyInterface.CreateBody(bodySettings);
    if (body == nullptr) return kInvalidRigidBodyHandle;
    const RigidBodyHandle handle = impl_->allocate_handle();
    if (handle == kInvalidRigidBodyHandle) {
        bodyInterface.DestroyBody(body->GetID());
        return handle;
    }
    body->SetUserData(static_cast<JPH::uint64>(handle) + 1U);
    bodyInterface.AddBody(body->GetID(), desc.dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
    Impl::Slot& slot = impl_->slots[handle];
    slot.alive = true;
    slot.isStatic = !desc.dynamic;
    slot.bodyId = body->GetID();
    slot.previousTransform = normalized;
    slot.currentTransform = normalized;
    slot.mutableCompound = std::move(shape);
    slot.mutableChildren.resize(desc.boxes.size());
    for (std::size_t i = 0; i < slot.mutableChildren.size(); ++i) {
        slot.mutableChildren[i].alive = true;
        slot.mutableChildren[i].shapeIndex = static_cast<JPH::uint>(i);
    }
    return handle;
}

JoltMutableCompoundChildHandle JoltRigidBodyWorld::add_mutable_box(
    RigidBodyHandle body,
    const JoltMutableBoxDesc& desc) {
    if (!impl_->valid(body) || impl_->slots[body].mutableCompound == nullptr || !finite_float3(desc.center) ||
        !finite_float3(desc.halfExtents) || desc.halfExtents.x <= 0.0F || desc.halfExtents.y <= 0.0F || desc.halfExtents.z <= 0.0F)
        return kInvalidJoltMutableCompoundChildHandle;
    Impl::Slot& slot = impl_->slots[body];
    JPH::Ref<JPH::MutableCompoundShape> clone = slot.mutableCompound->Clone();
    const float radius = effective_convex_radius(desc.halfExtents, impl_->config);
    const JPH::uint shapeIndex = clone->AddShape(
        to_jolt_vec3(desc.center), to_jolt_quat(normalize(desc.rotation)),
        new JPH::BoxShape(to_jolt_vec3(desc.halfExtents), radius), desc.userData);
    JoltMutableCompoundChildHandle child;
    if (!slot.freeMutableChildren.empty()) {
        child = slot.freeMutableChildren.back();
        slot.freeMutableChildren.pop_back();
    } else {
        if (slot.mutableChildren.size() >= static_cast<std::size_t>(kInvalidJoltMutableCompoundChildHandle))
            return kInvalidJoltMutableCompoundChildHandle;
        child = static_cast<JoltMutableCompoundChildHandle>(slot.mutableChildren.size());
        slot.mutableChildren.emplace_back();
    }
    slot.mutableChildren[child] = {true, shapeIndex};
    impl_->system.GetBodyInterface().SetShape(slot.bodyId, clone, !slot.isStatic, JPH::EActivation::Activate);
    slot.mutableCompound = std::move(clone);
    impl_->mutableCompoundEdits.fetch_add(1U, std::memory_order_relaxed);
    return child;
}

bool JoltRigidBodyWorld::modify_mutable_box(
    RigidBodyHandle body,
    JoltMutableCompoundChildHandle child,
    const JoltMutableBoxDesc& desc) {
    if (!impl_->valid(body) || !finite_float3(desc.center) || !finite_float3(desc.halfExtents)) return false;
    Impl::Slot& slot = impl_->slots[body];
    if (slot.mutableCompound == nullptr || child >= slot.mutableChildren.size() || !slot.mutableChildren[child].alive ||
        desc.halfExtents.x <= 0.0F || desc.halfExtents.y <= 0.0F || desc.halfExtents.z <= 0.0F) return false;
    JPH::Ref<JPH::MutableCompoundShape> clone = slot.mutableCompound->Clone();
    const float radius = effective_convex_radius(desc.halfExtents, impl_->config);
    clone->ModifyShape(slot.mutableChildren[child].shapeIndex,
        to_jolt_vec3(desc.center), to_jolt_quat(normalize(desc.rotation)),
        new JPH::BoxShape(to_jolt_vec3(desc.halfExtents), radius));
    impl_->system.GetBodyInterface().SetShape(slot.bodyId, clone, !slot.isStatic, JPH::EActivation::Activate);
    slot.mutableCompound = std::move(clone);
    impl_->mutableCompoundEdits.fetch_add(1U, std::memory_order_relaxed);
    return true;
}

bool JoltRigidBodyWorld::remove_mutable_box(
    RigidBodyHandle body,
    JoltMutableCompoundChildHandle child) {
    if (!impl_->valid(body)) return false;
    Impl::Slot& slot = impl_->slots[body];
    if (slot.mutableCompound == nullptr || child >= slot.mutableChildren.size() || !slot.mutableChildren[child].alive)
        return false;
    const JPH::uint removedIndex = slot.mutableChildren[child].shapeIndex;
    JPH::Ref<JPH::MutableCompoundShape> clone = slot.mutableCompound->Clone();
    clone->RemoveShape(removedIndex);
    for (Impl::MutableChildRecord& record : slot.mutableChildren) {
        if (record.alive && record.shapeIndex > removedIndex) --record.shapeIndex;
    }
    slot.mutableChildren[child] = {};
    slot.freeMutableChildren.push_back(child);
    impl_->system.GetBodyInterface().SetShape(slot.bodyId, clone, !slot.isStatic, JPH::EActivation::Activate);
    slot.mutableCompound = std::move(clone);
    impl_->mutableCompoundEdits.fetch_add(1U, std::memory_order_relaxed);
    return true;
}

JoltSoftBodyHandle JoltRigidBodyWorld::create_soft_body(
    const SoftBodyAsset& asset,
    const JoltSoftBodyDesc& desc) {
    std::string error;
    if (!asset.validate(&error) || asset.vertices.empty() || !finite_float3(desc.transform.position))
        return kInvalidJoltSoftBodyHandle;
    const JoltSoftBodyRecipe recipe = make_jolt_soft_body_recipe(asset);
    JPH::Ref<JPH::SoftBodySharedSettings> shared = new JPH::SoftBodySharedSettings();
    shared->mVertices.reserve(recipe.vertices.size());
    for (const SoftBodyVertex& vertex : recipe.vertices)
        shared->mVertices.emplace_back(JPH::Float3(vertex.position.x, vertex.position.y, vertex.position.z),
                                       JPH::Float3(0.0F, 0.0F, 0.0F), std::max(0.0F, vertex.inverseMass));
    for (const SoftBodyFace& face : recipe.faces)
        shared->mFaces.emplace_back(face.vertices[0], face.vertices[1], face.vertices[2]);

    if (recipe.bendModel == SoftBodyBendModel::CosseratRod) {
        for (const SoftBodyDistanceConstraint& edge : asset.stretchConstraints)
            shared->mRodStretchShearConstraints.emplace_back(edge.vertexA, edge.vertexB, std::max(0.0F, edge.compliance));
        for (JPH::uint i = 1U; i < shared->mRodStretchShearConstraints.size(); ++i) {
            const auto& previous = shared->mRodStretchShearConstraints[i - 1U];
            const auto& current = shared->mRodStretchShearConstraints[i];
            if (previous.mVertex[1] == current.mVertex[0] || previous.mVertex[0] == current.mVertex[1] ||
                previous.mVertex[0] == current.mVertex[0] || previous.mVertex[1] == current.mVertex[1]) {
                float compliance = 0.0F;
                if (i - 1U < asset.bendConstraints.size()) compliance = std::max(0.0F, asset.bendConstraints[i - 1U].compliance);
                shared->mRodBendTwistConstraints.emplace_back(i - 1U, i, compliance);
            }
        }
        if (shared->mRodStretchShearConstraints.size() >= 2U) shared->CalculateRodProperties();
    } else {
        for (const SoftBodyDistanceConstraint& edge : recipe.edges) {
            JPH::SoftBodySharedSettings::Edge joltEdge(edge.vertexA, edge.vertexB, std::max(0.0F, edge.compliance));
            if (edge.restLength > 0.0F) joltEdge.mRestLength = edge.restLength;
            shared->mEdgeConstraints.push_back(joltEdge);
        }
    }
    for (const SoftBodyDihedralConstraint& bend : recipe.dihedrals) {
        JPH::SoftBodySharedSettings::DihedralBend joltBend(
            bend.vertices[0], bend.vertices[1], bend.vertices[2], bend.vertices[3], std::max(0.0F, bend.compliance));
        joltBend.mInitialAngle = bend.restAngle;
        shared->mDihedralBendConstraints.push_back(joltBend);
    }
    for (const SoftBodyVolumeConstraint& volume : recipe.volumes) {
        JPH::SoftBodySharedSettings::Volume joltVolume(
            volume.vertices[0], volume.vertices[1], volume.vertices[2], volume.vertices[3], std::max(0.0F, volume.compliance));
        joltVolume.mSixRestVolume = volume.sixRestVolume;
        shared->mVolumeConstraints.push_back(joltVolume);
    }
    shared->Optimize();

    const JoltSoftBodyHandle handle = impl_->allocate_soft_body_handle();
    if (handle == kInvalidJoltSoftBodyHandle) return handle;
    const RigidTransform normalized = make_rigid_transform(desc.transform.position, desc.transform.rotation);
    JPH::SoftBodyCreationSettings settings(
        shared, to_jolt_position(normalized.position), to_jolt_quat(normalized.rotation), Layers::kMoving);
    settings.mUserData = kSoftBodyUserDataBit | (static_cast<JPH::uint64>(handle) + 1U);
    settings.mNumIterations = std::clamp(desc.solverIterations, 1U, 64U);
    settings.mLinearDamping = std::max(0.0F, std::isfinite(desc.linearDamping) ? desc.linearDamping : recipe.linearDamping);
    settings.mRestitution = std::clamp(desc.restitution, 0.0F, 1.0F);
    settings.mFriction = std::max(0.0F, desc.friction);
    settings.mPressure = std::isfinite(desc.pressure) ? desc.pressure : recipe.pressure;
    settings.mGravityFactor = std::isfinite(desc.gravityFactor) ? desc.gravityFactor : 1.0F;
    settings.mVertexRadius = std::max(0.0F, desc.vertexRadiusMeters);
    settings.mMaxLinearVelocity = std::max(0.1F, desc.maximumLinearVelocity);
    settings.mUpdatePosition = desc.updatePosition;
    settings.mAllowSleeping = desc.allowSleeping;
    settings.mFacesDoubleSided = desc.facesDoubleSided || recipe.facesDoubleSided;
    JPH::BodyInterface& bodyInterface = impl_->system.GetBodyInterface();
    JPH::Body* body = bodyInterface.CreateSoftBody(settings);
    if (body == nullptr) {
        impl_->freeSoftBodyHandles.push_back(handle);
        return kInvalidJoltSoftBodyHandle;
    }
    bodyInterface.AddBody(body->GetID(), JPH::EActivation::Activate);
    impl_->softBodySlots[handle] = {true, body->GetID(), std::move(shared)};
    return handle;
}

JoltSoftBodyHandle JoltRigidBodyWorld::create_hair_rods(
    std::span<const JoltHairStrandDesc> strands,
    const JoltSoftBodyDesc& desc) {
    if (strands.empty()) return kInvalidJoltSoftBodyHandle;
    SoftBodyAsset asset;
    asset.name = "Jolt CPU Hair Rods";
    asset.kind = SoftBodyKind::Rope;
    asset.bendModel = SoftBodyBendModel::CosseratRod;
    for (const JoltHairStrandDesc& strand : strands) {
        if (strand.points.size() < 2U || !std::isfinite(strand.vertexMassKilograms) || strand.vertexMassKilograms <= 0.0F)
            return kInvalidJoltSoftBodyHandle;
        const std::uint32_t base = static_cast<std::uint32_t>(asset.vertices.size());
        for (std::size_t i = 0U; i < strand.points.size(); ++i) {
            if (!finite_float3(strand.points[i])) return kInvalidJoltSoftBodyHandle;
            asset.vertices.push_back({strand.points[i], (strand.pinRoot && i == 0U) ? 0.0F : 1.0F / strand.vertexMassKilograms, desc.vertexRadiusMeters});
        }
        for (std::size_t i = 1U; i < strand.points.size(); ++i) {
            asset.stretchConstraints.push_back({base + static_cast<std::uint32_t>(i - 1U), base + static_cast<std::uint32_t>(i),
                length(subtract(strand.points[i], strand.points[i - 1U])), std::max(0.0F, strand.stretchShearCompliance)});
            if (i > 1U) asset.bendConstraints.push_back({base + static_cast<std::uint32_t>(i - 2U), base + static_cast<std::uint32_t>(i),
                length(subtract(strand.points[i], strand.points[i - 2U])), std::max(0.0F, strand.bendTwistCompliance)});
        }
    }
    asset.doubleSided = true;
    asset.linearDamping = desc.linearDamping;
    asset.recompute_hash();
    return create_soft_body(asset, desc);
}

bool JoltRigidBodyWorld::destroy_soft_body(JoltSoftBodyHandle handle) {
    if (!impl_->valid_soft_body(handle)) return false;
    JPH::BodyInterface& bodyInterface = impl_->system.GetBodyInterface();
    const JPH::BodyID id = impl_->softBodySlots[handle].bodyId;
    bodyInterface.RemoveBody(id);
    bodyInterface.DestroyBody(id);
    impl_->softBodySlots[handle] = {};
    impl_->freeSoftBodyHandles.push_back(handle);
    return true;
}

std::optional<JoltSoftBodyState> JoltRigidBodyWorld::soft_body_state(JoltSoftBodyHandle handle) const {
    if (!impl_->valid_soft_body(handle)) return std::nullopt;
    const JPH::BodyID id = impl_->softBodySlots[handle].bodyId;
    JPH::BodyLockRead lock(impl_->system.GetBodyLockInterface(), id);
    if (!lock.Succeeded() || !lock.GetBody().IsSoftBody()) return std::nullopt;
    const JPH::Body& body = lock.GetBody();
    const auto* properties = static_cast<const JPH::SoftBodyMotionProperties*>(body.GetMotionProperties());
    JoltSoftBodyState result;
    result.transform = make_rigid_transform(from_jolt(body.GetPosition()), from_jolt(body.GetRotation()));
    result.sleeping = !body.IsActive();
    result.volume = properties->GetVolume();
    result.vertices.reserve(properties->GetVertices().size());
    const JPH::RMat44 transform = body.GetCenterOfMassTransform();
    for (const JPH::SoftBodyMotionProperties::Vertex& vertex : properties->GetVertices()) {
        const JPH::RVec3 worldPosition = transform * vertex.mPosition;
        const JPH::Vec3 worldVelocity = body.GetRotation() * vertex.mVelocity + body.GetLinearVelocity();
        result.vertices.push_back({from_jolt(worldPosition), from_jolt(worldVelocity), vertex.mInvMass});
    }
    return result;
}

bool JoltRigidBodyWorld::set_soft_body_vertex_velocity(
    JoltSoftBodyHandle handle,
    std::uint32_t vertex,
    Float3 velocity) {
    if (!impl_->valid_soft_body(handle) || !finite_float3(velocity)) return false;
    const JPH::BodyID id = impl_->softBodySlots[handle].bodyId;
    {
        JPH::BodyLockWrite lock(impl_->system.GetBodyLockInterface(), id);
        if (!lock.Succeeded() || !lock.GetBody().IsSoftBody()) return false;
        auto* properties = static_cast<JPH::SoftBodyMotionProperties*>(lock.GetBody().GetMotionProperties());
        if (vertex >= properties->GetVertices().size()) return false;
        properties->GetVertex(vertex).mVelocity = lock.GetBody().GetRotation().Conjugated() *
            (to_jolt_vec3(velocity) - lock.GetBody().GetLinearVelocity());
    }
    impl_->system.GetBodyInterface().ActivateBody(id);
    return true;
}

bool JoltRigidBodyWorld::set_soft_body_vertex_inverse_mass(
    JoltSoftBodyHandle handle,
    std::uint32_t vertex,
    float inverseMass) {
    if (!impl_->valid_soft_body(handle) || !std::isfinite(inverseMass) || inverseMass < 0.0F) return false;
    const JPH::BodyID id = impl_->softBodySlots[handle].bodyId;
    {
        JPH::BodyLockWrite lock(impl_->system.GetBodyLockInterface(), id);
        if (!lock.Succeeded() || !lock.GetBody().IsSoftBody()) return false;
        auto* properties = static_cast<JPH::SoftBodyMotionProperties*>(lock.GetBody().GetMotionProperties());
        if (vertex >= properties->GetVertices().size()) return false;
        properties->GetVertex(vertex).mInvMass = inverseMass;
        properties->CalculateMassAndInertia();
    }
    impl_->system.GetBodyInterface().ActivateBody(id);
    return true;
}

bool JoltRigidBodyWorld::apply_soft_body_impulse(JoltSoftBodyHandle handle, Float3 impulse) {
    if (!impl_->valid_soft_body(handle) || !finite_float3(impulse)) return false;
    const JPH::BodyID id = impl_->softBodySlots[handle].bodyId;
    {
        JPH::BodyLockWrite lock(impl_->system.GetBodyLockInterface(), id);
        if (!lock.Succeeded() || !lock.GetBody().IsSoftBody()) return false;
        auto* properties = static_cast<JPH::SoftBodyMotionProperties*>(lock.GetBody().GetMotionProperties());
        double totalMass = 0.0;
        for (const auto& vertex : properties->GetVertices())
            if (vertex.mInvMass > 0.0F) totalMass += 1.0 / vertex.mInvMass;
        if (!(totalMass > 0.0)) return false;
        const JPH::Vec3 localDelta = lock.GetBody().GetRotation().Conjugated() *
            (to_jolt_vec3(impulse) / static_cast<float>(totalMass));
        for (auto& vertex : properties->GetVertices())
            if (vertex.mInvMass > 0.0F) vertex.mVelocity += localDelta;
    }
    impl_->system.GetBodyInterface().ActivateBody(id);
    return true;
}

JoltVehicleHandle JoltRigidBodyWorld::create_wheeled_vehicle(const JoltWheeledVehicleDesc& desc) {
    if (desc.wheels.size() < 2U || !finite_float3(desc.up) || !finite_float3(desc.forward))
        return kInvalidJoltVehicleHandle;
    const RigidBodyHandle chassis = create_body(desc.chassis);
    if (chassis == kInvalidRigidBodyHandle) return kInvalidJoltVehicleHandle;
    JPH::Body* body = impl_->system.GetBodyLockInterfaceNoLock().TryGetBody(impl_->slots[chassis].bodyId);
    if (body == nullptr) {
        (void)destroy_body(chassis);
        return kInvalidJoltVehicleHandle;
    }
    JPH::VehicleConstraintSettings vehicleSettings;
    vehicleSettings.mUp = to_jolt_vec3(normalize(desc.up));
    vehicleSettings.mForward = to_jolt_vec3(normalize(desc.forward));
    vehicleSettings.mMaxPitchRollAngle = std::clamp(desc.maximumPitchRollAngleRadians, 0.0F, 3.14159265F);
    for (const JoltVehicleWheelDesc& wheelDesc : desc.wheels) {
        if (!finite_float3(wheelDesc.position) || wheelDesc.radiusMeters <= 0.0F || wheelDesc.widthMeters <= 0.0F) {
            (void)destroy_body(chassis);
            return kInvalidJoltVehicleHandle;
        }
        JPH::Ref<JPH::WheelSettingsWV> wheel = new JPH::WheelSettingsWV();
        wheel->mPosition = to_jolt_vec3(wheelDesc.position);
        wheel->mSuspensionDirection = to_jolt_vec3(normalize(wheelDesc.suspensionDirection));
        wheel->mSteeringAxis = to_jolt_vec3(normalize(wheelDesc.steeringAxis));
        wheel->mWheelUp = to_jolt_vec3(normalize(wheelDesc.wheelUp));
        wheel->mWheelForward = to_jolt_vec3(normalize(wheelDesc.wheelForward));
        wheel->mSuspensionMinLength = std::max(0.0F, wheelDesc.suspensionMinimumLength);
        wheel->mSuspensionMaxLength = std::max(wheel->mSuspensionMinLength, wheelDesc.suspensionMaximumLength);
        wheel->mSuspensionSpring.mFrequency = std::max(0.0F, wheelDesc.suspensionFrequencyHertz);
        wheel->mSuspensionSpring.mDamping = std::max(0.0F, wheelDesc.suspensionDampingRatio);
        wheel->mRadius = wheelDesc.radiusMeters;
        wheel->mWidth = wheelDesc.widthMeters;
        wheel->mMaxSteerAngle = wheelDesc.maximumSteerAngleRadians;
        wheel->mMaxBrakeTorque = std::max(0.0F, wheelDesc.maximumBrakeTorque);
        wheel->mMaxHandBrakeTorque = std::max(0.0F, wheelDesc.maximumHandBrakeTorque);
        vehicleSettings.mWheels.push_back(JPH::Ref<JPH::WheelSettings>(wheel.GetPtr()));
    }
    JPH::Ref<JPH::WheeledVehicleControllerSettings> controller = new JPH::WheeledVehicleControllerSettings();
    controller->mEngine.mMaxTorque = std::max(0.0F, desc.maximumEngineTorque);
    controller->mEngine.mMinRPM = std::max(1.0F, desc.minimumEngineRpm);
    controller->mEngine.mMaxRPM = std::max(controller->mEngine.mMinRPM, desc.maximumEngineRpm);
    controller->mTransmission.mClutchStrength = std::max(0.01F, desc.clutchStrength);
    if (!desc.differentials.empty()) {
        for (const JoltVehicleDifferentialDesc& differential : desc.differentials) {
            JPH::VehicleDifferentialSettings value;
            value.mLeftWheel = differential.leftWheel;
            value.mRightWheel = differential.rightWheel;
            value.mEngineTorqueRatio = differential.engineTorqueRatio;
            value.mLimitedSlipRatio = differential.limitedSlipRatio;
            controller->mDifferentials.push_back(value);
        }
    } else {
        JPH::VehicleDifferentialSettings value;
        value.mLeftWheel = 0;
        value.mRightWheel = 1;
        controller->mDifferentials.push_back(value);
    }
    vehicleSettings.mController = controller;
    JPH::Ref<JPH::VehicleConstraint> constraint = new JPH::VehicleConstraint(*body, vehicleSettings);
    JPH::Ref<JPH::VehicleCollisionTester> tester = new JPH::VehicleCollisionTesterRay(Layers::kMoving);
    constraint->SetVehicleCollisionTester(tester);
    const JoltVehicleHandle handle = impl_->allocate_vehicle_handle();
    if (handle == kInvalidJoltVehicleHandle) {
        (void)destroy_body(chassis);
        return handle;
    }
    impl_->system.AddConstraint(constraint);
    impl_->system.AddStepListener(constraint);
    impl_->vehicleSlots[handle] = {true, chassis, std::move(constraint), std::move(tester)};
    return handle;
}

bool JoltRigidBodyWorld::destroy_vehicle(JoltVehicleHandle handle) {
    if (!impl_->valid_vehicle(handle)) return false;
    Impl::VehicleSlot slot = impl_->vehicleSlots[handle];
    impl_->system.RemoveStepListener(slot.constraint);
    impl_->system.RemoveConstraint(slot.constraint);
    impl_->vehicleSlots[handle] = {};
    impl_->freeVehicleHandles.push_back(handle);
    if (impl_->valid(slot.chassis)) (void)destroy_body(slot.chassis);
    return true;
}

bool JoltRigidBodyWorld::set_vehicle_input(JoltVehicleHandle handle, const JoltVehicleInput& input) {
    if (!impl_->valid_vehicle(handle) || !std::isfinite(input.forward) || !std::isfinite(input.steering) ||
        !std::isfinite(input.brake) || !std::isfinite(input.handBrake)) return false;
    auto* controller = static_cast<JPH::WheeledVehicleController*>(impl_->vehicleSlots[handle].constraint->GetController());
    controller->SetDriverInput(
        std::clamp(input.forward, -1.0F, 1.0F), std::clamp(input.steering, -1.0F, 1.0F),
        std::clamp(input.brake, 0.0F, 1.0F), std::clamp(input.handBrake, 0.0F, 1.0F));
    impl_->system.GetBodyInterface().ActivateBody(impl_->slots[impl_->vehicleSlots[handle].chassis].bodyId);
    impl_->vehicleUpdates.fetch_add(1U, std::memory_order_relaxed);
    return true;
}

std::optional<JoltVehicleState> JoltRigidBodyWorld::vehicle_state(JoltVehicleHandle handle) const {
    if (!impl_->valid_vehicle(handle)) return std::nullopt;
    const Impl::VehicleSlot& slot = impl_->vehicleSlots[handle];
    JoltVehicleState result;
    result.chassis = slot.chassis;
    const JPH::Wheels& wheels = slot.constraint->GetWheels();
    result.wheels.reserve(wheels.size());
    for (const JPH::Wheel* wheel : wheels) {
        JoltVehicleWheelState stateValue;
        stateValue.hasContact = wheel->HasContact();
        stateValue.suspensionLength = wheel->GetSuspensionLength();
        stateValue.angularVelocity = wheel->GetAngularVelocity();
        stateValue.rotationAngle = wheel->GetRotationAngle();
        stateValue.steerAngle = wheel->GetSteerAngle();
        if (stateValue.hasContact) {
            stateValue.contactBody = impl_->handle_from_body_id(wheel->GetContactBodyID());
            stateValue.contactPoint = from_jolt(wheel->GetContactPosition());
            stateValue.contactNormal = from_jolt(wheel->GetContactNormal());
        }
        result.wheels.push_back(stateValue);
    }
    return result;
}

#ifdef JPH_DEBUG_RENDERER
namespace {
class DveJoltCaptureRenderer final : public JPH::DebugRendererSimple {
public:
    explicit DveJoltCaptureRenderer(JoltDebugFrame& frame) : frame_(frame) { Initialize(); SetCameraPos(JPH::RVec3::sZero()); }
    void DrawLine(JPH::RVec3Arg from, JPH::RVec3Arg to, JPH::ColorArg color) override {
        frame_.lines.push_back({from_jolt(from), from_jolt(to), color.GetUInt32()});
    }
    void DrawTriangle(JPH::RVec3Arg a, JPH::RVec3Arg b, JPH::RVec3Arg c, JPH::ColorArg color, ECastShadow) override {
        frame_.triangles.push_back({from_jolt(a), from_jolt(b), from_jolt(c), color.GetUInt32()});
    }
    void DrawText3D(JPH::RVec3Arg position, const JPH::string_view& text, JPH::ColorArg color, float height) override {
        frame_.text.push_back({from_jolt(position), std::string(text), color.GetUInt32(), height});
    }
private:
    JoltDebugFrame& frame_;
};
}
#endif

JoltDebugFrame JoltRigidBodyWorld::capture_debug_frame(const JoltDebugDrawSettings& settings) const {
    JoltDebugFrame frame;
#ifdef JPH_DEBUG_RENDERER
    frame.supported = true;
    DveJoltCaptureRenderer renderer(frame);
    JPH::BodyManager::DrawSettings draw;
    draw.mDrawShape = settings.shapes;
    draw.mDrawShapeWireframe = settings.wireframe;
    draw.mDrawBoundingBox = settings.bounds;
    draw.mDrawCenterOfMassTransform = settings.centerOfMass;
    draw.mDrawVelocity = settings.velocity;
    draw.mDrawSoftBodyVertices = settings.softBodyVertices;
    draw.mDrawSoftBodyEdgeConstraints = settings.softBodyConstraints;
    draw.mDrawSoftBodyBendConstraints = settings.softBodyConstraints;
    draw.mDrawSoftBodyVolumeConstraints = settings.softBodyConstraints;
    draw.mDrawSoftBodyRods = settings.softBodyRods;
    draw.mDrawSoftBodyRodStates = settings.softBodyRods;
    draw.mDrawSoftBodyRodBendTwistConstraints = settings.softBodyRods;
    impl_->system.DrawBodies(draw, &renderer);
    if (settings.constraints) impl_->system.DrawConstraints(&renderer);
    if (settings.constraintLimits) impl_->system.DrawConstraintLimits(&renderer);
#endif
    impl_->debugFramesCaptured.fetch_add(1U, std::memory_order_relaxed);
    return frame;
}


void JoltRigidBodyWorld::optimize_broad_phase() {
    impl_->system.OptimizeBroadPhase();
}

std::vector<RigidBodyQueryHit> JoltRigidBodyWorld::ray_cast_all(
    Float3 origin,
    Float3 direction,
    float maximumDistance,
    const RigidBodyQueryFilter& filter) const {
    std::vector<RigidBodyQueryHit> hits;
    if (!finite_float3(origin) || !finite_float3(direction) ||
        !std::isfinite(maximumDistance) || !(maximumDistance > 0.0F) ||
        !(length_squared(direction) > 1.0e-12F) ||
        (!filter.includeStatic && !filter.includeDynamic)) return hits;
    impl_->rayQueries.fetch_add(1U, std::memory_order_relaxed);

    const Float3 translation = multiply(normalize(direction), maximumDistance);
    JPH::RayCastSettings settings;
    settings.mBackFaceModeTriangles = JPH::EBackFaceMode::CollideWithBackFaces;
    JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
    impl_->system.GetNarrowPhaseQuery().CastRay(
        JPH::RRayCast(to_jolt_position(origin), to_jolt_vec3(translation)), settings, collector);
    hits.reserve(collector.mHits.size());
    for (const JPH::RayCastResult& hit : collector.mHits) {
        const RigidBodyHandle handle = impl_->handle_from_body_id(hit.mBodyID);
        if (handle == kInvalidRigidBodyHandle ||
            !query_allows(handle, impl_->slots[handle].isStatic, filter)) continue;
        JPH::BodyLockRead lock(impl_->system.GetBodyLockInterface(), hit.mBodyID);
        if (!lock.Succeeded()) continue;
        const JPH::RVec3 point =
            to_jolt_position(origin) + hit.mFraction * to_jolt_vec3(translation);
        hits.push_back({
            handle,
            from_jolt(point),
            from_jolt(lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, point)),
            hit.mFraction,
            hit.mFraction * maximumDistance,
            impl_->slots[handle].contactMaterial,
            hit.mSubShapeID2.GetValue(),
        });
    }
    sort_query_hits(hits);
    return hits;
}

std::vector<RigidBodyQueryHit> JoltRigidBodyWorld::overlap_aabb(
    RigidBodyWorldBounds bounds,
    const RigidBodyQueryFilter& filter) const {
    std::vector<RigidBodyQueryHit> hits;
    if (!finite_float3(bounds.minimum) || !finite_float3(bounds.maximum) ||
        bounds.minimum.x > bounds.maximum.x || bounds.minimum.y > bounds.maximum.y ||
        bounds.minimum.z > bounds.maximum.z ||
        (!filter.includeStatic && !filter.includeDynamic)) return hits;
    impl_->overlapQueries.fetch_add(1U, std::memory_order_relaxed);

    JPH::AllHitCollisionCollector<JPH::CollideShapeBodyCollector> collector;
    impl_->system.GetBroadPhaseQuery().CollideAABox(
        JPH::AABox(to_jolt_vec3(bounds.minimum), to_jolt_vec3(bounds.maximum)), collector);
    hits.reserve(collector.mHits.size());
    std::vector<std::uint8_t> seen(impl_->slots.size(), 0U);
    for (const JPH::BodyID bodyId : collector.mHits) {
        const RigidBodyHandle handle = impl_->handle_from_body_id(bodyId);
        if (handle == kInvalidRigidBodyHandle ||
            !query_allows(handle, impl_->slots[handle].isStatic, filter)) continue;
        if (seen[handle] != 0U) continue;
        seen[handle] = 1U;
        hits.push_back({
            handle,
            impl_->slots[handle].currentTransform.position,
            {},
            0.0F,
            0.0F,
            impl_->slots[handle].contactMaterial,
            0U,
        });
    }
    sort_query_hits(hits);
    return hits;
}

std::vector<RigidBodyQueryHit> JoltRigidBodyWorld::cast_sphere_all(
    Float3 origin,
    float radius,
    Float3 direction,
    float maximumDistance,
    const RigidBodyQueryFilter& filter) const {
    std::vector<RigidBodyQueryHit> hits;
    if (!finite_float3(origin) || !finite_float3(direction) || !std::isfinite(radius) ||
        radius < 0.0F || !std::isfinite(maximumDistance) || !(maximumDistance > 0.0F) ||
        !(length_squared(direction) > 1.0e-12F) ||
        (!filter.includeStatic && !filter.includeDynamic)) return hits;
    impl_->shapeCasts.fetch_add(1U, std::memory_order_relaxed);
    if (radius == 0.0F) {
        hits = ray_cast_all(origin, direction, maximumDistance, filter);
        impl_->rayQueries.fetch_sub(1U, std::memory_order_relaxed);
        return hits;
    }

    const Float3 translation = multiply(normalize(direction), maximumDistance);
    JPH::SphereShape sphere(radius);
    const JPH::RShapeCast cast(
        &sphere,
        JPH::Vec3::sOne(),
        JPH::RMat44::sTranslation(to_jolt_position(origin)),
        to_jolt_vec3(translation));
    JPH::ShapeCastSettings settings;
    settings.mBackFaceModeTriangles = JPH::EBackFaceMode::CollideWithBackFaces;
    settings.mReturnDeepestPoint = true;
    JPH::AllHitCollisionCollector<JPH::CastShapeCollector> collector;
    impl_->system.GetNarrowPhaseQuery().CastShape(
        cast, settings, JPH::RVec3::sZero(), collector);
    hits.reserve(collector.mHits.size());
    for (const JPH::ShapeCastResult& hit : collector.mHits) {
        const RigidBodyHandle handle = impl_->handle_from_body_id(hit.mBodyID2);
        if (handle == kInvalidRigidBodyHandle ||
            !query_allows(handle, impl_->slots[handle].isStatic, filter)) continue;
        JPH::Vec3 normal = -hit.mPenetrationAxis;
        if (normal.LengthSq() > 1.0e-12F) normal = normal.Normalized();
        hits.push_back({
            handle,
            from_jolt(hit.mContactPointOn2),
            from_jolt(normal),
            hit.mFraction,
            hit.mFraction * maximumDistance,
            impl_->slots[handle].contactMaterial,
            hit.mSubShapeID2.GetValue(),
        });
    }
    sort_query_hits(hits);
    return hits;
}

std::optional<JoltRayHit> JoltRigidBodyWorld::ray_cast_closest(
    Float3 origin,
    Float3 translation,
    bool collideWithBackFaces) const {
    if (!finite_float3(origin) || !finite_float3(translation) ||
        length_squared(translation) <= 1.0e-12F) return std::nullopt;
    impl_->rayQueries.fetch_add(1U, std::memory_order_relaxed);

    JPH::RayCastSettings settings;
    settings.mBackFaceModeTriangles = collideWithBackFaces
        ? JPH::EBackFaceMode::CollideWithBackFaces
        : JPH::EBackFaceMode::IgnoreBackFaces;
    JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> collector;
    impl_->system.GetNarrowPhaseQuery().CastRay(
        JPH::RRayCast(to_jolt_position(origin), to_jolt_vec3(translation)),
        settings,
        collector);
    if (!collector.HadHit()) return std::nullopt;

    const JPH::RayCastResult& hit = collector.mHit;
    JPH::BodyLockRead lock(impl_->system.GetBodyLockInterface(), hit.mBodyID);
    if (!lock.Succeeded()) return std::nullopt;
    const JPH::RVec3 point = to_jolt_position(origin) + hit.mFraction * to_jolt_vec3(translation);
    JoltRayHit result;
    result.body = impl_->handle_from_body_id(hit.mBodyID);
    result.point = from_jolt(point);
    result.normal = from_jolt(lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, point));
    result.fraction = hit.mFraction;
    result.material = impl_->material_for_body(hit.mBodyID);
    result.subShapeId = hit.mSubShapeID2.GetValue();
    return result;
}

std::size_t JoltRigidBodyWorld::query_aabb(
    const RigidBodyWorldBounds& bounds,
    std::span<JoltOverlapHit> output) const {
    if (!finite_float3(bounds.minimum) || !finite_float3(bounds.maximum) ||
        bounds.minimum.x > bounds.maximum.x || bounds.minimum.y > bounds.maximum.y ||
        bounds.minimum.z > bounds.maximum.z) return 0U;
    impl_->overlapQueries.fetch_add(1U, std::memory_order_relaxed);

    JPH::AllHitCollisionCollector<JPH::CollideShapeBodyCollector> collector;
    impl_->system.GetBroadPhaseQuery().CollideAABox(
        JPH::AABox(to_jolt_vec3(bounds.minimum), to_jolt_vec3(bounds.maximum)), collector);
    std::size_t written = 0U;
    for (const JPH::BodyID bodyId : collector.mHits) {
        if (written >= output.size()) break;
        const RigidBodyHandle handle = impl_->handle_from_body_id(bodyId);
        if (handle == kInvalidRigidBodyHandle) continue;
        output[written++] = {handle, impl_->material_for_body(bodyId)};
    }
    return written;
}

std::optional<JoltShapeCastHit> JoltRigidBodyWorld::cast_sphere_closest(
    Float3 center,
    float radiusMeters,
    Float3 translation) const {
    if (!finite_float3(center) || !finite_float3(translation) ||
        !std::isfinite(radiusMeters) || radiusMeters <= 0.0F ||
        length_squared(translation) <= 1.0e-12F) return std::nullopt;
    impl_->shapeCasts.fetch_add(1U, std::memory_order_relaxed);

    JPH::SphereShape sphere(radiusMeters);
    const JPH::RShapeCast cast(
        &sphere,
        JPH::Vec3::sOne(),
        JPH::RMat44::sTranslation(to_jolt_position(center)),
        to_jolt_vec3(translation));
    JPH::ShapeCastSettings settings;
    settings.mBackFaceModeTriangles = JPH::EBackFaceMode::CollideWithBackFaces;
    settings.mReturnDeepestPoint = true;
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    impl_->system.GetNarrowPhaseQuery().CastShape(
        cast, settings, JPH::RVec3::sZero(), collector);
    if (!collector.HadHit()) return std::nullopt;

    const JPH::ShapeCastResult& hit = collector.mHit;
    JoltShapeCastHit result;
    result.body = impl_->handle_from_body_id(hit.mBodyID2);
    result.point = from_jolt(hit.mContactPointOn2);
    JPH::Vec3 normal = -hit.mPenetrationAxis;
    if (normal.LengthSq() > 1.0e-12F) normal = normal.Normalized();
    result.normal = from_jolt(normal);
    result.fraction = hit.mFraction;
    result.penetrationDepth = hit.mPenetrationDepth;
    result.material = impl_->material_for_body(hit.mBodyID2);
    return result;
}

std::optional<JoltWorldSnapshot> JoltRigidBodyWorld::save_snapshot() const {
    MemoryStateRecorder recorder;
    impl_->system.SaveState(recorder, JPH::EStateRecorderState::All);
    if (recorder.IsFailed()) return std::nullopt;

    JoltWorldSnapshot snapshot;
    snapshot.solverBytes = recorder.bytes();
    snapshot.bodyStates.resize(impl_->slots.size());
    snapshot.liveBodyMask.resize(impl_->slots.size(), 0U);
    for (std::size_t index = 0U; index < impl_->slots.size(); ++index) {
        if (!impl_->slots[index].alive) continue;
        snapshot.liveBodyMask[index] = 1U;
        const auto bodyState = state(static_cast<RigidBodyHandle>(index));
        if (!bodyState) return std::nullopt;
        snapshot.bodyStates[index] = *bodyState;
    }

    MemoryStateRecorder characterRecorder;
    snapshot.characterStates.resize(impl_->characterSlots.size());
    snapshot.liveCharacterMask.resize(impl_->characterSlots.size(), 0U);
    for (std::size_t index = 0U; index < impl_->characterSlots.size(); ++index) {
        if (!impl_->characterSlots[index].alive) continue;
        snapshot.liveCharacterMask[index] = 1U;
        impl_->characterSlots[index].character->SaveState(characterRecorder);
        const auto characterState = virtual_character_state(
            static_cast<JoltVirtualCharacterHandle>(index));
        if (!characterState) return std::nullopt;
        snapshot.characterStates[index] = *characterState;
    }
    if (characterRecorder.IsFailed()) return std::nullopt;
    snapshot.characterSolverBytes = characterRecorder.bytes();
    snapshot.liveSoftBodyMask.resize(impl_->softBodySlots.size(), 0U);
    for (std::size_t index = 0U; index < impl_->softBodySlots.size(); ++index)
        snapshot.liveSoftBodyMask[index] = impl_->softBodySlots[index].alive ? 1U : 0U;
    snapshot.liveVehicleMask.resize(impl_->vehicleSlots.size(), 0U);
    for (std::size_t index = 0U; index < impl_->vehicleSlots.size(); ++index)
        snapshot.liveVehicleMask[index] = impl_->vehicleSlots[index].alive ? 1U : 0U;
    snapshot.contactTimeSeconds = impl_->contactTimeSeconds.load(std::memory_order_relaxed);
    impl_->snapshotsSaved.fetch_add(1U, std::memory_order_relaxed);
    return snapshot;
}

bool JoltRigidBodyWorld::restore_snapshot(const JoltWorldSnapshot& snapshot) {
    if (snapshot.liveBodyMask.size() != impl_->slots.size() ||
        snapshot.bodyStates.size() != impl_->slots.size() ||
        snapshot.liveCharacterMask.size() != impl_->characterSlots.size() ||
        snapshot.characterStates.size() != impl_->characterSlots.size() ||
        snapshot.liveSoftBodyMask.size() != impl_->softBodySlots.size() ||
        snapshot.liveVehicleMask.size() != impl_->vehicleSlots.size() ||
        snapshot.solverBytes.empty()) return false;
    for (std::size_t index = 0U; index < impl_->slots.size(); ++index) {
        if ((snapshot.liveBodyMask[index] != 0U) != impl_->slots[index].alive) return false;
    }
    for (std::size_t index = 0U; index < impl_->characterSlots.size(); ++index) {
        if ((snapshot.liveCharacterMask[index] != 0U) != impl_->characterSlots[index].alive) return false;
    }
    for (std::size_t index = 0U; index < impl_->softBodySlots.size(); ++index) {
        if ((snapshot.liveSoftBodyMask[index] != 0U) != impl_->softBodySlots[index].alive) return false;
    }
    for (std::size_t index = 0U; index < impl_->vehicleSlots.size(); ++index) {
        if ((snapshot.liveVehicleMask[index] != 0U) != impl_->vehicleSlots[index].alive) return false;
    }

    MemoryStateRecorder recorder(snapshot.solverBytes);
    if (!impl_->system.RestoreState(recorder) || recorder.IsFailed()) return false;
    for (std::size_t index = 0U; index < impl_->slots.size(); ++index) {
        if (!impl_->slots[index].alive) continue;
        impl_->slots[index].previousTransform = snapshot.bodyStates[index].previousTransform;
        impl_->slots[index].currentTransform = snapshot.bodyStates[index].currentTransform;
    }

    MemoryStateRecorder characterRecorder(snapshot.characterSolverBytes);
    for (std::size_t index = 0U; index < impl_->characterSlots.size(); ++index) {
        if (!impl_->characterSlots[index].alive) continue;
        impl_->characterSlots[index].character->RestoreState(characterRecorder);
    }
    if (characterRecorder.IsFailed()) return false;
    impl_->contactTimeSeconds.store(snapshot.contactTimeSeconds, std::memory_order_relaxed);
    impl_->snapshotsRestored.fetch_add(1U, std::memory_order_relaxed);
    return true;
}

JoltVirtualCharacterHandle JoltRigidBodyWorld::create_virtual_character(
    const JoltVirtualCharacterDesc& desc) {
    if (!finite_float3(desc.transform.position) || !std::isfinite(desc.radiusMeters) ||
        !std::isfinite(desc.cylinderHalfHeightMeters) || desc.radiusMeters <= 0.0F ||
        desc.cylinderHalfHeightMeters <= 0.0F || !std::isfinite(desc.massKilograms) ||
        desc.massKilograms <= 0.0F || !std::isfinite(desc.maxStrengthNewtons) ||
        desc.maxStrengthNewtons < 0.0F || !std::isfinite(desc.maxSlopeRadians) ||
        desc.maxSlopeRadians <= 0.0F || desc.maxSlopeRadians >= 1.570796F) {
        return kInvalidJoltVirtualCharacterHandle;
    }
    const JoltVirtualCharacterHandle handle = impl_->allocate_character_handle();
    if (handle == kInvalidJoltVirtualCharacterHandle) return handle;

    JPH::RefConst<JPH::Shape> shape = new JPH::CapsuleShape(
        desc.cylinderHalfHeightMeters, desc.radiusMeters);
    JPH::CharacterVirtualSettings settings;
    settings.mShape = shape;
    settings.mMass = desc.massKilograms;
    settings.mMaxStrength = desc.maxStrengthNewtons;
    settings.mMaxSlopeAngle = desc.maxSlopeRadians;
    settings.mCharacterPadding = std::max(0.001F, desc.characterPaddingMeters);
    settings.mPredictiveContactDistance = std::max(
        settings.mCharacterPadding, desc.predictiveContactDistanceMeters);
    if (desc.createInnerBody) {
        settings.mInnerBodyShape = shape;
        settings.mInnerBodyLayer = Layers::kMoving;
    }
    const RigidTransform normalized = make_rigid_transform(
        desc.transform.position, desc.transform.rotation);
    const JPH::uint64 userData = kCharacterUserDataBit |
        (static_cast<JPH::uint64>(handle) + 1U);
    JPH::Ref<JPH::CharacterVirtual> character = new JPH::CharacterVirtual(
        &settings,
        to_jolt_position(normalized.position),
        to_jolt_quat(normalized.rotation),
        userData,
        &impl_->system);
    if (character == nullptr) {
        impl_->freeCharacterHandles.push_back(handle);
        return kInvalidJoltVirtualCharacterHandle;
    }
    Impl::CharacterSlot& slot = impl_->characterSlots[handle];
    slot.alive = true;
    slot.character = std::move(character);
    slot.character->RefreshContacts(
        impl_->system.GetDefaultBroadPhaseLayerFilter(Layers::kMoving),
        impl_->system.GetDefaultLayerFilter(Layers::kMoving),
        {}, {}, impl_->tempAllocator);
    return handle;
}

bool JoltRigidBodyWorld::destroy_virtual_character(JoltVirtualCharacterHandle handle) {
    if (!impl_->valid_character(handle)) return false;
    impl_->characterSlots[handle] = {};
    impl_->freeCharacterHandles.push_back(handle);
    return true;
}

bool JoltRigidBodyWorld::update_virtual_character(
    JoltVirtualCharacterHandle handle,
    const JoltVirtualCharacterInput& input,
    float fixedDeltaSeconds) {
    if (!impl_->valid_character(handle) || !(fixedDeltaSeconds > 0.0F) ||
        !std::isfinite(fixedDeltaSeconds) || !finite_float3(input.desiredHorizontalVelocity) ||
        !std::isfinite(input.jumpSpeedMetersPerSecond) || input.jumpSpeedMetersPerSecond < 0.0F) {
        return false;
    }
    JPH::CharacterVirtual& character = *impl_->characterSlots[handle].character;
    character.UpdateGroundVelocity();
    const JPH::Vec3 up = character.GetUp();
    const JPH::Vec3 currentVelocity = character.GetLinearVelocity();
    const JPH::Vec3 currentVertical = currentVelocity.Dot(up) * up;
    const JPH::Vec3 currentHorizontal = currentVelocity - currentVertical;
    JPH::Vec3 desired = to_jolt_vec3(input.desiredHorizontalVelocity);
    desired -= desired.Dot(up) * up;
    const JPH::Vec3 groundVelocity = character.GetGroundVelocity();
    const bool supported = character.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
    const bool movingAwayFromGround = (currentVertical - groundVelocity).Dot(up) > 0.1F;

    JPH::Vec3 velocity;
    if (supported && !movingAwayFromGround) {
        velocity = groundVelocity + desired;
        if (input.jump) velocity += input.jumpSpeedMetersPerSecond * up;
    } else {
        velocity = currentVertical + (input.controlInAir ? desired : currentHorizontal);
    }
    velocity += impl_->system.GetGravity() * fixedDeltaSeconds;
    character.SetLinearVelocity(velocity);

    JPH::CharacterVirtual::ExtendedUpdateSettings updateSettings;
    character.ExtendedUpdate(
        fixedDeltaSeconds,
        impl_->system.GetGravity(),
        updateSettings,
        impl_->system.GetDefaultBroadPhaseLayerFilter(Layers::kMoving),
        impl_->system.GetDefaultLayerFilter(Layers::kMoving),
        {}, {}, impl_->tempAllocator);
    impl_->characterUpdates.fetch_add(1U, std::memory_order_relaxed);
    return true;
}

std::optional<JoltVirtualCharacterState> JoltRigidBodyWorld::virtual_character_state(
    JoltVirtualCharacterHandle handle) const {
    if (!impl_->valid_character(handle)) return std::nullopt;
    const JPH::CharacterVirtual& character = *impl_->characterSlots[handle].character;
    JoltVirtualCharacterState result;
    result.transform = make_rigid_transform(
        from_jolt(character.GetPosition()), from_jolt(character.GetRotation()));
    result.linearVelocity = from_jolt(character.GetLinearVelocity());
    result.groundVelocity = from_jolt(character.GetGroundVelocity());
    result.groundNormal = from_jolt(character.GetGroundNormal());
    const JPH::uint64 groundUserData = character.GetGroundUserData();
    if ((groundUserData & kCharacterUserDataBit) == 0U && groundUserData > 0U &&
        groundUserData <= static_cast<JPH::uint64>(kInvalidRigidBodyHandle)) {
        const RigidBodyHandle body = static_cast<RigidBodyHandle>(groundUserData - 1U);
        if (impl_->valid(body)) result.groundBody = body;
    }
    switch (character.GetGroundState()) {
    case JPH::CharacterBase::EGroundState::OnGround:
        result.groundState = JoltCharacterGroundState::OnGround;
        break;
    case JPH::CharacterBase::EGroundState::OnSteepGround:
        result.groundState = JoltCharacterGroundState::OnSteepGround;
        break;
    case JPH::CharacterBase::EGroundState::NotSupported:
        result.groundState = JoltCharacterGroundState::NotSupported;
        break;
    case JPH::CharacterBase::EGroundState::InAir:
        result.groundState = JoltCharacterGroundState::InAir;
        break;
    }
    return result;
}

bool JoltRigidBodyWorld::set_virtual_character_state(
    JoltVirtualCharacterHandle handle,
    const JoltVirtualCharacterState& stateValue) {
    if (!impl_->valid_character(handle) || !finite_float3(stateValue.transform.position) ||
        !finite_float3(stateValue.linearVelocity)) return false;
    JPH::CharacterVirtual& character = *impl_->characterSlots[handle].character;
    const RigidTransform normalized = make_rigid_transform(
        stateValue.transform.position, stateValue.transform.rotation);
    character.SetPosition(to_jolt_position(normalized.position));
    character.SetRotation(to_jolt_quat(normalized.rotation));
    character.SetLinearVelocity(to_jolt_vec3(stateValue.linearVelocity));
    character.RefreshContacts(
        impl_->system.GetDefaultBroadPhaseLayerFilter(Layers::kMoving),
        impl_->system.GetDefaultLayerFilter(Layers::kMoving),
        {}, {}, impl_->tempAllocator);
    return true;
}

bool JoltRigidBodyWorld::destroy_body(RigidBodyHandle handle) {
    if (!impl_->valid(handle)) return false;
    for (std::size_t index = 0U; index < impl_->vehicleSlots.size(); ++index) {
        if (impl_->vehicleSlots[index].alive && impl_->vehicleSlots[index].chassis == handle) {
            Impl::VehicleSlot& vehicle = impl_->vehicleSlots[index];
            impl_->system.RemoveStepListener(vehicle.constraint);
            impl_->system.RemoveConstraint(vehicle.constraint);
            vehicle = {};
            impl_->freeVehicleHandles.push_back(static_cast<JoltVehicleHandle>(index));
        }
    }
    for (std::size_t index = 0U; index < impl_->constraintSlots.size(); ++index) {
        const Impl::ConstraintSlot& slot = impl_->constraintSlots[index];
        if (slot.alive && (slot.parentBody == handle || slot.childBody == handle))
            (void)destroy_constraint(static_cast<RigidBodyConstraintHandle>(index));
    }
    JPH::BodyInterface& bodyInterface = impl_->system.GetBodyInterface();
    const JPH::BodyID id = impl_->slots[handle].bodyId;
    bodyInterface.RemoveBody(id);
    bodyInterface.DestroyBody(id);
    if (IPhysicsContactSink* sink = impl_->contactSink.load(std::memory_order_acquire))
        sink->body_removed(handle);
    impl_->slots[handle] = Impl::Slot{};
    impl_->freeHandles.push_back(handle);
    return true;
}

std::optional<RigidBodyState> JoltRigidBodyWorld::state(RigidBodyHandle handle) const {
    if (!impl_->valid(handle)) return std::nullopt;
    const Impl::Slot& slot = impl_->slots[handle];
    RigidBodyState result;
    result.previousTransform = slot.previousTransform;
    result.currentTransform = slot.currentTransform;
    if (!slot.isStatic) {
        const JPH::BodyInterface& bodyInterface = impl_->system.GetBodyInterfaceNoLock();
        result.linearVelocity = from_jolt(bodyInterface.GetLinearVelocity(slot.bodyId));
        result.angularVelocity = from_jolt(bodyInterface.GetAngularVelocity(slot.bodyId));
        result.sleeping = !bodyInterface.IsActive(slot.bodyId);
    } else {
        result.sleeping = true;
    }
    return result;
}

bool JoltRigidBodyWorld::set_state(RigidBodyHandle handle, const RigidBodyState& stateValue) {
    if (!impl_->valid(handle) || !rigid_body_state_is_finite(stateValue)) return false;
    Impl::Slot& slot = impl_->slots[handle];
    JPH::BodyInterface& bodyInterface = impl_->system.GetBodyInterface();
    const RigidTransform previous = make_rigid_transform(
        stateValue.previousTransform.position, stateValue.previousTransform.rotation);
    const RigidTransform current = make_rigid_transform(
        stateValue.currentTransform.position, stateValue.currentTransform.rotation);

    if (slot.isStatic) {
        bodyInterface.SetPositionAndRotation(
            slot.bodyId,
            to_jolt_position(current.position),
            to_jolt_quat(current.rotation),
            JPH::EActivation::DontActivate);
    } else {
        bodyInterface.SetPositionRotationAndVelocity(
            slot.bodyId,
            to_jolt_position(current.position),
            to_jolt_quat(current.rotation),
            to_jolt_vec3(stateValue.linearVelocity),
            to_jolt_vec3(stateValue.angularVelocity));
        if (stateValue.sleeping) bodyInterface.DeactivateBody(slot.bodyId);
        else bodyInterface.ActivateBody(slot.bodyId);
    }
    slot.previousTransform = previous;
    slot.currentTransform = current;
    return true;
}

bool JoltRigidBodyWorld::apply_impulse(RigidBodyHandle handle, Float3 worldImpulse) {
    if (!impl_->valid(handle) || impl_->slots[handle].isStatic) return false;
    if (!std::isfinite(worldImpulse.x) || !std::isfinite(worldImpulse.y) ||
        !std::isfinite(worldImpulse.z)) return false;
    impl_->system.GetBodyInterface().AddImpulse(
        impl_->slots[handle].bodyId, to_jolt_vec3(worldImpulse));
    return true;
}

bool JoltRigidBodyWorld::apply_force(RigidBodyHandle handle, Float3 worldForce) {
    if (!impl_->valid(handle) || impl_->slots[handle].isStatic) return false;
    if (!std::isfinite(worldForce.x) || !std::isfinite(worldForce.y) ||
        !std::isfinite(worldForce.z)) return false;
    impl_->system.GetBodyInterface().AddForce(
        impl_->slots[handle].bodyId, to_jolt_vec3(worldForce));
    return true;
}

RigidBodyConstraintHandle JoltRigidBodyWorld::create_constraint(
    const RigidBodyConstraintDesc& desc) {
    JPH::Ref<JPH::Constraint> constraint = impl_->build_constraint(desc);
    if (constraint == nullptr) return kInvalidRigidBodyConstraintHandle;
    const RigidBodyConstraintHandle handle = impl_->allocate_constraint_handle();
    if (handle == kInvalidRigidBodyConstraintHandle) return handle;

    constraint->SetUserData(static_cast<JPH::uint64>(handle) + 1U);
    impl_->system.AddConstraint(constraint);
    Impl::ConstraintSlot& slot = impl_->constraintSlots[handle];
    slot.alive = true;
    slot.parentBody = desc.parentBody;
    slot.childBody = desc.childBody;
    slot.constraint = std::move(constraint);
    return handle;
}

bool JoltRigidBodyWorld::destroy_constraint(RigidBodyConstraintHandle handle) {
    if (!impl_->valid_constraint(handle)) return false;
    Impl::ConstraintSlot& slot = impl_->constraintSlots[handle];
    impl_->system.RemoveConstraint(slot.constraint);
    slot = {};
    impl_->freeConstraintHandles.push_back(handle);
    return true;
}

bool JoltRigidBodyWorld::teleport_body(
    RigidBodyHandle handle,
    const RigidTransform& transform,
    Float3 linearVelocity,
    Float3 angularVelocity) {
    RigidBodyState target;
    target.previousTransform = transform;
    target.currentTransform = transform;
    target.linearVelocity = linearVelocity;
    target.angularVelocity = angularVelocity;
    target.sleeping = false;
    return set_state(handle, target);
}

void JoltRigidBodyWorld::step(float fixedDeltaSeconds) {
    if (!(fixedDeltaSeconds > 0.0F) || !std::isfinite(fixedDeltaSeconds)) {
        ++impl_->telemetry.invalidStepInputs;
        return;
    }
    const JPH::BodyInterface& bodyInterface = impl_->system.GetBodyInterfaceNoLock();
    for (Impl::Slot& slot : impl_->slots) {
        if (!slot.alive || slot.isStatic) continue;
        slot.previousTransform = slot.currentTransform;
    }
    impl_->contactTimeSeconds.store(
        impl_->contactTimeSeconds.load(std::memory_order_relaxed) + fixedDeltaSeconds,
        std::memory_order_relaxed);
    const JPH::EPhysicsUpdateError error = impl_->system.Update(
        fixedDeltaSeconds, static_cast<int>(impl_->config.collisionSteps),
        &impl_->tempAllocator, &impl_->jobSystem);
    impl_->record_update_error(error);
    if (!impl_->softBodySlots.empty()) impl_->softBodyUpdates.fetch_add(1U, std::memory_order_relaxed);
    for (Impl::Slot& slot : impl_->slots) {
        if (!slot.alive || slot.isStatic) continue;
        const JPH::RVec3 position = bodyInterface.GetCenterOfMassPosition(slot.bodyId);
        const JPH::Quat rotation = bodyInterface.GetRotation(slot.bodyId);
        slot.currentTransform = make_rigid_transform(from_jolt(position), from_jolt(rotation));
    }
}

void JoltRigidBodyWorld::set_gravity(Float3 acceleration) noexcept {
    if (std::isfinite(acceleration.x) && std::isfinite(acceleration.y) && std::isfinite(acceleration.z)) {
        impl_->system.SetGravity(to_jolt_vec3(acceleration));
    }
}

bool JoltRigidBodyWorld::set_damping(
    RigidBodyHandle handle,
    float linearDamping,
    float angularDamping) {
    if (!impl_->valid(handle) || impl_->slots[handle].isStatic) return false;
    if (!std::isfinite(linearDamping) || !std::isfinite(angularDamping) ||
        linearDamping < 0.0F || angularDamping < 0.0F) return false;
    JPH::Body* body = impl_->system.GetBodyLockInterfaceNoLock().TryGetBody(impl_->slots[handle].bodyId);
    if (body == nullptr) return false;
    JPH::MotionProperties* motionProperties = body->GetMotionProperties();
    motionProperties->SetLinearDamping(linearDamping);
    motionProperties->SetAngularDamping(angularDamping);
    return true;
}

bool JoltRigidBodyWorld::apply_impulse_at_point(
    RigidBodyHandle handle,
    Float3 impulse,
    Float3 worldPoint) {
    if (!impl_->valid(handle) || impl_->slots[handle].isStatic) return false;
    if (!std::isfinite(impulse.x) || !std::isfinite(impulse.y) || !std::isfinite(impulse.z) ||
        !std::isfinite(worldPoint.x) || !std::isfinite(worldPoint.y) || !std::isfinite(worldPoint.z)) {
        return false;
    }
    impl_->system.GetBodyInterface().AddImpulse(
        impl_->slots[handle].bodyId, to_jolt_vec3(impulse), to_jolt_position(worldPoint));
    return true;
}

bool JoltRigidBodyWorld::apply_force_at_point(
    RigidBodyHandle handle,
    Float3 force,
    Float3 worldPoint) {
    if (!impl_->valid(handle) || impl_->slots[handle].isStatic) return false;
    if (!std::isfinite(force.x) || !std::isfinite(force.y) || !std::isfinite(force.z) ||
        !std::isfinite(worldPoint.x) || !std::isfinite(worldPoint.y) || !std::isfinite(worldPoint.z)) {
        return false;
    }
    impl_->system.GetBodyInterface().AddForce(
        impl_->slots[handle].bodyId, to_jolt_vec3(force), to_jolt_position(worldPoint));
    return true;
}

bool JoltRigidBodyWorld::apply_angular_impulse(
    RigidBodyHandle handle,
    Float3 worldAngularImpulse) {
    if (!impl_->valid(handle) || impl_->slots[handle].isStatic ||
        !finite_float3(worldAngularImpulse)) return false;
    impl_->system.GetBodyInterface().AddAngularImpulse(
        impl_->slots[handle].bodyId, to_jolt_vec3(worldAngularImpulse));
    return true;
}

bool JoltRigidBodyWorld::apply_torque(
    RigidBodyHandle handle,
    Float3 worldTorque) {
    if (!impl_->valid(handle) || impl_->slots[handle].isStatic ||
        !finite_float3(worldTorque)) return false;
    impl_->system.GetBodyInterface().AddTorque(
        impl_->slots[handle].bodyId, to_jolt_vec3(worldTorque));
    return true;
}

std::optional<bool> JoltRigidBodyWorld::is_active(RigidBodyHandle handle) const {
    if (!impl_->valid(handle)) return std::nullopt;
    if (impl_->slots[handle].isStatic) return false;
    return impl_->system.GetBodyInterfaceNoLock().IsActive(impl_->slots[handle].bodyId);
}

RigidTransform JoltRigidBodyWorld::interpolated_transform(
    RigidBodyHandle handle,
    float alpha) const {
    if (!impl_->valid(handle)) return {};
    const Impl::Slot& slot = impl_->slots[handle];
    return interpolate_rigid_transform(slot.previousTransform, slot.currentTransform, alpha);
}

RigidBodyCounts JoltRigidBodyWorld::body_counts() const {
    RigidBodyCounts counts;
    const JPH::BodyInterface& bodyInterface = impl_->system.GetBodyInterfaceNoLock();
    for (const Impl::Slot& slot : impl_->slots) {
        if (!slot.alive) continue;
        ++counts.total;
        if (slot.isStatic) {
            ++counts.staticBodies;
            continue;
        }
        ++counts.dynamicBodies;
        if (bodyInterface.IsActive(slot.bodyId)) ++counts.awakeDynamicBodies;
        else ++counts.sleepingDynamicBodies;
    }
    return counts;
}

std::size_t JoltRigidBodyWorld::body_count() const { return body_counts().total; }
std::size_t JoltRigidBodyWorld::dynamic_body_count() const { return body_counts().dynamicBodies; }
std::size_t JoltRigidBodyWorld::awake_body_count() const { return body_counts().awakeDynamicBodies; }
std::size_t JoltRigidBodyWorld::sleeping_body_count() const { return body_counts().sleepingDynamicBodies; }

std::size_t JoltRigidBodyWorld::constraint_count() const noexcept {
    return static_cast<std::size_t>(std::count_if(
        impl_->constraintSlots.begin(), impl_->constraintSlots.end(),
        [](const Impl::ConstraintSlot& slot) { return slot.alive; }));
}

JoltPhysicsTelemetry JoltRigidBodyWorld::telemetry() const {
    JoltPhysicsTelemetry result = impl_->telemetry;
    result.contactCallbacks = impl_->contactCallbacks.load(std::memory_order_relaxed);
    result.contactEventsQueued = impl_->contactEventsQueued.load(std::memory_order_relaxed);
    result.contactEventsRejected = impl_->contactEventsRejected.load(std::memory_order_relaxed);
    result.rayQueries = impl_->rayQueries.load(std::memory_order_relaxed);
    result.overlapQueries = impl_->overlapQueries.load(std::memory_order_relaxed);
    result.shapeCasts = impl_->shapeCasts.load(std::memory_order_relaxed);
    result.snapshotsSaved = impl_->snapshotsSaved.load(std::memory_order_relaxed);
    result.snapshotsRestored = impl_->snapshotsRestored.load(std::memory_order_relaxed);
    result.characterUpdates = impl_->characterUpdates.load(std::memory_order_relaxed);
    result.heightFieldUpdates = impl_->heightFieldUpdates.load(std::memory_order_relaxed);
    result.mutableCompoundEdits = impl_->mutableCompoundEdits.load(std::memory_order_relaxed);
    result.softBodyUpdates = impl_->softBodyUpdates.load(std::memory_order_relaxed);
    result.vehicleUpdates = impl_->vehicleUpdates.load(std::memory_order_relaxed);
    result.debugFramesCaptured = impl_->debugFramesCaptured.load(std::memory_order_relaxed);
    result.virtualCharacterCount = static_cast<std::size_t>(std::count_if(
        impl_->characterSlots.begin(), impl_->characterSlots.end(),
        [](const Impl::CharacterSlot& slot) { return slot.alive; }));
    result.softBodyCount = static_cast<std::size_t>(std::count_if(
        impl_->softBodySlots.begin(), impl_->softBodySlots.end(),
        [](const Impl::SoftBodySlot& slot) { return slot.alive; }));
    result.vehicleCount = static_cast<std::size_t>(std::count_if(
        impl_->vehicleSlots.begin(), impl_->vehicleSlots.end(),
        [](const Impl::VehicleSlot& slot) { return slot.alive; }));
    result.bodyCounts = body_counts();
    return result;
}

bool JoltRigidBodyWorld::last_update_succeeded() const noexcept {
    return impl_->telemetry.lastUpdateErrorBits == 0U;
}

std::optional<Float3> JoltRigidBodyWorld::solver_shape_center_of_mass_local(
    RigidBodyHandle handle) const {
    if (!impl_->valid(handle)) return std::nullopt;
    const JPH::RefConst<JPH::Shape> shape =
        impl_->system.GetBodyInterfaceNoLock().GetShape(impl_->slots[handle].bodyId);
    if (shape == nullptr) return std::nullopt;
    return from_jolt(shape->GetCenterOfMass());
}

std::optional<SolverWorldBounds> JoltRigidBodyWorld::solver_world_bounds(
    RigidBodyHandle handle) const {
    if (!impl_->valid(handle)) return std::nullopt;
    const JPH::BodyInterface& bodyInterface = impl_->system.GetBodyInterfaceNoLock();
    const JPH::RefConst<JPH::Shape> shape = bodyInterface.GetShape(impl_->slots[handle].bodyId);
    if (shape == nullptr) return std::nullopt;
    const JPH::AABox bounds = shape->GetWorldSpaceBounds(
        bodyInterface.GetCenterOfMassTransform(impl_->slots[handle].bodyId), JPH::Vec3::sOne());
    return SolverWorldBounds{from_jolt(bounds.mMin), from_jolt(bounds.mMax)};
}

} // namespace dve
