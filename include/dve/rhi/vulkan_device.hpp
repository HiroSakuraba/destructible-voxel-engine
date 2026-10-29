#pragma once

#include <memory>
#include <filesystem>
#include <cstdint>

#include "dve/rhi/device.hpp"

namespace dve::rhi {

// Vulkan 1.0 graphics/compute backend: buffer and texture transfer, color/depth targets,
// shader modules, render passes, descriptor binding, compute, timestamps, and window
// presentation (VK_KHR_swapchain on Win32, Xlib, Wayland, Metal and headless surfaces).
//
// Environment:
//   DVE_VULKAN_VALIDATION=1   enable VK_LAYER_KHRONOS_validation when installed; messages are
//                             printed to stderr and counted (validation_*_count()).
//   DVE_VULKAN_ADAPTER=text   pick the adapter whose name contains `text` (case-insensitive);
//                             otherwise discrete > integrated > virtual > CPU.
//   DVE_PSO_CACHE_FILE=path   persistent driver pipeline cache (default constructor).
// Durations measure actual vkCreate*Pipelines driver calls, not shader compilation,
// GPU execution or disk-cache hits. Cache-load success means validated seed accepted,
// not that any individual PSO was a hit.
struct VulkanPipelineCacheStats {
    bool cacheAvailable{}, diskSeedAccepted{}, diskSaveSucceeded{};
    std::uint64_t diskSeedBytes{}, diskSavedBytes{};
    std::uint64_t graphicsAttempts{}, computeAttempts{};
    std::uint64_t graphicsTotalNanoseconds{}, computeTotalNanoseconds{};
    std::uint64_t graphicsMaxNanoseconds{}, computeMaxNanoseconds{};
};

class VulkanDevice final : public IDevice {
public:
    VulkanDevice();
    // Empty path = in-memory cache only. Nonempty path opts into validated
    // persistent cache; DVE_PSO_CACHE_FILE provides the default constructor's path.
    explicit VulkanDevice(std::filesystem::path pipelineCacheFile);
    [[nodiscard]] VulkanPipelineCacheStats pipeline_cache_stats() const noexcept;
    [[nodiscard]] bool flush_pipeline_cache() noexcept;
    // Validation-layer messages seen since creation (0 when the layer is off).
    [[nodiscard]] bool validation_enabled() const noexcept;
    [[nodiscard]] std::uint64_t validation_warning_count() const noexcept;
    [[nodiscard]] std::uint64_t validation_error_count() const noexcept;
    ~VulkanDevice() override;
    VulkanDevice(const VulkanDevice&) = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;

    [[nodiscard]] const DeviceCapabilities& capabilities() const noexcept override;
    [[nodiscard]] TextureFormatCapabilities texture_format_capabilities(
        TextureFormat format) const noexcept override;
    [[nodiscard]] DeviceStatistics statistics() const noexcept override;
    [[nodiscard]] DeviceStatus status() const noexcept override;
    [[nodiscard]] std::string_view device_loss_reason() const noexcept override;

    [[nodiscard]] BufferHandle create_buffer(const BufferDesc&, std::string* = nullptr) override;
    bool destroy_buffer(BufferHandle, std::string* = nullptr) override;
    bool write_buffer(BufferHandle, std::size_t, std::span<const std::byte>, std::string* = nullptr) override;
    bool read_buffer(BufferHandle, std::size_t, std::span<std::byte>, std::string* = nullptr) override;
    bool write_texture(TextureHandle, std::uint32_t, std::uint32_t, std::span<const std::byte>,
                       std::size_t, std::string* = nullptr) override;
    bool read_texture(TextureHandle, std::uint32_t, std::uint32_t, std::span<std::byte>,
                      std::size_t, std::string* = nullptr) override;

    [[nodiscard]] TextureHandle create_texture(const TextureDesc&, std::string* = nullptr) override;
    bool destroy_texture(TextureHandle, std::string* = nullptr) override;
    [[nodiscard]] TextureViewHandle create_texture_view(const TextureViewDesc&, std::string* = nullptr) override;
    bool destroy_texture_view(TextureViewHandle, std::string* = nullptr) override;
    [[nodiscard]] SamplerHandle create_sampler(const SamplerDesc&, std::string* = nullptr) override;
    bool destroy_sampler(SamplerHandle, std::string* = nullptr) override;

    [[nodiscard]] SwapchainHandle create_swapchain(const SwapchainDesc&, std::string* = nullptr) override;
    bool destroy_swapchain(SwapchainHandle, std::string* = nullptr) override;
    bool resize_swapchain(SwapchainHandle, std::uint32_t, std::uint32_t, std::string* = nullptr) override;
    [[nodiscard]] std::uint32_t swapchain_image_count(SwapchainHandle) const noexcept override;
    [[nodiscard]] TextureFormat swapchain_format(SwapchainHandle) const noexcept override;
    [[nodiscard]] bool swapchain_size(SwapchainHandle, std::uint32_t&, std::uint32_t&) const noexcept override;
    [[nodiscard]] AcquiredSwapchainImage acquire_next_image(SwapchainHandle, std::string* = nullptr) override;
    [[nodiscard]] PresentResult present(SwapchainHandle, FenceHandle, std::string* = nullptr) override;

    [[nodiscard]] BindGroupLayoutHandle create_bind_group_layout(
        const BindGroupLayoutDesc&, std::string* = nullptr) override;
    bool destroy_bind_group_layout(BindGroupLayoutHandle, std::string* = nullptr) override;
    [[nodiscard]] BindGroupHandle create_bind_group(
        const BindGroupDesc&, std::string* = nullptr) override;
    bool destroy_bind_group(BindGroupHandle, std::string* = nullptr) override;

    [[nodiscard]] ComputePipelineHandle create_compute_pipeline(const ComputePipelineDesc&, std::string* = nullptr) override;
    bool destroy_compute_pipeline(ComputePipelineHandle, std::string* = nullptr) override;
    [[nodiscard]] GraphicsPipelineHandle create_graphics_pipeline(const GraphicsPipelineDesc&,
                                                                   std::string* = nullptr) override;
    bool destroy_graphics_pipeline(GraphicsPipelineHandle, std::string* = nullptr) override;

    [[nodiscard]] CommandListHandle begin_commands(QueueKind, std::string_view, std::string* = nullptr) override;
    bool copy_buffer(CommandListHandle, BufferHandle, std::size_t, BufferHandle, std::size_t,
                     std::size_t, std::string* = nullptr) override;
    bool transition_buffer(CommandListHandle, BufferHandle, ResourceState, ResourceState,
                           std::string* = nullptr) override;
    bool transition_texture(CommandListHandle, TextureHandle, ResourceState, ResourceState,
                            std::string* = nullptr) override;
    bool dispatch(CommandListHandle, ComputePipelineHandle, std::uint32_t, std::uint32_t,
                  std::uint32_t, std::string* = nullptr) override;
    bool begin_render_pass(CommandListHandle, const RenderPassDesc&, std::string* = nullptr) override;
    bool end_render_pass(CommandListHandle, std::string* = nullptr) override;
    bool bind_graphics_pipeline(CommandListHandle, GraphicsPipelineHandle,
                                std::string* = nullptr) override;
    bool set_viewport(CommandListHandle, const Viewport&, std::string* = nullptr) override;
    bool set_scissor(CommandListHandle, const ScissorRect&, std::string* = nullptr) override;
    bool clear_depth_region(CommandListHandle, float, const ScissorRect&,
                            std::string* = nullptr) override;
    bool bind_vertex_buffer(CommandListHandle, std::uint32_t, BufferHandle, std::size_t,
                            std::uint32_t, std::string* = nullptr) override;
    bool bind_index_buffer(CommandListHandle, BufferHandle, std::size_t, IndexFormat,
                           std::string* = nullptr) override;
    bool bind_compute_bind_group(CommandListHandle, std::uint32_t, BindGroupHandle,
                                 std::string* = nullptr) override;
    bool bind_graphics_bind_group(CommandListHandle, std::uint32_t, BindGroupHandle,
                                  std::string* = nullptr) override;
    bool draw_indexed(CommandListHandle, std::uint32_t, std::uint32_t, std::uint32_t,
                      std::int32_t, std::uint32_t, std::string* = nullptr) override;
    bool begin_debug_label(CommandListHandle, std::string_view, std::string* = nullptr) override;
    bool end_debug_label(CommandListHandle, std::string* = nullptr) override;

    [[nodiscard]] TimestampQueryPoolHandle create_timestamp_query_pool(std::uint32_t,
        std::string_view, std::string* = nullptr) override;
    bool destroy_timestamp_query_pool(TimestampQueryPoolHandle, std::string* = nullptr) override;
    bool write_timestamp(CommandListHandle, TimestampQueryPoolHandle, std::uint32_t,
                         std::string* = nullptr) override;
    bool resolve_timestamps(TimestampQueryPoolHandle, std::uint32_t, std::span<std::uint64_t>,
                            std::string* = nullptr) override;

    [[nodiscard]] FenceHandle submit(CommandListHandle, std::string* = nullptr) override;
    [[nodiscard]] bool fence_complete(FenceHandle) const noexcept override;
    bool wait(FenceHandle, std::string* = nullptr) override;
    void wait_idle() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dve::rhi
