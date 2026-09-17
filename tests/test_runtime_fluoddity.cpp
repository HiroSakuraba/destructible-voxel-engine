#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include "dve/runtime_fluoddity_world.hpp"

#ifndef DVE_TEST_SOURCE_DIR
#define DVE_TEST_SOURCE_DIR "."
#endif

#define CHECK(expression) do { if (!(expression)) throw std::runtime_error(std::string("CHECK failed: ") + #expression); } while (false)

namespace {

dve::FluoddityRuleAsset load_asset() {
    const auto path = std::filesystem::path(DVE_TEST_SOURCE_DIR) /
                      "tests/assets/fluoddity_v7_av0.json";
    auto imported = dve::import_fluoddity_preset_json(path);
    if (!imported) throw std::runtime_error(imported.error);
    return *imported.asset;
}

void run() {
    dve::RuntimeFluoddityWorld world;
    dve::FluoddityRuleAsset asset = load_asset();
    dve::RuntimeFluoddityInstance instance;
    instance.quality = dve::fluoddity_quality_profile(dve::FluoddityQuality::Low);
    instance.simulationFrequencyHz = 60.0F;
    std::string error;
    const auto objectId = world.create(asset, instance, 77U, asset.contentHash, &error);
    CHECK(objectId == 77U);
    CHECK(world.create(asset, instance, 77U, asset.contentHash, &error) == 0U);
    CHECK(world.snapshot().validate(&error));

    auto plans = world.plan_frame(1.0 / 30.0);
    CHECK(plans.size() == 1U);
    CHECK(plans.front().enabled);
    CHECK(plans.front().budgetAccepted);
    CHECK(plans.front().stepCount == 2U);
    CHECK(plans.front().particleWorkgroupsPerStep == 3907U);
    CHECK(plans.front().trailWorkgroupsPerStep[0] == 32U);
    CHECK(plans.front().computePassesPerStep == 3U);
    CHECK(plans.front().finalTrailReadIndex == 0U);
    CHECK(world.find_state(objectId)->simulationFrame == 2U);

    CHECK(world.pause(objectId));
    plans = world.plan_frame(1.0);
    CHECK(plans.front().stepCount == 0U);
    CHECK(world.step_once(objectId));
    plans = world.plan_frame(0.0);
    CHECK(plans.front().stepCount == 1U);
    CHECK(plans.front().finalTrailReadIndex == 1U);

    CHECK(world.reset(objectId));
    plans = world.plan_frame(0.0);
    CHECK(plans.front().resetBeforeStep);
    CHECK(world.find_state(objectId)->simulationFrame == 0U);
    CHECK(world.find_state(objectId)->trailReadIndex == 0U);

    auto replacement = *world.find_instance(objectId);
    replacement.quality = dve::fluoddity_quality_profile(dve::FluoddityQuality::High);
    CHECK(world.update_instance(objectId, replacement, &error));
    world.set_global_gpu_budget(64ULL * 1024ULL * 1024ULL);
    plans = world.plan_frame(0.0);
    CHECK(!plans.front().enabled);
    CHECK(!plans.front().budgetAccepted);
    CHECK(!plans.front().rejectionReason.empty());

    world.set_global_gpu_budget(2ULL * 1024ULL * 1024ULL * 1024ULL);
    plans = world.plan_frame(0.0);
    CHECK(plans.front().budgetAccepted);
    CHECK(plans.front().resetBeforeStep);

    dve::FluoddityRuleAsset changed = asset;
    changed.parameters[0].value += 0.25F;
    changed.recompute_hash();
    CHECK(world.update_asset(objectId, changed, &error));
    CHECK(world.find_instance(objectId)->assetId == changed.contentHash);
    plans = world.plan_frame(0.0);
    CHECK(plans.front().resetBeforeStep);

    CHECK(world.destroy(objectId));
    CHECK(world.snapshot().assets.empty());
}

} // namespace

int main() {
    try {
        run();
        std::cout << "runtime Fluoddity tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
