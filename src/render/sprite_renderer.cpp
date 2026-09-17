#include "dve/render/sprite_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <utility>

namespace dve::render {
namespace {

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

[[nodiscard]] bool checked_product(
    std::size_t left, std::size_t right, std::size_t& result) noexcept {
    if (left != 0U && right > std::numeric_limits<std::size_t>::max() / left) return false;
    result = left * right;
    return true;
}

[[nodiscard]] std::size_t grown_capacity(std::size_t required) noexcept {
    std::size_t capacity = 256U;
    while (capacity < required && capacity <= std::numeric_limits<std::size_t>::max() / 2U)
        capacity *= 2U;
    return std::max(capacity, required);
}

[[nodiscard]] bool finite_color(const std::array<float, 4>& color) noexcept {
    return std::all_of(color.begin(), color.end(), [](float value) { return std::isfinite(value); });
}

[[nodiscard]] std::uint64_t resolved_palette_color_hash(
    std::span<const SpriteColor8> colors) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const SpriteColor8 color : colors) {
        for (const std::uint8_t component : {color.r, color.g, color.b, color.a}) {
            hash ^= component;
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

[[nodiscard]] rhi::BlendMode to_rhi_blend(SpriteBlendMode mode) noexcept {
    switch (mode) {
        case SpriteBlendMode::Opaque: return rhi::BlendMode::Opaque;
        case SpriteBlendMode::Alpha: return rhi::BlendMode::Alpha;
        case SpriteBlendMode::Additive: return rhi::BlendMode::Additive;
        case SpriteBlendMode::Multiply: return rhi::BlendMode::Multiply;
    }
    return rhi::BlendMode::Alpha;
}

[[nodiscard]] bool load_spirv_module(
    const std::filesystem::path& path,
    std::vector<std::byte>& output,
    std::string* error) {
    constexpr std::uintmax_t maximumShaderBytes = 1024U * 1024U;
    std::error_code filesystemError;
    const std::uintmax_t bytes = std::filesystem::file_size(path, filesystemError);
    if (filesystemError)
        return fail(error, "could not inspect sprite shader module: " + path.string());
    if (bytes < sizeof(std::uint32_t) || bytes > maximumShaderBytes ||
        bytes % sizeof(std::uint32_t) != 0U)
        return fail(error, "sprite shader module has an invalid byte length: " + path.string());
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return fail(error, "could not open sprite shader module: " + path.string());
    output.resize(static_cast<std::size_t>(bytes));
    stream.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(output.size()));
    if (!stream || stream.gcount() != static_cast<std::streamsize>(output.size())) {
        output.clear();
        return fail(error, "could not read the complete sprite shader module: " + path.string());
    }
    std::uint32_t magic{};
    std::memcpy(&magic, output.data(), sizeof(magic));
    if (magic != 0x07230203U) {
        output.clear();
        return fail(error, "sprite shader module is not SPIR-V: " + path.string());
    }
    return true;
}

} // namespace

bool load_sprite_spirv_shaders(
    const std::filesystem::path& directory,
    SpriteRhiShaderBytecode& bytecode,
    std::string* error) {
    bytecode = {};
    if (!load_spirv_module(directory / "sprite_2d.vert.spv", bytecode.vertex, error))
        return false;
    if (!load_spirv_module(directory / "sprite_2d.frag.spv", bytecode.fragment, error)) {
        bytecode = {};
        return false;
    }
    return true;
}

bool load_sprite_indexed_spirv_shader(
    const std::filesystem::path& directory,
    SpriteRhiShaderBytecode& bytecode,
    std::string* error) {
    bytecode.indexedFragment.clear();
    return load_spirv_module(
        directory / "sprite_2d_indexed.frag.spv", bytecode.indexedFragment, error);
}

GpuSpriteVertex project_sprite_vertex(
    const SpriteVertex& source,
    GameplayPlane2D plane,
    const PixelPresentationConfig& presentation,
    Float3 cameraOrigin,
    SpriteVec2 logicalCameraCenter) noexcept {
    const float worldVertical = plane == GameplayPlane2D::XY
        ? source.position.y - cameraOrigin.y
        : source.position.z - cameraOrigin.z;
    const float logicalX = logicalCameraCenter.x +
        (source.position.x - cameraOrigin.x) * presentation.pixelsPerWorldUnit;
    const float logicalY = logicalCameraCenter.y -
        worldVertical * presentation.pixelsPerWorldUnit;
    GpuSpriteVertex vertex;
    vertex.positionNdc = {
        logicalX * 2.0F / static_cast<float>(presentation.logicalWidth) - 1.0F,
        1.0F - logicalY * 2.0F / static_cast<float>(presentation.logicalHeight),
        0.0F,
    };
    vertex.uv = {source.uv.x, source.uv.y};
    vertex.color = source.color;
    return vertex;
}

SpriteRhiRenderer::~SpriteRhiRenderer() {
    std::string ignored;
    (void)shutdown(&ignored);
}

bool SpriteRhiRenderer::create_groups(
    rhi::TextureViewHandle view, TextureGroups& groups, std::string* error) {
    if (!textureLayout_ || !nearestSampler_ || !linearSampler_ || !view)
        return fail(error, "sprite texture groups require initialized resources and a valid view");
    rhi::BindGroupDesc desc;
    desc.layout = textureLayout_;
    desc.debugName = "Sprite nearest texture group";
    desc.entries = {{0U, {}, view, 0U, 0U, nearestSampler_}};
    groups.nearest = device_.create_bind_group(desc, error);
    if (!groups.nearest) return false;
    desc.debugName = "Sprite linear texture group";
    desc.entries[0].sampler = linearSampler_;
    groups.linear = device_.create_bind_group(desc, error);
    if (!groups.linear) {
        std::string ignored;
        (void)device_.destroy_bind_group(groups.nearest, &ignored);
        groups.nearest = {};
        return false;
    }
    groups.view = view;
    return true;
}

bool SpriteRhiRenderer::initialize(
    const SpriteRhiShaderBytecode& bytecode,
    rhi::TextureFormat colorFormat,
    std::string* error) {
    if (initialized()) return fail(error, "sprite RHI renderer is already initialized");
    if (!bytecode.valid()) return fail(error, "sprite RHI shaders are missing");
    if (colorFormat == rhi::TextureFormat::D32Float)
        return fail(error, "sprite color target cannot use a depth format");
    colorFormat_ = colorFormat;

    rhi::BindGroupLayoutDesc layout;
    layout.debugName = "Sprite sampled texture layout";
    layout.bindings = {{0U, rhi::BindingType::SampledTexture, rhi::ShaderStage::Fragment}};
    textureLayout_ = device_.create_bind_group_layout(layout, error);
    if (!textureLayout_) return false;

    if (bytecode.indexed_valid()) {
        layout = {};
        layout.debugName = "Sprite indexed palette layout";
        layout.bindings = {{0U, rhi::BindingType::StorageBufferReadOnly,
                            rhi::ShaderStage::Fragment}};
        paletteLayout_ = device_.create_bind_group_layout(layout, error);
        if (!paletteLayout_) { (void)shutdown(nullptr); return false; }
    }

    rhi::SamplerDesc sampler;
    sampler.minFilter = rhi::FilterMode::Nearest;
    sampler.magFilter = rhi::FilterMode::Nearest;
    sampler.mipmapFilter = rhi::MipmapFilterMode::Nearest;
    sampler.addressU = rhi::AddressMode::ClampToEdge;
    sampler.addressV = rhi::AddressMode::ClampToEdge;
    sampler.addressW = rhi::AddressMode::ClampToEdge;
    sampler.debugName = "Sprite nearest sampler";
    nearestSampler_ = device_.create_sampler(sampler, error);
    if (!nearestSampler_) { (void)shutdown(nullptr); return false; }
    sampler.minFilter = rhi::FilterMode::Linear;
    sampler.magFilter = rhi::FilterMode::Linear;
    sampler.mipmapFilter = rhi::MipmapFilterMode::Linear;
    sampler.debugName = "Sprite linear sampler";
    linearSampler_ = device_.create_sampler(sampler, error);
    if (!linearSampler_) { (void)shutdown(nullptr); return false; }

    rhi::TextureDesc fallback;
    fallback.format = rhi::TextureFormat::RGBA8Unorm;
    fallback.width = 2U;
    fallback.height = 2U;
    fallback.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDestination;
    fallback.initialState = rhi::ResourceState::ShaderRead;
    fallback.debugName = "Sprite missing-texture fallback";
    fallbackTexture_ = device_.create_texture(fallback, error);
    if (!fallbackTexture_) { (void)shutdown(nullptr); return false; }
    const std::array<std::byte, 16> checker{
        std::byte{255}, std::byte{0}, std::byte{255}, std::byte{255},
        std::byte{24}, std::byte{24}, std::byte{24}, std::byte{255},
        std::byte{24}, std::byte{24}, std::byte{24}, std::byte{255},
        std::byte{255}, std::byte{0}, std::byte{255}, std::byte{255},
    };
    if (!device_.write_texture(fallbackTexture_, 0U, 0U, checker, 8U, error)) {
        (void)shutdown(nullptr);
        return false;
    }
    rhi::TextureViewDesc fallbackView;
    fallbackView.texture = fallbackTexture_;
    fallbackView.debugName = "Sprite missing-texture fallback view";
    fallbackView_ = device_.create_texture_view(fallbackView, error);
    if (!fallbackView_ || !create_groups(fallbackView_, fallbackGroups_, error)) {
        (void)shutdown(nullptr);
        return false;
    }

    for (std::size_t index = 0U; index < pipelines_.size(); ++index) {
        rhi::GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.debugName = "Sprite pipeline";
        pipelineDesc.vertexBytecode = bytecode.vertex;
        pipelineDesc.fragmentBytecode = bytecode.fragment;
        pipelineDesc.bindGroupLayouts = {textureLayout_};
        pipelineDesc.vertexBuffer = rhi::VertexBufferLayoutDesc{
            static_cast<std::uint32_t>(sizeof(GpuSpriteVertex)), false};
        pipelineDesc.vertexAttributes = {
            {0U, rhi::VertexFormat::Float3,
                static_cast<std::uint32_t>(offsetof(GpuSpriteVertex, positionNdc))},
            {1U, rhi::VertexFormat::Float2,
                static_cast<std::uint32_t>(offsetof(GpuSpriteVertex, uv))},
            {2U, rhi::VertexFormat::Float4,
                static_cast<std::uint32_t>(offsetof(GpuSpriteVertex, color))},
        };
        pipelineDesc.colorFormat = colorFormat_;
        pipelineDesc.depthFormat.reset();
        pipelineDesc.cullMode = rhi::CullMode::Disabled;
        pipelineDesc.depthTest = false;
        pipelineDesc.depthWrite = false;
        pipelineDesc.blend = to_rhi_blend(static_cast<SpriteBlendMode>(index));
        pipelines_[index] = device_.create_graphics_pipeline(pipelineDesc, error);
        if (!pipelines_[index]) { (void)shutdown(nullptr); return false; }
        if (bytecode.indexed_valid()) {
            pipelineDesc.debugName = "Indexed sprite palette pipeline";
            pipelineDesc.fragmentBytecode = bytecode.indexedFragment;
            pipelineDesc.bindGroupLayouts = {textureLayout_, paletteLayout_};
            indexedPipelines_[index] = device_.create_graphics_pipeline(pipelineDesc, error);
            if (!indexedPipelines_[index]) { (void)shutdown(nullptr); return false; }
        }
    }
    return true;
}

bool SpriteRhiRenderer::shutdown(std::string* error) noexcept {
    bool ok = true;
    std::string local;
    const auto step = [&](bool value) {
        if (!value && ok) {
            ok = false;
            if (error) *error = local;
        }
        local.clear();
    };
    for (auto& [name, groups] : textures_) {
        (void)name;
        if (groups.linear) step(device_.destroy_bind_group(groups.linear, &local));
        if (groups.nearest) step(device_.destroy_bind_group(groups.nearest, &local));
    }
    textures_.clear();
    for (auto& [key, resource] : paletteResources_) {
        (void)key;
        if (resource.group) step(device_.destroy_bind_group(resource.group, &local));
        if (resource.buffer) step(device_.destroy_buffer(resource.buffer, &local));
    }
    paletteResources_.clear();
    if (fallbackGroups_.linear)
        step(device_.destroy_bind_group(fallbackGroups_.linear, &local));
    if (fallbackGroups_.nearest)
        step(device_.destroy_bind_group(fallbackGroups_.nearest, &local));
    fallbackGroups_ = {};
    for (auto& pipelineHandle : indexedPipelines_) {
        if (pipelineHandle) step(device_.destroy_graphics_pipeline(pipelineHandle, &local));
        pipelineHandle = {};
    }
    for (auto& pipelineHandle : pipelines_) {
        if (pipelineHandle) step(device_.destroy_graphics_pipeline(pipelineHandle, &local));
        pipelineHandle = {};
    }
    if (vertexBuffer_) step(device_.destroy_buffer(vertexBuffer_, &local));
    if (indexBuffer_) step(device_.destroy_buffer(indexBuffer_, &local));
    vertexBuffer_ = {};
    indexBuffer_ = {};
    vertexCapacity_ = 0U;
    indexCapacity_ = 0U;
    if (fallbackView_) step(device_.destroy_texture_view(fallbackView_, &local));
    if (fallbackTexture_) step(device_.destroy_texture(fallbackTexture_, &local));
    fallbackView_ = {};
    fallbackTexture_ = {};
    if (linearSampler_) step(device_.destroy_sampler(linearSampler_, &local));
    if (nearestSampler_) step(device_.destroy_sampler(nearestSampler_, &local));
    linearSampler_ = {};
    nearestSampler_ = {};
    if (paletteLayout_) step(device_.destroy_bind_group_layout(paletteLayout_, &local));
    paletteLayout_ = {};
    if (textureLayout_) step(device_.destroy_bind_group_layout(textureLayout_, &local));
    textureLayout_ = {};
    return ok;
}

bool SpriteRhiRenderer::register_texture(
    std::string textureAsset, rhi::TextureViewHandle view, std::string* error) {
    if (!initialized()) return fail(error, "sprite RHI renderer is not initialized");
    if (textureAsset.empty() || textureAsset.size() > 4096U || !view)
        return fail(error, "sprite texture registration is invalid");
    if (textures_.contains(textureAsset))
        return fail(error, "sprite texture is already registered");
    TextureGroups groups;
    if (!create_groups(view, groups, error)) return false;
    textures_.emplace(std::move(textureAsset), groups);
    return true;
}

bool SpriteRhiRenderer::unregister_texture(
    std::string_view textureAsset, std::string* error) noexcept {
    const auto found = textures_.find(textureAsset);
    if (found == textures_.end()) return false;
    std::string local;
    bool ok = true;
    if (found->second.linear && !device_.destroy_bind_group(found->second.linear, &local)) ok = false;
    if (found->second.nearest && !device_.destroy_bind_group(found->second.nearest, &local)) ok = false;
    if (!ok && error) *error = local;
    textures_.erase(found);
    return ok;
}

bool SpriteRhiRenderer::has_texture(std::string_view textureAsset) const noexcept {
    return textures_.contains(textureAsset);
}

bool SpriteRhiRenderer::ensure_buffers(
    std::size_t vertexBytes, std::size_t indexBytes, std::string* error) {
    const auto replace = [&](rhi::BufferHandle& handle, std::size_t& capacity,
                             std::size_t required, rhi::BufferUsage usage,
                             std::string_view debugName) {
        if (required <= capacity && handle) return true;
        rhi::BufferDesc desc;
        desc.bytes = grown_capacity(required);
        desc.usage = usage | rhi::BufferUsage::CopyDestination;
        desc.memory = rhi::MemoryDomain::Upload;
        desc.initialState = rhi::ResourceState::ShaderRead;
        desc.debugName.assign(debugName);
        rhi::BufferHandle replacement = device_.create_buffer(desc, error);
        if (!replacement) return false;
        if (handle) {
            std::string local;
            if (!device_.destroy_buffer(handle, &local)) {
                (void)device_.destroy_buffer(replacement, nullptr);
                return fail(error, local);
            }
        }
        handle = replacement;
        capacity = desc.bytes;
        return true;
    };
    return replace(vertexBuffer_, vertexCapacity_, vertexBytes, rhi::BufferUsage::Vertex,
                   "Dynamic sprite vertices") &&
        replace(indexBuffer_, indexCapacity_, indexBytes, rhi::BufferUsage::Index,
                "Dynamic sprite indices");
}

bool SpriteRhiRenderer::ensure_palette_resource(
    const SpritePalettePacket& packet, bool& uploaded, std::string* error) {
    uploaded = false;
    if (!indexed_palette_supported())
        return fail(error, "indexed sprite palette shader resources are unavailable");
    if (packet.paletteAsset.empty() || packet.colors.empty() ||
        packet.colors.size() > kMaximumSpritePaletteEntries || packet.stateHash == 0U)
        return fail(error, "sprite palette packet is invalid or unaddressable");
    const std::uint64_t resolvedHash = resolved_palette_color_hash(packet.colors);
    const auto key = std::make_pair(packet.paletteAsset, packet.stateHash);
    const auto found = paletteResources_.find(key);
    if (found != paletteResources_.end()) {
        if (found->second.paletteContentHash != packet.paletteContentHash ||
            found->second.resolvedColorHash != resolvedHash ||
            found->second.colorCount != packet.colors.size())
            return fail(error, "sprite palette state hash collision changed packet contents");
        return true;
    }

    constexpr std::size_t paletteEntries = kMaximumSpritePaletteEntries;
    std::array<std::uint32_t, paletteEntries> packed{};
    for (std::size_t index = 0U; index < packet.colors.size(); ++index) {
        const SpriteColor8 color = packet.colors[index];
        packed[index] = static_cast<std::uint32_t>(color.r) |
            (static_cast<std::uint32_t>(color.g) << 8U) |
            (static_cast<std::uint32_t>(color.b) << 16U) |
            (static_cast<std::uint32_t>(color.a) << 24U);
    }
    rhi::BufferDesc bufferDesc;
    bufferDesc.bytes = sizeof(packed);
    bufferDesc.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDestination;
    bufferDesc.memory = rhi::MemoryDomain::Upload;
    bufferDesc.initialState = rhi::ResourceState::ShaderRead;
    bufferDesc.debugName = "Sprite indexed palette packet";
    PaletteResource resource;
    resource.buffer = device_.create_buffer(bufferDesc, error);
    if (!resource.buffer) return false;
    if (!device_.write_buffer(resource.buffer, 0U, std::as_bytes(std::span(packed)), error)) {
        (void)device_.destroy_buffer(resource.buffer, nullptr);
        return false;
    }
    rhi::BindGroupDesc groupDesc;
    groupDesc.layout = paletteLayout_;
    groupDesc.debugName = "Sprite indexed palette group";
    groupDesc.entries = {{0U, resource.buffer, {}, 0U, sizeof(packed), {}}};
    resource.group = device_.create_bind_group(groupDesc, error);
    if (!resource.group) {
        (void)device_.destroy_buffer(resource.buffer, nullptr);
        return false;
    }
    resource.paletteContentHash = packet.paletteContentHash;
    resource.resolvedColorHash = resolvedHash;
    resource.colorCount = packet.colors.size();
    paletteResources_.emplace(key, resource);
    uploaded = true;
    return true;
}

rhi::GraphicsPipelineHandle SpriteRhiRenderer::pipeline(
    SpriteBlendMode mode, bool indexed) const noexcept {
    const std::size_t index = static_cast<std::size_t>(mode);
    if (index >= pipelines_.size()) return {};
    return indexed ? indexedPipelines_[index] : pipelines_[index];
}

bool SpriteRhiRenderer::record_frame(
    const SpriteRhiFrameDesc& frame,
    SpriteRhiFrameStats& stats,
    rhi::FenceHandle* fence,
    std::string* error) {
    stats = {};
    if (!initialized()) return fail(error, "sprite RHI renderer is not initialized");
    if (!frame.colorTarget || frame.outputWidth == 0U || frame.outputHeight == 0U ||
        !finite_color(frame.letterboxColor))
        return fail(error, "sprite RHI frame target or clear color is invalid");
    const auto layout = compute_pixel_presentation(
        frame.presentation, frame.outputWidth, frame.outputHeight, error);
    if (!layout) return false;
    stats.layout = *layout;
    if (frame.items.empty() != frame.batches.empty())
        return fail(error, "sprite RHI items and batches must both be empty or non-empty");

    std::size_t expectedItem = 0U;
    for (const SpriteBatch& batch : frame.batches) {
        if (batch.itemCount == 0U || batch.firstItem != expectedItem ||
            batch.firstItem > frame.items.size() ||
            batch.itemCount > frame.items.size() - batch.firstItem)
            return fail(error, "sprite RHI batch ranges are not contiguous and complete");
        for (std::size_t index = batch.firstItem;
             index < batch.firstItem + batch.itemCount; ++index) {
            const SpriteDrawItem& item = frame.items[index];
            if (item.textureAsset != batch.textureAsset || item.materialId != batch.materialId ||
                item.paletteBank != batch.paletteBank || item.paletteAsset != batch.paletteAsset ||
                item.paletteStateHash != batch.paletteStateHash ||
                item.palettePacket != batch.palettePacket || item.sampling != batch.sampling ||
                item.blendMode != batch.blendMode)
                return fail(error, "sprite RHI batch does not match its draw items");
            const bool indexed = !item.paletteAsset.empty() ||
                item.palettePacket != kInvalidSpritePalettePacket;
            if (indexed != (!batch.paletteAsset.empty() ||
                            batch.palettePacket != kInvalidSpritePalettePacket))
                return fail(error, "sprite RHI indexed batch state is inconsistent");
        }
        const bool indexed = !batch.paletteAsset.empty() ||
            batch.palettePacket != kInvalidSpritePalettePacket;
        if (indexed) {
            if (!indexed_palette_supported())
                return fail(error, "indexed sprite palette shader resources are unavailable");
            if (batch.paletteAsset.empty() || batch.palettePacket == kInvalidSpritePalettePacket ||
                batch.palettePacket >= frame.palettePackets.size())
                return fail(error, "sprite indexed batch references a missing palette packet");
            const SpritePalettePacket& packet = frame.palettePackets[batch.palettePacket];
            if (packet.paletteAsset != batch.paletteAsset || packet.bank != batch.paletteBank ||
                packet.stateHash != batch.paletteStateHash)
                return fail(error, "sprite indexed batch does not match its palette packet");
            bool uploaded = false;
            if (!ensure_palette_resource(packet, uploaded, error)) return false;
            if (uploaded) ++stats.palettePacketUploads;
        }
        expectedItem += batch.itemCount;
    }
    if (expectedItem != frame.items.size())
        return fail(error, "sprite RHI batches do not cover every item");

    std::size_t vertexCount = 0U;
    std::size_t indexCount = 0U;
    std::size_t vertexBytes = 0U;
    std::size_t indexBytes = 0U;
    if (!checked_product(frame.items.size(), 4U, vertexCount) ||
        !checked_product(frame.items.size(), 6U, indexCount) ||
        !checked_product(vertexCount, sizeof(GpuSpriteVertex), vertexBytes) ||
        !checked_product(indexCount, sizeof(std::uint32_t), indexBytes) ||
        indexCount > std::numeric_limits<std::uint32_t>::max())
        return fail(error, "sprite RHI frame exceeds addressable buffer limits");

    std::vector<GpuSpriteVertex> vertices;
    std::vector<std::uint32_t> indices;
    vertices.reserve(vertexCount);
    indices.reserve(indexCount);
    const SpriteVec2 cameraCenter =
        frame.logicalCameraCenter.x == 0.0F && frame.logicalCameraCenter.y == 0.0F
        ? SpriteVec2{static_cast<float>(frame.presentation.logicalWidth) * 0.5F,
                     static_cast<float>(frame.presentation.logicalHeight) * 0.5F}
        : frame.logicalCameraCenter;
    for (const SpriteDrawItem& item : frame.items) {
        if (static_cast<unsigned>(item.plane) > static_cast<unsigned>(GameplayPlane2D::XZ))
            return fail(error, "sprite RHI draw item has an invalid gameplay plane");
        const std::uint32_t firstVertex = static_cast<std::uint32_t>(vertices.size());
        for (const SpriteVertex& source : item.vertices) {
            vertices.push_back(project_sprite_vertex(
                source, item.plane, frame.presentation, frame.cameraOrigin, cameraCenter));
        }
        indices.insert(indices.end(), {
            firstVertex, firstVertex + 1U, firstVertex + 2U,
            firstVertex, firstVertex + 2U, firstVertex + 3U,
        });
    }

    if (!vertices.empty()) {
        if (!ensure_buffers(vertexBytes, indexBytes, error) ||
            !device_.write_buffer(vertexBuffer_, 0U, std::as_bytes(std::span(vertices)), error) ||
            !device_.write_buffer(indexBuffer_, 0U, std::as_bytes(std::span(indices)), error))
            return false;
    }

    const rhi::CommandListHandle commands =
        device_.begin_commands(rhi::QueueKind::Graphics, "Sprite logical frame", error);
    if (!commands) return false;
    if (frame.colorBefore != rhi::ResourceState::RenderTarget &&
        !device_.transition_texture(commands, frame.colorTarget, frame.colorBefore,
                                    rhi::ResourceState::RenderTarget, error)) return false;
    rhi::RenderPassDesc pass;
    pass.debugName = "Sprite logical presentation";
    pass.colors.push_back({frame.colorTarget, frame.clear, frame.letterboxColor[0],
        frame.letterboxColor[1], frame.letterboxColor[2], frame.letterboxColor[3]});
    if (!device_.begin_render_pass(commands, pass, error)) return false;
    if (!device_.set_viewport(commands, {
            static_cast<float>(layout->viewportX), static_cast<float>(layout->viewportY),
            static_cast<float>(layout->viewportWidth), static_cast<float>(layout->viewportHeight),
            0.0F, 1.0F}, error)) return false;
    const std::int64_t right = std::min<std::int64_t>(frame.outputWidth,
        static_cast<std::int64_t>(layout->viewportX) + layout->viewportWidth);
    const std::int64_t bottom = std::min<std::int64_t>(frame.outputHeight,
        static_cast<std::int64_t>(layout->viewportY) + layout->viewportHeight);
    const std::int32_t left = std::max(0, layout->viewportX);
    const std::int32_t top = std::max(0, layout->viewportY);
    if (right <= left || bottom <= top ||
        !device_.set_scissor(commands, {left, top,
            static_cast<std::uint32_t>(right - left),
            static_cast<std::uint32_t>(bottom - top)}, error)) return false;

    if (!vertices.empty()) {
        if (!device_.bind_vertex_buffer(commands, 0U, vertexBuffer_, 0U,
                static_cast<std::uint32_t>(sizeof(GpuSpriteVertex)), error) ||
            !device_.bind_index_buffer(commands, indexBuffer_, 0U,
                rhi::IndexFormat::Uint32, error)) return false;
        for (const SpriteBatch& batch : frame.batches) {
            const bool indexed = !batch.paletteAsset.empty();
            const rhi::GraphicsPipelineHandle selectedPipeline =
                pipeline(batch.blendMode, indexed);
            if (!selectedPipeline ||
                !device_.bind_graphics_pipeline(commands, selectedPipeline, error)) return false;
            const auto found = textures_.find(batch.textureAsset);
            const TextureGroups* groups = found == textures_.end() ? &fallbackGroups_ : &found->second;
            if (found == textures_.end()) ++stats.fallbackTextureBatches;
            const rhi::BindGroupHandle group = batch.sampling == SpriteSampling::Nearest
                ? groups->nearest : groups->linear;
            if (!device_.bind_graphics_bind_group(commands, 0U, group, error)) return false;
            if (indexed) {
                const SpritePalettePacket& packet = frame.palettePackets[batch.palettePacket];
                const auto resource = paletteResources_.find(
                    std::make_pair(packet.paletteAsset, packet.stateHash));
                if (resource == paletteResources_.end() ||
                    !device_.bind_graphics_bind_group(commands, 1U, resource->second.group, error))
                    return false;
                ++stats.indexedPaletteBatches;
            }
            const std::size_t batchIndices = batch.itemCount * 6U;
            const std::size_t firstIndex = batch.firstItem * 6U;
            if (batchIndices > std::numeric_limits<std::uint32_t>::max() ||
                firstIndex > std::numeric_limits<std::uint32_t>::max() ||
                !device_.draw_indexed(commands, static_cast<std::uint32_t>(batchIndices), 1U,
                    static_cast<std::uint32_t>(firstIndex), 0, 0U, error)) return false;
            ++stats.drawCalls;
        }
    }
    if (!device_.end_render_pass(commands, error)) return false;
    if (frame.colorAfter != rhi::ResourceState::RenderTarget &&
        !device_.transition_texture(commands, frame.colorTarget,
            rhi::ResourceState::RenderTarget, frame.colorAfter, error)) return false;
    const rhi::FenceHandle submitted = device_.submit(commands, error);
    if (!submitted) return false;
    if (fence) *fence = submitted;
    stats.items = frame.items.size();
    stats.batches = frame.batches.size();
    stats.triangles = frame.items.size() * 2U;
    stats.uploadedVertexBytes = vertexBytes;
    stats.uploadedIndexBytes = indexBytes;
    return true;
}

} // namespace dve::render
