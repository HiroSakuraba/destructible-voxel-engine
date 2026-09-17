#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include "dve/soft_body.hpp"

namespace {
void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}
}

int main() {
    try {
        const auto cloth = dve::make_soft_body_cloth({8U, 6U, 0.1F, 0.02F, 1.0e-7F,
                                                       1.0e-4F, true});
        const auto rope = dve::make_soft_body_rope({12U, 0.08F, 0.02F, 1.0e-7F,
                                                     1.0e-4F, true});
        const auto plant = dve::make_soft_body_vegetation({10U, 0.06F, 0.01F, 1.0e-7F,
                                                            5.0e-4F, true});
        const auto box = dve::make_soft_body_deformable_box({{2U, 2U, 2U}, 0.12F, 0.05F,
                                                              1.0e-6F, 1.0e-7F});
        std::string error;
        require(cloth.validate(&error), error);
        require(rope.validate(&error), error);
        require(plant.validate(&error), error);
        require(box.validate(&error), error);
        require(!cloth.faces.empty() && !cloth.stretchConstraints.empty(),
                "cloth topology is incomplete");
        require(cloth.bendModel == dve::SoftBodyBendModel::DihedralShell &&
                !cloth.dihedralConstraints.empty(),
                "cloth did not build true dihedral shell constraints");
        require(rope.kind == dve::SoftBodyKind::Rope &&
                plant.kind == dve::SoftBodyKind::Vegetation,
                "rope and vegetation kinds were not preserved");
        require(!box.volumeConstraints.empty(), "deformable box has no volume constraints");
        const auto jolt = dve::make_jolt_soft_body_recipe(cloth);
        require(jolt.vertices.size() == cloth.vertices.size() &&
                jolt.faces.size() == cloth.faces.size() &&
                jolt.dihedrals.size() == cloth.dihedralConstraints.size(),
                "Jolt soft-body recipe lost topology");
        require(jolt.edges.size() == cloth.stretchConstraints.size() +
                                     cloth.bendConstraints.size(),
                "Jolt soft-body recipe lost constraints");

        dve::SoftBodyWorld world;
        dve::RuntimeSoftBodyInstance instance;
        instance.groundHeight = -1.0F;
        instance.solverIterations = 10U;
        const auto id = world.create(cloth, instance, &error);
        require(id != 0U, error);
        const auto* initial = world.find_state(id);
        require(initial != nullptr, "soft-body runtime state was not created");
        const float pinnedY = initial->positions.front().y;
        const float freeY = initial->positions.back().y;
        dve::SoftBodyStepTelemetry telemetry{};
        for (int step = 0; step < 12; ++step)
            telemetry = world.step(1.0F / 120.0F, {0.0F, -9.81F, 0.0F});
        const auto* advanced = world.find_state(id);
        require(advanced != nullptr, "soft-body runtime state disappeared");
        require(std::abs(advanced->positions.front().y - pinnedY) < 1.0e-5F,
                "pinned cloth vertex moved");
        require(advanced->positions.back().y < freeY,
                "free cloth vertex did not respond to gravity");
        require(telemetry.distanceConstraintsSolved > 0U &&
                telemetry.dihedralConstraintsSolved > 0U,
                "soft-body XPBD shell constraints were not solved");
        require(telemetry.substeps > 0U, "soft-body adaptive stepping was not used");
        require(telemetry.nonFiniteCorrections == 0U,
                "soft-body solver required non-finite repair");
        std::cout << "DVE soft-body tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "DVE soft-body tests failed: " << exception.what() << '\n';
        return 1;
    }
}
