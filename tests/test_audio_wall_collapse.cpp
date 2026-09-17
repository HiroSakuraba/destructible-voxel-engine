#include "dve/audio/destruction_commit_coordinator.hpp"
#include "dve/audio/spatial_transition.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

dve::audio::AudioEventAsset collapse_event() {
    using namespace dve::audio;
    AudioEventAsset asset;
    asset.graph.name = "destruction/wall/collapse";
    asset.graph.root = 0;
    asset.graph.nodes.resize(1);
    asset.graph.nodes[0].type = AudioEventNodeType::SynthNote;
    asset.graph.nodes[0].note = 36;
    asset.graph.nodes[0].velocity = 1.0F;
    asset.graph.nodes[0].durationSeconds = 0.2F;
    asset.graph.nodes[0].priority = AudioPriority::Critical;
    return asset;
}

dve::audio::AcousticBuildRequest closed_wall_request(std::uint64_t generation) {
    using namespace dve::audio;
    AcousticBuildRequest request;
    request.sourceGeneration = generation;
    request.width = 8;
    request.height = 4;
    request.depth = 4;
    request.cellSizeMeters = 0.5F;
    request.origin = {-2.0F, 0.0F, -1.0F};
    request.resetGrid = true;
    AcousticBrickUpdate update;
    update.width = 8;
    update.height = 4;
    update.depth = 4;
    update.cells.resize(8U * 4U * 4U);
    for (std::uint32_t z = 0; z < 4; ++z) {
        for (std::uint32_t y = 0; y < 4; ++y) {
            const std::size_t index = 4U + 8U * (y + 4U * z);
            update.cells[index] = {255, 1, 0, 0};
        }
    }
    request.dirtyBricks.push_back(std::move(update));
    return request;
}

dve::audio::AcousticBuildRequest open_wall_request(std::uint64_t generation) {
    using namespace dve::audio;
    AcousticBuildRequest request;
    request.sourceGeneration = generation;
    request.width = 8;
    request.height = 4;
    request.depth = 4;
    request.cellSizeMeters = 0.5F;
    request.origin = {-2.0F, 0.0F, -1.0F};
    request.resetGrid = false;
    AcousticBrickUpdate update;
    update.originX = 4;
    update.width = 1;
    update.height = 4;
    update.depth = 4;
    update.cells.resize(1U * 4U * 4U); // empty cells remove the wall
    request.dirtyBricks.push_back(std::move(update));
    return request;
}
} // namespace

int main() {
    using namespace dve::audio;

    // The Jolt-facing accumulator is deterministic and independent of Jolt headers. Reversed
    // body order and multiple solver contacts collapse into one perceptual record.
    PhysicsContactAudioAccumulator contacts({0.5F, 0.1, 16});
    PhysicsContactSample first;
    first.bodyA = 12;
    first.bodyB = 44;
    first.materialA = 1;
    first.materialB = 2;
    first.position = {0.1F, 0.5F, 0.1F};
    first.relativeVelocity = {0.0F, -3.0F, 0.0F};
    first.contactNormal = {0.0F, 1.0F, 0.0F};
    first.effectiveMass = 5.0F;
    first.timeSeconds = 1.02;
    require(contacts.record(first), "first contact was rejected");
    PhysicsContactSample second = first;
    second.bodyA = 44;
    second.bodyB = 12;
    second.materialA = 2;
    second.materialB = 1;
    second.position.x = 0.2F;
    second.normalImpulse = 4.0F;
    second.timeSeconds = 1.05;
    require(contacts.record(second), "second contact was rejected");
    std::vector<PhysicsContactAudio> aggregated;
    require(contacts.drain(1.2, aggregated) == 1 && aggregated.size() == 1,
            "contact samples did not aggregate by pair/material/cell/window");
    require(aggregated[0].normalImpulse > 18.9F,
            "estimated and explicit contact impulses were not accumulated");

    AudioMixer mixer(48000);
    AudioEventLibrary events;
    std::string error;
    require(events.register_asset(collapse_event(), &error), error.c_str());
    DestructionAudioRuntime runtime(mixer, events);
    runtime.set_default_binding({1, "destruction/wall/collapse", "destruction/wall/collapse",
                                 "destruction/wall/collapse", "destruction/wall/collapse"});
    AsyncAcousticPublisher publisher;
    DestructionAudioIngress ingress;
    DestructionCommitCoordinator coordinator;

    require(coordinator.begin(100, 1.0, &error), error.c_str());
    require(coordinator.set_acoustic_build(closed_wall_request(999)),
            "closed acoustic request was rejected");
    require(coordinator.commit(ingress, &error), error.c_str());
    require(ingress.drain(runtime, publisher, 1.0, true) == 0,
            "enclosure-only commit unexpectedly emitted an event");
    publisher.wait_idle();
    auto snapshot = publisher.snapshot();
    require(snapshot && snapshot->statistics.sourceGeneration == 100,
            "closed wall snapshot lost authoritative generation identity");

    auto sharedPublisher = std::shared_ptr<const AsyncAcousticPublisher>(&publisher,
                                                                         [](const auto*) {});
    PublishedAcousticSpatializer spatializer(sharedPublisher);
    AudioListenerState listener;
    listener.position = {-1.5F, 1.0F, 0.0F};
    listener.forward = {1.0F, 0.0F, 0.0F};
    AudioEmitterState emitter;
    emitter.position = {1.5F, 1.0F, 0.0F};
    const SpatializationResult closed = spatializer.spatialize(listener, emitter);

    require(coordinator.begin(101, 1.25, &error), error.c_str());
    for (int index = 0; index < 64; ++index) {
        VoxelAudioEdit edit;
        edit.position = {0.25F, 0.5F + 0.01F * static_cast<float>(index), 0.0F};
        edit.velocity = {2.0F, -1.0F, 0.0F};
        edit.material = 1;
        edit.removedVolume = 0.125F;
        edit.fractureArea = 0.25F;
        require(coordinator.record(edit), "voxel edit exceeded transaction limits");
    }
    require(coordinator.record(aggregated[0]), "aggregated Jolt contact was rejected");
    require(coordinator.set_acoustic_build(open_wall_request(0)),
            "open acoustic request was rejected");
    require(coordinator.commit(ingress, &error), error.c_str());
    const std::size_t actions = ingress.drain(runtime, publisher, 2.0, true);
    require(actions > 0, "wall-collapse commit did not dispatch event audio");
    publisher.wait_idle();
    snapshot = publisher.snapshot();
    require(snapshot && snapshot->statistics.sourceGeneration == 101,
            "open acoustic snapshot did not use collapse event generation");
    const SpatializationResult open = spatializer.spatialize(listener, emitter);
    require(open.lowPassHertz > closed.lowPassHertz + 5000.0F,
            "wall removal did not audibly open the direct path");
    require(open.distanceGain > closed.distanceGain * 1.5F,
            "wall removal did not restore sufficient direct gain");

    SpatializationTransition transition(48000);
    transition.reset(100, closed);
    require(transition.publish_target(101, open, 0.10F),
            "open acoustic generation was not accepted by transition mailbox");
    const auto halfTransition = transition.advance(2400);
    require(halfTransition.lowPassHertz > closed.lowPassHertz &&
                halfTransition.lowPassHertz < open.lowPassHertz,
            "scene transition did not crossfade direct-path filtering");
    const auto completeTransition = transition.advance(3000);
    require(std::fabs(completeTransition.lowPassHertz - open.lowPassHertz) < 0.1F,
            "scene transition did not complete at the open acoustic state");
    require(!transition.publish_target(100, closed, 0.10F),
            "scene transition accepted a stale generation");
    const auto transitionMetrics = transition.metrics();
    require(transitionMetrics.acceptedTargets == 1 && transitionMetrics.rejectedTargets == 1 &&
                transitionMetrics.completedTargets == 1 && transitionMetrics.activeGeneration == 101,
            "scene transition generation metrics mismatch");

    const auto ingressMetrics = ingress.metrics();
    const auto coordinatorMetrics = coordinator.metrics();
    const auto contactMetrics = contacts.metrics();
    require(ingressMetrics.lastDrainedGeneration == 101 &&
                ingressMetrics.acousticRequests == 2 &&
                coordinatorMetrics.lastCommittedGeneration == 101 &&
                contactMetrics.emittedContacts == 1,
            "vertical-slice generation/producer metrics mismatch");

    std::array<float, 1024> output{};
    mixer.render(output);
    require(mixer.meters().synthVoices > 0,
            "committed collapse event did not reach the audio mixer");

    std::cout << "DVE authoritative wall-collapse audio/acoustic vertical slice passed\n";
    std::cout << "closed_lowpass_hz=" << closed.lowPassHertz
              << " open_lowpass_hz=" << open.lowPassHertz
              << " actions=" << actions << " generation=101\n";
    return 0;
}
