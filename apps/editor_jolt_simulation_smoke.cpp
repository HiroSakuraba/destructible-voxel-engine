// Validates EditorJoltSimulation end to end: a floor object (anchored -> static) and a crate
// object (dynamic) are simulated through real Jolt Physics, the crate must settle and sleep,
// and running the same workflow through EditorWorkspace::set_mode() must restore the document
// to its exact pre-simulation state on return to Edit mode, even though the live document was
// mutated in place while Simulate mode was running. This is an integration smoke test, not a
// physics-quality benchmark; apps/jolt_solver_smoke.cpp already covers the underlying solver.

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "dve/editor_simulation.hpp"
#include "dve/editor_workspace.hpp"

namespace {

int failures = 0;

#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                               \
        }                                                                             \
    } while (false)

using namespace dve;
using namespace dve::editor;

constexpr float kVoxelSizeMeters = 0.25F;

EditorDocument make_scene() {
    EditorDocument document;

    // Anchored floor: 20 x 2 x 20 voxels of Concrete, top surface at world y = 0.
    EditorObject floor(1, "Floor");
    floor.voxelSizeMeters = kVoxelSizeMeters;
    floor.flags.anchored = true;
    floor.transform = make_rigid_transform({-2.5F, -0.5F, -2.5F}, {});
    for (int x = 0; x < 20; ++x)
        for (int y = 0; y < 2; ++y)
            for (int z = 0; z < 20; ++z) floor.voxels->set_voxel({x, y, z}, 1); // Concrete
    document.add_object(std::move(floor));

    // Dynamic crate: 2x2x2 voxels of Brick, dropped from just above the floor.
    EditorObject crate(2, "Crate");
    crate.voxelSizeMeters = kVoxelSizeMeters;
    crate.transform = make_rigid_transform({0.0F, 3.0F, 0.0F}, {});
    for (int x = 0; x < 2; ++x)
        for (int y = 0; y < 2; ++y)
            for (int z = 0; z < 2; ++z) crate.voxels->set_voxel({x, y, z}, 2); // Brick
    document.add_object(std::move(crate));

    return document;
}

void test_direct_simulation() {
    const EditorMaterialLibrary materials = EditorMaterialLibrary::make_default();
    EditorDocument document = make_scene();

    EditorJoltSimulation simulation;
    std::string error;
    CHECK(simulation.start(document, materials, &error));
    if (!error.empty()) std::fprintf(stderr, "start() error: %s\n", error.c_str());
    CHECK(simulation.simulated_dynamic_object_count() == 1U);
    CHECK(simulation.simulated_static_object_count() == 1U);
    CHECK(simulation.skipped_object_count() == 0U);

    constexpr float kFixedDelta = 1.0F / 120.0F;
    bool sleptWithinBudget = false;
    for (int step = 0; step < 120 * 6; ++step) {
        CHECK(simulation.step(document, kFixedDelta));
        const auto awake = simulation.object_awake(2);
        CHECK(awake.has_value());
        if (awake.has_value() && !*awake) { sleptWithinBudget = true; break; }
    }
    CHECK(sleptWithinBudget);

    const EditorObject* crate = document.find_object(2);
    CHECK(crate != nullptr);
    if (crate != nullptr) {
        // Crate bottom (local y=0) should rest on the floor top (world y=0), so the crate's
        // origin (its local (0,0,0) corner, since EditorObject transforms are unrotated
        // voxel-grid origins here) should settle near world y=0, not at the y=3 drop height
        // and not below zero (which would mean it fell through the floor).
        const float restY = crate->transform.position.y;
        std::printf("crate_rest_y=%.4f\n", static_cast<double>(restY));
        CHECK(restY > -0.05F && restY < 0.30F);
    }

    // The floor (anchored/static) must never move.
    const EditorObject* floor = document.find_object(1);
    CHECK(floor != nullptr);
    if (floor != nullptr) {
        CHECK(std::fabs(floor->transform.position.y - (-0.5F)) < 1.0e-5F);
    }

    simulation.stop();
    CHECK(!simulation.running());
}

void test_checkpoint_restoration_through_workspace() {
    const EditorMaterialLibrary materials = EditorMaterialLibrary::make_default();
    EditorWorkspace workspace(make_scene());

    const EditorObject* crateBefore = workspace.document().find_object(2);
    CHECK(crateBefore != nullptr);
    const Float3 originalPosition = crateBefore != nullptr ? crateBefore->transform.position : Float3{};

    std::string modeError;
    CHECK(workspace.set_mode(EditorMode::Simulate, &modeError));
    CHECK(workspace.mode() == EditorMode::Simulate);

    EditorJoltSimulation simulation;
    CHECK(simulation.start(workspace.document(), materials));

    constexpr float kFixedDelta = 1.0F / 120.0F;
    for (int step = 0; step < 120; ++step) simulation.step(workspace.document(), kFixedDelta); // 1 s of real physics

    const EditorObject* crateDuring = workspace.document().find_object(2);
    CHECK(crateDuring != nullptr);
    if (crateDuring != nullptr) {
        // After a second of falling under Jolt gravity the crate must actually have moved:
        // this is the "checkpoint restoration" test, so it only means something if the live
        // document was genuinely mutated by real physics before being restored.
        CHECK(crateDuring->transform.position.y < originalPosition.y - 0.05F);
    }

    simulation.stop();
    CHECK(workspace.set_mode(EditorMode::Edit, &modeError));
    CHECK(workspace.mode() == EditorMode::Edit);

    const EditorObject* crateAfter = workspace.document().find_object(2);
    CHECK(crateAfter != nullptr);
    if (crateAfter != nullptr) {
        CHECK(std::fabs(crateAfter->transform.position.x - originalPosition.x) < 1.0e-6F);
        CHECK(std::fabs(crateAfter->transform.position.y - originalPosition.y) < 1.0e-6F);
        CHECK(std::fabs(crateAfter->transform.position.z - originalPosition.z) < 1.0e-6F);
    }
}

} // namespace

int main() {
    test_direct_simulation();
    test_checkpoint_restoration_through_workspace();

    if (failures == 0) {
        std::printf("dve_editor_jolt_simulation_smoke: PASS\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "dve_editor_jolt_simulation_smoke: %d FAILURE(S)\n", failures);
    return EXIT_FAILURE;
}
