#include "dve/rhi/vulkan_device.hpp"

#include "vulkan_loader_api.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace dve::rhi {
namespace {

void set_error(std::string* error, std::string_view message) {
    if (error != nullptr) error->assign(message.begin(), message.end());
}

std::string result_message(vk::Result result) {
    switch (result) {
    case vk::Success: return "success";
    case vk::NotReady: return "not ready";
    case vk::Timeout: return "timeout";
    case vk::ErrorOutOfHostMemory: return "out of host memory";
    case vk::ErrorOutOfDeviceMemory: return "out of device memory";
    case vk::ErrorInitializationFailed: return "initialization failed";
    case vk::ErrorDeviceLost: return "device lost";
    case vk::ErrorIncompatibleDriver: return "incompatible driver";
    default: return "Vulkan result " + std::to_string(result);
    }
}

template <class T>
T function_pointer(void* address) noexcept {
    static_assert(std::is_pointer_v<T>);
    T result{};
    static_assert(sizeof(result) == sizeof(address));
    std::memcpy(&result, &address, sizeof(result));
    return result;
}

template <class T>
T function_pointer(vk::VoidFunction address) noexcept {
    return reinterpret_cast<T>(address);
}

void* open_vulkan_loader() noexcept {
#if defined(_WIN32)
    return reinterpret_cast<void*>(LoadLibraryA("vulkan-1.dll"));
#elif defined(__APPLE__)
    if (void* loader = dlopen("libvulkan.1.dylib", RTLD_NOW | RTLD_LOCAL)) return loader;
    if (void* loader = dlopen("libMoltenVK.dylib", RTLD_NOW | RTLD_LOCAL)) return loader;
    return nullptr;
#else
    return dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
}

void close_vulkan_loader(void* loader) noexcept {
    if (!loader) return;
#if defined(_WIN32)
    FreeLibrary(reinterpret_cast<HMODULE>(loader));
#else
    dlclose(loader);
#endif
}

void* loader_symbol(void* loader, const char* name) noexcept {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(loader), name));
#else
    return dlsym(loader, name);
#endif
}

bool range_fits(std::size_t offset, std::size_t bytes, std::size_t capacity) noexcept {
    return offset <= capacity && bytes <= capacity - offset;
}

template <class Slot>
std::uint32_t allocate_slot(std::vector<Slot>& slots) {
    for (std::uint32_t index = 0; index < slots.size(); ++index)
        if (!slots[index].alive) return index;
    slots.emplace_back();
    return static_cast<std::uint32_t>(slots.size() - 1U);
}

template <class Slot>
void retire_slot(Slot& slot) noexcept {
    slot.alive = false;
    slot.generation = slot.generation == std::numeric_limits<std::uint32_t>::max()
                          ? 1U : slot.generation + 1U;
}

vk::BufferUsageFlags vulkan_buffer_usage(BufferUsage usage) noexcept {
    vk::BufferUsageFlags result = vk::BufferUsageTransferSourceBit |
                                  vk::BufferUsageTransferDestinationBit;
    if (has_usage(usage, BufferUsage::Storage)) result |= vk::BufferUsageStorageBufferBit;
    if (has_usage(usage, BufferUsage::Constant)) result |= vk::BufferUsageUniformBufferBit;
    if (has_usage(usage, BufferUsage::Vertex)) result |= vk::BufferUsageVertexBufferBit;
    if (has_usage(usage, BufferUsage::Index)) result |= vk::BufferUsageIndexBufferBit;
    if (has_usage(usage, BufferUsage::Indirect)) result |= vk::BufferUsageIndirectBufferBit;
    return result;
}

vk::Format vulkan_format(TextureFormat format) noexcept {
    switch (format) {
    case TextureFormat::RGBA8Unorm: return vk::FormatR8G8B8A8Unorm;
    case TextureFormat::BGRA8Unorm: return vk::FormatB8G8R8A8Unorm;
    case TextureFormat::R32Uint: return vk::FormatR32Uint;
    case TextureFormat::R32Sint: return vk::FormatR32Sint;
    case TextureFormat::RGBA32Sint: return vk::FormatR32G32B32A32Sint;
    case TextureFormat::RGBA16Float: return vk::FormatR16G16B16A16Sfloat;
    case TextureFormat::RG16Uint: return vk::FormatR16G16Uint;
    case TextureFormat::D32Float: return vk::FormatD32Sfloat;
    }
    return vk::FormatR8G8B8A8Unorm;
}

vk::Format vulkan_vertex_format(VertexFormat format) noexcept {
    switch (format) {
        case VertexFormat::Float2: return vk::FormatR32G32Sfloat;
        case VertexFormat::Float3: return vk::FormatR32G32B32Sfloat;
        case VertexFormat::Float4: return vk::FormatR32G32B32A32Sfloat;
    }
    return vk::FormatR32G32Sfloat;
}

vk::ImageUsageFlags vulkan_image_usage(TextureUsage usage) noexcept {
    vk::ImageUsageFlags result{};
    if (has_usage(usage, TextureUsage::CopySource)) result |= vk::ImageUsageTransferSourceBit;
    if (has_usage(usage, TextureUsage::CopyDestination)) result |= vk::ImageUsageTransferDestinationBit;
    if (has_usage(usage, TextureUsage::Sampled)) result |= vk::ImageUsageSampledBit;
    if (has_usage(usage, TextureUsage::Storage)) result |= vk::ImageUsageStorageBit;
    if (has_usage(usage, TextureUsage::RenderTarget) || has_usage(usage, TextureUsage::Present))
        result |= vk::ImageUsageColorAttachmentBit;
    if (has_usage(usage, TextureUsage::DepthStencil)) result |= vk::ImageUsageDepthStencilAttachmentBit;
    return result;
}

vk::ImageLayout vulkan_image_layout(ResourceState state) noexcept {
    switch (state) {
    case ResourceState::Undefined: return vk::ImageLayoutUndefined;
    case ResourceState::CopySource: return vk::ImageLayoutTransferSourceOptimal;
    case ResourceState::CopyDestination: return vk::ImageLayoutTransferDestinationOptimal;
    case ResourceState::ShaderRead: return vk::ImageLayoutShaderReadOnlyOptimal;
    case ResourceState::ShaderWrite: return vk::ImageLayoutGeneral;
    case ResourceState::RenderTarget: return vk::ImageLayoutColorAttachmentOptimal;
    case ResourceState::DepthWrite: return vk::ImageLayoutDepthStencilAttachmentOptimal;
    case ResourceState::Present: return vk::ImageLayoutPresentSource;
    }
    return vk::ImageLayoutGeneral;
}

vk::ImageAspectFlags image_aspect(TextureFormat format) noexcept {
    return format == TextureFormat::D32Float ? vk::ImageAspectDepthBit : vk::ImageAspectColorBit;
}

std::size_t texture_pixel_bytes(TextureFormat format) noexcept {
    switch (format) {
    case TextureFormat::RGBA32Sint: return 16U;
    case TextureFormat::RGBA16Float: return 8U;
    case TextureFormat::RG16Uint: return 4U;
    case TextureFormat::RGBA8Unorm:
    case TextureFormat::BGRA8Unorm:
    case TextureFormat::R32Uint:
    case TextureFormat::R32Sint:
    case TextureFormat::D32Float:
        return 4U;
    }
    return 4U;
}
std::uint32_t mip_dimension(std::uint32_t value, std::uint32_t mip) noexcept {
    return std::max(1U, value >> mip);
}

struct StageAccess {
    vk::PipelineStageFlags stage{vk::PipelineStageAllCommandsBit};
    vk::AccessFlags access{vk::AccessMemoryReadBit | vk::AccessMemoryWriteBit};
};

StageAccess stage_access(ResourceState state) noexcept {
    switch (state) {
    case ResourceState::Undefined: return {vk::PipelineStageTopOfPipeBit, 0U};
    case ResourceState::CopySource: return {vk::PipelineStageTransferBit, vk::AccessTransferReadBit};
    case ResourceState::CopyDestination: return {vk::PipelineStageTransferBit, vk::AccessTransferWriteBit};
    case ResourceState::ShaderRead: return {vk::PipelineStageVertexShaderBit | vk::PipelineStageFragmentShaderBit | vk::PipelineStageComputeShaderBit, vk::AccessShaderReadBit};
    case ResourceState::ShaderWrite: return {vk::PipelineStageComputeShaderBit, vk::AccessShaderWriteBit};
    case ResourceState::RenderTarget: return {vk::PipelineStageColorAttachmentOutputBit, vk::AccessColorAttachmentWriteBit};
    case ResourceState::DepthWrite: return {vk::PipelineStageEarlyFragmentTestsBit | vk::PipelineStageLateFragmentTestsBit, vk::AccessDepthStencilAttachmentReadBit | vk::AccessDepthStencilAttachmentWriteBit};
    case ResourceState::Present: return {vk::PipelineStageBottomOfPipeBit, vk::AccessMemoryReadBit};
    }
    return {};
}

vk::CullModeFlags vulkan_cull_mode(CullMode mode) noexcept {
    switch (mode) {
    case CullMode::Disabled: return vk::CullModeNone;
    case CullMode::FrontFaces: return vk::CullModeFrontBit;
    case CullMode::BackFaces: return vk::CullModeBackBit;
    }
    return vk::CullModeBackBit;
}

vk::FrontFace vulkan_front_face(FrontFace face) noexcept {
    return face == FrontFace::Clockwise ? vk::FrontFaceClockwise : vk::FrontFaceCounterClockwise;
}

vk::CompareOp vulkan_compare(CompareOp compare) noexcept {
    switch (compare) {
    case CompareOp::NeverPass: return vk::CompareNever;
    case CompareOp::Less: return vk::CompareLess;
    case CompareOp::LessEqual: return vk::CompareLessOrEqual;
    case CompareOp::Equal: return vk::CompareEqual;
    case CompareOp::GreaterEqual: return vk::CompareGreaterOrEqual;
    case CompareOp::Greater: return vk::CompareGreater;
    case CompareOp::AlwaysPass: return vk::CompareAlways;
    }
    return vk::CompareLessOrEqual;
}

vk::IndexType vulkan_index_type(IndexFormat format) noexcept {
    return format == IndexFormat::Uint16 ? vk::IndexTypeUint16 : vk::IndexTypeUint32;
}

vk::Filter vulkan_filter(FilterMode filter) noexcept {
    return filter == FilterMode::Nearest ? vk::FilterNearest : vk::FilterLinear;
}

vk::SamplerMipmapMode vulkan_mipmap_filter(MipmapFilterMode filter) noexcept {
    return filter == MipmapFilterMode::Nearest
        ? vk::SamplerMipmapModeNearest : vk::SamplerMipmapModeLinear;
}

vk::SamplerAddressMode vulkan_address_mode(AddressMode mode) noexcept {
    switch (mode) {
    case AddressMode::Repeat: return vk::SamplerAddressModeRepeat;
    case AddressMode::MirroredRepeat: return vk::SamplerAddressModeMirroredRepeat;
    case AddressMode::ClampToEdge: return vk::SamplerAddressModeClampToEdge;
    }
    return vk::SamplerAddressModeRepeat;
}

vk::ImageViewType vulkan_view_type(const TextureDesc& texture,
                                   TextureViewDimension dimension) noexcept {
    if (dimension == TextureViewDimension::Automatic) {
        if (texture.dimension == TextureDimension::Texture3D) return vk::ImageViewType3D;
        if (texture.dimension == TextureDimension::TextureCube) return vk::ImageViewTypeCube;
        return texture.arrayLayers > 1U ? vk::ImageViewType2DArray : vk::ImageViewType2D;
    }
    switch (dimension) {
    case TextureViewDimension::Texture2D: return vk::ImageViewType2D;
    case TextureViewDimension::Texture2DArray: return vk::ImageViewType2DArray;
    case TextureViewDimension::Texture3D: return vk::ImageViewType3D;
    case TextureViewDimension::TextureCube: return vk::ImageViewTypeCube;
    case TextureViewDimension::Automatic: break;
    }
    return vk::ImageViewType2D;
}


vk::ShaderStageFlags vulkan_shader_stages(ShaderStage stages) noexcept {
    vk::ShaderStageFlags result{};
    if (has_stage(stages, ShaderStage::Compute)) result |= vk::ShaderStageComputeBit;
    if (has_stage(stages, ShaderStage::Vertex)) result |= vk::ShaderStageVertexBit;
    if (has_stage(stages, ShaderStage::Fragment)) result |= vk::ShaderStageFragmentBit;
    return result;
}

vk::DescriptorType vulkan_descriptor_type(BindingType type) noexcept {
    switch (type) {
    case BindingType::UniformBuffer: return vk::DescriptorTypeUniformBuffer;
    case BindingType::StorageBufferReadOnly:
    case BindingType::StorageBufferReadWrite: return vk::DescriptorTypeStorageBuffer;
    case BindingType::SampledTexture: return vk::DescriptorTypeCombinedImageSampler;
    case BindingType::StorageTexture: return vk::DescriptorTypeStorageImage;
    }
    return vk::DescriptorTypeStorageBuffer;
}

bool valid_spirv(std::span<const std::byte> bytecode) noexcept {
    if (bytecode.size() < 20U || bytecode.size() % sizeof(std::uint32_t) != 0U) return false;
    std::uint32_t magic{};
    std::memcpy(&magic, bytecode.data(), sizeof(magic));
    return magic == 0x07230203U;
}

} // namespace

struct VulkanDevice::Impl {
    struct Functions {
        vk::GetInstanceProcAddr getInstanceProcAddr{};
        vk::GetDeviceProcAddr getDeviceProcAddr{};
        vk::PFN_CreateInstance createInstance{};
        vk::PFN_DestroyInstance destroyInstance{};
        vk::PFN_EnumeratePhysicalDevices enumeratePhysicalDevices{};
        vk::PFN_GetPhysicalDeviceProperties getPhysicalDeviceProperties{};
        vk::PFN_GetPhysicalDeviceQueueFamilyProperties getQueueFamilyProperties{};
        vk::PFN_GetPhysicalDeviceMemoryProperties getMemoryProperties{};
        vk::PFN_GetPhysicalDeviceFormatProperties getFormatProperties{};
        vk::PFN_CreateDevice createDevice{};
        vk::PFN_DestroyDevice destroyDevice{};
        vk::PFN_GetDeviceQueue getDeviceQueue{};
        vk::PFN_CreateCommandPool createCommandPool{};
        vk::PFN_DestroyCommandPool destroyCommandPool{};
        vk::PFN_CreateBuffer createBuffer{};
        vk::PFN_DestroyBuffer destroyBuffer{};
        vk::PFN_GetBufferMemoryRequirements getBufferMemoryRequirements{};
        vk::PFN_AllocateMemory allocateMemory{};
        vk::PFN_FreeMemory freeMemory{};
        vk::PFN_BindBufferMemory bindBufferMemory{};
        vk::PFN_MapMemory mapMemory{};
        vk::PFN_UnmapMemory unmapMemory{};
        vk::PFN_AllocateCommandBuffers allocateCommandBuffers{};
        vk::PFN_FreeCommandBuffers freeCommandBuffers{};
        vk::PFN_BeginCommandBuffer beginCommandBuffer{};
        vk::PFN_EndCommandBuffer endCommandBuffer{};
        vk::PFN_CmdCopyBuffer cmdCopyBuffer{};
        vk::PFN_CmdCopyBufferToImage cmdCopyBufferToImage{};
        vk::PFN_CmdCopyImageToBuffer cmdCopyImageToBuffer{};
        vk::PFN_CmdPipelineBarrier cmdPipelineBarrier{};
        vk::PFN_CreateImage createImage{};
        vk::PFN_DestroyImage destroyImage{};
        vk::PFN_GetImageMemoryRequirements getImageMemoryRequirements{};
        vk::PFN_BindImageMemory bindImageMemory{};
        vk::PFN_CreateImageView createImageView{};
        vk::PFN_DestroyImageView destroyImageView{};
        vk::PFN_CreateSampler createSampler{};
        vk::PFN_DestroySampler destroySampler{};
        vk::PFN_CreateShaderModule createShaderModule{};
        vk::PFN_DestroyShaderModule destroyShaderModule{};
        vk::PFN_CreateRenderPass createRenderPass{};
        vk::PFN_DestroyRenderPass destroyRenderPass{};
        vk::PFN_CreateFramebuffer createFramebuffer{};
        vk::PFN_DestroyFramebuffer destroyFramebuffer{};
        vk::PFN_CreateDescriptorSetLayout createDescriptorSetLayout{};
        vk::PFN_DestroyDescriptorSetLayout destroyDescriptorSetLayout{};
        vk::PFN_CreateDescriptorPool createDescriptorPool{};
        vk::PFN_DestroyDescriptorPool destroyDescriptorPool{};
        vk::PFN_AllocateDescriptorSets allocateDescriptorSets{};
        vk::PFN_UpdateDescriptorSets updateDescriptorSets{};
        vk::PFN_CreatePipelineLayout createPipelineLayout{};
        vk::PFN_DestroyPipelineLayout destroyPipelineLayout{};
        vk::PFN_CreateGraphicsPipelines createGraphicsPipelines{};
        vk::PFN_CreateComputePipelines createComputePipelines{};
        vk::PFN_DestroyPipeline destroyPipeline{};
        vk::PFN_CmdBeginRenderPass cmdBeginRenderPass{};
        vk::PFN_CmdEndRenderPass cmdEndRenderPass{};
        vk::PFN_CmdBindPipeline cmdBindPipeline{};
        vk::PFN_CmdBindDescriptorSets cmdBindDescriptorSets{};
        vk::PFN_CmdDispatch cmdDispatch{};
        vk::PFN_CmdSetViewport cmdSetViewport{};
        vk::PFN_CmdSetScissor cmdSetScissor{};
        vk::PFN_CmdClearAttachments cmdClearAttachments{};
        vk::PFN_CmdBindVertexBuffers cmdBindVertexBuffers{};
        vk::PFN_CmdBindIndexBuffer cmdBindIndexBuffer{};
        vk::PFN_CmdDrawIndexed cmdDrawIndexed{};
        vk::PFN_CreateQueryPool createQueryPool{};
        vk::PFN_DestroyQueryPool destroyQueryPool{};
        vk::PFN_CmdResetQueryPool cmdResetQueryPool{};
        vk::PFN_CmdWriteTimestamp cmdWriteTimestamp{};
        vk::PFN_GetQueryPoolResults getQueryPoolResults{};
        vk::PFN_CreateFence createFence{};
        vk::PFN_DestroyFence destroyFence{};
        vk::PFN_QueueSubmit queueSubmit{};
        vk::PFN_WaitForFences waitForFences{};
        vk::PFN_DeviceWaitIdle deviceWaitIdle{};
    } fn;

    struct BufferSlot {
        std::uint32_t generation{1};
        bool alive{};
        BufferDesc desc;
        ResourceState state{ResourceState::Undefined};
        vk::Buffer buffer{};
        vk::DeviceMemory memory{};
        vk::DeviceSize allocationBytes{};
        vk::MemoryPropertyFlags memoryProperties{};
    };
    struct TextureSlot {
        std::uint32_t generation{1};
        bool alive{};
        TextureDesc desc;
        ResourceState state{ResourceState::Undefined};
        vk::Image image{};
        vk::DeviceMemory memory{};
        vk::DeviceSize allocationBytes{};
    };
    struct TextureViewSlot {
        std::uint32_t generation{1};
        bool alive{};
        TextureViewDesc desc;
        vk::ImageView view{};
    };
    struct SamplerSlot {
        std::uint32_t generation{1};
        bool alive{};
        SamplerDesc desc;
        vk::Sampler sampler{};
    };
    struct BindGroupLayoutSlot {
        std::uint32_t generation{1};
        bool alive{};
        BindGroupLayoutDesc desc;
        vk::DescriptorSetLayout layout{};
    };
    struct BindGroupSlot {
        std::uint32_t generation{1};
        bool alive{};
        BindGroupDesc desc;
        vk::DescriptorPool pool{};
        vk::DescriptorSet set{};
    };
    struct ComputePipelineSlot {
        std::uint32_t generation{1};
        bool alive{};
        ComputePipelineDesc desc;
        vk::Pipeline pipeline{};
        vk::PipelineLayout layout{};
    };
    struct GraphicsPipelineSlot {
        std::uint32_t generation{1};
        bool alive{};
        GraphicsPipelineDesc desc;
        vk::Pipeline pipeline{};
        vk::PipelineLayout layout{};
        vk::RenderPass compatibleRenderPass{};
    };
    struct TimestampPoolSlot {
        std::uint32_t generation{1};
        bool alive{};
        std::uint32_t count{};
        std::string debugName;
        vk::QueryPool pool{};
    };
    struct CopyCommand {
        BufferHandle source;
        std::size_t sourceOffset{};
        BufferHandle destination;
        std::size_t destinationOffset{};
        std::size_t bytes{};
    };
    struct BarrierCommand {
        BufferHandle buffer;
        ResourceState before{ResourceState::Undefined};
        ResourceState after{ResourceState::Undefined};
    };
    struct TextureBarrierCommand { TextureHandle texture; ResourceState before; ResourceState after; };
    struct BeginRenderPassCommand { RenderPassDesc desc; };
    struct EndRenderPassCommand {};
    struct BindPipelineCommand { GraphicsPipelineHandle pipeline; };
    struct BindComputeGroupCommand { std::uint32_t index{}; BindGroupHandle group; };
    struct BindGraphicsGroupCommand { std::uint32_t index{}; BindGroupHandle group; };
    struct DispatchCommand { ComputePipelineHandle pipeline; std::uint32_t groupsX{}; std::uint32_t groupsY{}; std::uint32_t groupsZ{}; };
    struct ViewportCommand { Viewport viewport; };
    struct ScissorCommand { ScissorRect scissor; };
    struct ClearDepthRegionCommand { float depth{}; ScissorRect region; };
    struct VertexBufferCommand { BufferHandle buffer; std::size_t offset; };
    struct IndexBufferCommand { BufferHandle buffer; std::size_t offset; IndexFormat format; };
    struct DrawIndexedCommand { std::uint32_t indexCount; std::uint32_t instanceCount; std::uint32_t firstIndex; std::int32_t vertexOffset; std::uint32_t firstInstance; };
    struct TimestampCommand { TimestampQueryPoolHandle pool; std::uint32_t index{}; };
    using RecordedCommand = std::variant<CopyCommand, BarrierCommand, TextureBarrierCommand,
        BeginRenderPassCommand, EndRenderPassCommand, BindPipelineCommand, BindComputeGroupCommand,
        BindGraphicsGroupCommand, DispatchCommand, ViewportCommand, ScissorCommand,
        ClearDepthRegionCommand, VertexBufferCommand, IndexBufferCommand,
        DrawIndexedCommand, TimestampCommand>;
    struct CommandSlot {
        std::uint32_t generation{1};
        bool alive{};
        QueueKind queue{QueueKind::Graphics};
        std::string debugName;
        std::vector<RecordedCommand> commands;
        std::uint32_t debugDepth{};
        bool renderPassOpen{};
        GraphicsPipelineHandle graphicsPipeline{};
        BufferHandle vertexBuffer{};
        std::uint32_t vertexStride{};
        BufferHandle indexBuffer{};
        IndexFormat indexFormat{IndexFormat::Uint32};
        std::size_t indexOffset{};
        bool viewportSet{};
        bool scissorSet{};
        bool activeDepthAttachment{};
        std::uint32_t activePassWidth{};
        std::uint32_t activePassHeight{};
        std::optional<TextureFormat> activeColorFormat{TextureFormat::RGBA8Unorm};
        std::optional<TextureFormat> activeDepthFormat{};
        std::vector<BindGroupHandle> computeBindGroups;
        std::vector<BindGroupHandle> graphicsBindGroups;
    };
    struct RawBuffer {
        vk::Buffer buffer{};
        vk::DeviceMemory memory{};
        vk::DeviceSize bytes{};
        vk::MemoryPropertyFlags properties{};
    };

    void* loader{};
    vk::Instance instance{};
    vk::PhysicalDevice physicalDevice{};
    vk::Device device{};
    vk::Queue queue{};
    vk::CommandPool commandPool{};
    std::uint32_t queueFamily{};
    std::uint32_t timestampValidBits{};
    vk::PhysicalDeviceMemoryProperties memoryProperties{};
    DeviceCapabilities capabilities{};
    DeviceStatistics statistics{};
    DeviceStatus status{DeviceStatus::Lost};
    std::string reason{"Vulkan backend has not initialized"};
    std::vector<BufferSlot> buffers;
    std::vector<TextureSlot> textures;
    std::vector<TextureViewSlot> textureViews;
    std::vector<SamplerSlot> samplers;
    std::vector<BindGroupLayoutSlot> bindGroupLayouts;
    std::vector<BindGroupSlot> bindGroups;
    std::vector<ComputePipelineSlot> computePipelines;
    std::vector<GraphicsPipelineSlot> graphicsPipelines;
    std::vector<TimestampPoolSlot> timestampPools;
    std::vector<CommandSlot> commandLists;
    std::uint64_t nextFence{1U};
    std::uint64_t completedFence{};

    Impl() {
        capabilities.backend = Backend::Vulkan;
        capabilities.adapterName = "Vulkan 1.0 offscreen graphics device";
        capabilities.presentation = false;
        capabilities.timestampQueries = false;
        capabilities.timestampValidBits = 0U;
        initialize();
    }

    ~Impl() { shutdown(); }

    template <class T>
    bool load_global(T& destination, const char* name) {
        const vk::VoidFunction address = fn.getInstanceProcAddr(nullptr, name);
        destination = function_pointer<T>(address);
        if (!destination) reason = std::string("Vulkan loader is missing ") + name;
        return destination != nullptr;
    }

    template <class T>
    bool load_instance(T& destination, const char* name) {
        const vk::VoidFunction address = fn.getInstanceProcAddr(instance, name);
        destination = function_pointer<T>(address);
        if (!destination) reason = std::string("Vulkan instance is missing ") + name;
        return destination != nullptr;
    }

    template <class T>
    bool load_device(T& destination, const char* name) {
        const vk::VoidFunction address = fn.getDeviceProcAddr(device, name);
        destination = function_pointer<T>(address);
        if (!destination) reason = std::string("Vulkan device is missing ") + name;
        return destination != nullptr;
    }

    bool check(vk::Result result, std::string_view operation, std::string* error = nullptr) {
        if (result == vk::Success) return true;
        reason = std::string(operation) + " failed: " + result_message(result);
        if (result == vk::ErrorDeviceLost) status = DeviceStatus::Lost;
        set_error(error, reason);
        return false;
    }

    void initialize() {
        loader = open_vulkan_loader();
        if (!loader) { reason = "Vulkan loader was not found"; return; }
        fn.getInstanceProcAddr = function_pointer<vk::GetInstanceProcAddr>(
            loader_symbol(loader, "vkGetInstanceProcAddr"));
        if (!fn.getInstanceProcAddr) { reason = "Vulkan loader does not export vkGetInstanceProcAddr"; return; }
        if (!load_global(fn.createInstance, "vkCreateInstance")) return;

        const vk::ApplicationInfo application{
            vk::StructureTypeApplicationInfo, nullptr, "DVE Vulkan graphics validation", 1U,
            "Destructible Voxel Engine", 13400U, vk::ApiVersion10};
        const vk::InstanceCreateInfo createInfo{
            vk::StructureTypeInstanceCreateInfo, nullptr, 0U, &application,
            0U, nullptr, 0U, nullptr};
        if (!check(fn.createInstance(&createInfo, nullptr, &instance), "vkCreateInstance")) return;

        if (!load_instance(fn.destroyInstance, "vkDestroyInstance") ||
            !load_instance(fn.enumeratePhysicalDevices, "vkEnumeratePhysicalDevices") ||
            !load_instance(fn.getPhysicalDeviceProperties, "vkGetPhysicalDeviceProperties") ||
            !load_instance(fn.getQueueFamilyProperties, "vkGetPhysicalDeviceQueueFamilyProperties") ||
            !load_instance(fn.getMemoryProperties, "vkGetPhysicalDeviceMemoryProperties") ||
            !load_instance(fn.getFormatProperties, "vkGetPhysicalDeviceFormatProperties") ||
            !load_instance(fn.createDevice, "vkCreateDevice") ||
            !load_instance(fn.getDeviceProcAddr, "vkGetDeviceProcAddr")) return;

        std::uint32_t physicalCount{};
        if (!check(fn.enumeratePhysicalDevices(instance, &physicalCount, nullptr),
                   "vkEnumeratePhysicalDevices")) return;
        if (physicalCount == 0U) { reason = "Vulkan loader found no physical device"; return; }
        std::vector<vk::PhysicalDevice> devices(physicalCount);
        if (!check(fn.enumeratePhysicalDevices(instance, &physicalCount, devices.data()),
                   "vkEnumeratePhysicalDevices")) return;

        bool found{};
        for (vk::PhysicalDevice candidate : devices) {
            std::uint32_t familyCount{};
            fn.getQueueFamilyProperties(candidate, &familyCount, nullptr);
            std::vector<vk::QueueFamilyProperties> families(familyCount);
            fn.getQueueFamilyProperties(candidate, &familyCount, families.data());
            for (std::uint32_t family = 0; family < familyCount; ++family) {
                const vk::QueueFlags required = vk::QueueGraphicsBit | vk::QueueComputeBit | vk::QueueTransferBit;
                if (families[family].queueCount > 0U &&
                    (families[family].queueFlags & required) == required) {
                    physicalDevice = candidate;
                    queueFamily = family;
                    timestampValidBits = families[family].timestampValidBits;
                    found = true;
                    break;
                }
            }
            if (found) break;
        }
        if (!found) { reason = "Vulkan device has no graphics, compute, and transfer queue"; return; }
        capabilities.queueFamilyIndex = queueFamily;
        vk::PhysicalDeviceProperties physicalProperties{};
        fn.getPhysicalDeviceProperties(physicalDevice, &physicalProperties);
        capabilities.adapterName = physicalProperties.deviceName[0] != '\0'
            ? std::string(physicalProperties.deviceName)
            : std::string("Unnamed Vulkan adapter");
        capabilities.vendorId = physicalProperties.vendorID;
        capabilities.deviceId = physicalProperties.deviceID;
        capabilities.driverVersion = physicalProperties.driverVersion;
        capabilities.apiVersion = physicalProperties.apiVersion;
        capabilities.nativeLimitsQueried = true;
        capabilities.maxTextureDimension2D = physicalProperties.limits.maxImageDimension2D;
        capabilities.maxComputeInvocations = physicalProperties.limits.maxComputeWorkGroupInvocations;
        capabilities.maxComputeWorkgroupSizeX = physicalProperties.limits.maxComputeWorkGroupSize[0];
        capabilities.maxComputeWorkgroupSizeY = physicalProperties.limits.maxComputeWorkGroupSize[1];
        capabilities.maxComputeWorkgroupSizeZ = physicalProperties.limits.maxComputeWorkGroupSize[2];
        capabilities.minStorageBufferOffsetAlignment = static_cast<std::uint32_t>(
            std::min<vk::DeviceSize>(physicalProperties.limits.minStorageBufferOffsetAlignment,
                                     std::numeric_limits<std::uint32_t>::max()));
        capabilities.minUniformBufferOffsetAlignment = static_cast<std::uint32_t>(
            std::min<vk::DeviceSize>(physicalProperties.limits.minUniformBufferOffsetAlignment,
                                     std::numeric_limits<std::uint32_t>::max()));
        capabilities.timestampPeriodNanoseconds =
            static_cast<double>(physicalProperties.limits.timestampPeriod);
        switch (physicalProperties.deviceType) {
        case vk::PhysicalDeviceTypeIntegratedGpu:
            capabilities.adapterClass = AdapterClass::Integrated;
            break;
        case vk::PhysicalDeviceTypeDiscreteGpu:
            capabilities.adapterClass = AdapterClass::Discrete;
            break;
        case vk::PhysicalDeviceTypeVirtualGpu:
            capabilities.adapterClass = AdapterClass::Virtual;
            break;
        case vk::PhysicalDeviceTypeCpu:
            capabilities.adapterClass = AdapterClass::Cpu;
            capabilities.softwareAdapter = true;
            break;
        default:
            capabilities.adapterClass = AdapterClass::Unknown;
            break;
        }
        fn.getMemoryProperties(physicalDevice, &memoryProperties);
        for (std::uint32_t heap = 0U; heap < memoryProperties.memoryHeapCount; ++heap) {
            if ((memoryProperties.memoryHeaps[heap].flags & vk::MemoryHeapDeviceLocalBit) != 0U)
                capabilities.dedicatedVideoMemoryBytes += memoryProperties.memoryHeaps[heap].size;
        }

        const float priority = 1.0F;
        const vk::DeviceQueueCreateInfo queueInfo{
            vk::StructureTypeDeviceQueueCreateInfo, nullptr, 0U, queueFamily, 1U, &priority};
        const vk::DeviceCreateInfo deviceInfo{
            vk::StructureTypeDeviceCreateInfo, nullptr, 0U, 1U, &queueInfo,
            0U, nullptr, 0U, nullptr, nullptr};
        if (!check(fn.createDevice(physicalDevice, &deviceInfo, nullptr, &device),
                   "vkCreateDevice")) return;

        if (!load_device(fn.destroyDevice, "vkDestroyDevice") ||
            !load_device(fn.getDeviceQueue, "vkGetDeviceQueue") ||
            !load_device(fn.createCommandPool, "vkCreateCommandPool") ||
            !load_device(fn.destroyCommandPool, "vkDestroyCommandPool") ||
            !load_device(fn.createBuffer, "vkCreateBuffer") ||
            !load_device(fn.destroyBuffer, "vkDestroyBuffer") ||
            !load_device(fn.getBufferMemoryRequirements, "vkGetBufferMemoryRequirements") ||
            !load_device(fn.allocateMemory, "vkAllocateMemory") ||
            !load_device(fn.freeMemory, "vkFreeMemory") ||
            !load_device(fn.bindBufferMemory, "vkBindBufferMemory") ||
            !load_device(fn.mapMemory, "vkMapMemory") ||
            !load_device(fn.unmapMemory, "vkUnmapMemory") ||
            !load_device(fn.allocateCommandBuffers, "vkAllocateCommandBuffers") ||
            !load_device(fn.freeCommandBuffers, "vkFreeCommandBuffers") ||
            !load_device(fn.beginCommandBuffer, "vkBeginCommandBuffer") ||
            !load_device(fn.endCommandBuffer, "vkEndCommandBuffer") ||
            !load_device(fn.cmdCopyBuffer, "vkCmdCopyBuffer") ||
            !load_device(fn.cmdCopyBufferToImage, "vkCmdCopyBufferToImage") ||
            !load_device(fn.cmdCopyImageToBuffer, "vkCmdCopyImageToBuffer") ||
            !load_device(fn.cmdPipelineBarrier, "vkCmdPipelineBarrier") ||
            !load_device(fn.createImage, "vkCreateImage") ||
            !load_device(fn.destroyImage, "vkDestroyImage") ||
            !load_device(fn.getImageMemoryRequirements, "vkGetImageMemoryRequirements") ||
            !load_device(fn.bindImageMemory, "vkBindImageMemory") ||
            !load_device(fn.createImageView, "vkCreateImageView") ||
            !load_device(fn.destroyImageView, "vkDestroyImageView") ||
            !load_device(fn.createSampler, "vkCreateSampler") ||
            !load_device(fn.destroySampler, "vkDestroySampler") ||
            !load_device(fn.createShaderModule, "vkCreateShaderModule") ||
            !load_device(fn.destroyShaderModule, "vkDestroyShaderModule") ||
            !load_device(fn.createRenderPass, "vkCreateRenderPass") ||
            !load_device(fn.destroyRenderPass, "vkDestroyRenderPass") ||
            !load_device(fn.createFramebuffer, "vkCreateFramebuffer") ||
            !load_device(fn.destroyFramebuffer, "vkDestroyFramebuffer") ||
            !load_device(fn.createDescriptorSetLayout, "vkCreateDescriptorSetLayout") ||
            !load_device(fn.destroyDescriptorSetLayout, "vkDestroyDescriptorSetLayout") ||
            !load_device(fn.createDescriptorPool, "vkCreateDescriptorPool") ||
            !load_device(fn.destroyDescriptorPool, "vkDestroyDescriptorPool") ||
            !load_device(fn.allocateDescriptorSets, "vkAllocateDescriptorSets") ||
            !load_device(fn.updateDescriptorSets, "vkUpdateDescriptorSets") ||
            !load_device(fn.createPipelineLayout, "vkCreatePipelineLayout") ||
            !load_device(fn.destroyPipelineLayout, "vkDestroyPipelineLayout") ||
            !load_device(fn.createGraphicsPipelines, "vkCreateGraphicsPipelines") ||
            !load_device(fn.createComputePipelines, "vkCreateComputePipelines") ||
            !load_device(fn.destroyPipeline, "vkDestroyPipeline") ||
            !load_device(fn.cmdBeginRenderPass, "vkCmdBeginRenderPass") ||
            !load_device(fn.cmdEndRenderPass, "vkCmdEndRenderPass") ||
            !load_device(fn.cmdBindPipeline, "vkCmdBindPipeline") ||
            !load_device(fn.cmdBindDescriptorSets, "vkCmdBindDescriptorSets") ||
            !load_device(fn.cmdDispatch, "vkCmdDispatch") ||
            !load_device(fn.cmdSetViewport, "vkCmdSetViewport") ||
            !load_device(fn.cmdSetScissor, "vkCmdSetScissor") ||
            !load_device(fn.cmdClearAttachments, "vkCmdClearAttachments") ||
            !load_device(fn.cmdBindVertexBuffers, "vkCmdBindVertexBuffers") ||
            !load_device(fn.cmdBindIndexBuffer, "vkCmdBindIndexBuffer") ||
            !load_device(fn.cmdDrawIndexed, "vkCmdDrawIndexed") ||
            !load_device(fn.createQueryPool, "vkCreateQueryPool") ||
            !load_device(fn.destroyQueryPool, "vkDestroyQueryPool") ||
            !load_device(fn.cmdResetQueryPool, "vkCmdResetQueryPool") ||
            !load_device(fn.cmdWriteTimestamp, "vkCmdWriteTimestamp") ||
            !load_device(fn.getQueryPoolResults, "vkGetQueryPoolResults") ||
            !load_device(fn.createFence, "vkCreateFence") ||
            !load_device(fn.destroyFence, "vkDestroyFence") ||
            !load_device(fn.queueSubmit, "vkQueueSubmit") ||
            !load_device(fn.waitForFences, "vkWaitForFences") ||
            !load_device(fn.deviceWaitIdle, "vkDeviceWaitIdle")) return;

        capabilities.timestampQueries = timestampValidBits != 0U;
        capabilities.timestampValidBits = timestampValidBits;
        fn.getDeviceQueue(device, queueFamily, 0U, &queue);
        if (!queue) { reason = "vkGetDeviceQueue returned a null queue"; return; }
        const vk::CommandPoolCreateInfo poolInfo{
            vk::StructureTypeCommandPoolCreateInfo, nullptr,
            vk::CommandPoolCreateTransientBit | vk::CommandPoolCreateResetCommandBufferBit,
            queueFamily};
        if (!check(fn.createCommandPool(device, &poolInfo, nullptr, &commandPool),
                   "vkCreateCommandPool")) return;

        status = DeviceStatus::Ready;
        reason.clear();
    }

    void shutdown() noexcept {
        if (device && fn.deviceWaitIdle) (void)fn.deviceWaitIdle(device);
        if (device) {
            if (fn.destroyQueryPool) {
                for (auto& slot : timestampPools) {
                    if (slot.alive && slot.pool) fn.destroyQueryPool(device, slot.pool, nullptr);
                    slot.alive = false;
                }
            }
            if (fn.destroyPipeline && fn.destroyPipelineLayout) {
                for (auto& slot : computePipelines) {
                    if (!slot.alive) continue;
                    if (slot.pipeline) fn.destroyPipeline(device, slot.pipeline, nullptr);
                    if (slot.layout) fn.destroyPipelineLayout(device, slot.layout, nullptr);
                    slot.alive = false;
                }
            }
            if (fn.destroyDescriptorPool) {
                for (auto& slot : bindGroups) {
                    if (slot.alive && slot.pool) fn.destroyDescriptorPool(device, slot.pool, nullptr);
                    slot.alive = false;
                }
            }
            if (fn.destroyDescriptorSetLayout) {
                for (auto& slot : bindGroupLayouts) {
                    if (slot.alive && slot.layout) fn.destroyDescriptorSetLayout(device, slot.layout, nullptr);
                    slot.alive = false;
                }
            }
            if (fn.destroyPipeline && fn.destroyPipelineLayout && fn.destroyRenderPass) {
                for (auto& slot : graphicsPipelines) {
                    if (!slot.alive) continue;
                    if (slot.pipeline) fn.destroyPipeline(device, slot.pipeline, nullptr);
                    if (slot.layout) fn.destroyPipelineLayout(device, slot.layout, nullptr);
                    if (slot.compatibleRenderPass) fn.destroyRenderPass(device, slot.compatibleRenderPass, nullptr);
                    slot.alive = false;
                }
            }
            if (fn.destroyImageView) {
                for (auto& slot : textureViews) {
                    if (slot.alive && slot.view) fn.destroyImageView(device, slot.view, nullptr);
                    slot.alive = false;
                }
            }
            if (fn.destroySampler) {
                for (auto& slot : samplers) {
                    if (slot.alive && slot.sampler) fn.destroySampler(device, slot.sampler, nullptr);
                    slot.alive = false;
                }
            }
            if (fn.destroyImage && fn.freeMemory) {
                for (auto& slot : textures) {
                    if (!slot.alive) continue;
                    if (slot.image) fn.destroyImage(device, slot.image, nullptr);
                    if (slot.memory) fn.freeMemory(device, slot.memory, nullptr);
                    slot.alive = false;
                }
            }
            if (fn.destroyBuffer && fn.freeMemory) {
                for (auto& slot : buffers) {
                    if (!slot.alive) continue;
                    if (slot.buffer) fn.destroyBuffer(device, slot.buffer, nullptr);
                    if (slot.memory) fn.freeMemory(device, slot.memory, nullptr);
                    slot.alive = false;
                }
            }
        }
        if (device && commandPool && fn.destroyCommandPool)
            fn.destroyCommandPool(device, commandPool, nullptr);
        if (device && fn.destroyDevice) fn.destroyDevice(device, nullptr);
        if (instance && fn.destroyInstance) fn.destroyInstance(instance, nullptr);
        close_vulkan_loader(loader);
        commandPool = {};
        device = nullptr;
        instance = nullptr;
        loader = nullptr;
    }

    bool ready(std::string* error) const {
        if (status == DeviceStatus::Ready) return true;
        set_error(error, reason.empty() ? "Vulkan device is unavailable" : reason);
        return false;
    }

    BufferSlot* buffer(BufferHandle handle, std::string* error) {
        if (!handle || handle.index >= buffers.size()) {
            set_error(error, "invalid Vulkan buffer handle"); return nullptr;
        }
        auto& slot = buffers[handle.index];
        if (!slot.alive || slot.generation != handle.generation) {
            set_error(error, "stale Vulkan buffer handle"); return nullptr;
        }
        return &slot;
    }
    const BufferSlot* buffer(BufferHandle handle, std::string* error) const {
        if (!handle || handle.index >= buffers.size()) {
            set_error(error, "invalid Vulkan buffer handle"); return nullptr;
        }
        const auto& slot = buffers[handle.index];
        if (!slot.alive || slot.generation != handle.generation) {
            set_error(error, "stale Vulkan buffer handle"); return nullptr;
        }
        return &slot;
    }
    TextureSlot* texture(TextureHandle handle, std::string* error) {
        if (!handle || handle.index >= textures.size()) {
            set_error(error, "invalid Vulkan texture handle"); return nullptr;
        }
        auto& slot = textures[handle.index];
        if (!slot.alive || slot.generation != handle.generation) {
            set_error(error, "stale Vulkan texture handle"); return nullptr;
        }
        return &slot;
    }
    const TextureSlot* texture(TextureHandle handle, std::string* error) const {
        if (!handle || handle.index >= textures.size()) {
            set_error(error, "invalid Vulkan texture handle"); return nullptr;
        }
        const auto& slot = textures[handle.index];
        if (!slot.alive || slot.generation != handle.generation) {
            set_error(error, "stale Vulkan texture handle"); return nullptr;
        }
        return &slot;
    }
    TextureViewSlot* texture_view(TextureViewHandle handle, std::string* error) {
        if (!handle || handle.index >= textureViews.size()) {
            set_error(error, "invalid Vulkan texture-view handle"); return nullptr;
        }
        auto& slot = textureViews[handle.index];
        if (!slot.alive || slot.generation != handle.generation) {
            set_error(error, "stale Vulkan texture-view handle"); return nullptr;
        }
        return &slot;
    }
    SamplerSlot* sampler(SamplerHandle handle, std::string* error) {
        if (!handle || handle.index >= samplers.size()) {
            set_error(error, "invalid Vulkan sampler handle"); return nullptr;
        }
        auto& slot = samplers[handle.index];
        if (!slot.alive || slot.generation != handle.generation) {
            set_error(error, "stale Vulkan sampler handle"); return nullptr;
        }
        return &slot;
    }
    const SamplerSlot* sampler(SamplerHandle handle, std::string* error) const {
        if (!handle || handle.index >= samplers.size()) {
            set_error(error, "invalid Vulkan sampler handle"); return nullptr;
        }
        const auto& slot = samplers[handle.index];
        if (!slot.alive || slot.generation != handle.generation) {
            set_error(error, "stale Vulkan sampler handle"); return nullptr;
        }
        return &slot;
    }
    BindGroupLayoutSlot* bind_group_layout(BindGroupLayoutHandle handle, std::string* error) {
        if (!handle || handle.index >= bindGroupLayouts.size()) {
            set_error(error, "invalid Vulkan bind-group-layout handle"); return nullptr;
        }
        auto& slot = bindGroupLayouts[handle.index];
        if (!slot.alive || slot.generation != handle.generation) {
            set_error(error, "stale Vulkan bind-group-layout handle"); return nullptr;
        }
        return &slot;
    }
    const BindGroupLayoutSlot* bind_group_layout(BindGroupLayoutHandle handle, std::string* error) const {
        if (!handle || handle.index >= bindGroupLayouts.size()) {
            set_error(error, "invalid Vulkan bind-group-layout handle"); return nullptr;
        }
        const auto& slot = bindGroupLayouts[handle.index];
        if (!slot.alive || slot.generation != handle.generation) {
            set_error(error, "stale Vulkan bind-group-layout handle"); return nullptr;
        }
        return &slot;
    }
    BindGroupSlot* bind_group(BindGroupHandle handle, std::string* error) {
        if (!handle || handle.index >= bindGroups.size()) {
            set_error(error, "invalid Vulkan bind-group handle"); return nullptr;
        }
        auto& slot = bindGroups[handle.index];
        if (!slot.alive || slot.generation != handle.generation) {
            set_error(error, "stale Vulkan bind-group handle"); return nullptr;
        }
        return &slot;
    }
    const BindGroupSlot* bind_group(BindGroupHandle handle, std::string* error) const {
        if (!handle || handle.index >= bindGroups.size()) {
            set_error(error, "invalid Vulkan bind-group handle"); return nullptr;
        }
        const auto& slot = bindGroups[handle.index];
        if (!slot.alive || slot.generation != handle.generation) {
            set_error(error, "stale Vulkan bind-group handle"); return nullptr;
        }
        return &slot;
    }
    ComputePipelineSlot* compute_pipeline(ComputePipelineHandle handle, std::string* error) {
        if (!handle || handle.index >= computePipelines.size()) {
            set_error(error, "invalid Vulkan compute-pipeline handle"); return nullptr;
        }
        auto& slot = computePipelines[handle.index];
        if (!slot.alive || slot.generation != handle.generation) {
            set_error(error, "stale Vulkan compute-pipeline handle"); return nullptr;
        }
        return &slot;
    }
    const ComputePipelineSlot* compute_pipeline(ComputePipelineHandle handle, std::string* error) const {
        if (!handle || handle.index >= computePipelines.size()) {
            set_error(error, "invalid Vulkan compute-pipeline handle"); return nullptr;
        }
        const auto& slot = computePipelines[handle.index];
        if (!slot.alive || slot.generation != handle.generation) {
            set_error(error, "stale Vulkan compute-pipeline handle"); return nullptr;
        }
        return &slot;
    }
    GraphicsPipelineSlot* graphics_pipeline(GraphicsPipelineHandle handle, std::string* error) {
        if (!handle || handle.index >= graphicsPipelines.size()) {
            set_error(error, "invalid Vulkan graphics-pipeline handle"); return nullptr;
        }
        auto& slot = graphicsPipelines[handle.index];
        if (!slot.alive || slot.generation != handle.generation) {
            set_error(error, "stale Vulkan graphics-pipeline handle"); return nullptr;
        }
        return &slot;
    }
    const GraphicsPipelineSlot* graphics_pipeline(GraphicsPipelineHandle handle, std::string* error) const {
        if (!handle || handle.index >= graphicsPipelines.size()) {
            set_error(error, "invalid Vulkan graphics-pipeline handle"); return nullptr;
        }
        const auto& slot = graphicsPipelines[handle.index];
        if (!slot.alive || slot.generation != handle.generation) {
            set_error(error, "stale Vulkan graphics-pipeline handle"); return nullptr;
        }
        return &slot;
    }
    TimestampPoolSlot* timestamp_pool(TimestampQueryPoolHandle handle, std::string* error) {
        if (!handle || handle.index >= timestampPools.size()) {
            set_error(error, "invalid Vulkan timestamp-query-pool handle"); return nullptr;
        }
        auto& slot = timestampPools[handle.index];
        if (!slot.alive || slot.generation != handle.generation) {
            set_error(error, "stale Vulkan timestamp-query-pool handle"); return nullptr;
        }
        return &slot;
    }
    const TimestampPoolSlot* timestamp_pool(TimestampQueryPoolHandle handle, std::string* error) const {
        if (!handle || handle.index >= timestampPools.size()) {
            set_error(error, "invalid Vulkan timestamp-query-pool handle"); return nullptr;
        }
        const auto& slot = timestampPools[handle.index];
        if (!slot.alive || slot.generation != handle.generation) {
            set_error(error, "stale Vulkan timestamp-query-pool handle"); return nullptr;
        }
        return &slot;
    }

    CommandSlot* command(CommandListHandle handle, std::string* error) {
        if (!handle || handle.index >= commandLists.size()) {
            set_error(error, "invalid Vulkan command-list handle"); return nullptr;
        }
        auto& slot = commandLists[handle.index];
        if (!slot.alive || slot.generation != handle.generation) {
            set_error(error, "stale Vulkan command-list handle"); return nullptr;
        }
        return &slot;
    }

    std::uint32_t find_memory_type(std::uint32_t allowed,
                                   vk::MemoryPropertyFlags required,
                                   vk::MemoryPropertyFlags preferred) const noexcept {
        std::uint32_t fallback = 0xFFFFFFFFU;
        for (std::uint32_t index = 0; index < memoryProperties.memoryTypeCount; ++index) {
            if ((allowed & (1U << index)) == 0U) continue;
            const auto flags = memoryProperties.memoryTypes[index].propertyFlags;
            if ((flags & required) != required) continue;
            if ((flags & preferred) == preferred) return index;
            if (fallback == 0xFFFFFFFFU) fallback = index;
        }
        return fallback;
    }

    bool create_raw_buffer(vk::DeviceSize bytes, vk::BufferUsageFlags usage,
                           vk::MemoryPropertyFlags required,
                           vk::MemoryPropertyFlags preferred,
                           RawBuffer& output, std::string* error) {
        const vk::BufferCreateInfo bufferInfo{
            vk::StructureTypeBufferCreateInfo, nullptr, 0U, bytes, usage,
            vk::SharingModeExclusive, 0U, nullptr};
        if (!check(fn.createBuffer(device, &bufferInfo, nullptr, &output.buffer),
                   "vkCreateBuffer", error)) return false;
        vk::MemoryRequirements requirements{};
        fn.getBufferMemoryRequirements(device, output.buffer, &requirements);
        const std::uint32_t memoryType = find_memory_type(requirements.memoryTypeBits, required, preferred);
        if (memoryType == 0xFFFFFFFFU) {
            fn.destroyBuffer(device, output.buffer, nullptr);
            output.buffer = {};
            set_error(error, "Vulkan device has no compatible memory type");
            return false;
        }
        const vk::MemoryAllocateInfo allocation{
            vk::StructureTypeMemoryAllocateInfo, nullptr, requirements.size, memoryType};
        if (!check(fn.allocateMemory(device, &allocation, nullptr, &output.memory),
                   "vkAllocateMemory", error)) {
            fn.destroyBuffer(device, output.buffer, nullptr);
            output.buffer = {};
            return false;
        }
        if (!check(fn.bindBufferMemory(device, output.buffer, output.memory, 0U),
                   "vkBindBufferMemory", error)) {
            fn.freeMemory(device, output.memory, nullptr);
            fn.destroyBuffer(device, output.buffer, nullptr);
            output = {};
            return false;
        }
        output.bytes = requirements.size;
        output.properties = memoryProperties.memoryTypes[memoryType].propertyFlags;
        return true;
    }

    void destroy_raw_buffer(RawBuffer& buffer) noexcept {
        if (buffer.buffer) fn.destroyBuffer(device, buffer.buffer, nullptr);
        if (buffer.memory) fn.freeMemory(device, buffer.memory, nullptr);
        buffer = {};
    }

    bool map_copy(vk::DeviceMemory memory, vk::DeviceSize offset, std::span<const std::byte> source,
                  std::string* error) {
        void* mapped{};
        if (!check(fn.mapMemory(device, memory, offset, source.size(), 0U, &mapped),
                   "vkMapMemory", error)) return false;
        std::memcpy(mapped, source.data(), source.size());
        fn.unmapMemory(device, memory);
        return true;
    }

    bool map_read(vk::DeviceMemory memory, vk::DeviceSize offset, std::span<std::byte> destination,
                  std::string* error) {
        void* mapped{};
        if (!check(fn.mapMemory(device, memory, offset, destination.size(), 0U, &mapped),
                   "vkMapMemory", error)) return false;
        std::memcpy(destination.data(), mapped, destination.size());
        fn.unmapMemory(device, memory);
        return true;
    }

    bool immediate_copy(vk::Buffer source, vk::DeviceSize sourceOffset,
                        vk::Buffer destination, vk::DeviceSize destinationOffset,
                        vk::DeviceSize bytes, std::string* error) {
        vk::CommandBuffer commandBuffer{};
        const vk::CommandBufferAllocateInfo allocateInfo{
            vk::StructureTypeCommandBufferAllocateInfo, nullptr, commandPool,
            vk::CommandBufferLevelPrimary, 1U};
        if (!check(fn.allocateCommandBuffers(device, &allocateInfo, &commandBuffer),
                   "vkAllocateCommandBuffers", error)) return false;
        const vk::CommandBufferBeginInfo beginInfo{
            vk::StructureTypeCommandBufferBeginInfo, nullptr,
            vk::CommandBufferUsageOneTimeSubmitBit, nullptr};
        if (!check(fn.beginCommandBuffer(commandBuffer, &beginInfo),
                   "vkBeginCommandBuffer", error)) {
            fn.freeCommandBuffers(device, commandPool, 1U, &commandBuffer); return false;
        }
        const vk::BufferCopy copy{sourceOffset, destinationOffset, bytes};
        fn.cmdCopyBuffer(commandBuffer, source, destination, 1U, &copy);
        if (!check(fn.endCommandBuffer(commandBuffer), "vkEndCommandBuffer", error)) {
            fn.freeCommandBuffers(device, commandPool, 1U, &commandBuffer); return false;
        }
        const bool submitted = submit_native(commandBuffer, error);
        fn.freeCommandBuffers(device, commandPool, 1U, &commandBuffer);
        return submitted;
    }

    bool submit_native(vk::CommandBuffer commandBuffer, std::string* error) {
        vk::Fence fence{};
        const vk::FenceCreateInfo fenceInfo{vk::StructureTypeFenceCreateInfo, nullptr, 0U};
        if (!check(fn.createFence(device, &fenceInfo, nullptr, &fence), "vkCreateFence", error))
            return false;
        const vk::SubmitInfo submitInfo{
            vk::StructureTypeSubmitInfo, nullptr, 0U, nullptr, nullptr,
            1U, &commandBuffer, 0U, nullptr};
        bool ok = check(fn.queueSubmit(queue, 1U, &submitInfo, fence), "vkQueueSubmit", error);
        if (ok) ok = check(fn.waitForFences(device, 1U, &fence, 1U,
                                            std::numeric_limits<std::uint64_t>::max()),
                           "vkWaitForFences", error);
        fn.destroyFence(device, fence, nullptr);
        return ok;
    }

    bool record_immediate(const auto& recorder, std::string* error) {
        vk::CommandBuffer commandBuffer{};
        const vk::CommandBufferAllocateInfo allocateInfo{
            vk::StructureTypeCommandBufferAllocateInfo, nullptr, commandPool,
            vk::CommandBufferLevelPrimary, 1U};
        if (!check(fn.allocateCommandBuffers(device, &allocateInfo, &commandBuffer),
                   "vkAllocateCommandBuffers", error)) return false;
        const vk::CommandBufferBeginInfo beginInfo{
            vk::StructureTypeCommandBufferBeginInfo, nullptr,
            vk::CommandBufferUsageOneTimeSubmitBit, nullptr};
        bool ok = check(fn.beginCommandBuffer(commandBuffer, &beginInfo),
                        "vkBeginCommandBuffer", error);
        if (ok) ok = recorder(commandBuffer);
        if (ok) ok = check(fn.endCommandBuffer(commandBuffer), "vkEndCommandBuffer", error);
        if (ok) ok = submit_native(commandBuffer, error);
        fn.freeCommandBuffers(device, commandPool, 1U, &commandBuffer);
        return ok;
    }

    bool image_barrier(vk::CommandBuffer commandBuffer, TextureSlot& textureSlot,
                       ResourceState beforeState, ResourceState afterState,
                       std::string* error) {
        if (textureSlot.state != beforeState) {
            set_error(error, "Vulkan texture barrier before-state mismatch");
            return false;
        }
        const StageAccess before = stage_access(beforeState);
        const StageAccess after = stage_access(afterState);
        const vk::ImageMemoryBarrier barrier{
            vk::StructureTypeImageMemoryBarrier, nullptr, before.access, after.access,
            vulkan_image_layout(beforeState), vulkan_image_layout(afterState),
            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, textureSlot.image,
            {image_aspect(textureSlot.desc.format), 0U, textureSlot.desc.mipLevels,
             0U, textureSlot.desc.arrayLayers}};
        fn.cmdPipelineBarrier(commandBuffer, before.stage, after.stage, 0U,
                              0U, nullptr, 0U, nullptr, 1U, &barrier);
        textureSlot.state = afterState;
        return true;
    }

    bool create_native_image(const TextureDesc& desc, vk::Image& image,
                             vk::DeviceMemory& memory, vk::DeviceSize& allocationBytes,
                             std::string* error) {
        const vk::ImageCreateInfo info{
            vk::StructureTypeImageCreateInfo, nullptr,
            desc.dimension == TextureDimension::TextureCube ? vk::ImageCreateCubeCompatibleBit : 0U,
            desc.dimension == TextureDimension::Texture3D ? vk::ImageType3D : vk::ImageType2D,
            vulkan_format(desc.format), {desc.width, desc.height, desc.depth},
            desc.mipLevels, desc.arrayLayers, vk::SampleCount1Bit, vk::ImageTilingOptimal,
            vulkan_image_usage(desc.usage), vk::SharingModeExclusive, 0U, nullptr,
            vk::ImageLayoutUndefined};
        if (!check(fn.createImage(device, &info, nullptr, &image), "vkCreateImage", error))
            return false;
        vk::MemoryRequirements requirements{};
        fn.getImageMemoryRequirements(device, image, &requirements);
        std::uint32_t memoryType = find_memory_type(requirements.memoryTypeBits,
                                                    vk::MemoryPropertyDeviceLocalBit,
                                                    vk::MemoryPropertyDeviceLocalBit);
        if (memoryType == 0xFFFFFFFFU)
            memoryType = find_memory_type(requirements.memoryTypeBits, 0U,
                                          vk::MemoryPropertyDeviceLocalBit);
        if (memoryType == 0xFFFFFFFFU) {
            fn.destroyImage(device, image, nullptr); image = {};
            set_error(error, "Vulkan texture has no compatible memory type");
            return false;
        }
        const vk::MemoryAllocateInfo allocation{
            vk::StructureTypeMemoryAllocateInfo, nullptr, requirements.size, memoryType};
        if (!check(fn.allocateMemory(device, &allocation, nullptr, &memory),
                   "vkAllocateMemory(texture)", error)) {
            fn.destroyImage(device, image, nullptr); image = {}; return false;
        }
        if (!check(fn.bindImageMemory(device, image, memory, 0U),
                   "vkBindImageMemory", error)) {
            fn.freeMemory(device, memory, nullptr); memory = {};
            fn.destroyImage(device, image, nullptr); image = {}; return false;
        }
        allocationBytes = requirements.size;
        return true;
    }

    bool create_native_view(const TextureSlot& textureSlot, std::uint32_t baseMip,
                            std::uint32_t mipCount, std::uint32_t baseLayer,
                            std::uint32_t layerCount, TextureViewDimension dimension,
                            vk::ImageView& output, std::string* error) {
        const vk::ImageViewCreateInfo info{
            vk::StructureTypeImageViewCreateInfo, nullptr, 0U, textureSlot.image,
            vulkan_view_type(textureSlot.desc, dimension),
            vulkan_format(textureSlot.desc.format),
            {vk::ComponentSwizzleIdentity, vk::ComponentSwizzleIdentity,
             vk::ComponentSwizzleIdentity, vk::ComponentSwizzleIdentity},
            {image_aspect(textureSlot.desc.format), baseMip, mipCount, baseLayer, layerCount}};
        return check(fn.createImageView(device, &info, nullptr, &output),
                     "vkCreateImageView", error);
    }

    bool create_compatible_render_pass(std::optional<TextureFormat> colorFormat,
                                       std::optional<TextureFormat> depthFormat,
                                       bool clearColor, bool clearDepth,
                                       vk::RenderPass& output, std::string* error) {
        if (!colorFormat && !depthFormat) {
            set_error(error, "Vulkan render pass requires a color or depth attachment");
            return false;
        }
        std::array<vk::AttachmentDescription, 2> attachments{};
        std::uint32_t attachmentCount = 0U;
        vk::AttachmentReference colorReference{};
        vk::AttachmentReference depthReference{};
        if (colorFormat) {
            const std::uint32_t index = attachmentCount++;
            attachments[index] = {0U, vulkan_format(*colorFormat), vk::SampleCount1Bit,
                                  clearColor ? vk::AttachmentLoadOpClear : vk::AttachmentLoadOpLoad,
                                  vk::AttachmentStoreOpStore, vk::AttachmentLoadOpDontCare,
                                  vk::AttachmentStoreOpDontCare, vk::ImageLayoutColorAttachmentOptimal,
                                  vk::ImageLayoutColorAttachmentOptimal};
            colorReference = {index, vk::ImageLayoutColorAttachmentOptimal};
        }
        if (depthFormat) {
            const std::uint32_t index = attachmentCount++;
            attachments[index] = {0U, vulkan_format(*depthFormat), vk::SampleCount1Bit,
                                  clearDepth ? vk::AttachmentLoadOpClear : vk::AttachmentLoadOpLoad,
                                  vk::AttachmentStoreOpStore, vk::AttachmentLoadOpDontCare,
                                  vk::AttachmentStoreOpDontCare,
                                  vk::ImageLayoutDepthStencilAttachmentOptimal,
                                  vk::ImageLayoutDepthStencilAttachmentOptimal};
            depthReference = {index, vk::ImageLayoutDepthStencilAttachmentOptimal};
        }
        const vk::SubpassDescription subpass{
            0U, vk::PipelineBindPointGraphics, 0U, nullptr,
            colorFormat ? 1U : 0U, colorFormat ? &colorReference : nullptr,
            nullptr, depthFormat ? &depthReference : nullptr, 0U, nullptr};
        vk::PipelineStageFlags stages = vk::PipelineStageEarlyFragmentTestsBit;
        vk::AccessFlags access = depthFormat ? vk::AccessDepthStencilAttachmentWriteBit : 0U;
        if (colorFormat) {
            stages |= vk::PipelineStageColorAttachmentOutputBit;
            access |= vk::AccessColorAttachmentWriteBit;
        }
        const vk::SubpassDependency dependency{
            vk::SubpassExternal, 0U, stages, stages, 0U, access, 0U};
        const vk::RenderPassCreateInfo info{
            vk::StructureTypeRenderPassCreateInfo, nullptr, 0U, attachmentCount,
            attachments.data(), 1U, &subpass, 1U, &dependency};
        return check(fn.createRenderPass(device, &info, nullptr, &output),
                     "vkCreateRenderPass", error);
    }

    bool unsupported(std::string_view feature, std::string* error) const {
        set_error(error, std::string("Vulkan v1.34 backend does not yet implement ") +
                         std::string(feature));
        return false;
    }
};

VulkanDevice::VulkanDevice() : impl_(std::make_unique<Impl>()) {}
VulkanDevice::~VulkanDevice() = default;
const DeviceCapabilities& VulkanDevice::capabilities() const noexcept { return impl_->capabilities; }
TextureFormatCapabilities VulkanDevice::texture_format_capabilities(TextureFormat format) const noexcept {
    if (!impl_->physicalDevice || !impl_->fn.getFormatProperties) return {};
    vk::FormatProperties properties{};
    impl_->fn.getFormatProperties(impl_->physicalDevice, vulkan_format(format), &properties);
    const auto features = properties.optimalTilingFeatures;
    return {
        (features & vk::FormatFeatureSampledImageBit) != 0U,
        (features & vk::FormatFeatureStorageImageBit) != 0U,
        (features & vk::FormatFeatureStorageImageAtomicBit) != 0U,
        (features & vk::FormatFeatureColorAttachmentBit) != 0U,
        (features & vk::FormatFeatureDepthStencilAttachmentBit) != 0U};
}
DeviceStatistics VulkanDevice::statistics() const noexcept { return impl_->statistics; }
DeviceStatus VulkanDevice::status() const noexcept { return impl_->status; }
std::string_view VulkanDevice::device_loss_reason() const noexcept { return impl_->reason; }

BufferHandle VulkanDevice::create_buffer(const BufferDesc& desc, std::string* error) {
    if (!impl_->ready(error)) return {};
    if (desc.bytes == 0U) { set_error(error, "Vulkan buffer size must be nonzero"); return {}; }
    vk::MemoryPropertyFlags required{};
    vk::MemoryPropertyFlags preferred{};
    switch (desc.memory) {
    case MemoryDomain::DeviceLocal:
        required = vk::MemoryPropertyDeviceLocalBit;
        preferred = vk::MemoryPropertyDeviceLocalBit;
        break;
    case MemoryDomain::Upload:
        required = vk::MemoryPropertyHostVisibleBit | vk::MemoryPropertyHostCoherentBit;
        preferred = required;
        break;
    case MemoryDomain::Readback:
        required = vk::MemoryPropertyHostVisibleBit | vk::MemoryPropertyHostCoherentBit;
        preferred = required | vk::MemoryPropertyHostCachedBit;
        break;
    }
    Impl::RawBuffer raw;
    if (!impl_->create_raw_buffer(desc.bytes, vulkan_buffer_usage(desc.usage), required, preferred,
                                  raw, error)) {
        if (desc.memory == MemoryDomain::DeviceLocal) {
            // Integrated and software devices may expose only host-visible memory. The Vulkan
            // specification permits this fallback; the RHI still preserves logical domains.
            if (!impl_->create_raw_buffer(desc.bytes, vulkan_buffer_usage(desc.usage), 0U,
                                          vk::MemoryPropertyDeviceLocalBit, raw, error)) return {};
        } else return {};
    }
    const auto index = allocate_slot(impl_->buffers);
    auto& slot = impl_->buffers[index];
    slot.alive = true;
    slot.desc = desc;
    slot.state = desc.initialState;
    slot.buffer = raw.buffer;
    slot.memory = raw.memory;
    slot.allocationBytes = raw.bytes;
    slot.memoryProperties = raw.properties;
    ++impl_->statistics.buffersCreated;
    return {index, slot.generation};
}

bool VulkanDevice::destroy_buffer(BufferHandle handle, std::string* error) {
    auto* slot = impl_->buffer(handle, error);
    if (!slot) return false;
    impl_->fn.destroyBuffer(impl_->device, slot->buffer, nullptr);
    impl_->fn.freeMemory(impl_->device, slot->memory, nullptr);
    slot->desc = {};
    slot->buffer = {};
    slot->memory = {};
    retire_slot(*slot);
    ++impl_->statistics.buffersDestroyed;
    return true;
}

bool VulkanDevice::write_buffer(BufferHandle handle, std::size_t offset,
                                std::span<const std::byte> bytes, std::string* error) {
    if (!impl_->ready(error)) return false;
    auto* slot = impl_->buffer(handle, error);
    if (!slot) return false;
    if (!range_fits(offset, bytes.size(), slot->desc.bytes)) {
        set_error(error, "Vulkan buffer upload range exceeds allocation"); return false;
    }
    if (bytes.empty()) return true;
    if ((slot->memoryProperties & (vk::MemoryPropertyHostVisibleBit |
                                   vk::MemoryPropertyHostCoherentBit)) ==
        (vk::MemoryPropertyHostVisibleBit | vk::MemoryPropertyHostCoherentBit)) {
        if (!impl_->map_copy(slot->memory, offset, bytes, error)) return false;
    } else {
        Impl::RawBuffer staging;
        const auto host = vk::MemoryPropertyHostVisibleBit | vk::MemoryPropertyHostCoherentBit;
        if (!impl_->create_raw_buffer(bytes.size(), vk::BufferUsageTransferSourceBit,
                                      host, host, staging, error)) return false;
        const bool written = impl_->map_copy(staging.memory, 0U, bytes, error) &&
                             impl_->immediate_copy(staging.buffer, 0U, slot->buffer, offset,
                                                   bytes.size(), error);
        impl_->destroy_raw_buffer(staging);
        if (!written) return false;
    }
    impl_->statistics.uploadedBytes += bytes.size();
    return true;
}

bool VulkanDevice::read_buffer(BufferHandle handle, std::size_t offset,
                               std::span<std::byte> destination, std::string* error) {
    if (!impl_->ready(error)) return false;
    const auto* slot = impl_->buffer(handle, error);
    if (!slot) return false;
    if (!range_fits(offset, destination.size(), slot->desc.bytes)) {
        set_error(error, "Vulkan buffer readback range exceeds allocation"); return false;
    }
    if (destination.empty()) return true;
    if ((slot->memoryProperties & (vk::MemoryPropertyHostVisibleBit |
                                   vk::MemoryPropertyHostCoherentBit)) ==
        (vk::MemoryPropertyHostVisibleBit | vk::MemoryPropertyHostCoherentBit)) {
        if (!impl_->map_read(slot->memory, offset, destination, error)) return false;
    } else {
        Impl::RawBuffer staging;
        const auto host = vk::MemoryPropertyHostVisibleBit | vk::MemoryPropertyHostCoherentBit;
        if (!impl_->create_raw_buffer(destination.size(), vk::BufferUsageTransferDestinationBit,
                                      host, host | vk::MemoryPropertyHostCachedBit,
                                      staging, error)) return false;
        const bool copied = impl_->immediate_copy(slot->buffer, offset, staging.buffer, 0U,
                                                  destination.size(), error) &&
                            impl_->map_read(staging.memory, 0U, destination, error);
        impl_->destroy_raw_buffer(staging);
        if (!copied) return false;
    }
    impl_->statistics.readbackBytes += destination.size();
    return true;
}

bool VulkanDevice::write_texture(TextureHandle handle, std::uint32_t mipLevel,
                                 std::uint32_t arrayLayer, std::span<const std::byte> bytes,
                                 std::size_t rowPitchBytes, std::string* error) {
    if (!impl_->ready(error)) return false;
    auto* slot = impl_->texture(handle, error);
    if (!slot) return false;
    if (!has_usage(slot->desc.usage, TextureUsage::CopyDestination)) {
        set_error(error, "Vulkan texture lacks CopyDestination usage"); return false;
    }
    if (mipLevel >= slot->desc.mipLevels || arrayLayer >= slot->desc.arrayLayers) {
        set_error(error, "Vulkan texture upload subresource is out of range"); return false;
    }
    const std::size_t pixelBytes = texture_pixel_bytes(slot->desc.format);
    const std::uint32_t width = mip_dimension(slot->desc.width, mipLevel);
    const std::uint32_t height = mip_dimension(slot->desc.height, mipLevel);
    const std::size_t tightPitch = static_cast<std::size_t>(width) * pixelBytes;
    if (rowPitchBytes < tightPitch || rowPitchBytes % pixelBytes != 0U ||
        bytes.size() < rowPitchBytes * static_cast<std::size_t>(height)) {
        set_error(error, "Vulkan texture upload row pitch or byte span is invalid"); return false;
    }
    Impl::RawBuffer staging;
    const auto host = vk::MemoryPropertyHostVisibleBit | vk::MemoryPropertyHostCoherentBit;
    const std::size_t stagingBytes = rowPitchBytes * static_cast<std::size_t>(height);
    if (!impl_->create_raw_buffer(stagingBytes, vk::BufferUsageTransferSourceBit,
                                  host, host, staging, error)) return false;
    bool ok = impl_->map_copy(staging.memory, 0U, bytes.first(stagingBytes), error);
    const ResourceState original = slot->state;
    if (ok) ok = impl_->record_immediate([&](vk::CommandBuffer commandBuffer) {
        if (slot->state != ResourceState::CopyDestination &&
            !impl_->image_barrier(commandBuffer, *slot, slot->state,
                                  ResourceState::CopyDestination, error)) return false;
        const vk::BufferImageCopy copy{
            0U, static_cast<std::uint32_t>(rowPitchBytes / pixelBytes), height,
            {image_aspect(slot->desc.format), mipLevel, arrayLayer, 1U},
            {0, 0}, 0, {width, height, 1U}};
        impl_->fn.cmdCopyBufferToImage(commandBuffer, staging.buffer, slot->image,
                                      vk::ImageLayoutTransferDestinationOptimal, 1U, &copy);
        if (original != ResourceState::Undefined && original != ResourceState::CopyDestination)
            return impl_->image_barrier(commandBuffer, *slot,
                                        ResourceState::CopyDestination, original, error);
        return true;
    }, error);
    impl_->destroy_raw_buffer(staging);
    if (ok) impl_->statistics.uploadedBytes += stagingBytes;
    return ok;
}

bool VulkanDevice::read_texture(TextureHandle handle, std::uint32_t mipLevel,
                                std::uint32_t arrayLayer, std::span<std::byte> destination,
                                std::size_t rowPitchBytes, std::string* error) {
    if (!impl_->ready(error)) return false;
    auto* slot = impl_->texture(handle, error);
    if (!slot) return false;
    if (!has_usage(slot->desc.usage, TextureUsage::CopySource)) {
        set_error(error, "Vulkan texture lacks CopySource usage"); return false;
    }
    if (slot->state == ResourceState::Undefined) {
        set_error(error, "Vulkan texture cannot be read while undefined"); return false;
    }
    if (mipLevel >= slot->desc.mipLevels || arrayLayer >= slot->desc.arrayLayers) {
        set_error(error, "Vulkan texture readback subresource is out of range"); return false;
    }
    const std::size_t pixelBytes = texture_pixel_bytes(slot->desc.format);
    const std::uint32_t width = mip_dimension(slot->desc.width, mipLevel);
    const std::uint32_t height = mip_dimension(slot->desc.height, mipLevel);
    const std::size_t tightPitch = static_cast<std::size_t>(width) * pixelBytes;
    const std::size_t stagingBytes = rowPitchBytes * static_cast<std::size_t>(height);
    if (rowPitchBytes < tightPitch || rowPitchBytes % pixelBytes != 0U ||
        destination.size() < stagingBytes) {
        set_error(error, "Vulkan texture readback row pitch or byte span is invalid"); return false;
    }
    Impl::RawBuffer staging;
    const auto host = vk::MemoryPropertyHostVisibleBit | vk::MemoryPropertyHostCoherentBit;
    if (!impl_->create_raw_buffer(stagingBytes, vk::BufferUsageTransferDestinationBit,
                                  host, host | vk::MemoryPropertyHostCachedBit,
                                  staging, error)) return false;
    const ResourceState original = slot->state;
    bool ok = impl_->record_immediate([&](vk::CommandBuffer commandBuffer) {
        if (slot->state != ResourceState::CopySource &&
            !impl_->image_barrier(commandBuffer, *slot, slot->state,
                                  ResourceState::CopySource, error)) return false;
        const vk::BufferImageCopy copy{
            0U, static_cast<std::uint32_t>(rowPitchBytes / pixelBytes), height,
            {image_aspect(slot->desc.format), mipLevel, arrayLayer, 1U},
            {0, 0}, 0, {width, height, 1U}};
        impl_->fn.cmdCopyImageToBuffer(commandBuffer, slot->image,
                                      vk::ImageLayoutTransferSourceOptimal,
                                      staging.buffer, 1U, &copy);
        if (original != ResourceState::CopySource)
            return impl_->image_barrier(commandBuffer, *slot,
                                        ResourceState::CopySource, original, error);
        return true;
    }, error);
    if (ok) ok = impl_->map_read(staging.memory, 0U, destination.first(stagingBytes), error);
    impl_->destroy_raw_buffer(staging);
    if (ok) impl_->statistics.readbackBytes += stagingBytes;
    return ok;
}

TextureHandle VulkanDevice::create_texture(const TextureDesc& desc, std::string* error) {
    if (!impl_->ready(error)) return {};
    if (desc.width == 0U || desc.height == 0U || desc.depth == 0U ||
        desc.mipLevels == 0U || desc.arrayLayers == 0U ||
        desc.usage == TextureUsage::NoUsage ||
        (desc.dimension == TextureDimension::Texture3D && desc.arrayLayers != 1U) ||
        (desc.dimension == TextureDimension::TextureCube &&
         (desc.width != desc.height || desc.depth != 1U || desc.arrayLayers != 6U))) {
        set_error(error, "Vulkan texture description is invalid"); return {};
    }
    vk::Image image{}; vk::DeviceMemory memory{}; vk::DeviceSize allocationBytes{};
    if (!impl_->create_native_image(desc, image, memory, allocationBytes, error)) return {};
    const auto index = allocate_slot(impl_->textures);
    auto& slot = impl_->textures[index];
    slot.alive = true; slot.desc = desc; slot.state = ResourceState::Undefined;
    slot.image = image; slot.memory = memory; slot.allocationBytes = allocationBytes;
    if (desc.initialState != ResourceState::Undefined) {
        if (!impl_->record_immediate([&](vk::CommandBuffer commandBuffer) {
            return impl_->image_barrier(commandBuffer, slot, ResourceState::Undefined,
                                        desc.initialState, error);
        }, error)) {
            impl_->fn.destroyImage(impl_->device, slot.image, nullptr);
            impl_->fn.freeMemory(impl_->device, slot.memory, nullptr);
            retire_slot(slot); return {};
        }
    }
    ++impl_->statistics.texturesCreated;
    return {index, slot.generation};
}

bool VulkanDevice::destroy_texture(TextureHandle handle, std::string* error) {
    auto* slot = impl_->texture(handle, error); if (!slot) return false;
    for (const auto& view : impl_->textureViews) {
        if (view.alive && view.desc.texture == handle) {
            set_error(error, "Vulkan texture still has a live view"); return false;
        }
    }
    impl_->fn.destroyImage(impl_->device, slot->image, nullptr);
    impl_->fn.freeMemory(impl_->device, slot->memory, nullptr);
    slot->desc = {}; slot->image = {}; slot->memory = {}; retire_slot(*slot);
    ++impl_->statistics.texturesDestroyed;
    return true;
}

TextureViewHandle VulkanDevice::create_texture_view(const TextureViewDesc& desc,
                                                     std::string* error) {
    if (!impl_->ready(error)) return {};
    const auto* texture = impl_->texture(desc.texture, error); if (!texture) return {};
    const TextureViewDimension resolvedDimension = desc.dimension == TextureViewDimension::Automatic
        ? (texture->desc.dimension == TextureDimension::Texture3D
               ? TextureViewDimension::Texture3D
               : texture->desc.dimension == TextureDimension::TextureCube
                     ? TextureViewDimension::TextureCube
                     : texture->desc.arrayLayers > 1U
                           ? TextureViewDimension::Texture2DArray
                           : TextureViewDimension::Texture2D)
        : desc.dimension;
    const bool dimensionValid =
        (resolvedDimension == TextureViewDimension::Texture3D &&
         texture->desc.dimension == TextureDimension::Texture3D && desc.baseLayer == 0U &&
         desc.layerCount == 1U) ||
        (resolvedDimension == TextureViewDimension::TextureCube &&
         texture->desc.dimension == TextureDimension::TextureCube && desc.baseLayer == 0U &&
         desc.layerCount == 6U) ||
        (resolvedDimension == TextureViewDimension::Texture2D &&
         texture->desc.dimension == TextureDimension::Texture2D && desc.layerCount == 1U) ||
        (resolvedDimension == TextureViewDimension::Texture2DArray &&
         texture->desc.dimension == TextureDimension::Texture2D);
    if (desc.mipCount == 0U || desc.layerCount == 0U || !dimensionValid ||
        desc.baseMip >= texture->desc.mipLevels ||
        desc.mipCount > texture->desc.mipLevels - desc.baseMip ||
        desc.baseLayer >= texture->desc.arrayLayers ||
        desc.layerCount > texture->desc.arrayLayers - desc.baseLayer) {
        set_error(error, "Vulkan texture-view range is invalid"); return {};
    }
    vk::ImageView view{};
    if (!impl_->create_native_view(*texture, desc.baseMip, desc.mipCount,
                                   desc.baseLayer, desc.layerCount, resolvedDimension,
                                   view, error)) return {};
    const auto index = allocate_slot(impl_->textureViews);
    auto& slot = impl_->textureViews[index];
    slot.alive = true; slot.desc = desc; slot.view = view;
    ++impl_->statistics.textureViewsCreated;
    return {index, slot.generation};
}

bool VulkanDevice::destroy_texture_view(TextureViewHandle handle, std::string* error) {
    auto* slot = impl_->texture_view(handle, error); if (!slot) return false;
    impl_->fn.destroyImageView(impl_->device, slot->view, nullptr);
    slot->desc = {}; slot->view = {}; retire_slot(*slot);
    ++impl_->statistics.textureViewsDestroyed;
    return true;
}

SamplerHandle VulkanDevice::create_sampler(const SamplerDesc& desc, std::string* error) {
    if (!impl_->ready(error)) return {};
    if (!std::isfinite(desc.minimumLod) || !std::isfinite(desc.maximumLod) ||
        !std::isfinite(desc.mipLodBias) || !std::isfinite(desc.maximumAnisotropy) ||
        desc.minimumLod > desc.maximumLod || desc.maximumAnisotropy < 1.0F ||
        (desc.anisotropy && desc.maximumAnisotropy > 1.0F) ||
        static_cast<std::uint8_t>(desc.comparisonOp) > static_cast<std::uint8_t>(CompareOp::AlwaysPass)) {
        set_error(error, "Vulkan sampler description is invalid or requests unsupported anisotropy");
        return {};
    }
    const vk::SamplerCreateInfo info{
        vk::StructureTypeSamplerCreateInfo, nullptr, 0U,
        vulkan_filter(desc.magFilter), vulkan_filter(desc.minFilter),
        vulkan_mipmap_filter(desc.mipmapFilter),
        vulkan_address_mode(desc.addressU), vulkan_address_mode(desc.addressV),
        vulkan_address_mode(desc.addressW), desc.mipLodBias,
        0U, 1.0F, desc.comparison ? 1U : 0U,
        desc.comparison ? vulkan_compare(desc.comparisonOp) : vk::CompareAlways,
        desc.minimumLod, desc.maximumLod, vk::BorderColorFloatTransparentBlack, 0U};
    vk::Sampler sampler{};
    if (!impl_->check(impl_->fn.createSampler(impl_->device, &info, nullptr, &sampler),
                      "vkCreateSampler", error)) return {};
    const auto index = allocate_slot(impl_->samplers);
    auto& slot = impl_->samplers[index];
    slot.alive = true; slot.desc = desc; slot.sampler = sampler;
    ++impl_->statistics.samplersCreated;
    return {index, slot.generation};
}

bool VulkanDevice::destroy_sampler(SamplerHandle handle, std::string* error) {
    auto* slot = impl_->sampler(handle, error); if (!slot) return false;
    for (const auto& group : impl_->bindGroups) {
        if (!group.alive) continue;
        for (const auto& entry : group.desc.entries) {
            if (entry.sampler == handle) {
                set_error(error, "Vulkan sampler is retained by a live bind group");
                return false;
            }
        }
    }
    impl_->fn.destroySampler(impl_->device, slot->sampler, nullptr);
    slot->desc = {}; slot->sampler = {}; retire_slot(*slot);
    ++impl_->statistics.samplersDestroyed;
    return true;
}

SwapchainHandle VulkanDevice::create_swapchain(const SwapchainDesc&, std::string* error) {
    impl_->unsupported("swapchains", error); return {};
}
bool VulkanDevice::destroy_swapchain(SwapchainHandle, std::string* error) { return impl_->unsupported("swapchains", error); }
AcquiredSwapchainImage VulkanDevice::acquire_next_image(SwapchainHandle, std::string* error) {
    impl_->unsupported("swapchain acquisition", error); return {};
}
PresentResult VulkanDevice::present(SwapchainHandle, FenceHandle, std::string* error) {
    impl_->unsupported("presentation", error);
    return impl_->status == DeviceStatus::Lost ? PresentResult::DeviceLost : PresentResult::Error;
}
BindGroupLayoutHandle VulkanDevice::create_bind_group_layout(
    const BindGroupLayoutDesc& desc, std::string* error) {
    if (!impl_->ready(error)) return {};
    if (desc.bindings.empty()) {
        set_error(error, "Vulkan bind-group layout must contain at least one binding"); return {};
    }
    std::vector<std::uint32_t> seen;
    std::vector<vk::DescriptorSetLayoutBinding> native;
    seen.reserve(desc.bindings.size()); native.reserve(desc.bindings.size());
    for (const auto& binding : desc.bindings) {
        if (binding.visibility == ShaderStage::NoStage ||
            std::find(seen.begin(), seen.end(), binding.binding) != seen.end()) {
            set_error(error, "Vulkan bind-group layout has empty visibility or duplicate bindings");
            return {};
        }
        seen.push_back(binding.binding);
        native.push_back({binding.binding, vulkan_descriptor_type(binding.type), 1U,
                          vulkan_shader_stages(binding.visibility), nullptr});
    }
    vk::DescriptorSetLayout layout{};
    const vk::DescriptorSetLayoutCreateInfo info{
        vk::StructureTypeDescriptorSetLayoutCreateInfo, nullptr, 0U,
        static_cast<std::uint32_t>(native.size()), native.data()};
    if (!impl_->check(impl_->fn.createDescriptorSetLayout(impl_->device, &info, nullptr, &layout),
                      "vkCreateDescriptorSetLayout", error)) return {};
    const auto index = allocate_slot(impl_->bindGroupLayouts);
    auto& slot = impl_->bindGroupLayouts[index];
    slot.alive = true; slot.desc = desc; slot.layout = layout;
    ++impl_->statistics.bindGroupLayoutsCreated;
    return {index, slot.generation};
}

bool VulkanDevice::destroy_bind_group_layout(BindGroupLayoutHandle handle, std::string* error) {
    auto* slot = impl_->bind_group_layout(handle, error); if (!slot) return false;
    for (const auto& group : impl_->bindGroups)
        if (group.alive && group.desc.layout == handle) {
            set_error(error, "Vulkan bind-group layout is retained by a live bind group"); return false;
        }
    for (const auto& pipeline : impl_->computePipelines)
        if (pipeline.alive && std::find(pipeline.desc.bindGroupLayouts.begin(),
                                       pipeline.desc.bindGroupLayouts.end(), handle) !=
                              pipeline.desc.bindGroupLayouts.end()) {
            set_error(error, "Vulkan bind-group layout is retained by a live compute pipeline");
            return false;
        }
    for (const auto& pipeline : impl_->graphicsPipelines)
        if (pipeline.alive && std::find(pipeline.desc.bindGroupLayouts.begin(),
                                       pipeline.desc.bindGroupLayouts.end(), handle) !=
                              pipeline.desc.bindGroupLayouts.end()) {
            set_error(error, "Vulkan bind-group layout is retained by a live graphics pipeline");
            return false;
        }
    impl_->fn.destroyDescriptorSetLayout(impl_->device, slot->layout, nullptr);
    slot->desc = {}; slot->layout = {}; retire_slot(*slot);
    ++impl_->statistics.bindGroupLayoutsDestroyed;
    return true;
}

BindGroupHandle VulkanDevice::create_bind_group(const BindGroupDesc& desc, std::string* error) {
    if (!impl_->ready(error)) return {};
    const auto* layout = impl_->bind_group_layout(desc.layout, error); if (!layout) return {};
    if (desc.entries.size() != layout->desc.bindings.size()) {
        set_error(error, "Vulkan bind group must provide exactly one entry per layout binding");
        return {};
    }
    std::vector<vk::DescriptorPoolSize> poolSizes;
    std::vector<vk::WriteDescriptorSet> writes;
    std::vector<vk::DescriptorBufferInfo> bufferInfos;
    std::vector<vk::DescriptorImageInfo> imageInfos;
    std::vector<std::uint32_t> seen;
    poolSizes.reserve(layout->desc.bindings.size()); writes.reserve(desc.entries.size());
    bufferInfos.reserve(desc.entries.size()); imageInfos.reserve(desc.entries.size());
    seen.reserve(desc.entries.size());
    for (const auto& entry : desc.entries) {
        if (std::find(seen.begin(), seen.end(), entry.binding) != seen.end()) {
            set_error(error, "Vulkan bind group contains duplicate entries"); return {};
        }
        seen.push_back(entry.binding);
        const auto bindingIt = std::find_if(layout->desc.bindings.begin(), layout->desc.bindings.end(),
            [&](const BindGroupLayoutBinding& binding) { return binding.binding == entry.binding; });
        if (bindingIt == layout->desc.bindings.end()) {
            set_error(error, "Vulkan bind-group entry does not exist in its layout"); return {};
        }
        const vk::DescriptorType descriptorType = vulkan_descriptor_type(bindingIt->type);
        const auto poolIt = std::find_if(poolSizes.begin(), poolSizes.end(),
            [&](const vk::DescriptorPoolSize& size) { return size.type == descriptorType; });
        if (poolIt == poolSizes.end()) poolSizes.push_back({descriptorType, 1U});
        else ++poolIt->descriptorCount;
        if (bindingIt->type == BindingType::UniformBuffer ||
            bindingIt->type == BindingType::StorageBufferReadOnly ||
            bindingIt->type == BindingType::StorageBufferReadWrite) {
            const auto* resource = impl_->buffer(entry.buffer, error); if (!resource) return {};
            if (entry.textureView || entry.sampler) {
                set_error(error, "Vulkan buffer binding also supplied a texture view or sampler"); return {};
            }
            const std::size_t bytes = entry.bytes == 0U
                ? resource->desc.bytes - std::min(entry.offset, resource->desc.bytes) : entry.bytes;
            if (bytes == 0U || !range_fits(entry.offset, bytes, resource->desc.bytes)) {
                set_error(error, "Vulkan bind-group buffer range exceeds allocation"); return {};
            }
            const BufferUsage required = bindingIt->type == BindingType::UniformBuffer
                ? BufferUsage::Constant : BufferUsage::Storage;
            if (!has_usage(resource->desc.usage, required)) {
                set_error(error, "Vulkan bind-group buffer lacks required usage"); return {};
            }
            bufferInfos.push_back({resource->buffer, entry.offset, bytes});
            writes.push_back({vk::StructureTypeWriteDescriptorSet, nullptr, 0U, entry.binding, 0U,
                              1U, descriptorType, nullptr, &bufferInfos.back(), nullptr});
        } else if (bindingIt->type == BindingType::StorageTexture) {
            auto* view = impl_->texture_view(entry.textureView, error); if (!view) return {};
            const auto* resource = impl_->texture(view->desc.texture, error); if (!resource) return {};
            if (entry.buffer || entry.sampler || !has_usage(resource->desc.usage, TextureUsage::Storage)) {
                set_error(error, "Vulkan storage-texture binding is invalid"); return {};
            }
            imageInfos.push_back({0U, view->view, vk::ImageLayoutGeneral});
            writes.push_back({vk::StructureTypeWriteDescriptorSet, nullptr, 0U, entry.binding, 0U,
                              1U, descriptorType, &imageInfos.back(), nullptr, nullptr});
        } else if (bindingIt->type == BindingType::SampledTexture) {
            auto* view = impl_->texture_view(entry.textureView, error); if (!view) return {};
            const auto* resource = impl_->texture(view->desc.texture, error); if (!resource) return {};
            const auto* sampler = impl_->sampler(entry.sampler, error); if (!sampler) return {};
            if (entry.buffer || !has_usage(resource->desc.usage, TextureUsage::Sampled)) {
                set_error(error, "Vulkan sampled-texture binding is invalid"); return {};
            }
            imageInfos.push_back({sampler->sampler, view->view, vk::ImageLayoutShaderReadOnlyOptimal});
            writes.push_back({vk::StructureTypeWriteDescriptorSet, nullptr, 0U, entry.binding, 0U,
                              1U, descriptorType, &imageInfos.back(), nullptr, nullptr});
        } else {
            set_error(error, "Vulkan bind-group binding type is unsupported"); return {};
        }
    }
    vk::DescriptorPool pool{};
    const vk::DescriptorPoolCreateInfo poolInfo{
        vk::StructureTypeDescriptorPoolCreateInfo, nullptr, 0U, 1U,
        static_cast<std::uint32_t>(poolSizes.size()), poolSizes.data()};
    if (!impl_->check(impl_->fn.createDescriptorPool(impl_->device, &poolInfo, nullptr, &pool),
                      "vkCreateDescriptorPool", error)) return {};
    vk::DescriptorSet set{};
    const vk::DescriptorSetAllocateInfo allocateInfo{
        vk::StructureTypeDescriptorSetAllocateInfo, nullptr, pool, 1U, &layout->layout};
    if (!impl_->check(impl_->fn.allocateDescriptorSets(impl_->device, &allocateInfo, &set),
                      "vkAllocateDescriptorSets", error)) {
        impl_->fn.destroyDescriptorPool(impl_->device, pool, nullptr); return {};
    }
    for (auto& write : writes) write.dstSet = set;
    impl_->fn.updateDescriptorSets(impl_->device, static_cast<std::uint32_t>(writes.size()),
                                   writes.data(), 0U, nullptr);
    const auto index = allocate_slot(impl_->bindGroups);
    auto& slot = impl_->bindGroups[index];
    slot.alive = true; slot.desc = desc; slot.pool = pool; slot.set = set;
    ++impl_->statistics.bindGroupsCreated;
    return {index, slot.generation};
}

bool VulkanDevice::destroy_bind_group(BindGroupHandle handle, std::string* error) {
    auto* slot = impl_->bind_group(handle, error); if (!slot) return false;
    impl_->fn.destroyDescriptorPool(impl_->device, slot->pool, nullptr);
    slot->desc = {}; slot->pool = {}; slot->set = {}; retire_slot(*slot);
    ++impl_->statistics.bindGroupsDestroyed;
    return true;
}

ComputePipelineHandle VulkanDevice::create_compute_pipeline(const ComputePipelineDesc& desc,
                                                              std::string* error) {
    if (!impl_->ready(error)) return {};
    const std::uint64_t invocations = static_cast<std::uint64_t>(desc.threadsX) *
                                      desc.threadsY * desc.threadsZ;
    if (desc.entryPoint.empty() || !valid_spirv(desc.bytecode) || desc.threadsX == 0U ||
        desc.threadsY == 0U || desc.threadsZ == 0U ||
        desc.threadsX > impl_->capabilities.maxComputeWorkgroupSizeX ||
        desc.threadsY > impl_->capabilities.maxComputeWorkgroupSizeY ||
        desc.threadsZ > impl_->capabilities.maxComputeWorkgroupSizeZ ||
        invocations > impl_->capabilities.maxComputeInvocations) {
        set_error(error, "Vulkan compute-pipeline description, SPIR-V, or workgroup size is invalid");
        return {};
    }
    std::vector<vk::DescriptorSetLayout> nativeLayouts;
    nativeLayouts.reserve(desc.bindGroupLayouts.size());
    for (const auto handle : desc.bindGroupLayouts) {
        const auto* layout = impl_->bind_group_layout(handle, error); if (!layout) return {};
        nativeLayouts.push_back(layout->layout);
    }
    std::vector<std::uint32_t> words(desc.bytecode.size() / sizeof(std::uint32_t));
    std::memcpy(words.data(), desc.bytecode.data(), desc.bytecode.size());
    vk::ShaderModule module{};
    const vk::ShaderModuleCreateInfo moduleInfo{
        vk::StructureTypeShaderModuleCreateInfo, nullptr, 0U, desc.bytecode.size(), words.data()};
    if (!impl_->check(impl_->fn.createShaderModule(impl_->device, &moduleInfo, nullptr, &module),
                      "vkCreateShaderModule(compute)", error)) return {};
    vk::PipelineLayout pipelineLayout{};
    vk::Pipeline pipeline{};
    const vk::PipelineLayoutCreateInfo layoutInfo{
        vk::StructureTypePipelineLayoutCreateInfo, nullptr, 0U,
        static_cast<std::uint32_t>(nativeLayouts.size()), nativeLayouts.data(), 0U, nullptr};
    bool ok = impl_->check(impl_->fn.createPipelineLayout(impl_->device, &layoutInfo, nullptr,
                                                          &pipelineLayout),
                           "vkCreatePipelineLayout(compute)", error);
    if (ok) {
        const vk::PipelineShaderStageCreateInfo stage{
            vk::StructureTypePipelineShaderStageCreateInfo, nullptr, 0U,
            vk::ShaderStageComputeBit, module, desc.entryPoint.c_str(), nullptr};
        const vk::ComputePipelineCreateInfo info{
            vk::StructureTypeComputePipelineCreateInfo, nullptr, 0U, stage,
            pipelineLayout, 0U, -1};
        ok = impl_->check(impl_->fn.createComputePipelines(impl_->device, 0U, 1U, &info,
                                                           nullptr, &pipeline),
                          "vkCreateComputePipelines", error);
    }
    impl_->fn.destroyShaderModule(impl_->device, module, nullptr);
    if (!ok) {
        if (pipeline) impl_->fn.destroyPipeline(impl_->device, pipeline, nullptr);
        if (pipelineLayout) impl_->fn.destroyPipelineLayout(impl_->device, pipelineLayout, nullptr);
        return {};
    }
    const auto index = allocate_slot(impl_->computePipelines);
    auto& slot = impl_->computePipelines[index];
    slot.alive = true; slot.desc = desc; slot.pipeline = pipeline; slot.layout = pipelineLayout;
    return {index, slot.generation};
}

bool VulkanDevice::destroy_compute_pipeline(ComputePipelineHandle handle, std::string* error) {
    auto* slot = impl_->compute_pipeline(handle, error); if (!slot) return false;
    impl_->fn.destroyPipeline(impl_->device, slot->pipeline, nullptr);
    impl_->fn.destroyPipelineLayout(impl_->device, slot->layout, nullptr);
    slot->desc = {}; slot->pipeline = {}; slot->layout = {}; retire_slot(*slot);
    return true;
}

GraphicsPipelineHandle VulkanDevice::create_graphics_pipeline(const GraphicsPipelineDesc& desc,
                                                               std::string* error) {
    if (!impl_->ready(error)) return {};
    if (!validate_vertex_input_layout(desc, error)) return {};
    const bool hasFragment = !desc.fragmentBytecode.empty() || !desc.fragmentEntryPoint.empty();
    if (desc.vertexEntryPoint.empty() || !valid_spirv(desc.vertexBytecode) ||
        (hasFragment && (desc.fragmentEntryPoint.empty() || !valid_spirv(desc.fragmentBytecode))) ||
        (!desc.colorFormat && !desc.depthFormat) ||
        desc.topology != PrimitiveTopology::TriangleList || desc.sampleCount != 1U ||
        (desc.colorFormat && *desc.colorFormat == TextureFormat::D32Float) ||
        (desc.depthFormat && *desc.depthFormat != TextureFormat::D32Float) ||
        (desc.colorFormat && !hasFragment)) {
        set_error(error, "Vulkan graphics-pipeline description or SPIR-V is invalid"); return {};
    }
    std::vector<vk::DescriptorSetLayout> nativeLayouts;
    nativeLayouts.reserve(desc.bindGroupLayouts.size());
    for (const auto handle : desc.bindGroupLayouts) {
        const auto* bindLayout = impl_->bind_group_layout(handle, error);
        if (!bindLayout) return {};
        nativeLayouts.push_back(bindLayout->layout);
    }
    auto create_module = [&](std::span<const std::byte> bytes, vk::ShaderModule& module) {
        std::vector<std::uint32_t> words(bytes.size() / sizeof(std::uint32_t));
        std::memcpy(words.data(), bytes.data(), bytes.size());
        const vk::ShaderModuleCreateInfo info{
            vk::StructureTypeShaderModuleCreateInfo, nullptr, 0U,
            bytes.size(), words.data()};
        return impl_->check(impl_->fn.createShaderModule(impl_->device, &info, nullptr, &module),
                            "vkCreateShaderModule", error);
    };
    vk::ShaderModule vertex{}, fragment{};
    if (!create_module(desc.vertexBytecode, vertex)) return {};
    if (hasFragment && !create_module(desc.fragmentBytecode, fragment)) {
        impl_->fn.destroyShaderModule(impl_->device, vertex, nullptr); return {};
    }
    vk::RenderPass compatible{};
    vk::PipelineLayout layout{};
    vk::Pipeline pipeline{};
    bool ok = impl_->create_compatible_render_pass(desc.colorFormat, desc.depthFormat,
                                                   true, true, compatible, error);
    if (ok) {
        const vk::PipelineLayoutCreateInfo layoutInfo{
            vk::StructureTypePipelineLayoutCreateInfo, nullptr, 0U,
            static_cast<std::uint32_t>(nativeLayouts.size()), nativeLayouts.data(), 0U, nullptr};
        ok = impl_->check(impl_->fn.createPipelineLayout(impl_->device, &layoutInfo, nullptr,
                                                        &layout),
                          "vkCreatePipelineLayout", error);
    }
    if (ok) {
        std::array<vk::PipelineShaderStageCreateInfo, 2> stageStorage{};
        stageStorage[0] = {vk::StructureTypePipelineShaderStageCreateInfo, nullptr, 0U,
                           vk::ShaderStageVertexBit, vertex, desc.vertexEntryPoint.c_str(), nullptr};
        std::uint32_t stageCount = 1U;
        if (hasFragment) {
            stageStorage[stageCount++] = {
                vk::StructureTypePipelineShaderStageCreateInfo, nullptr, 0U,
                vk::ShaderStageFragmentBit, fragment, desc.fragmentEntryPoint.c_str(), nullptr};
        }
        vk::VertexInputBindingDescription vertexBinding{};
        std::vector<vk::VertexInputAttributeDescription> vertexAttributes;
        if (desc.vertexBuffer) {
            vertexBinding = {0U, desc.vertexBuffer->stride,
                desc.vertexBuffer->perInstance
                    ? vk::VertexInputRateInstance : vk::VertexInputRateVertex};
            vertexAttributes.reserve(desc.vertexAttributes.size());
            for (const VertexAttributeDesc& attribute : desc.vertexAttributes)
                vertexAttributes.push_back({attribute.location, 0U,
                    vulkan_vertex_format(attribute.format), attribute.offset});
        }
        const vk::PipelineVertexInputStateCreateInfo vertexInput{
            vk::StructureTypePipelineVertexInputStateCreateInfo, nullptr, 0U,
            desc.vertexBuffer ? 1U : 0U, desc.vertexBuffer ? &vertexBinding : nullptr,
            static_cast<std::uint32_t>(vertexAttributes.size()), vertexAttributes.data()};
        const vk::PipelineInputAssemblyStateCreateInfo assembly{
            vk::StructureTypePipelineInputAssemblyStateCreateInfo, nullptr, 0U,
            vk::PrimitiveTopologyTriangleList, 0U};
        const vk::PipelineViewportStateCreateInfo viewportState{
            vk::StructureTypePipelineViewportStateCreateInfo, nullptr, 0U,
            1U, nullptr, 1U, nullptr};
        const vk::PipelineRasterizationStateCreateInfo raster{
            vk::StructureTypePipelineRasterizationStateCreateInfo, nullptr, 0U,
            0U, 0U, vk::PolygonModeFill, vulkan_cull_mode(desc.cullMode),
            vulkan_front_face(desc.frontFace), 0U, 0.0F, 0.0F, 0.0F, 1.0F};
        const vk::PipelineMultisampleStateCreateInfo multisample{
            vk::StructureTypePipelineMultisampleStateCreateInfo, nullptr, 0U,
            vk::SampleCount1Bit, 0U, 0.0F, nullptr, 0U, 0U};
        const vk::PipelineDepthStencilStateCreateInfo depth{
            vk::StructureTypePipelineDepthStencilStateCreateInfo, nullptr, 0U,
            desc.depthTest ? 1U : 0U, desc.depthWrite ? 1U : 0U,
            vulkan_compare(desc.depthCompare), 0U, 0U, {}, {}, 0.0F, 1.0F};
        vk::PipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = vk::ColorComponentRBit | vk::ColorComponentGBit |
                               vk::ColorComponentBBit | vk::ColorComponentABit;
        if (desc.blend == BlendMode::Opaque) {
            blend.blendEnable = 0U;
            blend.srcColorBlendFactor = vk::BlendFactorOne;
            blend.dstColorBlendFactor = vk::BlendFactorZero;
            blend.srcAlphaBlendFactor = vk::BlendFactorOne;
            blend.dstAlphaBlendFactor = vk::BlendFactorZero;
        } else if (desc.blend == BlendMode::Alpha) {
            blend.blendEnable = 1U;
            blend.srcColorBlendFactor = vk::BlendFactorSrcAlpha;
            blend.dstColorBlendFactor = vk::BlendFactorOneMinusSrcAlpha;
            blend.srcAlphaBlendFactor = vk::BlendFactorOne;
            blend.dstAlphaBlendFactor = vk::BlendFactorOneMinusSrcAlpha;
        } else if (desc.blend == BlendMode::Additive) {
            blend.blendEnable = 1U;
            blend.srcColorBlendFactor = vk::BlendFactorOne;
            blend.dstColorBlendFactor = vk::BlendFactorOne;
            blend.srcAlphaBlendFactor = vk::BlendFactorOne;
            blend.dstAlphaBlendFactor = vk::BlendFactorOne;
        } else {
            blend.blendEnable = 1U;
            blend.srcColorBlendFactor = vk::BlendFactorDstColor;
            blend.dstColorBlendFactor = vk::BlendFactorZero;
            blend.srcAlphaBlendFactor = vk::BlendFactorOne;
            blend.dstAlphaBlendFactor = vk::BlendFactorZero;
        }
        blend.colorBlendOp = vk::BlendOpAdd;
        blend.alphaBlendOp = vk::BlendOpAdd;
        const vk::PipelineColorBlendStateCreateInfo blendState{
            vk::StructureTypePipelineColorBlendStateCreateInfo, nullptr, 0U,
            0U, 0, desc.colorFormat ? 1U : 0U, desc.colorFormat ? &blend : nullptr,
            {0.0F, 0.0F, 0.0F, 0.0F}};
        const std::array<vk::DynamicState, 2> dynamicStates{
            vk::DynamicStateViewport, vk::DynamicStateScissor};
        const vk::PipelineDynamicStateCreateInfo dynamic{
            vk::StructureTypePipelineDynamicStateCreateInfo, nullptr, 0U,
            static_cast<std::uint32_t>(dynamicStates.size()), dynamicStates.data()};
        const vk::GraphicsPipelineCreateInfo info{
            vk::StructureTypeGraphicsPipelineCreateInfo, nullptr, 0U,
            stageCount, stageStorage.data(),
            &vertexInput, &assembly, nullptr, &viewportState, &raster,
            &multisample, desc.depthFormat ? &depth : nullptr,
            &blendState, &dynamic, layout, compatible, 0U, 0U, -1};
        ok = impl_->check(impl_->fn.createGraphicsPipelines(impl_->device, 0U, 1U,
                                                            &info, nullptr, &pipeline),
                          "vkCreateGraphicsPipelines", error);
    }
    if (fragment) impl_->fn.destroyShaderModule(impl_->device, fragment, nullptr);
    impl_->fn.destroyShaderModule(impl_->device, vertex, nullptr);
    if (!ok) {
        if (pipeline) impl_->fn.destroyPipeline(impl_->device, pipeline, nullptr);
        if (layout) impl_->fn.destroyPipelineLayout(impl_->device, layout, nullptr);
        if (compatible) impl_->fn.destroyRenderPass(impl_->device, compatible, nullptr);
        return {};
    }
    const auto index = allocate_slot(impl_->graphicsPipelines);
    auto& slot = impl_->graphicsPipelines[index];
    slot.alive = true; slot.desc = desc; slot.pipeline = pipeline;
    slot.layout = layout; slot.compatibleRenderPass = compatible;
    return {index, slot.generation};
}

bool VulkanDevice::destroy_graphics_pipeline(GraphicsPipelineHandle handle,
                                              std::string* error) {
    auto* slot = impl_->graphics_pipeline(handle, error); if (!slot) return false;
    impl_->fn.destroyPipeline(impl_->device, slot->pipeline, nullptr);
    impl_->fn.destroyPipelineLayout(impl_->device, slot->layout, nullptr);
    impl_->fn.destroyRenderPass(impl_->device, slot->compatibleRenderPass, nullptr);
    slot->desc = {}; slot->pipeline = {}; slot->layout = {};
    slot->compatibleRenderPass = {}; retire_slot(*slot);
    return true;
}


CommandListHandle VulkanDevice::begin_commands(QueueKind queue, std::string_view debugName,
                                                std::string* error) {
    if (!impl_->ready(error)) return {};
    const auto index = allocate_slot(impl_->commandLists);
    auto& slot = impl_->commandLists[index];
    slot.alive = true;
    slot.queue = queue;
    slot.debugName.assign(debugName);
    slot.commands.clear();
    slot.debugDepth = 0U;
    slot.renderPassOpen = false;
    slot.graphicsPipeline = {};
    slot.vertexBuffer = {};
    slot.vertexStride = 0U;
    slot.indexBuffer = {};
    slot.viewportSet = false;
    slot.scissorSet = false;
    slot.activeDepthFormat.reset();
    slot.computeBindGroups.clear();
    slot.graphicsBindGroups.clear();
    return {index, slot.generation};
}

bool VulkanDevice::copy_buffer(CommandListHandle commands, BufferHandle source,
                               std::size_t sourceOffset, BufferHandle destination,
                               std::size_t destinationOffset, std::size_t bytes,
                               std::string* error) {
    auto* list = impl_->command(commands, error);
    const auto* src = impl_->buffer(source, error);
    const auto* dst = impl_->buffer(destination, error);
    if (!list || !src || !dst) return false;
    if (!has_usage(src->desc.usage, BufferUsage::CopySource) ||
        !has_usage(dst->desc.usage, BufferUsage::CopyDestination)) {
        set_error(error, "Vulkan copy buffers lack source/destination usage"); return false;
    }
    if (bytes == 0U || !range_fits(sourceOffset, bytes, src->desc.bytes) ||
        !range_fits(destinationOffset, bytes, dst->desc.bytes)) {
        set_error(error, "Vulkan buffer copy range is invalid"); return false;
    }
    list->commands.emplace_back(Impl::CopyCommand{
        source, sourceOffset, destination, destinationOffset, bytes});
    return true;
}

bool VulkanDevice::transition_buffer(CommandListHandle commands, BufferHandle handle,
                                     ResourceState before, ResourceState after,
                                     std::string* error) {
    auto* list = impl_->command(commands, error);
    if (!list || !impl_->buffer(handle, error)) return false;
    if (before == after) { set_error(error, "Vulkan buffer barrier must change state"); return false; }
    list->commands.emplace_back(Impl::BarrierCommand{handle, before, after});
    return true;
}

bool VulkanDevice::transition_texture(CommandListHandle commands, TextureHandle handle,
                                      ResourceState before, ResourceState after,
                                      std::string* error) {
    auto* list = impl_->command(commands, error);
    const auto* texture = impl_->texture(handle, error);
    if (!list || !texture) return false;
    if (before == after) { set_error(error, "Vulkan texture barrier must change state"); return false; }
    if (list->renderPassOpen) { set_error(error, "Vulkan texture barriers cannot be recorded inside a render pass"); return false; }
    ResourceState recordedState = texture->state;
    for (const Impl::RecordedCommand& command : list->commands) {
        const auto* barrier = std::get_if<Impl::TextureBarrierCommand>(&command);
        if (barrier != nullptr && barrier->texture == handle) recordedState = barrier->after;
    }
    if (recordedState != before) {
        set_error(error, "Vulkan texture barrier before-state mismatch");
        return false;
    }
    list->commands.emplace_back(Impl::TextureBarrierCommand{handle, before, after});
    return true;
}

bool VulkanDevice::dispatch(CommandListHandle commands, ComputePipelineHandle handle,
                            std::uint32_t groupsX, std::uint32_t groupsY,
                            std::uint32_t groupsZ, std::string* error) {
    auto* list = impl_->command(commands, error);
    const auto* pipeline = impl_->compute_pipeline(handle, error);
    if (!list || !pipeline) return false;
    if (list->renderPassOpen || list->queue == QueueKind::Copy ||
        groupsX == 0U || groupsY == 0U || groupsZ == 0U) {
        set_error(error, "Vulkan compute dispatch requires nonzero groups outside a render pass");
        return false;
    }
    if (list->computeBindGroups.size() < pipeline->desc.bindGroupLayouts.size()) {
        set_error(error, "Vulkan compute dispatch is missing required bind groups"); return false;
    }
    for (std::size_t index = 0; index < pipeline->desc.bindGroupLayouts.size(); ++index) {
        const auto* group = impl_->bind_group(list->computeBindGroups[index], error);
        if (!group) return false;
        if (group->desc.layout != pipeline->desc.bindGroupLayouts[index]) {
            set_error(error, "Vulkan compute bind-group layout does not match pipeline layout");
            return false;
        }
    }
    list->commands.emplace_back(Impl::DispatchCommand{handle, groupsX, groupsY, groupsZ});
    return true;
}

bool VulkanDevice::begin_render_pass(CommandListHandle commands, const RenderPassDesc& desc,
                                     std::string* error) {
    auto* list = impl_->command(commands, error); if (!list) return false;
    if (list->queue != QueueKind::Graphics || list->renderPassOpen ||
        desc.colors.size() > 1U || (desc.colors.empty() && !desc.depth)) {
        set_error(error, "Vulkan render pass requires a graphics queue, at most one color target, and at least one attachment");
        return false;
    }
    std::uint32_t width = 0U, height = 0U;
    std::optional<TextureFormat> colorFormat;
    const auto recorded_texture_state = [&](TextureHandle handle, ResourceState initial) {
        ResourceState state = initial;
        for (const Impl::RecordedCommand& command : list->commands) {
            const auto* barrier = std::get_if<Impl::TextureBarrierCommand>(&command);
            if (barrier != nullptr && barrier->texture == handle) state = barrier->after;
        }
        return state;
    };
    if (!desc.colors.empty()) {
        const auto* color = impl_->texture(desc.colors.front().texture, error); if (!color) return false;
        if (!has_usage(color->desc.usage, TextureUsage::RenderTarget) ||
            color->desc.format == TextureFormat::D32Float ||
            recorded_texture_state(desc.colors.front().texture, color->state) !=
                ResourceState::RenderTarget) {
            set_error(error, "Vulkan color attachment must be a RenderTarget texture in RenderTarget state");
            return false;
        }
        width = color->desc.width; height = color->desc.height; colorFormat = color->desc.format;
    }
    if (desc.depth) {
        const auto* depth = impl_->texture(desc.depth->texture, error); if (!depth) return false;
        if (!has_usage(depth->desc.usage, TextureUsage::DepthStencil) ||
            depth->desc.format != TextureFormat::D32Float ||
            recorded_texture_state(desc.depth->texture, depth->state) !=
                ResourceState::DepthWrite ||
            (width != 0U && (depth->desc.width != width || depth->desc.height != height)) ||
            !std::isfinite(desc.depth->clearDepth) || desc.depth->clearDepth < 0.0F ||
            desc.depth->clearDepth > 1.0F) {
            set_error(error, "Vulkan depth attachment is invalid or not in DepthWrite state");
            return false;
        }
        if (width == 0U) { width = depth->desc.width; height = depth->desc.height; }
    }
    list->renderPassOpen = true; list->graphicsPipeline = {}; list->vertexBuffer = {};
    list->vertexStride = 0U;
    list->indexBuffer = {}; list->viewportSet = false; list->scissorSet = false;
    list->graphicsBindGroups.clear();
    list->activeColorFormat = colorFormat;
    list->activeDepthFormat = desc.depth ? std::optional<TextureFormat>{TextureFormat::D32Float}
                                        : std::nullopt;
    list->activeDepthAttachment = desc.depth.has_value();
    list->activePassWidth = width; list->activePassHeight = height;
    list->commands.emplace_back(Impl::BeginRenderPassCommand{desc});
    return true;
}

bool VulkanDevice::end_render_pass(CommandListHandle commands, std::string* error) {
    auto* list = impl_->command(commands, error); if (!list) return false;
    if (!list->renderPassOpen) { set_error(error, "no Vulkan render pass is open"); return false; }
    list->renderPassOpen = false;
    list->activeDepthAttachment = false; list->activePassWidth = 0U; list->activePassHeight = 0U;
    list->commands.emplace_back(Impl::EndRenderPassCommand{});
    return true;
}

bool VulkanDevice::bind_graphics_pipeline(CommandListHandle commands,
                                          GraphicsPipelineHandle handle,
                                          std::string* error) {
    auto* list = impl_->command(commands, error);
    const auto* pipeline = impl_->graphics_pipeline(handle, error);
    if (!list || !pipeline) return false;
    if (!list->renderPassOpen || pipeline->desc.colorFormat != list->activeColorFormat ||
        pipeline->desc.depthFormat != list->activeDepthFormat) {
        set_error(error, "Vulkan graphics pipeline is incompatible with the active render pass");
        return false;
    }
    list->graphicsPipeline = handle;
    list->commands.emplace_back(Impl::BindPipelineCommand{handle});
    return true;
}

bool VulkanDevice::set_viewport(CommandListHandle commands, const Viewport& viewport,
                                std::string* error) {
    auto* list = impl_->command(commands, error); if (!list) return false;
    if (!list->renderPassOpen || !std::isfinite(viewport.x) || !std::isfinite(viewport.y) ||
        !std::isfinite(viewport.width) || !std::isfinite(viewport.height) ||
        !std::isfinite(viewport.minimumDepth) || !std::isfinite(viewport.maximumDepth) ||
        viewport.width <= 0.0F || viewport.height <= 0.0F || viewport.minimumDepth < 0.0F ||
        viewport.maximumDepth > 1.0F || viewport.minimumDepth > viewport.maximumDepth) {
        set_error(error, "Vulkan viewport is invalid or outside a render pass"); return false;
    }
    list->viewportSet = true;
    list->commands.emplace_back(Impl::ViewportCommand{viewport});
    return true;
}

bool VulkanDevice::set_scissor(CommandListHandle commands, const ScissorRect& scissor,
                               std::string* error) {
    auto* list = impl_->command(commands, error); if (!list) return false;
    if (!list->renderPassOpen || scissor.width == 0U || scissor.height == 0U) {
        set_error(error, "Vulkan scissor is invalid or outside a render pass"); return false;
    }
    list->scissorSet = true;
    list->commands.emplace_back(Impl::ScissorCommand{scissor});
    return true;
}

bool VulkanDevice::clear_depth_region(CommandListHandle commands, float depthValue,
                                      const ScissorRect& region, std::string* error) {
    auto* list = impl_->command(commands, error); if (!list) return false;
    const auto right = static_cast<std::int64_t>(region.x) + region.width;
    const auto bottom = static_cast<std::int64_t>(region.y) + region.height;
    if (!list->renderPassOpen || !list->activeDepthAttachment ||
        !std::isfinite(depthValue) || depthValue < 0.0F || depthValue > 1.0F ||
        region.x < 0 || region.y < 0 || region.width == 0U || region.height == 0U ||
        right > static_cast<std::int64_t>(list->activePassWidth) ||
        bottom > static_cast<std::int64_t>(list->activePassHeight)) {
        set_error(error, "Vulkan depth clear region is invalid or the active pass has no depth attachment");
        return false;
    }
    list->commands.emplace_back(Impl::ClearDepthRegionCommand{depthValue, region});
    return true;
}

bool VulkanDevice::bind_vertex_buffer(CommandListHandle commands, std::uint32_t slotIndex,
                                      BufferHandle handle, std::size_t offset,
                                      std::uint32_t stride, std::string* error) {
    auto* list = impl_->command(commands, error);
    const auto* buffer = impl_->buffer(handle, error);
    if (!list || !buffer) return false;
    if (!list->renderPassOpen || slotIndex != 0U || stride == 0U ||
        offset >= buffer->desc.bytes || !has_usage(buffer->desc.usage, BufferUsage::Vertex)) {
        set_error(error, "Vulkan vertex-buffer binding is invalid"); return false;
    }
    list->vertexBuffer = handle;
    list->vertexStride = stride;
    list->commands.emplace_back(Impl::VertexBufferCommand{handle, offset});
    return true;
}

bool VulkanDevice::bind_index_buffer(CommandListHandle commands, BufferHandle handle,
                                     std::size_t offset, IndexFormat format,
                                     std::string* error) {
    auto* list = impl_->command(commands, error);
    const auto* buffer = impl_->buffer(handle, error);
    const std::size_t alignment = format == IndexFormat::Uint16 ? 2U : 4U;
    if (!list || !buffer) return false;
    if (!list->renderPassOpen || offset >= buffer->desc.bytes || offset % alignment != 0U ||
        !has_usage(buffer->desc.usage, BufferUsage::Index)) {
        set_error(error, "Vulkan index-buffer binding is invalid"); return false;
    }
    list->indexBuffer = handle; list->indexOffset = offset; list->indexFormat = format;
    list->commands.emplace_back(Impl::IndexBufferCommand{handle, offset, format});
    return true;
}

bool VulkanDevice::bind_compute_bind_group(CommandListHandle commands, std::uint32_t index,
                                           BindGroupHandle handle, std::string* error) {
    auto* list = impl_->command(commands, error);
    if (!list || !impl_->bind_group(handle, error)) return false;
    if (list->renderPassOpen || list->queue == QueueKind::Copy || index >= 8U) {
        set_error(error, "Vulkan compute bind-group binding is invalid"); return false;
    }
    if (list->computeBindGroups.size() <= index) list->computeBindGroups.resize(index + 1U);
    list->computeBindGroups[index] = handle;
    list->commands.emplace_back(Impl::BindComputeGroupCommand{index, handle});
    return true;
}

bool VulkanDevice::bind_graphics_bind_group(CommandListHandle commands, std::uint32_t index,
                                            BindGroupHandle handle, std::string* error) {
    auto* list = impl_->command(commands, error);
    const auto* group = impl_->bind_group(handle, error);
    if (!list || !group) return false;
    if (!list->renderPassOpen || !list->graphicsPipeline || index >= 8U) {
        set_error(error, "Vulkan graphics bind-group binding is invalid"); return false;
    }
    const auto* pipeline = impl_->graphics_pipeline(list->graphicsPipeline, error);
    if (!pipeline || index >= pipeline->desc.bindGroupLayouts.size() ||
        group->desc.layout != pipeline->desc.bindGroupLayouts[index]) {
        set_error(error, "Vulkan graphics bind group is incompatible with the bound pipeline");
        return false;
    }
    if (list->graphicsBindGroups.size() <= index) list->graphicsBindGroups.resize(index + 1U);
    list->graphicsBindGroups[index] = handle;
    list->commands.emplace_back(Impl::BindGraphicsGroupCommand{index, handle});
    return true;
}

bool VulkanDevice::draw_indexed(CommandListHandle commands, std::uint32_t indexCount,
                                std::uint32_t instanceCount, std::uint32_t firstIndex,
                                std::int32_t vertexOffset, std::uint32_t firstInstance,
                                std::string* error) {
    auto* list = impl_->command(commands, error); if (!list) return false;
    const auto* indexBuffer = impl_->buffer(list->indexBuffer, error);
    const auto* pipeline = impl_->graphics_pipeline(list->graphicsPipeline, error);
    if (!list->renderPassOpen || !list->graphicsPipeline || !pipeline || !list->vertexBuffer ||
        !list->indexBuffer || !list->viewportSet || !list->scissorSet || !indexBuffer ||
        (pipeline->desc.vertexBuffer && list->vertexStride != pipeline->desc.vertexBuffer->stride) ||
        list->graphicsBindGroups.size() < pipeline->desc.bindGroupLayouts.size() ||
        indexCount == 0U || instanceCount == 0U) {
        set_error(error, "Vulkan indexed draw is missing required render state"); return false;
    }
    const std::size_t indexBytes = list->indexFormat == IndexFormat::Uint16 ? 2U : 4U;
    const std::uint64_t endIndex = static_cast<std::uint64_t>(firstIndex) + indexCount;
    const std::uint64_t endBytes = static_cast<std::uint64_t>(list->indexOffset) +
                                   endIndex * indexBytes;
    if (endBytes > indexBuffer->desc.bytes) {
        set_error(error, "Vulkan indexed draw exceeds the bound index buffer"); return false;
    }
    list->commands.emplace_back(Impl::DrawIndexedCommand{
        indexCount, instanceCount, firstIndex, vertexOffset, firstInstance});
    return true;
}

bool VulkanDevice::begin_debug_label(CommandListHandle commands, std::string_view label,
                                     std::string* error) {
    auto* list = impl_->command(commands, error);
    if (!list) return false;
    if (label.empty()) { set_error(error, "Vulkan debug label cannot be empty"); return false; }
    ++list->debugDepth;
    return true;
}
bool VulkanDevice::end_debug_label(CommandListHandle commands, std::string* error) {
    auto* list = impl_->command(commands, error);
    if (!list) return false;
    if (list->debugDepth == 0U) { set_error(error, "Vulkan debug-label stack underflow"); return false; }
    --list->debugDepth;
    return true;
}
TimestampQueryPoolHandle VulkanDevice::create_timestamp_query_pool(
    std::uint32_t count, std::string_view debugName, std::string* error) {
    if (!impl_->ready(error)) return {};
    if (count == 0U) {
        set_error(error, "Vulkan timestamp query count must be nonzero");
        return {};
    }
    if (!impl_->capabilities.timestampQueries) {
        set_error(error, "selected Vulkan queue family does not expose timestamp bits");
        return {};
    }
    const auto slotIndex = allocate_slot(impl_->timestampPools);
    auto& slot = impl_->timestampPools[slotIndex];
    const vk::QueryPoolCreateInfo info{
        vk::StructureTypeQueryPoolCreateInfo, nullptr, 0U, vk::QueryTypeTimestamp, count, 0U};
    if (!impl_->check(impl_->fn.createQueryPool(impl_->device, &info, nullptr, &slot.pool),
                      "vkCreateQueryPool", error)) {
        slot.pool = {};
        return {};
    }
    slot.alive = true;
    slot.count = count;
    slot.debugName.assign(debugName);
    return {slotIndex, slot.generation};
}
bool VulkanDevice::destroy_timestamp_query_pool(TimestampQueryPoolHandle handle,
                                                 std::string* error) {
    auto* slot = impl_->timestamp_pool(handle, error);
    if (!slot) return false;
    impl_->fn.destroyQueryPool(impl_->device, slot->pool, nullptr);
    slot->pool = {};
    slot->count = 0U;
    slot->debugName.clear();
    retire_slot(*slot);
    return true;
}
bool VulkanDevice::write_timestamp(CommandListHandle commands, TimestampQueryPoolHandle pool,
                                   std::uint32_t index, std::string* error) {
    auto* list = impl_->command(commands, error);
    const auto* query = impl_->timestamp_pool(pool, error);
    if (!list || !query) return false;
    if (index >= query->count) {
        set_error(error, "Vulkan timestamp index exceeds query pool");
        return false;
    }
    for (const auto& recorded : list->commands) {
        if (const auto* timestamp = std::get_if<Impl::TimestampCommand>(&recorded);
            timestamp && timestamp->pool == pool && timestamp->index == index) {
            set_error(error, "Vulkan timestamp index may be written only once per command list");
            return false;
        }
    }
    list->commands.emplace_back(Impl::TimestampCommand{pool, index});
    return true;
}
bool VulkanDevice::resolve_timestamps(TimestampQueryPoolHandle pool, std::uint32_t first,
                                      std::span<std::uint64_t> destination, std::string* error) {
    const auto* query = impl_->timestamp_pool(pool, error);
    if (!query) return false;
    if (first > query->count || destination.size() > query->count - first) {
        set_error(error, "Vulkan timestamp resolve range exceeds query pool");
        return false;
    }
    if (destination.empty()) return true;
    const auto bytes = destination.size_bytes();
    return impl_->check(impl_->fn.getQueryPoolResults(
                            impl_->device, query->pool, first,
                            static_cast<std::uint32_t>(destination.size()), bytes,
                            destination.data(), sizeof(std::uint64_t),
                            vk::QueryResult64Bit | vk::QueryResultWaitBit),
                        "vkGetQueryPoolResults", error);
}

FenceHandle VulkanDevice::submit(CommandListHandle commands, std::string* error) {
    if (!impl_->ready(error)) return {};
    auto* list = impl_->command(commands, error);
    if (!list) return {};
    if (list->debugDepth != 0U || list->renderPassOpen) {
        set_error(error, "unbalanced Vulkan debug labels or render pass at submission"); return {};
    }

    vk::CommandBuffer native{};
    const vk::CommandBufferAllocateInfo allocateInfo{
        vk::StructureTypeCommandBufferAllocateInfo, nullptr, impl_->commandPool,
        vk::CommandBufferLevelPrimary, 1U};
    if (!impl_->check(impl_->fn.allocateCommandBuffers(impl_->device, &allocateInfo, &native),
                      "vkAllocateCommandBuffers", error)) return {};
    const vk::CommandBufferBeginInfo beginInfo{
        vk::StructureTypeCommandBufferBeginInfo, nullptr,
        vk::CommandBufferUsageOneTimeSubmitBit, nullptr};
    if (!impl_->check(impl_->fn.beginCommandBuffer(native, &beginInfo),
                      "vkBeginCommandBuffer", error)) {
        impl_->fn.freeCommandBuffers(impl_->device, impl_->commandPool, 1U, &native); return {};
    }

    struct NativePass {
        vk::RenderPass renderPass{};
        vk::Framebuffer framebuffer{};
        std::vector<vk::ImageView> views;
    };
    std::vector<NativePass> nativePasses;
    bool nativePassOpen{};
    std::vector<BindGroupHandle> activeComputeGroups;
    std::vector<BindGroupHandle> activeGraphicsGroups;
    GraphicsPipelineHandle activeGraphicsPipeline{};
    bool recorded = true;
    for (const auto& command : list->commands) {
        const auto* timestamp = std::get_if<Impl::TimestampCommand>(&command);
        if (!timestamp) continue;
        const auto* pool = impl_->timestamp_pool(timestamp->pool, error);
        if (!pool) { recorded = false; break; }
        impl_->fn.cmdResetQueryPool(native, pool->pool, timestamp->index, 1U);
    }
    for (const auto& command : list->commands) {
        if (!recorded) break;
        recorded = std::visit([&](const auto& item) -> bool {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, Impl::CopyCommand>) {
                const auto* src = impl_->buffer(item.source, error);
                const auto* dst = impl_->buffer(item.destination, error);
                if (!src || !dst) return false;
                const vk::BufferCopy copy{item.sourceOffset, item.destinationOffset, item.bytes};
                impl_->fn.cmdCopyBuffer(native, src->buffer, dst->buffer, 1U, &copy);
                return true;
            } else if constexpr (std::is_same_v<T, Impl::BarrierCommand>) {
                auto* resource = impl_->buffer(item.buffer, error);
                if (!resource) return false;
                if (resource->state != item.before) {
                    set_error(error, "Vulkan buffer barrier before-state mismatch"); return false;
                }
                const StageAccess before = stage_access(item.before);
                const StageAccess after = stage_access(item.after);
                const vk::BufferMemoryBarrier barrier{
                    vk::StructureTypeBufferMemoryBarrier, nullptr,
                    before.access, after.access, vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                    resource->buffer, 0U, vk::WholeSize};
                impl_->fn.cmdPipelineBarrier(native, before.stage, after.stage, 0U,
                                             0U, nullptr, 1U, &barrier, 0U, nullptr);
                resource->state = item.after;
                return true;
            } else if constexpr (std::is_same_v<T, Impl::TextureBarrierCommand>) {
                auto* texture = impl_->texture(item.texture, error);
                return texture && impl_->image_barrier(native, *texture, item.before, item.after, error);
            } else if constexpr (std::is_same_v<T, Impl::BeginRenderPassCommand>) {
                if (nativePassOpen || item.desc.colors.size() > 1U ||
                    (item.desc.colors.empty() && !item.desc.depth)) {
                    set_error(error, "invalid nested Vulkan render pass"); return false;
                }
                const Impl::TextureSlot* color = nullptr;
                const Impl::TextureSlot* depth = nullptr;
                if (!item.desc.colors.empty()) {
                    color = impl_->texture(item.desc.colors.front().texture, error);
                    if (!color) return false;
                }
                if (item.desc.depth) {
                    depth = impl_->texture(item.desc.depth->texture, error);
                    if (!depth) return false;
                }
                const std::uint32_t width = color ? color->desc.width : depth->desc.width;
                const std::uint32_t height = color ? color->desc.height : depth->desc.height;
                NativePass pass;
                pass.views.resize((color ? 1U : 0U) + (depth ? 1U : 0U));
                std::size_t viewIndex = 0U;
                if (color) {
                    if (!impl_->create_native_view(*color, 0U, 1U, 0U, 1U,
                                                   TextureViewDimension::Texture2D,
                                                   pass.views[viewIndex++], error)) return false;
                }
                if (depth) {
                    if (!impl_->create_native_view(*depth, 0U, 1U, 0U, 1U,
                                                   TextureViewDimension::Texture2D,
                                                   pass.views[viewIndex], error)) {
                        for (auto view : pass.views) if (view)
                            impl_->fn.destroyImageView(impl_->device, view, nullptr);
                        return false;
                    }
                }
                const std::optional<TextureFormat> colorFormat = color
                    ? std::optional<TextureFormat>{color->desc.format} : std::nullopt;
                const std::optional<TextureFormat> depthFormat = depth
                    ? std::optional<TextureFormat>{depth->desc.format} : std::nullopt;
                if (!impl_->create_compatible_render_pass(
                        colorFormat, depthFormat,
                        color ? item.desc.colors.front().clear : false,
                        depth ? item.desc.depth->clear : false,
                        pass.renderPass, error)) {
                    for (auto view : pass.views)
                        if (view) impl_->fn.destroyImageView(impl_->device, view, nullptr);
                    return false;
                }
                const vk::FramebufferCreateInfo framebufferInfo{
                    vk::StructureTypeFramebufferCreateInfo, nullptr, 0U, pass.renderPass,
                    static_cast<std::uint32_t>(pass.views.size()), pass.views.data(),
                    width, height, 1U};
                if (!impl_->check(impl_->fn.createFramebuffer(impl_->device, &framebufferInfo,
                                                              nullptr, &pass.framebuffer),
                                  "vkCreateFramebuffer", error)) {
                    impl_->fn.destroyRenderPass(impl_->device, pass.renderPass, nullptr);
                    for (auto view : pass.views)
                        if (view) impl_->fn.destroyImageView(impl_->device, view, nullptr);
                    return false;
                }
                std::array<vk::ClearValue, 2> clearValues{};
                std::uint32_t clearCount = 0U;
                if (color) {
                    clearValues[clearCount].color.float32[0] = item.desc.colors.front().clearR;
                    clearValues[clearCount].color.float32[1] = item.desc.colors.front().clearG;
                    clearValues[clearCount].color.float32[2] = item.desc.colors.front().clearB;
                    clearValues[clearCount].color.float32[3] = item.desc.colors.front().clearA;
                    ++clearCount;
                }
                if (depth) {
                    clearValues[clearCount].depthStencil.depth = item.desc.depth->clearDepth;
                    clearValues[clearCount].depthStencil.stencil = 0U;
                    ++clearCount;
                }
                const vk::RenderPassBeginInfo passBegin{
                    vk::StructureTypeRenderPassBeginInfo, nullptr, pass.renderPass,
                    pass.framebuffer, {{0, 0}, {width, height}},
                    clearCount, clearValues.data()};
                impl_->fn.cmdBeginRenderPass(native, &passBegin, vk::SubpassContentsInline);
                nativePasses.push_back(std::move(pass));
                nativePassOpen = true;
                activeGraphicsPipeline = {};
                activeGraphicsGroups.clear();
                return true;
            } else if constexpr (std::is_same_v<T, Impl::EndRenderPassCommand>) {
                if (!nativePassOpen) { set_error(error, "Vulkan render-pass end without begin"); return false; }
                impl_->fn.cmdEndRenderPass(native); nativePassOpen = false;
                activeGraphicsPipeline = {}; activeGraphicsGroups.clear(); return true;
            } else if constexpr (std::is_same_v<T, Impl::BindPipelineCommand>) {
                const auto* pipeline = impl_->graphics_pipeline(item.pipeline, error);
                if (!pipeline || !nativePassOpen) return false;
                impl_->fn.cmdBindPipeline(native, vk::PipelineBindPointGraphics,
                                          pipeline->pipeline);
                activeGraphicsPipeline = item.pipeline;
                activeGraphicsGroups.clear();
                return true;
            } else if constexpr (std::is_same_v<T, Impl::BindComputeGroupCommand>) {
                if (nativePassOpen || !impl_->bind_group(item.group, error)) return false;
                if (activeComputeGroups.size() <= item.index)
                    activeComputeGroups.resize(item.index + 1U);
                activeComputeGroups[item.index] = item.group;
                return true;
            } else if constexpr (std::is_same_v<T, Impl::BindGraphicsGroupCommand>) {
                const auto* pipeline = impl_->graphics_pipeline(activeGraphicsPipeline, error);
                const auto* group = impl_->bind_group(item.group, error);
                if (!pipeline || !group || !nativePassOpen ||
                    item.index >= pipeline->desc.bindGroupLayouts.size() ||
                    group->desc.layout != pipeline->desc.bindGroupLayouts[item.index]) return false;
                if (activeGraphicsGroups.size() <= item.index)
                    activeGraphicsGroups.resize(item.index + 1U);
                activeGraphicsGroups[item.index] = item.group;
                impl_->fn.cmdBindDescriptorSets(native, vk::PipelineBindPointGraphics,
                                                pipeline->layout, item.index, 1U, &group->set,
                                                0U, nullptr);
                return true;
            } else if constexpr (std::is_same_v<T, Impl::DispatchCommand>) {
                const auto* pipeline = impl_->compute_pipeline(item.pipeline, error);
                if (!pipeline || nativePassOpen ||
                    activeComputeGroups.size() < pipeline->desc.bindGroupLayouts.size()) return false;
                impl_->fn.cmdBindPipeline(native, vk::PipelineBindPointCompute, pipeline->pipeline);
                for (std::uint32_t index = 0U;
                     index < static_cast<std::uint32_t>(pipeline->desc.bindGroupLayouts.size());
                     ++index) {
                    const auto* group = impl_->bind_group(activeComputeGroups[index], error);
                    if (!group || group->desc.layout != pipeline->desc.bindGroupLayouts[index])
                        return false;
                    impl_->fn.cmdBindDescriptorSets(native, vk::PipelineBindPointCompute,
                                                    pipeline->layout, index, 1U, &group->set,
                                                    0U, nullptr);
                }
                impl_->fn.cmdDispatch(native, item.groupsX, item.groupsY, item.groupsZ);
                return true;
            } else if constexpr (std::is_same_v<T, Impl::ViewportCommand>) {
                const vk::Viewport viewport{item.viewport.x, item.viewport.y,
                    item.viewport.width, item.viewport.height,
                    item.viewport.minimumDepth, item.viewport.maximumDepth};
                impl_->fn.cmdSetViewport(native, 0U, 1U, &viewport); return true;
            } else if constexpr (std::is_same_v<T, Impl::ScissorCommand>) {
                const vk::Rect2D scissor{{item.scissor.x, item.scissor.y},
                                         {item.scissor.width, item.scissor.height}};
                impl_->fn.cmdSetScissor(native, 0U, 1U, &scissor); return true;
            } else if constexpr (std::is_same_v<T, Impl::ClearDepthRegionCommand>) {
                if (!nativePassOpen) return false;
                vk::ClearAttachment attachment{};
                attachment.aspectMask = vk::ImageAspectDepthBit;
                attachment.clearValue.depthStencil.depth = item.depth;
                attachment.clearValue.depthStencil.stencil = 0U;
                const vk::ClearRect rect{{{item.region.x, item.region.y},
                                          {item.region.width, item.region.height}}, 0U, 1U};
                impl_->fn.cmdClearAttachments(native, 1U, &attachment, 1U, &rect);
                return true;
            } else if constexpr (std::is_same_v<T, Impl::VertexBufferCommand>) {
                const auto* buffer = impl_->buffer(item.buffer, error); if (!buffer) return false;
                const vk::DeviceSize offset = item.offset;
                impl_->fn.cmdBindVertexBuffers(native, 0U, 1U, &buffer->buffer, &offset);
                return true;
            } else if constexpr (std::is_same_v<T, Impl::IndexBufferCommand>) {
                const auto* buffer = impl_->buffer(item.buffer, error); if (!buffer) return false;
                impl_->fn.cmdBindIndexBuffer(native, buffer->buffer, item.offset,
                                             vulkan_index_type(item.format));
                return true;
            } else if constexpr (std::is_same_v<T, Impl::DrawIndexedCommand>) {
                const auto* pipeline = impl_->graphics_pipeline(activeGraphicsPipeline, error);
                if (!pipeline || activeGraphicsGroups.size() < pipeline->desc.bindGroupLayouts.size())
                    return false;
                impl_->fn.cmdDrawIndexed(native, item.indexCount, item.instanceCount,
                                         item.firstIndex, item.vertexOffset,
                                         item.firstInstance);
                return true;
            } else if constexpr (std::is_same_v<T, Impl::TimestampCommand>) {
                const auto* pool = impl_->timestamp_pool(item.pool, error);
                if (!pool || nativePassOpen) return false;
                impl_->fn.cmdWriteTimestamp(native, vk::PipelineStageBottomOfPipeBit,
                                            pool->pool, item.index);
                return true;
            }
            return false;
        }, command);
        if (!recorded) break;
    }
    if (recorded && nativePassOpen) {
        set_error(error, "Vulkan native render pass remained open"); recorded = false;
    }
    if (recorded)
        recorded = impl_->check(impl_->fn.endCommandBuffer(native), "vkEndCommandBuffer", error);
    if (recorded) recorded = impl_->submit_native(native, error);
    impl_->fn.freeCommandBuffers(impl_->device, impl_->commandPool, 1U, &native);
    for (auto& pass : nativePasses) {
        if (pass.framebuffer) impl_->fn.destroyFramebuffer(impl_->device, pass.framebuffer, nullptr);
        if (pass.renderPass) impl_->fn.destroyRenderPass(impl_->device, pass.renderPass, nullptr);
        for (auto view : pass.views)
            if (view) impl_->fn.destroyImageView(impl_->device, view, nullptr);
    }
    if (!recorded) return {};

    for (const auto& command : list->commands) {
        if (std::holds_alternative<Impl::CopyCommand>(command)) ++impl_->statistics.copiesExecuted;
        else if (std::holds_alternative<Impl::BarrierCommand>(command) ||
                 std::holds_alternative<Impl::TextureBarrierCommand>(command))
            ++impl_->statistics.barriersExecuted;
        else if (std::holds_alternative<Impl::DispatchCommand>(command))
            ++impl_->statistics.dispatchesExecuted;
        else if (std::holds_alternative<Impl::BeginRenderPassCommand>(command))
            ++impl_->statistics.renderPassesExecuted;
        else if (const auto* draw = std::get_if<Impl::DrawIndexedCommand>(&command)) {
            ++impl_->statistics.indexedDrawsExecuted;
            impl_->statistics.indexedTrianglesSubmitted +=
                static_cast<std::uint64_t>(draw->indexCount / 3U) * draw->instanceCount;
        }
    }
    const FenceHandle fence{impl_->nextFence++};
    impl_->completedFence = fence.value;
    ++impl_->statistics.commandListsSubmitted;
    list->commands.clear();
    retire_slot(*list);
    return fence;
}

bool VulkanDevice::fence_complete(FenceHandle fence) const noexcept {
    return fence && fence.value <= impl_->completedFence;
}
bool VulkanDevice::wait(FenceHandle fence, std::string* error) {
    if (!impl_->ready(error)) return false;
    if (!fence || fence.value >= impl_->nextFence) { set_error(error, "unknown Vulkan fence"); return false; }
    return fence_complete(fence);
}
void VulkanDevice::wait_idle() noexcept {
    if (impl_->device && impl_->fn.deviceWaitIdle) {
        if (impl_->fn.deviceWaitIdle(impl_->device) == vk::ErrorDeviceLost) {
            impl_->status = DeviceStatus::Lost;
            impl_->reason = "vkDeviceWaitIdle reported device loss";
        }
    }
    impl_->completedFence = impl_->nextFence > 0U ? impl_->nextFence - 1U : 0U;
}

} // namespace dve::rhi
