#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <array>
#include <map>
#include <set>
#include <span>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <utility>

#include "dve/asset_cooker.hpp"
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
    // apart when damaged, see damage_sphere). false => static collision, immovable,
    // cannot be moved after creation.
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

// Where an object's geometry came from. Player saves (dve/game_save.hpp) store a voxel
// object's destruction as a brick delta against this asset instead of every voxel, and
// re-read polygon geometry from it. The scene loader and the Lua host's world.spawn_asset set
// it; fragments split off by damage inherit their parent's source with `derived` set (their
// voxels are a subset of the source, so saves store them in full but take the material table
// from the source).
struct GameObjectSource {
    std::string path;               // content path (or filesystem path) of the .dvox / .dmesh
    std::uint64_t contentHash{};    // FNV-1a 64 of the file bytes
    bool derived{};
    bool operator==(const GameObjectSource&) const = default;
};

// Plain-data snapshot of a GameWorld for save games (GameWorld::capture_save_state /
// restore_save_state). Encoding, deltas and versioning live in dve/game_save.hpp; this is
// only the in-memory state, complete enough to rebuild every object bit for bit.
struct GameWorldBrickState {
    BrickKey key{};
    std::uint32_t generation{};
    std::array<MaterialId, kBrickVoxelCount> materials{};
};

struct GameWorldObjectState {
    GameObjectId id{kInvalidGameObjectId};
    std::string name;
    std::vector<std::string> tags;
    std::vector<std::string> groups;
    std::uint32_t layer{};
    std::vector<Component> components;
    bool enabled{true};
    std::optional<GameObjectAttachment> attachment;
    RigidTransform authoredTransform{};
    float voxelSizeMeters{0.1F};
    GameGeometryKind kind{GameGeometryKind::Marker};   // Marker, Voxel or Polygon
    bool dynamic{};
    bool structural{true};
    bool visualOnly{};
    std::optional<GameObjectSource> source;
    // Voxel objects: every brick header in storage order (empty bricks included, since the
    // engine never erases them), the VoxelObject id and the render/mass tables.
    std::uint64_t voxelObjectId{};
    std::vector<GameWorldBrickState> bricks;
    std::vector<VoxelMaterialDefinition> materials;
    std::array<std::uint16_t, 256> densityUnits{};
    double densityQuantumKilogramsPerCubicMeter{1.0};
    // Polygon objects: capture leaves this empty (saves re-read `source`); restore needs it.
    std::shared_ptr<const CookedPolygonAsset> polygon;
    // Physics body (static or dynamic) as the backend reports it.
    bool hasBody{};
    RigidBodyState body{};
    Float3 localCenterOfMassMeters{};
    std::optional<GameObjectPoolId> pool;
};

struct GameWorldTimerState {
    std::uint64_t id{};
    float fireAtSeconds{};
    float intervalSeconds{};   // legacy schema v1/v2
    std::uint64_t fireAtTick{};
    std::uint64_t intervalTicks{};
    std::uint64_t intervalNanoseconds{}, originTick{}, releaseIndex{1};
};

struct GameWorldPoolState {
    GameObjectPoolId id{kInvalidGameObjectPoolId};
    std::string name;
    std::uint64_t capacity{};
    std::vector<GameObjectId> freeIds;
};

struct GameWorldDestructionState {
    std::uint64_t requestId{};
    GameObjectId objectId{};
    Float3 worldCenter{};
    float radius{};
    Float3 localCenterVoxels{};
    float localRadiusVoxels{};
    std::uint64_t workUnits{};
    bool stale{};
};

struct GameWorldSessionState {
    std::uint64_t debrisLimit{}, nextDebrisSequence{}, rejectedDestruction{};
    std::map<GameObjectId, std::uint64_t> debris;
    std::map<std::string, bool> actions;
    std::map<std::string, float> axes;
    // Reference solver loads issued since the last physics step, keyed by object id.
    std::map<GameObjectId, ReferenceRigidBodyWorld::PendingLoads> pendingLoads;
};

struct GameWorldSaveState {
    GameObjectId nextObjectId{1};
    GameObjectPoolId nextPoolId{1};
    std::uint64_t nextTimerId{1};
    float elapsedSeconds{}; // legacy v1/v2 conversion boundary
    std::uint64_t tickCount{};
    float fixedDeltaSeconds{1.0F / 60.0F};
    bool integerClock{};
    std::optional<GameWorldSessionState> session;
    std::uint64_t nextDestructionId{1};
    std::uint32_t destructionUnitsPerTick{4096};
    std::vector<GameWorldDestructionState> pendingDestruction;
    std::vector<GameWorldObjectState> objects;   // sorted by id
    std::vector<GameWorldTimerState> timers;     // live timers, in scheduling order
    std::vector<GameWorldPoolState> pools;       // sorted by id
};

struct GameWorldRestoreOptions {
    // Keep saved timers that have no live callback as *unbound* timers (same id, schedule and
    // order) instead of dropping them, so a script host can re-attach them by id
    // (GameWorld::bind_restored_timer, used for named Lua timers). Unbound timers never fire;
    // call drop_unbound_timers() once every binder has run.
    bool keepUnboundTimers{};
};

struct GameWorldRestoreReport {
    std::size_t objectsRestored{};
    std::size_t objectsRemoved{};     // live objects that were not in the save
    std::size_t timersRestored{};
    std::size_t timersCancelled{};    // live timers that were not in the save (already fired)
    std::size_t timersDropped{};      // saved timers whose callback no longer exists
    std::size_t timersUnbound{};      // saved timers kept without a callback (keepUnboundTimers)
    std::size_t poolsRestored{};
    std::size_t poolsDropped{};       // saved pools that were not registered again
};

// Read-only view of one GameWorld object for a renderer (see GameWorld::render_objects()).
// Pointers/spans reference GameWorld-owned storage and stay valid only until the next
// mutating GameWorld call (tick, spawn, destroy, damage, ...); copy what must outlive that.
struct GameRenderObject {
    GameObjectId id{kInvalidGameObjectId};
    const std::string* name{};
    GameGeometryKind kind{GameGeometryKind::Marker};
    const VoxelObject* voxels{};                      // Voxel objects only
    const CookedPolygonAsset* polygon{};              // Polygon objects only
    // Cooked material table for assets spawned from .dvox (spawn_asset & friends). Empty for
    // create_object()/spawn_box objects; renderers should fall back to a default palette.
    std::span<const VoxelMaterialDefinition> materials;
    RigidTransform transform{};                       // current world transform (object origin)
    float voxelSizeMeters{};
    bool enabled{true};
    bool dynamic{};
    // false for visual-only voxel objects (spawn_visual_asset): drawn, but no physics body and
    // skipped by raycasts/overlaps/capsule queries.
    bool collision{true};
    // Optional render-only levels supplied by the host. Their storage must
    // outlive render(); source/collision geometry remains `polygon`.
    std::span<const PolygonLodLevel> polygonLods;
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
    // In-memory variant of spawn_asset() for content read from a ContentSource/.dvepak.
    // `extension` selects the decoder (".dvox" or ".dmesh"); `name` defaults to "asset".
    // Validation and the resulting object are identical to spawning the same file by path.
    [[nodiscard]] GameObjectId spawn_asset_from_bytes(
        std::span<const std::byte> bytes, std::string_view extension, std::string name,
        const RigidTransform& transform, bool dynamic, bool structural = true,
        std::string* error = nullptr);
    // Spawns an already-decoded cooked voxel asset (what spawn_asset does after read_dvox).
    // Lets loaders validate every asset up front before creating any object.
    [[nodiscard]] GameObjectId spawn_cooked_asset(
        CookedVoxelAsset asset, std::string name, const RigidTransform& transform,
        bool dynamic, bool structural = true, std::string* error = nullptr);
    // Visual-only voxel object: keeps the voxels and material table for rendering but creates
    // no physics body, and raycast/sphere_overlap/capsule queries ignore it (the runtime
    // equivalent of DVOXSCENE generateCollision=false). It moves like a marker (set_position/
    // set_rotation always work), damage_sphere still carves it but never fragments it, and
    // replace_voxel_brick edits it without rebuilding collision.
    [[nodiscard]] GameObjectId spawn_visual_asset(
        CookedVoxelAsset asset, std::string name, const RigidTransform& transform,
        std::string* error = nullptr);
    // false for visual-only voxel objects; true for every other live object (markers have no
    // body either, but they never had geometry to collide with). nullopt for unknown ids.
    [[nodiscard]] std::optional<bool> has_collision(GameObjectId id) const noexcept;
    [[nodiscard]] GameObjectId spawn_cooked_polygon_asset(
        CookedPolygonAsset asset, std::string name, const RigidTransform& transform,
        bool dynamic, bool structural = true, std::string* error = nullptr);
    // Visual-only polygon object (DVOXSCENE generateCollision=false for a .dmesh): rendered,
    // movable like a marker, no physics body, ignored by raycasts/overlaps/capsule queries.
    [[nodiscard]] GameObjectId spawn_visual_polygon_asset(
        CookedPolygonAsset asset, std::string name, const RigidTransform& transform,
        std::string* error = nullptr);
    // Explicit polygon path. The generic spawn_asset() dispatches .dvox and .dmesh by extension.
    [[nodiscard]] GameObjectId spawn_polygon_asset(
        const std::filesystem::path& path, std::string name, const RigidTransform& transform,
        bool dynamic, bool structural = true, std::string* error = nullptr);
    [[nodiscard]] std::optional<GameGeometryKind> geometry_kind(GameObjectId id) const noexcept;
    bool destroy_object(GameObjectId id);
    [[nodiscard]] bool has_object(GameObjectId id) const;
    [[nodiscard]] std::size_t object_count() const noexcept { return objects_.size(); }
    // Debris cap (the voxel.debris_limit setting): bounds the dynamic
    // fragments damage splits off. Only split-created debris is counted —
    // authored objects and the surviving primary piece of a split are
    // never counted and never retired. When a new fragment would exceed
    // the cap, the oldest debris is retired first (deterministic FIFO);
    // a cap of zero suppresses debris creation entirely. Lowering the cap
    // below the current count retires the excess immediately.
    // The cap is runtime-only session state: the debris marking is not
    // written to saves, so fragments restored from a save return as
    // ordinary objects and are not counted against the cap.
    void set_debris_limit(std::size_t limit);
    [[nodiscard]] std::size_t debris_limit() const noexcept { return debrisLimit_; }
    [[nodiscard]] std::size_t debris_count() const noexcept;
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
    // Every live object (markers included, so callers can filter), sorted by id, with its
    // current world transform and read-only geometry/material views. This is the renderer's
    // only window into GameWorld; it never mutates state. See GameRenderObject for lifetime.
    [[nodiscard]] std::vector<GameRenderObject> render_objects(float interpolationAlpha = 1.0F) const;
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
    // Optional bounded alternative. Preparation never mutates live geometry. Results
    // publish before physics at a tick boundary. A stale source or a package exceeding
    // 32 components / 256 proxy boxes is rejected without changing the object.
    [[nodiscard]] std::optional<std::uint64_t> queue_damage_sphere(GameObjectId id, Float3 worldCenter, float radius);
    void set_destruction_units_per_tick(std::uint32_t units) noexcept { destructionUnitsPerTick_ = units; }
    [[nodiscard]] std::size_t pending_destruction_count() const noexcept;
    [[nodiscard]] std::uint64_t rejected_destruction_requests() const noexcept { return rejectedDestruction_; }
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
    [[nodiscard]] bool has_timer(TimerId id) const noexcept;   // scheduled and not cancelled

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

    // --- Save games (see dve/game_save.hpp for the file format) ---------------------------
    bool set_object_source(GameObjectId id, GameObjectSource source);
    [[nodiscard]] const GameObjectSource* object_source(GameObjectId id) const noexcept;
    // Everything needed to rebuild the objects, bodies, timers and pools. Polygon geometry is
    // referenced through GameObjectSource, not copied. Sub-runtimes (characters, triggers,
    // cameras, animation, ragdolls, hair) are captured separately by
    // capture_game_runtime_state() in dve/game_save.hpp.
    [[nodiscard]] GameWorldSaveState capture_save_state() const;
    // Replaces the world's objects with `state` (validated first; nothing changes if
    // validation fails). Objects are matched by id: live objects missing from the save are
    // destroyed, the others are rebuilt in place, so sub-runtime bindings keyed by id (made by
    // the startup script of a freshly booted world) survive. Timers are matched by id: the
    // callbacks of a fresh boot are kept with the saved schedule; saved timers whose callback
    // no longer exists are dropped (see the report). No lifecycle/destroy events fire. If a
    // physics backend rejects a body after validation, this returns false and the world is
    // incomplete; callers restoring into a fresh world should then discard it.
    [[nodiscard]] bool restore_save_state(
        const GameWorldSaveState& state, GameWorldRestoreReport* report = nullptr,
        std::string* error = nullptr, GameWorldRestoreOptions options = {});
    // Timers kept by GameWorldRestoreOptions::keepUnboundTimers, in scheduling order.
    [[nodiscard]] std::vector<TimerId> unbound_timer_ids() const;
    // Attaches `callback` to an unbound restored timer; false if `id` is not unbound.
    bool bind_restored_timer(TimerId id, std::function<void()> callback);
    // Cancels every timer that is still unbound; returns how many were dropped.
    std::size_t drop_unbound_timers();
    // FNV-1a over the saved state (ids, flags, transforms, bodies, voxels, tables, timers):
    // equal hashes mean capture_save_state() would produce the same snapshot.
    [[nodiscard]] std::uint64_t state_hash() const;
    // Includes session-only authority state that the legacy world hash omitted.
    [[nodiscard]] std::uint64_t runtime_state_hash() const;

    // Advances timers, steps physics by fixedDeltaSeconds, then fires tick listeners. Damage/
    // destroy listeners fire synchronously from damage_sphere()/destroy_object(), not from
    // here, since those are point-in-time actions a script triggers directly, not something
    // this loop discovers on its own (there is no "spontaneous destruction" source yet).
    void tick(float fixedDeltaSeconds);
    // Hosts opt in to frame-driven cameras/UI; legacy embedders retain tick presentation.
    void set_frame_presentation(bool enabled) noexcept { framePresentation_ = enabled; }
    [[nodiscard]] bool frame_presentation() const noexcept { return framePresentation_; }
    void update_presentation(float frameDeltaSeconds, float interpolationAlpha=1.0F);
    [[nodiscard]] std::optional<RigidTransform> presentation_transform(GameObjectId id, float alpha=1.0F) const;
    void reset_presentation_history() noexcept;
    [[nodiscard]] std::uint64_t tick_count() const noexcept { return tickCount_; }
    [[nodiscard]] float fixed_delta_seconds() const noexcept { return fixedDeltaSeconds_; }
    [[nodiscard]] double elapsed_seconds() const noexcept;

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
    struct PendingDestruction;
    void process_pending_destruction();
    bool commit_prepared_destruction(PendingDestruction& pending);
    std::vector<PendingDestruction> pendingDestruction_;
    std::uint64_t nextDestructionId_{1}, rejectedDestruction_{};
    std::uint32_t destructionUnitsPerTick_{4096};
    void advance_world_clock(float dt);
    void dispatch_timers();
    void step_physics(float dt);
    void evaluate_animation(float dt);
    void apply_root_motion();
    void step_gameplay(float dt);
    void step_secondary_motion(float dt);
    void dispatch_tick_listeners(float dt);
    bool framePresentation_{};
    bool inTick_{};
    std::map<GameObjectId, RigidTransform> presentationPrevious_;

    struct Object;
    struct Timer;
    struct Pool;

    [[nodiscard]] RigidTransform resolve_base_transform(const Object& object) const;
    [[nodiscard]] std::optional<RigidTransform> resolve_attachment_frame(
        const GameObjectAttachment& attachment) const;
    [[nodiscard]] RigidTransform resolve_transform(const Object& object) const;
    [[nodiscard]] bool synchronize_attached_body(Object& object);
    void synchronize_attached_bodies();
    // Rigid velocity field of an object (v(p) = linear + angular x (p - origin)), following
    // attachments up to the first free body. Zero for markers, static and visual-only objects.
    struct MotionField { Float3 linear{}; Float3 angular{}; Float3 origin{}; };
    [[nodiscard]] MotionField motion_field(const Object& object, std::size_t depth = 0U) const;
    // Keeps physics collision disabled between every attached child body and its parent body
    // (IRigidBodyWorld::set_pair_collision_enabled), so an attachment that touches or overlaps
    // its parent does not push it around.
    void update_attachment_collision_filters();
    [[nodiscard]] GameObjectId allocate_id() noexcept { return nextId_++; }
    [[nodiscard]] GameObjectId create_object_internal(GameObjectDesc desc, std::optional<GameObjectId> forcedId,
                                                      bool dispatchSpawn, std::string* error);
    bool destroy_object_internal(GameObjectId id, bool dispatchDestroy);
    // Destroys the debris body with the lowest creation sequence; false
    // when no debris exists. Used by the debris cap and its setter.
    bool retire_oldest_debris();
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
    void rebuild_primary_body(Object& object, const std::optional<RigidBodyState>& parentState);

    std::unique_ptr<IRigidBodyWorld> physics_;
    std::set<std::pair<RigidBodyHandle, RigidBodyHandle>> attachmentCollisionFilters_;
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
    std::size_t debrisLimit_{2048};
    std::uint64_t nextDebrisSequence_{1};
    GameObjectPoolId nextPoolId_{1};
    std::vector<Timer> timers_;
    TimerId nextTimerId_{1};
    std::uint64_t tickCount_{};
    float fixedDeltaSeconds_{1.0F / 60.0F};

    std::vector<std::shared_ptr<TickListener>> tickListeners_;
    std::vector<DamageListener> damageListeners_;
    std::vector<DestroyListener> destroyListeners_;
    std::vector<LifecycleListener> lifecycleListeners_;
    std::unordered_map<std::string, bool> actionStates_;
    std::unordered_map<std::string, float> axisStates_;
};

} // namespace dve
