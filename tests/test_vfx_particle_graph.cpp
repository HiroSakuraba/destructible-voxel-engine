#include <iostream>
#include <stdexcept>
#include <string_view>

#include "dve/vfx_particle_graph.hpp"

namespace {
void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}
dve::VfxModule module(std::uint32_t id, dve::VfxModuleKind kind,
                      dve::VfxModulePhase phase) {
    dve::VfxModule result;
    result.id = id;
    result.kind = kind;
    result.phase = phase;
    return result;
}
}

int main() {
    try {
        dve::VfxParticleGraphAsset graph;
        graph.name = "Foundation Spark";
        graph.maximumParticles = 512U;
        graph.durationSeconds = 2.0F;
        auto rate = module(1U, dve::VfxModuleKind::SpawnRate, dve::VfxModulePhase::Spawn);
        rate.parameters[0] = 120.0F;
        auto box = module(2U, dve::VfxModuleKind::InitializeBox, dve::VfxModulePhase::Spawn);
        box.parameters = {-0.1F, 0.1F, 0.2F, 0.4F, -0.1F, 0.1F, 0.0F, 0.0F};
        auto velocity = module(3U, dve::VfxModuleKind::VelocityCone,
                               dve::VfxModulePhase::Spawn);
        velocity.parameters[0] = 1.0F;
        velocity.parameters[1] = 2.0F;
        velocity.parameters[2] = 0.3F;
        auto lifetime = module(4U, dve::VfxModuleKind::LifetimeRange,
                               dve::VfxModulePhase::Spawn);
        lifetime.parameters[0] = 0.5F;
        lifetime.parameters[1] = 1.0F;
        auto gravity = module(5U, dve::VfxModuleKind::Gravity, dve::VfxModulePhase::Update);
        gravity.parameters[1] = -9.81F;
        auto drag = module(6U, dve::VfxModuleKind::Drag, dve::VfxModulePhase::Update);
        drag.parameters[0] = 0.2F;
        auto ground = module(7U, dve::VfxModuleKind::GroundCollision,
                             dve::VfxModulePhase::Update);
        ground.parameters[0] = 0.0F;
        ground.parameters[1] = 0.4F;
        auto size = module(8U, dve::VfxModuleKind::SizeOverLife,
                           dve::VfxModulePhase::Update);
        size.parameters[0] = 0.08F;
        size.parameters[1] = 0.0F;
        auto attractor = module(9U, dve::VfxModuleKind::AttractorPoint,
                                dve::VfxModulePhase::Update);
        attractor.parameters = {0.0F, 0.5F, 0.0F, 0.2F, 10.0F, 0.0F, 0.0F, 0.0F};
        auto limiter = module(10U, dve::VfxModuleKind::VelocityLimit,
                              dve::VfxModulePhase::Update);
        limiter.parameters[0] = 4.0F;
        auto sphere = module(11U, dve::VfxModuleKind::SphereCollision,
                             dve::VfxModulePhase::Update);
        sphere.parameters = {0.0F, 0.15F, 0.0F, 0.08F, 0.5F, 0.0F, 0.0F, 0.0F};
        auto renderer = module(12U, dve::VfxModuleKind::BillboardRenderer,
                               dve::VfxModulePhase::Render);
        graph.modules = {rate, box, velocity, lifetime, gravity, drag, ground, size,
                         attractor, limiter, sphere, renderer};
        const auto compiled = dve::compile_vfx_particle_graph(graph);
        require(static_cast<bool>(compiled), compiled.error);
        dve::VfxCpuRuntime runtime(*compiled.program);
        dve::VfxRuntimeTelemetry telemetry{};
        for (int frame = 0; frame < 30; ++frame) telemetry = runtime.step(1.0F / 60.0F);
        require(telemetry.alive > 0U, "VFX runtime produced no live particles");
        require(runtime.effect_time_seconds() > 0.0F, "VFX effect time did not advance");
        require(!runtime.events().empty() && telemetry.eventsWritten > 0U,
                "VFX runtime did not emit lifecycle/collision events");
        const auto plan = dve::plan_vfx_gpu_frame(*compiled.program, true);
        std::string error;
        require(plan.validate(&error), error);
        require(plan.requiresSorting && plan.dispatches.size() == 7U,
                "transparent VFX GPU plan is incomplete");
        std::cout << "DVE VFX particle graph tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "DVE VFX particle graph tests failed: " << exception.what() << '\n';
        return 1;
    }
}
