// Vulkan presentation (VK_KHR_swapchain) contract on a headless surface.
//
// VK_EXT_headless_surface lets the full acquire -> render -> present cycle run without a window
// system (Mesa lavapipe/llvmpipe and most desktop drivers on Linux expose it), so CI exercises
// the same swapchain code path dve_player uses on Win32/Xlib/Wayland. Drivers without the
// extension skip honestly (exit 77).
//
// Checks: swapchain creation, image count/format, Present resting state, clear + readback of
// an acquired image, present, resize (image handles invalidated, new size honoured), error
// paths (double acquire, present without acquire, destroying a swapchain-owned texture), and,
// when DVE_VULKAN_VALIDATION=1 with the Khronos layer installed, zero validation errors.
#include "dve_test_skip.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "dve/rhi/vulkan_device.hpp"

namespace {

using namespace dve::rhi;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

// Clears the acquired image, reads it back, returns it to Present and presents it.
void clear_and_present(VulkanDevice& device, SwapchainHandle swapchain, std::uint32_t width,
                       std::uint32_t height, float red, float green, float blue) {
    std::string error;
    const AcquiredSwapchainImage image = device.acquire_next_image(swapchain, &error);
    require(!image.outOfDate, "fresh headless swapchain reported out of date");
    require(static_cast<bool>(image.texture), "acquire failed: " + error);

    // A second acquire before present is a contract error.
    std::string doubleAcquire;
    require(!device.acquire_next_image(swapchain, &doubleAcquire).texture &&
                doubleAcquire.find("already acquired") != std::string::npos,
            "double acquire was not rejected");

    const CommandListHandle commands = device.begin_commands(QueueKind::Graphics, "present clear", &error);
    require(static_cast<bool>(commands), error);
    require(device.transition_texture(commands, image.texture, ResourceState::Present,
                                      ResourceState::RenderTarget, &error), error);
    RenderPassDesc pass;
    pass.colors.push_back({image.texture, true, red, green, blue, 1.0F});
    pass.debugName = "swapchain clear";
    require(device.begin_render_pass(commands, pass, &error), error);
    require(device.end_render_pass(commands, &error), error);
    require(device.transition_texture(commands, image.texture, ResourceState::RenderTarget,
                                      ResourceState::CopySource, &error), error);
    const FenceHandle fence = device.submit(commands, &error);
    require(fence && device.fence_complete(fence), "submit failed: " + error);

    std::vector<std::byte> pixels(static_cast<std::size_t>(width) * height * 4U);
    require(device.read_texture(image.texture, 0U, 0U, pixels, static_cast<std::size_t>(width) * 4U, &error),
            "swapchain readback failed: " + error);
    const TextureFormat format = device.swapchain_format(swapchain);
    const std::size_t centre = (static_cast<std::size_t>(height / 2U) * width + width / 2U) * 4U;
    const auto channel = [&](std::size_t c) { return std::to_integer<int>(pixels[centre + c]); };
    const int r = format == TextureFormat::BGRA8Unorm ? channel(2) : channel(0);
    const int g = channel(1);
    const int b = format == TextureFormat::BGRA8Unorm ? channel(0) : channel(2);
    const auto near = [](int value, float expected) {
        const int target = static_cast<int>(expected * 255.0F + 0.5F);
        return value >= target - 2 && value <= target + 2;
    };
    require(near(r, red) && near(g, green) && near(b, blue),
            "swapchain clear colour mismatch: got " + std::to_string(r) + "," + std::to_string(g) + "," +
                std::to_string(b));

    const CommandListHandle back = device.begin_commands(QueueKind::Graphics, "to present", &error);
    require(device.transition_texture(back, image.texture, ResourceState::CopySource,
                                      ResourceState::Present, &error), error);
    const FenceHandle backFence = device.submit(back, &error);
    require(static_cast<bool>(backFence), error);
    const PresentResult presented = device.present(swapchain, backFence, &error);
    require(presented == PresentResult::Presented || presented == PresentResult::Suboptimal,
            "present failed: " + error);
}

int run() {
    VulkanDevice device;
    if (device.status() != DeviceStatus::Ready) {
        if (std::getenv("DVE_REQUIRE_VULKAN") != nullptr)
            throw std::runtime_error("Vulkan required: " + std::string(device.device_loss_reason()));
        return dve::testing::skip("Vulkan presentation: " + std::string(device.device_loss_reason()));
    }
    std::cout << "adapter: " << device.capabilities().adapterName << '\n';
    if (!device.capabilities().presentation)
        return dve::testing::skip("Vulkan presentation: VK_KHR_surface/VK_KHR_swapchain unavailable");

    SwapchainDesc desc;
    desc.surface.kind = dve::platform::NativeSurfaceKind::Headless;
    desc.width = 96U;
    desc.height = 64U;
    desc.imageCount = 3U;
    desc.presentMode = PresentMode::Fifo;
    desc.debugName = "headless presentation test";
    std::string error;
    const SwapchainHandle swapchain = device.create_swapchain(desc, &error);
    if (!swapchain && error.find("VK_EXT_headless_surface") != std::string::npos)
        return dve::testing::skip("Vulkan presentation: " + error);
    require(static_cast<bool>(swapchain), "create_swapchain failed: " + error);
    const std::uint32_t imageCount = device.swapchain_image_count(swapchain);
    require(imageCount >= 2U, "swapchain has fewer than two images");
    const TextureFormat format = device.swapchain_format(swapchain);
    require(format == TextureFormat::BGRA8Unorm || format == TextureFormat::RGBA8Unorm ||
                format == TextureFormat::RGBA8Srgb,
            "unexpected swapchain format");

    std::string presentError;
    require(device.present(swapchain, {}, &presentError) == PresentResult::Error &&
                presentError.find("without acquiring") != std::string::npos,
            "present without acquire was not rejected");

    // Several frames so every image in the chain is used at least once.
    for (std::uint32_t frame = 0U; frame < imageCount + 2U; ++frame)
        clear_and_present(device, swapchain, desc.width, desc.height, 0.25F, 0.5F, 0.75F);

    // Swapchain-owned images cannot be destroyed through the texture API.
    {
        const AcquiredSwapchainImage image = device.acquire_next_image(swapchain, &error);
        require(static_cast<bool>(image.texture), error);
        std::string destroyError;
        require(!device.destroy_texture(image.texture, &destroyError) &&
                    destroyError.find("owned by their swapchain") != std::string::npos,
                "destroying a swapchain image was not rejected");
        std::string resizeError;
        require(!device.resize_swapchain(swapchain, 48U, 32U, &resizeError),
                "resize while an image is acquired was not rejected");
        const CommandListHandle commands = device.begin_commands(QueueKind::Graphics, "idle frame", &error);
        require(device.transition_texture(commands, image.texture, ResourceState::Present,
                                          ResourceState::RenderTarget, &error), error);
        require(device.transition_texture(commands, image.texture, ResourceState::RenderTarget,
                                          ResourceState::Present, &error), error);
        const FenceHandle fence = device.submit(commands, &error);
        require(device.present(swapchain, fence, &error) != PresentResult::Error, error);
        const AcquiredSwapchainImage stale = image;
        require(device.resize_swapchain(swapchain, 48U, 32U, &error), "resize failed: " + error);
        std::string staleError;
        require(!device.read_texture(stale.texture, 0U, 0U, std::span<std::byte>{}, 0U, &staleError),
                "image handle survived a swapchain resize");
    }
    for (std::uint32_t frame = 0U; frame < 3U; ++frame)
        clear_and_present(device, swapchain, 48U, 32U, 1.0F, 0.0F, 0.5F);

    require(device.destroy_swapchain(swapchain, &error), "destroy_swapchain failed: " + error);
    const DeviceStatistics stats = device.statistics();
    require(stats.swapchainsCreated == 1U && stats.swapchainsDestroyed == 1U, "swapchain counters are wrong");
    require(stats.presents >= imageCount + 6U, "present counter is wrong");

    if (device.validation_enabled()) {
        std::cout << "validation: " << device.validation_error_count() << " errors, "
                  << device.validation_warning_count() << " warnings\n";
        require(device.validation_error_count() == 0U, "Vulkan validation reported errors");
    } else {
        std::cout << "validation: layer off (set DVE_VULKAN_VALIDATION=1 with the Khronos layer installed)\n";
    }
    std::cout << "Vulkan headless presentation tests passed (" << imageCount << " images)\n";
    return dve::testing::exit_code(false);
}

} // namespace

int main() {
    try {
        return run();
    } catch (const std::exception& exception) {
        std::cerr << "dve_vulkan_presentation_tests: " << exception.what() << '\n';
        return 1;
    }
}
