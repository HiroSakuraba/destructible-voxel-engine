#include "dve/simulation_validation.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

namespace dve {
namespace {

void set_error(std::string* error, std::string_view message) {
    if (error != nullptr) error->assign(message.begin(), message.end());
}

bool write_text_file(const std::filesystem::path& path, std::string_view text,
                     std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        set_error(error, "could not create evidence directory: " + ec.message());
        return false;
    }
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            set_error(error, "could not open evidence file for writing: " + path.string());
            return false;
        }
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!stream) {
            set_error(error, "could not write evidence file: " + path.string());
            return false;
        }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(temporary, path, ec);
    }
    if (ec) {
        set_error(error, "could not commit evidence file: " + path.string());
        return false;
    }
    return true;
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

ai::JsonValue artifact_json(const ValidationArtifact& artifact) {
    return ai::JsonValue::Object{
        {"path", artifact.relativePath},
        {"bytes", static_cast<double>(artifact.bytes)},
        {"fnv1a64", validation_hex64(artifact.fnv1a64)},
    };
}

ValidationArtifact describe_artifact(const std::filesystem::path& root,
                                     const std::filesystem::path& path) {
    const std::string text = read_text_file(path);
    return {
        std::filesystem::relative(path, root).generic_string(),
        static_cast<std::uint64_t>(text.size()),
        validation_fnv1a64(text),
    };
}

std::string metric_threshold(const ValidationMetric& metric) {
    std::ostringstream stream;
    stream << std::setprecision(8);
    switch (metric.comparator) {
    case ValidationComparator::LessEqual:
        stream << "<= " << metric.maximum;
        break;
    case ValidationComparator::GreaterEqual:
        stream << ">= " << metric.minimum;
        break;
    case ValidationComparator::ClosedRange:
        stream << '[' << metric.minimum << ", " << metric.maximum << ']';
        break;
    case ValidationComparator::Approximate:
        stream << metric.minimum << " +/- " << metric.tolerance;
        break;
    case ValidationComparator::Finite:
        stream << "finite";
        break;
    }
    return stream.str();
}

} // namespace

bool ValidationMetric::evaluate() noexcept {
    if (!std::isfinite(value)) {
        status = ValidationStatus::Failed;
        if (detail.empty()) detail = "metric is not finite";
        return false;
    }
    bool passed{};
    switch (comparator) {
    case ValidationComparator::LessEqual:
        passed = value <= maximum;
        break;
    case ValidationComparator::GreaterEqual:
        passed = value >= minimum;
        break;
    case ValidationComparator::ClosedRange:
        passed = value >= minimum && value <= maximum;
        break;
    case ValidationComparator::Approximate:
        passed = std::abs(value - minimum) <= tolerance;
        break;
    case ValidationComparator::Finite:
        passed = true;
        break;
    }
    status = passed ? ValidationStatus::Passed : ValidationStatus::Failed;
    return passed;
}


bool ValidationArrayComparison::evaluate() noexcept {
    const bool passed = mismatchCount == 0U && std::isfinite(maximumAbsoluteError) &&
        maximumAbsoluteError <= tolerance && !cpuHash.empty() && cpuHash == deviceHash;
    status = passed ? ValidationStatus::Passed : ValidationStatus::Failed;
    if (!passed && detail.empty()) {
        detail = "array readback differs from CPU oracle";
    }
    return passed;
}

void ValidationFixtureResult::evaluate() noexcept {
    if (status == ValidationStatus::Skipped) return;
    bool passed = true;
    for (auto& metric : metrics) passed = metric.evaluate() && passed;
    for (auto& comparison : arrays) passed = comparison.evaluate() && passed;
    status = passed ? ValidationStatus::Passed : ValidationStatus::Failed;
}

ValidationStatus ValidationEvidenceBundle::status() const noexcept {
    bool hasPassed{};
    for (const auto& fixture : fixtures) {
        if (fixture.status == ValidationStatus::Failed) return ValidationStatus::Failed;
        if (fixture.status == ValidationStatus::Passed) hasPassed = true;
    }
    return hasPassed ? ValidationStatus::Passed : ValidationStatus::Skipped;
}

std::string_view validation_status_name(ValidationStatus status) noexcept {
    switch (status) {
    case ValidationStatus::Passed: return "passed";
    case ValidationStatus::Failed: return "failed";
    case ValidationStatus::Skipped: return "skipped";
    }
    return "unknown";
}

std::string_view validation_comparator_name(ValidationComparator comparator) noexcept {
    switch (comparator) {
    case ValidationComparator::LessEqual: return "less_equal";
    case ValidationComparator::GreaterEqual: return "greater_equal";
    case ValidationComparator::ClosedRange: return "closed_range";
    case ValidationComparator::Approximate: return "approximate";
    case ValidationComparator::Finite: return "finite";
    }
    return "unknown";
}

ai::JsonValue validation_metric_json(const ValidationMetric& metric) {
    return ai::JsonValue::Object{
        {"name", metric.name},
        {"unit", metric.unit},
        {"value", metric.value},
        {"comparator", std::string(validation_comparator_name(metric.comparator))},
        {"minimum", metric.minimum},
        {"maximum", metric.maximum},
        {"tolerance", metric.tolerance},
        {"status", std::string(validation_status_name(metric.status))},
        {"detail", metric.detail},
    };
}

ai::JsonValue validation_array_json(const ValidationArrayComparison& comparison) {
    return ai::JsonValue::Object{
        {"name", comparison.name},
        {"element_type", comparison.elementType},
        {"element_count", static_cast<double>(comparison.elementCount)},
        {"mismatch_count", static_cast<double>(comparison.mismatchCount)},
        {"maximum_absolute_error", comparison.maximumAbsoluteError},
        {"tolerance", comparison.tolerance},
        {"cpu_hash", comparison.cpuHash},
        {"device_hash", comparison.deviceHash},
        {"status", std::string(validation_status_name(comparison.status))},
        {"detail", comparison.detail},
    };
}

ai::JsonValue validation_kernel_json(const ValidationKernelRecord& kernel) {
    return ai::JsonValue::Object{
        {"name", kernel.name},
        {"evidence_class", kernel.evidenceClass},
        {"bytecode_format", kernel.bytecodeFormat},
        {"bytecode_bytes", static_cast<double>(kernel.bytecodeBytes)},
        {"bytecode_hash", kernel.bytecodeHash},
        {"source_path", kernel.sourcePath},
        {"source_hash", kernel.sourceHash},
        {"compiler_identity", kernel.compilerIdentity},
        {"specialization", kernel.specialization},
        {"local_size", ai::JsonValue::Array{
            static_cast<double>(kernel.localSizeX), static_cast<double>(kernel.localSizeY),
            static_cast<double>(kernel.localSizeZ)}},
        {"groups", ai::JsonValue::Array{
            static_cast<double>(kernel.groupsX), static_cast<double>(kernel.groupsY),
            static_cast<double>(kernel.groupsZ)}},
    };
}

ai::JsonValue validation_fixture_json(const ValidationFixtureResult& fixture) {
    ai::JsonValue::Array metrics;
    metrics.reserve(fixture.metrics.size());
    for (const auto& metric : fixture.metrics) metrics.emplace_back(validation_metric_json(metric));
    ai::JsonValue::Array arrays;
    arrays.reserve(fixture.arrays.size());
    for (const auto& comparison : fixture.arrays) arrays.emplace_back(validation_array_json(comparison));
    ai::JsonValue::Array kernels;
    kernels.reserve(fixture.kernels.size());
    for (const auto& kernel : fixture.kernels) kernels.emplace_back(validation_kernel_json(kernel));
    ai::JsonValue::Array notes;
    notes.reserve(fixture.notes.size());
    for (const auto& note : fixture.notes) notes.emplace_back(note);
    return ai::JsonValue::Object{
        {"system", fixture.system},
        {"name", fixture.name},
        {"evidence_class", fixture.evidenceClass},
        {"status", std::string(validation_status_name(fixture.status))},
        {"duration_ms", fixture.durationMilliseconds},
        {"metrics", std::move(metrics)},
        {"arrays", std::move(arrays)},
        {"kernels", std::move(kernels)},
        {"notes", std::move(notes)},
    };
}

ai::JsonValue validation_bundle_json(const ValidationEvidenceBundle& bundle) {
    ai::JsonValue::Array fixtures;
    fixtures.reserve(bundle.fixtures.size());
    for (const auto& fixture : bundle.fixtures) fixtures.emplace_back(validation_fixture_json(fixture));
    ai::JsonValue::Array warnings;
    warnings.reserve(bundle.warnings.size());
    for (const auto& warning : bundle.warnings) warnings.emplace_back(warning);
    ai::JsonValue::Array artifacts;
    artifacts.reserve(bundle.artifacts.size());
    for (const auto& artifact : bundle.artifacts) artifacts.emplace_back(artifact_json(artifact));
    return ai::JsonValue::Object{
        {"schema", static_cast<double>(bundle.schemaVersion)},
        {"engine_version", bundle.engineVersion},
        {"run_id", bundle.runId},
        {"suite", bundle.suite},
        {"backend", bundle.backend},
        {"externally_executed", bundle.externallyExecuted},
        {"status", std::string(validation_status_name(bundle.status()))},
        {"environment", bundle.environment},
        {"device", bundle.device},
        {"shader_abi", bundle.shaderAbi},
        {"fixtures", std::move(fixtures)},
        {"warnings", std::move(warnings)},
        {"artifacts", std::move(artifacts)},
    };
}

std::string validation_summary_markdown(const ValidationEvidenceBundle& bundle) {
    std::size_t passed{};
    std::size_t failed{};
    std::size_t skipped{};
    for (const auto& fixture : bundle.fixtures) {
        switch (fixture.status) {
        case ValidationStatus::Passed: ++passed; break;
        case ValidationStatus::Failed: ++failed; break;
        case ValidationStatus::Skipped: ++skipped; break;
        }
    }
    std::ostringstream stream;
    stream << "# DVE Simulation Validation Evidence\n\n"
           << "- Engine: `" << bundle.engineVersion << "`\n"
           << "- Run: `" << bundle.runId << "`\n"
           << "- Suite: `" << bundle.suite << "`\n"
           << "- Backend: `" << bundle.backend << "`\n"
           << "- Overall: **" << validation_status_name(bundle.status()) << "**\n"
           << "- Fixtures: " << passed << " passed, " << failed << " failed, "
           << skipped << " skipped\n"
           << "- External execution: " << (bundle.externallyExecuted ? "yes" : "no") << "\n\n";
    if (!bundle.warnings.empty()) {
        stream << "## Warnings\n\n";
        for (const auto& warning : bundle.warnings) stream << "- " << warning << '\n';
        stream << '\n';
    }
    stream << "## Fixtures\n\n"
           << "| System | Fixture | Evidence class | Status | Duration (ms) |\n"
           << "|---|---|---|---:|---:|\n";
    for (const auto& fixture : bundle.fixtures) {
        stream << "| " << fixture.system << " | " << fixture.name << " | "
               << fixture.evidenceClass << " | " << validation_status_name(fixture.status) << " | "
               << std::fixed << std::setprecision(3) << fixture.durationMilliseconds << " |\n";
    }
    stream << "\n## Metrics\n\n";
    for (const auto& fixture : bundle.fixtures) {
        stream << "### " << fixture.system << " / " << fixture.name << "\n\n";
        if (fixture.metrics.empty()) {
            stream << "No numeric metrics were emitted.\n\n";
        } else {
            stream << "| Metric | Value | Unit | Expected | Status |\n"
                   << "|---|---:|---|---|---:|\n";
            for (const auto& metric : fixture.metrics) {
                stream << "| " << metric.name << " | " << std::setprecision(8) << metric.value
                       << " | " << metric.unit << " | " << metric_threshold(metric) << " | "
                       << validation_status_name(metric.status) << " |\n";
            }
            stream << '\n';
        }
        if (!fixture.arrays.empty()) {
            stream << "Array readbacks:\n\n"
                   << "| Array | Elements | Mismatches | Max abs error | Tolerance | CPU hash | Device hash | Status |\n"
                   << "|---|---:|---:|---:|---:|---|---|---:|\n";
            for (const auto& comparison : fixture.arrays) {
                stream << "| " << comparison.name << " | " << comparison.elementCount
                       << " | " << comparison.mismatchCount << " | "
                       << comparison.maximumAbsoluteError << " | " << comparison.tolerance
                       << " | `" << comparison.cpuHash << "` | `" << comparison.deviceHash
                       << "` | " << validation_status_name(comparison.status) << " |\n";
            }
            stream << '\n';
        }
        if (!fixture.kernels.empty()) {
            stream << "Kernel records:\n\n"
                   << "| Kernel | Class | Format | Bytes | Hash | Source | Source hash | Local size | Groups |\n"
                   << "|---|---|---|---:|---|---|---|---|---|\n";
            for (const auto& kernel : fixture.kernels) {
                stream << "| " << kernel.name << " | " << kernel.evidenceClass << " | "
                       << kernel.bytecodeFormat << " | " << kernel.bytecodeBytes << " | `"
                       << kernel.bytecodeHash << "` | "
                       << (kernel.sourcePath.empty() ? "-" : kernel.sourcePath) << " | "
                       << (kernel.sourceHash.empty() ? "-" : "`" + kernel.sourceHash + "`")
                       << " | " << kernel.localSizeX << 'x' << kernel.localSizeY << 'x'
                       << kernel.localSizeZ << " | " << kernel.groupsX << 'x'
                       << kernel.groupsY << 'x' << kernel.groupsZ << " |\n";
                if (!kernel.specialization.empty())
                    stream << "  - Specialization: " << kernel.specialization << "\n";
                if (!kernel.compilerIdentity.empty())
                    stream << "  - Compiler identity: `" << kernel.compilerIdentity << "`\n";
            }
            stream << '\n';
        }
    }
    return stream.str();
}

std::uint64_t validation_fnv1a64(std::string_view text) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char character : text) {
        hash ^= character;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string validation_hex64(std::uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << value;
    return stream.str();
}

std::string make_validation_run_id(std::string_view engineVersion,
                                   std::string_view backend,
                                   std::string_view suite,
                                   std::uint64_t seed) noexcept {
    std::string canonical;
    canonical.reserve(engineVersion.size() + backend.size() + suite.size() + 32U);
    canonical.append(engineVersion);
    canonical.push_back('|');
    canonical.append(backend);
    canonical.push_back('|');
    canonical.append(suite);
    canonical.push_back('|');
    canonical.append(std::to_string(seed));
    return "validation-" + validation_hex64(validation_fnv1a64(canonical));
}

bool write_validation_evidence_bundle(const std::filesystem::path& outputDirectory,
                                      ValidationEvidenceBundle& bundle,
                                      std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(outputDirectory, ec);
    if (ec) {
        set_error(error, "could not create validation evidence directory: " + ec.message());
        return false;
    }

    const auto write_json = [&](std::string_view filename, const ai::JsonValue& value) {
        return write_text_file(outputDirectory / filename,
                               ai::stringify_json(value, true) + "\n", error);
    };

    ai::JsonValue::Array fixtureRows;
    for (const auto& fixture : bundle.fixtures)
        fixtureRows.emplace_back(validation_fixture_json(fixture));
    if (!write_json("environment.json", bundle.environment) ||
        !write_json("device.json", bundle.device) ||
        !write_json("shader_abi.json", bundle.shaderAbi) ||
        !write_json("conformance.json", ai::JsonValue::Object{
            {"schema", 3.0},
            {"fixtures", std::move(fixtureRows)},
        }) ||
        !write_text_file(outputDirectory / "summary.md",
                         validation_summary_markdown(bundle), error)) {
        return false;
    }

    std::ostringstream timings;
    timings << "system,fixture,evidence_class,status,duration_ms\n";
    for (const auto& fixture : bundle.fixtures) {
        timings << '"' << fixture.system << "\",\"" << fixture.name << "\",\""
                << fixture.evidenceClass << "\"," << validation_status_name(fixture.status) << ','
                << std::fixed << std::setprecision(6) << fixture.durationMilliseconds << '\n';
    }
    if (!write_text_file(outputDirectory / "timings.csv", timings.str(), error)) return false;

    std::ostringstream arraysCsv;
    arraysCsv << "system,fixture,array,element_type,elements,mismatches,max_abs_error,tolerance,cpu_hash,device_hash,status\n";
    for (const auto& fixture : bundle.fixtures) {
        for (const auto& comparison : fixture.arrays) {
            arraysCsv << '"' << fixture.system << "\",\"" << fixture.name << "\",\""
                      << comparison.name << "\",\"" << comparison.elementType << "\","
                      << comparison.elementCount << ',' << comparison.mismatchCount << ','
                      << comparison.maximumAbsoluteError << ',' << comparison.tolerance << ",\""
                      << comparison.cpuHash << "\",\"" << comparison.deviceHash << "\","
                      << validation_status_name(comparison.status) << '\n';
        }
    }
    if (!write_text_file(outputDirectory / "array_comparisons.csv", arraysCsv.str(), error)) return false;

    std::ostringstream kernelsCsv;
    kernelsCsv << "system,fixture,kernel,evidence_class,format,bytes,hash,source_path,source_hash,compiler_identity,specialization,local_x,local_y,local_z,groups_x,groups_y,groups_z\n";
    for (const auto& fixture : bundle.fixtures) {
        for (const auto& kernel : fixture.kernels) {
            kernelsCsv << '"' << fixture.system << "\",\"" << fixture.name << "\",\""
                       << kernel.name << "\",\"" << kernel.evidenceClass << "\",\""
                       << kernel.bytecodeFormat << "\"," << kernel.bytecodeBytes << ",\""
                       << kernel.bytecodeHash << "\",\"" << kernel.sourcePath << "\",\""
                       << kernel.sourceHash << "\",\"" << kernel.compilerIdentity << "\",\""
                       << kernel.specialization << "\"," << kernel.localSizeX << ','
                       << kernel.localSizeY << ',' << kernel.localSizeZ << ',' << kernel.groupsX
                       << ',' << kernel.groupsY << ',' << kernel.groupsZ << '\n';
        }
    }
    if (!write_text_file(outputDirectory / "kernels.csv", kernelsCsv.str(), error)) return false;

    bundle.artifacts.clear();
    const std::vector<std::filesystem::path> primary{
        outputDirectory / "environment.json",
        outputDirectory / "device.json",
        outputDirectory / "shader_abi.json",
        outputDirectory / "conformance.json",
        outputDirectory / "summary.md",
        outputDirectory / "timings.csv",
        outputDirectory / "array_comparisons.csv",
        outputDirectory / "kernels.csv",
    };
    for (const auto& path : primary) bundle.artifacts.push_back(describe_artifact(outputDirectory, path));

    ai::JsonValue::Array artifacts;
    for (const auto& artifact : bundle.artifacts) artifacts.emplace_back(artifact_json(artifact));
    if (!write_json("artifact_manifest.json", ai::JsonValue::Object{
            {"schema", 1.0},
            {"artifacts", std::move(artifacts)},
        })) {
        return false;
    }
    bundle.artifacts.push_back(describe_artifact(outputDirectory,
                                                 outputDirectory / "artifact_manifest.json"));
    return write_json("run.json", validation_bundle_json(bundle));
}

} // namespace dve
