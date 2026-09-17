#include "dve/audio/physics_contact_sink_adapter.hpp"

#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
}

int main() {
    using namespace dve;
    using namespace dve::audio;
    PhysicsContactAccumulatorSettings settings;
    settings.spatialCellMeters = 0.1F;
    settings.aggregationWindowSeconds = 0.01;
    settings.capacity = 256;
    PhysicsContactAudioAccumulator accumulator(settings);
    PhysicsContactAccumulatorSink sink(accumulator);

    constexpr std::size_t kThreads = 4;
    constexpr std::size_t kPerThread = 64;
    std::vector<std::thread> workers;
    for (std::size_t threadIndex = 0; threadIndex < kThreads; ++threadIndex) {
        workers.emplace_back([&, threadIndex] {
            for (std::size_t index = 0; index < kPerThread; ++index) {
                PhysicsContactEvent event;
                event.bodyA = threadIndex * 1000U + index + 1U;
                event.bodyB = event.bodyA + 50000U;
                event.materialA = 2;
                event.materialB = 5;
                event.position = {static_cast<float>(index) * 0.2F,
                                  static_cast<float>(threadIndex), 0.0F};
                event.relativeVelocity = {0.0F, -2.0F, 0.0F};
                event.contactNormal = {0.0F, 1.0F, 0.0F};
                event.effectiveMass = 2.0F;
                event.timeSeconds = 1.0 + static_cast<double>(index) * 0.02;
                require(sink.record_contact(event), "bounded MPSC queue unexpectedly rejected contact");
            }
        });
    }
    for (auto& worker : workers) worker.join();

    std::vector<PhysicsContactAudio> output;
    const std::size_t emitted = sink.drain(10.0, output, true);
    require(emitted == kThreads * kPerThread, "contact sink drain lost or merged distinct records");
    require(output.size() == emitted, "contact sink output size mismatch");
    sink.body_removed(7);
    const auto metrics = sink.metrics();
    require(metrics.queuedContacts == kThreads * kPerThread,
            "queued contact telemetry mismatch");
    require(metrics.rejectedContacts == 0 && metrics.drainedContacts == kThreads * kPerThread,
            "contact sink queue telemetry mismatch");
    require(metrics.removedBodyNotifications == 1,
            "body removal telemetry mismatch");
    std::cout << "DVE physics contact sink tests passed\n";
}
