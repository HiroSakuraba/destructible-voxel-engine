#include "dve/render/text3d_renderer.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <utility>

namespace dve::render {
namespace {

void set_error(std::string* error, std::string_view message) {
    if (error) error->assign(message.begin(), message.end());
}

std::uint16_t float_to_half(float value) noexcept {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    const std::uint32_t sign = (bits >> 16U) & 0x8000U;
    std::int32_t exponent = static_cast<std::int32_t>((bits >> 23U) & 0xFFU) - 127 + 15;
    std::uint32_t mantissa = bits & 0x7FFFFFU;
    if (((bits >> 23U) & 0xFFU) == 0xFFU) {
        return static_cast<std::uint16_t>(sign | (mantissa == 0U ? 0x7C00U : 0x7E00U));
    }
    if (exponent <= 0) {
        if (exponent < -10) return static_cast<std::uint16_t>(sign);
        mantissa = (mantissa | 0x800000U) >> static_cast<std::uint32_t>(1 - exponent);
        if ((mantissa & 0x1000U) != 0U) mantissa += 0x2000U;
        return static_cast<std::uint16_t>(sign | (mantissa >> 13U));
    }
    if (exponent >= 31) return static_cast<std::uint16_t>(sign | 0x7C00U);
    if ((mantissa & 0x1000U) != 0U) {
        mantissa += 0x2000U;
        if ((mantissa & 0x800000U) != 0U) {
            mantissa = 0U;
            ++exponent;
            if (exponent >= 31) return static_cast<std::uint16_t>(sign | 0x7C00U);
        }
    }
    return static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(exponent) << 10U) |
                                      (mantissa >> 13U));
}

std::vector<std::uint16_t> pack_curve_atlas(const SlugAtlas& atlas) {
    const std::size_t texelCount = static_cast<std::size_t>(SlugAtlas::kTextureWidth) *
                                   std::max(1U, atlas.curveTextureHeight);
    std::vector<std::uint16_t> packed(texelCount * 4U, 0U);
    for (std::size_t i = 0; i < atlas.curveTexels.size(); ++i) {
        const Float4 value = atlas.curveTexels[i];
        packed[i * 4U] = float_to_half(value.x);
        packed[i * 4U + 1U] = float_to_half(value.y);
        packed[i * 4U + 2U] = float_to_half(value.z);
        packed[i * 4U + 3U] = float_to_half(value.w);
    }
    return packed;
}

std::vector<std::uint16_t> pack_band_atlas(const SlugAtlas& atlas) {
    const std::size_t texelCount = static_cast<std::size_t>(SlugAtlas::kTextureWidth) *
                                   std::max(1U, atlas.bandTextureHeight);
    std::vector<std::uint16_t> packed(texelCount * 2U, 0U);
    for (std::size_t i = 0; i < atlas.bandTexels.size(); ++i) {
        packed[i * 2U] = atlas.bandTexels[i].x;
        packed[i * 2U + 1U] = atlas.bandTexels[i].y;
    }
    return packed;
}

bool upload_buffer(rhi::IDevice& device, rhi::BufferHandle& handle, std::span<const std::byte> bytes,
                   rhi::BufferUsage usage, std::string_view name, std::string* error) {
    if (bytes.empty()) {
        set_error(error, "3D text GPU buffer cannot be empty");
        return false;
    }
    rhi::BufferDesc desc;
    desc.bytes = bytes.size();
    desc.usage = usage | rhi::BufferUsage::CopyDestination;
    desc.memory = rhi::MemoryDomain::DeviceLocal;
    desc.initialState = rhi::ResourceState::CopyDestination;
    desc.debugName = std::string(name);
    handle = device.create_buffer(desc, error);
    if (!handle || !device.write_buffer(handle, 0U, bytes, error)) return false;
    const auto commands = device.begin_commands(rhi::QueueKind::Graphics, "3D text buffer transition", error);
    if (!commands || !device.transition_buffer(commands, handle, rhi::ResourceState::CopyDestination,
                                                rhi::ResourceState::ShaderRead, error) ||
        !device.submit(commands, error)) return false;
    return true;
}

bool upload_texture(rhi::IDevice& device, rhi::TextureHandle& texture, rhi::TextureViewHandle& view,
                    rhi::TextureFormat format, std::uint32_t width, std::uint32_t height,
                    std::span<const std::byte> bytes, std::size_t rowPitch, std::string_view name,
                    std::string* error) {
    rhi::TextureDesc desc;
    desc.format = format;
    desc.width = width;
    desc.height = std::max(1U, height);
    desc.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDestination |
                 rhi::TextureUsage::CopySource;
    desc.initialState = rhi::ResourceState::CopyDestination;
    desc.debugName = std::string(name);
    texture = device.create_texture(desc, error);
    if (!texture || !device.write_texture(texture, 0U, 0U, bytes, rowPitch, error)) return false;
    const auto commands = device.begin_commands(rhi::QueueKind::Graphics, "3D text atlas transition", error);
    if (!commands || !device.transition_texture(commands, texture, rhi::ResourceState::CopyDestination,
                                                 rhi::ResourceState::ShaderRead, error) ||
        !device.submit(commands, error)) return false;
    view = device.create_texture_view({texture, 0U, 1U, 0U, 1U, std::string(name) + " view"}, error);
    return static_cast<bool>(view);
}

const CookedText3DAsset* find_asset(std::span<const CookedText3DAsset> assets,
                                    std::uint64_t id) noexcept {
    const auto it = std::find_if(assets.begin(), assets.end(),
                                 [id](const CookedText3DAsset& asset) { return asset.objectId == id; });
    return it == assets.end() ? nullptr : &*it;
}

int pass_rank(Text3DPassKind pass) noexcept {
    switch (pass) {
    case Text3DPassKind::BackFaces: return 0;
    case Text3DPassKind::ExtrusionSides: return 1;
    case Text3DPassKind::FrontFaces: return 2;
    case Text3DPassKind::SelectionOverlay: return 3;
    }
    return 4;
}

rhi::BindGroupHandle object_binding(
    std::span<const Text3DObjectBinding> bindings, std::uint64_t objectId) noexcept {
    const auto iterator = std::find_if(bindings.begin(), bindings.end(),
        [objectId](const Text3DObjectBinding& binding) { return binding.objectId == objectId; });
    return iterator == bindings.end() ? rhi::BindGroupHandle{} : iterator->constantsGroup;
}

} // namespace

Text3DGpuCache::~Text3DGpuCache() { clear(); }

bool Text3DGpuCache::destroy(Text3DGpuAsset& asset, std::string* error) noexcept {
    bool ok = true;
    const auto step = [&](bool result) { ok = result && ok; };
    if (asset.atlasGroup) step(device_.destroy_bind_group(asset.atlasGroup, error));
    if (asset.atlasLayout) step(device_.destroy_bind_group_layout(asset.atlasLayout, error));
    if (asset.atlasSampler) step(device_.destroy_sampler(asset.atlasSampler, error));
    if (asset.curveView) step(device_.destroy_texture_view(asset.curveView, error));
    if (asset.bandView) step(device_.destroy_texture_view(asset.bandView, error));
    if (asset.curveTexture) step(device_.destroy_texture(asset.curveTexture, error));
    if (asset.bandTexture) step(device_.destroy_texture(asset.bandTexture, error));
    if (asset.faceVertexBuffer) step(device_.destroy_buffer(asset.faceVertexBuffer, error));
    if (asset.faceIndexBuffer) step(device_.destroy_buffer(asset.faceIndexBuffer, error));
    if (asset.sideVertexBuffer) step(device_.destroy_buffer(asset.sideVertexBuffer, error));
    if (asset.sideIndexBuffer) step(device_.destroy_buffer(asset.sideIndexBuffer, error));
    asset = {};
    return ok;
}

bool Text3DGpuCache::upload(const CookedText3DAsset& asset, std::string* error) {
    std::string validation;
    if (!validate_text3d_asset(asset, &validation)) {
        set_error(error, validation);
        ++stats_.failures;
        return false;
    }
    if (const auto existing = assets_.find(asset.objectId); existing != assets_.end() &&
        existing->second.contentHash == asset.contentHash) {
        ++stats_.unchanged;
        return true;
    }

    Text3DGpuAsset staged;
    staged.assetId = asset.objectId;
    staged.contentHash = asset.contentHash;
    staged.curveWidth = SlugAtlas::kTextureWidth;
    staged.curveHeight = std::max(1U, asset.atlas.curveTextureHeight);
    staged.bandWidth = SlugAtlas::kTextureWidth;
    staged.bandHeight = std::max(1U, asset.atlas.bandTextureHeight);
    staged.glyphCount = static_cast<std::uint32_t>(asset.glyphInstances.size());

    const auto facePacket = build_text3d_render_packet(asset);
    staged.faceIndexCount = static_cast<std::uint32_t>(facePacket.faceIndices.size());
    staged.sideIndexCount = static_cast<std::uint32_t>(asset.sideMesh.indices.size());
    const auto curve = pack_curve_atlas(asset.atlas);
    const auto band = pack_band_atlas(asset.atlas);

    bool ok = upload_texture(device_, staged.curveTexture, staged.curveView,
                             rhi::TextureFormat::RGBA16Float, staged.curveWidth, staged.curveHeight,
                             std::as_bytes(std::span<const std::uint16_t>(curve)),
                             static_cast<std::size_t>(staged.curveWidth) * 8U,
                             "Slug curve atlas", error) &&
              upload_texture(device_, staged.bandTexture, staged.bandView,
                             rhi::TextureFormat::RG16Uint, staged.bandWidth, staged.bandHeight,
                             std::as_bytes(std::span<const std::uint16_t>(band)),
                             static_cast<std::size_t>(staged.bandWidth) * 4U,
                             "Slug band atlas", error) &&
              upload_buffer(device_, staged.faceVertexBuffer,
                            std::as_bytes(std::span<const SlugTextFaceVertex>(facePacket.faceVertices)),
                            rhi::BufferUsage::Vertex, "Slug face vertices", error) &&
              upload_buffer(device_, staged.faceIndexBuffer,
                            std::as_bytes(std::span<const std::uint32_t>(facePacket.faceIndices)),
                            rhi::BufferUsage::Index, "Slug face indices", error) &&
              upload_buffer(device_, staged.sideVertexBuffer,
                            std::as_bytes(std::span<const PolygonVertex>(asset.sideMesh.vertices)),
                            rhi::BufferUsage::Vertex, "3D text side vertices", error) &&
              upload_buffer(device_, staged.sideIndexBuffer,
                            std::as_bytes(std::span<const std::uint32_t>(asset.sideMesh.indices)),
                            rhi::BufferUsage::Index, "3D text side indices", error);

    if (ok) {
        rhi::SamplerDesc sampler;
        sampler.minFilter = rhi::FilterMode::Linear;
        sampler.magFilter = rhi::FilterMode::Linear;
        sampler.mipmapFilter = rhi::MipmapFilterMode::Nearest;
        sampler.addressU = rhi::AddressMode::ClampToEdge;
        sampler.addressV = rhi::AddressMode::ClampToEdge;
        sampler.addressW = rhi::AddressMode::ClampToEdge;
        sampler.maximumLod = 0.0F;
        sampler.debugName = "Slug text atlas sampler";
        staged.atlasSampler = device_.create_sampler(sampler, error);
        ok = static_cast<bool>(staged.atlasSampler);
    }
    if (ok) {
        rhi::BindGroupLayoutDesc layout;
        layout.debugName = "Slug text atlas layout";
        layout.bindings = {
            {20U, rhi::BindingType::SampledTexture, rhi::ShaderStage::Fragment},
            {21U, rhi::BindingType::SampledTexture, rhi::ShaderStage::Fragment},
        };
        staged.atlasLayout = device_.create_bind_group_layout(layout, error);
        ok = static_cast<bool>(staged.atlasLayout);
    }
    if (ok) {
        rhi::BindGroupDesc group;
        group.layout = staged.atlasLayout;
        group.debugName = "Slug text atlas group";
        group.entries = {{20U, {}, staged.curveView, 0U, 0U, staged.atlasSampler},
                         {21U, {}, staged.bandView, 0U, 0U, staged.atlasSampler}};
        staged.atlasGroup = device_.create_bind_group(group, error);
        ok = static_cast<bool>(staged.atlasGroup);
    }
    staged.residentBytes = curve.size() * sizeof(std::uint16_t) +
                           band.size() * sizeof(std::uint16_t) +
                           facePacket.faceVertices.size() * sizeof(SlugTextFaceVertex) +
                           facePacket.faceIndices.size() * sizeof(std::uint32_t) +
                           asset.sideMesh.vertices.size() * sizeof(PolygonVertex) +
                           asset.sideMesh.indices.size() * sizeof(std::uint32_t);
    if (!ok) {
        (void)destroy(staged, nullptr);
        ++stats_.failures;
        return false;
    }

    if (auto existing = assets_.find(asset.objectId); existing != assets_.end()) {
        Text3DGpuAsset old = std::move(existing->second);
        existing->second = std::move(staged);
        stats_.residentBytes = stats_.residentBytes - old.residentBytes + existing->second.residentBytes;
        ++stats_.replacements;
        (void)destroy(old, nullptr);
    } else {
        stats_.residentBytes += staged.residentBytes;
        assets_.emplace(asset.objectId, std::move(staged));
    }
    ++stats_.uploads;
    stats_.residentAssets = assets_.size();
    return true;
}

const Text3DGpuAsset* Text3DGpuCache::find(std::uint64_t assetId) const noexcept {
    const auto it = assets_.find(assetId);
    return it == assets_.end() ? nullptr : &it->second;
}

bool Text3DGpuCache::remove(std::uint64_t assetId, std::string* error) {
    const auto it = assets_.find(assetId);
    if (it == assets_.end()) return false;
    const std::size_t bytes = it->second.residentBytes;
    if (!destroy(it->second, error)) return false;
    assets_.erase(it);
    stats_.residentBytes -= bytes;
    stats_.residentAssets = assets_.size();
    ++stats_.removals;
    return true;
}

void Text3DGpuCache::clear() noexcept {
    for (auto& [id, asset] : assets_) {
        (void)id;
        (void)destroy(asset, nullptr);
    }
    assets_.clear();
    stats_.residentAssets = 0U;
    stats_.residentBytes = 0U;
}

Text3DGpuCacheStats Text3DGpuCache::stats() const noexcept {
    Text3DGpuCacheStats result = stats_;
    result.residentAssets = assets_.size();
    return result;
}

bool Text3DFramePlan::validate(std::string* error) const noexcept {
    const auto fail = [&](std::string_view message) {
        set_error(error, message);
        return false;
    };
    int previous = -1;
    std::uint64_t faceCount{}, sideCount{}, selectionCount{};
    for (const auto& packet : packets) {
        if (packet.objectId == 0U || packet.assetId == 0U || packet.gpu == nullptr)
            return fail("3D text frame packet has an invalid identity or GPU asset");
        if (pass_rank(packet.pass) < previous) return fail("3D text frame passes are out of order");
        previous = pass_rank(packet.pass);
        if (packet.pass == Text3DPassKind::FrontFaces || packet.pass == Text3DPassKind::BackFaces) {
            if (packet.indexCount != 6U || packet.firstIndex + packet.indexCount > packet.gpu->faceIndexCount)
                return fail("3D text face draw range is invalid");
            ++faceCount;
        } else if (packet.pass == Text3DPassKind::ExtrusionSides) {
            if (packet.indexCount != packet.gpu->sideIndexCount || packet.indexCount == 0U)
                return fail("3D text side draw range is invalid");
            ++sideCount;
        } else {
            ++selectionCount;
        }
    }
    if (faceCount != faceDraws || sideCount != sideDraws || selectionCount != selectionDraws)
        return fail("3D text frame statistics drift");
    return true;
}

Text3DFramePlan make_text3d_frame_plan(std::span<const Text3DRenderInstance> instances,
                                       const Text3DGpuCache& cache,
                                       std::span<const CookedText3DAsset> assets) {
    Text3DFramePlan plan;
    struct Resolved { const Text3DRenderInstance* instance{}; const CookedText3DAsset* asset{}; const Text3DGpuAsset* gpu{}; };
    std::vector<Resolved> resolved;
    for (const auto& instance : instances) {
        if (!instance.visible) continue;
        ++plan.submittedInstances;
        const auto* gpu = cache.find(instance.assetId);
        const auto* asset = find_asset(assets, instance.assetId);
        if (!gpu || !asset) { ++plan.missingAssets; continue; }
        resolved.push_back({&instance, asset, gpu});
    }
    for (const auto& item : resolved) {
        for (std::uint32_t glyph = 0U; glyph < item.gpu->glyphCount; ++glyph) {
            plan.packets.push_back({Text3DPassKind::BackFaces, item.instance->objectId,
                item.instance->assetId, item.instance->transform, item.gpu, glyph * 12U + 6U, 6U,
                0, item.asset->style.faceMaterialId, item.instance->selected});
            ++plan.faceDraws;
        }
    }
    for (const auto& item : resolved) {
        plan.packets.push_back({Text3DPassKind::ExtrusionSides, item.instance->objectId,
            item.instance->assetId, item.instance->transform, item.gpu, 0U, item.gpu->sideIndexCount,
            0, item.asset->style.sideMaterialId, item.instance->selected});
        ++plan.sideDraws;
    }
    for (const auto& item : resolved) {
        for (std::uint32_t glyph = 0U; glyph < item.gpu->glyphCount; ++glyph) {
            plan.packets.push_back({Text3DPassKind::FrontFaces, item.instance->objectId,
                item.instance->assetId, item.instance->transform, item.gpu, glyph * 12U, 6U,
                0, item.asset->style.faceMaterialId, item.instance->selected});
            ++plan.faceDraws;
        }
    }
    for (const auto& item : resolved) if (item.instance->selected) {
        plan.packets.push_back({Text3DPassKind::SelectionOverlay, item.instance->objectId,
            item.instance->assetId, item.instance->transform, item.gpu, 0U, 0U, 0,
            item.asset->style.faceMaterialId, true});
        ++plan.selectionDraws;
    }
    return plan;
}

Text3DFramePlan make_text3d_frame_plan(const RuntimeText3DSceneSnapshot& snapshot,
                                       const Text3DGpuCache& cache) {
    std::vector<Text3DRenderInstance> instances;
    instances.reserve(snapshot.instances.size());
    for (const auto& instance : snapshot.instances) {
        instances.push_back({instance.objectId, instance.assetId, instance.worldTransform,
                             instance.visible, instance.selected, instance.castShadows,
                             instance.receiveGlobalIllumination});
    }
    return make_text3d_frame_plan(instances, cache, snapshot.assets);
}

bool record_text3d_face_pass(rhi::IDevice& device, rhi::CommandListHandle commands,
                             rhi::GraphicsPipelineHandle pipeline, const Text3DFramePlan& plan,
                             Text3DPassKind pass, rhi::BindGroupHandle constantsGroup,
                             std::string* error) {
    if (pass != Text3DPassKind::FrontFaces && pass != Text3DPassKind::BackFaces) {
        set_error(error, "record_text3d_face_pass requires a front- or back-face pass");
        return false;
    }
    std::string validation;
    if (!plan.validate(&validation)) { set_error(error, validation); return false; }
    if (!device.bind_graphics_pipeline(commands, pipeline, error)) return false;
    if (constantsGroup && !device.bind_graphics_bind_group(commands, 0U, constantsGroup, error)) return false;
    const Text3DGpuAsset* bound = nullptr;
    for (const auto& packet : plan.packets) {
        if (packet.pass != pass) continue;
        if (packet.gpu != bound) {
            bound = packet.gpu;
            if (!device.bind_vertex_buffer(commands, 0U, bound->faceVertexBuffer, 0U,
                                           static_cast<std::uint32_t>(sizeof(SlugTextFaceVertex)), error) ||
                !device.bind_index_buffer(commands, bound->faceIndexBuffer, 0U,
                                          rhi::IndexFormat::Uint32, error) ||
                !device.bind_graphics_bind_group(commands, 1U, bound->atlasGroup, error)) return false;
        }
        if (!device.draw_indexed(commands, packet.indexCount, 1U, packet.firstIndex,
                                 packet.vertexOffset, 0U, error)) return false;
    }
    return true;
}

bool record_text3d_bound_face_pass(
    rhi::IDevice& device, rhi::CommandListHandle commands, rhi::GraphicsPipelineHandle pipeline,
    const Text3DFramePlan& plan, Text3DPassKind pass,
    std::span<const Text3DObjectBinding> objectBindings, std::string* error) {
    if (pass != Text3DPassKind::FrontFaces && pass != Text3DPassKind::BackFaces) {
        set_error(error, "record_text3d_bound_face_pass requires a front- or back-face pass");
        return false;
    }
    std::string validation;
    if (!plan.validate(&validation)) { set_error(error, validation); return false; }
    if (!device.bind_graphics_pipeline(commands, pipeline, error)) return false;
    const Text3DGpuAsset* boundAsset = nullptr;
    std::uint64_t boundObject = 0U;
    for (const auto& packet : plan.packets) {
        if (packet.pass != pass) continue;
        if (packet.objectId != boundObject) {
            boundObject = packet.objectId;
            const auto constants = object_binding(objectBindings, packet.objectId);
            if (!constants) {
                set_error(error, "3D text draw is missing its object constants bind group");
                return false;
            }
            if (!device.bind_graphics_bind_group(commands, 0U, constants, error)) return false;
        }
        if (packet.gpu != boundAsset) {
            boundAsset = packet.gpu;
            if (!device.bind_vertex_buffer(commands, 0U, boundAsset->faceVertexBuffer, 0U,
                                           static_cast<std::uint32_t>(sizeof(SlugTextFaceVertex)), error) ||
                !device.bind_index_buffer(commands, boundAsset->faceIndexBuffer, 0U,
                                          rhi::IndexFormat::Uint32, error) ||
                !device.bind_graphics_bind_group(commands, 1U, boundAsset->atlasGroup, error)) return false;
        }
        if (!device.draw_indexed(commands, packet.indexCount, 1U, packet.firstIndex,
                                 packet.vertexOffset, 0U, error)) return false;
    }
    return true;
}

bool record_text3d_side_pass(
    rhi::IDevice& device, rhi::CommandListHandle commands, rhi::GraphicsPipelineHandle pipeline,
    const Text3DFramePlan& plan, std::span<const Text3DObjectBinding> objectBindings,
    std::string* error) {
    std::string validation;
    if (!plan.validate(&validation)) { set_error(error, validation); return false; }
    if (!device.bind_graphics_pipeline(commands, pipeline, error)) return false;
    const Text3DGpuAsset* boundAsset = nullptr;
    std::uint64_t boundObject = 0U;
    for (const auto& packet : plan.packets) {
        if (packet.pass != Text3DPassKind::ExtrusionSides) continue;
        if (!objectBindings.empty() && packet.objectId != boundObject) {
            boundObject = packet.objectId;
            const auto constants = object_binding(objectBindings, packet.objectId);
            if (!constants) {
                set_error(error, "3D text side draw is missing its object constants bind group");
                return false;
            }
            if (!device.bind_graphics_bind_group(commands, 0U, constants, error)) return false;
        }
        if (packet.gpu != boundAsset) {
            boundAsset = packet.gpu;
            if (!device.bind_vertex_buffer(commands, 0U, boundAsset->sideVertexBuffer, 0U,
                                           static_cast<std::uint32_t>(sizeof(PolygonVertex)), error) ||
                !device.bind_index_buffer(commands, boundAsset->sideIndexBuffer, 0U,
                                          rhi::IndexFormat::Uint32, error)) return false;
        }
        if (!device.draw_indexed(commands, packet.indexCount, 1U, packet.firstIndex,
                                 packet.vertexOffset, 0U, error)) return false;
    }
    return true;
}

bool record_text3d_frame(
    rhi::IDevice& device, rhi::CommandListHandle commands, const Text3DRenderPipelines& pipelines,
    const Text3DFramePlan& plan, std::span<const Text3DObjectBinding> objectBindings,
    std::string* error) {
    if (!pipelines.backFaces || !pipelines.extrusionSides || !pipelines.frontFaces) {
        set_error(error, "3D text frame requires back-face, side, and front-face pipelines");
        return false;
    }
    if (objectBindings.empty()) {
        if (!record_text3d_face_pass(device, commands, pipelines.backFaces, plan,
                                     Text3DPassKind::BackFaces, {}, error) ||
            !record_text3d_side_pass(device, commands, pipelines.extrusionSides, plan, {}, error) ||
            !record_text3d_face_pass(device, commands, pipelines.frontFaces, plan,
                                     Text3DPassKind::FrontFaces, {}, error)) return false;
    } else {
        if (!record_text3d_bound_face_pass(device, commands, pipelines.backFaces, plan,
                                           Text3DPassKind::BackFaces, objectBindings, error) ||
            !record_text3d_side_pass(device, commands, pipelines.extrusionSides, plan,
                                     objectBindings, error) ||
            !record_text3d_bound_face_pass(device, commands, pipelines.frontFaces, plan,
                                           Text3DPassKind::FrontFaces, objectBindings, error)) return false;
    }
    // Selection packets are metadata consumed by the editor outline/composite pass. The selection
    // pipeline is optional because the main editor can instead use its object-ID outline pass.
    (void)pipelines.selectionOverlay;
    return true;
}

} // namespace dve::render
