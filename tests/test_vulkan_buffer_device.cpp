#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include "dve/packed_brickmap.hpp"
#include "dve/render/brickmap_rhi_mirror.hpp"
#include "dve/rhi/vulkan_device.hpp"
#include "dve/voxel_object.hpp"

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    try {
        using namespace dve;
        using namespace dve::rhi;
        VulkanDevice device;
        if (device.status() != DeviceStatus::Ready) {
            if (std::getenv("DVE_REQUIRE_VULKAN") != nullptr)
                throw std::runtime_error(std::string("required Vulkan device unavailable: ") +
                                         std::string(device.device_loss_reason()));
            std::cout << "Vulkan buffer test skipped: " << device.device_loss_reason() << '\n';
            return 0;
        }
        require(device.capabilities().backend == Backend::Vulkan, "wrong backend identity");
        require(!device.capabilities().presentation,
                "v1.34 offscreen Vulkan backend incorrectly claims presentation support");

        std::string error;
        const BufferHandle upload = device.create_buffer({
            64U, BufferUsage::CopySource | BufferUsage::Storage,
            MemoryDomain::Upload, "Vulkan upload"}, &error);
        const BufferHandle deviceLocal = device.create_buffer({
            64U, BufferUsage::CopySource | BufferUsage::CopyDestination | BufferUsage::Storage,
            MemoryDomain::DeviceLocal, "Vulkan device local"}, &error);
        const BufferHandle readback = device.create_buffer({
            64U, BufferUsage::CopyDestination | BufferUsage::Storage,
            MemoryDomain::Readback, "Vulkan readback"}, &error);
        require(static_cast<bool>(upload) && static_cast<bool>(deviceLocal) &&
                static_cast<bool>(readback), error.c_str());

        std::array<std::byte, 32> source{};
        for (std::size_t i = 0; i < source.size(); ++i)
            source[i] = static_cast<std::byte>((i * 11U + 5U) & 0xFFU);
        require(device.write_buffer(upload, 0U, source, &error), error.c_str());

        const CommandListHandle commands = device.begin_commands(QueueKind::Copy,
                                                                  "Vulkan copy contract", &error);
        require(static_cast<bool>(commands), error.c_str());
        require(device.begin_debug_label(commands, "copy", &error), error.c_str());
        require(device.copy_buffer(commands, upload, 0U, deviceLocal, 0U, source.size(), &error),
                error.c_str());
        require(device.copy_buffer(commands, deviceLocal, 0U, readback, 0U, source.size(), &error),
                error.c_str());
        require(device.end_debug_label(commands, &error), error.c_str());
        const FenceHandle fence = device.submit(commands, &error);
        require(fence && device.fence_complete(fence), error.c_str());

        std::array<std::byte, 32> destination{};
        require(device.read_buffer(readback, 0U, destination, &error), error.c_str());
        require(destination == source, "Vulkan GPU copy/readback differs from source bytes");

        require(device.destroy_buffer(upload, &error), error.c_str());
        require(device.destroy_buffer(deviceLocal, &error), error.c_str());
        require(device.destroy_buffer(readback, &error), error.c_str());

        VoxelObject object(116U);
        object.fill_brick({0, 0, 0}, 4U);
        object.set_voxel({8, 1, 0}, 9U);
        PackedBrickmapScene scene;
        scene.rebuild(object);
        require(scene.validate_against(object), "Vulkan test scene is invalid");
        {
            render::PackedBrickmapRhiMirror mirror(device);
            require(mirror.upload(scene, &error), error.c_str());
            require(mirror.readback_matches(scene, &error), error.c_str());
        }

        const DeviceStatistics stats = device.statistics();
        require(stats.buffersCreated == stats.buffersDestroyed,
                "Vulkan exposed buffer lifetime is unbalanced");
        require(stats.copiesExecuted >= 2U, "Vulkan copy commands were not executed");
        std::cout << "Vulkan buffer publication tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Vulkan buffer publication tests failed: " << exception.what() << '\n';
        return 1;
    }
}
