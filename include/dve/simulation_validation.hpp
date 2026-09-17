#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "dve/ai/json.hpp"

namespace dve {

enum class ValidationStatus : std::uint8_t {
    Passed,
    Failed,
    Skipped,
};

enum class ValidationComparator : std::uint8_t {
    LessEqual,
    GreaterEqual,
    ClosedRange,
    Approximate,
    Finite,
};

struct ValidationMetric {
    std::string name;
    std::string unit;
    double value{};
    ValidationComparator comparator{ValidationComparator::Finite};
    double minimum{};
    double maximum{};
    double tolerance{};
    ValidationStatus status{ValidationStatus::Skipped};
    std::string detail;

    [[nodiscard]] bool evaluate() noexcept;
};

struct ValidationArrayComparison {
    std::string name;
    std::string elementType;
    std::uint64_t elementCount{};
    std::uint64_t mismatchCount{};
    double maximumAbsoluteError{};
    double tolerance{};
    std::string cpuHash;
    std::string deviceHash;
    ValidationStatus status{ValidationStatus::Skipped};
    std::string detail;

    [[nodiscard]] bool evaluate() noexcept;
};

struct ValidationKernelRecord {
    std::string name;
    std::string evidenceClass{"algorithmic_microkernel"};
    std::string bytecodeFormat{"spirv"};
    std::uint64_t bytecodeBytes{};
    std::string bytecodeHash;
    std::string sourcePath;
    std::string sourceHash;
    std::string compilerIdentity;
    std::string specialization;
    std::uint32_t localSizeX{1U};
    std::uint32_t localSizeY{1U};
    std::uint32_t localSizeZ{1U};
    std::uint32_t groupsX{1U};
    std::uint32_t groupsY{1U};
    std::uint32_t groupsZ{1U};
};

struct ValidationArtifact {
    std::string relativePath;
    std::uint64_t bytes{};
    std::uint64_t fnv1a64{};
};

struct ValidationFixtureResult {
    std::string system;
    std::string name;
    std::string evidenceClass{"cpu_reference"};
    ValidationStatus status{ValidationStatus::Skipped};
    double durationMilliseconds{};
    std::vector<ValidationMetric> metrics;
    std::vector<ValidationArrayComparison> arrays;
    std::vector<ValidationKernelRecord> kernels;
    std::vector<std::string> notes;

    void evaluate() noexcept;
};

struct ValidationEvidenceBundle {
    std::uint32_t schemaVersion{3U};
    std::string engineVersion;
    std::string runId;
    std::string suite;
    std::string backend;
    bool externallyExecuted{};
    ai::JsonValue environment{ai::JsonValue::Object{}};
    ai::JsonValue device{ai::JsonValue::Object{}};
    ai::JsonValue shaderAbi{ai::JsonValue::Object{}};
    std::vector<ValidationFixtureResult> fixtures;
    std::vector<std::string> warnings;
    std::vector<ValidationArtifact> artifacts;

    [[nodiscard]] ValidationStatus status() const noexcept;
};

[[nodiscard]] std::string_view validation_status_name(ValidationStatus status) noexcept;
[[nodiscard]] std::string_view validation_comparator_name(
    ValidationComparator comparator) noexcept;
[[nodiscard]] ai::JsonValue validation_metric_json(const ValidationMetric& metric);
[[nodiscard]] ai::JsonValue validation_array_json(const ValidationArrayComparison& comparison);
[[nodiscard]] ai::JsonValue validation_kernel_json(const ValidationKernelRecord& kernel);
[[nodiscard]] ai::JsonValue validation_fixture_json(const ValidationFixtureResult& fixture);
[[nodiscard]] ai::JsonValue validation_bundle_json(const ValidationEvidenceBundle& bundle);
[[nodiscard]] std::string validation_summary_markdown(
    const ValidationEvidenceBundle& bundle);

[[nodiscard]] std::uint64_t validation_fnv1a64(std::string_view text) noexcept;
[[nodiscard]] std::string validation_hex64(std::uint64_t value);
[[nodiscard]] std::string make_validation_run_id(std::string_view engineVersion,
                                                  std::string_view backend,
                                                  std::string_view suite,
                                                  std::uint64_t seed) noexcept;

[[nodiscard]] bool write_validation_evidence_bundle(
    const std::filesystem::path& outputDirectory,
    ValidationEvidenceBundle& bundle,
    std::string* error = nullptr);

} // namespace dve
