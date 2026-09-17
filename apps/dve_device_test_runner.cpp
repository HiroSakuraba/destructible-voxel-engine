#include "dve/simulation_validation.hpp"
#include "dve/simulation_kernel_conformance.hpp"

#include "dve/rhi/null_device.hpp"
#if defined(DVE_DEVICE_RUNNER_HAS_VULKAN)
#include "dve/rhi/vulkan_device.hpp"
#endif
#if defined(DVE_ENABLE_FLUODDITY)
#include "dve/fluoddity.hpp"
#include "dve/fluoddity_solver.hpp"
#endif
#if defined(DVE_ENABLE_SOFT_BODIES)
#include "dve/soft_body.hpp"
#endif
#if defined(DVE_ENABLE_VFX_PARTICLES)
#include "dve/vfx_particle_graph.hpp"
#endif
#if defined(DVE_ENABLE_PBF_LIQUID)
#include "dve/liquid_pbf.hpp"
#endif
#if defined(DVE_ENABLE_GRID_FLUIDS)
#include "dve/grid_fluid.hpp"
#endif
#if defined(DVE_ENABLE_FLIP_LIQUIDS)
#include "dve/flip_liquid.hpp"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef DVE_SOURCE_DIR
#define DVE_SOURCE_DIR "."
#endif

namespace {
using Clock = std::chrono::steady_clock;
using dve::ValidationComparator;
using dve::ValidationFixtureResult;
using dve::ValidationMetric;
using dve::ValidationStatus;

struct Options {
    std::string backend{"null"};
    std::string suite{"all"};
    std::filesystem::path output{"dve_validation_evidence"};
    std::filesystem::path sourceRoot{DVE_SOURCE_DIR};
    std::uint64_t seed{1U};
    bool requireBackend{};
    bool requirePhysical{};
    bool requireKernelReadback{};
    bool requireProductionReadback{};
    bool requireTimestamps{};
    bool externallyExecuted{};
};

void usage() {
    std::cout
        << "dve_device_test_runner [options]\n"
        << "  --backend null|vulkan\n"
        << "  --suite device|simulation|all\n"
        << "  --output <directory>\n"
        << "  --source-root <directory>\n"
        << "  --seed <integer>\n"
        << "  --require-backend\n"
        << "  --require-physical\n"
        << "  --require-kernel-readback\n"
        << "  --require-production-readback\n"
        << "  --require-timestamps\n"
        << "  --external-execution\n";
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        const auto value = [&](std::string_view name) -> std::string_view {
            if (index + 1 >= argc) throw std::runtime_error(std::string(name) + " requires a value");
            return argv[++index];
        };
        if (argument == "--backend") options.backend = value(argument);
        else if (argument == "--suite") options.suite = value(argument);
        else if (argument == "--output") options.output = value(argument);
        else if (argument == "--source-root") options.sourceRoot = value(argument);
        else if (argument == "--seed") options.seed = std::stoull(std::string(value(argument)));
        else if (argument == "--require-backend") options.requireBackend = true;
        else if (argument == "--require-physical") options.requirePhysical = true;
        else if (argument == "--require-kernel-readback") options.requireKernelReadback = true;
        else if (argument == "--require-production-readback") options.requireProductionReadback = true;
        else if (argument == "--require-timestamps") options.requireTimestamps = true;
        else if (argument == "--external-execution") options.externallyExecuted = true;
        else if (argument == "--help" || argument == "-h") { usage(); std::exit(0); }
        else throw std::runtime_error("unknown option: " + std::string(argument));
    }
    if (options.backend != "null" && options.backend != "vulkan")
        throw std::runtime_error("backend must be null or vulkan");
    if (options.suite != "device" && options.suite != "simulation" && options.suite != "all")
        throw std::runtime_error("suite must be device, simulation, or all");
    return options;
}

ValidationMetric less_equal(std::string name, double value, double maximum,
                            std::string unit = {}, std::string detail = {}) {
    return {std::move(name), std::move(unit), value, ValidationComparator::LessEqual,
            0.0, maximum, 0.0, ValidationStatus::Skipped, std::move(detail)};
}

ValidationMetric greater_equal(std::string name, double value, double minimum,
                               std::string unit = {}, std::string detail = {}) {
    return {std::move(name), std::move(unit), value, ValidationComparator::GreaterEqual,
            minimum, 0.0, 0.0, ValidationStatus::Skipped, std::move(detail)};
}

ValidationMetric range_metric(std::string name, double value, double minimum, double maximum,
                              std::string unit = {}, std::string detail = {}) {
    return {std::move(name), std::move(unit), value, ValidationComparator::ClosedRange,
            minimum, maximum, 0.0, ValidationStatus::Skipped, std::move(detail)};
}

template <class Function>
ValidationFixtureResult timed_fixture(std::string system, std::string name, Function&& function) {
    ValidationFixtureResult result;
    result.system = std::move(system);
    result.name = std::move(name);
    result.status = ValidationStatus::Passed;
    const auto begin = Clock::now();
    try {
        function(result);
        result.evaluate();
    } catch (const std::exception& exception) {
        result.status = ValidationStatus::Failed;
        result.notes.emplace_back(exception.what());
    }
    result.durationMilliseconds = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    return result;
}

std::string adapter_class_name(dve::rhi::AdapterClass value) {
    switch (value) {
    case dve::rhi::AdapterClass::Unknown: return "unknown";
    case dve::rhi::AdapterClass::Integrated: return "integrated";
    case dve::rhi::AdapterClass::Discrete: return "discrete";
    case dve::rhi::AdapterClass::Virtual: return "virtual";
    case dve::rhi::AdapterClass::Cpu: return "cpu";
    }
    return "unknown";
}

bool is_physical_adapter(const dve::rhi::DeviceCapabilities& capabilities) noexcept {
    return !capabilities.softwareAdapter &&
        (capabilities.adapterClass == dve::rhi::AdapterClass::Integrated ||
         capabilities.adapterClass == dve::rhi::AdapterClass::Discrete);
}

std::string execution_class_name(const dve::rhi::DeviceCapabilities& capabilities) {
    if (capabilities.backend == dve::rhi::Backend::Null) return "contract_only";
    if (capabilities.softwareAdapter || capabilities.adapterClass == dve::rhi::AdapterClass::Cpu)
        return "software_vulkan";
    if (capabilities.adapterClass == dve::rhi::AdapterClass::Virtual) return "virtual_gpu";
    if (is_physical_adapter(capabilities)) return "physical_gpu";
    return "unclassified_vulkan";
}

std::string texture_format_name(dve::rhi::TextureFormat format) {
    using dve::rhi::TextureFormat;
    switch (format) {
    case TextureFormat::RGBA8Unorm: return "rgba8_unorm";
    case TextureFormat::BGRA8Unorm: return "bgra8_unorm";
    case TextureFormat::R32Uint: return "r32_uint";
    case TextureFormat::R32Sint: return "r32_sint";
    case TextureFormat::RGBA32Sint: return "rgba32_sint";
    case TextureFormat::RGBA16Float: return "rgba16_float";
    case TextureFormat::RG16Uint: return "rg16_uint";
    case TextureFormat::D32Float: return "d32_float";
    }
    return "unknown";
}

std::string platform_name() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

std::string compiler_name() {
#if defined(__clang__)
    return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("gcc ") + __VERSION__;
#elif defined(_MSC_VER)
    return std::string("msvc ") + std::to_string(_MSC_VER);
#else
    return "unknown";
#endif
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

dve::ai::JsonValue inspect_shader_abi(const std::filesystem::path& sourceRoot) {
    const auto manifestPath = sourceRoot / "shaders/shader_manifest.json";
    const std::string text = read_file(manifestPath);
    if (text.empty()) {
        return dve::ai::JsonValue::Object{
            {"status", "unavailable"}, {"reason", "shader manifest was not found"},
        };
    }
    const auto parsed = dve::ai::parse_json(text);
    if (!parsed.value || !parsed.value->is_object()) {
        return dve::ai::JsonValue::Object{
            {"status", "failed"}, {"reason", parsed.error},
        };
    }
    const auto* shaders = parsed.value->find("shaders");
    std::size_t count{};
    std::size_t compute{};
    std::size_t graphics{};
    std::size_t bindings{};
    if (shaders != nullptr && shaders->is_array()) {
        count = shaders->as_array().size();
        for (const auto& shader : shaders->as_array()) {
            if (!shader.is_object()) continue;
            const auto* stage = shader.find("stage");
            if (stage != nullptr && stage->as_string() == "compute") ++compute;
            else ++graphics;
            const auto* rows = shader.find("bindings");
            if (rows != nullptr && rows->is_array()) bindings += rows->as_array().size();
        }
    }
    return dve::ai::JsonValue::Object{
        {"status", "manifest_validated_at_build_or_test_time"},
        {"path", manifestPath.generic_string()},
        {"fnv1a64", dve::validation_hex64(dve::validation_fnv1a64(text))},
        {"shader_count", static_cast<double>(count)},
        {"compute_shader_count", static_cast<double>(compute)},
        {"graphics_shader_count", static_cast<double>(graphics)},
        {"binding_count", static_cast<double>(bindings)},
        {"note", "This inventory is source-contract evidence, not external DXC/glslang compilation evidence."},
    };
}

std::unique_ptr<dve::rhi::IDevice> make_device(const Options& options, std::string& error) {
    if (options.backend == "null") return std::make_unique<dve::rhi::NullDevice>();
#if defined(DVE_DEVICE_RUNNER_HAS_VULKAN)
    auto device = std::make_unique<dve::rhi::VulkanDevice>();
    if (device->status() != dve::rhi::DeviceStatus::Ready)
        error = std::string(device->device_loss_reason());
    return device;
#else
    error = "the runner was built without the Vulkan backend";
    return {};
#endif
}

std::vector<std::byte> as_bytes(const std::vector<std::uint32_t>& words) {
    std::vector<std::byte> result(words.size() * sizeof(std::uint32_t));
    std::memcpy(result.data(), words.data(), result.size());
    return result;
}

void emit(std::vector<std::uint32_t>& words, std::uint16_t opcode,
          std::initializer_list<std::uint32_t> operands) {
    words.push_back((static_cast<std::uint32_t>(operands.size() + 1U) << 16U) | opcode);
    words.insert(words.end(), operands.begin(), operands.end());
}

void emit_string_instruction(std::vector<std::uint32_t>& words, std::uint16_t opcode,
                             std::initializer_list<std::uint32_t> prefix,
                             std::string_view text) {
    std::vector<std::uint32_t> operands(prefix);
    const std::size_t byteCount = text.size() + 1U;
    const std::size_t wordCount = (byteCount + 3U) / 4U;
    const std::size_t offset = operands.size();
    operands.resize(offset + wordCount, 0U);
    std::memcpy(operands.data() + static_cast<std::ptrdiff_t>(offset), text.data(), text.size());
    words.push_back((static_cast<std::uint32_t>(operands.size() + 1U) << 16U) | opcode);
    words.insert(words.end(), operands.begin(), operands.end());
}

std::vector<std::byte> storage_write_shader() {
    std::vector<std::uint32_t> words{0x07230203U, 0x00010000U, 0U, 14U, 0U};
    emit(words, 17, {1U}); emit(words, 14, {0U, 1U});
    emit_string_instruction(words, 15, {5U, 11U}, "main");
    emit(words, 16, {11U, 17U, 1U, 1U, 1U});
    emit(words, 71, {6U, 6U, 4U}); emit(words, 72, {7U, 0U, 35U, 0U});
    emit(words, 71, {7U, 3U}); emit(words, 71, {9U, 34U, 0U}); emit(words, 71, {9U, 33U, 0U});
    emit(words, 19, {1U}); emit(words, 33, {2U, 1U}); emit(words, 21, {3U, 32U, 0U});
    emit(words, 43, {3U, 4U, 0U}); emit(words, 29, {6U, 3U}); emit(words, 30, {7U, 6U});
    emit(words, 32, {8U, 2U, 7U}); emit(words, 32, {5U, 2U, 3U}); emit(words, 59, {8U, 9U, 2U});
    emit(words, 43, {3U, 10U, 0x12345678U}); emit(words, 54, {1U, 11U, 0U, 2U});
    emit(words, 248, {12U}); emit(words, 65, {5U, 13U, 9U, 4U, 4U}); emit(words, 62, {13U, 10U});
    emit(words, 253, {}); emit(words, 56, {});
    return as_bytes(words);
}

std::vector<std::byte> atomic_add_shader() {
    std::vector<std::uint32_t> words{0x07230203U, 0x00010000U, 0U, 18U, 0U};
    emit(words, 17, {1U}); emit(words, 14, {0U, 1U});
    emit_string_instruction(words, 15, {5U, 14U}, "main");
    emit(words, 16, {14U, 17U, 8U, 1U, 1U}); emit(words, 71, {9U, 6U, 4U});
    emit(words, 72, {10U, 0U, 35U, 0U}); emit(words, 71, {10U, 3U});
    emit(words, 71, {13U, 34U, 0U}); emit(words, 71, {13U, 33U, 0U});
    emit(words, 19, {1U}); emit(words, 33, {2U, 1U}); emit(words, 21, {3U, 32U, 1U});
    emit(words, 21, {4U, 32U, 0U}); emit(words, 43, {3U, 5U, 0U}); emit(words, 43, {4U, 6U, 0U});
    emit(words, 43, {3U, 7U, 1U}); emit(words, 43, {4U, 8U, 1U}); emit(words, 29, {9U, 3U});
    emit(words, 30, {10U, 9U}); emit(words, 32, {11U, 2U, 10U}); emit(words, 32, {12U, 2U, 3U});
    emit(words, 59, {11U, 13U, 2U}); emit(words, 54, {1U, 14U, 0U, 2U}); emit(words, 248, {15U});
    emit(words, 65, {12U, 16U, 13U, 5U, 5U}); emit(words, 234, {3U, 17U, 16U, 8U, 6U, 7U});
    emit(words, 253, {}); emit(words, 56, {});
    return as_bytes(words);
}

ValidationFixtureResult device_capability_fixture(dve::rhi::IDevice& device) {
    return timed_fixture("device", "capabilities", [&](ValidationFixtureResult& result) {
        result.evidenceClass = "device_capability";
        const auto& capabilities = device.capabilities();
        const auto signedFormat = device.texture_format_capabilities(dve::rhi::TextureFormat::R32Sint);
        result.metrics.push_back(greater_equal("max_compute_invocations",
            capabilities.maxComputeInvocations, 64.0, "threads"));
        result.metrics.push_back(greater_equal("max_compute_workgroup_x",
            capabilities.maxComputeWorkgroupSizeX, 64.0, "threads"));
        result.metrics.push_back(greater_equal("r32_sint_storage",
            signedFormat.storage ? 1.0 : 0.0, 1.0, "boolean"));
        result.metrics.push_back(greater_equal("r32_sint_storage_atomic",
            signedFormat.storageAtomic ? 1.0 : 0.0, 1.0, "boolean"));
        result.metrics.push_back(greater_equal("indirect_dispatch",
            capabilities.indirectDispatch ? 1.0 : 0.0, 1.0, "boolean"));
    });
}

ValidationFixtureResult compute_probe_fixture(dve::rhi::IDevice& device) {
    return timed_fixture("device", "compute_readback_and_signed_atomic", [&](ValidationFixtureResult& result) {
        result.evidenceClass = "generic_compute_readback";
        using namespace dve::rhi;
        std::string error;
        const BufferHandle buffer = device.create_buffer({
            16U, BufferUsage::Storage | BufferUsage::CopyDestination,
            MemoryDomain::Upload, "device runner compute probe", ResourceState::ShaderWrite}, &error);
        if (!buffer) throw std::runtime_error(error);
        const std::uint32_t zero{};
        if (!device.write_buffer(buffer, 0U, std::as_bytes(std::span(&zero, 1U)), &error))
            throw std::runtime_error(error);
        BindGroupLayoutDesc layoutDescription;
        layoutDescription.debugName = "device runner storage layout";
        layoutDescription.bindings.push_back({0U, BindingType::StorageBufferReadWrite, ShaderStage::Compute});
        const auto layout = device.create_bind_group_layout(layoutDescription, &error);
        if (!layout) throw std::runtime_error(error);
        BindGroupDesc groupDescription;
        groupDescription.layout = layout;
        groupDescription.debugName = "device runner storage group";
        groupDescription.entries.push_back({0U, buffer, {}, 0U, 16U});
        const auto group = device.create_bind_group(groupDescription, &error);
        if (!group) throw std::runtime_error(error);

        ComputePipelineDesc pipelineDescription;
        pipelineDescription.debugName = "device runner write probe";
        pipelineDescription.bytecode = storage_write_shader();
        pipelineDescription.bindGroupLayouts.push_back(layout);
        const auto pipeline = device.create_compute_pipeline(pipelineDescription, &error);
        if (!pipeline) throw std::runtime_error(error);
        TimestampQueryPoolHandle timestamps{};
        if (device.capabilities().timestampQueries) {
            timestamps = device.create_timestamp_query_pool(2U, "device runner compute timestamps", &error);
            if (!timestamps) throw std::runtime_error(error);
        }
        const auto commands = device.begin_commands(QueueKind::Compute, "device runner write", &error);
        if (!commands) throw std::runtime_error(error);
        if (timestamps && !device.write_timestamp(commands, timestamps, 0U, &error))
            throw std::runtime_error(error);
        if (!device.bind_compute_bind_group(commands, 0U, group, &error) ||
            !device.dispatch(commands, pipeline, 1U, 1U, 1U, &error))
            throw std::runtime_error(error);
        if (timestamps && !device.write_timestamp(commands, timestamps, 1U, &error))
            throw std::runtime_error(error);
        const auto fence = device.submit(commands, &error);
        if (!fence || !device.fence_complete(fence)) throw std::runtime_error(error);
        if (timestamps) {
            std::array<std::uint64_t, 2> values{};
            if (!device.resolve_timestamps(timestamps, 0U, values, &error))
                throw std::runtime_error(error);
            result.metrics.push_back(greater_equal("timestamp_ordered",
                values[1] >= values[0] ? 1.0 : 0.0, 1.0, "boolean"));
            const std::uint64_t deltaTicks = values[1] - values[0];
            result.metrics.push_back(greater_equal("timestamp_delta_raw_ticks",
                static_cast<double>(deltaTicks), 0.0, "device_ticks"));
            const double timestampPeriod = device.capabilities().timestampPeriodNanoseconds;
            if (std::isfinite(timestampPeriod) && timestampPeriod > 0.0) {
                result.metrics.push_back(greater_equal("timestamp_delta_nanoseconds",
                    static_cast<double>(deltaTicks) * timestampPeriod, 0.0, "ns"));
                result.notes.emplace_back(
                    "Timestamp nanoseconds use the adapter-reported Vulkan timestampPeriod.");
            }
        } else {
            result.notes.emplace_back("Timestamp queries are unavailable on the selected queue family.");
        }

        if (device.capabilities().backend == Backend::Null) {
            result.notes.emplace_back("Null RHI validates command contracts but does not execute shader bytecode.");
            result.metrics.push_back(greater_equal("validated_dispatches",
                static_cast<double>(device.statistics().dispatchesExecuted), 1.0, "dispatches"));
        } else {
            std::uint32_t value{};
            if (!device.read_buffer(buffer, 0U, std::as_writable_bytes(std::span(&value, 1U)), &error))
                throw std::runtime_error(error);
            result.metrics.push_back(range_metric("storage_write_readback", value,
                0x12345678U, 0x12345678U, "uint32"));

            if (!device.write_buffer(buffer, 0U, std::as_bytes(std::span(&zero, 1U)), &error))
                throw std::runtime_error(error);
            ComputePipelineDesc atomicDescription;
            atomicDescription.debugName = "device runner atomic probe";
            atomicDescription.bytecode = atomic_add_shader();
            atomicDescription.bindGroupLayouts.push_back(layout);
            atomicDescription.threadsX = 8U;
            const auto atomicPipeline = device.create_compute_pipeline(atomicDescription, &error);
            if (!atomicPipeline) throw std::runtime_error(error);
            const auto atomicCommands = device.begin_commands(QueueKind::Compute, "device runner atomic", &error);
            if (!atomicCommands || !device.bind_compute_bind_group(atomicCommands, 0U, group, &error) ||
                !device.dispatch(atomicCommands, atomicPipeline, 4U, 1U, 1U, &error))
                throw std::runtime_error(error);
            const auto atomicFence = device.submit(atomicCommands, &error);
            if (!atomicFence || !device.fence_complete(atomicFence)) throw std::runtime_error(error);
            std::int32_t accumulated{};
            if (!device.read_buffer(buffer, 0U,
                    std::as_writable_bytes(std::span(&accumulated, 1U)), &error))
                throw std::runtime_error(error);
            result.metrics.push_back(range_metric("signed_atomic_accumulation", accumulated,
                32.0, 32.0, "int32"));
            (void)device.destroy_compute_pipeline(atomicPipeline, &error);
        }
        if (timestamps) (void)device.destroy_timestamp_query_pool(timestamps, &error);
        (void)device.destroy_compute_pipeline(pipeline, &error);
        (void)device.destroy_bind_group(group, &error);
        (void)device.destroy_bind_group_layout(layout, &error);
        (void)device.destroy_buffer(buffer, &error);
    });
}

#if defined(DVE_ENABLE_GRID_FLUIDS)
ValidationFixtureResult grid_fluid_fixture() {
    return timed_fixture("grid_fluid", "smoke_projection_and_conservation", [](ValidationFixtureResult& result) {
        dve::GridFluidSettings settings;
        settings.dimensions = {12U, 16U, 10U};
        settings.cellSize = 0.1F;
        settings.maximumSubsteps = 4U;
        settings.pressureMaximumIterations = 80U;
        settings.pressureRelativeTolerance = 5.0e-5F;
        settings.scalarAdvection = dve::GridFluidAdvection::MacCormack;
        std::string error;
        auto state = dve::make_grid_fluid_state(settings, &error);
        if (!error.empty()) throw std::runtime_error(error);
        dve::GridFluidEmitterSample emitter;
        emitter.center = {0.45F, 0.25F, 0.45F};
        emitter.radius = 0.16F;
        emitter.density = 0.7F;
        emitter.temperature = 1.0F;
        emitter.fuel = 0.4F;
        emitter.velocity = {0.1F, 0.6F, 0.0F};
        if (!dve::inject_grid_fluid_sphere(state, emitter, &error)) throw std::runtime_error(error);
        const double densityBefore = std::accumulate(state.density.begin(), state.density.end(), 0.0);
        dve::GridFluidTelemetry telemetry{};
        for (int frame = 0; frame < 3; ++frame) telemetry = dve::step_grid_fluid(state, 1.0F / 60.0F);
        const double densityAfter = std::accumulate(state.density.begin(), state.density.end(), 0.0);
        const double divergenceRatio = telemetry.maximumDivergenceBeforeProjection > 1.0e-8F
            ? telemetry.maximumDivergenceAfterProjection / telemetry.maximumDivergenceBeforeProjection : 0.0;
        result.metrics.push_back(less_equal("projection_divergence_ratio", divergenceRatio, 1.05, "ratio"));
        result.metrics.push_back(less_equal("pressure_residual", telemetry.pressureResidual, 5.0e-3, "relative"));
        result.metrics.push_back(greater_equal("density_retained_ratio",
            densityBefore > 0.0 ? densityAfter / densityBefore : 0.0, 0.05, "ratio"));
        const auto minimumDensity = *std::min_element(state.density.begin(), state.density.end());
        result.metrics.push_back(greater_equal("minimum_density", minimumDensity, -1.0e-6, "density"));
    });
}
#endif

#if defined(DVE_ENABLE_FLIP_LIQUIDS)
ValidationFixtureResult flip_liquid_fixture() {
    return timed_fixture("flip_apic", "free_surface_projection_and_volume", [](ValidationFixtureResult& result) {
        dve::FlipLiquidSettings settings;
        settings.dimensions = {10U, 12U, 8U};
        settings.cellSize = 0.1F;
        settings.particleRadius = 0.025F;
        settings.maximumSubsteps = 4U;
        settings.pressureMaximumIterations = 100U;
        settings.maximumParticles = 20'000U;
        std::string error;
        auto state = dve::make_flip_liquid_state(settings, &error);
        if (!error.empty()) throw std::runtime_error(error);
        const auto initial = dve::add_flip_liquid_box(state, {0.2F, 0.2F, 0.2F},
            {0.55F, 0.75F, 0.55F}, 0.055F, {}, &error);
        if (initial == 0U) throw std::runtime_error(error.empty() ? "liquid fixture emitted no particles" : error);
        double minimumVolume = std::numeric_limits<double>::max();
        double maximumVolume{};
        dve::FlipLiquidTelemetry telemetry{};
        for (int frame = 0; frame < 5; ++frame) {
            telemetry = dve::step_flip_liquid(state, 1.0F / 60.0F);
            minimumVolume = std::min(minimumVolume, static_cast<double>(telemetry.estimatedLiquidVolume));
            maximumVolume = std::max(maximumVolume, static_cast<double>(telemetry.estimatedLiquidVolume));
        }
        const double divergenceRatio = telemetry.maximumDivergenceBeforeProjection > 1.0e-8F
            ? telemetry.maximumDivergenceAfterProjection / telemetry.maximumDivergenceBeforeProjection : 0.0;
        const double volumeDrift = maximumVolume > 1.0e-12 ? (maximumVolume - minimumVolume) / maximumVolume : 0.0;
        result.metrics.push_back(less_equal("projection_divergence_ratio", divergenceRatio, 1.05, "ratio"));
        result.metrics.push_back(less_equal("estimated_volume_span", volumeDrift, 0.55, "ratio"));
        result.metrics.push_back(less_equal("non_finite_repairs", telemetry.nonFiniteCorrections, 0.0, "repairs"));
        result.metrics.push_back(range_metric("particle_count", state.particles.size(), 1.0,
            settings.maximumParticles, "particles"));
    });
}
#endif

#if defined(DVE_ENABLE_SOFT_BODIES)
ValidationFixtureResult soft_body_fixture() {
    return timed_fixture("soft_body", "pinned_cloth_constraint_stability", [](ValidationFixtureResult& result) {
        const auto cloth = dve::make_soft_body_cloth({8U, 6U, 0.1F, 0.02F, 1.0e-7F, 1.0e-4F, true});
        std::string error;
        dve::SoftBodyWorld world;
        dve::RuntimeSoftBodyInstance instance;
        instance.groundHeight = -1.0F;
        instance.solverIterations = 10U;
        const auto id = world.create(cloth, instance, &error);
        if (id == 0U) throw std::runtime_error(error);
        const auto* initial = world.find_state(id);
        if (initial == nullptr) throw std::runtime_error("cloth state missing");
        const float pinnedY = initial->positions.front().y;
        const float freeY = initial->positions.back().y;
        dve::SoftBodyStepTelemetry telemetry{};
        for (int frame = 0; frame < 12; ++frame)
            telemetry = world.step(1.0F / 120.0F, {0.0F, -9.81F, 0.0F});
        const auto* finalState = world.find_state(id);
        if (finalState == nullptr) throw std::runtime_error("cloth state disappeared");
        result.metrics.push_back(less_equal("pinned_vertex_drift",
            std::abs(finalState->positions.front().y - pinnedY), 1.0e-5, "meters"));
        result.metrics.push_back(greater_equal("free_vertex_fall",
            freeY - finalState->positions.back().y, 1.0e-5, "meters"));
        result.metrics.push_back(less_equal("non_finite_repairs",
            telemetry.nonFiniteCorrections, 0.0, "repairs"));
        result.metrics.push_back(greater_equal("dihedral_constraints_solved",
            telemetry.dihedralConstraintsSolved, 1.0, "constraints"));
    });
}
#endif

#if defined(DVE_ENABLE_PBF_LIQUID)
ValidationFixtureResult pbf_fixture() {
    return timed_fixture("pbf_xpbd", "bounded_particle_liquid", [](ValidationFixtureResult& result) {
        dve::PbfLiquidSettings settings;
        settings.maximumParticles = 512U;
        settings.maximumNeighbors = 96U;
        settings.boundsMinimum = {-0.5F, 0.0F, -0.5F};
        settings.boundsMaximum = {0.5F, 1.0F, 0.5F};
        dve::PbfLiquidWorld world(settings);
        std::string error;
        const auto added = world.add_box({-0.12F, 0.35F, -0.12F},
            {0.12F, 0.59F, 0.12F}, 0.08F, {}, &error);
        if (added == 0U) throw std::runtime_error(error);
        dve::PbfLiquidTelemetry telemetry{};
        for (int frame = 0; frame < 8; ++frame) telemetry = world.step(1.0F / 120.0F);
        float minimumY = std::numeric_limits<float>::max();
        for (const auto& particle : world.particles()) minimumY = std::min(minimumY, particle.position.y);
        result.metrics.push_back(less_equal("non_finite_repairs", telemetry.nonFiniteCorrections, 0.0, "repairs"));
        result.metrics.push_back(less_equal("neighbor_overflow", telemetry.neighborOverflow, 0.0, "pairs"));
        result.metrics.push_back(greater_equal("minimum_particle_height", minimumY,
            settings.boundsMinimum.y + settings.particleRadius - 1.0e-4F, "meters"));
        result.metrics.push_back(greater_equal("density_constraints", telemetry.densityConstraints, 1.0, "constraints"));
    });
}
#endif

#if defined(DVE_ENABLE_VFX_PARTICLES)
dve::VfxModule vfx_module(std::uint32_t id, dve::VfxModuleKind kind, dve::VfxModulePhase phase) {
    dve::VfxModule module;
    module.id = id;
    module.kind = kind;
    module.phase = phase;
    return module;
}

ValidationFixtureResult vfx_fixture() {
    return timed_fixture("vfx", "spawn_event_accounting", [](ValidationFixtureResult& result) {
        dve::VfxParticleGraphAsset graph;
        graph.maximumParticles = 256U;
        graph.durationSeconds = 1.0F;
        auto rate = vfx_module(1U, dve::VfxModuleKind::SpawnRate, dve::VfxModulePhase::Spawn);
        rate.parameters[0] = 90.0F;
        auto lifetime = vfx_module(2U, dve::VfxModuleKind::LifetimeRange, dve::VfxModulePhase::Spawn);
        lifetime.parameters[0] = 0.2F; lifetime.parameters[1] = 0.4F;
        auto gravity = vfx_module(3U, dve::VfxModuleKind::Gravity, dve::VfxModulePhase::Update);
        gravity.parameters[1] = -9.81F;
        auto ground = vfx_module(4U, dve::VfxModuleKind::GroundCollision, dve::VfxModulePhase::Update);
        ground.parameters[0] = 0.0F; ground.parameters[1] = 0.3F;
        auto render = vfx_module(5U, dve::VfxModuleKind::BillboardRenderer, dve::VfxModulePhase::Render);
        graph.modules = {rate, lifetime, gravity, ground, render};
        auto compiled = dve::compile_vfx_particle_graph(graph);
        if (!compiled) throw std::runtime_error(compiled.error);
        dve::VfxCpuRuntime runtime(*compiled.program);
        std::uint64_t spawned{};
        std::uint64_t killed{};
        std::uint64_t events{};
        std::uint64_t overflow{};
        dve::VfxRuntimeTelemetry telemetry{};
        for (int frame = 0; frame < 60; ++frame) {
            telemetry = runtime.step(1.0F / 60.0F);
            spawned += telemetry.spawned;
            killed += telemetry.killed;
            events += telemetry.eventsWritten;
            overflow += telemetry.eventOverflow;
        }
        result.metrics.push_back(greater_equal("spawned", spawned, 1.0, "particles"));
        result.metrics.push_back(greater_equal("events_written", events, 1.0, "events"));
        result.metrics.push_back(less_equal("event_overflow", overflow, 0.0, "events"));
        result.metrics.push_back(range_metric("alive", telemetry.alive, 0.0,
            graph.maximumParticles, "particles"));
        result.metrics.push_back(less_equal("accounting_residual",
            std::abs(static_cast<double>(spawned) - static_cast<double>(killed) - telemetry.alive),
            1.0, "particles"));
    });
}
#endif

#if defined(DVE_ENABLE_FLUODDITY)
ValidationFixtureResult fluoddity_fixture(const std::filesystem::path& sourceRoot) {
    return timed_fixture("fluoddity", "rule_and_trail_replay", [&](ValidationFixtureResult& result) {
        const auto source = sourceRoot / "tests/assets/fluoddity_v7_av0.json";
        const auto imported = dve::import_fluoddity_preset_json(source);
        if (!imported) throw std::runtime_error(imported.error);
        dve::FluoddityReferenceSettings settings;
        settings.trailResolution = 8U;
        settings.deltaSeconds = 1.0F / 120.0F;
        auto state = dve::make_fluoddity_reference_state(*imported.asset, 128U, settings);
        const auto telemetry = dve::step_fluoddity_reference(*imported.asset, settings, state);
        double trailMagnitude{};
        for (const auto& voxel : dve::fluoddity_read_trail(state))
            trailMagnitude += std::abs(voxel.velocity.x) + std::abs(voxel.velocity.y) +
                              std::abs(voxel.velocity.z) + std::abs(voxel.density);
        result.metrics.push_back(range_metric("moved_particles", telemetry.movedParticles,
            128.0, 128.0, "particles"));
        result.metrics.push_back(greater_equal("deposited_particles", telemetry.depositedParticles,
            1.0, "particles"));
        result.metrics.push_back(greater_equal("trail_magnitude", trailMagnitude, 1.0e-8, "absolute"));
        result.metrics.push_back(less_equal("non_finite_repairs", telemetry.nonFiniteCorrections,
            0.0, "repairs"));
    });
}
#endif

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        dve::ValidationEvidenceBundle bundle;
        bundle.engineVersion = "1.62.0";
        bundle.backend = options.backend;
        bundle.suite = options.suite;
        bundle.externallyExecuted = options.externallyExecuted;
        bundle.runId = dve::make_validation_run_id(bundle.engineVersion, bundle.backend,
                                                    bundle.suite, options.seed);
        bundle.environment = dve::ai::JsonValue::Object{
            {"platform", platform_name()},
            {"compiler", compiler_name()},
            {"pointer_bits", static_cast<double>(sizeof(void*) * 8U)},
            {"hardware_threads", static_cast<double>(std::thread::hardware_concurrency())},
            {"source_root", std::filesystem::absolute(options.sourceRoot).generic_string()},
            {"vk_icd_filenames", std::getenv("VK_ICD_FILENAMES") != nullptr
                ? std::string(std::getenv("VK_ICD_FILENAMES")) : std::string{}},
        };
        bundle.shaderAbi = inspect_shader_abi(options.sourceRoot);

        std::string deviceError;
        auto device = make_device(options, deviceError);
        if (!device || device->status() != dve::rhi::DeviceStatus::Ready) {
            bundle.device = dve::ai::JsonValue::Object{
                {"status", "unavailable"}, {"backend", options.backend}, {"reason", deviceError},
            };
            bundle.warnings.emplace_back("Requested device backend was unavailable: " + deviceError);
            ValidationFixtureResult unavailable;
            unavailable.system = "device";
            unavailable.name = "backend_availability";
            unavailable.evidenceClass = "availability";
            unavailable.status = ValidationStatus::Skipped;
            unavailable.notes.emplace_back(deviceError);
            bundle.fixtures.push_back(std::move(unavailable));
            if (options.requireBackend || options.requirePhysical) {
                std::string writeError;
                (void)dve::write_validation_evidence_bundle(options.output, bundle, &writeError);
                throw std::runtime_error("required backend unavailable: " + deviceError);
            }
        } else {
            const auto& capabilities = device->capabilities();
            dve::ai::JsonValue::Array formats;
            const std::array allFormats{
                dve::rhi::TextureFormat::RGBA8Unorm, dve::rhi::TextureFormat::BGRA8Unorm,
                dve::rhi::TextureFormat::R32Uint, dve::rhi::TextureFormat::R32Sint,
                dve::rhi::TextureFormat::RGBA32Sint, dve::rhi::TextureFormat::RGBA16Float,
                dve::rhi::TextureFormat::RG16Uint, dve::rhi::TextureFormat::D32Float,
            };
            for (const auto format : allFormats) {
                const auto support = device->texture_format_capabilities(format);
                formats.emplace_back(dve::ai::JsonValue::Object{
                    {"format", texture_format_name(format)}, {"sampled", support.sampled},
                    {"storage", support.storage}, {"storage_atomic", support.storageAtomic},
                    {"render_target", support.renderTarget}, {"depth_stencil", support.depthStencil},
                });
            }
            bundle.device = dve::ai::JsonValue::Object{
                {"status", "ready"},
                {"backend", std::string(dve::rhi::backend_name(capabilities.backend))},
                {"adapter_name", capabilities.adapterName},
                {"adapter_class", adapter_class_name(capabilities.adapterClass)},
                {"software_adapter", capabilities.softwareAdapter},
                {"physical_adapter_confirmed", is_physical_adapter(capabilities)},
                {"native_limits_queried", capabilities.nativeLimitsQueried},
                {"execution_class", execution_class_name(capabilities)},
                {"vendor_id", static_cast<double>(capabilities.vendorId)},
                {"device_id", static_cast<double>(capabilities.deviceId)},
                {"driver_version", static_cast<double>(capabilities.driverVersion)},
                {"api_version", static_cast<double>(capabilities.apiVersion)},
                {"queue_family", static_cast<double>(capabilities.queueFamilyIndex)},
                {"device_local_memory_bytes", static_cast<double>(capabilities.dedicatedVideoMemoryBytes)},
                {"max_compute_invocations", static_cast<double>(capabilities.maxComputeInvocations)},
                {"max_compute_group_x", static_cast<double>(capabilities.maxComputeWorkgroupSizeX)},
                {"max_compute_group_y", static_cast<double>(capabilities.maxComputeWorkgroupSizeY)},
                {"max_compute_group_z", static_cast<double>(capabilities.maxComputeWorkgroupSizeZ)},
                {"wave_operations", capabilities.waveOperations},
                {"timestamp_queries", capabilities.timestampQueries},
                {"timestamp_valid_bits", static_cast<double>(capabilities.timestampValidBits)},
                {"timestamp_period_nanoseconds", capabilities.timestampPeriodNanoseconds},
                {"indirect_dispatch", capabilities.indirectDispatch},
                {"formats", std::move(formats)},
            };
            const bool physicalAdapter = is_physical_adapter(capabilities);
            if (!physicalAdapter) {
                bundle.warnings.emplace_back(
                    "The selected adapter is not a confirmed integrated or discrete physical GPU; "
                    "this is not physical-GPU evidence.");
            }
            if (options.requirePhysical && !physicalAdapter) {
                bundle.warnings.emplace_back("Physical-adapter requirement failed.");
                ValidationFixtureResult physical;
                physical.system = "device";
                physical.name = "physical_adapter_requirement";
                physical.evidenceClass = "policy_check";
                physical.status = ValidationStatus::Passed;
                physical.metrics.push_back(greater_equal("physical_adapter",
                    physicalAdapter ? 1.0 : 0.0, 1.0, "boolean"));
                physical.evaluate();
                bundle.fixtures.push_back(std::move(physical));
            }
            if (options.suite == "device" || options.suite == "all") {
                bundle.fixtures.push_back(device_capability_fixture(*device));
                bundle.fixtures.push_back(compute_probe_fixture(*device));
                auto kernelFixtures = dve::run_simulation_kernel_conformance(*device);
                bundle.fixtures.insert(bundle.fixtures.end(),
                                       std::make_move_iterator(kernelFixtures.begin()),
                                       std::make_move_iterator(kernelFixtures.end()));
                auto productionFixtures = dve::run_simulation_production_pass_conformance(
                    *device, options.sourceRoot);
                bundle.fixtures.insert(bundle.fixtures.end(),
                                       std::make_move_iterator(productionFixtures.begin()),
                                       std::make_move_iterator(productionFixtures.end()));
            }
        }

        if (options.suite == "simulation" || options.suite == "all") {
#if defined(DVE_ENABLE_GRID_FLUIDS)
            bundle.fixtures.push_back(grid_fluid_fixture());
#endif
#if defined(DVE_ENABLE_FLIP_LIQUIDS)
            bundle.fixtures.push_back(flip_liquid_fixture());
#endif
#if defined(DVE_ENABLE_SOFT_BODIES)
            bundle.fixtures.push_back(soft_body_fixture());
#endif
#if defined(DVE_ENABLE_PBF_LIQUID)
            bundle.fixtures.push_back(pbf_fixture());
#endif
#if defined(DVE_ENABLE_VFX_PARTICLES)
            bundle.fixtures.push_back(vfx_fixture());
#endif
#if defined(DVE_ENABLE_FLUODDITY)
            bundle.fixtures.push_back(fluoddity_fixture(options.sourceRoot));
#endif
        }

        bool kernelReadbackFailure = false;
        if (options.requireKernelReadback) {
            std::size_t readbackFixtures{};
            for (const auto& fixture : bundle.fixtures) {
                if (fixture.evidenceClass == "algorithmic_kernel_readback") {
                    ++readbackFixtures;
                    if (fixture.status != ValidationStatus::Passed) kernelReadbackFailure = true;
                }
            }
            if (readbackFixtures == 0U) kernelReadbackFailure = true;
            if (kernelReadbackFailure)
                bundle.warnings.emplace_back("Algorithmic kernel readback requirement failed.");
        }

        bool productionReadbackFailure = false;
        if (options.requireProductionReadback) {
            std::size_t readbackFixtures{};
            for (const auto& fixture : bundle.fixtures) {
                if (fixture.evidenceClass == "production_pass_specialization_readback") {
                    ++readbackFixtures;
                    if (fixture.status != ValidationStatus::Passed)
                        productionReadbackFailure = true;
                }
            }
            if (readbackFixtures == 0U) productionReadbackFailure = true;
            ValidationFixtureResult productionPolicy;
            productionPolicy.system = "device";
            productionPolicy.name = "production_pass_specialization_requirement";
            productionPolicy.evidenceClass = "policy_check";
            productionPolicy.status = ValidationStatus::Passed;
            productionPolicy.metrics.push_back(greater_equal(
                "production_pass_specializations", productionReadbackFailure ? 0.0 : 1.0,
                1.0, "boolean"));
            productionPolicy.evaluate();
            bundle.fixtures.push_back(std::move(productionPolicy));
            if (productionReadbackFailure)
                bundle.warnings.emplace_back("Production-pass specialization readback requirement failed.");
        }

        bool timestampFailure = false;
        if (options.requireTimestamps) {
            const bool capability = device &&
                device->status() == dve::rhi::DeviceStatus::Ready &&
                device->capabilities().backend != dve::rhi::Backend::Null &&
                device->capabilities().timestampQueries &&
                device->capabilities().timestampValidBits != 0U;
            bool observedTimestampMetric = false;
            for (const auto& fixture : bundle.fixtures) {
                for (const auto& metric : fixture.metrics) {
                    if ((metric.name == "timestamp_delta_raw_ticks" ||
                         metric.name == "device_timestamp_raw_ticks") &&
                        metric.status == ValidationStatus::Passed) {
                        observedTimestampMetric = true;
                    }
                }
            }
            timestampFailure = !capability || !observedTimestampMetric;
            ValidationFixtureResult timestampPolicy;
            timestampPolicy.system = "device";
            timestampPolicy.name = "timestamp_query_requirement";
            timestampPolicy.evidenceClass = "policy_check";
            timestampPolicy.status = ValidationStatus::Passed;
            timestampPolicy.metrics.push_back(greater_equal(
                "timestamp_query_and_readback", timestampFailure ? 0.0 : 1.0,
                1.0, "boolean"));
            timestampPolicy.evaluate();
            bundle.fixtures.push_back(std::move(timestampPolicy));
            if (timestampFailure)
                bundle.warnings.emplace_back("Timestamp-query requirement failed.");
        }

        std::string writeError;
        if (!dve::write_validation_evidence_bundle(options.output, bundle, &writeError))
            throw std::runtime_error(writeError);
        std::cout << "DVE validation evidence written to " << options.output << '\n'
                  << "Run: " << bundle.runId << '\n'
                  << "Status: " << dve::validation_status_name(bundle.status()) << '\n';
        const bool physicalFailure = options.requirePhysical &&
            (!device || device->status() != dve::rhi::DeviceStatus::Ready ||
             !is_physical_adapter(device->capabilities()));
        return bundle.status() == ValidationStatus::Failed || physicalFailure ||
            kernelReadbackFailure || productionReadbackFailure || timestampFailure ? 1 : 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_device_test_runner: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
