#include "dve/sprite_particle_renderer.hpp"

#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

namespace {
using namespace dve;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

VfxProgram make_program(bool continuous) {
    VfxParticleGraphAsset graph;
    graph.name = continuous ? "continuous" : "burst";
    graph.maximumParticles = 96U;
    graph.durationSeconds = 2.0F;
    graph.looping = continuous;
    graph.seed = continuous ? 0x2181U : 0x2182U;
    VfxModule spawn{1U, continuous ? VfxModuleKind::SpawnRate : VfxModuleKind::SpawnBurst,
                    VfxModulePhase::Spawn};
    spawn.parameters[0] = continuous ? 30.0F : 18.0F;
    VfxModule velocity{2U, VfxModuleKind::VelocityCone, VfxModulePhase::Spawn};
    velocity.parameters[0] = 2.0F;
    velocity.parameters[1] = 1.2F;
    VfxModule lifetime{3U, VfxModuleKind::LifetimeRange, VfxModulePhase::Spawn};
    lifetime.parameters[0] = 0.4F;
    lifetime.parameters[1] = 0.9F;
    VfxModule gravity{4U, VfxModuleKind::Gravity, VfxModulePhase::Update};
    gravity.parameters[1] = -1.0F;
    VfxModule color{5U, VfxModuleKind::ColorOverLife, VfxModulePhase::Update};
    color.parameters = {1.0F, 0.9F, 0.2F, 1.0F, 1.0F, 0.2F, 0.1F, 0.0F};
    VfxModule size{6U, VfxModuleKind::SizeOverLife, VfxModulePhase::Update};
    size.parameters[0] = 1.0F;
    size.parameters[1] = 0.15F;
    VfxModule renderer{7U, VfxModuleKind::BillboardRenderer, VfxModulePhase::Render};
    graph.modules = {spawn, velocity, lifetime, gravity, color, size, renderer};
    const auto compiled = compile_vfx_particle_graph(graph);
    require(compiled.program.has_value(), "particle graph did not compile");
    return *compiled.program;
}

SpriteParticleStyle style() {
    SpriteParticleStyle value;
    value.textureAsset = "assets/sprites/v218_sun_route_particles.png";
    value.atlasColumns = 4U;
    value.flipbookFramesPerSecond = 16.0F;
    value.baseSizePixels = 6.0F;
    value.velocityStretch = 0.25F;
    value.rotationRateDegrees = 140.0F;
    value.blendMode = SpriteBlendMode::Additive;
    return value;
}

void test_particles() {
    SpriteParticleSystem system(16U);
    std::string error;
    require(system.spawn(make_program(false), style(), {0.0F, 0.0F, 0.0F}, &error).has_value(), error);
    require(system.spawn(make_program(true), style(), {1.0F, 0.0F, 0.0F}, &error).has_value(), error);
    for (int frame = 0; frame < 12; ++frame) system.step(1.0F / 60.0F);

    SpriteParticleTrailDesc trail;
    trail.owner = 90U;
    trail.textureAsset = style().textureAsset;
    trail.points = {{{-1.0F, 0.0F, 0.0F}, 0.0F}, {{0.0F, 0.5F, 0.0F}, 0.1F},
                    {{1.0F, 0.0F, 0.0F}, 0.2F}};
    SpriteParticleBeamDesc beam;
    beam.owner = 91U;
    beam.textureAsset = style().textureAsset;
    beam.start = {-2.0F, 1.0F, 0.0F};
    beam.end = {2.0F, 1.0F, 0.0F};
    system.set_trails({trail});
    system.set_beams({beam});

    SpriteIntervalEvent event;
    event.owner = 7U;
    event.kind = SpriteIntervalEventKind::FrameEvent;
    event.name = "impact";
    std::map<std::string, std::pair<VfxProgram, SpriteParticleStyle>, std::less<>> registry;
    registry.emplace("impact", std::make_pair(make_program(false), style()));
    const std::map<SpriteOwnerId, Float3> positions{{7U, {3.0F, 2.0F, 0.0F}}};
    require(system.spawn_from_events(std::span<const SpriteIntervalEvent>(&event, 1U), registry,
                                     positions, &error).size() == 1U, error);
    SpriteSocketAttachmentSample socket;
    socket.id = 1U;
    socket.parent = 7U;
    socket.socket = "muzzle";
    socket.visible = true;
    socket.worldTransform.position = {4.0F, 2.0F, 0.0F};
    require(system.spawn_from_sockets(std::span<const SpriteSocketAttachmentSample>(&socket, 1U),
                                      "muzzle", make_program(false), style(), &error).size() == 1U, error);
    system.step(1.0F / 60.0F);
    const auto packet = system.build_render_packet({}, GameplayPlane2D::XY);
    require(packet.stats.emitters >= 4U && packet.stats.particles > 0U,
            "particle emitters or particles are missing");
    require(packet.stats.trailSegments == 2U && packet.stats.beams == 1U,
            "trail or beam geometry is missing");
    require(!packet.renderList.items.empty() && !packet.renderList.batches.empty(),
            "particle sprite sorting/batching output is empty");
    const auto imageA = rasterize_sprite_particles_reference(packet, 256U, 160U);
    const auto imageB = rasterize_sprite_particles_reference(packet, 256U, 160U);
    require(imageA.contentHash != 0U && imageA.contentHash == imageB.contentHash,
            "particle reference renderer is not deterministic");
}

} // namespace

int main() {
    try {
        test_particles();
        std::cout << "v2.18 sprite particles passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
