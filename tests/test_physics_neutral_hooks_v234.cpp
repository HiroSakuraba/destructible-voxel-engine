#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

#include "dve/rigid_body_adapter.hpp"

namespace {

void require(bool value, const char* message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main() {
    using namespace dve;

    ReferenceRigidBodyWorld world;
    world.set_gravity({});

    RigidBodyCreateDesc body;
    body.transform = make_rigid_transform({0.0F, 2.0F, 0.0F}, {});
    body.massKilograms = 2.0;
    body.inertiaKilogramMetersSquared = {1.0, 1.0, 1.0, 0.0, 0.0, 0.0};
    body.boxes.push_back({{}, {0.5F, 0.5F, 0.5F}});

    const RigidBodyHandle handle = world.create_body(body);
    require(handle != kInvalidRigidBodyHandle, "reference body creation failed");

    const auto beforeForce = world.state(handle);
    require(beforeForce.has_value(), "reference body state missing");
    require(world.apply_force_at_point(handle, {0.0F, 120.0F, 0.0F}, {1.0F, 2.0F, 0.0F}),
            "neutral point force was rejected");
    // Forces accumulate until the next step, which integrates them over its actual duration.
    const auto pendingForce = world.pending_loads(handle);
    require(pendingForce.has_value() && pendingForce->force.y == 120.0F &&
                (pendingForce->torque.x != 0.0F || pendingForce->torque.y != 0.0F ||
                 pendingForce->torque.z != 0.0F),
            "reference point force was not accumulated as a pending load");
    const auto beforeStep = world.state(handle);
    require(beforeStep.has_value() && beforeStep->linearVelocity.y == beforeForce->linearVelocity.y,
            "reference point force changed velocity before the step");
    world.step(0.5F);
    const auto afterForce = world.state(handle);
    require(afterForce.has_value() &&
                std::abs((afterForce->linearVelocity.y - beforeForce->linearVelocity.y) - 30.0F) < 1.0e-4F,
            "reference point-force fallback did not preserve linear loading");

    const float beforeImpulseX = afterForce->linearVelocity.x;
    require(world.apply_impulse_at_point(handle, {2.0F, 0.0F, 0.0F}, {0.0F, 2.0F, 1.0F}),
            "neutral point impulse was rejected");
    const auto afterImpulse = world.state(handle);
    require(afterImpulse.has_value() && afterImpulse->linearVelocity.x > beforeImpulseX,
            "reference point-impulse fallback did not preserve linear loading");

    require(!world.apply_force_at_point(
                handle, {0.0F, 1.0F, 0.0F},
                {std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F}),
            "non-finite point force was accepted");
    world.set_contact_sink(nullptr);
    require(world.set_contact_material(handle, 3U),
            "reference backend rejected a valid contact material");

    std::cout << "DVE v2.34 solver-neutral physics hook tests passed\n";
    return 0;
}
