#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <utility>

#include "dve/component.hpp"
#include "dve/damage.hpp"
#include "dve/editor_materials.hpp"
#include "dve/fragment.hpp"
#include "dve/geometry_build.hpp"
#include "dve/polygon_asset.hpp"
#include "dve/query.hpp"
#include "dve/rigid_body_adapter.hpp"
#include "dve/ragdoll_runtime.hpp"
#include "dve/transform.hpp"
#include "dve/voxel_object.hpp"
#if defined(DVE_ENABLE_DEFORMABLE_RUNTIME)
#include "dve/deformable_runtime.hpp"
#endif
#if defined(DVE_ENABLE_CPU_HAIR)
#include "dve/cpu_hair_runtime.hpp"
#endif

namespace dve {

namespace camera { class GameCameraRuntime; }
class GameplayRuntime;
class SkeletalAnimationRuntime;
class AnimationControllerRuntime;
class ControlRigRuntime;
namespace ui { class UiRuntime; }

using GameObjectId = std::uint64_t;
inline constexpr GameObjectId kInvalidGameObjectId = 0;

enum class GameGeometryKind : std::uint8_t { Marker, Voxel, Polygon, Deformable };


struct GameObjectAttachment {
    GameObjectId parent{kInvalidGameObjectId};
    RigidTransform localTransform{};
    std::string socket;
    bool inheritPosition{true};
    bool inheritRotation{true};
};

// Describes a new object at creation time. This is intentionally simpler than the editor's
// EditorObject/material-table pipeline: gameplay spawning wants "a box of this density," not
// per-voxel multi-material authoring, so every occupied voxel is treated as one uniform
// material for mass/physics purposes regardless of its stored material id. Authoring detail
// (multiple real materials, exact density per material) still comes from the editor/cooker
// pipeline; this path is for objects a script creates at runtime.
struct GameObjectDesc {
    GameObjectDesc() = default;
    GameObjectDesc(const GameObjectDesc&);
    GameObjectDesc& operator=(const GameObjectDesc&);
    GameObjectDesc(GameObjectDesc&&) noexcept = default;
    GameObjectDesc& operator=(GameObjectDesc&&) noexcept = default;
    std::string name;
    std::vector<std::string> tags;
    std::vector<std::string> groups;
    std::uint32_t layer{};
    std::vector<Component> components;
    bool enabled{true};
    RigidTransform transform{};
    float voxelSizeMeters{0.1F};
    // Null => a marker object: no voxels, no physics body, just an id/name/tags/transform a
    // script can query and move directly. Useful for spawn points, triggers, AI waypoints.
    std::unique_ptr<VoxelObject> voxels;
    // Ignored if voxels is null. true => dynamic rigid body (falls, can be pushed/damaged
    // apart... though runtime splitting-on-destruction is not implemented yet, see
    // damage_sphere). false => static collision, immovable, cannot be moved after creation.
    bool dynamic{true};
    double densityKilogramsPerCubicMeter{1000.0};
    bool structural{true};
};


enum class GameLifecycleEventKind : std::uint8_t {
    Spawn, Enable, Disable, OverlapBegin, OverlapEnd, Destroy,
    ComponentAdded, ComponentRemoved, ComponentEnabled, ComponentDisabled,
    PoolAcquire, PoolRelease
};

struct GameLifecycleEvent {
    GameLifecycleEventKind kind{GameLifecycleEventKind::Spawn};
    GameObjectId objectId{kInvalidGameObjectId};
    GameObjectId otherObjectId{kInvalidGameObjectId};
    ComponentId componentId{kInvalidComponentId};
    std::string componentType;
    std::string poolName;

    GameLifecycleEvent() = default;
    GameLifecycleEvent(GameLifecycleEventKind eventKind,
                       GameObjectId object = kInvalidGameObjectId,
                       GameObjectId other = kInvalidGameObjectId,
                       ComponentId component = kInvalidComponentId,
                       std::string type = {}, std::string pool = {})
        : kind(eventKind), objectId(object), otherObjectId(other), componentId(component),
          componentType(std::move(type)), poolName(std::move(pool)) {}
};

using GameObjectPoolId = std::uint64_t;
inline constexpr GameObjectPoolId kInvalidGameObjectPoolId = 0U;

struct GameObjectPoolDesc {
    std::string name;
    GameObjectDesc prototype;
    std::size_t capacity{};
};

struct GameRaycastHit {
    GameObjectId objectId{kInvalidGameObjectId};
    Float3 worldPosition{};
    Float3 worldNormal{};
    float distance{};
    MaterialId material{};
};


struct GameCapsuleHit {
    GameObjectId objectId{kInvalidGameObjectId};
    float time{};
    Float3 worldPosition{};
    Float3 worldNormal{};
    MaterialId material{};
    bool dynamic{};
};

struct GameCapsuleDepenetration {
    Float3 correction{};
    std::size_t iterations{};
};

struct GameVoxelBrickSnapshot {
    BrickKey brick{};
    std::uint32_t revision{};
    std::array<MaterialId, kBrickVoxelCount> materials{};
    std::uint64_t contentHash{};
};

struct GameDamageEvent {
    GameObjectId objectId{kInvalidGameObjectId};
    Float3 worldCenter{};
    float radius{};
    std::uint64_t removedVoxelCount{};
    bool destroyed{}; // true if this damage brought the object's voxel count to zero
    // Ids of any new dynamic objects created because this damage disconnected part of the
    // object from the rest (see GameWorld::damage_sphere). Empty if nothing detached, which
    // is the common case for most single hits.
    std::vector<GameObjectId> newFragmentIds;
};

// The tick loop and live, script-facing object model this engine did not previously have:
// RuntimeSceneObject is read-only streaming/rendering linkage, EditorJoltSimulation is scoped
// to the editor's Simulate/Play preview. GameWorld is the production runtime counterpart,
// independent of the editor, meant to be driven by a fixed-step loop (see tick()) and bound
// to a scripting layer (see game_script.hpp) rather than used directly by UI code.
class GameWorld {
public:
    explicit GameWorld(std::unique_ptr<IRigidBodyWorld> physics);
    ~GameWorld();
    GameWorld(const GameWorld&) = delete;
    GameWorld& operator=(const GameWorld&) = delete;

    [[nodiscard]] GameObjectId create_object(GameObjectDesc desc, std::string* error = nullptr);
    // Loads a .dvox cooked voxel asset (see dvox.hpp) and spawns it as a new object at the
    // given transform, using the asset's own per-material densities (not a single uniform
    // density the way create_object/spawn_box do) via build_fragment_solver_package, exactly
    // as src/runtime_scene.cpp's own asset-to-rigid-body path does for cooked scene loading.
    // `name` defaults to the file's stem if empty.
    [[nodiscard]] GameObjectId spawn_asset(
        const std::filesystem::path& path, std::string name, const RigidTransform& transform,
        bool dynamic, bool structural = true, std::string* error = nullptr);
    // Explicit polygon path. The generic spawn_asset() dispatches .dvox and .dmesh by extension.
    [[nodiscard]] GameObjectId spawn_polygon_asset(
        const std::filesystem::path& path, std::string name, const RigidTransform& transform,
        bool dynamic, bool structural = true, std::string* error = nullptr);
    [[nodiscard]] std::optional<GameGeometryKind> geometry_kind(GameObjectId id) const noexcept;
    bool destroy_object(GameObjectId id);
    [[nodiscard]] bool has_object(GameObjectId id) const;
    [[nodiscard]] std::size_t object_count() const noexcept { return objects_.size(); }
    [[nodiscard]] std::vector<GameObjectId> object_ids() const;

    [[nodiscard]] std::optional<GameObjectId> find_by_name(std::string_view name) const;
    [[nodiscard]] std::vector<GameObjectId> find_by_tag(std::string_view tag) const;
    [[nodiscard]] std::vector<GameObjectId> find_by_group(std::string_view group) const;
    [[nodiscard]] std::vector<GameObjectId> find_by_layer(std::uint32_t layer) const;
    [[nodiscard]] bool is_enabled(GameObjectId id) const noexcept;
    [[nodiscard]] bool set_enabled(GameObjectId id, bool enabled);
    [[nodiscard]] std::vector<GameObjectId> find_by_component(std::string_view type) const;
    [[nodiscard]] const std::vector<Component>* components(GameObjectId id) const noexcept;
    [[nodiscard]] Component* add_component(GameObjectId id, Component component, std::string* error = nullptr);
    [[nodiscard]] bool remove_component(GameObjectId id, ComponentId componentId);
    [[nodiscard]] bool set_component_property(GameObjectId id, ComponentId componentId,
                                              std::string property, ComponentValue value,
                                              std::string* error = nullptr);
    [[nodiscard]] bool set_component_enabled(GameObjectId id, ComponentId componentId, bool enabled);
    [[nodiscard]] const Component* component(GameObjectId id, ComponentId componentId) const noexcept;

    [[nodiscard]] bool attach_object(GameObjectId child, GameObjectId parent,
                                     bool preserveWorldTransform = true,
                                     std::string socket = {},
                                     bool inheritPosition = true,
                                     bool inheritRotation = true,
                                     std::string* error = nullptr);
    [[nodiscard]] bool detach_object(GameObjectId child, bool preserveWorldTransform = true,
                                     std::string* error = nullptr);
    [[nodiscard]] std::optional<GameObjectId> parent_of(GameObjectId child) const noexcept;
    [[nodiscard]] std::vector<GameObjectId> children_of(GameObjectId parent) const;
    [[nodiscard]] std::optional<RigidTransform> local_transform(GameObjectId child) const noexcept;
    [[nodiscard]] const std::string& name_of(GameObjectId id) const;
    [[nodiscard]] bool has_tag(GameObjectId id, std::string_view tag) const;
    // nullopt for an unknown id or a marker (no voxel data at all); 0 is a real, valid count
    // only in principle (create_object rejects an all-empty VoxelObject, and damage_sphere
    // auto-destroys an object the moment it reaches zero, so this should never actually be
    // observed at zero in practice, but nothing here assumes that).
    [[nodiscard]] std::optional<std::uint64_t> voxel_count(GameObjectId id) const;
    [[nodiscard]] std::optional<GameVoxelBrickSnapshot> voxel_brick_snapshot(
        GameObjectId id, BrickKey brick) const;
    // Applies a complete authoritative brick replacement and swaps in rebuilt collision only
    // after the replacement body has been accepted by the physics backend.
    bool replace_voxel_brick(
        GameObjectId id, const GameVoxelBrickSnapshot& snapshot,
        std::string* error = nullptr);

    // nullopt if id is unknown. For a dynamic object this is derived live from the physics
    // body every call (see the .cpp for why: center-of-mass vs. object-origin framing), not
    // cached, so it is always current even if called mid-tick.
    [[nodiscard]] std::optional<RigidTransform> transform(GameObjectId id) const;
    [[nodiscard]] std::optional<Float3> position(GameObjectId id) const;
    // Markers (no body) and dynamic bodies can be moved; static voxel bodies cannot (their
    // Jolt collision is baked in at creation) and this returns false for them.
    bool set_position(GameObjectId id, Float3 worldPosition);
    bool set_rotation(GameObjectId id, Quaternion worldRotation);

    [[nodiscard]] std::optional<Float3> linear_velocity(GameObjectId id) const;
    bool set_linear_velocity(GameObjectId id, Float3 velocity);
    bool apply_impulse(GameObjectId id, Float3 worldImpulse);
    bool apply_force(GameObjectId id, Float3 worldForce);

    // Skeletal actors use marker objects while this runtime owns their constrained body set.
    [[nodiscard]] bool bind_ragdoll(
        GameObjectId id, RagdollDefinition definition, RagdollRuntimeConfig config = {},
        std::string* error = nullptr);
    [[nodiscard]] bool activate_ragdoll(
        GameObjectId id, RagdollActivationOptions options = {}, std::string* error = nullptr);
    [[nodiscard]] bool recover_ragdoll(
        GameObjectId id, const RagdollRecoveryOptions& options, std::string* error = nullptr);

#if defined(DVE_ENABLE_DEFORMABLE_RUNTIME)
    [[nodiscard]] bool bind_deformable(
        GameObjectId id, SoftBodyAsset asset, DeformableBindOptions options = {},
        std::string* error = nullptr);
    [[nodiscard]] bool bind_deformable_asset(
        GameObjectId id, const std::filesystem::path& path,
        DeformableBindOptions options = {}, std::string* error = nullptr);
    [[nodiscard]] bool unbind_deformable(GameObjectId id) noexcept;
    [[nodiscard]] bool apply_deformable_impulse(GameObjectId id, Float3 worldImpulse) noexcept;
    [[nodiscard]] bool apply_deformable_radial_impulse(
        GameObjectId id, Float3 worldCenter, float radius, float strength) noexcept;
    [[nodiscard]] bool set_deformable_vertex_target(
        GameObjectId id, std::uint32_t vertex, Float3 worldPosition,
        bool clearVelocity = true) noexcept;
#endif

#if defined(DVE_ENABLE_CPU_HAIR)
    [[nodiscard]] bool bind_cpu_hair(
        GameObjectId id, HairAsset asset, CpuHairBindOptions options = {},
        std::string* error = nullptr);
    [[nodiscard]] bool bind_cpu_hair_asset(
        GameObjectId id, const std::filesystem::path& path,
        CpuHairBindOptions options = {}, std::string* error = nullptr);
    [[nodiscard]] bool unbind_cpu_hair(GameObjectId id) noexcept;
    [[nodiscard]] bool set_cpu_hair_root_targets(
        GameObjectId id, std::span<const HairRootTarget> targets,
        bool teleport = false) noexcept;
    [[nodiscard]] bool clear_cpu_hair_root_targets(GameObjectId id) noexcept;
    [[nodiscard]] bool set_cpu_hair_wind(GameObjectId id, Float3 windVelocity) noexcept;
    [[nodiscard]] bool set_cpu_hair_collision(
        GameObjectId id, HairCollisionSet collision);
    [[nodiscard]] bool apply_cpu_hair_impulse(GameObjectId id, Float3 impulse) noexcept;
#endif

    // Removes voxels within `radius` meters of `worldCenter` (world space) from the object's
    // own voxel data. Returns the removed voxel count, or nullopt if `id` is unknown or the
    // object has no voxels (a marker). If this empties the object completely, it is destroyed
    // automatically (physics body and all) and an on_destroyed event fires after the
    // on_damage event for this call. Does not currently split a partially-damaged dynamic
    // object into separate fragment bodies the way the editor's derived-jobs pipeline can for
    // authored destruction; a damaged object stays one (possibly disconnected-looking) body.
    std::optional<std::uint64_t> damage_sphere(GameObjectId id, Float3 worldCenter, float radius);

    // Nearest hit across every object with voxel data, or nullopt. Static and dynamic objects
    // are both tested at their current transform.
    [[nodiscard]] std::optional<GameRaycastHit> raycast(
        Float3 worldOrigin, Float3 worldDirection, float maxDistance) const;

    // Every object (with voxel data) whose collision proxy overlaps the given world-space
    // sphere, tested exactly (sphere-vs-oriented-box in the object's own local frame, not an
    // axis-aligned-world-box approximation), not optimized for repeated per-frame queries: it
    // rebuilds each candidate's box proxy on every call rather than caching it.
    [[nodiscard]] std::vector<GameObjectId> sphere_overlap(Float3 worldCenter, float radius) const;

    // World-wide character collision queries. Voxel objects use the conservative voxel capsule
    // sweep directly; polygon objects use exact segment/triangle capsule distance and continuous
    // conservative advancement through the polygon BVH. `ignoreObject` is normally the marker
    // object that represents the pawn.
    [[nodiscard]] std::optional<GameCapsuleHit> capsule_sweep(
        const Capsule& worldCapsule, Float3 worldDisplacement,
        GameObjectId ignoreObject = kInvalidGameObjectId) const;
    [[nodiscard]] bool capsule_overlaps(
        const Capsule& worldCapsule, GameObjectId ignoreObject = kInvalidGameObjectId) const;
    [[nodiscard]] GameCapsuleDepenetration depenetrate_capsule(
        Capsule& worldCapsule, GameObjectId ignoreObject = kInvalidGameObjectId,
        std::size_t maximumIterations = 8U, float skinMeters = 0.001F) const;
    [[nodiscard]] std::optional<MovingRigidTransform> moving_transform(GameObjectId id) const;
    [[nodiscard]] std::optional<Float3> velocity_at_point(GameObjectId id, Float3 worldPoint) const;
    [[nodiscard]] bool is_dynamic(GameObjectId id) const noexcept;

    // A minimal named input snapshot: something outside GameWorld (a real device poll, a test,
    // a future platform host) calls set_action_pressed/set_axis once per frame before tick(),
    // and scripts read it back via is_action_pressed/get_axis. GameWorld does not read any
    // actual input device itself; device polling is a platform concern this pass does not
    // touch (see the notes doc).
    void set_action_pressed(const std::string& action, bool pressed);
    [[nodiscard]] bool is_action_pressed(const std::string& action) const;
    void set_axis(const std::string& axis, float value);
    [[nodiscard]] float get_axis(const std::string& axis) const;

    using TimerId = std::uint64_t;
    // Fires once, `secondsFromNow` of simulated (tick-accumulated) time from now.
    TimerId schedule_once(float secondsFromNow, std::function<void()> callback);
    // Fires every `intervalSeconds`, starting `intervalSeconds` from now.
    TimerId schedule_repeating(float intervalSeconds, std::function<void()> callback);
    bool cancel_timer(TimerId id);

    using TickListener = std::function<void(float)>;
    using DamageListener = std::function<void(const GameDamageEvent&)>;
    using DestroyListener = std::function<void(GameObjectId)>;
    void on_tick(TickListener listener);
    void on_damage(DamageListener listener);
    void on_destroyed(DestroyListener listener);
    using LifecycleListener = std::function<void(const GameLifecycleEvent&)>;
    void on_lifecycle(LifecycleListener listener);

    [[nodiscard]] GameObjectPoolId register_pool(GameObjectPoolDesc desc, std::string* error = nullptr);
    [[nodiscard]] GameObjectId acquire_from_pool(GameObjectPoolId poolId, const RigidTransform& transform,
                                                 std::string* error = nullptr);
    [[nodiscard]] bool release_to_pool(GameObjectId id, std::string* error = nullptr);
    [[nodiscard]] std::size_t pool_available(GameObjectPoolId poolId) const noexcept;

    // Advances timers, steps physics by fixedDeltaSeconds, then fires tick listeners. Damage/
    // destroy listeners fire synchronously from damage_sphere()/destroy_object(), not from
    // here, since those are point-in-time actions a script triggers directly, not something
    // this loop discovers on its own (there is no "spontaneous destruction" source yet).
    void tick(float fixedDeltaSeconds);

    [[nodiscard]] IRigidBodyWorld& physics() noexcept { return *physics_; }
    [[nodiscard]] const IRigidBodyWorld& physics() const noexcept { return *physics_; }

    [[nodiscard]] camera::GameCameraRuntime& cameras() noexcept;
    [[nodiscard]] const camera::GameCameraRuntime& cameras() const noexcept;
    [[nodiscard]] GameplayRuntime& gameplay() noexcept;
    [[nodiscard]] const GameplayRuntime& gameplay() const noexcept;
    [[nodiscard]] SkeletalAnimationRuntime& animation() noexcept;
    [[nodiscard]] const SkeletalAnimationRuntime& animation() const noexcept;
    [[nodiscard]] AnimationControllerRuntime& animation_controllers() noexcept;
    [[nodiscard]] const AnimationControllerRuntime& animation_controllers() const noexcept;
    [[nodiscard]] ControlRigRuntime& control_rigs() noexcept;
    [[nodiscard]] const ControlRigRuntime& control_rigs() const noexcept;
    [[nodiscard]] RagdollRuntime& ragdolls() noexcept;
    [[nodiscard]] const RagdollRuntime& ragdolls() const noexcept;
    [[nodiscard]] ui::UiRuntime& ui_runtime() noexcept;
    [[nodiscard]] const ui::UiRuntime& ui_runtime() const noexcept;
#if defined(DVE_ENABLE_DEFORMABLE_RUNTIME)
    [[nodiscard]] DeformableRuntime& deformables() noexcept;
    [[nodiscard]] const DeformableRuntime& deformables() const noexcept;
    [[nodiscard]] const DeformableTickTelemetry& last_deformable_telemetry() const noexcept {
        return lastDeformableTelemetry_;
    }
#endif
#if defined(DVE_ENABLE_CPU_HAIR)
    [[nodiscard]] CpuHairRuntime& cpu_hair() noexcept;
    [[nodiscard]] const CpuHairRuntime& cpu_hair() const noexcept;
    [[nodiscard]] const CpuHairStepTelemetry& last_cpu_hair_telemetry() const noexcept {
        return lastCpuHairTelemetry_;
    }
#endif

private:
    struct Object;
    struct Timer;
    struct Pool;

    [[nodiscard]] RigidTransform resolve_base_transform(const Object& object) const;
    [[nodiscard]] std::optional<RigidTransform> resolve_attachment_frame(
        const GameObjectAttachment& attachment) const;
    [[nodiscard]] RigidTransform resolve_transform(const Object& object) const;
    [[nodiscard]] bool synchronize_attached_body(Object& object);
    void synchronize_attached_bodies();
    [[nodiscard]] GameObjectId allocate_id() noexcept { return nextId_++; }
    [[nodiscard]] GameObjectId create_object_internal(GameObjectDesc desc, std::optional<GameObjectId> forcedId,
                                                      bool dispatchSpawn, std::string* error);
    bool destroy_object_internal(GameObjectId id, bool dispatchDestroy);
    void dispatch_lifecycle(GameLifecycleEvent event);
    void synchronize_membership_component(Object& object);
    [[nodiscard]] std::optional<RigidBodyCreateDesc> build_dynamic_body_desc(
        const VoxelObject& voxels, const RigidTransform& authoredTransform, float voxelSizeMeters,
        const MaterialMassTable& massTable, double densityQuantumKilogramsPerCubicMeter, bool structural,
        Float3* outLocalCenterOfMassMeters, std::string* error) const;
    // Splits off every non-primary connected component left after a damage_sphere() call into
    // its own new dynamic GameObject (falling debris), rebuilds the (now smaller) primary
    // body in place, and returns the ids of every fragment created. Capped at
    // kMaxFragmentsPerDamageCall components; beyond that, the object is left as one
    // (visually disconnected) body rather than risk creating an unbounded number of bodies
    // from one call, matching the same budget-over-precision tradeoff the editor's own
    // collision-proxy pipeline makes elsewhere in this codebase.
    std::vector<GameObjectId> fragment_after_damage(GameObjectId id, Object& object);

    std::unique_ptr<IRigidBodyWorld> physics_;
    std::unique_ptr<camera::GameCameraRuntime> cameras_;
    std::unique_ptr<GameplayRuntime> gameplay_;
    std::unique_ptr<SkeletalAnimationRuntime> animation_;
    std::unique_ptr<AnimationControllerRuntime> animationControllers_;
    std::unique_ptr<ControlRigRuntime> controlRigs_;
    std::unique_ptr<RagdollRuntime> ragdolls_;
    std::unique_ptr<ui::UiRuntime> uiRuntime_;
#if defined(DVE_ENABLE_DEFORMABLE_RUNTIME)
    std::unique_ptr<DeformableRuntime> deformables_;
    DeformableTickTelemetry lastDeformableTelemetry_{};
#endif
#if defined(DVE_ENABLE_CPU_HAIR)
    std::unique_ptr<CpuHairRuntime> cpuHair_;
    CpuHairStepTelemetry lastCpuHairTelemetry_{};
#endif
    std::unordered_map<GameObjectId, Object> objects_;
    std::map<GameObjectPoolId, Pool> pools_;
    std::unordered_map<GameObjectId, GameObjectPoolId> objectPools_;
    GameObjectId nextId_{1};
    GameObjectPoolId nextPoolId_{1};
    std::vector<Timer> timers_;
    TimerId nextTimerId_{1};
    float elapsedSeconds_{};

    std::vector<TickListener> tickListeners_;
    std::vector<DamageListener> damageListeners_;
    std::vector<DestroyListener> destroyListeners_;
    std::vector<LifecycleListener> lifecycleListeners_;
    std::unordered_map<std::string, bool> actionStates_;
    std::unordered_map<std::string, float> axisStates_;
};

} // namespace dve
