#include "dve/rhi/null_device.hpp"

#include <algorithm>
#include <limits>
#include <cmath>
#include <cstring>
#include <optional>
#include <type_traits>
#include <utility>

namespace dve::rhi {
namespace {
template <class Slot>
std::uint32_t allocate_slot(std::vector<Slot>& slots) {
    for (std::uint32_t index = 0; index < slots.size(); ++index)
        if (!slots[index].alive) return index;
    slots.emplace_back();
    return static_cast<std::uint32_t>(slots.size() - 1U);
}

template <class Slot>
void retire_slot(Slot& slot) {
    slot.alive = false;
    if (slot.generation == std::numeric_limits<std::uint32_t>::max()) slot.generation = 1U;
    else ++slot.generation;
}

bool range_fits(std::size_t offset, std::size_t bytes, std::size_t capacity) noexcept {
    return offset <= capacity && bytes <= capacity - offset;
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

std::optional<std::size_t> texture_layer_bytes(const TextureDesc& desc) noexcept {
    std::uint64_t total = 0U;
    for (std::uint32_t mip = 0; mip < desc.mipLevels; ++mip) {
        const std::uint64_t width = mip_dimension(desc.width, mip);
        const std::uint64_t height = mip_dimension(desc.height, mip);
        const std::uint64_t depth = desc.dimension == TextureDimension::Texture3D
            ? mip_dimension(desc.depth, mip) : desc.depth;
        const std::uint64_t bytes = width * height * depth * texture_pixel_bytes(desc.format);
        if (bytes > std::numeric_limits<std::size_t>::max() - total) return std::nullopt;
        total += bytes;
    }
    return static_cast<std::size_t>(total);
}

std::optional<std::size_t> texture_storage_bytes(const TextureDesc& desc) noexcept {
    const auto layer = texture_layer_bytes(desc);
    if (!layer || desc.arrayLayers > std::numeric_limits<std::size_t>::max() / *layer)
        return std::nullopt;
    return *layer * desc.arrayLayers;
}

std::optional<std::size_t> texture_subresource_offset(const TextureDesc& desc,
                                                      std::uint32_t mip,
                                                      std::uint32_t layer) noexcept {
    const auto layerBytes = texture_layer_bytes(desc);
    if (!layerBytes || mip >= desc.mipLevels || layer >= desc.arrayLayers) return std::nullopt;
    std::uint64_t offset = static_cast<std::uint64_t>(*layerBytes) * layer;
    for (std::uint32_t index = 0; index < mip; ++index) {
        offset += static_cast<std::uint64_t>(mip_dimension(desc.width, index)) *
                  mip_dimension(desc.height, index) *
                  (desc.dimension == TextureDimension::Texture3D
                       ? mip_dimension(desc.depth, index) : desc.depth) * texture_pixel_bytes(desc.format);
    }
    if (offset > std::numeric_limits<std::size_t>::max()) return std::nullopt;
    return static_cast<std::size_t>(offset);
}
}

TextureFormatCapabilities NullDevice::texture_format_capabilities(TextureFormat format) const noexcept {
    if (format == TextureFormat::D32Float) return {false, false, false, false, true};
    const bool integerAtomic = format == TextureFormat::R32Uint || format == TextureFormat::R32Sint;
    return {true, true, integerAtomic, true, false};
}

NullDevice::NullDevice() {
    capabilities_.backend = Backend::Null;
    capabilities_.adapterName = "DVE deterministic Null RHI";
    capabilities_.adapterClass = AdapterClass::Cpu;
    capabilities_.softwareAdapter = true;
    capabilities_.apiVersion = 1U;
}

void NullDevice::set_error(std::string* error, std::string_view message) {
    if (error != nullptr) error->assign(message.begin(), message.end());
}
bool NullDevice::ready(std::string* error) const {
    if (status_ == DeviceStatus::Ready) return true;
    set_error(error, lossReason_.empty() ? "device is lost" : lossReason_);
    return false;
}

template <class HandleType, class SlotType>
SlotType* lookup_slot(HandleType handle, std::vector<SlotType>& slots,
                      std::string_view label, std::string* error) {
    if (!handle || handle.index >= slots.size()) {
        if (error != nullptr) *error = std::string("invalid ") + std::string(label) + " handle";
        return nullptr;
    }
    SlotType& slot = slots[handle.index];
    if (!slot.alive || slot.generation != handle.generation) {
        if (error != nullptr) *error = std::string("stale ") + std::string(label) + " handle";
        return nullptr;
    }
    return &slot;
}

NullDevice::BufferSlot* NullDevice::buffer(BufferHandle handle, std::string* error) {
    return lookup_slot(handle, buffers_, "buffer", error);
}
NullDevice::TextureSlot* NullDevice::texture(TextureHandle handle, std::string* error) {
    return lookup_slot(handle, textures_, "texture", error);
}
NullDevice::TextureViewSlot* NullDevice::texture_view(TextureViewHandle handle, std::string* error) {
    return lookup_slot(handle, textureViews_, "texture-view", error);
}
NullDevice::SamplerSlot* NullDevice::sampler(SamplerHandle handle, std::string* error) {
    return lookup_slot(handle, samplers_, "sampler", error);
}
NullDevice::BindGroupLayoutSlot* NullDevice::bind_group_layout(BindGroupLayoutHandle handle, std::string* error) {
    return lookup_slot(handle, bindGroupLayouts_, "bind-group-layout", error);
}
const NullDevice::BindGroupLayoutSlot* NullDevice::bind_group_layout(BindGroupLayoutHandle handle, std::string* error) const {
    if (!handle || handle.index >= bindGroupLayouts_.size()) { set_error(error, "invalid bind-group-layout handle"); return nullptr; }
    const auto& slot = bindGroupLayouts_[handle.index];
    if (!slot.alive || slot.generation != handle.generation) { set_error(error, "stale bind-group-layout handle"); return nullptr; }
    return &slot;
}
NullDevice::BindGroupSlot* NullDevice::bind_group(BindGroupHandle handle, std::string* error) {
    return lookup_slot(handle, bindGroups_, "bind-group", error);
}
NullDevice::PipelineSlot* NullDevice::pipeline(ComputePipelineHandle handle, std::string* error) {
    return lookup_slot(handle, pipelines_, "compute-pipeline", error);
}
NullDevice::GraphicsPipelineSlot* NullDevice::graphics_pipeline(GraphicsPipelineHandle handle,
                                                                   std::string* error) {
    return lookup_slot(handle, graphicsPipelines_, "graphics-pipeline", error);
}
NullDevice::SwapchainSlot* NullDevice::swapchain(SwapchainHandle handle, std::string* error) {
    return lookup_slot(handle, swapchains_, "swapchain", error);
}
NullDevice::TimestampPoolSlot* NullDevice::timestamp_pool(TimestampQueryPoolHandle handle, std::string* error) {
    return lookup_slot(handle, timestampPools_, "timestamp-pool", error);
}
NullDevice::CommandSlot* NullDevice::command_list(CommandListHandle handle, std::string* error) {
    return lookup_slot(handle, commandLists_, "command-list", error);
}

const NullDevice::BufferSlot* NullDevice::buffer(BufferHandle handle, std::string* error) const {
    if (!handle || handle.index >= buffers_.size()) { set_error(error, "invalid buffer handle"); return nullptr; }
    const auto& slot = buffers_[handle.index];
    if (!slot.alive || slot.generation != handle.generation) { set_error(error, "stale buffer handle"); return nullptr; }
    return &slot;
}
const NullDevice::TextureSlot* NullDevice::texture(TextureHandle handle, std::string* error) const {
    if (!handle || handle.index >= textures_.size()) { set_error(error, "invalid texture handle"); return nullptr; }
    const auto& slot = textures_[handle.index];
    if (!slot.alive || slot.generation != handle.generation) { set_error(error, "stale texture handle"); return nullptr; }
    return &slot;
}
const NullDevice::SamplerSlot* NullDevice::sampler(SamplerHandle handle, std::string* error) const {
    if (!handle || handle.index >= samplers_.size()) { set_error(error, "invalid sampler handle"); return nullptr; }
    const auto& slot = samplers_[handle.index];
    if (!slot.alive || slot.generation != handle.generation) { set_error(error, "stale sampler handle"); return nullptr; }
    return &slot;
}
const NullDevice::TimestampPoolSlot* NullDevice::timestamp_pool(TimestampQueryPoolHandle handle, std::string* error) const {
    if (!handle || handle.index >= timestampPools_.size()) { set_error(error, "invalid timestamp-pool handle"); return nullptr; }
    const auto& slot = timestampPools_[handle.index];
    if (!slot.alive || slot.generation != handle.generation) { set_error(error, "stale timestamp-pool handle"); return nullptr; }
    return &slot;
}

BufferHandle NullDevice::create_buffer(const BufferDesc& desc, std::string* error) {
    if (!ready(error)) return {};
    if (desc.bytes == 0U) { set_error(error, "buffer size must be nonzero"); return {}; }
    const auto index = allocate_slot(buffers_);
    auto& slot = buffers_[index];
    slot.alive = true; slot.desc = desc; slot.state = desc.initialState;
    slot.storage.assign(desc.bytes, std::byte{0});
    ++statistics_.buffersCreated;
    return {index, slot.generation};
}
bool NullDevice::destroy_buffer(BufferHandle handle, std::string* error) {
    auto* slot = buffer(handle, error); if (!slot) return false;
    for (const auto& group : bindGroups_) {
        if (!group.alive) continue;
        for (const auto& entry : group.desc.entries)
            if (entry.buffer == handle) { set_error(error, "buffer is retained by a live bind group"); return false; }
    }
    slot->desc = {}; slot->storage.clear(); retire_slot(*slot); ++statistics_.buffersDestroyed; return true;
}
bool NullDevice::write_buffer(BufferHandle handle, std::size_t offset, std::span<const std::byte> bytes, std::string* error) {
    if (!ready(error)) return false;
    auto* slot = buffer(handle, error); if (!slot) return false;
    if (slot->desc.memory == MemoryDomain::Readback) { set_error(error, "cannot upload directly into a readback buffer"); return false; }
    if (!range_fits(offset, bytes.size(), slot->storage.size())) { set_error(error, "buffer upload range exceeds allocation"); return false; }
    std::copy(bytes.begin(), bytes.end(), slot->storage.begin() + static_cast<std::ptrdiff_t>(offset));
    statistics_.uploadedBytes += bytes.size(); return true;
}
bool NullDevice::read_buffer(BufferHandle handle, std::size_t offset, std::span<std::byte> destination, std::string* error) {
    if (!ready(error)) return false;
    const auto* slot = buffer(handle, error); if (!slot) return false;
    if (!range_fits(offset, destination.size(), slot->storage.size())) { set_error(error, "buffer readback range exceeds allocation"); return false; }
    std::copy(slot->storage.begin() + static_cast<std::ptrdiff_t>(offset),
              slot->storage.begin() + static_cast<std::ptrdiff_t>(offset + destination.size()), destination.begin());
    statistics_.readbackBytes += destination.size(); return true;
}

bool NullDevice::write_texture(TextureHandle handle, std::uint32_t mipLevel,
                               std::uint32_t arrayLayer, std::span<const std::byte> bytes,
                               std::size_t rowPitchBytes, std::string* error) {
    if (!ready(error)) return false;
    auto* slot = texture(handle, error); if (!slot) return false;
    if (!has_usage(slot->desc.usage, TextureUsage::CopyDestination)) {
        set_error(error, "texture lacks CopyDestination usage"); return false;
    }
    const auto offset = texture_subresource_offset(slot->desc, mipLevel, arrayLayer);
    if (!offset) { set_error(error, "texture upload subresource is invalid"); return false; }
    const std::size_t width = mip_dimension(slot->desc.width, mipLevel);
    const std::size_t height = mip_dimension(slot->desc.height, mipLevel);
    const std::size_t depth = slot->desc.dimension == TextureDimension::Texture3D
        ? mip_dimension(slot->desc.depth, mipLevel) : slot->desc.depth;
    const std::size_t packedRow = width * texture_pixel_bytes(slot->desc.format);
    if (rowPitchBytes < packedRow || bytes.size() < rowPitchBytes * height * depth) {
        set_error(error, "texture upload row pitch or byte count is too small"); return false;
    }
    for (std::size_t z = 0; z < depth; ++z) {
        for (std::size_t y = 0; y < height; ++y) {
            const std::size_t sourceOffset = (z * height + y) * rowPitchBytes;
            const std::size_t destinationOffset = *offset + (z * height + y) * packedRow;
            std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(sourceOffset), packedRow,
                        slot->storage.begin() + static_cast<std::ptrdiff_t>(destinationOffset));
        }
    }
    statistics_.uploadedBytes += packedRow * height * depth;
    return true;
}

bool NullDevice::read_texture(TextureHandle handle, std::uint32_t mipLevel,
                              std::uint32_t arrayLayer, std::span<std::byte> destination,
                              std::size_t rowPitchBytes, std::string* error) {
    if (!ready(error)) return false;
    const auto* slot = texture(handle, error); if (!slot) return false;
    if (!has_usage(slot->desc.usage, TextureUsage::CopySource)) {
        set_error(error, "texture lacks CopySource usage"); return false;
    }
    const auto offset = texture_subresource_offset(slot->desc, mipLevel, arrayLayer);
    if (!offset) { set_error(error, "texture readback subresource is invalid"); return false; }
    const std::size_t width = mip_dimension(slot->desc.width, mipLevel);
    const std::size_t height = mip_dimension(slot->desc.height, mipLevel);
    const std::size_t depth = slot->desc.dimension == TextureDimension::Texture3D
        ? mip_dimension(slot->desc.depth, mipLevel) : slot->desc.depth;
    const std::size_t packedRow = width * texture_pixel_bytes(slot->desc.format);
    if (rowPitchBytes < packedRow || destination.size() < rowPitchBytes * height * depth) {
        set_error(error, "texture readback row pitch or byte count is too small"); return false;
    }
    for (std::size_t z = 0; z < depth; ++z) {
        for (std::size_t y = 0; y < height; ++y) {
            const std::size_t destinationOffset = (z * height + y) * rowPitchBytes;
            const std::size_t sourceOffset = *offset + (z * height + y) * packedRow;
            std::copy_n(slot->storage.begin() + static_cast<std::ptrdiff_t>(sourceOffset), packedRow,
                        destination.begin() + static_cast<std::ptrdiff_t>(destinationOffset));
        }
    }
    statistics_.readbackBytes += packedRow * height * depth;
    return true;
}

TextureHandle NullDevice::create_texture(const TextureDesc& desc, std::string* error) {
    if (!ready(error)) return {};
    if (desc.width == 0U || desc.height == 0U || desc.depth == 0U || desc.mipLevels == 0U || desc.arrayLayers == 0U) {
        set_error(error, "texture dimensions, mips, and layers must be nonzero"); return {};
    }
    if (desc.width > capabilities_.maxTextureDimension2D || desc.height > capabilities_.maxTextureDimension2D) {
        set_error(error, "texture exceeds device dimensions"); return {};
    }
    if (desc.dimension == TextureDimension::TextureCube &&
        (desc.width != desc.height || desc.depth != 1U || desc.arrayLayers != 6U)) {
        set_error(error, "cube texture requires square 2D faces and exactly six array layers"); return {};
    }
    if (desc.dimension == TextureDimension::Texture3D && desc.arrayLayers != 1U) {
        set_error(error, "3D textures cannot have array layers"); return {};
    }
    const auto storageBytes = texture_storage_bytes(desc);
    if (!storageBytes) { set_error(error, "texture storage size overflows address space"); return {}; }
    const auto index = allocate_slot(textures_); auto& slot = textures_[index];
    slot.alive = true; slot.desc = desc; slot.state = desc.initialState; slot.swapchainOwned = false;
    try { slot.storage.assign(*storageBytes, std::byte{0}); }
    catch (...) { slot = {}; set_error(error, "texture storage allocation failed"); return {}; }
    ++statistics_.texturesCreated; return {index, slot.generation};
}
bool NullDevice::destroy_texture_internal(TextureHandle handle, bool fromSwapchain, std::string* error) {
    auto* slot = texture(handle, error); if (!slot) return false;
    if (slot->swapchainOwned && !fromSwapchain) { set_error(error, "swapchain images are owned by their swapchain"); return false; }
    for (const auto& view : textureViews_)
        if (view.alive && view.desc.texture == handle) { set_error(error, "texture still has a live view"); return false; }
    slot->desc = {}; slot->state = ResourceState::Undefined; slot->swapchainOwned = false;
    slot->storage.clear();
    retire_slot(*slot); ++statistics_.texturesDestroyed; return true;
}
bool NullDevice::destroy_texture(TextureHandle handle, std::string* error) { return destroy_texture_internal(handle, false, error); }
TextureViewHandle NullDevice::create_texture_view(const TextureViewDesc& desc, std::string* error) {
    const auto* source = texture(desc.texture, error); if (!source) return {};
    if (desc.mipCount == 0U || desc.layerCount == 0U || desc.baseMip >= source->desc.mipLevels ||
        desc.mipCount > source->desc.mipLevels - desc.baseMip || desc.baseLayer >= source->desc.arrayLayers ||
        desc.layerCount > source->desc.arrayLayers - desc.baseLayer) {
        set_error(error, "texture-view range exceeds texture"); return {};
    }
    const TextureViewDimension dimension = desc.dimension == TextureViewDimension::Automatic
        ? (source->desc.dimension == TextureDimension::Texture3D ? TextureViewDimension::Texture3D
           : source->desc.dimension == TextureDimension::TextureCube ? TextureViewDimension::TextureCube
           : source->desc.arrayLayers > 1U ? TextureViewDimension::Texture2DArray
           : TextureViewDimension::Texture2D)
        : desc.dimension;
    if ((dimension == TextureViewDimension::Texture3D && source->desc.dimension != TextureDimension::Texture3D) ||
        (dimension == TextureViewDimension::TextureCube &&
            (source->desc.dimension != TextureDimension::TextureCube || desc.layerCount != 6U || desc.baseLayer != 0U)) ||
        (dimension == TextureViewDimension::Texture2D && desc.layerCount != 1U) ||
        (dimension == TextureViewDimension::Texture2DArray && source->desc.dimension == TextureDimension::Texture3D)) {
        set_error(error, "texture-view dimension is incompatible with the texture"); return {};
    }
    const auto index = allocate_slot(textureViews_); auto& slot = textureViews_[index];
    slot.alive = true; slot.desc = desc; slot.desc.dimension = dimension;
    ++statistics_.textureViewsCreated;
    return {index, slot.generation};
}
bool NullDevice::destroy_texture_view(TextureViewHandle handle, std::string* error) {
    auto* slot = texture_view(handle, error); if (!slot) return false;
    for (const auto& group : bindGroups_) {
        if (!group.alive) continue;
        for (const auto& entry : group.desc.entries)
            if (entry.textureView == handle) { set_error(error, "texture view is retained by a live bind group"); return false; }
    }
    slot->desc = {}; retire_slot(*slot); ++statistics_.textureViewsDestroyed; return true;
}

SamplerHandle NullDevice::create_sampler(const SamplerDesc& desc, std::string* error) {
    if (!ready(error)) return {};
    if (!std::isfinite(desc.minimumLod) || !std::isfinite(desc.maximumLod) ||
        !std::isfinite(desc.mipLodBias) || !std::isfinite(desc.maximumAnisotropy) ||
        desc.minimumLod > desc.maximumLod || desc.maximumAnisotropy < 1.0F ||
        desc.maximumAnisotropy > 16.0F || (!desc.anisotropy && desc.maximumAnisotropy != 1.0F) ||
        static_cast<std::uint8_t>(desc.comparisonOp) > static_cast<std::uint8_t>(CompareOp::AlwaysPass)) {
        set_error(error, "sampler LOD, anisotropy, or comparison settings are invalid"); return {};
    }
    const auto index = allocate_slot(samplers_); auto& slot = samplers_[index];
    slot.alive = true; slot.desc = desc; ++statistics_.samplersCreated;
    return {index, slot.generation};
}

bool NullDevice::destroy_sampler(SamplerHandle handle, std::string* error) {
    auto* slot = sampler(handle, error); if (!slot) return false;
    for (const auto& group : bindGroups_) {
        if (!group.alive) continue;
        for (const auto& entry : group.desc.entries)
            if (entry.sampler == handle) { set_error(error, "sampler is retained by a live bind group"); return false; }
    }
    slot->desc = {}; retire_slot(*slot); ++statistics_.samplersDestroyed; return true;
}

SwapchainHandle NullDevice::create_swapchain(const SwapchainDesc& desc, std::string* error) {
    if (!ready(error)) return {};
    if (desc.width == 0U || desc.height == 0U || desc.imageCount < 2U || desc.imageCount > 4U) {
        set_error(error, "swapchain requires positive dimensions and two to four images"); return {};
    }
    const auto index = allocate_slot(swapchains_); auto& slot = swapchains_[index];
    slot.alive = true; slot.desc = desc; slot.images.clear(); slot.nextImage = 0U; slot.acquired = false;
    for (std::uint32_t image = 0; image < desc.imageCount; ++image) {
        TextureDesc textureDesc;
        textureDesc.format = desc.format; textureDesc.width = desc.width; textureDesc.height = desc.height;
        textureDesc.usage = TextureUsage::RenderTarget | TextureUsage::Present;
        textureDesc.initialState = ResourceState::Present;
        textureDesc.debugName = desc.debugName + " image " + std::to_string(image);
        TextureHandle textureHandle = create_texture(textureDesc, error);
        if (!textureHandle) {
            for (TextureHandle made : slot.images) (void)destroy_texture_internal(made, true, nullptr);
            slot.images.clear(); retire_slot(slot); return {};
        }
        textures_[textureHandle.index].swapchainOwned = true;
        slot.images.push_back(textureHandle);
    }
    ++statistics_.swapchainsCreated; return {index, slot.generation};
}
bool NullDevice::destroy_swapchain(SwapchainHandle handle, std::string* error) {
    auto* slot = swapchain(handle, error); if (!slot) return false;
    for (TextureHandle image : slot->images)
        if (!destroy_texture_internal(image, true, error)) return false;
    slot->images.clear(); slot->desc = {}; slot->acquired = false; retire_slot(*slot);
    ++statistics_.swapchainsDestroyed; return true;
}
AcquiredSwapchainImage NullDevice::acquire_next_image(SwapchainHandle handle, std::string* error) {
    if (!ready(error)) return {};
    auto* slot = swapchain(handle, error); if (!slot) return {};
    if (slot->acquired) { set_error(error, "swapchain image already acquired"); return {}; }
    slot->acquiredIndex = slot->nextImage % static_cast<std::uint32_t>(slot->images.size());
    slot->nextImage = (slot->acquiredIndex + 1U) % static_cast<std::uint32_t>(slot->images.size());
    slot->acquired = true;
    return {slot->images[slot->acquiredIndex], slot->acquiredIndex, false};
}
PresentResult NullDevice::present(SwapchainHandle handle, FenceHandle waitFence, std::string* error) {
    if (!ready(error)) return PresentResult::DeviceLost;
    auto* slot = swapchain(handle, error); if (!slot) return PresentResult::Error;
    if (!slot->acquired) { set_error(error, "present called without acquiring an image"); return PresentResult::Error; }
    if (waitFence && !fence_complete(waitFence)) { set_error(error, "present wait fence is incomplete"); return PresentResult::Error; }
    const auto* image = texture(slot->images[slot->acquiredIndex], error);
    if (!image) return PresentResult::Error;
    if (image->state != ResourceState::Present) { set_error(error, "swapchain image is not in Present state"); return PresentResult::Error; }
    slot->acquired = false; ++statistics_.presents; return PresentResult::Presented;
}

BindGroupLayoutHandle NullDevice::create_bind_group_layout(const BindGroupLayoutDesc& desc, std::string* error) {
    if (!ready(error)) return {};
    if (desc.bindings.empty()) { set_error(error, "bind-group layout must contain at least one binding"); return {}; }
    std::vector<std::uint32_t> seen;
    seen.reserve(desc.bindings.size());
    for (const auto& binding : desc.bindings) {
        if (binding.visibility == ShaderStage::NoStage) { set_error(error, "bind-group binding visibility cannot be empty"); return {}; }
        if (std::find(seen.begin(), seen.end(), binding.binding) != seen.end()) {
            set_error(error, "bind-group layout contains duplicate binding numbers"); return {};
        }
        seen.push_back(binding.binding);
    }
    const auto index = allocate_slot(bindGroupLayouts_); auto& slot = bindGroupLayouts_[index];
    slot.alive = true; slot.desc = desc; ++statistics_.bindGroupLayoutsCreated;
    return {index, slot.generation};
}
bool NullDevice::destroy_bind_group_layout(BindGroupLayoutHandle handle, std::string* error) {
    auto* slot = bind_group_layout(handle, error); if (!slot) return false;
    for (const auto& group : bindGroups_)
        if (group.alive && group.desc.layout == handle) {
            set_error(error, "bind-group layout is retained by a live bind group"); return false;
        }
    slot->desc = {}; retire_slot(*slot); ++statistics_.bindGroupLayoutsDestroyed; return true;
}
BindGroupHandle NullDevice::create_bind_group(const BindGroupDesc& desc, std::string* error) {
    if (!ready(error)) return {};
    const auto* layout = bind_group_layout(desc.layout, error); if (!layout) return {};
    if (desc.entries.size() != layout->desc.bindings.size()) {
        set_error(error, "bind group must provide exactly one entry per layout binding"); return {};
    }
    std::vector<std::uint32_t> seen;
    seen.reserve(desc.entries.size());
    for (const auto& entry : desc.entries) {
        if (std::find(seen.begin(), seen.end(), entry.binding) != seen.end()) {
            set_error(error, "bind group contains duplicate entries"); return {};
        }
        seen.push_back(entry.binding);
        const auto bindingIt = std::find_if(layout->desc.bindings.begin(), layout->desc.bindings.end(),
            [&](const BindGroupLayoutBinding& binding) { return binding.binding == entry.binding; });
        if (bindingIt == layout->desc.bindings.end()) {
            set_error(error, "bind-group entry does not exist in its layout"); return {};
        }
        switch (bindingIt->type) {
        case BindingType::UniformBuffer:
        case BindingType::StorageBufferReadOnly:
        case BindingType::StorageBufferReadWrite: {
            const auto* resource = buffer(entry.buffer, error); if (!resource) return {};
            if (entry.textureView) { set_error(error, "buffer binding also supplied a texture view"); return {}; }
            const std::size_t bytes = entry.bytes == 0U ? resource->desc.bytes - std::min(entry.offset, resource->desc.bytes) : entry.bytes;
            if (bytes == 0U || !range_fits(entry.offset, bytes, resource->desc.bytes)) {
                set_error(error, "bind-group buffer range exceeds allocation"); return {};
            }
            if (bindingIt->type == BindingType::UniformBuffer && !has_usage(resource->desc.usage, BufferUsage::Constant)) {
                set_error(error, "uniform binding requires Constant buffer usage"); return {};
            }
            if (bindingIt->type != BindingType::UniformBuffer && !has_usage(resource->desc.usage, BufferUsage::Storage)) {
                set_error(error, "storage binding requires Storage buffer usage"); return {};
            }
            break;
        }
        case BindingType::SampledTexture:
        case BindingType::StorageTexture: {
            auto* view = texture_view(entry.textureView, error); if (!view) return {};
            if (entry.buffer) { set_error(error, "texture binding also supplied a buffer"); return {}; }
            const auto* resource = texture(view->desc.texture, error); if (!resource) return {};
            const TextureUsage required = bindingIt->type == BindingType::SampledTexture
                                              ? TextureUsage::Sampled : TextureUsage::Storage;
            if (!has_usage(resource->desc.usage, required)) {
                set_error(error, bindingIt->type == BindingType::SampledTexture
                                     ? "sampled binding requires Sampled texture usage"
                                     : "storage binding requires Storage texture usage");
                return {};
            }
            if (bindingIt->type == BindingType::SampledTexture) {
                if (!sampler(entry.sampler, error)) return {};
            } else if (entry.sampler) {
                set_error(error, "storage texture binding cannot include a sampler"); return {};
            }
            break;
        }
        }
    }
    const auto index = allocate_slot(bindGroups_); auto& slot = bindGroups_[index];
    slot.alive = true; slot.desc = desc; ++statistics_.bindGroupsCreated;
    return {index, slot.generation};
}
bool NullDevice::destroy_bind_group(BindGroupHandle handle, std::string* error) {
    auto* slot = bind_group(handle, error); if (!slot) return false;
    slot->desc = {}; retire_slot(*slot); ++statistics_.bindGroupsDestroyed; return true;
}

ComputePipelineHandle NullDevice::create_compute_pipeline(const ComputePipelineDesc& desc, std::string* error) {
    if (!ready(error)) return {};
    if (desc.entryPoint.empty()) { set_error(error, "compute pipeline entry point is empty"); return {}; }
    const std::uint64_t invocations = static_cast<std::uint64_t>(desc.threadsX) * desc.threadsY * desc.threadsZ;
    if (desc.threadsX == 0U || desc.threadsY == 0U || desc.threadsZ == 0U ||
        desc.threadsX > capabilities_.maxComputeWorkgroupSizeX || desc.threadsY > capabilities_.maxComputeWorkgroupSizeY ||
        desc.threadsZ > capabilities_.maxComputeWorkgroupSizeZ || invocations > capabilities_.maxComputeInvocations) {
        set_error(error, "compute workgroup dimensions exceed device limits"); return {};
    }
    for (const BindGroupLayoutHandle layout : desc.bindGroupLayouts)
        if (!bind_group_layout(layout, error)) return {};
    const auto index = allocate_slot(pipelines_); auto& slot = pipelines_[index];
    slot.alive = true; slot.desc = desc; return {index, slot.generation};
}
bool NullDevice::destroy_compute_pipeline(ComputePipelineHandle handle, std::string* error) {
    auto* slot = pipeline(handle, error); if (!slot) return false; slot->desc = {}; retire_slot(*slot); return true;
}

GraphicsPipelineHandle NullDevice::create_graphics_pipeline(const GraphicsPipelineDesc& desc,
                                                            std::string* error) {
    if (!ready(error)) return {};
    if (!validate_vertex_input_layout(desc, error)) return {};
    const bool hasFragment = !desc.fragmentBytecode.empty() || !desc.fragmentEntryPoint.empty();
    if (desc.vertexEntryPoint.empty() || desc.vertexBytecode.empty() ||
        (hasFragment && (desc.fragmentEntryPoint.empty() || desc.fragmentBytecode.empty())) ||
        (!desc.colorFormat && !desc.depthFormat) || (desc.colorFormat && !hasFragment) ||
        (desc.colorFormat && *desc.colorFormat == TextureFormat::D32Float) ||
        (desc.depthFormat && *desc.depthFormat != TextureFormat::D32Float)) {
        set_error(error, "graphics pipeline requires valid attachment formats and shader stages");
        return {};
    }
    for (const BindGroupLayoutHandle layout : desc.bindGroupLayouts)
        if (!bind_group_layout(layout, error)) return {};
    if (desc.sampleCount != 1U && desc.sampleCount != 2U && desc.sampleCount != 4U &&
        desc.sampleCount != 8U) {
        set_error(error, "graphics pipeline sample count must be 1, 2, 4, or 8");
        return {};
    }
    const auto index = allocate_slot(graphicsPipelines_);
    auto& slot = graphicsPipelines_[index];
    slot.alive = true; slot.desc = desc;
    return {index, slot.generation};
}

bool NullDevice::destroy_graphics_pipeline(GraphicsPipelineHandle handle, std::string* error) {
    auto* slot = graphics_pipeline(handle, error); if (!slot) return false;
    slot->desc = {}; retire_slot(*slot); return true;
}

CommandListHandle NullDevice::begin_commands(QueueKind queue, std::string_view debugName, std::string* error) {
    if (!ready(error)) return {};
    const auto index = allocate_slot(commandLists_); auto& slot = commandLists_[index];
    slot.alive = true; slot.queue = queue; slot.debugName.assign(debugName); slot.commands.clear(); slot.debugDepth = 0U;
    slot.renderPassOpen = false; slot.graphicsPipeline = {}; slot.vertexBuffer = {};
    slot.vertexOffset = 0U; slot.vertexStride = 0U; slot.indexBuffer = {}; slot.indexOffset = 0U;
    slot.indexFormat = IndexFormat::Uint32; slot.viewport = {}; slot.scissor = {};
    slot.viewportSet = false; slot.scissorSet = false; slot.computeBindGroups.clear();
    slot.graphicsBindGroups.clear();
    return {index, slot.generation};
}
bool NullDevice::copy_buffer(CommandListHandle commands, BufferHandle source, std::size_t sourceOffset,
                             BufferHandle destination, std::size_t destinationOffset, std::size_t bytes,
                             std::string* error) {
    auto* list = command_list(commands, error); const auto* src = buffer(source, error); const auto* dst = buffer(destination, error);
    if (!list || !src || !dst) return false;
    if (!has_usage(src->desc.usage, BufferUsage::CopySource) || !has_usage(dst->desc.usage, BufferUsage::CopyDestination)) {
        set_error(error, "copy buffers lack source/destination usage"); return false;
    }
    if (bytes == 0U || !range_fits(sourceOffset, bytes, src->storage.size()) || !range_fits(destinationOffset, bytes, dst->storage.size())) {
        set_error(error, "buffer copy range is invalid"); return false;
    }
    list->commands.emplace_back(CopyCommand{source, sourceOffset, destination, destinationOffset, bytes}); return true;
}
bool NullDevice::transition_buffer(CommandListHandle commands, BufferHandle handle, ResourceState before,
                                   ResourceState after, std::string* error) {
    auto* list = command_list(commands, error); if (!list || !buffer(handle, error)) return false;
    if (before == after) { set_error(error, "buffer barrier must change state"); return false; }
    list->commands.emplace_back(BufferBarrierCommand{handle, before, after}); return true;
}
bool NullDevice::transition_texture(CommandListHandle commands, TextureHandle handle, ResourceState before,
                                    ResourceState after, std::string* error) {
    auto* list = command_list(commands, error); if (!list || !texture(handle, error)) return false;
    if (before == after) { set_error(error, "texture barrier must change state"); return false; }
    list->commands.emplace_back(TextureBarrierCommand{handle, before, after}); return true;
}
bool NullDevice::dispatch(CommandListHandle commands, ComputePipelineHandle handle,
                          std::uint32_t groupsX, std::uint32_t groupsY, std::uint32_t groupsZ,
                          std::string* error) {
    auto* list = command_list(commands, error); auto* compute = pipeline(handle, error);
    if (!list || !compute) return false;
    if (list->renderPassOpen || list->queue == QueueKind::Copy) {
        set_error(error, "compute dispatch requires a graphics or compute queue outside a render pass"); return false;
    }
    if (groupsX == 0U || groupsY == 0U || groupsZ == 0U) { set_error(error, "dispatch group dimensions must be nonzero"); return false; }
    if (list->computeBindGroups.size() < compute->desc.bindGroupLayouts.size()) {
        set_error(error, "compute dispatch is missing required bind groups"); return false;
    }
    for (std::size_t index = 0; index < compute->desc.bindGroupLayouts.size(); ++index) {
        const auto* group = bind_group(list->computeBindGroups[index], error); if (!group) return false;
        if (group->desc.layout != compute->desc.bindGroupLayouts[index]) {
            set_error(error, "compute bind-group layout does not match the pipeline layout"); return false;
        }
    }
    list->commands.emplace_back(DispatchCommand{handle, groupsX, groupsY, groupsZ}); return true;
}

bool NullDevice::begin_render_pass(CommandListHandle commands, const RenderPassDesc& desc,
                                   std::string* error) {
    auto* list = command_list(commands, error); if (!list) return false;
    if (list->queue != QueueKind::Graphics) { set_error(error, "render passes require a graphics queue"); return false; }
    if (list->renderPassOpen) { set_error(error, "render pass is already open"); return false; }
    if (desc.colors.size() > 4U || (desc.colors.empty() && !desc.depth)) {
        set_error(error, "render pass requires at least one attachment and at most four color attachments"); return false;
    }
    std::uint32_t width = 0U, height = 0U;
    for (const auto& attachment : desc.colors) {
        const auto* target = texture(attachment.texture, error); if (!target) return false;
        if (!has_usage(target->desc.usage, TextureUsage::RenderTarget)) {
            set_error(error, "color attachment lacks RenderTarget usage"); return false;
        }
        if (target->desc.format == TextureFormat::D32Float) {
            set_error(error, "depth texture cannot be a color attachment"); return false;
        }
        if (width == 0U) { width = target->desc.width; height = target->desc.height; }
        else if (width != target->desc.width || height != target->desc.height) {
            set_error(error, "render-pass attachment dimensions differ"); return false;
        }
    }
    if (desc.depth) {
        const auto* depth = texture(desc.depth->texture, error); if (!depth) return false;
        if (!has_usage(depth->desc.usage, TextureUsage::DepthStencil) ||
            depth->desc.format != TextureFormat::D32Float) {
            set_error(error, "depth attachment must be a D32Float DepthStencil texture"); return false;
        }
        if (width != 0U && (depth->desc.width != width || depth->desc.height != height)) {
            set_error(error, "depth attachment dimensions differ from color target"); return false;
        }
        if (!std::isfinite(desc.depth->clearDepth) || desc.depth->clearDepth < 0.0F ||
            desc.depth->clearDepth > 1.0F) {
            set_error(error, "depth clear value must be finite and in [0,1]"); return false;
        }
        if (width == 0U) { width = depth->desc.width; height = depth->desc.height; }
    }
    list->renderPassOpen = true; list->graphicsPipeline = {}; list->vertexBuffer = {};
    list->indexBuffer = {}; list->viewportSet = false; list->scissorSet = false;
    list->activeDepthTexture = desc.depth ? desc.depth->texture : TextureHandle{};
    list->activePassWidth = width; list->activePassHeight = height;
    list->commands.emplace_back(BeginRenderPassCommand{desc});
    return true;
}

bool NullDevice::end_render_pass(CommandListHandle commands, std::string* error) {
    auto* list = command_list(commands, error); if (!list) return false;
    if (!list->renderPassOpen) { set_error(error, "no render pass is open"); return false; }
    list->renderPassOpen = false;
    list->activeDepthTexture = {}; list->activePassWidth = 0U; list->activePassHeight = 0U;
    list->commands.emplace_back(EndRenderPassCommand{});
    return true;
}

bool NullDevice::bind_graphics_pipeline(CommandListHandle commands, GraphicsPipelineHandle handle,
                                        std::string* error) {
    auto* list = command_list(commands, error); if (!list || !graphics_pipeline(handle, error)) return false;
    if (!list->renderPassOpen) { set_error(error, "graphics pipeline must be bound inside a render pass"); return false; }
    list->graphicsPipeline = handle; return true;
}

bool NullDevice::set_viewport(CommandListHandle commands, const Viewport& viewport,
                              std::string* error) {
    auto* list = command_list(commands, error); if (!list) return false;
    if (!list->renderPassOpen || !std::isfinite(viewport.x) || !std::isfinite(viewport.y) ||
        !std::isfinite(viewport.width) || !std::isfinite(viewport.height) ||
        !std::isfinite(viewport.minimumDepth) || !std::isfinite(viewport.maximumDepth) ||
        viewport.width <= 0.0F || viewport.height <= 0.0F || viewport.minimumDepth < 0.0F ||
        viewport.maximumDepth > 1.0F || viewport.minimumDepth > viewport.maximumDepth) {
        set_error(error, "viewport is invalid or outside a render pass"); return false;
    }
    list->viewport = viewport; list->viewportSet = true; return true;
}

bool NullDevice::set_scissor(CommandListHandle commands, const ScissorRect& scissor,
                             std::string* error) {
    auto* list = command_list(commands, error); if (!list) return false;
    if (!list->renderPassOpen || scissor.width == 0U || scissor.height == 0U) {
        set_error(error, "scissor is invalid or outside a render pass"); return false;
    }
    list->scissor = scissor; list->scissorSet = true; return true;
}

bool NullDevice::clear_depth_region(CommandListHandle commands, float depthValue,
                                    const ScissorRect& region, std::string* error) {
    auto* list = command_list(commands, error); if (!list) return false;
    const auto right = static_cast<std::int64_t>(region.x) + region.width;
    const auto bottom = static_cast<std::int64_t>(region.y) + region.height;
    if (!list->renderPassOpen || !list->activeDepthTexture ||
        !std::isfinite(depthValue) || depthValue < 0.0F || depthValue > 1.0F ||
        region.x < 0 || region.y < 0 || region.width == 0U || region.height == 0U ||
        right > static_cast<std::int64_t>(list->activePassWidth) ||
        bottom > static_cast<std::int64_t>(list->activePassHeight)) {
        set_error(error, "depth clear region is invalid or the active pass has no depth attachment");
        return false;
    }
    list->commands.emplace_back(ClearDepthRegionCommand{list->activeDepthTexture, depthValue, region});
    return true;
}

bool NullDevice::bind_vertex_buffer(CommandListHandle commands, std::uint32_t slotIndex,
                                    BufferHandle handle, std::size_t offset,
                                    std::uint32_t stride, std::string* error) {
    auto* list = command_list(commands, error); const auto* resource = buffer(handle, error);
    if (!list || !resource) return false;
    if (!list->renderPassOpen || slotIndex != 0U || stride == 0U || offset >= resource->desc.bytes ||
        !has_usage(resource->desc.usage, BufferUsage::Vertex)) {
        set_error(error, "vertex buffer binding is invalid"); return false;
    }
    list->vertexBuffer = handle; list->vertexOffset = offset; list->vertexStride = stride; return true;
}

bool NullDevice::bind_index_buffer(CommandListHandle commands, BufferHandle handle,
                                   std::size_t offset, IndexFormat format,
                                   std::string* error) {
    auto* list = command_list(commands, error); const auto* resource = buffer(handle, error);
    if (!list || !resource) return false;
    const std::size_t alignment = format == IndexFormat::Uint16 ? 2U : 4U;
    if (!list->renderPassOpen || offset >= resource->desc.bytes || offset % alignment != 0U ||
        !has_usage(resource->desc.usage, BufferUsage::Index)) {
        set_error(error, "index buffer binding is invalid"); return false;
    }
    list->indexBuffer = handle; list->indexOffset = offset; list->indexFormat = format; return true;
}


bool NullDevice::bind_compute_bind_group(CommandListHandle commands, std::uint32_t index,
                                         BindGroupHandle groupHandle, std::string* error) {
    auto* list = command_list(commands, error); if (!list || !bind_group(groupHandle, error)) return false;
    if (list->renderPassOpen || list->queue == QueueKind::Copy || index >= 8U) {
        set_error(error, "compute bind-group binding is invalid"); return false;
    }
    if (list->computeBindGroups.size() <= index) list->computeBindGroups.resize(index + 1U);
    list->computeBindGroups[index] = groupHandle;
    return true;
}

bool NullDevice::bind_graphics_bind_group(CommandListHandle commands, std::uint32_t index,
                                          BindGroupHandle groupHandle, std::string* error) {
    auto* list = command_list(commands, error); auto* group = bind_group(groupHandle, error);
    if (!list || !group) return false;
    const auto* pipelineSlot = graphics_pipeline(list->graphicsPipeline, error);
    if (!list->renderPassOpen || !pipelineSlot || index >= 8U ||
        index >= pipelineSlot->desc.bindGroupLayouts.size() ||
        group->desc.layout != pipelineSlot->desc.bindGroupLayouts[index]) {
        set_error(error, "graphics bind-group binding is invalid or incompatible with the pipeline"); return false;
    }
    if (list->graphicsBindGroups.size() <= index) list->graphicsBindGroups.resize(index + 1U);
    list->graphicsBindGroups[index] = groupHandle;
    return true;
}

bool NullDevice::draw_indexed(CommandListHandle commands, std::uint32_t indexCount,
                              std::uint32_t instanceCount, std::uint32_t firstIndex,
                              std::int32_t vertexOffset, std::uint32_t firstInstance,
                              std::string* error) {
    auto* list = command_list(commands, error); if (!list) return false;
    const auto* indices = buffer(list->indexBuffer, error);
    const auto* boundPipeline = graphics_pipeline(list->graphicsPipeline, nullptr);
    if (!list->renderPassOpen || !list->graphicsPipeline || !list->vertexBuffer ||
        !list->indexBuffer || !list->viewportSet || !list->scissorSet || !indices ||
        !boundPipeline ||
        (boundPipeline->desc.vertexBuffer &&
         list->vertexStride != boundPipeline->desc.vertexBuffer->stride) ||
        list->graphicsBindGroups.size() < boundPipeline->desc.bindGroupLayouts.size() ||
        indexCount == 0U || instanceCount == 0U) {
        set_error(error, "indexed draw is missing required render state"); return false;
    }
    const std::size_t indexBytes = list->indexFormat == IndexFormat::Uint16 ? 2U : 4U;
    const std::uint64_t endIndex = static_cast<std::uint64_t>(firstIndex) + indexCount;
    const std::uint64_t endBytes = static_cast<std::uint64_t>(list->indexOffset) + endIndex * indexBytes;
    if (endBytes > indices->desc.bytes) {
        set_error(error, "indexed draw exceeds the bound index buffer"); return false;
    }
    (void)vertexOffset; (void)firstInstance;
    list->commands.emplace_back(DrawIndexedCommand{indexCount, instanceCount, firstIndex,
                                                    vertexOffset, firstInstance});
    return true;
}
bool NullDevice::begin_debug_label(CommandListHandle commands, std::string_view label, std::string* error) {
    auto* list = command_list(commands, error); if (!list) return false;
    if (label.empty()) { set_error(error, "debug label cannot be empty"); return false; }
    ++list->debugDepth; return true;
}
bool NullDevice::end_debug_label(CommandListHandle commands, std::string* error) {
    auto* list = command_list(commands, error); if (!list) return false;
    if (list->debugDepth == 0U) { set_error(error, "debug-label stack underflow"); return false; }
    --list->debugDepth; return true;
}

TimestampQueryPoolHandle NullDevice::create_timestamp_query_pool(std::uint32_t count, std::string_view debugName, std::string* error) {
    if (!ready(error)) return {};
    if (count == 0U || !capabilities_.timestampQueries) { set_error(error, "timestamp query count must be nonzero and supported"); return {}; }
    const auto index = allocate_slot(timestampPools_); auto& slot = timestampPools_[index];
    slot.alive = true; slot.debugName.assign(debugName); slot.values.assign(count, 0U); slot.available.assign(count, false);
    return {index, slot.generation};
}
bool NullDevice::destroy_timestamp_query_pool(TimestampQueryPoolHandle handle, std::string* error) {
    auto* slot = timestamp_pool(handle, error); if (!slot) return false;
    slot->values.clear(); slot->available.clear(); slot->debugName.clear(); retire_slot(*slot); return true;
}
bool NullDevice::write_timestamp(CommandListHandle commands, TimestampQueryPoolHandle pool,
                                 std::uint32_t index, std::string* error) {
    auto* list = command_list(commands, error); auto* query = timestamp_pool(pool, error);
    if (!list || !query) return false;
    if (index >= query->values.size()) { set_error(error, "timestamp index exceeds pool"); return false; }
    list->commands.emplace_back(TimestampCommand{pool, index}); return true;
}
bool NullDevice::resolve_timestamps(TimestampQueryPoolHandle pool, std::uint32_t first,
                                    std::span<std::uint64_t> destination, std::string* error) {
    const auto* query = timestamp_pool(pool, error); if (!query) return false;
    if (first > query->values.size() || destination.size() > query->values.size() - first) {
        set_error(error, "timestamp resolve range exceeds pool"); return false;
    }
    for (std::size_t offset = 0; offset < destination.size(); ++offset) {
        if (!query->available[first + offset]) { set_error(error, "timestamp query is not available"); return false; }
        destination[offset] = query->values[first + offset];
    }
    return true;
}

FenceHandle NullDevice::submit(CommandListHandle commands, std::string* error) {
    if (!ready(error)) return {};
    auto* list = command_list(commands, error); if (!list) return {};
    if (list->debugDepth != 0U) { set_error(error, "unbalanced debug labels at submission"); return {}; }
    if (list->renderPassOpen) { set_error(error, "render pass is still open at submission"); return {}; }
    for (const RecordedCommand& recorded : list->commands) {
        bool ok = std::visit([&](const auto& command) -> bool {
            using T = std::decay_t<decltype(command)>;
            if constexpr (std::is_same_v<T, CopyCommand>) {
                auto* src = buffer(command.source, error); auto* dst = buffer(command.destination, error);
                if (!src || !dst) return false;
                std::copy(src->storage.begin() + static_cast<std::ptrdiff_t>(command.sourceOffset),
                          src->storage.begin() + static_cast<std::ptrdiff_t>(command.sourceOffset + command.bytes),
                          dst->storage.begin() + static_cast<std::ptrdiff_t>(command.destinationOffset));
                ++statistics_.copiesExecuted; return true;
            } else if constexpr (std::is_same_v<T, BufferBarrierCommand>) {
                auto* resource = buffer(command.buffer, error); if (!resource) return false;
                if (resource->state != command.before) { set_error(error, "buffer barrier before-state mismatch"); return false; }
                resource->state = command.after; ++statistics_.barriersExecuted; return true;
            } else if constexpr (std::is_same_v<T, TextureBarrierCommand>) {
                auto* resource = texture(command.texture, error); if (!resource) return false;
                if (resource->state != command.before) { set_error(error, "texture barrier before-state mismatch"); return false; }
                resource->state = command.after; ++statistics_.barriersExecuted; return true;
            } else if constexpr (std::is_same_v<T, DispatchCommand>) {
                if (!pipeline(command.pipeline, error)) return false;
                ++statistics_.dispatchesExecuted; return true;
            } else if constexpr (std::is_same_v<T, BeginRenderPassCommand>) {
                for (const auto& attachment : command.desc.colors) {
                    auto* target = texture(attachment.texture, error); if (!target) return false;
                    if (target->state != ResourceState::RenderTarget) {
                        set_error(error, "color attachment is not in RenderTarget state"); return false;
                    }
                    if (attachment.clear && target->storage.size() >= 4U) {
                        const auto channel = [](float value) -> std::byte {
                            return static_cast<std::byte>(static_cast<std::uint8_t>(
                                std::clamp(value, 0.0F, 1.0F) * 255.0F + 0.5F));
                        };
                        const std::array<std::byte,4> rgba{channel(attachment.clearR),channel(attachment.clearG),
                                                          channel(attachment.clearB),channel(attachment.clearA)};
                        for (std::size_t i=0;i+3U<target->storage.size();i+=4U)
                            std::copy(rgba.begin(),rgba.end(),target->storage.begin()+static_cast<std::ptrdiff_t>(i));
                    }
                }
                if (command.desc.depth) {
                    auto* depth = texture(command.desc.depth->texture, error); if (!depth) return false;
                    if (depth->state != ResourceState::DepthWrite) {
                        set_error(error, "depth attachment is not in DepthWrite state"); return false;
                    }
                    if (command.desc.depth->clear) {
                        for (std::size_t i=0;i+sizeof(float)<=depth->storage.size();i+=sizeof(float))
                            std::memcpy(depth->storage.data()+static_cast<std::ptrdiff_t>(i),
                                        &command.desc.depth->clearDepth,sizeof(float));
                    }
                }
                ++statistics_.renderPassesExecuted; return true;
            } else if constexpr (std::is_same_v<T, EndRenderPassCommand>) {
                return true;
            } else if constexpr (std::is_same_v<T, ClearDepthRegionCommand>) {
                auto* depth = texture(command.depthTexture, error); if (!depth) return false;
                if (depth->desc.format != TextureFormat::D32Float ||
                    depth->state != ResourceState::DepthWrite) {
                    set_error(error, "depth-region clear target is invalid"); return false;
                }
                for (std::uint32_t y = 0U; y < command.region.height; ++y) {
                    const std::size_t pixel =
                        (static_cast<std::size_t>(command.region.y + static_cast<std::int32_t>(y)) *
                         depth->desc.width + static_cast<std::size_t>(command.region.x));
                    std::byte* row = depth->storage.data() +
                        static_cast<std::ptrdiff_t>(pixel * sizeof(float));
                    for (std::uint32_t x = 0U; x < command.region.width; ++x)
                        std::memcpy(row + static_cast<std::ptrdiff_t>(x * sizeof(float)),
                                    &command.depth, sizeof(float));
                }
                return true;
            } else if constexpr (std::is_same_v<T, DrawIndexedCommand>) {
                ++statistics_.indexedDrawsExecuted;
                statistics_.indexedTrianglesSubmitted +=
                    static_cast<std::uint64_t>(command.indexCount / 3U) * command.instanceCount;
                return true;
            } else {
                auto* pool = timestamp_pool(command.pool, error); if (!pool) return false;
                pool->values[command.index] = syntheticTimestamp_;
                syntheticTimestamp_ += 100U; pool->available[command.index] = true; return true;
            }
        }, recorded);
        if (!ok) return {};
    }
    const FenceHandle fence{nextFence_++}; completedFence_ = fence.value;
    ++statistics_.commandListsSubmitted;
    list->commands.clear(); retire_slot(*list); return fence;
}
bool NullDevice::fence_complete(FenceHandle fence) const noexcept { return fence && fence.value <= completedFence_; }
bool NullDevice::wait(FenceHandle fence, std::string* error) {
    if (!ready(error)) return false;
    if (!fence || fence.value >= nextFence_) { set_error(error, "unknown fence"); return false; }
    completedFence_ = std::max(completedFence_, fence.value); return true;
}
void NullDevice::wait_idle() noexcept { completedFence_ = nextFence_ > 0U ? nextFence_ - 1U : 0U; }
void NullDevice::simulate_device_loss(std::string reason) {
    status_ = DeviceStatus::Lost; lossReason_ = reason.empty() ? "simulated device loss" : std::move(reason);
}

} // namespace dve::rhi
