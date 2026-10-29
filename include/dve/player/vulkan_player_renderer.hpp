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
// Ambient occlusion is screen-space (shaders/player_ssao_ps.hlsl): the CPU renderer's ray
// definition marched through the depth buffer, so it only sees what is on screen.
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

// What ambient-occlusion rays can hit.
enum class PlayerAmbientOcclusionMode : std::uint8_t {
    ScreenSpace,   // marches the camera depth buffer: on-screen geometry only, cheap
    VoxelTraced,   // walks the whole scene's voxel grid: off-screen occluders count, voxel objects only
};

// How indirect (bounced) diffuse light is computed. Ambient is the sky/ground hemisphere term the
// CPU player's default quality uses; the traced modes follow the CPU reference's
// GlobalIlluminationMode of the same name and need the scene voxel grid (voxel objects only).
// An environment with global illumination Off, or a Physical sky, always uses Ambient.
enum class PlayerGlobalIlluminationMode : std::uint8_t {
    Ambient,
    VoxelOneBounce,
    RadianceCascades,
};

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
    // Screen-space ambient occlusion, driven by the environment (ambientOcclusionSamples,
    // ambientOcclusionMaxDistanceMeters; off when the samples are 0 or global illumination is
    // Off). Costs a depth pre-pass and one full-screen pass. false disables it regardless.
    bool ambientOcclusion{true};
    PlayerAmbientOcclusionMode ambientOcclusionMode{PlayerAmbientOcclusionMode::ScreenSpace};
    PlayerGlobalIlluminationMode globalIllumination{PlayerGlobalIlluminationMode::Ambient};
    // VoxelTraced: grid cell size in metres (0 = the smallest voxel in the scene) and the largest
    // grid per axis; scenes beyond that keep a window around the camera.
    float tracedAoCellMeters{0.0F};
    std::uint32_t tracedAoMaximumDimension{256U};
};

// True when this build has the Vulkan backend and its compiled shaders (DXC at build time).
[[nodiscard]] bool vulkan_player_renderer_available() noexcept;

// Null on failure (no Vulkan loader/device, no presentation support for the surface, shaders
// not built); `error` says why so the caller can fall back to the CPU renderer.
[[nodiscard]] std::unique_ptr<IPlayerRenderer> make_vulkan_player_renderer(
    const VulkanPlayerRendererOptions& options, std::string* error = nullptr);

} // namespace dve::player
