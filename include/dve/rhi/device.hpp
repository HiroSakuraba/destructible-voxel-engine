#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dve/platform/application_host.hpp"

namespace dve::rhi {

enum class Backend : std::uint8_t { Null, Direct3D12, Vulkan, Metal };
enum class QueueKind : std::uint8_t { Graphics, Compute, Copy };
enum class MemoryDomain : std::uint8_t { DeviceLocal, Upload, Readback };
enum class ResourceState : std::uint8_t {
    Undefined,
    CopySource,
    CopyDestination,
    ShaderRead,
    ShaderWrite,
    RenderTarget,
    DepthWrite,
    Present,
};

enum class BufferUsage : std::uint32_t {
    NoUsage = 0,
    CopySource = 1U << 0U,
    CopyDestination = 1U << 1U,
    Vertex = 1U << 2U,
    Index = 1U << 3U,
    Constant = 1U << 4U,
    Storage = 1U << 5U,
    Indirect = 1U << 6U,
};
[[nodiscard]] constexpr BufferUsage operator|(BufferUsage left, BufferUsage right) noexcept {
    return static_cast<BufferUsage>(static_cast<std::uint32_t>(left) |
                                    static_cast<std::uint32_t>(right));
}
[[nodiscard]] constexpr BufferUsage operator&(BufferUsage left, BufferUsage right) noexcept {
    return static_cast<BufferUsage>(static_cast<std::uint32_t>(left) &
                                    static_cast<std::uint32_t>(right));
}
[[nodiscard]] constexpr bool has_usage(BufferUsage value, BufferUsage flag) noexcept {
    return (value & flag) != BufferUsage::NoUsage;
}

enum class TextureUsage : std::uint32_t {
    NoUsage = 0,
    CopySource = 1U << 0U,
    CopyDestination = 1U << 1U,
    Sampled = 1U << 2U,
    Storage = 1U << 3U,
    RenderTarget = 1U << 4U,
    DepthStencil = 1U << 5U,
    Present = 1U << 6U,
};
[[nodiscard]] constexpr TextureUsage operator|(TextureUsage left, TextureUsage right) noexcept {
    return static_cast<TextureUsage>(static_cast<std::uint32_t>(left) |
                                     static_cast<std::uint32_t>(right));
}
[[nodiscard]] constexpr TextureUsage operator&(TextureUsage left, TextureUsage right) noexcept {
    return static_cast<TextureUsage>(static_cast<std::uint32_t>(left) &
                                     static_cast<std::uint32_t>(right));
}
[[nodiscard]] constexpr bool has_usage(TextureUsage value, TextureUsage flag) noexcept {
    return (value & flag) != TextureUsage::NoUsage;
}

enum class TextureDimension : std::uint8_t { Texture2D, Texture3D, TextureCube };
enum class TextureViewDimension : std::uint8_t { Automatic, Texture2D, Texture2DArray, Texture3D, TextureCube };
enum class FilterMode : std::uint8_t { Nearest, Linear };
enum class MipmapFilterMode : std::uint8_t { Nearest, Linear };
enum class AddressMode : std::uint8_t { Repeat, MirroredRepeat, ClampToEdge };
enum class TextureFormat : std::uint8_t {
    RGBA8Unorm,
    BGRA8Unorm,
    R32Uint,
    R32Sint,
    RGBA32Sint,
    // Slug curve atlas: four IEEE-754 binary16 components.
    RGBA16Float,
    // Slug band atlas: two unsigned 16-bit components.
    RG16Uint,
    D32Float
};
enum class IndexFormat : std::uint8_t { Uint16, Uint32 };
enum class PrimitiveTopology : std::uint8_t { TriangleList };
enum class VertexFormat : std::uint8_t { Float2, Float3, Float4 };
enum class CullMode : std::uint8_t { Disabled, FrontFaces, BackFaces };
enum class FrontFace : std::uint8_t { CounterClockwise, Clockwise };
enum class CompareOp : std::uint8_t { NeverPass, Less, LessEqual, Equal, GreaterEqual, Greater, AlwaysPass };
enum class BlendMode : std::uint8_t { Opaque, Alpha, Additive, Multiply };
enum class PresentMode : std::uint8_t { Immediate, Mailbox, Fifo };
enum class DeviceStatus : std::uint8_t { Ready, Lost };
enum class PresentResult : std::uint8_t { Presented, Suboptimal, OutOfDate, DeviceLost, Error };

enum class ShaderStage : std::uint8_t { NoStage = 0, Compute = 1U << 0U, Vertex = 1U << 1U, Fragment = 1U << 2U };
[[nodiscard]] constexpr ShaderStage operator|(ShaderStage left, ShaderStage right) noexcept {
    return static_cast<ShaderStage>(static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right));
}
[[nodiscard]] constexpr ShaderStage operator&(ShaderStage left, ShaderStage right) noexcept {
    return static_cast<ShaderStage>(static_cast<std::uint8_t>(left) & static_cast<std::uint8_t>(right));
}
[[nodiscard]] constexpr bool has_stage(ShaderStage value, ShaderStage stage) noexcept {
    return (value & stage) != ShaderStage::NoStage;
}
enum class BindingType : std::uint8_t {
    UniformBuffer, StorageBufferReadOnly, StorageBufferReadWrite, SampledTexture, StorageTexture
};

template <class Tag>
struct Handle {
    std::uint32_t index{0xFFFFFFFFU};
    std::uint32_t generation{};
    [[nodiscard]] explicit operator bool() const noexcept { return index != 0xFFFFFFFFU; }
    friend bool operator==(const Handle&, const Handle&) = default;
};
struct BufferTag;
struct TextureTag;
struct TextureViewTag;
struct SamplerTag;
struct ComputePipelineTag;
struct GraphicsPipelineTag;
struct CommandListTag;
struct SwapchainTag;
struct TimestampQueryPoolTag;
struct BindGroupLayoutTag;
struct BindGroupTag;
using BufferHandle = Handle<BufferTag>;
using TextureHandle = Handle<TextureTag>;
using TextureViewHandle = Handle<TextureViewTag>;
using SamplerHandle = Handle<SamplerTag>;
using ComputePipelineHandle = Handle<ComputePipelineTag>;
using GraphicsPipelineHandle = Handle<GraphicsPipelineTag>;
using CommandListHandle = Handle<CommandListTag>;
using SwapchainHandle = Handle<SwapchainTag>;
using TimestampQueryPoolHandle = Handle<TimestampQueryPoolTag>;
using BindGroupLayoutHandle = Handle<BindGroupLayoutTag>;
using BindGroupHandle = Handle<BindGroupTag>;

struct FenceHandle {
    std::uint64_t value{};
    [[nodiscard]] explicit operator bool() const noexcept { return value != 0U; }
    friend bool operator==(const FenceHandle&, const FenceHandle&) = default;
};

enum class AdapterClass : std::uint8_t {
    Unknown,
    Integrated,
    Discrete,
    Virtual,
    Cpu,
};

struct DeviceCapabilities {
    Backend backend{Backend::Null};
    std::string adapterName{"Null validation device"};
    AdapterClass adapterClass{AdapterClass::Unknown};
    std::uint32_t vendorId{};
    std::uint32_t deviceId{};
    std::uint32_t driverVersion{};
    std::uint32_t apiVersion{};
    std::uint32_t queueFamilyIndex{};
    std::uint64_t dedicatedVideoMemoryBytes{};
    bool softwareAdapter{};
    bool nativeLimitsQueried{};
    std::uint32_t maxComputeWorkgroupSizeX{1024};
    std::uint32_t maxComputeWorkgroupSizeY{1024};
    std::uint32_t maxComputeWorkgroupSizeZ{64};
    std::uint32_t maxComputeInvocations{1024};
    std::uint32_t minStorageBufferOffsetAlignment{16};
    std::uint32_t minUniformBufferOffsetAlignment{256};
    std::uint32_t maxTextureDimension2D{16384};
    bool timestampQueries{true};
    std::uint32_t timestampValidBits{64U};
    double timestampPeriodNanoseconds{1.0};
    bool waveOperations{};
    bool indirectDispatch{true};
    bool presentation{true};
};

struct TextureFormatCapabilities {
    bool sampled{};
    bool storage{};
    bool storageAtomic{};
    bool renderTarget{};
    bool depthStencil{};
};

struct BufferDesc {
    std::size_t bytes{};
    BufferUsage usage{BufferUsage::NoUsage};
    MemoryDomain memory{MemoryDomain::DeviceLocal};
    std::string debugName;
    ResourceState initialState{ResourceState::Undefined};
};

struct TextureDesc {
    TextureDimension dimension{TextureDimension::Texture2D};
    TextureFormat format{TextureFormat::RGBA8Unorm};
    std::uint32_t width{1};
    std::uint32_t height{1};
    std::uint32_t depth{1};
    std::uint32_t mipLevels{1};
    std::uint32_t arrayLayers{1};
    TextureUsage usage{TextureUsage::Sampled};
    ResourceState initialState{ResourceState::Undefined};
    std::string debugName;
};

struct TextureViewDesc {
    TextureHandle texture;
    std::uint32_t baseMip{};
    std::uint32_t mipCount{1};
    std::uint32_t baseLayer{};
    std::uint32_t layerCount{1};
    std::string debugName;
    TextureViewDimension dimension{TextureViewDimension::Automatic};
};

struct SamplerDesc {
    FilterMode minFilter{FilterMode::Linear};
    FilterMode magFilter{FilterMode::Linear};
    MipmapFilterMode mipmapFilter{MipmapFilterMode::Linear};
    AddressMode addressU{AddressMode::Repeat};
    AddressMode addressV{AddressMode::Repeat};
    AddressMode addressW{AddressMode::Repeat};
    float minimumLod{};
    float maximumLod{1000.0F};
    float mipLodBias{};
    bool anisotropy{};
    float maximumAnisotropy{1.0F};
    bool comparison{};
    CompareOp comparisonOp{CompareOp::LessEqual};
    std::string debugName;
};


struct Viewport {
    float x{};
    float y{};
    float width{1.0F};
    float height{1.0F};
    float minimumDepth{};
    float maximumDepth{1.0F};
};

struct ScissorRect {
    std::int32_t x{};
    std::int32_t y{};
    std::uint32_t width{1};
    std::uint32_t height{1};
};

struct RenderPassColorAttachment {
    TextureHandle texture;
    bool clear{true};
    float clearR{};
    float clearG{};
    float clearB{};
    float clearA{1.0F};
};

struct RenderPassDepthAttachment {
    TextureHandle texture;
    bool clear{true};
    float clearDepth{1.0F};
};

struct RenderPassDesc {
    std::vector<RenderPassColorAttachment> colors;
    std::optional<RenderPassDepthAttachment> depth;
    std::string debugName;
};

struct SwapchainDesc {
    platform::NativeWindowHandle window;
    TextureFormat format{TextureFormat::BGRA8Unorm};
    std::uint32_t width{1280};
    std::uint32_t height{800};
    std::uint32_t imageCount{3};
    PresentMode presentMode{PresentMode::Fifo};
    std::string debugName;
};

struct AcquiredSwapchainImage {
    TextureHandle texture;
    std::uint32_t imageIndex{};
    bool suboptimal{};
};

struct BindGroupLayoutBinding {
    std::uint32_t binding{};
    BindingType type{BindingType::StorageBufferReadOnly};
    ShaderStage visibility{ShaderStage::Compute};
};

struct BindGroupLayoutDesc {
    std::vector<BindGroupLayoutBinding> bindings;
    std::string debugName;
};

struct BindGroupEntry {
    std::uint32_t binding{};
    BufferHandle buffer;
    TextureViewHandle textureView;
    std::size_t offset{};
    std::size_t bytes{};
    SamplerHandle sampler;
};

struct BindGroupDesc {
    BindGroupLayoutHandle layout;
    std::vector<BindGroupEntry> entries;
    std::string debugName;
};

struct ComputePipelineDesc {
    std::string debugName;
    std::string entryPoint{"main"};
    std::vector<std::byte> bytecode;
    std::vector<BindGroupLayoutHandle> bindGroupLayouts;
    std::uint32_t threadsX{1};
    std::uint32_t threadsY{1};
    std::uint32_t threadsZ{1};
};

struct VertexBufferLayoutDesc {
    std::uint32_t stride{};
    bool perInstance{};
};

struct VertexAttributeDesc {
    std::uint32_t location{};
    VertexFormat format{VertexFormat::Float2};
    std::uint32_t offset{};
};

struct GraphicsPipelineDesc {
    std::string debugName;
    std::string vertexEntryPoint{"main"};
    std::string fragmentEntryPoint{"main"};
    std::vector<std::byte> vertexBytecode;
    std::vector<std::byte> fragmentBytecode;
    std::vector<BindGroupLayoutHandle> bindGroupLayouts;
    std::optional<VertexBufferLayoutDesc> vertexBuffer;
    std::vector<VertexAttributeDesc> vertexAttributes;
    PrimitiveTopology topology{PrimitiveTopology::TriangleList};
    std::optional<TextureFormat> colorFormat{TextureFormat::RGBA8Unorm};
    std::optional<TextureFormat> depthFormat{TextureFormat::D32Float};
    CullMode cullMode{CullMode::BackFaces};
    FrontFace frontFace{FrontFace::CounterClockwise};
    bool depthTest{true};
    bool depthWrite{true};
    CompareOp depthCompare{CompareOp::LessEqual};
    BlendMode blend{BlendMode::Opaque};
    std::uint32_t sampleCount{1};
};

[[nodiscard]] std::uint32_t vertex_format_bytes(VertexFormat format) noexcept;
[[nodiscard]] bool validate_vertex_input_layout(
    const GraphicsPipelineDesc& desc, std::string* error = nullptr);

struct DeviceStatistics {
    std::uint64_t buffersCreated{};
    std::uint64_t buffersDestroyed{};
    std::uint64_t texturesCreated{};
    std::uint64_t texturesDestroyed{};
    std::uint64_t textureViewsCreated{};
    std::uint64_t textureViewsDestroyed{};
    std::uint64_t samplersCreated{};
    std::uint64_t samplersDestroyed{};
    std::uint64_t swapchainsCreated{};
    std::uint64_t bindGroupLayoutsCreated{};
    std::uint64_t bindGroupLayoutsDestroyed{};
    std::uint64_t bindGroupsCreated{};
    std::uint64_t bindGroupsDestroyed{};
    std::uint64_t swapchainsDestroyed{};
    std::uint64_t presents{};
    std::uint64_t commandListsSubmitted{};
    std::uint64_t copiesExecuted{};
    std::uint64_t dispatchesExecuted{};
    std::uint64_t renderPassesExecuted{};
    std::uint64_t indexedDrawsExecuted{};
    std::uint64_t indexedTrianglesSubmitted{};
    std::uint64_t barriersExecuted{};
    std::uint64_t uploadedBytes{};
    std::uint64_t readbackBytes{};
};

class IDevice {
public:
    virtual ~IDevice() = default;

    [[nodiscard]] virtual const DeviceCapabilities& capabilities() const noexcept = 0;
    [[nodiscard]] virtual TextureFormatCapabilities texture_format_capabilities(
        TextureFormat format) const noexcept;
    [[nodiscard]] virtual DeviceStatistics statistics() const noexcept = 0;
    [[nodiscard]] virtual DeviceStatus status() const noexcept = 0;
    [[nodiscard]] virtual std::string_view device_loss_reason() const noexcept = 0;

    [[nodiscard]] virtual BufferHandle create_buffer(const BufferDesc& desc,
                                                     std::string* error = nullptr) = 0;
    virtual bool destroy_buffer(BufferHandle handle, std::string* error = nullptr) = 0;
    virtual bool write_buffer(BufferHandle handle, std::size_t offset,
                              std::span<const std::byte> bytes,
                              std::string* error = nullptr) = 0;
    virtual bool read_buffer(BufferHandle handle, std::size_t offset,
                             std::span<std::byte> destination,
                             std::string* error = nullptr) = 0;
    virtual bool write_texture(TextureHandle handle, std::uint32_t mipLevel,
                               std::uint32_t arrayLayer, std::span<const std::byte> bytes,
                               std::size_t rowPitchBytes, std::string* error = nullptr);
    virtual bool read_texture(TextureHandle handle, std::uint32_t mipLevel,
                              std::uint32_t arrayLayer, std::span<std::byte> destination,
                              std::size_t rowPitchBytes, std::string* error = nullptr);

    [[nodiscard]] virtual TextureHandle create_texture(const TextureDesc& desc,
                                                       std::string* error = nullptr) = 0;
    virtual bool destroy_texture(TextureHandle handle, std::string* error = nullptr) = 0;
    [[nodiscard]] virtual TextureViewHandle create_texture_view(
        const TextureViewDesc& desc, std::string* error = nullptr) = 0;
    virtual bool destroy_texture_view(TextureViewHandle handle,
                                      std::string* error = nullptr) = 0;
    [[nodiscard]] virtual SamplerHandle create_sampler(
        const SamplerDesc& desc, std::string* error = nullptr) = 0;
    virtual bool destroy_sampler(SamplerHandle handle,
                                 std::string* error = nullptr) = 0;

    [[nodiscard]] virtual SwapchainHandle create_swapchain(const SwapchainDesc& desc,
                                                           std::string* error = nullptr) = 0;
    virtual bool destroy_swapchain(SwapchainHandle handle, std::string* error = nullptr) = 0;
    [[nodiscard]] virtual AcquiredSwapchainImage acquire_next_image(
        SwapchainHandle handle, std::string* error = nullptr) = 0;
    [[nodiscard]] virtual PresentResult present(SwapchainHandle handle, FenceHandle waitFence,
                                                std::string* error = nullptr) = 0;

    [[nodiscard]] virtual BindGroupLayoutHandle create_bind_group_layout(
        const BindGroupLayoutDesc& desc, std::string* error = nullptr) = 0;
    virtual bool destroy_bind_group_layout(BindGroupLayoutHandle handle,
                                           std::string* error = nullptr) = 0;
    [[nodiscard]] virtual BindGroupHandle create_bind_group(
        const BindGroupDesc& desc, std::string* error = nullptr) = 0;
    virtual bool destroy_bind_group(BindGroupHandle handle,
                                    std::string* error = nullptr) = 0;

    [[nodiscard]] virtual ComputePipelineHandle create_compute_pipeline(
        const ComputePipelineDesc& desc, std::string* error = nullptr) = 0;
    virtual bool destroy_compute_pipeline(ComputePipelineHandle handle,
                                          std::string* error = nullptr) = 0;
    [[nodiscard]] virtual GraphicsPipelineHandle create_graphics_pipeline(
        const GraphicsPipelineDesc& desc, std::string* error = nullptr);
    virtual bool destroy_graphics_pipeline(GraphicsPipelineHandle handle,
                                           std::string* error = nullptr);

    [[nodiscard]] virtual CommandListHandle begin_commands(QueueKind queue,
                                                           std::string_view debugName,
                                                           std::string* error = nullptr) = 0;
    virtual bool copy_buffer(CommandListHandle commands, BufferHandle source,
                             std::size_t sourceOffset, BufferHandle destination,
                             std::size_t destinationOffset, std::size_t bytes,
                             std::string* error = nullptr) = 0;
    virtual bool transition_buffer(CommandListHandle commands, BufferHandle buffer,
                                   ResourceState before, ResourceState after,
                                   std::string* error = nullptr) = 0;
    virtual bool transition_texture(CommandListHandle commands, TextureHandle texture,
                                    ResourceState before, ResourceState after,
                                    std::string* error = nullptr) = 0;
    virtual bool dispatch(CommandListHandle commands, ComputePipelineHandle pipeline,
                          std::uint32_t groupsX, std::uint32_t groupsY,
                          std::uint32_t groupsZ, std::string* error = nullptr) = 0;
    virtual bool begin_render_pass(CommandListHandle commands, const RenderPassDesc& desc,
                                   std::string* error = nullptr);
    virtual bool end_render_pass(CommandListHandle commands, std::string* error = nullptr);
    virtual bool bind_graphics_pipeline(CommandListHandle commands,
                                        GraphicsPipelineHandle pipeline,
                                        std::string* error = nullptr);
    virtual bool set_viewport(CommandListHandle commands, const Viewport& viewport,
                              std::string* error = nullptr);
    virtual bool set_scissor(CommandListHandle commands, const ScissorRect& scissor,
                             std::string* error = nullptr);
    // Clears a rectangular portion of the active depth attachment. The rectangle is
    // expressed in framebuffer pixels and is clipped only by validation, not by the current scissor.
    virtual bool clear_depth_region(CommandListHandle commands, float depth,
                                    const ScissorRect& region,
                                    std::string* error = nullptr);
    virtual bool bind_vertex_buffer(CommandListHandle commands, std::uint32_t slot,
                                    BufferHandle buffer, std::size_t offset,
                                    std::uint32_t stride, std::string* error = nullptr);
    virtual bool bind_index_buffer(CommandListHandle commands, BufferHandle buffer,
                                   std::size_t offset, IndexFormat format,
                                   std::string* error = nullptr);
    virtual bool bind_compute_bind_group(CommandListHandle commands, std::uint32_t index,
                                         BindGroupHandle group,
                                         std::string* error = nullptr);
    virtual bool bind_graphics_bind_group(CommandListHandle commands, std::uint32_t index,
                                          BindGroupHandle group,
                                          std::string* error = nullptr);
    virtual bool draw_indexed(CommandListHandle commands, std::uint32_t indexCount,
                              std::uint32_t instanceCount, std::uint32_t firstIndex,
                              std::int32_t vertexOffset, std::uint32_t firstInstance,
                              std::string* error = nullptr);
    virtual bool begin_debug_label(CommandListHandle commands, std::string_view label,
                                   std::string* error = nullptr) = 0;
    virtual bool end_debug_label(CommandListHandle commands, std::string* error = nullptr) = 0;

    [[nodiscard]] virtual TimestampQueryPoolHandle create_timestamp_query_pool(
        std::uint32_t count, std::string_view debugName, std::string* error = nullptr) = 0;
    virtual bool destroy_timestamp_query_pool(TimestampQueryPoolHandle handle,
                                              std::string* error = nullptr) = 0;
    virtual bool write_timestamp(CommandListHandle commands, TimestampQueryPoolHandle pool,
                                 std::uint32_t index, std::string* error = nullptr) = 0;
    virtual bool resolve_timestamps(TimestampQueryPoolHandle pool, std::uint32_t first,
                                    std::span<std::uint64_t> destination,
                                    std::string* error = nullptr) = 0;

    [[nodiscard]] virtual FenceHandle submit(CommandListHandle commands,
                                             std::string* error = nullptr) = 0;
    [[nodiscard]] virtual bool fence_complete(FenceHandle fence) const noexcept = 0;
    virtual bool wait(FenceHandle fence, std::string* error = nullptr) = 0;
    virtual void wait_idle() noexcept = 0;
};

[[nodiscard]] std::string_view backend_name(Backend backend) noexcept;
[[nodiscard]] std::string_view resource_state_name(ResourceState state) noexcept;

} // namespace dve::rhi
