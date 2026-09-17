#pragma once

#include <memory>

#include "dve/rhi/device.hpp"

namespace dve::rhi {

// Vulkan 1.0 offscreen graphics backend. v1.34 certifies buffer and texture transfer,
// color/depth targets, shader modules, render passes, dynamic viewport/scissor state, and
// indexed/instanced draws. Presentation, descriptor binding, compute, and timestamps remain
// explicit unsupported operations until their dedicated work packages.
class VulkanDevice final : public IDevice {
public:
    VulkanDevice();
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
