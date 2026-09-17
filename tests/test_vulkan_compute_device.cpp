#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "dve/rhi/vulkan_device.hpp"

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void emit(std::vector<std::uint32_t>& words, std::uint16_t opcode,
          std::initializer_list<std::uint32_t> operands) {
    words.push_back((static_cast<std::uint32_t>(operands.size() + 1U) << 16U) | opcode);
    words.insert(words.end(), operands.begin(), operands.end());
}
std::vector<std::uint32_t> encoded_string(const char* text) {
    const std::size_t bytes = std::strlen(text) + 1U;
    std::vector<std::uint32_t> words((bytes + 3U) / 4U, 0U);
    std::memcpy(words.data(), text, bytes);
    return words;
}
void emit_entry_point(std::vector<std::uint32_t>& words, std::uint32_t model,
                      std::uint32_t function, const char* name) {
    auto stringWords = encoded_string(name);
    const std::uint32_t count = 3U + static_cast<std::uint32_t>(stringWords.size());
    words.push_back((count << 16U) | 15U); // OpEntryPoint
    words.push_back(model);
    words.push_back(function);
    words.insert(words.end(), stringWords.begin(), stringWords.end());
}
std::vector<std::byte> as_bytes(const std::vector<std::uint32_t>& words) {
    std::vector<std::byte> result(words.size() * sizeof(std::uint32_t));
    std::memcpy(result.data(), words.data(), result.size());
    return result;
}
std::vector<std::byte> storage_write_shader() {
    // SPIR-V 1.0 equivalent to:
    // layout(local_size_x=1) in;
    // layout(set=0,binding=0,std430) buffer Data { uint values[]; } data;
    // void main() { data.values[0] = 0x12345678u; }
    std::vector<std::uint32_t> w{0x07230203U, 0x00010000U, 0U, 14U, 0U};
    emit(w, 17, {1U});                    // OpCapability Shader
    emit(w, 14, {0U, 1U});               // OpMemoryModel Logical GLSL450
    emit_entry_point(w, 5U, 11U, "main"); // GLCompute
    emit(w, 16, {11U, 17U, 1U, 1U, 1U}); // OpExecutionMode LocalSize
    emit(w, 71, {5U, 6U, 4U});            // runtime array ArrayStride 4
    emit(w, 72, {6U, 0U, 35U, 0U});       // struct member Offset 0
    emit(w, 71, {6U, 3U});                // BufferBlock
    emit(w, 71, {9U, 34U, 0U});           // DescriptorSet 0
    emit(w, 71, {9U, 33U, 0U});           // Binding 0
    emit(w, 19, {1U});                    // void
    emit(w, 33, {2U, 1U});                // function type
    emit(w, 21, {3U, 32U, 0U});           // uint
    emit(w, 43, {3U, 4U, 0U});            // uint 0
    emit(w, 29, {5U, 3U});                // runtime array uint
    emit(w, 30, {6U, 5U});                // buffer struct
    emit(w, 32, {7U, 2U, 6U});            // ptr Uniform struct
    emit(w, 32, {8U, 2U, 3U});            // ptr Uniform uint
    emit(w, 59, {7U, 9U, 2U});            // variable Uniform
    emit(w, 43, {3U, 10U, 0x12345678U});  // output value
    emit(w, 54, {1U, 11U, 0U, 2U});       // function main
    emit(w, 248, {12U});                  // label
    emit(w, 65, {8U, 13U, 9U, 4U, 4U});   // &data.values[0]
    emit(w, 62, {13U, 10U});              // store
    emit(w, 253, {});                      // return
    emit(w, 56, {});                       // function end
    return as_bytes(w);
}

std::vector<std::byte> atomic_add_shader() {
    // Every invocation atomically adds one to a flattened signed accumulator.
    std::vector<std::uint32_t> w{0x07230203U, 0x00010000U, 0U, 18U, 0U};
    emit(w, 17, {1U});                    // OpCapability Shader
    emit(w, 14, {0U, 1U});               // OpMemoryModel Logical GLSL450
    emit_entry_point(w, 5U, 14U, "main");
    emit(w, 16, {14U, 17U, 8U, 1U, 1U}); // LocalSize 8
    emit(w, 71, {9U, 6U, 4U});            // ArrayStride 4
    emit(w, 72, {10U, 0U, 35U, 0U});      // Offset 0
    emit(w, 71, {10U, 3U});               // BufferBlock
    emit(w, 71, {13U, 34U, 0U});          // DescriptorSet 0
    emit(w, 71, {13U, 33U, 0U});          // Binding 0
    emit(w, 19, {1U});                    // void
    emit(w, 33, {2U, 1U});                // function type
    emit(w, 21, {3U, 32U, 1U});           // int
    emit(w, 21, {4U, 32U, 0U});           // uint
    emit(w, 43, {3U, 5U, 0U});            // int 0
    emit(w, 43, {4U, 6U, 0U});            // uint 0 / relaxed semantics
    emit(w, 43, {3U, 7U, 1U});            // int 1
    emit(w, 43, {4U, 8U, 1U});            // Scope Device
    emit(w, 29, {9U, 3U});                // runtime array int
    emit(w, 30, {10U, 9U});               // buffer struct
    emit(w, 32, {11U, 2U, 10U});          // ptr Uniform struct
    emit(w, 32, {12U, 2U, 3U});           // ptr Uniform int
    emit(w, 59, {11U, 13U, 2U});          // variable Uniform
    emit(w, 54, {1U, 14U, 0U, 2U});       // function main
    emit(w, 248, {15U});                  // label
    emit(w, 65, {12U, 16U, 13U, 5U, 5U}); // &data.values[0]
    emit(w, 234, {3U, 17U, 16U, 8U, 6U, 7U}); // OpAtomicIAdd
    emit(w, 253, {});
    emit(w, 56, {});
    return as_bytes(w);
}
} // namespace

int main() {
    try {
        using namespace dve::rhi;
        VulkanDevice device;
        if (device.status() != DeviceStatus::Ready) {
            if (std::getenv("DVE_REQUIRE_VULKAN") != nullptr)
                throw std::runtime_error(std::string("required Vulkan compute device unavailable: ") +
                                         std::string(device.device_loss_reason()));
            std::cout << "Vulkan compute test skipped: " << device.device_loss_reason() << '\n';
            return 0;
        }
        std::string error;
        const auto signedFormat = device.texture_format_capabilities(TextureFormat::R32Sint);
        require(signedFormat.storage, "Vulkan R32Sint must support storage images for Fluoddity");
        require(signedFormat.storageAtomic,
                "Vulkan R32Sint must support storage-image atomics for the fixed-point path");
        const BufferHandle buffer = device.create_buffer({
            16U, BufferUsage::Storage | BufferUsage::CopyDestination,
            MemoryDomain::Upload, "compute storage output", ResourceState::ShaderWrite}, &error);
        require(static_cast<bool>(buffer), error.c_str());
        const std::uint32_t zero{};
        require(device.write_buffer(buffer, 0U,
                                    std::as_bytes(std::span<const std::uint32_t>(&zero, 1U)),
                                    &error), error.c_str());

        BindGroupLayoutDesc layoutDesc;
        layoutDesc.debugName = "compute storage layout";
        layoutDesc.bindings.push_back({0U, BindingType::StorageBufferReadWrite,
                                      ShaderStage::Compute});
        const BindGroupLayoutHandle layout = device.create_bind_group_layout(layoutDesc, &error);
        require(static_cast<bool>(layout), error.c_str());

        BindGroupDesc groupDesc;
        groupDesc.layout = layout;
        groupDesc.debugName = "compute storage group";
        groupDesc.entries.push_back({0U, buffer, {}, 0U, 16U});
        const BindGroupHandle group = device.create_bind_group(groupDesc, &error);
        require(static_cast<bool>(group), error.c_str());

        const TextureHandle volume = device.create_texture({
            TextureDimension::Texture3D, TextureFormat::R32Sint, 8U, 8U, 8U, 1U, 1U,
            TextureUsage::Storage, ResourceState::ShaderWrite, "signed 3D accumulation volume"},
            &error);
        require(static_cast<bool>(volume), error.c_str());
        const TextureViewHandle volumeView = device.create_texture_view(
            {volume, 0U, 1U, 0U, 1U, "signed 3D accumulation view"}, &error);
        require(static_cast<bool>(volumeView), error.c_str());
        BindGroupLayoutDesc imageLayoutDesc;
        imageLayoutDesc.debugName = "3D storage image layout";
        imageLayoutDesc.bindings.push_back({0U, BindingType::StorageTexture,
                                           ShaderStage::Compute});
        const BindGroupLayoutHandle imageLayout =
            device.create_bind_group_layout(imageLayoutDesc, &error);
        require(static_cast<bool>(imageLayout), error.c_str());
        BindGroupDesc imageGroupDesc;
        imageGroupDesc.layout = imageLayout;
        imageGroupDesc.debugName = "3D storage image group";
        imageGroupDesc.entries.push_back({0U, {}, volumeView, 0U, 0U});
        const BindGroupHandle imageGroup = device.create_bind_group(imageGroupDesc, &error);
        require(static_cast<bool>(imageGroup), error.c_str());

        ComputePipelineDesc pipelineDesc;
        pipelineDesc.debugName = "embedded storage write compute";
        pipelineDesc.bytecode = storage_write_shader();
        pipelineDesc.bindGroupLayouts.push_back(layout);
        pipelineDesc.bindGroupLayouts.push_back(imageLayout);
        const ComputePipelineHandle pipeline = device.create_compute_pipeline(pipelineDesc, &error);
        require(static_cast<bool>(pipeline), error.c_str());

        TimestampQueryPoolHandle timestamps{};
        if (device.capabilities().timestampQueries) {
            timestamps = device.create_timestamp_query_pool(2U, "compute dispatch timestamps", &error);
            require(static_cast<bool>(timestamps), error.c_str());
        }
        const CommandListHandle commands = device.begin_commands(QueueKind::Compute,
                                                                  "storage write dispatch", &error);
        require(static_cast<bool>(commands), error.c_str());
        if (timestamps) require(device.write_timestamp(commands, timestamps, 0U, &error), error.c_str());
        require(device.bind_compute_bind_group(commands, 0U, group, &error), error.c_str());
        require(device.bind_compute_bind_group(commands, 1U, imageGroup, &error), error.c_str());
        require(device.dispatch(commands, pipeline, 1U, 1U, 1U, &error), error.c_str());
        if (timestamps) require(device.write_timestamp(commands, timestamps, 1U, &error), error.c_str());
        const FenceHandle fence = device.submit(commands, &error);
        require(static_cast<bool>(fence) && device.fence_complete(fence), error.c_str());
        if (timestamps) {
            std::array<std::uint64_t, 2> values{};
            require(device.resolve_timestamps(timestamps, 0U, values, &error), error.c_str());
            require(values[1] >= values[0], "Vulkan compute timestamp ordering is invalid");
        }

        std::uint32_t value{};
        require(device.read_buffer(buffer, 0U,
                                   std::as_writable_bytes(std::span<std::uint32_t>(&value, 1U)),
                                   &error), error.c_str());
        require(value == 0x12345678U, "Vulkan compute shader did not write the storage buffer");
        require(device.statistics().dispatchesExecuted == 1U,
                "Vulkan dispatch statistics did not advance");

        require(device.write_buffer(buffer, 0U,
                                    std::as_bytes(std::span<const std::uint32_t>(&zero, 1U)),
                                    &error), error.c_str());
        ComputePipelineDesc atomicDesc;
        atomicDesc.debugName = "signed fixed-point atomic accumulation";
        atomicDesc.bytecode = atomic_add_shader();
        atomicDesc.bindGroupLayouts.push_back(layout);
        atomicDesc.threadsX = 8U;
        const ComputePipelineHandle atomicPipeline =
            device.create_compute_pipeline(atomicDesc, &error);
        require(static_cast<bool>(atomicPipeline), error.c_str());
        const CommandListHandle atomicCommands = device.begin_commands(
            QueueKind::Compute, "signed atomic accumulation", &error);
        require(static_cast<bool>(atomicCommands), error.c_str());
        require(device.bind_compute_bind_group(atomicCommands, 0U, group, &error), error.c_str());
        require(device.dispatch(atomicCommands, atomicPipeline, 4U, 1U, 1U, &error),
                error.c_str());
        const FenceHandle atomicFence = device.submit(atomicCommands, &error);
        require(static_cast<bool>(atomicFence), error.c_str());
        std::int32_t accumulated{};
        require(device.read_buffer(buffer, 0U,
                                   std::as_writable_bytes(std::span<std::int32_t>(&accumulated, 1U)),
                                   &error), error.c_str());
        require(accumulated == 32, "signed fixed-point atomic accumulation is incorrect");
        require(device.statistics().dispatchesExecuted == 2U,
                "Vulkan atomic dispatch statistics did not advance");
        require(device.destroy_compute_pipeline(atomicPipeline, &error), error.c_str());

        if (timestamps) require(device.destroy_timestamp_query_pool(timestamps, &error), error.c_str());
        require(device.destroy_compute_pipeline(pipeline, &error), error.c_str());
        require(device.destroy_bind_group(imageGroup, &error), error.c_str());
        require(device.destroy_bind_group_layout(imageLayout, &error), error.c_str());
        require(device.destroy_texture_view(volumeView, &error), error.c_str());
        require(device.destroy_texture(volume, &error), error.c_str());
        require(device.destroy_bind_group(group, &error), error.c_str());
        require(device.destroy_bind_group_layout(layout, &error), error.c_str());
        require(device.destroy_buffer(buffer, &error), error.c_str());
        std::cout << "dve_vulkan_compute_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_vulkan_compute_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
