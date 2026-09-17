#include "dve/editor_simulation.hpp"

#include <algorithm>
#include <cmath>

namespace dve::editor {
namespace {

// Deliberately mirrors src/runtime_scene.cpp's make_material_mass_table /
// voxel_box_to_solver_box (anonymous-namespace, not exported there) rather than sharing them:
// the input type here is EditorMaterialLibrary, not CookedVoxelAsset, so the two are not
// literally the same function. The quantization scheme (linear density units, quantum sized
// from the largest density present) is kept identical on purpose so that an object's physical
// behavior does not change when its authoring representation is cooked into a runtime asset.
[[nodiscard]] MaterialMassTable build_material_mass_table(
    const VoxelObject& object,
    const EditorMaterialLibrary& materials,
    double& densityQuantumKilogramsPerCubicMeter) {
    double maximumDensity = 0.0;
    for (const EditorMaterialEntry& entry : materials.entries()) {
        maximumDensity = std::max(maximumDensity, static_cast<double>(entry.definition.densityKilogramsPerCubicMeter));
    }
    MaterialMassTable table;
    if (!(maximumDensity > 0.0) || !std::isfinite(maximumDensity)) {
        densityQuantumKilogramsPerCubicMeter = 0.0;
        return table; // every voxel becomes non-mass-bearing; caller's mass-units check rejects it
    }
    densityQuantumKilogramsPerCubicMeter = maximumDensity / static_cast<double>(kMaximumMaterialDensityUnits);
    for (const EditorMaterialEntry& entry : materials.entries()) {
        const double density = static_cast<double>(entry.definition.densityKilogramsPerCubicMeter);
        if (!(density > 0.0)) continue;
        const long rounded = std::lround(density / densityQuantumKilogramsPerCubicMeter);
        const auto units = static_cast<std::uint16_t>(
            std::clamp<long>(rounded, 1L, static_cast<long>(kMaximumMaterialDensityUnits)));
        table.set_density_units(entry.id, units);
    }
    (void)object;
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

// Builds a dynamic RigidBodyCreateDesc for a non-anchored object. The fragment package is
// intentionally constructed in voxel units: the object translation is divided by the voxel
// size before package construction, and make_rigid_body_desc() performs the one authoritative
// conversion of body translation, box geometry, mass, and inertia into SI units.
[[nodiscard]] std::optional<RigidBodyCreateDesc> build_dynamic_desc(
    const EditorObject& object,
    const EditorMaterialLibrary& materials,
    Float3& outLocalCenterOfMassMeters) {
    if (!object.voxels || object.voxels->occupied_voxel_count() == 0) return std::nullopt;

    double densityQuantum = 0.0;
    const MaterialMassTable massTable = build_material_mass_table(*object.voxels, materials, densityQuantum);
    if (!(densityQuantum > 0.0)) return std::nullopt;

    const float voxelSizeMeters = object.voxelSizeMeters;
    if (!(voxelSizeMeters > 0.0F) || !std::isfinite(voxelSizeMeters)) return std::nullopt;

    const RigidTransform voxelUnitTransform = make_rigid_transform(
        multiply(object.transform.position, 1.0F / voxelSizeMeters), object.transform.rotation);
    const FragmentSolverPackage package =
        build_fragment_solver_package(*object.voxels, voxelUnitTransform, massTable);
    if (package.proxyOverBudget) return std::nullopt;

    const FragmentSolverScale scale{
        densityQuantum * static_cast<double>(voxelSizeMeters) * voxelSizeMeters * voxelSizeMeters,
        voxelSizeMeters};
    std::optional<RigidBodyCreateDesc> desc = make_rigid_body_desc(package, scale);
    if (!desc) return std::nullopt;

    desc->collisionClass =
        object.flags.structural ? RigidBodyCollisionClass::Full : RigidBodyCollisionClass::DebrisNoSelf;
    if (!validate_rigid_body_desc(*desc)) return std::nullopt;

    outLocalCenterOfMassMeters = multiply(package.mass.centerOfMass, voxelSizeMeters);
    return desc;
}

[[nodiscard]] std::optional<StaticRigidBodyCreateDesc> build_static_desc(const EditorObject& object) {
    if (!object.voxels || object.voxels->occupied_voxel_count() == 0) return std::nullopt;
    const std::vector<VoxelBox> boxes = build_merged_object_box_proxy(*object.voxels);
    if (boxes.empty()) return std::nullopt;

    StaticRigidBodyCreateDesc desc;
    desc.transform = object.transform;
    desc.collisionClass = RigidBodyCollisionClass::Full;
    desc.boxes.reserve(boxes.size());
    for (const VoxelBox& box : boxes) desc.boxes.push_back(voxel_box_to_solver_box(box, object.voxelSizeMeters));
    if (!validate_static_rigid_body_desc(desc)) return std::nullopt;
    return desc;
}

} // namespace

EditorJoltSimulation::EditorJoltSimulation() = default;
EditorJoltSimulation::EditorJoltSimulation(const JoltWorldConfig& config) : config_(config) {}
EditorJoltSimulation::~EditorJoltSimulation() { stop(); }

bool EditorJoltSimulation::start(
    const EditorDocument& document, const EditorMaterialLibrary& materials, std::string* error) {
    if (running()) {
        if (error) *error = "simulation is already running; call stop() first";
        return false;
    }
    std::string validation;
    if (!document.validate(&validation)) {
        if (error) *error = validation;
        return false;
    }

    std::vector<RigidBodyCreateDesc> dynamicDescs;
    std::vector<EditorObjectId> dynamicIds;
    std::vector<Float3> dynamicLocalComs;
    std::vector<StaticRigidBodyCreateDesc> staticDescs;
    std::size_t skipped = 0;

    for (const auto& [id, object] : document.objects()) {
        if (!object.flags.collisionEnabled || !object.voxels || object.voxels->occupied_voxel_count() == 0) continue;
        if (object.flags.anchored) {
            if (auto desc = build_static_desc(object)) {
                staticDescs.push_back(std::move(*desc));
            } else {
                ++skipped;
            }
            continue;
        }
        Float3 localCom{};
        if (auto desc = build_dynamic_desc(object, materials, localCom)) {
            dynamicDescs.push_back(std::move(*desc));
            dynamicIds.push_back(id);
            dynamicLocalComs.push_back(localCom);
        } else {
            ++skipped;
        }
    }

    auto world = std::make_unique<JoltRigidBodyWorld>(config_);

    if (!staticDescs.empty()) {
        const std::vector<RigidBodyHandle> handles = world->create_static_bodies(staticDescs);
        if (handles.size() != staticDescs.size()) {
            if (error) *error = "failed to create static simulation bodies";
            return false;
        }
        for (const RigidBodyHandle handle : handles) {
            if (handle == kInvalidRigidBodyHandle) { ++skipped; continue; }
        }
        staticHandles_ = handles;
    }

    if (!dynamicDescs.empty()) {
        const std::vector<RigidBodyHandle> handles = world->create_bodies(dynamicDescs);
        if (handles.size() != dynamicDescs.size()) {
            if (error) *error = "failed to create dynamic simulation bodies";
            return false;
        }
        dynamicObjects_.reserve(handles.size());
        for (std::size_t i = 0; i < handles.size(); ++i) {
            if (handles[i] == kInvalidRigidBodyHandle) { ++skipped; continue; }
            dynamicObjects_.push_back({dynamicIds[i], handles[i], dynamicLocalComs[i]});
        }
    }

    skippedObjectCount_ = skipped;
    world_ = std::move(world);
    return true;
}

bool EditorJoltSimulation::step(EditorDocument& document, float fixedDeltaSeconds) {
    if (!running() || !(fixedDeltaSeconds > 0.0F) || !std::isfinite(fixedDeltaSeconds)) return false;
    world_->step(fixedDeltaSeconds);
    if (!world_->last_update_succeeded()) return false;
    for (const SimulatedDynamicObject& simulated : dynamicObjects_) {
        const std::optional<RigidBodyState> state = world_->state(simulated.handle);
        if (!state) continue;
        EditorObject* object = document.find_object(simulated.objectId);
        if (!object) continue;
        const Float3 worldOffset = rotate(state->currentTransform.rotation, simulated.localCenterOfMassMeters);
        object->transform.rotation = state->currentTransform.rotation;
        object->transform.position = subtract(state->currentTransform.position, worldOffset);
    }
    return true;
}

void EditorJoltSimulation::stop() {
    world_.reset();
    dynamicObjects_.clear();
    staticHandles_.clear();
    skippedObjectCount_ = 0;
}

JoltPhysicsTelemetry EditorJoltSimulation::telemetry() const {
    return world_ ? world_->telemetry() : JoltPhysicsTelemetry{};
}

std::optional<bool> EditorJoltSimulation::object_awake(EditorObjectId id) const {
    if (!running()) return std::nullopt;
    for (const SimulatedDynamicObject& simulated : dynamicObjects_) {
        if (simulated.objectId != id) continue;
        return world_->is_active(simulated.handle);
    }
    return std::nullopt;
}

} // namespace dve::editor
