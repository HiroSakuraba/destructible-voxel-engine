#pragma once

#include <variant>

#include "dve/rhi/device.hpp"

namespace dve::rhi {

// Deterministic validation backend. It performs real bounds-checked CPU copies, validates
// resource states/lifetimes, models swapchain acquire/present, and records synthetic timestamps.
class NullDevice final : public IDevice {
public:
    NullDevice();

    [[nodiscard]] const DeviceCapabilities& capabilities() const noexcept override { return capabilities_; }
    [[nodiscard]] TextureFormatCapabilities texture_format_capabilities(
        TextureFormat format) const noexcept override;
    [[nodiscard]] DeviceStatistics statistics() const noexcept override { return statistics_; }
    [[nodiscard]] DeviceStatus status() const noexcept override { return status_; }
    [[nodiscard]] std::string_view device_loss_reason() const noexcept override { return lossReason_; }

    [[nodiscard]] BufferHandle create_buffer(const BufferDesc& desc,
                                             std::string* error = nullptr) override;
    bool destroy_buffer(BufferHandle handle, std::string* error = nullptr) override;
    bool write_buffer(BufferHandle handle, std::size_t offset, std::span<const std::byte> bytes,
                      std::string* error = nullptr) override;
    bool read_buffer(BufferHandle handle, std::size_t offset, std::span<std::byte> destination,
                     std::string* error = nullptr) override;
    bool write_texture(TextureHandle handle, std::uint32_t mipLevel,
                       std::uint32_t arrayLayer, std::span<const std::byte> bytes,
                       std::size_t rowPitchBytes, std::string* error = nullptr) override;
    bool read_texture(TextureHandle handle, std::uint32_t mipLevel,
                      std::uint32_t arrayLayer, std::span<std::byte> destination,
                      std::size_t rowPitchBytes, std::string* error = nullptr) override;

    [[nodiscard]] TextureHandle create_texture(const TextureDesc& desc,
                                               std::string* error = nullptr) override;
    bool destroy_texture(TextureHandle handle, std::string* error = nullptr) override;
    [[nodiscard]] TextureViewHandle create_texture_view(const TextureViewDesc& desc,
                                                        std::string* error = nullptr) override;
    bool destroy_texture_view(TextureViewHandle handle, std::string* error = nullptr) override;
    [[nodiscard]] SamplerHandle create_sampler(const SamplerDesc& desc,
                                               std::string* error = nullptr) override;
    bool destroy_sampler(SamplerHandle handle, std::string* error = nullptr) override;

    [[nodiscard]] SwapchainHandle create_swapchain(const SwapchainDesc& desc,
                                                   std::string* error = nullptr) override;
    bool destroy_swapchain(SwapchainHandle handle, std::string* error = nullptr) override;
    [[nodiscard]] AcquiredSwapchainImage acquire_next_image(
        SwapchainHandle handle, std::string* error = nullptr) override;
    [[nodiscard]] PresentResult present(SwapchainHandle handle, FenceHandle waitFence,
                                        std::string* error = nullptr) override;

    [[nodiscard]] BindGroupLayoutHandle create_bind_group_layout(
        const BindGroupLayoutDesc& desc, std::string* error = nullptr) override;
    bool destroy_bind_group_layout(BindGroupLayoutHandle handle,
                                   std::string* error = nullptr) override;
    [[nodiscard]] BindGroupHandle create_bind_group(
        const BindGroupDesc& desc, std::string* error = nullptr) override;
    bool destroy_bind_group(BindGroupHandle handle,
                            std::string* error = nullptr) override;

    [[nodiscard]] ComputePipelineHandle create_compute_pipeline(
        const ComputePipelineDesc& desc, std::string* error = nullptr) override;
    bool destroy_compute_pipeline(ComputePipelineHandle handle,
                                  std::string* error = nullptr) override;
    [[nodiscard]] GraphicsPipelineHandle create_graphics_pipeline(
        const GraphicsPipelineDesc& desc, std::string* error = nullptr) override;
    bool destroy_graphics_pipeline(GraphicsPipelineHandle handle,
                                   std::string* error = nullptr) override;

    [[nodiscard]] CommandListHandle begin_commands(QueueKind queue, std::string_view debugName,
                                                   std::string* error = nullptr) override;
    bool copy_buffer(CommandListHandle commands, BufferHandle source, std::size_t sourceOffset,
                     BufferHandle destination, std::size_t destinationOffset, std::size_t bytes,
                     std::string* error = nullptr) override;
    bool transition_buffer(CommandListHandle commands, BufferHandle buffer, ResourceState before,
                           ResourceState after, std::string* error = nullptr) override;
    bool transition_texture(CommandListHandle commands, TextureHandle texture, ResourceState before,
                            ResourceState after, std::string* error = nullptr) override;
    bool dispatch(CommandListHandle commands, ComputePipelineHandle pipeline,
                  std::uint32_t groupsX, std::uint32_t groupsY, std::uint32_t groupsZ,
                  std::string* error = nullptr) override;
    bool begin_render_pass(CommandListHandle commands, const RenderPassDesc& desc,
                           std::string* error = nullptr) override;
    bool end_render_pass(CommandListHandle commands, std::string* error = nullptr) override;
    bool bind_graphics_pipeline(CommandListHandle commands, GraphicsPipelineHandle pipeline,
                                std::string* error = nullptr) override;
    bool set_viewport(CommandListHandle commands, const Viewport& viewport,
                      std::string* error = nullptr) override;
    bool set_scissor(CommandListHandle commands, const ScissorRect& scissor,
                     std::string* error = nullptr) override;
    bool clear_depth_region(CommandListHandle commands, float depth,
                            const ScissorRect& region,
                            std::string* error = nullptr) override;
    bool bind_vertex_buffer(CommandListHandle commands, std::uint32_t slot,
                            BufferHandle buffer, std::size_t offset, std::uint32_t stride,
                            std::string* error = nullptr) override;
    bool bind_index_buffer(CommandListHandle commands, BufferHandle buffer,
                           std::size_t offset, IndexFormat format,
                           std::string* error = nullptr) override;
    bool bind_compute_bind_group(CommandListHandle commands, std::uint32_t index,
                                 BindGroupHandle group,
                                 std::string* error = nullptr) override;
    bool bind_graphics_bind_group(CommandListHandle commands, std::uint32_t index,
                                  BindGroupHandle group,
                                  std::string* error = nullptr) override;
    bool draw_indexed(CommandListHandle commands, std::uint32_t indexCount,
                      std::uint32_t instanceCount, std::uint32_t firstIndex,
                      std::int32_t vertexOffset, std::uint32_t firstInstance,
                      std::string* error = nullptr) override;
    bool begin_debug_label(CommandListHandle commands, std::string_view label,
                           std::string* error = nullptr) override;
    bool end_debug_label(CommandListHandle commands, std::string* error = nullptr) override;

    [[nodiscard]] TimestampQueryPoolHandle create_timestamp_query_pool(
        std::uint32_t count, std::string_view debugName, std::string* error = nullptr) override;
    bool destroy_timestamp_query_pool(TimestampQueryPoolHandle handle,
                                      std::string* error = nullptr) override;
    bool write_timestamp(CommandListHandle commands, TimestampQueryPoolHandle pool,
                         std::uint32_t index, std::string* error = nullptr) override;
    bool resolve_timestamps(TimestampQueryPoolHandle pool, std::uint32_t first,
                            std::span<std::uint64_t> destination,
                            std::string* error = nullptr) override;

    [[nodiscard]] FenceHandle submit(CommandListHandle commands,
                                     std::string* error = nullptr) override;
    [[nodiscard]] bool fence_complete(FenceHandle fence) const noexcept override;
    bool wait(FenceHandle fence, std::string* error = nullptr) override;
    void wait_idle() noexcept override;

    void simulate_device_loss(std::string reason);

private:
    struct BufferSlot {
        std::uint32_t generation{1}; bool alive{}; BufferDesc desc;
        ResourceState state{ResourceState::Undefined}; std::vector<std::byte> storage;
    };
    struct TextureSlot {
        std::uint32_t generation{1}; bool alive{}; TextureDesc desc;
        ResourceState state{ResourceState::Undefined}; bool swapchainOwned{};
        std::vector<std::byte> storage;
    };
    struct TextureViewSlot {
        std::uint32_t generation{1}; bool alive{}; TextureViewDesc desc;
    };
    struct SamplerSlot {
        std::uint32_t generation{1}; bool alive{}; SamplerDesc desc;
    };
    struct BindGroupLayoutSlot {
        std::uint32_t generation{1}; bool alive{}; BindGroupLayoutDesc desc;
    };
    struct BindGroupSlot {
        std::uint32_t generation{1}; bool alive{}; BindGroupDesc desc;
    };
    struct PipelineSlot {
        std::uint32_t generation{1}; bool alive{}; ComputePipelineDesc desc;
    };
    struct GraphicsPipelineSlot {
        std::uint32_t generation{1}; bool alive{}; GraphicsPipelineDesc desc;
    };
    struct SwapchainSlot {
        std::uint32_t generation{1}; bool alive{}; SwapchainDesc desc;
        std::vector<TextureHandle> images; std::uint32_t nextImage{};
        std::uint32_t acquiredIndex{}; bool acquired{};
    };
    struct TimestampPoolSlot {
        std::uint32_t generation{1}; bool alive{}; std::string debugName;
        std::vector<std::uint64_t> values; std::vector<bool> available;
    };

    struct CopyCommand { BufferHandle source; std::size_t sourceOffset{}; BufferHandle destination; std::size_t destinationOffset{}; std::size_t bytes{}; };
    struct BufferBarrierCommand { BufferHandle buffer; ResourceState before{}; ResourceState after{}; };
    struct TextureBarrierCommand { TextureHandle texture; ResourceState before{}; ResourceState after{}; };
    struct DispatchCommand { ComputePipelineHandle pipeline; std::uint32_t groupsX{}; std::uint32_t groupsY{}; std::uint32_t groupsZ{}; };
    struct BeginRenderPassCommand { RenderPassDesc desc; };
    struct EndRenderPassCommand {};
    struct ClearDepthRegionCommand { TextureHandle depthTexture; float depth{}; ScissorRect region; };
    struct DrawIndexedCommand { std::uint32_t indexCount{}; std::uint32_t instanceCount{}; std::uint32_t firstIndex{}; std::int32_t vertexOffset{}; std::uint32_t firstInstance{}; };
    struct TimestampCommand { TimestampQueryPoolHandle pool; std::uint32_t index{}; };
    using RecordedCommand = std::variant<CopyCommand, BufferBarrierCommand, TextureBarrierCommand,
                                         DispatchCommand, BeginRenderPassCommand,
                                         EndRenderPassCommand, ClearDepthRegionCommand,
                                         DrawIndexedCommand, TimestampCommand>;
    struct CommandSlot {
        std::uint32_t generation{1}; bool alive{}; QueueKind queue{QueueKind::Graphics};
        std::string debugName; std::vector<RecordedCommand> commands; std::uint32_t debugDepth{};
        bool renderPassOpen{};
        TextureHandle activeDepthTexture{};
        std::uint32_t activePassWidth{};
        std::uint32_t activePassHeight{};
        GraphicsPipelineHandle graphicsPipeline{};
        BufferHandle vertexBuffer{};
        std::size_t vertexOffset{};
        std::uint32_t vertexStride{};
        BufferHandle indexBuffer{};
        std::size_t indexOffset{};
        IndexFormat indexFormat{IndexFormat::Uint32};
        Viewport viewport{};
        ScissorRect scissor{};
        bool viewportSet{};
        bool scissorSet{};
        std::vector<BindGroupHandle> computeBindGroups;
        std::vector<BindGroupHandle> graphicsBindGroups;
    };

    [[nodiscard]] bool ready(std::string* error) const;
    [[nodiscard]] BufferSlot* buffer(BufferHandle handle, std::string* error);
    [[nodiscard]] const BufferSlot* buffer(BufferHandle handle, std::string* error) const;
    [[nodiscard]] TextureSlot* texture(TextureHandle handle, std::string* error);
    [[nodiscard]] const TextureSlot* texture(TextureHandle handle, std::string* error) const;
    [[nodiscard]] TextureViewSlot* texture_view(TextureViewHandle handle, std::string* error);
    [[nodiscard]] SamplerSlot* sampler(SamplerHandle handle, std::string* error);
    [[nodiscard]] const SamplerSlot* sampler(SamplerHandle handle, std::string* error) const;
    [[nodiscard]] BindGroupLayoutSlot* bind_group_layout(BindGroupLayoutHandle handle, std::string* error);
    [[nodiscard]] const BindGroupLayoutSlot* bind_group_layout(BindGroupLayoutHandle handle, std::string* error) const;
    [[nodiscard]] BindGroupSlot* bind_group(BindGroupHandle handle, std::string* error);
    [[nodiscard]] PipelineSlot* pipeline(ComputePipelineHandle handle, std::string* error);
    [[nodiscard]] GraphicsPipelineSlot* graphics_pipeline(GraphicsPipelineHandle handle,
                                                           std::string* error);
    [[nodiscard]] SwapchainSlot* swapchain(SwapchainHandle handle, std::string* error);
    [[nodiscard]] TimestampPoolSlot* timestamp_pool(TimestampQueryPoolHandle handle, std::string* error);
    [[nodiscard]] const TimestampPoolSlot* timestamp_pool(TimestampQueryPoolHandle handle, std::string* error) const;
    [[nodiscard]] CommandSlot* command_list(CommandListHandle handle, std::string* error);
    bool destroy_texture_internal(TextureHandle handle, bool fromSwapchain, std::string* error);
    static void set_error(std::string* error, std::string_view message);

    DeviceCapabilities capabilities_{};
    DeviceStatistics statistics_{};
    DeviceStatus status_{DeviceStatus::Ready};
    std::string lossReason_;
    std::vector<BufferSlot> buffers_;
    std::vector<TextureSlot> textures_;
    std::vector<TextureViewSlot> textureViews_;
    std::vector<SamplerSlot> samplers_;
    std::vector<BindGroupLayoutSlot> bindGroupLayouts_;
    std::vector<BindGroupSlot> bindGroups_;
    std::vector<PipelineSlot> pipelines_;
    std::vector<GraphicsPipelineSlot> graphicsPipelines_;
    std::vector<SwapchainSlot> swapchains_;
    std::vector<TimestampPoolSlot> timestampPools_;
    std::vector<CommandSlot> commandLists_;
    std::uint64_t nextFence_{1};
    std::uint64_t completedFence_{};
    std::uint64_t syntheticTimestamp_{1000};
};

} // namespace dve::rhi
