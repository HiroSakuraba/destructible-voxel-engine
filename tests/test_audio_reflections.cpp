#include "dve/audio/reflection_runtime.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
void require(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
}

int main() {
    using namespace dve::audio;
    ReflectionBudgetSettings settings;
    settings.workerBudgetMilliseconds = 1.5;
    settings.overloadSamplesToDegrade = 2;
    settings.underBudgetSamplesToRecover = 3;
    ReflectionBudgetController controller(settings);
    controller.set_requested_tier(ReflectionQualityTier::High);
    require(controller.active_tier() == ReflectionQualityTier::Medium,
            "requested quality should not force an immediate promotion");
    controller.record_worker_sample(2.0);
    controller.record_worker_sample(2.1);
    require(controller.active_tier() == ReflectionQualityTier::Low,
            "reflection quality did not degrade after sustained overload");
    controller.record_worker_sample(0.5);
    controller.record_worker_sample(0.5);
    controller.record_worker_sample(0.5);
    require(controller.active_tier() == ReflectionQualityTier::Medium,
            "reflection quality did not recover after sustained headroom");
    const auto profile = controller.profile();
    require(profile.sourceCount == 2 && profile.rays == 4096 && profile.ambisonicOrder == 1,
            "medium reflection profile mismatch");

    ReflectionFieldPublisher publisher;
    ReflectionFieldSnapshot snapshot;
    snapshot.generation = 44;
    snapshot.profile = profile;
    snapshot.simulatedSources = 2;
    snapshot.reverbTimesSeconds = {0.8F, 0.6F, 0.4F};
    snapshot.workerMilliseconds = 0.75;
    require(publisher.publish(snapshot), "valid reflection snapshot was rejected");
    require(!publisher.publish(snapshot), "stale reflection snapshot was accepted");
    snapshot.generation = 45;
    snapshot.workerMilliseconds = std::numeric_limits<double>::quiet_NaN();
    require(!publisher.publish(snapshot), "non-finite reflection timing was accepted");
    const auto published = publisher.snapshot();
    require(published && published->generation == 44 && published->profile.ambisonicOrder == 1,
            "reflection snapshot publication mismatch");
    const auto metrics = publisher.metrics();
    require(metrics.acceptedGenerations == 1 && metrics.rejectedGenerations == 2,
            "reflection publisher metrics mismatch");
    std::cout << "DVE reflection budget/publication tests passed\n";
}
