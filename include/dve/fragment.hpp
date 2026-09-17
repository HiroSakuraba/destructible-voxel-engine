#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "dve/collision_proxy.hpp"
#include "dve/transform.hpp"

namespace dve {

// Integer density units keep authoritative mass aggregation deterministic.
// The conversion from units to solver mass is a caller-selected scale.
constexpr std::uint16_t kMaximumMaterialDensityUnits = 4095;
constexpr std::uint64_t kMaximumDynamicFragmentVoxels = 1U << 20U;
constexpr std::int32_t kMaximumDynamicFragmentExtent = 4096;

class MaterialMassTable {
public:
    MaterialMassTable() noexcept;

    void set_density_units(MaterialId material, std::uint16_t densityUnits) noexcept;
    [[nodiscard]] std::uint16_t density_units(MaterialId material) const noexcept {
        return densities_[material];
    }

private:
    std::array<std::uint16_t, 256> densities_{};
};

struct SymmetricInertiaTensor {
    double xx{};
    double yy{};
    double zz{};
    double xy{};
    double xz{};
    double yz{};
};

struct FragmentMassProperties {
    bool exactWithinLimits{};
    std::uint64_t voxelCount{};
    std::uint64_t massBearingVoxelCount{};
    std::uint64_t massUnits{};
    Float3 centerOfMass{}; // object-local voxel coordinates
    SymmetricInertiaTensor inertiaAboutCenter{}; // density-unit * voxel^2
};

// Portable exact uint64 accumulation of mass and moments within the documented
// dynamic-fragment limits. Only the final rational conversion is rounded.
[[nodiscard]] FragmentMassProperties compute_fragment_mass_properties(
    const VoxelObject& object,
    const MaterialMassTable& materialMasses = {});

// Deterministically merges exactly adjacent boxes with identical spans on the
// other two axes. Coverage remains exact; no empty voxel is added.
[[nodiscard]] std::vector<VoxelBox> merge_adjacent_voxel_boxes(std::vector<VoxelBox> boxes);
[[nodiscard]] std::vector<VoxelBox> build_merged_object_box_proxy(const VoxelObject& object);

struct SolverBox {
    Float3 center{};      // relative to the rigid body's center of mass
    Float3 halfExtents{};
};

struct FragmentSolverPackage {
    FragmentMassProperties mass{};
    // Center-of-mass body pose in voxel units. Rotation is dimensionless; translation is
    // converted to meters by make_rigid_body_desc() using FragmentSolverScale::metersPerVoxel.
    RigidTransform bodyTransform{};
    std::vector<SolverBox> boxes{};
    std::size_t unmergedBoxCount{};
    bool proxyOverBudget{};
};

// Keeps voxel coordinates unchanged. The input translation and returned body translation
// are in voxel units; the rigid-body frame is placed at the exact center of mass, and each
// collision box is offset into that body frame. make_rigid_body_desc() performs the single
// authoritative voxel-to-meter conversion.
[[nodiscard]] FragmentSolverPackage build_fragment_solver_package(
    const VoxelObject& object,
    const RigidTransform& voxelObjectWorldTransform,
    const MaterialMassTable& materialMasses = {},
    std::size_t maximumProxyBoxes = 256);

[[nodiscard]] bool validate_solver_package(
    const VoxelObject& object,
    const FragmentSolverPackage& package,
    double tolerance = 1.0e-5);

} // namespace dve
