#include "dve/render/dashr_atlas.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cmath>
#include <limits>
#include <span>
#include <string_view>
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

constexpr float kDifferentialEpsilon = 1.0e-8F;

Float3 add3(Float3 a, Float3 b) noexcept {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Float3 sub3(Float3 a, Float3 b) noexcept {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
Float3 mul3(Float3 a, float s) noexcept {
    return {a.x * s, a.y * s, a.z * s};
}
float dot3(Float3 a, Float3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
Float3 cross3(Float3 a, Float3 b) noexcept {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}
bool finite3(Float3 v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
Float3 normalize3(Float3 v, Float3 fallback) noexcept {
    const float squared = dot3(v, v);
    if (!(squared > kDifferentialEpsilon * kDifferentialEpsilon) ||
        !std::isfinite(squared)) return fallback;
    return mul3(v, 1.0F / std::sqrt(squared));
}

struct DifferentialField {
    std::vector<Float3> dPdu;
    std::vector<Float3> dPdv;
    std::vector<float> weights;
    std::uint64_t degenerateUvTriangles{};
};

bool build_differential_field(const CookedPolygonAsset& asset,
                              std::span<const Float3> positions,
                              DifferentialField& field,
                              bool countDegenerate,
                              std::string* error) {
    const bool rest = positions.empty();
    if (!rest && positions.size() != asset.vertices.size()) {
        set_error(error, "DASHR deformed position count does not match the polygon asset");
        return false;
    }
    if (!rest && std::any_of(positions.begin(), positions.end(),
                             [](Float3 p) { return !finite3(p); })) {
        set_error(error, "DASHR deformed positions contain a non-finite value");
        return false;
    }

    field.dPdu.assign(asset.vertices.size(), {});
    field.dPdv.assign(asset.vertices.size(), {});
    field.weights.assign(asset.vertices.size(), 0.0F);
    field.degenerateUvTriangles = 0U;

    const auto position = [&](std::uint32_t index) -> Float3 {
        return rest ? asset.vertices[index].position : positions[index];
    };
    for (std::size_t triangle = 0U; triangle < asset.indices.size(); triangle += 3U) {
        const std::uint32_t i0 = asset.indices[triangle];
        const std::uint32_t i1 = asset.indices[triangle + 1U];
        const std::uint32_t i2 = asset.indices[triangle + 2U];
        const Float2 uv0 = asset.vertices[i0].texcoord;
        const Float2 uv1 = asset.vertices[i1].texcoord;
        const Float2 uv2 = asset.vertices[i2].texcoord;
        const float du1 = uv1.x - uv0.x;
        const float dv1 = uv1.y - uv0.y;
        const float du2 = uv2.x - uv0.x;
        const float dv2 = uv2.y - uv0.y;
        const float determinant = du1 * dv2 - du2 * dv1;
        if (!std::isfinite(determinant) || std::abs(determinant) <= kDifferentialEpsilon) {
            if (countDegenerate) ++field.degenerateUvTriangles;
            continue;
        }

        const Float3 p0 = position(i0);
        const Float3 e1 = sub3(position(i1), p0);
        const Float3 e2 = sub3(position(i2), p0);
        const float inverse = 1.0F / determinant;
        const Float3 dPdu = mul3(sub3(mul3(e1, dv2), mul3(e2, dv1)), inverse);
        const Float3 dPdv = mul3(sub3(mul3(e2, du1), mul3(e1, du2)), inverse);
        const float area2 = std::sqrt(std::max(0.0F, dot3(cross3(e1, e2), cross3(e1, e2))));
        const float weight = std::max(area2, 1.0e-6F);
        for (const std::uint32_t index : {i0, i1, i2}) {
            field.dPdu[index] = add3(field.dPdu[index], mul3(dPdu, weight));
            field.dPdv[index] = add3(field.dPdv[index], mul3(dPdv, weight));
            field.weights[index] += weight;
        }
    }
    for (std::size_t index = 0U; index < asset.vertices.size(); ++index) {
        if (field.weights[index] > kDifferentialEpsilon) {
            const float inverse = 1.0F / field.weights[index];
            field.dPdu[index] = mul3(field.dPdu[index], inverse);
            field.dPdv[index] = mul3(field.dPdv[index], inverse);
            continue;
        }
        const PolygonVertex& vertex = asset.vertices[index];
        const Float3 normal = normalize3(vertex.normal, {0.0F, 0.0F, 1.0F});
        const Float3 tangent = normalize3(
            {vertex.tangent.x, vertex.tangent.y, vertex.tangent.z},
            {1.0F, 0.0F, 0.0F});
        Float3 bitangent = normalize3(cross3(normal, tangent), {0.0F, 1.0F, 0.0F});
        if (vertex.tangent.w < 0.0F) bitangent = mul3(bitangent, -1.0F);
        field.dPdu[index] = tangent;
        field.dPdv[index] = bitangent;
    }
    return true;
}

std::size_t grown_capacity(std::size_t current, std::size_t required) noexcept {
    std::size_t result = std::max<std::size_t>(current, 256U);
    while (result < required) result += result / 2U;
    return result;
}

} // namespace

std::optional<std::vector<GpuDashrSurfaceVertex>> build_dashr_surface_vertices(
    const CookedPolygonAsset& asset,
    std::span<const Float3> deformedPositions,
    DashrSurfaceMeshBuildStats* stats,
    std::string* error) {
    const auto valid = validate_polygon_asset(asset);
    if (!valid) {
        set_error(error, valid.message);
        return std::nullopt;
    }

    DifferentialField rest;
    if (!build_differential_field(asset, {}, rest, true, error)) return std::nullopt;
    DifferentialField current;
    if (deformedPositions.empty()) current = rest;
    else if (!build_differential_field(asset, deformedPositions, current, false, error))
        return std::nullopt;

    DashrSurfaceMeshBuildStats local;
    local.triangles = asset.indices.size() / 3U;
    local.degenerateUvTriangles = rest.degenerateUvTriangles;
    local.minimumDistortionU = local.minimumDistortionV = std::numeric_limits<float>::infinity();
    local.maximumDistortionU = local.maximumDistortionV = -std::numeric_limits<float>::infinity();

    std::vector<GpuDashrSurfaceVertex> result;
    result.reserve(asset.vertices.size());
    const bool restPose = deformedPositions.empty();
    for (std::size_t index = 0U; index < asset.vertices.size(); ++index) {
        const Float3 restU = rest.dPdu[index];
        const Float3 restV = rest.dPdv[index];
        const Float3 currentU = current.dPdu[index];
        const Float3 currentV = current.dPdv[index];
        const float restUSquared = dot3(restU, restU);
        const float restVSquared = dot3(restV, restV);
        if (rest.weights[index] <= kDifferentialEpsilon) ++local.fallbackVertices;

        float distortionU = restPose ? 1.0F :
            dot3(currentU, restU) / std::max(restUSquared, kDifferentialEpsilon);
        float distortionV = restPose ? 1.0F :
            dot3(currentV, restV) / std::max(restVSquared, kDifferentialEpsilon);
        if (!std::isfinite(distortionU)) distortionU = 1.0F;
        if (!std::isfinite(distortionV)) distortionV = 1.0F;
        distortionU = std::clamp(distortionU, -64.0F, 64.0F);
        distortionV = std::clamp(distortionV, -64.0F, 64.0F);

        local.minimumDistortionU = std::min(local.minimumDistortionU, distortionU);
        local.maximumDistortionU = std::max(local.maximumDistortionU, distortionU);
        local.minimumDistortionV = std::min(local.minimumDistortionV, distortionV);
        local.maximumDistortionV = std::max(local.maximumDistortionV, distortionV);

        const Float3 position = restPose ? asset.vertices[index].position
                                        : deformedPositions[index];
        const Float2 uv = asset.vertices[index].texcoord;
        result.push_back({
            position.x, position.y, position.z,
            currentU.x, currentU.y, currentU.z,
            currentV.x, currentV.y, currentV.z,
            uv.x, uv.y, distortionU, distortionV});
    }
    if (result.empty()) {
        local.minimumDistortionU = local.maximumDistortionU = 1.0F;
        local.minimumDistortionV = local.maximumDistortionV = 1.0F;
    }
    if (stats) *stats = local;
    return result;
}

DashrSurfaceMeshMirror::~DashrSurfaceMeshMirror() { reset(); }

bool DashrSurfaceMeshMirror::ensure_buffer(
    rhi::BufferHandle& handle,
    std::size_t& capacity,
    std::size_t requiredBytes,
    rhi::BufferUsage usage,
    std::string_view debugName,
    std::string* error) {
    const std::size_t required = std::max<std::size_t>(requiredBytes, 4U);
    if (handle && capacity >= required) return true;
    if (handle && !device_.destroy_buffer(handle, error)) return false;
    capacity = grown_capacity(0U, required);
    rhi::BufferDesc desc;
    desc.bytes = capacity;
    desc.usage = usage | rhi::BufferUsage::CopySource | rhi::BufferUsage::CopyDestination;
    desc.memory = rhi::MemoryDomain::DeviceLocal;
    desc.initialState = rhi::ResourceState::ShaderRead;
    desc.debugName.assign(debugName.begin(), debugName.end());
    handle = device_.create_buffer(desc, error);
    if (!handle) {
        capacity = 0U;
        return false;
    }
    ++stats_.reallocations;
    return true;
}

bool DashrSurfaceMeshMirror::upload(
    const CookedPolygonAsset& asset,
    std::span<const Float3> deformedPositions,
    DashrSurfaceMeshBuildStats* buildStats,
    std::string* error) {
    if (asset.vertices.size() > std::numeric_limits<std::uint32_t>::max() ||
        asset.indices.size() > std::numeric_limits<std::uint32_t>::max()) {
        set_error(error, "DASHR surface mesh exceeds 32-bit GPU counts");
        return false;
    }
    auto packed = build_dashr_surface_vertices(asset, deformedPositions, buildStats, error);
    if (!packed) return false;

    const auto vertexBytes = std::as_bytes(std::span(*packed));
    const auto indexBytes = std::as_bytes(std::span(asset.indices));
    if (!ensure_buffer(vertexBuffer_, stats_.vertexCapacityBytes, vertexBytes.size(),
                       rhi::BufferUsage::Vertex, "DASHR scaled surface vertices", error) ||
        !ensure_buffer(indexBuffer_, stats_.indexCapacityBytes, indexBytes.size(),
                       rhi::BufferUsage::Index, "DASHR surface indices", error))
        return false;
    if (!vertexBytes.empty() && !device_.write_buffer(vertexBuffer_, 0U, vertexBytes, error))
        return false;

    const std::uint64_t contentHash = polygon_asset_content_hash(asset);
    std::size_t uploaded = vertexBytes.size();
    if (assetContentHash_ != contentHash || indexBytes_ != indexBytes.size()) {
        if (!indexBytes.empty() && !device_.write_buffer(indexBuffer_, 0U, indexBytes, error))
            return false;
        uploaded += indexBytes.size();
    }

    assetContentHash_ = contentHash;
    vertexBytes_ = vertexBytes.size();
    indexBytes_ = indexBytes.size();
    vertexCount_ = static_cast<std::uint32_t>(asset.vertices.size());
    indexCount_ = static_cast<std::uint32_t>(asset.indices.size());
    stats_.uploadedBytes += uploaded;
    ++stats_.publications;
    return true;
}

void DashrSurfaceMeshMirror::reset() noexcept {
    std::string ignored;
    if (vertexBuffer_) (void)device_.destroy_buffer(vertexBuffer_, &ignored);
    if (indexBuffer_) (void)device_.destroy_buffer(indexBuffer_, &ignored);
    vertexBuffer_ = {};
    indexBuffer_ = {};
    vertexBytes_ = indexBytes_ = 0U;
    vertexCount_ = indexCount_ = 0U;
    assetContentHash_ = 0U;
    stats_.vertexCapacityBytes = stats_.indexCapacityBytes = 0U;
}

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
        static_cast<std::uint32_t>(sizeof(GpuDashrSurfaceVertex)), false};
    atlasPipeline.vertexAttributes = {
        {0U, rhi::VertexFormat::Float3,
         static_cast<std::uint32_t>(offsetof(GpuDashrSurfaceVertex, positionX))},
        {1U, rhi::VertexFormat::Float3,
         static_cast<std::uint32_t>(offsetof(GpuDashrSurfaceVertex, dPduX))},
        {2U, rhi::VertexFormat::Float3,
         static_cast<std::uint32_t>(offsetof(GpuDashrSurfaceVertex, dPdvX))},
        {3U, rhi::VertexFormat::Float2,
         static_cast<std::uint32_t>(offsetof(GpuDashrSurfaceVertex, uvX))},
        {4U, rhi::VertexFormat::Float2,
         static_cast<std::uint32_t>(offsetof(GpuDashrSurfaceVertex, distortionU))},
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
    const DashrSurfaceMeshMirror& mirror,
    const CookedPolygonAsset& asset,
    DashrAtlasUpdateStats& stats,
    rhi::FenceHandle* fence,
    std::string* error) {
    stats = {};
    if (!r.valid() || !mirror.vertex_buffer() || !mirror.index_buffer() ||
        asset.indices.empty() ||
        mirror.vertex_count() != asset.vertices.size() ||
        mirror.index_count() != asset.indices.size() ||
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
                                   static_cast<std::uint32_t>(sizeof(GpuDashrSurfaceVertex)), error) ||
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
