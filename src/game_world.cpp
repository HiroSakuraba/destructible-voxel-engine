#include "dve/game_world.hpp"
#include "dve/animation.hpp"
#include "dve/animation_controller.hpp"
#include "dve/control_rig.hpp"
#include "dve/ragdoll_runtime.hpp"
#include "dve/gameplay_runtime.hpp"
#include "dve/camera_runtime.hpp"
#include "dve/game_ui.hpp"

#include <algorithm>
#include <cmath>

#include "dve/collision_proxy.hpp"
#include "dve/connectivity.hpp"
#include "dve/dvox.hpp"
#include "dve/polygon_collision.hpp"
#include "dve/polygon_bvh.hpp"
#include "dve/fragment.hpp"
#include "dve/query.hpp"

namespace dve {
namespace {

// Every occupied voxel is treated as the same density regardless of its stored material id
// (see GameObjectDesc's comment on why). Setting every possible material id to the same
// number of density units achieves that uniformly and keeps the existing mass/inertia math
// (compute_fragment_mass_properties etc.) completely unmodified.
[[nodiscard]] MaterialMassTable build_uniform_material_table(double densityKilogramsPerCubicMeter) {
    MaterialMassTable table;
    if (!(densityKilogramsPerCubicMeter > 0.0)) return table;
    for (int material = 0; material <= 255; ++material) {
        table.set_density_units(static_cast<MaterialId>(material), kMaximumMaterialDensityUnits);
    }
    return table;
}

// Mirrors src/runtime_scene.cpp's own make_material_mass_table() exactly (material index 0
// is Air/empty and is skipped, matching that convention): builds a table with each material's
// *real* density, for a cooked asset spawned via GameWorld::spawn_asset, rather than the
// single uniform density spawn_box/spawn_box3 use.
[[nodiscard]] MaterialMassTable build_material_mass_table_from_definitions(
    const std::vector<VoxelMaterialDefinition>& materials, double* outDensityQuantum) {
    double maximumDensity = 0.0;
    for (const VoxelMaterialDefinition& material : materials) {
        maximumDensity = std::max(maximumDensity, static_cast<double>(material.densityKilogramsPerCubicMeter));
    }
    MaterialMassTable table;
    if (!(maximumDensity > 0.0) || !std::isfinite(maximumDensity)) {
        if (outDensityQuantum) *outDensityQuantum = 0.0;
        return table;
    }
    const double quantum = maximumDensity / static_cast<double>(kMaximumMaterialDensityUnits);
    if (outDensityQuantum) *outDensityQuantum = quantum;
    const std::size_t count = std::min<std::size_t>(materials.size(), 256U);
    for (std::size_t i = 1; i < count; ++i) {
        const double density = static_cast<double>(materials[i].densityKilogramsPerCubicMeter);
        if (!(density > 0.0)) continue;
        const long rounded = std::lround(density / quantum);
        const auto units = static_cast<std::uint16_t>(std::clamp<long>(rounded, 1L, static_cast<long>(kMaximumMaterialDensityUnits)));
        table.set_density_units(static_cast<MaterialId>(i), units);
    }
    return table;
}

[[nodiscard]] SolverBox voxel_box_to_solver_box(const VoxelBox& box, float voxelSizeMeters) noexcept {
    const Float3 minimum{
        static_cast<float>(box.min.x) * voxelSizeMeters,
        static_cast<float>(box.min.y) * voxelSizeMeters,
        static_cast<float>(box.min.z) * voxelSizeMeters};
    const Float3 maximum{
        static_cast<float>(box.maxExclusive.x) * voxelSizeMeters,
        static_cast<float>(box.maxExclusive.y) * voxelSizeMeters,
        static_cast<float>(box.maxExclusive.z) * voxelSizeMeters};
    return {multiply(add(minimum, maximum), 0.5F), multiply(subtract(maximum, minimum), 0.5F)};
}


[[nodiscard]] double polygon_density(const CookedPolygonAsset& asset) noexcept {
    double weighted = 0.0;
    double weight = 0.0;
    for (const PolygonSubmesh& submesh : asset.submeshes) {
        if (submesh.materialIndex >= asset.materials.size()) continue;
        const double density = asset.materials[submesh.materialIndex].densityKilogramsPerCubicMeter;
        if (!(density > 0.0) || !std::isfinite(density)) continue;
        const double triangles = static_cast<double>(submesh.indexCount / 3U);
        weighted += density * triangles;
        weight += triangles;
    }
    return weight > 0.0 ? weighted / weight : 1000.0;
}


[[nodiscard]] Float3 cross_product(Float3 a, Float3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

[[nodiscard]] Capsule move_capsule(Capsule capsule, Float3 displacement) noexcept {
    capsule.pointA = add(capsule.pointA, displacement);
    capsule.pointB = add(capsule.pointB, displacement);
    return capsule;
}

#if defined(DVE_ENABLE_DEFORMABLE_RUNTIME)
struct DeformableRayHit { float distance{}; Float3 normal{}; };

std::optional<DeformableRayHit> ray_triangle_deformable(
    Float3 origin, Float3 direction, Float3 a, Float3 b, Float3 c, float maximumDistance) noexcept {
    const Float3 edgeA = subtract(b, a);
    const Float3 edgeB = subtract(c, a);
    const Float3 p = cross_product(direction, edgeB);
    const float determinant = dot(edgeA, p);
    if (std::abs(determinant) < 1.0e-8F) return std::nullopt;
    const float inverse = 1.0F / determinant;
    const Float3 offset = subtract(origin, a);
    const float u = dot(offset, p) * inverse;
    if (u < 0.0F || u > 1.0F) return std::nullopt;
    const Float3 q = cross_product(offset, edgeA);
    const float v = dot(direction, q) * inverse;
    if (v < 0.0F || u + v > 1.0F) return std::nullopt;
    const float distance = dot(edgeB, q) * inverse;
    if (distance < 0.0F || distance > maximumDistance) return std::nullopt;
    Float3 normal = normalize(cross_product(edgeA, edgeB));
    if (dot(normal, direction) > 0.0F) normal = multiply(normal, -1.0F);
    return DeformableRayHit{distance, normal};
}

std::optional<DeformableRayHit> ray_sphere_deformable(
    Float3 origin, Float3 direction, Float3 center, float radius, float maximumDistance) noexcept {
    const Float3 offset = subtract(origin, center);
    const float b = dot(offset, direction);
    const float c = dot(offset, offset) - radius * radius;
    const float discriminant = b * b - c;
    if (discriminant < 0.0F) return std::nullopt;
    float distance = -b - std::sqrt(discriminant);
    if (distance < 0.0F) distance = -b + std::sqrt(discriminant);
    if (distance < 0.0F || distance > maximumDistance) return std::nullopt;
    const Float3 position = add(origin, multiply(direction, distance));
    return DeformableRayHit{distance, normalize(subtract(position, center))};
}
#endif

[[nodiscard]] Capsule inverse_transform_capsule_exact(
    const RigidTransform& transform, const Capsule& worldCapsule) noexcept {
    return {inverse_transform_point(transform, worldCapsule.pointA),
            inverse_transform_point(transform, worldCapsule.pointB), worldCapsule.radius};
}

[[nodiscard]] std::optional<PolygonCapsuleContact> polygon_capsule_contact_world(
    const PolygonBvh& bvh, const RigidTransform& transform,
    const Capsule& worldCapsule) noexcept {
    auto contact = bvh.capsule_overlap(inverse_transform_capsule_exact(transform, worldCapsule));
    if (!contact) return std::nullopt;
    contact->capsulePoint = transform_point(transform, contact->capsulePoint);
    contact->polygonPoint = transform_point(transform, contact->polygonPoint);
    contact->normal = normalize(transform_vector(transform, contact->normal));
    return contact;
}

[[nodiscard]] bool polygon_capsule_overlap(
    const PolygonBvh& bvh, const RigidTransform& transform,
    const Capsule& capsule) noexcept {
    return polygon_capsule_contact_world(bvh, transform, capsule).has_value();
}

[[nodiscard]] std::optional<CapsuleSweepHit> sweep_polygon_capsule(
    const PolygonBvh& bvh, const RigidTransform& transform, const Capsule& capsule,
    Float3 displacement) noexcept {
    const Capsule localCapsule = inverse_transform_capsule_exact(transform, capsule);
    const Float3 localDisplacement = inverse_transform_vector(transform, displacement);
    const auto hit = bvh.sweep_capsule(localCapsule, localDisplacement);
    if (!hit) return std::nullopt;
    return CapsuleSweepHit{
        hit->time,
        transform_point(transform, hit->polygonPoint),
        normalize(transform_vector(transform, hit->normal)),
        {},
        static_cast<MaterialId>(std::min<std::uint32_t>(hit->materialIndex, 255U))};
}

RigidTransform compose_game_attachment(
    const RigidTransform& parentWorld, const GameObjectAttachment& attachment) noexcept {
    RigidTransform result = attachment.localTransform;
    if (attachment.inheritPosition) {
        result.position = add(parentWorld.position,
            attachment.inheritRotation
                ? rotate(parentWorld.rotation, attachment.localTransform.position)
                : attachment.localTransform.position);
    }
    if (attachment.inheritRotation)
        result.rotation = normalize(multiply(parentWorld.rotation, attachment.localTransform.rotation));
    else
        result.rotation = normalize(attachment.localTransform.rotation);
    return result;
}

RigidTransform game_attachment_local_from_world(
    const RigidTransform& parentWorld, const RigidTransform& childWorld,
    bool inheritPosition, bool inheritRotation) noexcept {
    RigidTransform local = childWorld;
    if (inheritPosition) {
        const Float3 delta = subtract(childWorld.position, parentWorld.position);
        local.position = inheritRotation
            ? rotate(conjugate(normalize(parentWorld.rotation)), delta)
            : delta;
    }
    if (inheritRotation)
        local.rotation = normalize(multiply(conjugate(normalize(parentWorld.rotation)), childWorld.rotation));
    else
        local.rotation = normalize(childWorld.rotation);
    return local;
}

std::unique_ptr<VoxelObject> clone_game_voxels(const VoxelObject* source) {
    if (!source) return {};
    auto copy = std::make_unique<VoxelObject>(source->id());
    copy->reserve_bricks(source->brick_count());
    for (const auto& [key, brick] : source->bricks()) {
        brick.occupancy().for_each_set([&](std::uint16_t index) {
            copy->set_voxel(global_from_local(key, local_from_index_unchecked(index)), brick.material(index));
        });
    }
    return copy;
}

constexpr std::size_t kMaxFragmentsPerDamageCall = 32U;

} // namespace

GameObjectDesc::GameObjectDesc(const GameObjectDesc& other)
    : name(other.name), tags(other.tags), groups(other.groups), layer(other.layer),
      components(other.components), enabled(other.enabled), transform(other.transform),
      voxelSizeMeters(other.voxelSizeMeters), voxels(clone_game_voxels(other.voxels.get())),
      dynamic(other.dynamic), densityKilogramsPerCubicMeter(other.densityKilogramsPerCubicMeter),
      structural(other.structural) {}

GameObjectDesc& GameObjectDesc::operator=(const GameObjectDesc& other) {
    if (this == &other) return *this;
    GameObjectDesc copy(other);
    *this = std::move(copy);
    return *this;
}

struct GameWorld::Object {
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
    std::unique_ptr<VoxelObject> voxels;
    std::unique_ptr<CookedPolygonAsset> polygon;
    std::unique_ptr<PolygonBvh> polygonBvh;
    bool dynamic{};
    bool structural{true};
    MaterialMassTable massTable{};
    double densityQuantumKilogramsPerCubicMeter{1.0};
    bool hasBody{};
    RigidBodyHandle bodyHandle{kInvalidRigidBodyHandle};
    Float3 localCenterOfMassMeters{}; // valid only if dynamic && hasBody
};

struct GameWorld::Timer {
    TimerId id{};
    float fireAtSeconds{};
    float intervalSeconds{}; // 0 => one-shot
    bool cancelled{};
    std::function<void()> callback;
};

struct GameWorld::Pool {
    GameObjectPoolId id{kInvalidGameObjectPoolId};
    std::string name;
    GameObjectDesc prototype;
    std::size_t capacity{};
    std::vector<GameObjectId> freeIds;
    std::set<GameObjectId> activeIds;
};

GameWorld::GameWorld(std::unique_ptr<IRigidBodyWorld> physics)
    : physics_(std::move(physics)), cameras_(std::make_unique<camera::GameCameraRuntime>()),
      gameplay_(std::make_unique<GameplayRuntime>(*this)),
      animation_(std::make_unique<SkeletalAnimationRuntime>()),
      animationControllers_(std::make_unique<AnimationControllerRuntime>(*animation_)),
      controlRigs_(std::make_unique<ControlRigRuntime>(*animation_)),
      ragdolls_(std::make_unique<RagdollRuntime>(*animation_, *physics_)),
      uiRuntime_(std::make_unique<ui::UiRuntime>())
#if defined(DVE_ENABLE_DEFORMABLE_RUNTIME)
      , deformables_(std::make_unique<DeformableRuntime>())
#endif
#if defined(DVE_ENABLE_CPU_HAIR)
      , cpuHair_(std::make_unique<CpuHairRuntime>())
#endif
      {
    gameplay_->on_trigger([this](const TriggerEvent& event) {
        if (event.kind == TriggerEventKind::Enter)
            dispatch_lifecycle({GameLifecycleEventKind::OverlapBegin, event.object, event.trigger});
        else if (event.kind == TriggerEventKind::Exit)
            dispatch_lifecycle({GameLifecycleEventKind::OverlapEnd, event.object, event.trigger});
    });
}
GameWorld::~GameWorld() = default;

camera::GameCameraRuntime& GameWorld::cameras() noexcept { return *cameras_; }
const camera::GameCameraRuntime& GameWorld::cameras() const noexcept { return *cameras_; }
GameplayRuntime& GameWorld::gameplay() noexcept { return *gameplay_; }
const GameplayRuntime& GameWorld::gameplay() const noexcept { return *gameplay_; }
SkeletalAnimationRuntime& GameWorld::animation() noexcept { return *animation_; }
const SkeletalAnimationRuntime& GameWorld::animation() const noexcept { return *animation_; }
AnimationControllerRuntime& GameWorld::animation_controllers() noexcept { return *animationControllers_; }
const AnimationControllerRuntime& GameWorld::animation_controllers() const noexcept { return *animationControllers_; }
ControlRigRuntime& GameWorld::control_rigs() noexcept { return *controlRigs_; }
const ControlRigRuntime& GameWorld::control_rigs() const noexcept { return *controlRigs_; }
RagdollRuntime& GameWorld::ragdolls() noexcept { return *ragdolls_; }
const RagdollRuntime& GameWorld::ragdolls() const noexcept { return *ragdolls_; }
ui::UiRuntime& GameWorld::ui_runtime() noexcept { return *uiRuntime_; }
const ui::UiRuntime& GameWorld::ui_runtime() const noexcept { return *uiRuntime_; }
#if defined(DVE_ENABLE_DEFORMABLE_RUNTIME)
DeformableRuntime& GameWorld::deformables() noexcept { return *deformables_; }
const DeformableRuntime& GameWorld::deformables() const noexcept { return *deformables_; }
#endif
#if defined(DVE_ENABLE_CPU_HAIR)
CpuHairRuntime& GameWorld::cpu_hair() noexcept { return *cpuHair_; }
const CpuHairRuntime& GameWorld::cpu_hair() const noexcept { return *cpuHair_; }
#endif

void GameWorld::dispatch_lifecycle(GameLifecycleEvent event) {
    for (const LifecycleListener& listener : lifecycleListeners_) listener(event);
}

void GameWorld::synchronize_membership_component(Object& object) {
    object.tags = normalize_membership_values(object.tags);
    object.groups = normalize_membership_values(object.groups);
    object.layer = std::min<std::uint32_t>(object.layer, 63U);
    Component* membership = dve::find_component_by_type(std::span<Component>(object.components), "dve.membership");
    if (!membership && object.tags.empty() && object.groups.empty() && object.layer == 0U) return;
    if (!membership) {
        Component component;
        component.id = 1U;
        for (const Component& existing : object.components) component.id = std::max(component.id, existing.id + 1U);
        component.type = "dve.membership";
        object.components.push_back(std::move(component));
        membership = &object.components.back();
    }
    membership->properties["tags"] = join_membership_values(object.tags);
    membership->properties["groups"] = join_membership_values(object.groups);
    membership->properties["layer"] = static_cast<std::int64_t>(object.layer);
}

std::optional<RigidBodyCreateDesc> GameWorld::build_dynamic_body_desc(
    const VoxelObject& voxels, const RigidTransform& authoredTransform, float voxelSizeMeters,
    const MaterialMassTable& massTable, double densityQuantumKilogramsPerCubicMeter, bool structural,
    Float3* outLocalCenterOfMassMeters, std::string* error) const {
    if (!(densityQuantumKilogramsPerCubicMeter > 0.0)) {
        if (error) *error = "object has no positive finite material density to build a body from";
        return std::nullopt;
    }
    // Fragment construction operates in voxel units. make_rigid_body_desc() is the single
    // authoritative conversion boundary and converts the body transform, boxes, mass, and
    // inertia to SI units.
    const RigidTransform voxelUnitTransform = make_rigid_transform(
        multiply(authoredTransform.position, 1.0F / voxelSizeMeters), authoredTransform.rotation);
    const FragmentSolverPackage package = build_fragment_solver_package(voxels, voxelUnitTransform, massTable);
    if (package.proxyOverBudget) {
        if (error) *error = "object's collision proxy exceeds the box budget";
        return std::nullopt;
    }
    const FragmentSolverScale scale{
        densityQuantumKilogramsPerCubicMeter * static_cast<double>(voxelSizeMeters) * voxelSizeMeters * voxelSizeMeters,
        voxelSizeMeters};
    std::optional<RigidBodyCreateDesc> bodyDesc = make_rigid_body_desc(package, scale);
    if (!bodyDesc) {
        if (error) *error = "could not build a rigid body for this object's voxel content";
        return std::nullopt;
    }
    bodyDesc->collisionClass = structural ? RigidBodyCollisionClass::Full : RigidBodyCollisionClass::DebrisNoSelf;
    if (!validate_rigid_body_desc(*bodyDesc)) {
        if (error) *error = "constructed rigid body descriptor failed validation";
        return std::nullopt;
    }
    if (outLocalCenterOfMassMeters) *outLocalCenterOfMassMeters = multiply(package.mass.centerOfMass, voxelSizeMeters);
    return bodyDesc;
}

GameObjectId GameWorld::create_object(GameObjectDesc desc, std::string* error) {
    return create_object_internal(std::move(desc), std::nullopt, true, error);
}

GameObjectId GameWorld::create_object_internal(
    GameObjectDesc desc, std::optional<GameObjectId> forcedId, bool dispatchSpawn, std::string* error) {
    if (desc.voxels && !geometry_kind_supported(GeometryKind::Voxel)) {
        if (error) *error = "this build profile does not enable voxel gameplay objects";
        return kInvalidGameObjectId;
    }
    Object object;
    object.name = std::move(desc.name);
    object.tags = normalize_membership_values(desc.tags);
    object.groups = normalize_membership_values(desc.groups);
    object.layer = std::min<std::uint32_t>(desc.layer, 63U);
    object.components = std::move(desc.components);
    object.enabled = desc.enabled;
    std::string componentError;
    if (!validate_components(object.components, nullptr, &componentError)) {
        if (error) *error = "invalid gameplay component set: " + componentError;
        return kInvalidGameObjectId;
    }
    object.authoredTransform = desc.transform;
    object.voxelSizeMeters = desc.voxelSizeMeters;
    object.dynamic = desc.dynamic;

    if (desc.voxels && (!(desc.voxelSizeMeters > 0.0F) || !std::isfinite(desc.voxelSizeMeters))) {
        if (error) *error = "voxelSizeMeters must be a positive, finite number of meters";
        return kInvalidGameObjectId;
    }

    if (desc.voxels && desc.voxels->occupied_voxel_count() > 0) {
        object.voxels = std::move(desc.voxels);
        object.structural = desc.structural;
        object.massTable = build_uniform_material_table(desc.densityKilogramsPerCubicMeter);
        object.densityQuantumKilogramsPerCubicMeter =
            desc.densityKilogramsPerCubicMeter / static_cast<double>(kMaximumMaterialDensityUnits);
        if (object.dynamic) {
            Float3 localComMeters{};
            const std::optional<RigidBodyCreateDesc> bodyDesc = build_dynamic_body_desc(
                *object.voxels, object.authoredTransform, object.voxelSizeMeters,
                object.massTable, object.densityQuantumKilogramsPerCubicMeter, desc.structural,
                &localComMeters, error);
            if (!bodyDesc) return kInvalidGameObjectId;
            const RigidBodyHandle handle = physics_->create_body(*bodyDesc);
            if (handle == kInvalidRigidBodyHandle) {
                if (error) *error = "physics backend rejected body creation";
                return kInvalidGameObjectId;
            }
            object.hasBody = true;
            object.bodyHandle = handle;
            object.localCenterOfMassMeters = localComMeters;
        } else {
            const std::vector<VoxelBox> boxes = build_merged_object_box_proxy(*object.voxels);
            if (boxes.empty()) {
                if (error) *error = "object has no collidable voxels";
                return kInvalidGameObjectId;
            }
            StaticRigidBodyCreateDesc staticDesc;
            staticDesc.transform = object.authoredTransform;
            staticDesc.collisionClass = RigidBodyCollisionClass::Full;
            staticDesc.boxes.reserve(boxes.size());
            for (const VoxelBox& box : boxes) staticDesc.boxes.push_back(voxel_box_to_solver_box(box, object.voxelSizeMeters));
            if (!validate_static_rigid_body_desc(staticDesc)) {
                if (error) *error = "constructed static body descriptor failed validation";
                return kInvalidGameObjectId;
            }
            const RigidBodyHandle handle = physics_->create_static_body(staticDesc);
            if (handle == kInvalidRigidBodyHandle) {
                if (error) *error = "physics backend rejected static body creation";
                return kInvalidGameObjectId;
            }
            object.hasBody = true;
            object.bodyHandle = handle;
        }
    }
    // No voxels, or voxels but empty: a marker. No physics body; authoredTransform is the
    // only source of truth and is directly settable.

    const GameObjectId id = forcedId.value_or(allocate_id());
    if (id == kInvalidGameObjectId || objects_.contains(id)) {
        if (object.hasBody) physics_->destroy_body(object.bodyHandle);
        if (error) *error = "game object id is already in use";
        return kInvalidGameObjectId;
    }
    nextId_ = std::max(nextId_, id + 1U);
    object.id = id;
    synchronize_membership_component(object);
    objects_.emplace(id, std::move(object));
    if (dispatchSpawn) dispatch_lifecycle({GameLifecycleEventKind::Spawn, id});
    return id;
}

GameObjectId GameWorld::spawn_asset(
    const std::filesystem::path& path, std::string name, const RigidTransform& transform,
    bool dynamic, bool structural, std::string* error) {
    const std::string extension = path.extension().string();
    if (extension == ".dmesh" || extension == ".DMESH") {
        return spawn_polygon_asset(path, std::move(name), transform, dynamic, structural, error);
    }
    if (!geometry_kind_supported(GeometryKind::Voxel)) {
        if (error) *error = "this build profile does not enable voxel gameplay objects";
        return kInvalidGameObjectId;
    }
    DvoxReadResult result = read_dvox(path);
    if (!result.success) {
        if (error) *error = result.error.empty() ? "failed to read cooked asset" : result.error;
        return kInvalidGameObjectId;
    }
    if (result.asset.object.occupied_voxel_count() == 0) {
        if (error) *error = "cooked asset has no occupied voxels";
        return kInvalidGameObjectId;
    }
    if (!(result.asset.voxelSizeMeters > 0.0F) || !std::isfinite(result.asset.voxelSizeMeters)) {
        if (error) *error = "cooked asset has an invalid voxelSizeMeters";
        return kInvalidGameObjectId;
    }

    Object object;
    object.name = name.empty() ? path.stem().string() : std::move(name);
    object.authoredTransform = transform;
    object.voxelSizeMeters = result.asset.voxelSizeMeters;
    object.dynamic = dynamic;
    object.structural = structural;
    object.voxels = std::make_unique<VoxelObject>(std::move(result.asset.object));
    object.massTable = build_material_mass_table_from_definitions(
        result.asset.materials, &object.densityQuantumKilogramsPerCubicMeter);

    if (object.dynamic) {
        Float3 localComMeters{};
        const std::optional<RigidBodyCreateDesc> bodyDesc = build_dynamic_body_desc(
            *object.voxels, object.authoredTransform, object.voxelSizeMeters,
            object.massTable, object.densityQuantumKilogramsPerCubicMeter, object.structural,
            &localComMeters, error);
        if (!bodyDesc) return kInvalidGameObjectId;
        const RigidBodyHandle handle = physics_->create_body(*bodyDesc);
        if (handle == kInvalidRigidBodyHandle) {
            if (error) *error = "physics backend rejected body creation";
            return kInvalidGameObjectId;
        }
        object.hasBody = true;
        object.bodyHandle = handle;
        object.localCenterOfMassMeters = localComMeters;
    } else {
        const std::vector<VoxelBox> boxes = build_merged_object_box_proxy(*object.voxels);
        if (boxes.empty()) {
            if (error) *error = "asset has no collidable voxels";
            return kInvalidGameObjectId;
        }
        StaticRigidBodyCreateDesc staticDesc;
        staticDesc.transform = object.authoredTransform;
        staticDesc.collisionClass = RigidBodyCollisionClass::Full;
        staticDesc.boxes.reserve(boxes.size());
        for (const VoxelBox& box : boxes) staticDesc.boxes.push_back(voxel_box_to_solver_box(box, object.voxelSizeMeters));
        if (!validate_static_rigid_body_desc(staticDesc)) {
            if (error) *error = "constructed static body descriptor failed validation";
            return kInvalidGameObjectId;
        }
        const RigidBodyHandle handle = physics_->create_static_body(staticDesc);
        if (handle == kInvalidRigidBodyHandle) {
            if (error) *error = "physics backend rejected static body creation";
            return kInvalidGameObjectId;
        }
        object.hasBody = true;
        object.bodyHandle = handle;
    }

    const GameObjectId id = allocate_id();
    object.id = id;
    synchronize_membership_component(object);
    objects_.emplace(id, std::move(object));
    dispatch_lifecycle({GameLifecycleEventKind::Spawn, id});
    return id;
}


GameObjectId GameWorld::spawn_polygon_asset(
    const std::filesystem::path& path, std::string name, const RigidTransform& transform,
    bool dynamic, bool structural, std::string* error) {
    if (!geometry_kind_supported(GeometryKind::Polygon)) {
        if (error) *error = "this build profile does not enable polygon gameplay objects";
        return kInvalidGameObjectId;
    }
    PolygonAssetReadResult result = read_dmesh(path);
    if (!result) {
        if (error) *error = result.error.empty() ? "failed to read polygon asset" : result.error;
        return kInvalidGameObjectId;
    }
    Object object;
    object.name = name.empty() ? path.stem().string() : std::move(name);
    object.authoredTransform = transform;
    object.dynamic = dynamic;
    object.structural = structural;
    object.polygon = std::make_unique<CookedPolygonAsset>(std::move(result.asset));
    object.polygonBvh = std::make_unique<PolygonBvh>();
    if (!object.polygonBvh->build(*object.polygon, error)) return kInvalidGameObjectId;
    if (dynamic) {
        PolygonCollisionOptions options;
        options.densityKilogramsPerCubicMeter = polygon_density(*object.polygon);
        options.structural = structural;
        Float3 localCenter{};
        const auto desc = make_polygon_dynamic_body_desc(*object.polygon, transform, options, &localCenter);
        if (!desc) { if (error) *error = "could not build polygon collision proxy"; return kInvalidGameObjectId; }
        const RigidBodyHandle handle = physics_->create_body(*desc);
        if (handle == kInvalidRigidBodyHandle) { if (error) *error = "physics backend rejected polygon body"; return kInvalidGameObjectId; }
        object.hasBody = true; object.bodyHandle = handle; object.localCenterOfMassMeters = localCenter;
    } else {
        const auto desc = make_polygon_static_body_desc(*object.polygon, transform);
        if (!desc) { if (error) *error = "could not build polygon static collision proxy"; return kInvalidGameObjectId; }
        const RigidBodyHandle handle = physics_->create_static_body(*desc);
        if (handle == kInvalidRigidBodyHandle) { if (error) *error = "physics backend rejected polygon static body"; return kInvalidGameObjectId; }
        object.hasBody = true; object.bodyHandle = handle;
    }
    const GameObjectId id = allocate_id();
    object.id = id;
    synchronize_membership_component(object);
    objects_.emplace(id, std::move(object));
    dispatch_lifecycle({GameLifecycleEventKind::Spawn, id});
    return id;
}

std::optional<GameGeometryKind> GameWorld::geometry_kind(GameObjectId id) const noexcept {
    const auto it = objects_.find(id);
    if (it == objects_.end()) return std::nullopt;
    if (it->second.voxels) return GameGeometryKind::Voxel;
    if (it->second.polygon) return GameGeometryKind::Polygon;
#if defined(DVE_ENABLE_DEFORMABLE_RUNTIME)
    if (deformables_->contains(id)) return GameGeometryKind::Deformable;
#endif
    return GameGeometryKind::Marker;
}

bool GameWorld::destroy_object(GameObjectId id) {
    if (objectPools_.contains(id)) return release_to_pool(id, nullptr);
    return destroy_object_internal(id, true);
}

bool GameWorld::destroy_object_internal(GameObjectId id, bool dispatchDestroy) {
    const auto it = objects_.find(id);
    if (it == objects_.end()) return false;
    for (auto& [childId, child] : objects_) {
        if (!child.attachment || child.attachment->parent != id) continue;
        const RigidTransform world = resolve_transform(child);
        child.attachment.reset();
        child.authoredTransform = world;
        (void)synchronize_attached_body(child);
        (void)childId;
    }
    if (dispatchDestroy) {
        dispatch_lifecycle({GameLifecycleEventKind::Destroy, id});
        for (const DestroyListener& listener : destroyListeners_) listener(id);
    }
    if (gameplay_) gameplay_->remove_character(id);
    if (animationControllers_) (void)animationControllers_->unbind(id);
    if (controlRigs_) (void)controlRigs_->unbind(id);
    if (ragdolls_) (void)ragdolls_->unbind(id);
    if (animation_) (void)animation_->unbind(id);
#if defined(DVE_ENABLE_DEFORMABLE_RUNTIME)
    if (deformables_) (void)deformables_->unbind(id);
#endif
#if defined(DVE_ENABLE_CPU_HAIR)
    if (cpuHair_) (void)cpuHair_->unbind(id);
#endif
    if (it->second.hasBody) physics_->destroy_body(it->second.bodyHandle);
    objects_.erase(it);
    return true;
}


std::vector<GameObjectId> GameWorld::object_ids() const {
    std::vector<GameObjectId> ids;
    ids.reserve(objects_.size());
    for (const auto& [id, object] : objects_) {
        (void)object;
        ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

bool GameWorld::has_object(GameObjectId id) const { return objects_.contains(id); }

std::optional<GameObjectId> GameWorld::find_by_name(std::string_view name) const {
    for (const auto& [id, object] : objects_) {
        if (object.name == name) return id;
    }
    return std::nullopt;
}

std::vector<GameObjectId> GameWorld::find_by_tag(std::string_view tag) const {
    std::vector<GameObjectId> result;
    for (const auto& [id, object] : objects_) {
        if (object.enabled && std::binary_search(object.tags.begin(), object.tags.end(), std::string(tag))) result.push_back(id);
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<GameObjectId> GameWorld::find_by_group(std::string_view group) const {
    std::vector<GameObjectId> result;
    for (const auto& [id, object] : objects_) {
        if (object.enabled && std::binary_search(object.groups.begin(), object.groups.end(), std::string(group))) result.push_back(id);
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<GameObjectId> GameWorld::find_by_layer(std::uint32_t layer) const {
    std::vector<GameObjectId> result;
    for (const auto& [id, object] : objects_) if (object.enabled && object.layer == layer) result.push_back(id);
    std::sort(result.begin(), result.end());
    return result;
}

bool GameWorld::is_enabled(GameObjectId id) const noexcept {
    const auto it = objects_.find(id);
    return it != objects_.end() && it->second.enabled;
}

bool GameWorld::set_enabled(GameObjectId id, bool enabled) {
    const auto it = objects_.find(id);
    if (it == objects_.end()) return false;
    if (it->second.enabled == enabled) return true;
    it->second.enabled = enabled;
#if defined(DVE_ENABLE_DEFORMABLE_RUNTIME)
    if (deformables_->contains(id)) (void)deformables_->set_running(id, enabled);
#endif
#if defined(DVE_ENABLE_CPU_HAIR)
    if (cpuHair_->contains(id)) (void)cpuHair_->set_running(id, enabled);
#endif
    dispatch_lifecycle({enabled ? GameLifecycleEventKind::Enable : GameLifecycleEventKind::Disable, id});
    return true;
}

std::vector<GameObjectId> GameWorld::find_by_component(std::string_view type) const {
    std::vector<GameObjectId> result;
    for (const auto& [id, object] : objects_) {
        if (object.enabled && find_component_by_type(std::span<const Component>(object.components), type)) result.push_back(id);
    }
    std::sort(result.begin(), result.end());
    return result;
}

const std::vector<Component>* GameWorld::components(GameObjectId id) const noexcept {
    const auto it = objects_.find(id);
    return it == objects_.end() ? nullptr : &it->second.components;
}

Component* GameWorld::add_component(GameObjectId id, Component componentValue, std::string* error) {
    const auto it = objects_.find(id);
    if (it == objects_.end()) { if (error) *error = "game object does not exist"; return nullptr; }
    Object& object = it->second;
    if (componentValue.id == kInvalidComponentId) {
        componentValue.id = 1U;
        for (const Component& existing : object.components)
            componentValue.id = std::max(componentValue.id, existing.id + 1U);
    }
    if (dve::find_component(std::span<const Component>(object.components), componentValue.id)) {
        if (error) *error = "component id already exists on game object";
        return nullptr;
    }
    std::string validationError;
    if (!validate_components(std::span<const Component>(&componentValue, 1U), nullptr, &validationError)) {
        if (error) *error = std::move(validationError);
        return nullptr;
    }
    object.components.push_back(std::move(componentValue));
    Component& added = object.components.back();
    if (added.type == "dve.membership") {
        if (const auto propertyIt = added.properties.find("tags"); propertyIt != added.properties.end())
            if (const auto* value = std::get_if<std::string>(&propertyIt->second)) object.tags = parse_membership_values(*value);
        if (const auto propertyIt = added.properties.find("groups"); propertyIt != added.properties.end())
            if (const auto* value = std::get_if<std::string>(&propertyIt->second)) object.groups = parse_membership_values(*value);
        if (const auto propertyIt = added.properties.find("layer"); propertyIt != added.properties.end())
            if (const auto* value = std::get_if<std::int64_t>(&propertyIt->second)) object.layer = static_cast<std::uint32_t>(std::clamp<std::int64_t>(*value, 0, 63));
        synchronize_membership_component(object);
    }
    dispatch_lifecycle({GameLifecycleEventKind::ComponentAdded, id, kInvalidGameObjectId, added.id, added.type});
    return &added;
}

bool GameWorld::remove_component(GameObjectId id, ComponentId componentId) {
    const auto it = objects_.find(id);
    if (it == objects_.end()) return false;
    const auto found = std::find_if(it->second.components.begin(), it->second.components.end(),
                                    [componentId](const Component& component) { return component.id == componentId; });
    if (found == it->second.components.end()) return false;
    const std::string type = found->type;
    if (type == "dve.membership") return false; // first-class membership cannot disappear.
    it->second.components.erase(found);
    dispatch_lifecycle({GameLifecycleEventKind::ComponentRemoved, id, kInvalidGameObjectId, componentId, type});
    return true;
}

bool GameWorld::set_component_property(
    GameObjectId id, ComponentId componentId, std::string property,
    ComponentValue value, std::string* error) {
    const auto it = objects_.find(id);
    if (it == objects_.end()) { if (error) *error = "game object does not exist"; return false; }
    Component* componentValue = dve::find_component(std::span<Component>(it->second.components), componentId);
    if (!componentValue) { if (error) *error = "component does not exist"; return false; }
    if (!component_property_name_is_valid(property) || !component_value_is_finite(value)) {
        if (error) *error = "invalid component property";
        return false;
    }
    const std::string propertyName = property;
    componentValue->properties[std::move(property)] = std::move(value);
    if (componentValue->type == "dve.membership") {
        if (propertyName == "tags") {
            if (const auto* typed = std::get_if<std::string>(&componentValue->properties[propertyName])) it->second.tags = parse_membership_values(*typed);
        } else if (propertyName == "groups") {
            if (const auto* typed = std::get_if<std::string>(&componentValue->properties[propertyName])) it->second.groups = parse_membership_values(*typed);
        } else if (propertyName == "layer") {
            if (const auto* typed = std::get_if<std::int64_t>(&componentValue->properties[propertyName]))
                it->second.layer = static_cast<std::uint32_t>(std::clamp<std::int64_t>(*typed, 0, 63));
        }
        synchronize_membership_component(it->second);
    }
    return true;
}

bool GameWorld::set_component_enabled(GameObjectId id, ComponentId componentId, bool enabled) {
    const auto it = objects_.find(id);
    if (it == objects_.end()) return false;
    Component* componentValue = dve::find_component(std::span<Component>(it->second.components), componentId);
    if (!componentValue) return false;
    if (componentValue->enabled == enabled) return true;
    componentValue->enabled = enabled;
    dispatch_lifecycle({enabled ? GameLifecycleEventKind::ComponentEnabled : GameLifecycleEventKind::ComponentDisabled,
                        id, kInvalidGameObjectId, componentId, componentValue->type});
    return true;
}

const Component* GameWorld::component(GameObjectId id, ComponentId componentId) const noexcept {
    const auto it = objects_.find(id);
    return it == objects_.end()
        ? nullptr
        : dve::find_component(std::span<const Component>(it->second.components), componentId);
}

bool GameWorld::attach_object(
    GameObjectId childId, GameObjectId parentId, bool preserveWorldTransform,
    std::string socket, bool inheritPosition, bool inheritRotation, std::string* error) {
    const auto fail = [&](std::string text) { if (error) *error = std::move(text); return false; };
    auto childIt = objects_.find(childId);
    auto parentIt = objects_.find(parentId);
    if (childIt == objects_.end() || parentIt == objects_.end() || childId == parentId)
        return fail("invalid game-object attachment endpoints");
#if defined(DVE_ENABLE_DEFORMABLE_RUNTIME)
    if (deformables_->contains(childId))
        return fail("a live deformable owner cannot be attached; move its pinned vertices instead");
#endif
    if (childIt->second.hasBody && !childIt->second.dynamic)
        return fail("static collision bodies cannot be attached after creation");
    for (GameObjectId cursor = parentId; cursor != kInvalidGameObjectId;) {
        if (cursor == childId) return fail("attachment would create a cycle");
        const auto it = objects_.find(cursor);
        if (it == objects_.end() || !it->second.attachment) break;
        cursor = it->second.attachment->parent;
    }
    const RigidTransform childWorld = resolve_transform(childIt->second);
    GameObjectAttachment attachment;
    attachment.parent = parentId;
    attachment.socket = std::move(socket);
    attachment.inheritPosition = inheritPosition;
    attachment.inheritRotation = inheritRotation;
    if (!attachment.socket.empty() && animation_ && animation_->has_instance(parentId) &&
        !animation_->socket_world_transform(
            parentId, attachment.socket, resolve_transform(parentIt->second)))
        return fail("skeletal socket does not exist on the attached parent");
    const RigidTransform parentWorld = resolve_attachment_frame(attachment)
        .value_or(resolve_transform(parentIt->second));
    attachment.localTransform = preserveWorldTransform
        ? game_attachment_local_from_world(parentWorld, childWorld, inheritPosition, inheritRotation)
        : childIt->second.authoredTransform;
    childIt->second.attachment = std::move(attachment);
    return synchronize_attached_body(childIt->second);
}

bool GameWorld::detach_object(GameObjectId childId, bool preserveWorldTransform, std::string* error) {
    const auto it = objects_.find(childId);
    if (it == objects_.end()) { if (error) *error = "game object does not exist"; return false; }
    if (!it->second.attachment) return true;
    const RigidTransform world = resolve_transform(it->second);
    it->second.attachment.reset();
    if (preserveWorldTransform) it->second.authoredTransform = world;
    return synchronize_attached_body(it->second);
}

std::optional<GameObjectId> GameWorld::parent_of(GameObjectId child) const noexcept {
    const auto it = objects_.find(child);
    if (it == objects_.end() || !it->second.attachment) return std::nullopt;
    return it->second.attachment->parent;
}

std::vector<GameObjectId> GameWorld::children_of(GameObjectId parent) const {
    std::vector<GameObjectId> result;
    for (const auto& [id, object] : objects_)
        if (object.attachment && object.attachment->parent == parent) result.push_back(id);
    std::sort(result.begin(), result.end());
    return result;
}

std::optional<RigidTransform> GameWorld::local_transform(GameObjectId child) const noexcept {
    const auto it = objects_.find(child);
    if (it == objects_.end() || !it->second.attachment) return std::nullopt;
    return it->second.attachment->localTransform;
}

namespace {
const std::string kEmptyName;
}

const std::string& GameWorld::name_of(GameObjectId id) const {
    const auto it = objects_.find(id);
    return it == objects_.end() ? kEmptyName : it->second.name;
}

bool GameWorld::has_tag(GameObjectId id, std::string_view tag) const {
    const auto it = objects_.find(id);
    if (it == objects_.end()) return false;
    return std::find(it->second.tags.begin(), it->second.tags.end(), tag) != it->second.tags.end();
}

std::optional<std::uint64_t> GameWorld::voxel_count(GameObjectId id) const {
    const auto it = objects_.find(id);
    if (it == objects_.end() || !it->second.voxels) return std::nullopt;
    return it->second.voxels->occupied_voxel_count();
}

std::optional<GameVoxelBrickSnapshot> GameWorld::voxel_brick_snapshot(
    GameObjectId id, BrickKey brick) const {
    const auto it = objects_.find(id);
    if (it == objects_.end() || !it->second.voxels) return std::nullopt;
    const VoxelBrickSnapshot source = it->second.voxels->snapshot_brick(brick);
    return GameVoxelBrickSnapshot{source.key, source.generation, source.materials, source.contentHash};
}

bool GameWorld::replace_voxel_brick(
    GameObjectId id, const GameVoxelBrickSnapshot& snapshot, std::string* error) {
    const auto it = objects_.find(id);
    if (it == objects_.end() || !it->second.voxels) {
        if (error != nullptr) *error = "brick repair target is not a live voxel object";
        return false;
    }
    Object& object = it->second;
    const VoxelBrickSnapshot before = object.voxels->snapshot_brick(snapshot.brick);
    const VoxelBrickSnapshot replacement{
        snapshot.brick, snapshot.revision, snapshot.materials, snapshot.contentHash};
    if (!object.voxels->replace_brick(replacement, error)) return false;
    if (object.voxels->occupied_voxel_count() == 0U) {
        (void)object.voxels->replace_brick(before);
        if (error != nullptr) *error = "brick repair cannot remove the final voxel; replicate object despawn instead";
        return false;
    }

    const RigidTransform currentTransform = resolve_transform(object);
    const std::optional<RigidBodyState> previousState =
        object.hasBody && object.dynamic ? physics_->state(object.bodyHandle) : std::nullopt;
    RigidBodyHandle replacementHandle = kInvalidRigidBodyHandle;
    Float3 replacementLocalCenterOfMass{};

    if (object.dynamic) {
        std::string buildError;
        auto bodyDesc = build_dynamic_body_desc(
            *object.voxels, currentTransform, object.voxelSizeMeters, object.massTable,
            object.densityQuantumKilogramsPerCubicMeter, object.structural,
            &replacementLocalCenterOfMass, &buildError);
        if (bodyDesc && previousState) {
            bodyDesc->linearVelocity = previousState->linearVelocity;
            bodyDesc->angularVelocity = previousState->angularVelocity;
        }
        if (bodyDesc) replacementHandle = physics_->create_body(*bodyDesc);
        if (!bodyDesc || replacementHandle == kInvalidRigidBodyHandle) {
            (void)object.voxels->replace_brick(before);
            if (error != nullptr) *error = buildError.empty()
                ? "physics backend rejected repaired dynamic voxel collision"
                : buildError;
            return false;
        }
    } else {
        const std::vector<VoxelBox> boxes = build_merged_object_box_proxy(*object.voxels);
        StaticRigidBodyCreateDesc staticDesc;
        staticDesc.transform = currentTransform;
        staticDesc.collisionClass = RigidBodyCollisionClass::Full;
        staticDesc.boxes.reserve(boxes.size());
        for (const VoxelBox& box : boxes)
            staticDesc.boxes.push_back(voxel_box_to_solver_box(box, object.voxelSizeMeters));
        if (boxes.empty() || !validate_static_rigid_body_desc(staticDesc)) {
            (void)object.voxels->replace_brick(before);
            if (error != nullptr) *error = "repaired static voxel collision is invalid";
            return false;
        }
        replacementHandle = physics_->create_static_body(staticDesc);
        if (replacementHandle == kInvalidRigidBodyHandle) {
            (void)object.voxels->replace_brick(before);
            if (error != nullptr) *error = "physics backend rejected repaired static voxel collision";
            return false;
        }
    }

    const RigidBodyHandle oldHandle = object.bodyHandle;
    const bool hadBody = object.hasBody;
    object.authoredTransform = currentTransform;
    object.hasBody = true;
    object.bodyHandle = replacementHandle;
    object.localCenterOfMassMeters = replacementLocalCenterOfMass;
    if (hadBody) physics_->destroy_body(oldHandle);
    return true;
}

RigidTransform GameWorld::resolve_base_transform(const Object& object) const {
    if (!object.hasBody || !object.dynamic) return object.authoredTransform;
    const std::optional<RigidBodyState> state = physics_->state(object.bodyHandle);
    if (!state) return object.authoredTransform;
    const Float3 worldOffset = rotate(state->currentTransform.rotation, object.localCenterOfMassMeters);
    return make_rigid_transform(subtract(state->currentTransform.position, worldOffset), state->currentTransform.rotation);
}

std::optional<RigidTransform> GameWorld::resolve_attachment_frame(
    const GameObjectAttachment& attachment) const {
    const auto parent = objects_.find(attachment.parent);
    if (parent == objects_.end()) return std::nullopt;
    const RigidTransform parentWorld = resolve_transform(parent->second);
    if (!attachment.socket.empty() && animation_) {
        if (const auto socket = animation_->socket_world_transform(
                parent->second.id, attachment.socket, parentWorld)) return socket;
    }
    return parentWorld;
}

RigidTransform GameWorld::resolve_transform(const Object& object) const {
    if (!object.attachment) return resolve_base_transform(object);
    const auto frame = resolve_attachment_frame(*object.attachment);
    if (!frame) return resolve_base_transform(object);
    return compose_game_attachment(*frame, *object.attachment);
}

bool GameWorld::synchronize_attached_body(Object& object) {
    if (!object.hasBody || !object.dynamic) return true;
    const RigidTransform world = resolve_transform(object);
    RigidBodyState state;
    state.currentTransform = make_rigid_transform(
        add(world.position, rotate(world.rotation, object.localCenterOfMassMeters)), world.rotation);
    state.previousTransform = state.currentTransform;
    if (const auto existing = physics_->state(object.bodyHandle)) {
        state.linearVelocity = existing->linearVelocity;
        state.angularVelocity = existing->angularVelocity;
        state.sleeping = existing->sleeping;
    }
    return physics_->set_state(object.bodyHandle, state);
}

void GameWorld::synchronize_attached_bodies() {
    for (auto& [id, object] : objects_) {
        (void)id;
        if (object.attachment) (void)synchronize_attached_body(object);
    }
}

std::optional<RigidTransform> GameWorld::transform(GameObjectId id) const {
    const auto it = objects_.find(id);
    if (it == objects_.end()) return std::nullopt;
    return resolve_transform(it->second);
}

std::optional<Float3> GameWorld::position(GameObjectId id) const {
    const auto value = transform(id);
    if (!value) return std::nullopt;
    return value->position;
}

bool GameWorld::set_position(GameObjectId id, Float3 worldPosition) {
    const auto it = objects_.find(id);
    if (it == objects_.end()) return false;
    Object& object = it->second;
#if defined(DVE_ENABLE_DEFORMABLE_RUNTIME)
    if (deformables_->contains(id)) return false;
#endif
    if (object.hasBody && !object.dynamic) return false; // static body: immovable after creation
    if (object.attachment) {
        const auto parentFrame = resolve_attachment_frame(*object.attachment);
        if (!parentFrame) return false;
        RigidTransform world = resolve_transform(object);
        world.position = worldPosition;
        object.attachment->localTransform = game_attachment_local_from_world(
            *parentFrame, world,
            object.attachment->inheritPosition, object.attachment->inheritRotation);
        return synchronize_attached_body(object);
    }
    if (object.hasBody) {
        const RigidTransform current = resolve_transform(object);
        RigidBodyState state;
        state.currentTransform = make_rigid_transform(
            add(worldPosition, rotate(current.rotation, object.localCenterOfMassMeters)), current.rotation);
        state.previousTransform = state.currentTransform;
        const auto existing = physics_->state(object.bodyHandle);
        if (existing) { state.linearVelocity = existing->linearVelocity; state.angularVelocity = existing->angularVelocity; }
        return physics_->set_state(object.bodyHandle, state);
    }
    object.authoredTransform.position = worldPosition;
    return true;
}

bool GameWorld::set_rotation(GameObjectId id, Quaternion worldRotation) {
    const auto it = objects_.find(id);
    if (it == objects_.end()) return false;
    Object& object = it->second;
#if defined(DVE_ENABLE_DEFORMABLE_RUNTIME)
    if (deformables_->contains(id)) return false;
#endif
    if (object.hasBody && !object.dynamic) return false;
    if (object.attachment) {
        const auto parentFrame = resolve_attachment_frame(*object.attachment);
        if (!parentFrame) return false;
        RigidTransform world = resolve_transform(object);
        world.rotation = worldRotation;
        object.attachment->localTransform = game_attachment_local_from_world(
            *parentFrame, world,
            object.attachment->inheritPosition, object.attachment->inheritRotation);
        return synchronize_attached_body(object);
    }
    if (object.hasBody) {
        const RigidTransform current = resolve_transform(object);
        RigidBodyState state;
        state.currentTransform = make_rigid_transform(
            add(current.position, rotate(worldRotation, object.localCenterOfMassMeters)), worldRotation);
        state.previousTransform = state.currentTransform;
        const auto existing = physics_->state(object.bodyHandle);
        if (existing) { state.linearVelocity = existing->linearVelocity; state.angularVelocity = existing->angularVelocity; }
        return physics_->set_state(object.bodyHandle, state);
    }
    object.authoredTransform.rotation = worldRotation;
    return true;
}

std::optional<Float3> GameWorld::linear_velocity(GameObjectId id) const {
    const auto it = objects_.find(id);
    if (it == objects_.end() || !it->second.hasBody || !it->second.dynamic) return std::nullopt;
    const auto state = physics_->state(it->second.bodyHandle);
    if (!state) return std::nullopt;
    return state->linearVelocity;
}

bool GameWorld::set_linear_velocity(GameObjectId id, Float3 velocity) {
    const auto it = objects_.find(id);
    if (it == objects_.end() || !it->second.hasBody || !it->second.dynamic) return false;
    const auto existing = physics_->state(it->second.bodyHandle);
    if (!existing) return false;
    RigidBodyState state = *existing;
    state.linearVelocity = velocity;
    return physics_->set_state(it->second.bodyHandle, state);
}

bool GameWorld::apply_impulse(GameObjectId id, Float3 worldImpulse) {
    const auto it = objects_.find(id);
#if defined(DVE_ENABLE_DEFORMABLE_RUNTIME)
    if (it != objects_.end() && deformables_->contains(id))
        return deformables_->apply_impulse(id, worldImpulse);
#endif
    if (it == objects_.end() || !it->second.hasBody || !it->second.dynamic || it->second.attachment) return false;
    return physics_->apply_impulse(it->second.bodyHandle, worldImpulse);
}

bool GameWorld::apply_force(GameObjectId id, Float3 worldForce) {
    const auto it = objects_.find(id);
    if (it == objects_.end() || !it->second.hasBody || !it->second.dynamic || it->second.attachment) return false;
    return physics_->apply_force(it->second.bodyHandle, worldForce);
}

bool GameWorld::bind_ragdoll(
    GameObjectId id, RagdollDefinition definition, RagdollRuntimeConfig config,
    std::string* error) {
    const auto found = objects_.find(id);
    if (found == objects_.end()) {
        if (error) *error = "ragdoll object does not exist";
        return false;
    }
    if (found->second.hasBody || found->second.attachment) {
        if (error) *error = "ragdoll actors must be unattached marker objects without a primary rigid body";
        return false;
    }
    return ragdolls_->bind(id, std::move(definition), config, error);
}

bool GameWorld::activate_ragdoll(
    GameObjectId id, RagdollActivationOptions options, std::string* error) {
    const auto world = transform(id);
    if (!world) {
        if (error) *error = "ragdoll object does not exist";
        return false;
    }
    return ragdolls_->activate(id, *world, options, error);
}

bool GameWorld::recover_ragdoll(
    GameObjectId id, const RagdollRecoveryOptions& options, std::string* error) {
    const auto found = objects_.find(id);
    if (found == objects_.end() || found->second.hasBody || found->second.attachment) {
        if (error) *error = "ragdoll recovery requires an unattached marker object";
        return false;
    }
    const RigidTransform current = found->second.authoredTransform;
    RigidTransform aligned;
    if (!ragdolls_->begin_recovery(id, current, options, &aligned, error)) return false;
    found->second.authoredTransform = aligned;
    return true;
}

#if defined(DVE_ENABLE_DEFORMABLE_RUNTIME)
bool GameWorld::bind_deformable(
    GameObjectId id, SoftBodyAsset asset, DeformableBindOptions options,
    std::string* error) {
    const auto found = objects_.find(id);
    if (found == objects_.end()) {
        if (error) *error = "deformable owner does not exist";
        return false;
    }
    if (found->second.hasBody || found->second.attachment || found->second.voxels || found->second.polygon) {
        if (error) *error = "deformable owners must be unattached marker objects without rigid geometry";
        return false;
    }
    if (!deformables_->bind(id, std::move(asset), found->second.authoredTransform, options, error)) return false;
    if (!found->second.enabled) (void)deformables_->set_running(id, false);
    return true;
}

bool GameWorld::bind_deformable_asset(
    GameObjectId id, const std::filesystem::path& path,
    DeformableBindOptions options, std::string* error) {
    const auto found = objects_.find(id);
    if (found == objects_.end()) {
        if (error) *error = "deformable owner does not exist";
        return false;
    }
    if (found->second.hasBody || found->second.attachment || found->second.voxels || found->second.polygon) {
        if (error) *error = "deformable owners must be unattached marker objects without rigid geometry";
        return false;
    }
    if (!deformables_->bind_asset(id, path, found->second.authoredTransform, options, error)) return false;
    if (!found->second.enabled) (void)deformables_->set_running(id, false);
    return true;
}

bool GameWorld::unbind_deformable(GameObjectId id) noexcept {
    return deformables_->unbind(id);
}

bool GameWorld::apply_deformable_impulse(GameObjectId id, Float3 impulse) noexcept {
    return deformables_->apply_impulse(id, impulse);
}

bool GameWorld::apply_deformable_radial_impulse(
    GameObjectId id, Float3 center, float radius, float strength) noexcept {
    return deformables_->apply_radial_impulse(id, center, radius, strength);
}

bool GameWorld::set_deformable_vertex_target(
    GameObjectId id, std::uint32_t vertex, Float3 position, bool clearVelocity) noexcept {
    return deformables_->set_vertex_target(id, vertex, position, clearVelocity);
}
#endif

#if defined(DVE_ENABLE_CPU_HAIR)
bool GameWorld::bind_cpu_hair(
    GameObjectId id, HairAsset asset, CpuHairBindOptions options, std::string* error) {
    const auto found = objects_.find(id);
    if (found == objects_.end()) {
        if (error) *error = "CPU hair owner does not exist";
        return false;
    }
    if (!cpuHair_->bind(id, std::move(asset), resolve_transform(found->second), options, error))
        return false;
    if (!found->second.enabled) (void)cpuHair_->set_running(id, false);
    return true;
}

bool GameWorld::bind_cpu_hair_asset(
    GameObjectId id, const std::filesystem::path& path,
    CpuHairBindOptions options, std::string* error) {
    const auto found = objects_.find(id);
    if (found == objects_.end()) {
        if (error) *error = "CPU hair owner does not exist";
        return false;
    }
    if (!cpuHair_->bind_asset(id, path, resolve_transform(found->second), options, error))
        return false;
    if (!found->second.enabled) (void)cpuHair_->set_running(id, false);
    return true;
}

bool GameWorld::unbind_cpu_hair(GameObjectId id) noexcept {
    return cpuHair_->unbind(id);
}

bool GameWorld::set_cpu_hair_root_targets(
    GameObjectId id, std::span<const HairRootTarget> targets, bool teleport) noexcept {
    return cpuHair_->set_root_targets(id, targets, teleport);
}

bool GameWorld::clear_cpu_hair_root_targets(GameObjectId id) noexcept {
    return cpuHair_->clear_root_targets(id);
}

bool GameWorld::set_cpu_hair_wind(GameObjectId id, Float3 windVelocity) noexcept {
    return cpuHair_->set_wind(id, windVelocity);
}

bool GameWorld::set_cpu_hair_collision(GameObjectId id, HairCollisionSet collision) {
    return cpuHair_->set_collision(id, std::move(collision));
}

bool GameWorld::apply_cpu_hair_impulse(GameObjectId id, Float3 impulse) noexcept {
    return cpuHair_->apply_impulse(id, impulse);
}
#endif

std::optional<std::uint64_t> GameWorld::damage_sphere(GameObjectId id, Float3 worldCenter, float radius) {
    const auto it = objects_.find(id);
    if (it == objects_.end() || !it->second.voxels) return std::nullopt;
    Object& object = it->second;
    if (!(radius > 0.0F) || !std::isfinite(radius)) return std::nullopt;

    const RigidTransform current = resolve_transform(object);
    const Float3 localMeters = inverse_transform_point(current, worldCenter);
    const Float3 localVoxelUnits = multiply(localMeters, 1.0F / object.voxelSizeMeters);
    const float radiusVoxelUnits = radius / object.voxelSizeMeters;

    const SphereDamageCommand command{localVoxelUnits, radiusVoxelUnits, 0};
    const DamageApplyReport report = apply_damage_commands(*object.voxels, {command});
    if (report.removedVoxelCount == 0) return 0U;

    const bool destroyed = object.voxels->occupied_voxel_count() == 0;
    std::vector<GameObjectId> newFragmentIds;
    if (!destroyed) newFragmentIds = fragment_after_damage(id, object);

    GameDamageEvent event{id, worldCenter, radius, report.removedVoxelCount, destroyed, std::move(newFragmentIds)};
    for (const DamageListener& listener : damageListeners_) listener(event);

    if (destroyed) (void)destroy_object(id);
    return report.removedVoxelCount;
}

std::vector<GameObjectId> GameWorld::fragment_after_damage(GameObjectId id, Object& object) {
    (void)id;
    std::vector<GameObjectId> fragmentIds;
    if (!object.voxels || object.voxels->occupied_voxel_count() == 0) return fragmentIds;

    const ConnectivitySnapshot snapshot = build_connectivity_snapshot(*object.voxels);
    if (snapshot.components.size() <= 1) return fragmentIds; // still one piece: nothing to do
    if (snapshot.components.size() > kMaxFragmentsPerDamageCall) return fragmentIds; // safety cap

    // Keep the largest component as the original object (rebuilt in place, same id); split
    // every other component off into its own new dynamic GameObject.
    std::size_t primaryIndex = 0;
    for (std::size_t i = 1; i < snapshot.components.size(); ++i) {
        if (snapshot.components[i].voxelCount > snapshot.components[primaryIndex].voxelCount) primaryIndex = i;
    }

    // Captured before any body is touched: a static or bodyless parent has no velocity to
    // inherit, so debris breaking off it simply starts at rest (gravity takes over next
    // tick), which is physically reasonable.
    std::optional<RigidBodyState> parentState;
    if (object.hasBody && object.dynamic) parentState = physics_->state(object.bodyHandle);

    for (std::size_t componentIndex = 0; componentIndex < snapshot.components.size(); ++componentIndex) {
        if (componentIndex == primaryIndex) continue;
        const auto plan = build_split_plan(*object.voxels, snapshot, componentIndex);
        if (!plan) continue; // stale brick generation or similar transient mismatch: skip defensively
        // commit_split_plan removes these voxels from object.voxels in place and returns a
        // fresh VoxelObject containing just them, still in the *same* voxel coordinate space
        // (not re-based to a local origin), so the fragment can keep the parent's exact
        // authoredTransform unchanged and still be geometrically correct.
        auto detached = commit_split_plan(*object.voxels, *plan, allocate_id());
        if (!detached) continue;

        auto fragmentVoxels = std::make_unique<VoxelObject>(std::move(*detached));
        Float3 fragmentLocalCom{};
        std::string buildError;
        std::optional<RigidBodyCreateDesc> fragmentDesc = build_dynamic_body_desc(
            *fragmentVoxels, object.authoredTransform, object.voxelSizeMeters,
            object.massTable, object.densityQuantumKilogramsPerCubicMeter, object.structural,
            &fragmentLocalCom, &buildError);
        if (!fragmentDesc) continue; // e.g. a sliver too small/odd to form a valid body; those voxels are simply lost

        if (parentState) {
            fragmentDesc->linearVelocity = inherited_child_center_of_mass_velocity(
                parentState->linearVelocity, parentState->angularVelocity,
                parentState->currentTransform.position, fragmentDesc->transform.position);
            fragmentDesc->angularVelocity = parentState->angularVelocity;
        }

        const RigidBodyHandle fragmentHandle = physics_->create_body(*fragmentDesc);
        if (fragmentHandle == kInvalidRigidBodyHandle) continue;

        Object fragmentObject;
        fragmentObject.name = object.name + "_fragment";
        fragmentObject.tags = object.tags;
        fragmentObject.groups = object.groups;
        fragmentObject.layer = object.layer;
        fragmentObject.components = object.components;
        fragmentObject.authoredTransform = object.authoredTransform;
        fragmentObject.voxelSizeMeters = object.voxelSizeMeters;
        fragmentObject.voxels = std::move(fragmentVoxels);
        fragmentObject.dynamic = true; // debris always falls, even when split from a static parent
        fragmentObject.structural = false; // detached debris is not structural by definition
        fragmentObject.densityQuantumKilogramsPerCubicMeter = object.densityQuantumKilogramsPerCubicMeter;
        fragmentObject.massTable = object.massTable;
        fragmentObject.hasBody = true;
        fragmentObject.bodyHandle = fragmentHandle;
        fragmentObject.localCenterOfMassMeters = fragmentLocalCom;

        const GameObjectId fragmentId = allocate_id();
        fragmentObject.id = fragmentId;
        synchronize_membership_component(fragmentObject);
        objects_.emplace(fragmentId, std::move(fragmentObject));
        dispatch_lifecycle({GameLifecycleEventKind::Spawn, fragmentId});
        fragmentIds.push_back(fragmentId);
    }

    // The primary component lost mass, and its center of mass may have shifted: rebuild its
    // body from the now-reduced voxel data rather than leaving the old, wrong-shaped body in
    // place with stale mass/inertia.
    if (object.hasBody) {
        physics_->destroy_body(object.bodyHandle);
        object.hasBody = false;
    }
    if (object.voxels->occupied_voxel_count() > 0) {
        if (object.dynamic) {
            Float3 localCom{};
            std::string buildError;
            std::optional<RigidBodyCreateDesc> primaryDesc = build_dynamic_body_desc(
                *object.voxels, object.authoredTransform, object.voxelSizeMeters,
                object.massTable, object.densityQuantumKilogramsPerCubicMeter, object.structural,
                &localCom, &buildError);
            if (primaryDesc) {
                if (parentState) {
                    primaryDesc->linearVelocity = inherited_child_center_of_mass_velocity(
                        parentState->linearVelocity, parentState->angularVelocity,
                        parentState->currentTransform.position, primaryDesc->transform.position);
                    primaryDesc->angularVelocity = parentState->angularVelocity;
                }
                const RigidBodyHandle handle = physics_->create_body(*primaryDesc);
                if (handle != kInvalidRigidBodyHandle) {
                    object.hasBody = true;
                    object.bodyHandle = handle;
                    object.localCenterOfMassMeters = localCom;
                }
            }
        } else {
            const std::vector<VoxelBox> boxes = build_merged_object_box_proxy(*object.voxels);
            if (!boxes.empty()) {
                StaticRigidBodyCreateDesc staticDesc;
                staticDesc.transform = object.authoredTransform;
                staticDesc.collisionClass = RigidBodyCollisionClass::Full;
                staticDesc.boxes.reserve(boxes.size());
                for (const VoxelBox& box : boxes)
                    staticDesc.boxes.push_back(voxel_box_to_solver_box(box, object.voxelSizeMeters));
                if (validate_static_rigid_body_desc(staticDesc)) {
                    const RigidBodyHandle handle = physics_->create_static_body(staticDesc);
                    if (handle != kInvalidRigidBodyHandle) {
                        object.hasBody = true;
                        object.bodyHandle = handle;
                    }
                }
            }
        }
    }

    return fragmentIds;
}

std::optional<GameRaycastHit> GameWorld::raycast(Float3 worldOrigin, Float3 worldDirection, float maxDistance) const {
    if (!(maxDistance > 0.0F) || !std::isfinite(maxDistance) ||
        !(length_squared(worldDirection) > 1.0e-12F)) return std::nullopt;
    std::optional<GameRaycastHit> best;
#if defined(DVE_ENABLE_DEFORMABLE_RUNTIME)
    const Float3 normalizedDirection = normalize(worldDirection);
#endif
    for (const auto& [id, object] : objects_) {
#if defined(DVE_ENABLE_DEFORMABLE_RUNTIME)
        if (const DeformableCollisionProxy* proxy = deformables_->collision_proxy(id)) {
            std::optional<DeformableRayHit> deformableHit;
            for (const auto& triangle : proxy->surfaceTriangles) {
                if (triangle[0] >= proxy->particles.size() || triangle[1] >= proxy->particles.size() ||
                    triangle[2] >= proxy->particles.size()) continue;
                const auto hit = ray_triangle_deformable(
                    worldOrigin, normalizedDirection, proxy->particles[triangle[0]],
                    proxy->particles[triangle[1]], proxy->particles[triangle[2]], maxDistance);
                if (hit && (!deformableHit || hit->distance < deformableHit->distance)) deformableHit = hit;
            }
            if (proxy->surfaceTriangles.empty()) {
                for (std::size_t vertex = 0U; vertex < proxy->particles.size(); ++vertex) {
                    const float particleRadius = vertex < proxy->radii.size() ? proxy->radii[vertex] : 0.01F;
                    const auto hit = ray_sphere_deformable(
                        worldOrigin, normalizedDirection, proxy->particles[vertex], particleRadius, maxDistance);
                    if (hit && (!deformableHit || hit->distance < deformableHit->distance)) deformableHit = hit;
                }
            }
            if (deformableHit && (!best || deformableHit->distance < best->distance))
                best = GameRaycastHit{id, add(worldOrigin, multiply(normalizedDirection, deformableHit->distance)),
                                      deformableHit->normal, deformableHit->distance, 0U};
        }
#endif
        const RigidTransform current = resolve_transform(object);
        if (object.polygon && object.polygonBvh) {
            const Float3 localOrigin = inverse_transform_point(current, worldOrigin);
            const Float3 localDirection = normalize(inverse_transform_vector(current, worldDirection));
            const auto polygonHit = object.polygonBvh->raycast(localOrigin, localDirection, maxDistance);
            if (polygonHit && (!best || polygonHit->distance < best->distance)) {
                best = GameRaycastHit{id, transform_point(current, polygonHit->position),
                                      transform_vector(current, polygonHit->normal), polygonHit->distance,
                                      static_cast<MaterialId>(std::min<std::uint32_t>(polygonHit->materialIndex, 255U))};
            }
        }
        if (!object.voxels) continue;
        // raycast_voxels_transformed works in whatever units objectTransform.position and the
        // ray are expressed in consistently (see query.cpp: it never multiplies by a separate
        // voxel scale). Convert everything to voxel-index units going in, and the returned
        // worldPosition/normal/distance back to meters coming out.
        const RigidTransform voxelUnitTransform = make_rigid_transform(
            multiply(current.position, 1.0F / object.voxelSizeMeters), current.rotation);
        const Float3 voxelUnitOrigin = multiply(worldOrigin, 1.0F / object.voxelSizeMeters);
        const float voxelUnitMaxDistance = maxDistance / object.voxelSizeMeters;
        const auto hit = raycast_voxels_transformed(*object.voxels, voxelUnitTransform, voxelUnitOrigin, worldDirection, voxelUnitMaxDistance);
        if (!hit) continue;
        const float distanceMeters = hit->objectHit.distance * object.voxelSizeMeters;
        if (best && distanceMeters >= best->distance) continue;
        best = GameRaycastHit{
            id,
            multiply(hit->worldPosition, object.voxelSizeMeters),
            hit->worldNormal,
            distanceMeters,
            hit->objectHit.material};
    }
    return best;
}

std::vector<GameObjectId> GameWorld::sphere_overlap(Float3 worldCenter, float radius) const {
    std::vector<GameObjectId> result;
    if (!(radius > 0.0F) || !std::isfinite(radius)) return result;
    const float radiusSquared = radius * radius;
    for (const auto& [id, object] : objects_) {
#if defined(DVE_ENABLE_DEFORMABLE_RUNTIME)
        if (const DeformableCollisionProxy* proxy = deformables_->collision_proxy(id)) {
            bool overlaps{};
            for (std::size_t vertex = 0U; vertex < proxy->particles.size(); ++vertex) {
                const float particleRadius = vertex < proxy->radii.size() ? proxy->radii[vertex] : 0.01F;
                const float combined = radius + particleRadius;
                if (length_squared(subtract(proxy->particles[vertex], worldCenter)) <= combined * combined) {
                    overlaps = true; break;
                }
            }
            if (overlaps) result.push_back(id);
        }
#endif
        const RigidTransform current = resolve_transform(object);
        if (object.polygon && object.polygonBvh) {
            const Float3 localCenter = inverse_transform_point(current, worldCenter);
            if (object.polygonBvh->sphere_overlap(localCenter, radius)) result.push_back(id);
        }
        if (!object.voxels) continue;
        // Testing in the object's own local frame (sphere center transformed in, box already
        // axis-aligned there) is an exact sphere-vs-oriented-box test, not an axis-aligned-
        // world-box approximation that would over- or under-report near a rotated object.
        const Float3 localCenter = inverse_transform_point(current, worldCenter);
        bool overlaps = false;
        for (const VoxelBox& box : build_merged_object_box_proxy(*object.voxels)) {
            const SolverBox solverBox = voxel_box_to_solver_box(box, object.voxelSizeMeters);
            const Float3 boxMin = subtract(solverBox.center, solverBox.halfExtents);
            const Float3 boxMax = add(solverBox.center, solverBox.halfExtents);
            const Float3 closest{
                std::clamp(localCenter.x, boxMin.x, boxMax.x),
                std::clamp(localCenter.y, boxMin.y, boxMax.y),
                std::clamp(localCenter.z, boxMin.z, boxMax.z)};
            if (length_squared(subtract(localCenter, closest)) <= radiusSquared) { overlaps = true; break; }
        }
        if (overlaps) result.push_back(id);
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}


std::optional<GameCapsuleHit> GameWorld::capsule_sweep(
    const Capsule& worldCapsule, Float3 worldDisplacement, GameObjectId ignoreObject) const {
    if (!(worldCapsule.radius > 0.0F) || !std::isfinite(worldCapsule.radius) ||
        !std::isfinite(worldDisplacement.x) || !std::isfinite(worldDisplacement.y) ||
        !std::isfinite(worldDisplacement.z)) return std::nullopt;
    std::optional<GameCapsuleHit> best;
    for (const auto& [id, object] : objects_) {
        if (id == ignoreObject || (!object.voxels && !object.polygon)) continue;
        const RigidTransform current = resolve_transform(object);
        std::optional<CapsuleSweepHit> hit;
        if (object.voxels) {
            const float inverseScale = 1.0F / object.voxelSizeMeters;
            const RigidTransform voxelTransform = make_rigid_transform(
                multiply(current.position, inverseScale), current.rotation);
            Capsule voxelCapsule{
                multiply(worldCapsule.pointA, inverseScale),
                multiply(worldCapsule.pointB, inverseScale),
                worldCapsule.radius * inverseScale};
            hit = sweep_capsule_conservative(
                *object.voxels, voxelTransform, voxelCapsule, multiply(worldDisplacement, inverseScale));
        } else if (object.polygon && object.polygonBvh) {
            hit = sweep_polygon_capsule(*object.polygonBvh, current, worldCapsule, worldDisplacement);
        }
        if (!hit) continue;
        if (best && (hit->time > best->time + 1.0e-7F ||
                     (std::abs(hit->time - best->time) <= 1.0e-7F && id >= best->objectId))) continue;
        best = GameCapsuleHit{
            id, hit->time, hit->worldPosition,
            hit->worldNormal, hit->material, object.dynamic};
    }
    return best;
}

bool GameWorld::capsule_overlaps(const Capsule& worldCapsule, GameObjectId ignoreObject) const {
    for (const auto& [id, object] : objects_) {
        if (id == ignoreObject) continue;
        const RigidTransform current = resolve_transform(object);
        if (object.voxels) {
            const float inverseScale = 1.0F / object.voxelSizeMeters;
            const RigidTransform voxelTransform = make_rigid_transform(
                multiply(current.position, inverseScale), current.rotation);
            const Capsule voxelCapsule{
                multiply(worldCapsule.pointA, inverseScale), multiply(worldCapsule.pointB, inverseScale),
                worldCapsule.radius * inverseScale};
            if (capsule_overlaps_voxels(*object.voxels, voxelTransform, voxelCapsule)) return true;
        } else if (object.polygon && object.polygonBvh &&
                   polygon_capsule_overlap(*object.polygonBvh, current, worldCapsule)) return true;
    }
    return false;
}

GameCapsuleDepenetration GameWorld::depenetrate_capsule(
    Capsule& worldCapsule, GameObjectId ignoreObject, std::size_t maximumIterations,
    float skinMeters) const {
    GameCapsuleDepenetration result;
    for (std::size_t outer = 0; outer < maximumIterations; ++outer) {
        Float3 best{};
        float bestLength = std::numeric_limits<float>::infinity();
        bool found = false;
        GameObjectId bestObject = std::numeric_limits<GameObjectId>::max();
        for (const auto& [id, object] : objects_) {
            if (id == ignoreObject || (!object.voxels && !object.polygon)) continue;
            const RigidTransform current = resolve_transform(object);
            Float3 correction{};
            bool penetrated = false;
            if (object.voxels) {
                const float inverseScale = 1.0F / object.voxelSizeMeters;
                const RigidTransform voxelTransform = make_rigid_transform(
                    multiply(current.position, inverseScale), current.rotation);
                const Capsule voxelCapsule{
                    multiply(worldCapsule.pointA, inverseScale), multiply(worldCapsule.pointB, inverseScale),
                    worldCapsule.radius * inverseScale};
                std::size_t iterations{};
                const Float3 voxelCorrection = depenetrate_capsule_conservative(
                    *object.voxels, voxelTransform, voxelCapsule, 1U,
                    std::max(0.0F, skinMeters * inverseScale), &iterations);
                correction = multiply(voxelCorrection, object.voxelSizeMeters);
                penetrated = iterations > 0U;
            } else if (object.polygonBvh) {
                const auto contact = polygon_capsule_contact_world(
                    *object.polygonBvh, current, worldCapsule);
                if (contact && contact->penetration > 0.0F) {
                    correction = multiply(
                        contact->normal, contact->penetration + std::max(0.0F, skinMeters));
                    penetrated = true;
                }
            }
            const float correctionLength = length_squared(correction);
            if (penetrated && correctionLength > 0.0F &&
                (correctionLength < bestLength - 1.0e-10F ||
                 (std::abs(correctionLength - bestLength) <= 1.0e-10F && id < bestObject))) {
                best = correction;
                bestLength = correctionLength;
                bestObject = id;
                found = true;
            }
        }
        if (!found) break;
        worldCapsule = move_capsule(worldCapsule, best);
        result.correction = add(result.correction, best);
        ++result.iterations;
    }
    return result;
}

std::optional<MovingRigidTransform> GameWorld::moving_transform(GameObjectId id) const {
    const auto it = objects_.find(id);
    if (it == objects_.end()) return std::nullopt;
    const Object& object = it->second;
    if (!object.hasBody || !object.dynamic) return MovingRigidTransform{object.authoredTransform, object.authoredTransform};
    const auto state = physics_->state(object.bodyHandle);
    if (!state) return std::nullopt;
    const auto toObjectFrame = [&](const RigidTransform& bodyTransform) {
        return make_rigid_transform(
            subtract(bodyTransform.position, rotate(bodyTransform.rotation, object.localCenterOfMassMeters)),
            bodyTransform.rotation);
    };
    return MovingRigidTransform{toObjectFrame(state->previousTransform), toObjectFrame(state->currentTransform)};
}

std::optional<Float3> GameWorld::velocity_at_point(GameObjectId id, Float3 worldPoint) const {
    const auto it = objects_.find(id);
    if (it == objects_.end() || !it->second.hasBody || !it->second.dynamic) return std::nullopt;
    const auto state = physics_->state(it->second.bodyHandle);
    if (!state) return std::nullopt;
    return add(state->linearVelocity,
        cross_product(state->angularVelocity, subtract(worldPoint, state->currentTransform.position)));
}

bool GameWorld::is_dynamic(GameObjectId id) const noexcept {
    const auto it = objects_.find(id);
    return it != objects_.end() && it->second.dynamic;
}

void GameWorld::set_action_pressed(const std::string& action, bool pressed) { actionStates_[action] = pressed; }
bool GameWorld::is_action_pressed(const std::string& action) const {
    const auto it = actionStates_.find(action);
    return it != actionStates_.end() && it->second;
}
void GameWorld::set_axis(const std::string& axis, float value) { axisStates_[axis] = value; }
float GameWorld::get_axis(const std::string& axis) const {
    const auto it = axisStates_.find(axis);
    return it == axisStates_.end() ? 0.0F : it->second;
}

GameWorld::TimerId GameWorld::schedule_once(float secondsFromNow, std::function<void()> callback) {
    const TimerId id = nextTimerId_++;
    timers_.push_back({id, elapsedSeconds_ + std::max(0.0F, secondsFromNow), 0.0F, false, std::move(callback)});
    return id;
}

GameWorld::TimerId GameWorld::schedule_repeating(float intervalSeconds, std::function<void()> callback) {
    const TimerId id = nextTimerId_++;
    const float interval = std::max(1.0F / 1000.0F, intervalSeconds);
    timers_.push_back({id, elapsedSeconds_ + interval, interval, false, std::move(callback)});
    return id;
}

bool GameWorld::cancel_timer(TimerId id) {
    for (Timer& timer : timers_) {
        if (timer.id == id && !timer.cancelled) { timer.cancelled = true; return true; }
    }
    return false;
}

GameObjectPoolId GameWorld::register_pool(GameObjectPoolDesc desc, std::string* error) {
    const auto fail = [&](std::string message) {
        if (error) *error = std::move(message);
        return kInvalidGameObjectPoolId;
    };
    if (!membership_name_is_valid(desc.name)) return fail("pool name is invalid");
    if (desc.capacity == 0U || desc.capacity > 100000U) return fail("pool capacity is out of range");
    for (const auto& [id, pool] : pools_) {
        (void)id;
        if (pool.name == desc.name) return fail("pool name is already registered");
    }
    Pool pool;
    pool.id = nextPoolId_++;
    pool.name = std::move(desc.name);
    pool.prototype = std::move(desc.prototype);
    pool.capacity = desc.capacity;
    pool.freeIds.reserve(pool.capacity);
    for (std::size_t slot = 0; slot < pool.capacity; ++slot) pool.freeIds.push_back(allocate_id());
    std::sort(pool.freeIds.begin(), pool.freeIds.end(), std::greater<>());
    const GameObjectPoolId id = pool.id;
    pools_.emplace(id, std::move(pool));
    return id;
}

GameObjectId GameWorld::acquire_from_pool(
    GameObjectPoolId poolId, const RigidTransform& transformValue, std::string* error) {
    auto poolIt = pools_.find(poolId);
    if (poolIt == pools_.end()) { if (error) *error = "object pool does not exist"; return kInvalidGameObjectId; }
    Pool& pool = poolIt->second;
    if (pool.freeIds.empty()) { if (error) *error = "object pool is exhausted"; return kInvalidGameObjectId; }
    const GameObjectId id = pool.freeIds.back();
    pool.freeIds.pop_back();
    GameObjectDesc desc = pool.prototype;
    desc.transform = transformValue;
    Component marker;
    marker.id = 1U;
    for (const Component& existing : desc.components) marker.id = std::max(marker.id, existing.id + 1U);
    marker.type = "dve.pool_member";
    marker.properties.emplace("pool", pool.name);
    marker.properties.emplace("slot", static_cast<std::int64_t>(id));
    desc.components.push_back(std::move(marker));
    const GameObjectId created = create_object_internal(std::move(desc), id, false, error);
    if (created == kInvalidGameObjectId) {
        pool.freeIds.push_back(id);
        std::sort(pool.freeIds.begin(), pool.freeIds.end(), std::greater<>());
        return created;
    }
    pool.activeIds.insert(id);
    objectPools_[id] = poolId;
    dispatch_lifecycle({GameLifecycleEventKind::PoolAcquire, id, kInvalidGameObjectId,
                        kInvalidComponentId, {}, pool.name});
    dispatch_lifecycle({GameLifecycleEventKind::Spawn, id});
    return id;
}

bool GameWorld::release_to_pool(GameObjectId id, std::string* error) {
    const auto owner = objectPools_.find(id);
    if (owner == objectPools_.end()) { if (error) *error = "object is not owned by a pool"; return false; }
    auto poolIt = pools_.find(owner->second);
    if (poolIt == pools_.end()) { if (error) *error = "object pool no longer exists"; return false; }
    Pool& pool = poolIt->second;
    dispatch_lifecycle({GameLifecycleEventKind::PoolRelease, id, kInvalidGameObjectId,
                        kInvalidComponentId, {}, pool.name});
    dispatch_lifecycle({GameLifecycleEventKind::Disable, id});
    if (!destroy_object_internal(id, false)) { if (error) *error = "pooled object no longer exists"; return false; }
    pool.activeIds.erase(id);
    objectPools_.erase(owner);
    pool.freeIds.push_back(id);
    std::sort(pool.freeIds.begin(), pool.freeIds.end(), std::greater<>());
    return true;
}

std::size_t GameWorld::pool_available(GameObjectPoolId poolId) const noexcept {
    const auto it = pools_.find(poolId);
    return it == pools_.end() ? 0U : it->second.freeIds.size();
}

void GameWorld::on_tick(TickListener listener) { tickListeners_.push_back(std::move(listener)); }
void GameWorld::on_damage(DamageListener listener) { damageListeners_.push_back(std::move(listener)); }
void GameWorld::on_destroyed(DestroyListener listener) { destroyListeners_.push_back(std::move(listener)); }
void GameWorld::on_lifecycle(LifecycleListener listener) { lifecycleListeners_.push_back(std::move(listener)); }

void GameWorld::tick(float fixedDeltaSeconds) {
    if (!(fixedDeltaSeconds > 0.0F) || !std::isfinite(fixedDeltaSeconds)) return;
    elapsedSeconds_ += fixedDeltaSeconds;

    // Snapshot due timers before firing any of them: a callback firing a timer can itself
    // schedule new timers (very common: "every 2s, wait 0.5s, then..."), and those must not
    // be eligible to fire within this same tick.
    std::vector<std::function<void()>> due;
    for (Timer& timer : timers_) {
        if (timer.cancelled || timer.fireAtSeconds > elapsedSeconds_) continue;
        due.push_back(timer.callback);
        if (timer.intervalSeconds > 0.0F) timer.fireAtSeconds += timer.intervalSeconds;
        else timer.cancelled = true;
    }
    std::erase_if(timers_, [](const Timer& timer) { return timer.cancelled; });
    for (const auto& callback : due) callback();

    physics_->step(fixedDeltaSeconds);
#if defined(DVE_ENABLE_DEFORMABLE_RUNTIME)
    lastDeformableTelemetry_ = deformables_->tick(fixedDeltaSeconds);
#endif
    animationControllers_->tick(fixedDeltaSeconds);
    animation_->tick(fixedDeltaSeconds);
    controlRigs_->evaluate_all();
    for (const std::uint64_t objectId : ragdolls_->object_ids()) {
        const auto world = transform(objectId);
        if (world) (void)ragdolls_->tick(objectId, fixedDeltaSeconds, *world, nullptr);
    }
    for (const std::uint64_t objectId : animation_->root_motion_objects()) {
        const auto delta = animation_->consume_root_motion(objectId);
        if (ragdolls_->owns_pose(objectId)) continue;
        const auto current = transform(objectId);
        if (!delta || !current) continue;
        const RigidTransform updated = compose_rigid_transforms(
            *current, make_rigid_transform(delta->translation, delta->rotation));
        (void)set_position(objectId, updated.position);
        (void)set_rotation(objectId, updated.rotation);
    }
    synchronize_attached_bodies();
    gameplay_->fixed_update(fixedDeltaSeconds);
#if defined(DVE_ENABLE_CPU_HAIR)
    for (const CpuHairOwnerId owner : cpuHair_->owner_span()) {
        const auto worldTransform = transform(owner);
        if (worldTransform) (void)cpuHair_->set_root_transform(owner, *worldTransform);
    }
    lastCpuHairTelemetry_ = cpuHair_->tick(fixedDeltaSeconds);
#endif
    cameras_->update(*this, fixedDeltaSeconds);
    uiRuntime_->rebuild();

    for (const TickListener& listener : tickListeners_) listener(fixedDeltaSeconds);
}

} // namespace dve
