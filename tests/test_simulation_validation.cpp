#include "dve/simulation_validation.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
}

int main() {
    try {
        dve::ValidationMetric upper{"upper", "ratio", 0.5,
            dve::ValidationComparator::LessEqual, 0.0, 1.0};
        dve::ValidationMetric lower{"lower", "count", 2.0,
            dve::ValidationComparator::GreaterEqual, 1.0};
        dve::ValidationMetric finite{"finite", "value", 4.0,
            dve::ValidationComparator::Finite};
        require(upper.evaluate() && lower.evaluate() && finite.evaluate(),
                "passing validation metrics failed");

        dve::ValidationFixtureResult fixture;
        fixture.system = "test";
        fixture.name = "deterministic_fixture";
        fixture.status = dve::ValidationStatus::Passed;
        fixture.durationMilliseconds = 1.25;
        fixture.metrics = {upper, lower, finite};
        fixture.arrays.push_back({"array", "int32", 4U, 0U, 0.0, 0.0,
                                  "0123456789abcdef", "0123456789abcdef"});
        fixture.evaluate();
        require(fixture.status == dve::ValidationStatus::Passed,
                "fixture did not aggregate passing metrics");

        dve::ValidationEvidenceBundle bundle;
        bundle.engineVersion = "1.61.0";
        bundle.backend = "null";
        bundle.suite = "test";
        bundle.runId = dve::make_validation_run_id("1.61.0", "null", "test", 7U);
        bundle.environment = dve::ai::JsonValue::Object{{"test", true}};
        bundle.device = dve::ai::JsonValue::Object{{"status", "ready"}};
        bundle.shaderAbi = dve::ai::JsonValue::Object{{"status", "test"}};
        bundle.fixtures.push_back(fixture);
        require(bundle.status() == dve::ValidationStatus::Passed,
                "bundle did not aggregate fixture status");

        const auto root = std::filesystem::temp_directory_path() /
            "dve_simulation_validation_test";
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::string error;
        require(dve::write_validation_evidence_bundle(root, bundle, &error), error);
        for (const auto* name : {"run.json", "environment.json", "device.json",
                                 "shader_abi.json", "conformance.json", "summary.md",
                                 "timings.csv", "array_comparisons.csv", "kernels.csv",
                                 "artifact_manifest.json"}) {
            require(std::filesystem::is_regular_file(root / name),
                    std::string("missing evidence artifact: ") + name);
        }
        const auto parsed = dve::ai::parse_json(read_file(root / "run.json"));
        require(parsed.value && parsed.value->is_object(), "run evidence JSON did not parse");
        require(parsed.value->find("status") != nullptr &&
                parsed.value->find("status")->as_string() == "passed",
                "run evidence status is incorrect");
        require(bundle.artifacts.size() == 9U,
                "evidence writer did not describe all primary artifacts");
        std::filesystem::remove_all(root, ec);
        std::cout << "DVE simulation validation tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "DVE simulation validation tests failed: " << exception.what() << '\n';
        return 1;
    }
}
