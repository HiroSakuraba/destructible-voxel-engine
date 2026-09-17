#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include "dve/fluoddity.hpp"

#ifndef DVE_TEST_SOURCE_DIR
#define DVE_TEST_SOURCE_DIR "."
#endif

namespace {
void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}
void require_near(float actual, float expected, float tolerance, std::string_view message) {
    if (std::abs(actual - expected) > tolerance) {
        throw std::runtime_error(std::string(message) + ": actual=" + std::to_string(actual) +
                                 " expected=" + std::to_string(expected));
    }
}
}

int main() {
    try {
        const std::filesystem::path source =
            std::filesystem::path(DVE_TEST_SOURCE_DIR) / "tests/assets/fluoddity_v7_av0.json";
        dve::FluoddityImportResult imported = dve::import_fluoddity_preset_json(source);
        require(static_cast<bool>(imported), imported.error);
        require(imported.asset->sourceVersion == 7U, "source version was not imported");
        require(imported.asset->sourcePreset == "fluoddity_v7_av0.json",
                "single-preset provenance retained an absolute path");
        require(imported.asset->cohortCount == 32U, "cohort count was not imported");
        require(imported.asset->sourceCanvasResolution == 256U, "canvas hint was not imported");
        require(imported.asset->trailMode == dve::FluoddityTrailMode::VelocityRgb,
                "legacy RGB trail semantics changed during import");
        require_near(imported.asset->rule[0].frequency[0], 2.40087795F, 1.0e-6F,
                     "rule layout changed");
        require_near(imported.asset->parameters[static_cast<std::size_t>(dve::FluoddityParameter::SensorGain)].value,
                     3.67799997F, 1.0e-6F, "sensor gain was not imported");

        const std::array<float, 6> input{0.1F, -0.2F, 0.3F, -0.4F, 0.5F, -0.6F};
        const std::array<float, 6> evaluated = dve::evaluate_fluoddity_rule(imported.asset->rule, input);
        const std::array<float, 6> expected{1.07312381F, -0.89942813F, 1.86445403F,
                                             -1.21004689F, 0.21998835F, -0.14980637F};
        for (std::size_t index = 0U; index < expected.size(); ++index) {
            require_near(evaluated[index], expected[index], 2.0e-5F,
                         "CPU Fourier evaluator diverged from the source equation");
        }

        const dve::FluoddityRule generated =
            dve::generate_fluoddity_rule_from_imported_seed(0.296842358F);
        require_near(generated[0].frequency[0], -0.55813813F, 2.0e-6F,
                     "source-compatible seed generation changed");
        require_near(generated[0].amplitudeExtension[1], -0.35342700F, 2.0e-6F,
                     "extension seed generation changed");
        const dve::FluoddityRule mutated = dve::mutate_fluoddity_rule(generated, 0.1F, 3.0F);
        require(mutated[0].amplitude[0] != generated[0].amplitude[0],
                "rule mutation did not change amplitude");

        dve::FluoddityParameterSetting setting;
        setting.value = 10.0F;
        setting.minimum = 0.0F;
        setting.maximum = 20.0F;
        setting.defaultMinimum = 0.0F;
        setting.defaultMaximum = 20.0F;
        setting.xSweep = 1.0F;
        setting.ySweep = -1.0F;
        const float swept = dve::evaluate_fluoddity_parameter(setting, 0.0F, 0.0F, 0.0F, 4U);
        require_near(swept, 10.0F, 1.0e-6F, "parameter sweep averaging changed");

        const std::filesystem::path root =
            std::filesystem::temp_directory_path() / "dve_fluoddity_tests";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        const std::filesystem::path presetTree = root / "preset_tree" / "Core";
        std::filesystem::create_directories(presetTree);
        std::filesystem::copy_file(source, presetTree / "AV0.json");
        const dve::FluoddityPresetBatchResult batch =
            dve::import_fluoddity_preset_directory(root / "preset_tree");
        require(batch.errors.empty() && batch.assets.size() == 1U,
                "preset directory import failed");
        require(batch.assets.front().sourcePreset == "Core/AV0.json",
                "directory import did not canonicalize relative provenance");

        const std::filesystem::path cooked = root / "AV0.dfluoddity";
        std::string error;
        require(dve::write_dfluoddity(cooked, *imported.asset, &error), error);
        dve::FluoddityImportResult loaded = dve::read_dfluoddity(cooked);
        require(static_cast<bool>(loaded), loaded.error);
        require(loaded.asset->contentHash == imported.asset->contentHash,
                "content hash changed on .dfluoddity round trip");
        require(loaded.asset->compatibility.inkWeight.has_value(),
                "legacy appearance metadata was not retained");

        const std::filesystem::path trailing = root / "trailing.dfluoddity";
        std::filesystem::copy_file(cooked, trailing);
        { std::ofstream stream(trailing, std::ios::binary | std::ios::app); stream.put('X'); }
        require(!dve::read_dfluoddity(trailing), "trailing asset bytes were accepted");

        const std::filesystem::path corrupt = root / "corrupt.dfluoddity";
        std::filesystem::copy_file(cooked, corrupt);
        {
            std::fstream stream(corrupt, std::ios::binary | std::ios::in | std::ios::out);
            stream.seekg(-1, std::ios::end);
            char byte{};
            stream.get(byte);
            stream.seekp(-1, std::ios::end);
            stream.put(static_cast<char>(byte ^ 0x01));
        }
        require(!dve::read_dfluoddity(corrupt), "corrupted asset was accepted");

        const dve::FluoddityMemoryEstimate low = dve::estimate_fluoddity_memory(
            dve::fluoddity_quality_profile(dve::FluoddityQuality::Low));
        const dve::FluoddityMemoryEstimate high = dve::estimate_fluoddity_memory(
            dve::fluoddity_quality_profile(dve::FluoddityQuality::High));
        require(low.totalBytes < high.totalBytes, "quality memory estimates are not monotonic");
        require(high.particleBytes == 128'000'000ULL, "high particle memory estimate is wrong");
        require(high.accumulationBytes == 268'435'456ULL,
                "portable signed accumulation memory estimate is wrong");

        std::cout << "DVE Fluoddity foundation tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "DVE Fluoddity foundation tests failed: " << exception.what() << '\n';
        return 1;
    }
}
