#pragma once
// GPU backend of dve_player (IPlayerRenderer on the Vulkan RHI).
//
// Frame: cascaded sun shadow maps -> HDR main pass (environment skybox, voxel objects meshed
// into per-brick greedy quads, polygon assets; live PBR shading with image-based sky light and
// shadowed direct sun) -> present pass (exposure, the environment's tonemap operator, sRGB) into
// the swapchain image, or into an offscreen image that readback() returns.
//
// Voxel meshes and polygon uploads are cached per object and rebuilt only when the object's
// VoxelObject::revision() (destruction, edits) or cell size changes. The sky light is baked from
// RenderEnvironment::skyColor/groundColor and rebaked when they change.
//
// What the CPU renderer does that this one does not yet: traced GI/AO, caustics, fog volumes,
// water, surface marks/decals, prototype HUD overlays. Those objects/passes are skipped, not
// approximated; PlayerRenderStats::skippedObjects counts objects this backend cannot draw.
#include <cstdint>
#include <memory>
#include <string>

#include "dve/platform/application_host.hpp"
#include "dve/player/player_renderer.hpp"
#include "dve/rhi/device.hpp"

namespace dve::player {

struct VulkanPlayerRendererOptions {
    // Window surface to present to. NativeSurfaceKind::Unavailable renders offscreen only (tests,
    // headless --hash runs); readback() is then always current.
    platform::NativeSurfaceHandle surface{};
    std::uint32_t drawableWidth{1280};
    std::uint32_t drawableHeight{720};
    rhi::PresentMode presentMode{rhi::PresentMode::Fifo};
    // Keep readback() current while presenting (screenshots, --hash). Costs a copy per frame.
    bool captureFrames{false};
    float exposure{1.0F};
    // Background: RenderEnvironment::skyColor as the CPU renderer's screen-space zenith gradient
    // (CpuPlayerRendererOptions::skyGradient), or flat when false.
    bool skyGradient{true};
    // Cascaded sun shadows. cascadeCount 0 disables the shadow pass (sun stays unshadowed).
    std::uint32_t shadowCascades{3};
    std::uint32_t shadowCascadeResolution{1024};
    float shadowDistanceMeters{80.0F};
};

// True when this build has the Vulkan backend and its compiled shaders (DXC at build time).
[[nodiscard]] bool vulkan_player_renderer_available() noexcept;

// Null on failure (no Vulkan loader/device, no presentation support for the surface, shaders
// not built); `error` says why so the caller can fall back to the CPU renderer.
[[nodiscard]] std::unique_ptr<IPlayerRenderer> make_vulkan_player_renderer(
    const VulkanPlayerRendererOptions& options, std::string* error = nullptr);

} // namespace dve::player
