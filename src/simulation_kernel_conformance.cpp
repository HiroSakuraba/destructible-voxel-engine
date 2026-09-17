#include "dve/simulation_kernel_conformance.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

void emit_entry_point(std::vector<std::uint32_t>& words, std::uint32_t model,
                      std::uint32_t function, std::string_view name,
                      std::initializer_list<std::uint32_t> interfaces) {
    const auto stringWords = encoded_string(name);
    const std::uint32_t count = 3U + static_cast<std::uint32_t>(stringWords.size()) +
        static_cast<std::uint32_t>(interfaces.size());
    words.push_back((count << 16U) | 15U); // OpEntryPoint
    words.push_back(model);
    words.push_back(function);
    words.insert(words.end(), stringWords.begin(), stringWords.end());
    words.insert(words.end(), interfaces.begin(), interfaces.end());
}

std::vector<std::byte> as_bytes(const std::vector<std::uint32_t>& words) {
    std::vector<std::byte> result(words.size() * sizeof(std::uint32_t));
    std::memcpy(result.data(), words.data(), result.size());
    return result;
}

// out[i] = input[i] * 3 - 7
std::vector<std::byte> affine_shader() {
    std::vector<std::uint32_t> w{0x07230203U, 0x00010000U, 0U, 26U, 0U};
    emit(w, 17, {1U});
    emit(w, 14, {0U, 1U});
    emit_entry_point(w, 5U, 17U, "main", {16U});
    emit(w, 16, {17U, 17U, 8U, 1U, 1U});
    emit(w, 71, {9U, 6U, 4U});
    emit(w, 72, {10U, 0U, 35U, 0U});
    emit(w, 71, {10U, 3U});
    emit(w, 71, {14U, 34U, 0U}); emit(w, 71, {14U, 33U, 0U});
    emit(w, 71, {15U, 34U, 0U}); emit(w, 71, {15U, 33U, 1U});
    emit(w, 71, {16U, 11U, 28U}); // BuiltIn GlobalInvocationId
    emit(w, 19, {1U});
    emit(w, 33, {2U, 1U});
    emit(w, 21, {3U, 32U, 1U});
    emit(w, 21, {4U, 32U, 0U});
    emit(w, 23, {5U, 4U, 3U});
    emit(w, 43, {4U, 6U, 0U});
    emit(w, 43, {3U, 7U, 3U});
    emit(w, 43, {3U, 8U, 7U});
    emit(w, 29, {9U, 3U});
    emit(w, 30, {10U, 9U});
    emit(w, 32, {11U, 2U, 10U});
    emit(w, 32, {12U, 2U, 3U});
    emit(w, 32, {13U, 1U, 5U});
    emit(w, 59, {11U, 14U, 2U});
    emit(w, 59, {11U, 15U, 2U});
    emit(w, 59, {13U, 16U, 1U});
    emit(w, 54, {1U, 17U, 0U, 2U});
    emit(w, 248, {18U});
    emit(w, 61, {5U, 19U, 16U});
    emit(w, 81, {4U, 20U, 19U, 0U});
    emit(w, 65, {12U, 21U, 14U, 6U, 20U});
    emit(w, 61, {3U, 22U, 21U});
    emit(w, 132, {3U, 23U, 22U, 7U});
    emit(w, 130, {3U, 24U, 23U, 8U});
    emit(w, 65, {12U, 25U, 15U, 6U, 20U});
    emit(w, 62, {25U, 24U});
    emit(w, 253, {});
    emit(w, 56, {});
    return as_bytes(w);
}

// out[i] = input[i] - 2 * input[i + 1] + input[i + 2]
std::vector<std::byte> stencil_shader() {
    std::vector<std::uint32_t> w{0x07230203U, 0x00010000U, 0U, 35U, 0U};
    emit(w, 17, {1U}); emit(w, 14, {0U, 1U});
    emit_entry_point(w, 5U, 18U, "main", {17U});
    emit(w, 16, {18U, 17U, 8U, 1U, 1U});
    emit(w, 71, {10U, 6U, 4U}); emit(w, 72, {11U, 0U, 35U, 0U}); emit(w, 71, {11U, 3U});
    emit(w, 71, {15U, 34U, 0U}); emit(w, 71, {15U, 33U, 0U});
    emit(w, 71, {16U, 34U, 0U}); emit(w, 71, {16U, 33U, 1U});
    emit(w, 71, {17U, 11U, 28U});
    emit(w, 19, {1U}); emit(w, 33, {2U, 1U});
    emit(w, 21, {3U, 32U, 1U}); emit(w, 21, {4U, 32U, 0U}); emit(w, 23, {5U, 4U, 3U});
    emit(w, 43, {4U, 6U, 0U}); emit(w, 43, {4U, 7U, 1U}); emit(w, 43, {4U, 8U, 2U});
    emit(w, 43, {3U, 9U, 2U});
    emit(w, 29, {10U, 3U}); emit(w, 30, {11U, 10U});
    emit(w, 32, {12U, 2U, 11U}); emit(w, 32, {13U, 2U, 3U}); emit(w, 32, {14U, 1U, 5U});
    emit(w, 59, {12U, 15U, 2U}); emit(w, 59, {12U, 16U, 2U}); emit(w, 59, {14U, 17U, 1U});
    emit(w, 54, {1U, 18U, 0U, 2U}); emit(w, 248, {19U});
    emit(w, 61, {5U, 20U, 17U}); emit(w, 81, {4U, 21U, 20U, 0U});
    emit(w, 128, {4U, 22U, 21U, 7U}); emit(w, 128, {4U, 23U, 21U, 8U});
    emit(w, 65, {13U, 24U, 15U, 6U, 21U}); emit(w, 61, {3U, 25U, 24U});
    emit(w, 65, {13U, 26U, 15U, 6U, 22U}); emit(w, 61, {3U, 27U, 26U});
    emit(w, 65, {13U, 28U, 15U, 6U, 23U}); emit(w, 61, {3U, 29U, 28U});
    emit(w, 132, {3U, 30U, 27U, 9U});
    emit(w, 130, {3U, 31U, 25U, 30U});
    emit(w, 128, {3U, 32U, 31U, 29U});
    emit(w, 65, {13U, 33U, 16U, 6U, 21U}); emit(w, 62, {33U, 32U});
    emit(w, 253, {}); emit(w, 56, {});
    return as_bytes(w);
}

// out[i] = (history[i] * 3 + current[i]) / 4
std::vector<std::byte> temporal_blend_shader() {
    std::vector<std::uint32_t> w{0x07230203U, 0x00010000U, 0U, 33U, 0U};
    emit(w, 17, {1U}); emit(w, 14, {0U, 1U});
    emit_entry_point(w, 5U, 18U, "main", {17U});
    emit(w, 16, {18U, 17U, 8U, 1U, 1U});
    emit(w, 71, {9U, 6U, 4U}); emit(w, 72, {10U, 0U, 35U, 0U}); emit(w, 71, {10U, 3U});
    emit(w, 71, {14U, 34U, 0U}); emit(w, 71, {14U, 33U, 0U});
    emit(w, 71, {15U, 34U, 0U}); emit(w, 71, {15U, 33U, 1U});
    emit(w, 71, {16U, 34U, 0U}); emit(w, 71, {16U, 33U, 2U});
    emit(w, 71, {17U, 11U, 28U});
    emit(w, 19, {1U}); emit(w, 33, {2U, 1U});
    emit(w, 21, {3U, 32U, 1U}); emit(w, 21, {4U, 32U, 0U}); emit(w, 23, {5U, 4U, 3U});
    emit(w, 43, {4U, 6U, 0U}); emit(w, 43, {3U, 7U, 3U}); emit(w, 43, {3U, 8U, 4U});
    emit(w, 29, {9U, 3U}); emit(w, 30, {10U, 9U});
    emit(w, 32, {11U, 2U, 10U}); emit(w, 32, {12U, 2U, 3U}); emit(w, 32, {13U, 1U, 5U});
    emit(w, 59, {11U, 14U, 2U}); emit(w, 59, {11U, 15U, 2U}); emit(w, 59, {11U, 16U, 2U});
    emit(w, 59, {13U, 17U, 1U});
    emit(w, 54, {1U, 18U, 0U, 2U}); emit(w, 248, {19U});
    emit(w, 61, {5U, 20U, 17U}); emit(w, 81, {4U, 21U, 20U, 0U});
    emit(w, 65, {12U, 22U, 14U, 6U, 21U}); emit(w, 61, {3U, 23U, 22U});
    emit(w, 65, {12U, 24U, 15U, 6U, 21U}); emit(w, 61, {3U, 25U, 24U});
    emit(w, 132, {3U, 26U, 23U, 7U}); emit(w, 128, {3U, 27U, 26U, 25U});
    emit(w, 135, {3U, 28U, 27U, 8U});
    emit(w, 65, {12U, 29U, 16U, 6U, 21U}); emit(w, 62, {29U, 28U});
    emit(w, 253, {}); emit(w, 56, {});
    return as_bytes(w);
}

// accumulator[bins[i]] += values[i]
std::vector<std::byte> atomic_scatter_shader() {
    std::vector<std::uint32_t> w{0x07230203U, 0x00010000U, 0U, 34U, 0U};
    emit(w, 17, {1U}); emit(w, 14, {0U, 1U});
    emit_entry_point(w, 5U, 19U, "main", {18U});
    emit(w, 16, {19U, 17U, 8U, 1U, 1U});
    emit(w, 71, {10U, 6U, 4U}); emit(w, 72, {11U, 0U, 35U, 0U}); emit(w, 71, {11U, 3U});
    emit(w, 71, {15U, 34U, 0U}); emit(w, 71, {15U, 33U, 0U});
    emit(w, 71, {16U, 34U, 0U}); emit(w, 71, {16U, 33U, 1U});
    emit(w, 71, {17U, 34U, 0U}); emit(w, 71, {17U, 33U, 2U});
    emit(w, 71, {18U, 11U, 28U});
    emit(w, 19, {1U}); emit(w, 33, {2U, 1U});
    emit(w, 21, {3U, 32U, 1U}); emit(w, 21, {4U, 32U, 0U}); emit(w, 23, {5U, 4U, 3U});
    emit(w, 43, {4U, 6U, 0U}); emit(w, 43, {4U, 7U, 1U}); emit(w, 43, {4U, 8U, 0U});
    emit(w, 29, {10U, 3U}); emit(w, 30, {11U, 10U});
    emit(w, 32, {12U, 2U, 11U}); emit(w, 32, {13U, 2U, 3U}); emit(w, 32, {14U, 1U, 5U});
    emit(w, 59, {12U, 15U, 2U}); emit(w, 59, {12U, 16U, 2U}); emit(w, 59, {12U, 17U, 2U});
    emit(w, 59, {14U, 18U, 1U});
    emit(w, 54, {1U, 19U, 0U, 2U}); emit(w, 248, {20U});
    emit(w, 61, {5U, 21U, 18U}); emit(w, 81, {4U, 22U, 21U, 0U});
    emit(w, 65, {13U, 23U, 15U, 6U, 22U}); emit(w, 61, {3U, 24U, 23U});
    emit(w, 65, {13U, 25U, 16U, 6U, 22U}); emit(w, 61, {3U, 26U, 25U});
    emit(w, 65, {13U, 27U, 17U, 6U, 26U});
    emit(w, 234, {3U, 28U, 27U, 7U, 8U, 24U});
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

std::uint64_t hash_i32(std::span<const std::int32_t> values) noexcept {
    return hash_bytes(std::as_bytes(values));
}

struct Comparison {
    std::uint64_t mismatches{};
    std::int64_t maximumAbsoluteError{};
};

Comparison compare(std::span<const std::int32_t> expected,
                   std::span<const std::int32_t> actual) noexcept {
    Comparison result;
    const std::size_t count = std::min(expected.size(), actual.size());
    for (std::size_t index = 0; index < count; ++index) {
        const std::int64_t error = static_cast<std::int64_t>(std::llabs(
            static_cast<long long>(expected[index]) - static_cast<long long>(actual[index])));
        if (error != 0) ++result.mismatches;
        result.maximumAbsoluteError = std::max(result.maximumAbsoluteError, error);
    }
    result.mismatches += expected.size() > count ? expected.size() - count : actual.size() - count;
    return result;
}

ValidationArrayComparison make_array_comparison(
    std::string name, std::uint64_t count, const Comparison& comparison,
    std::uint64_t cpuHash, std::uint64_t deviceHash) {
    ValidationArrayComparison result;
    result.name = std::move(name);
    result.elementType = "int32";
    result.elementCount = count;
    result.mismatchCount = comparison.mismatches;
    result.maximumAbsoluteError = static_cast<double>(comparison.maximumAbsoluteError);
    result.tolerance = 0.0;
    result.cpuHash = validation_hex64(cpuHash);
    result.deviceHash = validation_hex64(deviceHash);
    return result;
}

void record_kernel(ValidationFixtureResult& result, std::string name,
                   std::span<const std::byte> bytecode, std::uint32_t groupsX) {
    ValidationKernelRecord record;
    record.name = std::move(name);
    record.bytecodeBytes = bytecode.size();
    record.bytecodeHash = validation_hex64(hash_bytes(bytecode));
    record.localSizeX = 8U;
    record.groupsX = groupsX;
    result.kernels.push_back(std::move(record));
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

    rhi::BufferHandle buffer(std::span<const std::int32_t> values, std::string_view name) {
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
        description.debugName = "simulation kernel conformance layout";
        for (std::uint32_t binding = 0; binding < bindings; ++binding) {
            description.bindings.push_back({binding, rhi::BindingType::StorageBufferReadWrite,
                                            rhi::ShaderStage::Compute});
        }
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
        description.debugName = "simulation kernel conformance group";
        for (std::size_t index = 0; index < buffers.size(); ++index) {
            description.entries.push_back({static_cast<std::uint32_t>(index), buffers[index], {},
                                           0U, bytes[index]});
        }
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
        description.threadsX = 8U;
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

ValidationFixtureResult skipped_fixture(std::string system, std::string name,
                                        std::string note) {
    ValidationFixtureResult result;
    result.system = std::move(system);
    result.name = std::move(name);
    result.evidenceClass = "algorithmic_kernel_readback";
    result.status = ValidationStatus::Skipped;
    result.notes.push_back(std::move(note));
    return result;
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
        ValidationMetric metric;
        metric.name = "device_timestamp_raw_ticks";
        metric.value = static_cast<double>(values[1] - values[0]);
        metric.unit = "device_ticks";
        metric.comparator = ValidationComparator::GreaterEqual;
        metric.minimum = 0.0;
        result.metrics.push_back(std::move(metric));
        const double period = device.capabilities().timestampPeriodNanoseconds;
        if (std::isfinite(period) && period > 0.0) {
            ValidationMetric converted;
            converted.name = "device_timestamp_nanoseconds";
            converted.value = static_cast<double>(values[1] - values[0]) * period;
            converted.unit = "ns";
            converted.comparator = ValidationComparator::GreaterEqual;
            converted.minimum = 0.0;
            result.metrics.push_back(std::move(converted));
            result.notes.push_back(
                "Kernel timestamp nanoseconds use the adapter-reported Vulkan timestampPeriod.");
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
    result.evidenceClass = "algorithmic_kernel_readback";
    result.status = ValidationStatus::Passed;
    const auto begin = Clock::now();
    try {
        function(result);
        result.evaluate();
    } catch (const std::exception& exception) {
        result.status = ValidationStatus::Failed;
        result.notes.push_back(exception.what());
    }
    result.durationMilliseconds =
        std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    return result;
}

ValidationFixtureResult affine_fixture(rhi::IDevice& device) {
    return timed_fixture("vfx_particles", "integer_particle_update_readback",
        [&](ValidationFixtureResult& result) {
            constexpr std::size_t count = 64U;
            std::array<std::int32_t, count> input{};
            std::array<std::int32_t, count> expected{};
            std::array<std::int32_t, count> output{};
            output.fill(-1234567);
            for (std::size_t index = 0; index < count; ++index) {
                input[index] = static_cast<std::int32_t>((index * 17U) % 101U) - 50;
                expected[index] = input[index] * 3 - 7;
            }
            DeviceResources resources(device);
            const auto inputBuffer = resources.buffer(input, "affine input");
            const auto outputBuffer = resources.buffer(output, "affine output");
            const auto layout = resources.layout(2U);
            const std::array buffers{inputBuffer, outputBuffer};
            const std::array sizes{input.size() * sizeof(std::int32_t),
                                   output.size() * sizeof(std::int32_t)};
            const auto group = resources.group(layout, buffers, sizes);
            const auto bytecode = affine_shader();
            record_kernel(result, "affine_i32", bytecode, 8U);
            const auto pipeline = resources.pipeline(bytecode, layout, "affine conformance");
            submit_timed_commands(device, result, "affine conformance",
                [&](rhi::CommandListHandle commands, std::string& error) {
                    return device.bind_compute_bind_group(commands, 0U, group, &error) &&
                        device.dispatch(commands, pipeline, 8U, 1U, 1U, &error);
                });
            std::string error;
            if (!device.read_buffer(outputBuffer, 0U,
                    std::as_writable_bytes(std::span(output)), &error))
                throw std::runtime_error(error);
            const auto comparison = compare(expected, output);
            result.arrays.push_back(make_array_comparison(
                "particle_update", count, comparison, hash_i32(expected), hash_i32(output)));
            result.notes.push_back("Exact integer affine update standing in for deterministic particle integration dataflow.");
        });
}

ValidationFixtureResult stencil_fixture(rhi::IDevice& device) {
    return timed_fixture("grid_fluid", "integer_stencil_readback",
        [&](ValidationFixtureResult& result) {
            constexpr std::size_t count = 64U;
            std::array<std::int32_t, count + 2U> input{};
            std::array<std::int32_t, count> expected{};
            std::array<std::int32_t, count> output{};
            for (std::size_t index = 0; index < input.size(); ++index)
                input[index] = static_cast<std::int32_t>((index * index * 13U + 7U) % 211U) - 105;
            for (std::size_t index = 0; index < count; ++index)
                expected[index] = input[index] - 2 * input[index + 1U] + input[index + 2U];
            DeviceResources resources(device);
            const auto inputBuffer = resources.buffer(input, "stencil input");
            const auto outputBuffer = resources.buffer(output, "stencil output");
            const auto layout = resources.layout(2U);
            const std::array buffers{inputBuffer, outputBuffer};
            const std::array sizes{input.size() * sizeof(std::int32_t),
                                   output.size() * sizeof(std::int32_t)};
            const auto group = resources.group(layout, buffers, sizes);
            const auto bytecode = stencil_shader();
            record_kernel(result, "second_difference_i32", bytecode, 8U);
            const auto pipeline = resources.pipeline(bytecode, layout, "stencil conformance");
            submit_timed_commands(device, result, "stencil conformance",
                [&](rhi::CommandListHandle commands, std::string& error) {
                    return device.bind_compute_bind_group(commands, 0U, group, &error) &&
                        device.dispatch(commands, pipeline, 8U, 1U, 1U, &error);
                });
            std::string error;
            if (!device.read_buffer(outputBuffer, 0U,
                    std::as_writable_bytes(std::span(output)), &error))
                throw std::runtime_error(error);
            const auto comparison = compare(expected, output);
            result.arrays.push_back(make_array_comparison(
                "second_difference", count, comparison, hash_i32(expected), hash_i32(output)));
            result.notes.push_back("Exact second-difference stencil representing divergence, pressure, diffusion, and curvature access patterns.");
        });
}

ValidationFixtureResult blend_fixture(rhi::IDevice& device) {
    return timed_fixture("gabor_volume", "integer_temporal_accumulation_readback",
        [&](ValidationFixtureResult& result) {
            constexpr std::size_t count = 64U;
            std::array<std::int32_t, count> history{};
            std::array<std::int32_t, count> current{};
            std::array<std::int32_t, count> expected{};
            std::array<std::int32_t, count> output{};
            for (std::size_t index = 0; index < count; ++index) {
                history[index] = 4 * (static_cast<std::int32_t>(index) - 31);
                current[index] = 4 * (63 - static_cast<std::int32_t>(index));
                expected[index] = (history[index] * 3 + current[index]) / 4;
            }
            DeviceResources resources(device);
            const auto historyBuffer = resources.buffer(history, "temporal history");
            const auto currentBuffer = resources.buffer(current, "temporal current");
            const auto outputBuffer = resources.buffer(output, "temporal output");
            const auto layout = resources.layout(3U);
            const std::array buffers{historyBuffer, currentBuffer, outputBuffer};
            const std::array sizes{history.size() * sizeof(std::int32_t),
                                   current.size() * sizeof(std::int32_t),
                                   output.size() * sizeof(std::int32_t)};
            const auto group = resources.group(layout, buffers, sizes);
            const auto bytecode = temporal_blend_shader();
            record_kernel(result, "temporal_blend_i32", bytecode, 8U);
            const auto pipeline = resources.pipeline(bytecode, layout,
                                                     "temporal blend conformance");
            submit_timed_commands(device, result, "temporal blend conformance",
                [&](rhi::CommandListHandle commands, std::string& error) {
                    return device.bind_compute_bind_group(commands, 0U, group, &error) &&
                        device.dispatch(commands, pipeline, 8U, 1U, 1U, &error);
                });
            std::string error;
            if (!device.read_buffer(outputBuffer, 0U,
                    std::as_writable_bytes(std::span(output)), &error))
                throw std::runtime_error(error);
            const auto comparison = compare(expected, output);
            result.arrays.push_back(make_array_comparison(
                "temporal_accumulation", count, comparison, hash_i32(expected), hash_i32(output)));
            result.notes.push_back("Exact weighted history accumulation representing temporal volume and particle-history resolves.");
        });
}

ValidationFixtureResult scatter_fixture(rhi::IDevice& device) {
    return timed_fixture("fluoddity_flip", "signed_atomic_scatter_readback",
        [&](ValidationFixtureResult& result) {
            constexpr std::size_t count = 64U;
            constexpr std::size_t binsCount = 8U;
            std::array<std::int32_t, count> values{};
            std::array<std::int32_t, count> bins{};
            std::array<std::int32_t, binsCount> expected{};
            std::array<std::int32_t, binsCount> output{};
            for (std::size_t index = 0; index < count; ++index) {
                values[index] = static_cast<std::int32_t>((index * 29U) % 23U) - 11;
                bins[index] = static_cast<std::int32_t>((index * 5U + index / 3U) % binsCount);
                expected[static_cast<std::size_t>(bins[index])] += values[index];
            }
            DeviceResources resources(device);
            const auto valuesBuffer = resources.buffer(values, "scatter values");
            const auto binsBuffer = resources.buffer(bins, "scatter bins");
            const auto outputBuffer = resources.buffer(output, "scatter output");
            const auto layout = resources.layout(3U);
            const std::array buffers{valuesBuffer, binsBuffer, outputBuffer};
            const std::array sizes{values.size() * sizeof(std::int32_t),
                                   bins.size() * sizeof(std::int32_t),
                                   output.size() * sizeof(std::int32_t)};
            const auto group = resources.group(layout, buffers, sizes);
            const auto bytecode = atomic_scatter_shader();
            record_kernel(result, "atomic_scatter_i32", bytecode, 8U);
            const auto pipeline = resources.pipeline(bytecode, layout,
                                                     "atomic scatter conformance");
            submit_timed_commands(device, result, "atomic scatter conformance",
                [&](rhi::CommandListHandle commands, std::string& error) {
                    return device.bind_compute_bind_group(commands, 0U, group, &error) &&
                        device.dispatch(commands, pipeline, 8U, 1U, 1U, &error);
                });
            std::string error;
            if (!device.read_buffer(outputBuffer, 0U,
                    std::as_writable_bytes(std::span(output)), &error))
                throw std::runtime_error(error);
            const auto comparison = compare(expected, output);
            result.arrays.push_back(make_array_comparison(
                "atomic_bins", binsCount, comparison, hash_i32(expected), hash_i32(output)));
            result.notes.push_back("Exact signed atomic scatter representing Fluoddity trail deposition and particle-to-grid accumulation.");
        });
}

} // namespace

std::vector<ValidationFixtureResult> run_simulation_kernel_conformance(rhi::IDevice& device) {
    if (device.capabilities().backend == rhi::Backend::Null) {
        const std::string note = "Null RHI validates command contracts but does not execute shader bytecode; readback comparison skipped.";
        return {
            skipped_fixture("vfx_particles", "integer_particle_update_readback", note),
            skipped_fixture("grid_fluid", "integer_stencil_readback", note),
            skipped_fixture("gabor_volume", "integer_temporal_accumulation_readback", note),
            skipped_fixture("fluoddity_flip", "signed_atomic_scatter_readback", note),
        };
    }
    return {
        affine_fixture(device),
        stencil_fixture(device),
        blend_fixture(device),
        scatter_fixture(device),
    };
}

} // namespace dve
