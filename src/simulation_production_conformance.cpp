#include "dve/simulation_kernel_conformance.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dve {
namespace {

using Clock = std::chrono::steady_clock;

void emit(std::vector<std::uint32_t>& words, std::uint16_t opcode,
          std::initializer_list<std::uint32_t> operands) {
    words.push_back((static_cast<std::uint32_t>(operands.size() + 1U) << 16U) | opcode);
    words.insert(words.end(), operands.begin(), operands.end());
}

std::vector<std::uint32_t> encoded_string(std::string_view text) {
    const std::size_t bytes = text.size() + 1U;
    std::vector<std::uint32_t> words((bytes + 3U) / 4U, 0U);
    std::memcpy(words.data(), text.data(), text.size());
    return words;
}

void emit_entry_point(std::vector<std::uint32_t>& words, std::uint32_t function,
                      std::initializer_list<std::uint32_t> interfaces) {
    const auto stringWords = encoded_string("main");
    const std::uint32_t count = 3U + static_cast<std::uint32_t>(stringWords.size()) +
        static_cast<std::uint32_t>(interfaces.size());
    words.push_back((count << 16U) | 15U); // OpEntryPoint
    words.push_back(5U);                   // GLCompute
    words.push_back(function);
    words.insert(words.end(), stringWords.begin(), stringWords.end());
    words.insert(words.end(), interfaces.begin(), interfaces.end());
}

std::vector<std::byte> as_bytes(const std::vector<std::uint32_t>& words) {
    std::vector<std::byte> result(words.size() * sizeof(std::uint32_t));
    std::memcpy(result.data(), words.data(), result.size());
    return result;
}

std::uint32_t float_bits(float value) noexcept {
    return std::bit_cast<std::uint32_t>(value);
}

// Specialization of fluoddity_clear_accumulation.hlsl for exactly 256 scalar
// int lanes. The production int4 write is flattened to four scalar lanes.
std::vector<std::byte> fluoddity_clear_specialization() {
    std::vector<std::uint32_t> w{0x07230203U, 0x00010000U, 0U, 20U, 0U};
    emit(w, 17, {1U}); emit(w, 14, {0U, 1U});
    emit_entry_point(w, 15U, {14U});
    emit(w, 16, {15U, 17U, 64U, 1U, 1U});
    emit(w, 71, {8U, 6U, 4U});
    emit(w, 72, {9U, 0U, 35U, 0U}); emit(w, 71, {9U, 3U});
    emit(w, 71, {13U, 34U, 0U}); emit(w, 71, {13U, 33U, 0U});
    emit(w, 71, {14U, 11U, 28U});
    emit(w, 19, {1U}); emit(w, 33, {2U, 1U});
    emit(w, 21, {3U, 32U, 1U}); emit(w, 21, {4U, 32U, 0U});
    emit(w, 23, {5U, 4U, 3U});
    emit(w, 43, {4U, 6U, 0U}); emit(w, 43, {3U, 7U, 0U});
    emit(w, 29, {8U, 3U}); emit(w, 30, {9U, 8U});
    emit(w, 32, {10U, 2U, 9U}); emit(w, 32, {11U, 2U, 3U});
    emit(w, 32, {12U, 1U, 5U});
    emit(w, 59, {10U, 13U, 2U}); emit(w, 59, {12U, 14U, 1U});
    emit(w, 54, {1U, 15U, 0U, 2U}); emit(w, 248, {16U});
    emit(w, 61, {5U, 17U, 14U}); emit(w, 81, {4U, 18U, 17U, 0U});
    emit(w, 65, {11U, 19U, 13U, 6U, 18U}); emit(w, 62, {19U, 7U});
    emit(w, 253, {}); emit(w, 56, {});
    return as_bytes(w);
}

// Specialization of gabor_temporal_resolve.hlsl where resetHistory=0 and all
// depth differences are within the rejection threshold. Float4 pixels are
// flattened to scalar lanes and temporalWeight is fixed to 0.75.
std::vector<std::byte> gabor_temporal_specialization() {
    std::vector<std::uint32_t> w{0x07230203U, 0x00010000U, 0U, 30U, 0U};
    emit(w, 17, {1U}); emit(w, 14, {0U, 1U});
    emit_entry_point(w, 18U, {17U});
    emit(w, 16, {18U, 17U, 64U, 1U, 1U});
    emit(w, 71, {9U, 6U, 4U}); emit(w, 72, {10U, 0U, 35U, 0U}); emit(w, 71, {10U, 3U});
    emit(w, 71, {14U, 34U, 0U}); emit(w, 71, {14U, 33U, 0U});
    emit(w, 71, {15U, 34U, 0U}); emit(w, 71, {15U, 33U, 1U});
    emit(w, 71, {16U, 34U, 0U}); emit(w, 71, {16U, 33U, 2U});
    emit(w, 71, {17U, 11U, 28U});
    emit(w, 19, {1U}); emit(w, 33, {2U, 1U});
    emit(w, 22, {3U, 32U}); emit(w, 21, {4U, 32U, 0U}); emit(w, 23, {5U, 4U, 3U});
    emit(w, 43, {4U, 6U, 0U});
    emit(w, 43, {3U, 7U, float_bits(0.25F)}); emit(w, 43, {3U, 8U, float_bits(0.75F)});
    emit(w, 29, {9U, 3U}); emit(w, 30, {10U, 9U});
    emit(w, 32, {11U, 2U, 10U}); emit(w, 32, {12U, 2U, 3U}); emit(w, 32, {13U, 1U, 5U});
    emit(w, 59, {11U, 14U, 2U}); emit(w, 59, {11U, 15U, 2U}); emit(w, 59, {11U, 16U, 2U});
    emit(w, 59, {13U, 17U, 1U});
    emit(w, 54, {1U, 18U, 0U, 2U}); emit(w, 248, {19U});
    emit(w, 61, {5U, 20U, 17U}); emit(w, 81, {4U, 21U, 20U, 0U});
    emit(w, 65, {12U, 22U, 14U, 6U, 21U}); emit(w, 61, {3U, 23U, 22U});
    emit(w, 65, {12U, 24U, 15U, 6U, 21U}); emit(w, 61, {3U, 25U, 24U});
    emit(w, 133, {3U, 26U, 23U, 7U}); emit(w, 133, {3U, 27U, 25U, 8U});
    emit(w, 129, {3U, 28U, 26U, 27U});
    emit(w, 65, {12U, 29U, 16U, 6U, 21U}); emit(w, 62, {29U, 28U});
    emit(w, 253, {}); emit(w, 56, {});
    return as_bytes(w);
}

// Specialization of grid_fluid_divergence.hlsl for a 64x1x1 all-fluid grid,
// zero v/w velocity, and cellSize=0.5. Only the x-face contribution remains.
std::vector<std::byte> grid_divergence_specialization() {
    std::vector<std::uint32_t> w{0x07230203U, 0x00010000U, 0U, 31U, 0U};
    emit(w, 17, {1U}); emit(w, 14, {0U, 1U});
    emit_entry_point(w, 17U, {16U});
    emit(w, 16, {17U, 17U, 64U, 1U, 1U});
    emit(w, 71, {9U, 6U, 4U}); emit(w, 72, {10U, 0U, 35U, 0U}); emit(w, 71, {10U, 3U});
    emit(w, 71, {14U, 34U, 0U}); emit(w, 71, {14U, 33U, 0U});
    emit(w, 71, {15U, 34U, 0U}); emit(w, 71, {15U, 33U, 1U});
    emit(w, 71, {16U, 11U, 28U});
    emit(w, 19, {1U}); emit(w, 33, {2U, 1U});
    emit(w, 22, {3U, 32U}); emit(w, 21, {4U, 32U, 0U}); emit(w, 23, {5U, 4U, 3U});
    emit(w, 43, {4U, 6U, 0U}); emit(w, 43, {4U, 7U, 1U});
    emit(w, 43, {3U, 8U, float_bits(2.0F)});
    emit(w, 29, {9U, 3U}); emit(w, 30, {10U, 9U});
    emit(w, 32, {11U, 2U, 10U}); emit(w, 32, {12U, 2U, 3U}); emit(w, 32, {13U, 1U, 5U});
    emit(w, 59, {11U, 14U, 2U}); emit(w, 59, {11U, 15U, 2U}); emit(w, 59, {13U, 16U, 1U});
    emit(w, 54, {1U, 17U, 0U, 2U}); emit(w, 248, {18U});
    emit(w, 61, {5U, 19U, 16U}); emit(w, 81, {4U, 20U, 19U, 0U});
    emit(w, 128, {4U, 21U, 20U, 7U});
    emit(w, 65, {12U, 22U, 14U, 6U, 20U}); emit(w, 61, {3U, 23U, 22U});
    emit(w, 65, {12U, 24U, 14U, 6U, 21U}); emit(w, 61, {3U, 25U, 24U});
    emit(w, 131, {3U, 26U, 25U, 23U}); emit(w, 133, {3U, 27U, 26U, 8U});
    emit(w, 65, {12U, 28U, 15U, 6U, 20U}); emit(w, 62, {28U, 27U});
    emit(w, 253, {}); emit(w, 56, {});
    return as_bytes(w);
}

std::uint64_t hash_bytes(std::span<const std::byte> bytes) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const std::byte value : bytes) {
        hash ^= std::to_integer<std::uint8_t>(value);
        hash *= 1099511628211ULL;
    }
    return hash;
}

template <class T>
std::uint64_t hash_values(std::span<const T> values) noexcept {
    return hash_bytes(std::as_bytes(values));
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("could not read production shader source: " + path.string());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::string source_hash(const std::filesystem::path& path) {
    const std::string text = read_text_file(path);
    return validation_hex64(hash_bytes(std::as_bytes(std::span(text.data(), text.size()))));
}

struct FloatComparison {
    std::uint64_t mismatches{};
    double maximumAbsoluteError{};
};

FloatComparison compare_float(std::span<const float> expected, std::span<const float> actual,
                              double tolerance) noexcept {
    FloatComparison result;
    const std::size_t count = std::min(expected.size(), actual.size());
    for (std::size_t index = 0; index < count; ++index) {
        const double error = std::abs(static_cast<double>(expected[index]) -
                                      static_cast<double>(actual[index]));
        if (!std::isfinite(error) || error > tolerance) ++result.mismatches;
        result.maximumAbsoluteError = std::max(result.maximumAbsoluteError, error);
    }
    result.mismatches += expected.size() > count ? expected.size() - count : actual.size() - count;
    return result;
}

ValidationArrayComparison float_array_comparison(std::string name,
                                                  std::span<const float> expected,
                                                  std::span<const float> actual,
                                                  double tolerance) {
    const auto comparison = compare_float(expected, actual, tolerance);
    ValidationArrayComparison result;
    result.name = std::move(name);
    result.elementType = "float32";
    result.elementCount = expected.size();
    result.mismatchCount = comparison.mismatches;
    result.maximumAbsoluteError = comparison.maximumAbsoluteError;
    result.tolerance = tolerance;
    result.cpuHash = validation_hex64(hash_values(expected));
    result.deviceHash = validation_hex64(hash_values(actual));
    return result;
}

ValidationArrayComparison int_array_comparison(std::string name,
                                                std::span<const std::int32_t> expected,
                                                std::span<const std::int32_t> actual) {
    std::uint64_t mismatches{};
    std::int64_t maximum{};
    const std::size_t count = std::min(expected.size(), actual.size());
    for (std::size_t index = 0; index < count; ++index) {
        const auto error = std::llabs(static_cast<long long>(expected[index]) -
                                      static_cast<long long>(actual[index]));
        if (error != 0) ++mismatches;
        maximum = std::max(maximum, static_cast<std::int64_t>(error));
    }
    mismatches += expected.size() > count ? expected.size() - count : actual.size() - count;
    ValidationArrayComparison result;
    result.name = std::move(name);
    result.elementType = "int32";
    result.elementCount = expected.size();
    result.mismatchCount = mismatches;
    result.maximumAbsoluteError = static_cast<double>(maximum);
    result.tolerance = 0.0;
    result.cpuHash = validation_hex64(hash_values(expected));
    result.deviceHash = validation_hex64(hash_values(actual));
    return result;
}

class DeviceResources final {
public:
    explicit DeviceResources(rhi::IDevice& device) : device_(device) {}
    ~DeviceResources() {
        std::string ignored;
        for (auto handle = pipelines_.rbegin(); handle != pipelines_.rend(); ++handle)
            (void)device_.destroy_compute_pipeline(*handle, &ignored);
        for (auto handle = groups_.rbegin(); handle != groups_.rend(); ++handle)
            (void)device_.destroy_bind_group(*handle, &ignored);
        for (auto handle = layouts_.rbegin(); handle != layouts_.rend(); ++handle)
            (void)device_.destroy_bind_group_layout(*handle, &ignored);
        for (auto handle = buffers_.rbegin(); handle != buffers_.rend(); ++handle)
            (void)device_.destroy_buffer(*handle, &ignored);
    }

    template <class T>
    rhi::BufferHandle buffer(std::span<const T> values, std::string_view name) {
        std::string error;
        const std::size_t bytes = std::max<std::size_t>(values.size_bytes(), 4U);
        const auto handle = device_.create_buffer({
            bytes, rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDestination,
            rhi::MemoryDomain::Upload, std::string(name), rhi::ResourceState::ShaderWrite}, &error);
        if (!handle) throw std::runtime_error(error);
        buffers_.push_back(handle);
        if (!values.empty() && !device_.write_buffer(handle, 0U, std::as_bytes(values), &error))
            throw std::runtime_error(error);
        return handle;
    }

    rhi::BindGroupLayoutHandle layout(std::uint32_t bindings) {
        rhi::BindGroupLayoutDesc description;
        description.debugName = "production pass conformance layout";
        for (std::uint32_t binding = 0; binding < bindings; ++binding)
            description.bindings.push_back({binding, rhi::BindingType::StorageBufferReadWrite,
                                            rhi::ShaderStage::Compute});
        std::string error;
        const auto handle = device_.create_bind_group_layout(description, &error);
        if (!handle) throw std::runtime_error(error);
        layouts_.push_back(handle);
        return handle;
    }

    rhi::BindGroupHandle group(rhi::BindGroupLayoutHandle layoutHandle,
                               std::span<const rhi::BufferHandle> buffers,
                               std::span<const std::size_t> bytes) {
        rhi::BindGroupDesc description;
        description.layout = layoutHandle;
        description.debugName = "production pass conformance group";
        for (std::size_t index = 0; index < buffers.size(); ++index)
            description.entries.push_back({static_cast<std::uint32_t>(index), buffers[index], {},
                                           0U, bytes[index]});
        std::string error;
        const auto handle = device_.create_bind_group(description, &error);
        if (!handle) throw std::runtime_error(error);
        groups_.push_back(handle);
        return handle;
    }

    rhi::ComputePipelineHandle pipeline(std::vector<std::byte> bytecode,
                                        rhi::BindGroupLayoutHandle layoutHandle,
                                        std::string_view name) {
        rhi::ComputePipelineDesc description;
        description.debugName = std::string(name);
        description.bytecode = std::move(bytecode);
        description.bindGroupLayouts.push_back(layoutHandle);
        description.threadsX = 64U;
        std::string error;
        const auto handle = device_.create_compute_pipeline(description, &error);
        if (!handle) throw std::runtime_error(error);
        pipelines_.push_back(handle);
        return handle;
    }

private:
    rhi::IDevice& device_;
    std::vector<rhi::BufferHandle> buffers_;
    std::vector<rhi::BindGroupLayoutHandle> layouts_;
    std::vector<rhi::BindGroupHandle> groups_;
    std::vector<rhi::ComputePipelineHandle> pipelines_;
};

void record_kernel(ValidationFixtureResult& fixture, std::string name,
                   std::span<const std::byte> bytecode, std::uint32_t groupsX,
                   const std::filesystem::path& sourceRoot, std::string sourcePath,
                   std::string specialization) {
    ValidationKernelRecord record;
    record.name = std::move(name);
    record.evidenceClass = "production_pass_specialization";
    record.bytecodeFormat = "spirv";
    record.bytecodeBytes = bytecode.size();
    record.bytecodeHash = validation_hex64(hash_bytes(bytecode));
    record.sourcePath = std::move(sourcePath);
    record.sourceHash = source_hash(sourceRoot / record.sourcePath);
    record.compilerIdentity = "dve_deterministic_spirv_specializer_v1";
    record.specialization = std::move(specialization);
    record.localSizeX = 64U;
    record.groupsX = groupsX;
    fixture.kernels.push_back(std::move(record));
}

template <class Recorder>
void submit_timed_commands(rhi::IDevice& device, ValidationFixtureResult& result,
                           std::string_view debugName, Recorder&& recorder) {
    std::string error;
    rhi::TimestampQueryPoolHandle timestamps{};
    if (device.capabilities().timestampQueries) {
        timestamps = device.create_timestamp_query_pool(2U,
            std::string(debugName) + " timestamps", &error);
        if (!timestamps) throw std::runtime_error(error);
    }
    const auto commands = device.begin_commands(rhi::QueueKind::Compute, debugName, &error);
    if (!commands) throw std::runtime_error(error);
    if (timestamps && !device.write_timestamp(commands, timestamps, 0U, &error))
        throw std::runtime_error(error);
    if (!recorder(commands, error)) throw std::runtime_error(error);
    if (timestamps && !device.write_timestamp(commands, timestamps, 1U, &error))
        throw std::runtime_error(error);
    const auto fence = device.submit(commands, &error);
    if (!fence || !device.wait(fence, &error)) throw std::runtime_error(error);
    if (timestamps) {
        std::array<std::uint64_t, 2> values{};
        if (!device.resolve_timestamps(timestamps, 0U, values, &error))
            throw std::runtime_error(error);
        const std::uint64_t ticks = values[1] - values[0];
        ValidationMetric raw;
        raw.name = "device_timestamp_raw_ticks";
        raw.value = static_cast<double>(ticks);
        raw.unit = "device_ticks";
        raw.comparator = ValidationComparator::GreaterEqual;
        raw.minimum = 0.0;
        result.metrics.push_back(std::move(raw));
        const double period = device.capabilities().timestampPeriodNanoseconds;
        if (std::isfinite(period) && period > 0.0) {
            ValidationMetric converted;
            converted.name = "device_timestamp_nanoseconds";
            converted.value = static_cast<double>(ticks) * period;
            converted.unit = "ns";
            converted.comparator = ValidationComparator::GreaterEqual;
            converted.minimum = 0.0;
            result.metrics.push_back(std::move(converted));
            result.notes.push_back("Timestamp nanoseconds use the adapter-reported Vulkan timestampPeriod.");
        }
        if (!device.destroy_timestamp_query_pool(timestamps, &error))
            throw std::runtime_error(error);
    }
}

template <class Function>
ValidationFixtureResult timed_fixture(std::string system, std::string name, Function&& function) {
    ValidationFixtureResult result;
    result.system = std::move(system);
    result.name = std::move(name);
    result.evidenceClass = "production_pass_specialization_readback";
    result.status = ValidationStatus::Passed;
    const auto begin = Clock::now();
    try {
        function(result);
        result.evaluate();
    } catch (const std::exception& exception) {
        result.status = ValidationStatus::Failed;
        result.notes.emplace_back(exception.what());
    }
    result.durationMilliseconds =
        std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    return result;
}

ValidationFixtureResult skipped_fixture(std::string system, std::string name, std::string note) {
    ValidationFixtureResult result;
    result.system = std::move(system);
    result.name = std::move(name);
    result.evidenceClass = "production_pass_specialization_readback";
    result.status = ValidationStatus::Skipped;
    result.notes.push_back(std::move(note));
    return result;
}

ValidationFixtureResult fluoddity_clear_fixture(rhi::IDevice& device,
                                                 const std::filesystem::path& sourceRoot) {
    return timed_fixture("fluoddity", "production_clear_accumulation_specialization",
        [&](ValidationFixtureResult& result) {
            constexpr std::size_t count = 256U;
            std::array<std::int32_t, count> expected{};
            std::array<std::int32_t, count> output{};
            for (std::size_t index = 0; index < count; ++index)
                output[index] = static_cast<std::int32_t>(index * 17U + 3U);
            DeviceResources resources(device);
            const auto outputBuffer = resources.buffer(std::span<const std::int32_t>(output),
                                                       "fluoddity clear output");
            const auto layout = resources.layout(1U);
            const std::array buffers{outputBuffer};
            const std::array sizes{output.size() * sizeof(std::int32_t)};
            const auto group = resources.group(layout, buffers, sizes);
            const auto bytecode = fluoddity_clear_specialization();
            record_kernel(result, "fluoddity_clear_accumulation_flat_i32", bytecode, 4U,
                          sourceRoot, "shaders/fluoddity_clear_accumulation.hlsl",
                          "sim.y^3=64 voxels; int4 accumulation flattened to 256 scalar int lanes; bounds branch eliminated by exact dispatch");
            const auto pipeline = resources.pipeline(bytecode, layout, "fluoddity clear specialization");
            submit_timed_commands(device, result, "fluoddity clear specialization",
                [&](rhi::CommandListHandle commands, std::string& error) {
                    return device.bind_compute_bind_group(commands, 0U, group, &error) &&
                        device.dispatch(commands, pipeline, 4U, 1U, 1U, &error);
                });
            std::string error;
            if (!device.read_buffer(outputBuffer, 0U,
                    std::as_writable_bytes(std::span(output)), &error))
                throw std::runtime_error(error);
            result.arrays.push_back(int_array_comparison("accumulation_clear", expected, output));
            result.notes.push_back("Bounded specialization is source-hash-linked but is not output from an external HLSL compiler.");
        });
}

ValidationFixtureResult gabor_temporal_fixture(rhi::IDevice& device,
                                                const std::filesystem::path& sourceRoot) {
    return timed_fixture("gabor_volume", "production_temporal_resolve_specialization",
        [&](ValidationFixtureResult& result) {
            constexpr std::size_t count = 256U;
            std::array<float, count> current{};
            std::array<float, count> history{};
            std::array<float, count> expected{};
            std::array<float, count> output{};
            for (std::size_t index = 0; index < count; ++index) {
                current[index] = static_cast<float>(static_cast<int>(index % 31U) - 15) * 0.125F;
                history[index] = static_cast<float>(static_cast<int>((index * 7U) % 37U) - 18) * 0.0625F;
                expected[index] = current[index] * 0.25F + history[index] * 0.75F;
            }
            DeviceResources resources(device);
            const auto currentBuffer = resources.buffer(std::span<const float>(current), "gabor current");
            const auto historyBuffer = resources.buffer(std::span<const float>(history), "gabor history");
            const auto outputBuffer = resources.buffer(std::span<const float>(output), "gabor output");
            const auto layout = resources.layout(3U);
            const std::array buffers{currentBuffer, historyBuffer, outputBuffer};
            const std::array sizes{current.size() * sizeof(float), history.size() * sizeof(float),
                                   output.size() * sizeof(float)};
            const auto group = resources.group(layout, buffers, sizes);
            const auto bytecode = gabor_temporal_specialization();
            record_kernel(result, "gabor_temporal_resolve_flat_f32", bytecode, 4U,
                          sourceRoot, "shaders/gabor_temporal_resolve.hlsl",
                          "64 float4 pixels flattened to 256 scalar lanes; resetHistory=0; temporalWeight=0.75; all depth deltas below rejection threshold");
            const auto pipeline = resources.pipeline(bytecode, layout, "gabor temporal specialization");
            submit_timed_commands(device, result, "gabor temporal specialization",
                [&](rhi::CommandListHandle commands, std::string& error) {
                    return device.bind_compute_bind_group(commands, 0U, group, &error) &&
                        device.dispatch(commands, pipeline, 4U, 1U, 1U, &error);
                });
            std::string error;
            if (!device.read_buffer(outputBuffer, 0U,
                    std::as_writable_bytes(std::span(output)), &error))
                throw std::runtime_error(error);
            result.arrays.push_back(float_array_comparison(
                "temporal_resolve", expected, output, 2.0e-6));
            result.notes.push_back("Depth-rejection and reset branches are explicitly inactive in this specialization.");
        });
}

ValidationFixtureResult grid_divergence_fixture(rhi::IDevice& device,
                                                 const std::filesystem::path& sourceRoot) {
    return timed_fixture("grid_fluid", "production_divergence_specialization",
        [&](ValidationFixtureResult& result) {
            constexpr std::size_t count = 64U;
            std::array<float, count + 1U> velocityU{};
            std::array<float, count> expected{};
            std::array<float, count> output{};
            for (std::size_t index = 0; index < velocityU.size(); ++index)
                velocityU[index] = std::sin(static_cast<float>(index) * 0.19F) * 1.5F +
                    static_cast<float>(index) * 0.01F;
            for (std::size_t index = 0; index < count; ++index)
                expected[index] = (velocityU[index + 1U] - velocityU[index]) * 2.0F;
            DeviceResources resources(device);
            const auto velocityBuffer = resources.buffer(std::span<const float>(velocityU), "grid u velocity");
            const auto outputBuffer = resources.buffer(std::span<const float>(output), "grid divergence");
            const auto layout = resources.layout(2U);
            const std::array buffers{velocityBuffer, outputBuffer};
            const std::array sizes{velocityU.size() * sizeof(float), output.size() * sizeof(float)};
            const auto group = resources.group(layout, buffers, sizes);
            const auto bytecode = grid_divergence_specialization();
            record_kernel(result, "grid_fluid_divergence_1d_f32", bytecode, 1U,
                          sourceRoot, "shaders/grid_fluid_divergence.hlsl",
                          "grid=64x1x1; all cells fluid; v=w=0; cellSize=0.5; bounds branch eliminated by exact dispatch");
            const auto pipeline = resources.pipeline(bytecode, layout, "grid divergence specialization");
            submit_timed_commands(device, result, "grid divergence specialization",
                [&](rhi::CommandListHandle commands, std::string& error) {
                    return device.bind_compute_bind_group(commands, 0U, group, &error) &&
                        device.dispatch(commands, pipeline, 1U, 1U, 1U, &error);
                });
            std::string error;
            if (!device.read_buffer(outputBuffer, 0U,
                    std::as_writable_bytes(std::span(output)), &error))
                throw std::runtime_error(error);
            result.arrays.push_back(float_array_comparison(
                "divergence", expected, output, 2.0e-6));
            result.notes.push_back("This fixture validates the production divergence formula on a bounded degenerate grid, not the complete 3D shader control flow.");
        });
}

} // namespace

std::vector<ValidationFixtureResult> run_simulation_production_pass_conformance(
    rhi::IDevice& device, const std::filesystem::path& sourceRoot) {
    if (device.capabilities().backend == rhi::Backend::Null) {
        const std::string note =
            "Null RHI validates command contracts but does not execute specialization bytecode; production-pass readback skipped.";
        return {
            skipped_fixture("fluoddity", "production_clear_accumulation_specialization", note),
            skipped_fixture("gabor_volume", "production_temporal_resolve_specialization", note),
            skipped_fixture("grid_fluid", "production_divergence_specialization", note),
        };
    }
    return {
        fluoddity_clear_fixture(device, sourceRoot),
        gabor_temporal_fixture(device, sourceRoot),
        grid_divergence_fixture(device, sourceRoot),
    };
}

} // namespace dve
