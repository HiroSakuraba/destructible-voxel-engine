#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include "dve/cpu_hair.hpp"
#include "dve/cpu_hair_runtime.hpp"
#include "dve/game_world.hpp"

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

bool nearly_equal(dve::Float3 a, dve::Float3 b, float tolerance = 1.0e-5F) {
    return std::abs(a.x - b.x) <= tolerance &&
           std::abs(a.y - b.y) <= tolerance &&
           std::abs(a.z - b.z) <= tolerance;
}

std::filesystem::path temporary_path(std::string_view name) {
    return std::filesystem::temp_directory_path() / std::string(name);
}

} // namespace

int main() {
    try {
        std::string error;
        dve::HairAsset groom = dve::make_straight_hair_groom(64U, 12U, 0.01F, 0.02F);
        require(groom.validate(&error), error);
        require(groom.guides.size() == 64U && groom.point_count() == 768U,
                "straight groom topology is wrong");

        const std::filesystem::path assetPath = temporary_path("dve_cpu_hair_test.dvehair");
        require(dve::write_dvehair(assetPath, groom, &error), error);
        const dve::HairAssetReadResult reloaded = dve::read_dvehair(assetPath);
        require(static_cast<bool>(reloaded), reloaded.error);
        require(reloaded.asset.contentHash == groom.contentHash,
                "DVE hair round trip changed the content hash");

        const std::filesystem::path plyPath = temporary_path("dve_cpu_hair_sisir_test.ply");
        {
            std::ofstream stream(plyPath, std::ios::binary | std::ios::trunc);
            stream << "ply\nformat ascii 1.0\n"
                   << "element vertex 6\n"
                   << "property float x\nproperty float y\nproperty float z\n"
                   << "property uchar anchor\nproperty int layer_id\nproperty int curve_id\n"
                   << "end_header\n"
                   << "0 0 0 1 3 0\n0 -0.1 0 0 3 0\n0 -0.2 0 0 3 0\n"
                   << "0.1 0 0 1 4 1\n0.1 -0.1 0 0 4 1\n0.1 -0.2 0 0 4 1\n";
        }
        const dve::HairAssetReadResult imported = dve::read_sisir_hair_ply(plyPath);
        require(static_cast<bool>(imported), imported.error);
        require(imported.asset.guides.size() == 2U && imported.asset.guides[0].layerId == 3U &&
                imported.asset.guides[1].layerId == 4U,
                "Sisir PLY grouping or layer import failed");

        const std::filesystem::path invalidPlyPath =
            temporary_path("dve_cpu_hair_invalid_sisir_test.ply");
        {
            std::ofstream stream(invalidPlyPath, std::ios::binary | std::ios::trunc);
            stream << "ply\nformat ascii 1.0\n"
                   << "element vertex 2\n"
                   << "property float x\nproperty float y\nproperty float z\n"
                   << "property int curve_id\nend_header\n"
                   << "0 0 0 0.5\n0 -0.1 0 0.5\n";
        }
        require(!dve::read_sisir_hair_ply(invalidPlyPath),
                "Sisir PLY importer accepted a fractional curve id");

        dve::CpuHairInstanceDesc desc;
        desc.solver.solverIterations = 6U;
        desc.solver.maximumSubsteps = 4U;
        desc.solver.workerChunkGuides = 7U;
        desc.solver.enableSleeping = false;
        desc.windVelocity = {1.5F, 0.0F, 0.0F};
        desc.collision.plane = {{0.0F, 1.0F, 0.0F}, -0.30F, true};

        dve::CpuHairWorld serialWorld(0U);
        dve::CpuHairWorld parallelWorld(2U);
        const dve::CpuHairId serialId = serialWorld.create(groom, desc, &error);
        const dve::CpuHairId parallelId = parallelWorld.create(groom, desc, &error);
        require(serialId != 0U && parallelId != 0U, error);
        const dve::CpuHairView initial = serialWorld.view(serialId);
        require(initial.positions.size() == groom.point_count() &&
                initial.lineIndices.size() == (groom.point_count() - groom.guides.size()) * 2U,
                "CPU hair render view is incomplete");
        const dve::Float3 pinned = initial.positions.front();
        const dve::Float3 initialTip = initial.positions[11U];

        dve::CpuHairStepTelemetry telemetry{};
        for (int frame = 0; frame < 30; ++frame) {
            telemetry = serialWorld.step(1.0F / 60.0F);
            (void)parallelWorld.step(1.0F / 60.0F);
        }
        const dve::CpuHairView serialView = serialWorld.view(serialId);
        const dve::CpuHairView parallelView = parallelWorld.view(parallelId);
        require(nearly_equal(serialView.positions.front(), pinned),
                "anchored CPU hair root moved");
        require(serialView.positions[11U].x > initialTip.x,
                "CPU hair did not respond to wind");
        require(telemetry.pointsIntegrated > 0U && telemetry.stretchConstraintsSolved > 0U &&
                telemetry.bendConstraintsSolved > 0U && telemetry.workerBatches > 0U,
                "CPU hair telemetry did not record solver work");
        require(telemetry.nonFiniteCorrections == 0U,
                "CPU hair needed non-finite repair");
        require(serialView.positions.size() == parallelView.positions.size(),
                "serial and parallel hair views differ in size");
        for (std::size_t point = 0U; point < serialView.positions.size(); ++point) {
            require(nearly_equal(serialView.positions[point], parallelView.positions[point], 2.0e-5F),
                    "parallel CPU hair diverged from serial execution");
        }

        const dve::RigidTransform movedRoot{{0.25F, 0.0F, 0.0F}, {}};
        require(serialWorld.set_root_transform(serialId, movedRoot, true),
                "could not teleport CPU hair root");
        (void)serialWorld.step(1.0F / 60.0F);
        require(serialWorld.view(serialId).positions.front().x > pinned.x + 0.24F,
                "CPU hair root transform was not applied");

        dve::CpuHairSolverSettings lod = desc.solver;
        lod.maximumActiveGuides = 8U;
        require(serialWorld.set_solver_settings(serialId, lod, &error), error);
        const dve::CpuHairStepTelemetry lodTelemetry = serialWorld.step(1.0F / 60.0F);
        require(lodTelemetry.frozenGuides == 56U && lodTelemetry.simulatedGuides <= 16U,
                "CPU hair guide-budget LOD did not freeze the expected guides");

        dve::CpuHairInstanceDesc reducedRateDesc;
        reducedRateDesc.gravity = {};
        reducedRateDesc.solver.enableSleeping = false;
        reducedRateDesc.solver.updateRateDivisor = 2U;
        reducedRateDesc.solver.maximumSubsteps = 2U;
        dve::CpuHairWorld reducedRateWorld(0U);
        const dve::CpuHairId reducedRateId = reducedRateWorld.create(
            dve::make_straight_hair_groom(1U, 5U, 0.01F, 0.03F),
            reducedRateDesc, &error);
        require(reducedRateId != 0U, error);
        const dve::CpuHairStepTelemetry reducedFirst = reducedRateWorld.step(1.0F / 60.0F);
        const dve::CpuHairStepTelemetry reducedSecond = reducedRateWorld.step(1.0F / 60.0F);
        require(reducedFirst.substeps == 0U && reducedSecond.substeps == 2U &&
                reducedSecond.droppedTimeSeconds < 1.0e-8,
                "CPU hair update-rate division did not preserve accumulated simulation time");

        dve::HairAsset collisionGroom = dve::make_straight_hair_groom(1U, 5U, 0.01F, 0.03F);
        dve::CpuHairInstanceDesc collisionDesc;
        collisionDesc.gravity = {};
        collisionDesc.solver.enableSleeping = false;
        collisionDesc.collision.spheres.push_back({{0.0F, -0.06F, 0.0F}, 0.025F});
        dve::CpuHairWorld collisionWorld(0U);
        const dve::CpuHairId collisionId = collisionWorld.create(collisionGroom, collisionDesc, &error);
        require(collisionId != 0U, error);
        (void)collisionWorld.apply_impulse(collisionId, {0.1F, 0.0F, 0.0F});
        const dve::CpuHairStepTelemetry collisionTelemetry = collisionWorld.step(1.0F / 30.0F);
        require(collisionTelemetry.collisionTests > 0U &&
                collisionTelemetry.collisionProjections > 0U,
                "CPU hair sphere collision path was not exercised");

        dve::HairAsset selfCollisionGroom;
        selfCollisionGroom.name = "Self Collision Test";
        selfCollisionGroom.pointRadiusMeters = 0.004F;
        selfCollisionGroom.stretchCompliance = 1.0e-8F;
        selfCollisionGroom.bendCompliance = 1.0e-6F;
        for (float rootX : {-0.002F, 0.002F}) {
            dve::HairGuide guide;
            guide.points = {{rootX, 0.0F, 0.0F}, {rootX, -0.03F, 0.0F},
                            {rootX, -0.06F, 0.0F}, {rootX, -0.09F, 0.0F}};
            guide.anchored = {1U, 0U, 0U, 0U};
            selfCollisionGroom.guides.push_back(std::move(guide));
        }
        selfCollisionGroom.recompute_hash();
        dve::CpuHairInstanceDesc selfCollisionDesc;
        selfCollisionDesc.gravity = {};
        selfCollisionDesc.solver.enableSleeping = false;
        selfCollisionDesc.solver.enableSelfCollision = true;
        selfCollisionDesc.solver.selfCollisionIterations = 2U;
        selfCollisionDesc.solver.selfCollisionMaximumNeighbors = 32U;
        selfCollisionDesc.solver.selfCollisionStiffness = 1.0F;
        selfCollisionDesc.solver.solverIterations = 4U;
        selfCollisionDesc.solver.maximumSubsteps = 1U;
        dve::CpuHairWorld selfCollisionSerial(0U);
        dve::CpuHairWorld selfCollisionParallel(2U);
        const dve::CpuHairId selfCollisionSerialId = selfCollisionSerial.create(
            selfCollisionGroom, selfCollisionDesc, &error);
        const dve::CpuHairId selfCollisionParallelId = selfCollisionParallel.create(
            selfCollisionGroom, selfCollisionDesc, &error);
        require(selfCollisionSerialId != 0U && selfCollisionParallelId != 0U, error);
        const dve::CpuHairView selfInitial = selfCollisionSerial.view(selfCollisionSerialId);
        const float initialSeparation = std::abs(selfInitial.positions[1U].x -
                                                 selfInitial.positions[5U].x);
        const dve::CpuHairStepTelemetry selfTelemetry = selfCollisionSerial.step(1.0F / 120.0F);
        (void)selfCollisionParallel.step(1.0F / 120.0F);
        const dve::CpuHairView selfSerialView = selfCollisionSerial.view(selfCollisionSerialId);
        const dve::CpuHairView selfParallelView = selfCollisionParallel.view(selfCollisionParallelId);
        const float separated = std::abs(selfSerialView.positions[1U].x -
                                         selfSerialView.positions[5U].x);
        require(selfTelemetry.selfCollisionPoints > 0U &&
                selfTelemetry.selfCollisionTests > 0U &&
                selfTelemetry.selfCollisionProjections > 0U &&
                selfTelemetry.selfCollisionHashInsertFailures == 0U,
                "CPU hair self-collision path was not exercised cleanly");
        require(separated > initialSeparation + 0.002F,
                "CPU hair self-collision did not separate overlapping guides");
        for (std::size_t point = 0U; point < selfSerialView.positions.size(); ++point) {
            require(nearly_equal(selfSerialView.positions[point],
                                 selfParallelView.positions[point], 2.0e-5F),
                    "parallel self-collision diverged from serial execution");
        }

        dve::CpuHairSolverSettings selfCollisionLod = selfCollisionDesc.solver;
        selfCollisionLod.maximumSelfCollisionGuides = 1U;
        require(selfCollisionSerial.set_solver_settings(selfCollisionSerialId,
                                                        selfCollisionLod, &error), error);
        require(selfCollisionSerial.reset(selfCollisionSerialId),
                "could not reset self-collision test groom");
        const dve::CpuHairStepTelemetry selfLodTelemetry =
            selfCollisionSerial.step(1.0F / 120.0F);
        require(selfLodTelemetry.selfCollisionPoints == 6U &&
                selfLodTelemetry.selfCollisionTests == 0U &&
                selfLodTelemetry.selfCollisionProjections == 0U,
                "CPU hair self-collision guide LOD did not limit participation");

        dve::CpuHairInstanceDesc sleepingSelfCollisionDesc;
        sleepingSelfCollisionDesc.gravity = {};
        sleepingSelfCollisionDesc.solver.enableSleeping = true;
        sleepingSelfCollisionDesc.solver.sleepFrames = 1U;
        sleepingSelfCollisionDesc.solver.enableSelfCollision = true;
        sleepingSelfCollisionDesc.solver.selfCollisionIterations = 1U;
        sleepingSelfCollisionDesc.solver.maximumSubsteps = 1U;
        dve::CpuHairWorld sleepingSelfCollisionWorld(0U);
        const dve::CpuHairId sleepingSelfCollisionId = sleepingSelfCollisionWorld.create(
            dve::make_straight_hair_groom(2U, 5U, 0.02F, 0.03F),
            sleepingSelfCollisionDesc, &error);
        require(sleepingSelfCollisionId != 0U, error);
        const dve::CpuHairStepTelemetry sleepingFirst =
            sleepingSelfCollisionWorld.step(1.0F / 120.0F);
        const dve::CpuHairStepTelemetry sleepingSecond =
            sleepingSelfCollisionWorld.step(1.0F / 120.0F);
        require(sleepingFirst.selfCollisionPoints > 0U &&
                sleepingSecond.sleepingGuides == 2U &&
                sleepingSecond.selfCollisionPoints == 0U,
                "sleeping CPU hair did not skip settled self-collision work");

        dve::CpuHairSolverSettings invalidSelfCollision = selfCollisionDesc.solver;
        invalidSelfCollision.selfCollisionStiffness = 1.1F;
        require(!selfCollisionSerial.set_solver_settings(selfCollisionSerialId,
                                                         invalidSelfCollision, &error),
                "CPU hair accepted invalid self-collision stiffness");

        dve::GameWorld gameWorld(std::make_unique<dve::ReferenceRigidBodyWorld>());
        dve::GameObjectDesc ownerDesc;
        ownerDesc.name = "Hair owner";
        const dve::GameObjectId owner = gameWorld.create_object(std::move(ownerDesc), &error);
        require(owner != dve::kInvalidGameObjectId, error);
        dve::CpuHairBindOptions bindOptions;
        bindOptions.simulation.gravity = {};
        bindOptions.simulation.solver.enableSleeping = false;
        require(gameWorld.bind_cpu_hair(owner, collisionGroom, bindOptions, &error), error);
        require(gameWorld.cpu_hair().contains(owner),
                "GameWorld did not retain its CPU hair binding");
        const std::uint64_t frameBeforeMove = gameWorld.cpu_hair().view(owner).simulationFrame;
        require(gameWorld.set_position(owner, {1.0F, 0.0F, 0.0F}),
                "GameWorld rejected movement of a CPU hair owner");
        gameWorld.tick(1.0F / 60.0F);
        const dve::CpuHairView movedView = gameWorld.cpu_hair().view(owner);
        require(movedView.simulationFrame > frameBeforeMove && movedView.positions.front().x > 0.99F,
                "GameWorld did not synchronize the owner transform into CPU hair");
        require(gameWorld.set_enabled(owner, false),
                "could not disable CPU hair owner");
        const std::uint64_t disabledFrame = gameWorld.cpu_hair().view(owner).simulationFrame;
        gameWorld.tick(1.0F / 60.0F);
        require(gameWorld.cpu_hair().view(owner).simulationFrame == disabledFrame,
                "disabled GameWorld CPU hair continued simulating");
        require(gameWorld.destroy_object(owner), "could not destroy CPU hair owner");
        require(!gameWorld.cpu_hair().contains(owner),
                "destroyed GameWorld owner retained its CPU hair binding");

        std::error_code ignored;
        std::filesystem::remove(assetPath, ignored);
        std::filesystem::remove(plyPath, ignored);
        std::filesystem::remove(invalidPlyPath, ignored);
        std::cout << "DVE CPU hair tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "DVE CPU hair tests failed: " << exception.what() << '\n';
        return 1;
    }
}
