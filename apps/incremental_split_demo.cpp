#include <array>
#include <cstdint>
#include <iostream>

#include "dve/collision_proxy.hpp"
#include "dve/connectivity.hpp"
#include "dve/damage.hpp"
#include "dve/fragment.hpp"

int main() {
    using namespace dve;

    VoxelObject structure(1001);
    for (int z = 0; z < 4; ++z) {
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 8; ++x) structure.set_voxel({x, y, z}, 1);
        }
    }
    for (int z = 1; z < 5; ++z) {
        for (int y = 0; y < 4; ++y) {
            for (int x = 10; x < 18; ++x) structure.set_voxel({x, y, z}, 2);
        }
    }
    structure.set_voxel({8, 1, 2}, 1);
    structure.set_voxel({9, 1, 2}, 2);

    const AnchorPredicate anchoredToFloor = [](Int3 voxel) { return voxel.z == 0; };
    IncrementalConnectivityCache connectivity;
    connectivity.initialize(structure, anchoredToFloor);

    DamageBatchWorkspace damageWorkspace;
    damageWorkspace.reserve(4, 16, 16);
    const std::array<SphereDamageCommand, 1> commands{{
        {{8.5F, 1.5F, 2.5F}, 0.6F, 1},
    }};
    const DamageApplyReportView damage = apply_damage_commands(structure, commands, damageWorkspace);
    const ConnectivityUpdateStats update = connectivity.update(structure, damage.edits, anchoredToFloor);
    const ConnectivitySnapshot snapshot = connectivity.snapshot();

    std::size_t detachedIndex = snapshot.components.size();
    for (std::size_t i = 0; i < snapshot.components.size(); ++i) {
        if (!snapshot.components[i].anchored) {
            detachedIndex = i;
            break;
        }
    }
    if (detachedIndex == snapshot.components.size()) {
        std::cerr << "No detached component was found.\n";
        return 2;
    }

    const auto plan = build_split_plan(structure, snapshot, detachedIndex);
    if (!plan) {
        std::cerr << "Split plan failed generation validation.\n";
        return 3;
    }
    auto detached = commit_split_plan(structure, *plan, 1002);
    if (!detached) {
        std::cerr << "Split commit failed.\n";
        return 4;
    }
    const auto proxy = build_object_box_proxy(*detached);
    const FragmentSolverPackage solverPackage = build_fragment_solver_package(
        *detached, make_rigid_transform({0.0F, 0.0F, 0.0F}, {}));

    std::cout << "{\n"
              << "  \"removed_voxels\": " << damage.removedVoxelCount << ",\n"
              << "  \"topology_stable_bricks\": " << update.topologyStableBricks << ",\n"
              << "  \"graph_nodes_visited\": " << update.graphNodesVisited << ",\n"
              << "  \"components_after_cut\": " << snapshot.components.size() << ",\n"
              << "  \"source_voxels\": " << structure.occupied_voxel_count() << ",\n"
              << "  \"detached_voxels\": " << detached->occupied_voxel_count() << ",\n"
              << "  \"detached_proxy_boxes\": " << proxy.size() << ",\n"
              << "  \"merged_solver_boxes\": " << solverPackage.boxes.size() << ",\n"
              << "  \"detached_mass_units\": " << solverPackage.mass.massUnits << ",\n"
              << "  \"center_of_mass\": [" << solverPackage.mass.centerOfMass.x << ", "
              << solverPackage.mass.centerOfMass.y << ", " << solverPackage.mass.centerOfMass.z << "]\n"
              << "}\n";
    return structure.validate() && detached->validate() && validate_box_proxy(*detached, proxy) &&
           validate_solver_package(*detached, solverPackage) ? 0 : 5;
}
