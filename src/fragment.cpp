#include "dve/fragment.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>

namespace dve {
namespace {

struct ExactMoments {
    std::uint64_t voxelCount{};
    std::uint64_t mass{};
    std::uint64_t firstX2{};
    std::uint64_t firstY2{};
    std::uint64_t firstZ2{};
    std::uint64_t secondX4{};
    std::uint64_t secondY4{};
    std::uint64_t secondZ4{};
    std::uint64_t crossXY4{};
    std::uint64_t crossXZ4{};
    std::uint64_t crossYZ4{};
};

[[nodiscard]] bool checked_add_product(
    std::uint64_t& accumulator,
    std::uint64_t a,
    std::uint64_t b,
    std::uint64_t c = 1U) noexcept {
    constexpr std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    if (a != 0U && b > maximum / a) return false;
    const std::uint64_t ab = a * b;
    if (ab != 0U && c > maximum / ab) return false;
    const std::uint64_t product = ab * c;
    if (accumulator > maximum - product) return false;
    accumulator += product;
    return true;
}

[[nodiscard]] int axis_min(const VoxelBox& box, int axis) noexcept {
    return axis == 0 ? box.min.x : (axis == 1 ? box.min.y : box.min.z);
}
[[nodiscard]] int axis_max(const VoxelBox& box, int axis) noexcept {
    return axis == 0 ? box.maxExclusive.x : (axis == 1 ? box.maxExclusive.y : box.maxExclusive.z);
}
void set_axis_max(VoxelBox& box, int axis, int value) noexcept {
    if (axis == 0) box.maxExclusive.x = value;
    else if (axis == 1) box.maxExclusive.y = value;
    else box.maxExclusive.z = value;
}

[[nodiscard]] std::array<int, 4> orthogonal_signature(const VoxelBox& box, int axis) noexcept {
    if (axis == 0) return {box.min.y, box.maxExclusive.y, box.min.z, box.maxExclusive.z};
    if (axis == 1) return {box.min.x, box.maxExclusive.x, box.min.z, box.maxExclusive.z};
    return {box.min.x, box.maxExclusive.x, box.min.y, box.maxExclusive.y};
}

[[nodiscard]] bool merge_axis(std::vector<VoxelBox>& boxes, int axis) {
    if (boxes.size() < 2) return false;
    std::sort(boxes.begin(), boxes.end(), [axis](const VoxelBox& a, const VoxelBox& b) {
        const auto sa = orthogonal_signature(a, axis);
        const auto sb = orthogonal_signature(b, axis);
        if (sa != sb) return sa < sb;
        if (axis_min(a, axis) != axis_min(b, axis)) return axis_min(a, axis) < axis_min(b, axis);
        return axis_max(a, axis) < axis_max(b, axis);
    });

    std::vector<VoxelBox> merged;
    merged.reserve(boxes.size());
    bool changed = false;
    for (const VoxelBox& box : boxes) {
        if (!merged.empty() && orthogonal_signature(merged.back(), axis) == orthogonal_signature(box, axis) &&
            axis_max(merged.back(), axis) == axis_min(box, axis)) {
            set_axis_max(merged.back(), axis, axis_max(box, axis));
            changed = true;
        } else {
            merged.push_back(box);
        }
    }
    boxes.swap(merged);
    return changed;
}

} // namespace

MaterialMassTable::MaterialMassTable() noexcept {
    densities_.fill(1U);
    densities_[kAirMaterial] = 0U;
}

void MaterialMassTable::set_density_units(MaterialId material, std::uint16_t densityUnits) noexcept {
    assert(densityUnits <= kMaximumMaterialDensityUnits);
    densities_[material] = material == kAirMaterial ? 0U :
        std::min(densityUnits, kMaximumMaterialDensityUnits);
}

FragmentMassProperties compute_fragment_mass_properties(
    const VoxelObject& object,
    const MaterialMassTable& materialMasses) {
    FragmentMassProperties result;
    result.voxelCount = object.occupied_voxel_count();
    if (result.voxelCount == 0U) {
        result.exactWithinLimits = true;
        return result;
    }

    Int3 minimum{kInt3Max};
    Int3 maximum{kInt3Min};
    for (const auto& [key, brick] : object.bricks()) {
        const Bitset512 occupancy = brick.occupancy();
        occupancy.for_each_set([&](std::uint16_t index) {
            if (materialMasses.density_units(brick.material(index)) == 0U) return;
            const Int3 voxel = global_from_local(key, local_from_index_unchecked(index));
            minimum = min_components(minimum, voxel);
            maximum = max_components(maximum, voxel);
            ++result.massBearingVoxelCount;
        });
    }
    if (result.massBearingVoxelCount == 0U) {
        result.exactWithinLimits = true;
        return result;
    }
    const std::int64_t extentX = static_cast<std::int64_t>(maximum.x) - minimum.x + 1;
    const std::int64_t extentY = static_cast<std::int64_t>(maximum.y) - minimum.y + 1;
    const std::int64_t extentZ = static_cast<std::int64_t>(maximum.z) - minimum.z + 1;
    if (result.voxelCount > kMaximumDynamicFragmentVoxels ||
        extentX > kMaximumDynamicFragmentExtent || extentY > kMaximumDynamicFragmentExtent ||
        extentZ > kMaximumDynamicFragmentExtent) {
        return result;
    }

    ExactMoments exact;
    bool valid = true;
    for (const auto& [key, brick] : object.bricks()) {
        const Bitset512 occupancy = brick.occupancy();
        occupancy.for_each_set([&](std::uint16_t index) {
            if (!valid) return;
            const std::uint64_t mass = materialMasses.density_units(brick.material(index));
            if (mass == 0U) return;
            const Int3 voxel = global_from_local(key, local_from_index_unchecked(index));
            const std::uint64_t qx = static_cast<std::uint64_t>(voxel.x - minimum.x) * 2U + 1U;
            const std::uint64_t qy = static_cast<std::uint64_t>(voxel.y - minimum.y) * 2U + 1U;
            const std::uint64_t qz = static_cast<std::uint64_t>(voxel.z - minimum.z) * 2U + 1U;
            ++exact.voxelCount;
            valid = checked_add_product(exact.mass, mass, 1U) &&
                    checked_add_product(exact.firstX2, mass, qx) &&
                    checked_add_product(exact.firstY2, mass, qy) &&
                    checked_add_product(exact.firstZ2, mass, qz) &&
                    checked_add_product(exact.secondX4, mass, qx, qx) &&
                    checked_add_product(exact.secondY4, mass, qy, qy) &&
                    checked_add_product(exact.secondZ4, mass, qz, qz) &&
                    checked_add_product(exact.crossXY4, mass, qx, qy) &&
                    checked_add_product(exact.crossXZ4, mass, qx, qz) &&
                    checked_add_product(exact.crossYZ4, mass, qy, qz);
        });
        if (!valid) break;
    }
    if (!valid || exact.mass == 0U) return result;

    result.exactWithinLimits = true;
    result.massBearingVoxelCount = exact.voxelCount;
    result.massUnits = exact.mass;
    const long double mass = static_cast<long double>(exact.mass);
    const long double firstX2 = static_cast<long double>(exact.firstX2);
    const long double firstY2 = static_cast<long double>(exact.firstY2);
    const long double firstZ2 = static_cast<long double>(exact.firstZ2);
    const long double secondX4 = static_cast<long double>(exact.secondX4);
    const long double secondY4 = static_cast<long double>(exact.secondY4);
    const long double secondZ4 = static_cast<long double>(exact.secondZ4);
    const long double crossXY4 = static_cast<long double>(exact.crossXY4);
    const long double crossXZ4 = static_cast<long double>(exact.crossXZ4);
    const long double crossYZ4 = static_cast<long double>(exact.crossYZ4);

    result.centerOfMass = {
        static_cast<float>(static_cast<long double>(minimum.x) + firstX2 / (2.0L * mass)),
        static_cast<float>(static_cast<long double>(minimum.y) + firstY2 / (2.0L * mass)),
        static_cast<float>(static_cast<long double>(minimum.z) + firstZ2 / (2.0L * mass)),
    };

    const long double varianceX = (secondX4 - firstX2 * firstX2 / mass) / 4.0L;
    const long double varianceY = (secondY4 - firstY2 * firstY2 / mass) / 4.0L;
    const long double varianceZ = (secondZ4 - firstZ2 * firstZ2 / mass) / 4.0L;
    const long double covarianceXY = (crossXY4 - firstX2 * firstY2 / mass) / 4.0L;
    const long double covarianceXZ = (crossXZ4 - firstX2 * firstZ2 / mass) / 4.0L;
    const long double covarianceYZ = (crossYZ4 - firstY2 * firstZ2 / mass) / 4.0L;
    const long double cubeCenterInertia = mass / 6.0L;
    result.inertiaAboutCenter = {
        static_cast<double>(varianceY + varianceZ + cubeCenterInertia),
        static_cast<double>(varianceX + varianceZ + cubeCenterInertia),
        static_cast<double>(varianceX + varianceY + cubeCenterInertia),
        static_cast<double>(-covarianceXY),
        static_cast<double>(-covarianceXZ),
        static_cast<double>(-covarianceYZ),
    };
    return result;
}

std::vector<VoxelBox> merge_adjacent_voxel_boxes(std::vector<VoxelBox> boxes) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (int axis = 0; axis < 3; ++axis) changed = merge_axis(boxes, axis) || changed;
    }
    std::sort(boxes.begin(), boxes.end());
    return boxes;
}

std::vector<VoxelBox> build_merged_object_box_proxy(const VoxelObject& object) {
    return merge_adjacent_voxel_boxes(build_object_box_proxy(object));
}

FragmentSolverPackage build_fragment_solver_package(
    const VoxelObject& object,
    const RigidTransform& voxelObjectWorldTransform,
    const MaterialMassTable& materialMasses,
    std::size_t maximumProxyBoxes) {
    FragmentSolverPackage package;
    package.mass = compute_fragment_mass_properties(object, materialMasses);
    std::vector<VoxelBox> boxes = build_object_box_proxy(object);
    package.unmergedBoxCount = boxes.size();
    boxes = merge_adjacent_voxel_boxes(std::move(boxes));
    package.proxyOverBudget = maximumProxyBoxes != 0 && boxes.size() > maximumProxyBoxes;
    package.bodyTransform = make_rigid_transform(
        transform_point(voxelObjectWorldTransform, package.mass.centerOfMass),
        voxelObjectWorldTransform.rotation);
    package.boxes.reserve(boxes.size());
    for (const VoxelBox& box : boxes) {
        const Float3 boxCenter{
            0.5F * static_cast<float>(box.min.x + box.maxExclusive.x),
            0.5F * static_cast<float>(box.min.y + box.maxExclusive.y),
            0.5F * static_cast<float>(box.min.z + box.maxExclusive.z),
        };
        package.boxes.push_back({
            subtract(boxCenter, package.mass.centerOfMass),
            {
                0.5F * static_cast<float>(box.maxExclusive.x - box.min.x),
                0.5F * static_cast<float>(box.maxExclusive.y - box.min.y),
                0.5F * static_cast<float>(box.maxExclusive.z - box.min.z),
            },
        });
    }
    return package;
}

bool validate_solver_package(
    const VoxelObject& object,
    const FragmentSolverPackage& package,
    double tolerance) {
    if (!package.mass.exactWithinLimits) return false;
    if (package.mass.voxelCount != object.occupied_voxel_count()) return false;
    if (package.mass.massUnits == 0U && package.mass.voxelCount != 0U) return false;
    if (package.mass.massBearingVoxelCount > package.mass.voxelCount) return false;
    if (package.boxes.empty() != (package.mass.voxelCount == 0U)) return false;
    for (const SolverBox& box : package.boxes) {
        if (box.halfExtents.x <= 0.0F || box.halfExtents.y <= 0.0F || box.halfExtents.z <= 0.0F) return false;
    }
    if (tolerance < 0.0) return false;
    const Float3 worldCenter = package.bodyTransform.position;
    return std::isfinite(worldCenter.x) && std::isfinite(worldCenter.y) && std::isfinite(worldCenter.z) &&
           std::isfinite(package.mass.inertiaAboutCenter.xx) && package.mass.inertiaAboutCenter.xx >= 0.0 &&
           std::isfinite(package.mass.inertiaAboutCenter.yy) && package.mass.inertiaAboutCenter.yy >= 0.0 &&
           std::isfinite(package.mass.inertiaAboutCenter.zz) && package.mass.inertiaAboutCenter.zz >= 0.0;
}

} // namespace dve
