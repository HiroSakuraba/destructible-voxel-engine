#include "dve/render/dashr_atlas.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace dve::render {
namespace {

void set_error(std::string* error, std::string message) {
    if (error) *error = std::move(message);
}

std::uint16_t float_to_half(float value) noexcept {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    const std::uint32_t sign = (bits >> 16U) & 0x8000U;
    std::int32_t exponent = static_cast<std::int32_t>((bits >> 23U) & 0xFFU) - 127 + 15;
    std::uint32_t mantissa = bits & 0x7FFFFFU;
    if (((bits >> 23U) & 0xFFU) == 0xFFU)
        return static_cast<std::uint16_t>(sign | (mantissa == 0U ? 0x7C00U : 0x7E00U));
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
            mantissa = 0U; ++exponent;
            if (exponent >= 31) return static_cast<std::uint16_t>(sign | 0x7C00U);
        }
    }
    return static_cast<std::uint16_t>(
        sign | (static_cast<std::uint32_t>(exponent) << 10U) | (mantissa >> 13U));
}

std::vector<std::uint16_t> pack_rgba16f(std::span<const Float4> values) {
    std::vector<std::uint16_t> result(values.size() * 4U);
    for (std::size_t index = 0U; index < values.size(); ++index) {
        result[index * 4U] = float_to_half(values[index].x);
        result[index * 4U + 1U] = float_to_half(values[index].y);
        result[index * 4U + 2U] = float_to_half(values[index].z);
        result[index * 4U + 3U] = float_to_half(values[index].w);
    }
    return result;
}

bool all_handles(const std::array<rhi::TextureHandle, kDashrAtlasTargetCount>& handles) {
    return std::all_of(handles.begin(), handles.end(),
                       [](rhi::TextureHandle handle) { return static_cast<bool>(handle); });
}
bool all_views(const std::array<rhi::TextureViewHandle, kDashrAtlasTargetCount>& handles) {
    return std::all_of(handles.begin(), handles.end(),
                       [](rhi::TextureViewHandle handle) { return static_cast<bool>(handle); });
}

} // namespace

bool DashrAtlasResources::valid() const noexcept {
    return resolution > 0U && all_handles(rawTextures) && all_views(rawViews) &&
           all_handles(filledTextures) && all_views(filledViews) &&
           seamTexture && seamView && linearClampSampler && pointClampSampler &&
           edgeFillLayout && edgeFillGroup && edgeFillPipeline &&
           consumerLayout && consumerGroup && atlasPipeline;
}

bool destroy_dashr_atlas_resources(
    rhi::IDevice& device, DashrAtlasResources& r, std::string* error) {
    bool ok = true;
    std::string local;
    auto destroy = [&](bool result) {
        if (!result) {
            ok = false;
            if (error && error->empty()) *error = local;
        }
        local.clear();
    };

    if (r.atlasPipeline) destroy(device.destroy_graphics_pipeline(r.atlasPipeline, &local));
    if (r.edgeFillPipeline) destroy(device.destroy_compute_pipeline(r.edgeFillPipeline, &local));
    if (r.edgeFillGroup) destroy(device.destroy_bind_group(r.edgeFillGroup, &local));
    if (r.consumerGroup) destroy(device.destroy_bind_group(r.consumerGroup, &local));
    if (r.edgeFillLayout) destroy(device.destroy_bind_group_layout(r.edgeFillLayout, &local));
    if (r.consumerLayout) destroy(device.destroy_bind_group_layout(r.consumerLayout, &local));
    if (r.seamView) destroy(device.destroy_texture_view(r.seamView, &local));
    for (auto view : r.rawViews) if (view) destroy(device.destroy_texture_view(view, &local));
    for (auto view : r.filledViews) if (view) destroy(device.destroy_texture_view(view, &local));
    if (r.linearClampSampler) destroy(device.destroy_sampler(r.linearClampSampler, &local));
    if (r.pointClampSampler) destroy(device.destroy_sampler(r.pointClampSampler, &local));
    if (r.seamTexture) destroy(device.destroy_texture(r.seamTexture, &local));
    for (auto texture : r.rawTextures) if (texture) destroy(device.destroy_texture(texture, &local));
    for (auto texture : r.filledTextures) if (texture) destroy(device.destroy_texture(texture, &local));
    r = {};
    return ok;
}

bool create_dashr_atlas_resources(
    rhi::IDevice& device,
    const DashrAtlasShaderBytecode& bytecode,
    std::uint32_t resolution,
    DashrAtlasResources& out,
    std::string* error) {
    if (!bytecode.valid() || resolution < 32U || resolution > 2048U ||
        resolution > device.capabilities().maxTextureDimension2D) {
        set_error(error, "DASHR atlas creation arguments are invalid");
        return false;
    }
    const auto formatCaps = device.texture_format_capabilities(rhi::TextureFormat::RGBA16Float);
    if (!formatCaps.sampled || !formatCaps.renderTarget || !formatCaps.storage) {
        set_error(error, "DASHR requires sampled/render-target/storage RGBA16F support");
        return false;
    }

    DashrAtlasResources r;
    r.resolution = resolution;
    std::string local;
    auto fail = [&](std::string message) {
        std::string ignored;
        (void)destroy_dashr_atlas_resources(device, r, &ignored);
        set_error(error, std::move(message));
        return false;
    };

    rhi::TextureDesc rawDesc;
    rawDesc.format = rhi::TextureFormat::RGBA16Float;
    rawDesc.width = resolution;
    rawDesc.height = resolution;
    rawDesc.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled |
                    rhi::TextureUsage::CopySource;
    rawDesc.initialState = rhi::ResourceState::RenderTarget;

    rhi::TextureDesc filledDesc = rawDesc;
    filledDesc.usage = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled |
                       rhi::TextureUsage::CopySource;
    filledDesc.initialState = rhi::ResourceState::ShaderWrite;

    for (std::uint32_t index = 0U; index < kDashrAtlasTargetCount; ++index) {
        rawDesc.debugName = "DASHR raw deformation atlas " + std::to_string(index);
        r.rawTextures[index] = device.create_texture(rawDesc, &local);
        if (!r.rawTextures[index]) return fail(local);
        rhi::TextureViewDesc rawView;
        rawView.texture = r.rawTextures[index];
        rawView.debugName = "DASHR raw deformation atlas view " + std::to_string(index);
        r.rawViews[index] = device.create_texture_view(rawView, &local);
        if (!r.rawViews[index]) return fail(local);

        filledDesc.debugName = "DASHR edge-filled deformation atlas " + std::to_string(index);
        r.filledTextures[index] = device.create_texture(filledDesc, &local);
        if (!r.filledTextures[index]) return fail(local);
        rhi::TextureViewDesc filledView;
        filledView.texture = r.filledTextures[index];
        filledView.debugName = "DASHR edge-filled deformation atlas view " + std::to_string(index);
        r.filledViews[index] = device.create_texture_view(filledView, &local);
        if (!r.filledViews[index]) return fail(local);
    }

    rhi::TextureDesc seamDesc;
    seamDesc.format = rhi::TextureFormat::RGBA16Float;
    seamDesc.width = resolution;
    seamDesc.height = resolution;
    seamDesc.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDestination;
    seamDesc.initialState = rhi::ResourceState::ShaderRead;
    seamDesc.debugName = "DASHR seam teleport map";
    r.seamTexture = device.create_texture(seamDesc, &local);
    if (!r.seamTexture) return fail(local);
    rhi::TextureViewDesc seamView;
    seamView.texture = r.seamTexture;
    seamView.debugName = "DASHR seam teleport map view";
    r.seamView = device.create_texture_view(seamView, &local);
    if (!r.seamView) return fail(local);

    rhi::SamplerDesc linear;
    linear.minFilter = rhi::FilterMode::Linear;
    linear.magFilter = rhi::FilterMode::Linear;
    linear.mipmapFilter = rhi::MipmapFilterMode::Nearest;
    linear.addressU = linear.addressV = linear.addressW = rhi::AddressMode::ClampToEdge;
    linear.debugName = "DASHR linear clamp sampler";
    r.linearClampSampler = device.create_sampler(linear, &local);
    if (!r.linearClampSampler) return fail(local);
    rhi::SamplerDesc point = linear;
    point.minFilter = point.magFilter = rhi::FilterMode::Nearest;
    point.debugName = "DASHR point clamp sampler";
    r.pointClampSampler = device.create_sampler(point, &local);
    if (!r.pointClampSampler) return fail(local);

    rhi::BindGroupLayoutDesc edgeLayout;
    edgeLayout.debugName = "DASHR bounded edge-fill layout";
    for (std::uint32_t binding = 0U; binding < 4U; ++binding)
        edgeLayout.bindings.push_back(
            {binding, rhi::BindingType::SampledTexture, rhi::ShaderStage::Compute});
    for (std::uint32_t binding = 4U; binding < 8U; ++binding)
        edgeLayout.bindings.push_back(
            {binding, rhi::BindingType::StorageTexture, rhi::ShaderStage::Compute});
    r.edgeFillLayout = device.create_bind_group_layout(edgeLayout, &local);
    if (!r.edgeFillLayout) return fail(local);

    rhi::BindGroupDesc edgeGroup;
    edgeGroup.layout = r.edgeFillLayout;
    edgeGroup.debugName = "DASHR bounded edge-fill group";
    for (std::uint32_t binding = 0U; binding < 4U; ++binding)
        edgeGroup.entries.push_back(
            {binding, {}, r.rawViews[binding], 0U, 0U, r.pointClampSampler});
    for (std::uint32_t binding = 4U; binding < 8U; ++binding)
        edgeGroup.entries.push_back(
            {binding, {}, r.filledViews[binding - 4U], 0U, 0U, {}});
    r.edgeFillGroup = device.create_bind_group(edgeGroup, &local);
    if (!r.edgeFillGroup) return fail(local);

    rhi::BindGroupLayoutDesc consumerLayout;
    consumerLayout.debugName = "DASHR surface consumer layout";
    for (std::uint32_t binding = 0U; binding < 6U; ++binding)
        consumerLayout.bindings.push_back(
            {binding, rhi::BindingType::SampledTexture, rhi::ShaderStage::Fragment});
    r.consumerLayout = device.create_bind_group_layout(consumerLayout, &local);
    if (!r.consumerLayout) return fail(local);

    rhi::BindGroupDesc consumerGroup;
    consumerGroup.layout = r.consumerLayout;
    consumerGroup.debugName = "DASHR surface consumer group";
    for (std::uint32_t binding = 0U; binding < 4U; ++binding)
        consumerGroup.entries.push_back(
            {binding, {}, r.filledViews[binding], 0U, 0U, r.linearClampSampler});
    consumerGroup.entries.push_back({4U, {}, r.seamView, 0U, 0U, r.linearClampSampler});
    consumerGroup.entries.push_back({5U, {}, r.seamView, 0U, 0U, r.pointClampSampler});
    r.consumerGroup = device.create_bind_group(consumerGroup, &local);
    if (!r.consumerGroup) return fail(local);

    rhi::GraphicsPipelineDesc atlasPipeline;
    atlasPipeline.debugName = "DASHR UV-space deformation atlas";
    atlasPipeline.vertexBytecode = bytecode.atlasVertex;
    atlasPipeline.fragmentBytecode = bytecode.atlasFragment;
    atlasPipeline.colorFormat.reset();
    atlasPipeline.colorFormats.assign(kDashrAtlasTargetCount, rhi::TextureFormat::RGBA16Float);
    atlasPipeline.depthFormat.reset();
    atlasPipeline.cullMode = rhi::CullMode::Disabled;
    atlasPipeline.depthTest = false;
    atlasPipeline.depthWrite = false;
    atlasPipeline.vertexBuffer = rhi::VertexBufferLayoutDesc{
        static_cast<std::uint32_t>(sizeof(GpuPolygonVertex)), false};
    atlasPipeline.vertexAttributes = {
        {0U, rhi::VertexFormat::Float3, 0U},
        {1U, rhi::VertexFormat::Float3, 3U * sizeof(float)},
        {2U, rhi::VertexFormat::Float4, 6U * sizeof(float)},
        {3U, rhi::VertexFormat::Float2, 10U * sizeof(float)},
    };
    r.atlasPipeline = device.create_graphics_pipeline(atlasPipeline, &local);
    if (!r.atlasPipeline) return fail(local);

    rhi::ComputePipelineDesc edgePipeline;
    edgePipeline.debugName = "DASHR bounded two-pixel edge fill";
    edgePipeline.bytecode = bytecode.edgeFillCompute;
    edgePipeline.bindGroupLayouts = {r.edgeFillLayout};
    edgePipeline.threadsX = 8U; edgePipeline.threadsY = 8U; edgePipeline.threadsZ = 1U;
    r.edgeFillPipeline = device.create_compute_pipeline(edgePipeline, &local);
    if (!r.edgeFillPipeline) return fail(local);

    // Default map means no seam teleport anywhere.
    const std::size_t texelCount = static_cast<std::size_t>(resolution) * resolution;
    std::vector<Float4> emptySeam(texelCount, Float4{0.0F, 0.0F, -1.0F, 0.0F});
    const auto packed = pack_rgba16f(emptySeam);
    if (!device.write_texture(r.seamTexture, 0U, 0U,
                              std::as_bytes(std::span(packed)),
                              static_cast<std::size_t>(resolution) * 8U, &local))
        return fail(local);

    if (out.valid()) {
        std::string ignored;
        (void)destroy_dashr_atlas_resources(device, out, &ignored);
    }
    out = std::move(r);
    return true;
}

bool upload_dashr_seam_map(
    rhi::IDevice& device,
    DashrAtlasResources& resources,
    std::span<const Float4> seamTexels,
    std::string* error) {
    if (!resources.valid() ||
        seamTexels.size() != static_cast<std::size_t>(resources.resolution) *
                             resources.resolution) {
        set_error(error, "DASHR seam map dimensions do not match the atlas");
        return false;
    }
    const auto packed = pack_rgba16f(seamTexels);
    return device.write_texture(
        resources.seamTexture, 0U, 0U, std::as_bytes(std::span(packed)),
        static_cast<std::size_t>(resources.resolution) * 8U, error);
}

bool record_dashr_atlas_update(
    rhi::IDevice& device,
    DashrAtlasResources& r,
    const MeshRhiMirror& mirror,
    const CookedPolygonAsset& asset,
    DashrAtlasUpdateStats& stats,
    rhi::FenceHandle* fence,
    std::string* error) {
    stats = {};
    if (!r.valid() || !mirror.vertex_buffer() || !mirror.index_buffer() ||
        asset.indices.empty() ||
        asset.indices.size() > std::numeric_limits<std::uint32_t>::max()) {
        set_error(error, "DASHR atlas update inputs are invalid");
        return false;
    }

    auto commands = device.begin_commands(rhi::QueueKind::Graphics, "DASHR atlas update", error);
    if (!commands) return false;

    if (r.published) {
        for (auto texture : r.rawTextures) {
            if (!device.transition_texture(commands, texture, rhi::ResourceState::ShaderRead,
                                           rhi::ResourceState::RenderTarget, error)) return false;
            ++stats.textureTransitions;
        }
        for (auto texture : r.filledTextures) {
            if (!device.transition_texture(commands, texture, rhi::ResourceState::ShaderRead,
                                           rhi::ResourceState::ShaderWrite, error)) return false;
            ++stats.textureTransitions;
        }
    }

    rhi::RenderPassDesc pass;
    pass.debugName = "DASHR UV-space deformation atlas";
    for (auto texture : r.rawTextures)
        pass.colors.push_back({texture, true, 0.0F, 0.0F, 0.0F, 0.0F});
    if (!device.begin_render_pass(commands, pass, error) ||
        !device.bind_graphics_pipeline(commands, r.atlasPipeline, error) ||
        !device.set_viewport(commands, {0.0F, 0.0F, static_cast<float>(r.resolution),
                                        static_cast<float>(r.resolution), 0.0F, 1.0F}, error) ||
        !device.set_scissor(commands, {0, 0, r.resolution, r.resolution}, error) ||
        !device.bind_vertex_buffer(commands, 0U, mirror.vertex_buffer(), 0U,
                                   static_cast<std::uint32_t>(sizeof(GpuPolygonVertex)), error) ||
        !device.bind_index_buffer(commands, mirror.index_buffer(), 0U,
                                  rhi::IndexFormat::Uint32, error) ||
        !device.draw_indexed(commands, static_cast<std::uint32_t>(asset.indices.size()),
                             1U, 0U, 0, 0U, error) ||
        !device.end_render_pass(commands, error)) return false;
    ++stats.renderPasses;
    stats.trianglesRasterized = asset.indices.size() / 3U;

    for (auto texture : r.rawTextures) {
        if (!device.transition_texture(commands, texture, rhi::ResourceState::RenderTarget,
                                       rhi::ResourceState::ShaderRead, error)) return false;
        ++stats.textureTransitions;
    }

    if (!device.bind_compute_bind_group(commands, 0U, r.edgeFillGroup, error) ||
        !device.dispatch(commands, r.edgeFillPipeline,
                         (r.resolution + 7U) / 8U, (r.resolution + 7U) / 8U, 1U, error))
        return false;
    ++stats.edgeFillDispatches;

    for (auto texture : r.filledTextures) {
        if (!device.transition_texture(commands, texture, rhi::ResourceState::ShaderWrite,
                                       rhi::ResourceState::ShaderRead, error)) return false;
        ++stats.textureTransitions;
    }

    const auto submitted = device.submit(commands, error);
    if (!submitted) return false;
    r.published = true;
    if (fence) *fence = submitted;
    return true;
}

} // namespace dve::render
