#include "dve/render/mesh_rhi_mirror.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace dve::render {
namespace {
std::size_t grown_capacity(std::size_t current, std::size_t required) noexcept {
    std::size_t result = std::max<std::size_t>(current, 256U);
    while (result < required) result += result / 2U;
    return result;
}
void set_error(std::string* error, std::string_view text) {
    if (error != nullptr) error->assign(text.begin(), text.end());
}
std::vector<GpuPolygonVertex> pack_vertices(const CookedPolygonAsset& asset) {
    std::vector<GpuPolygonVertex> result;
    result.reserve(asset.vertices.size());
    for (const PolygonVertex& v : asset.vertices) {
        result.push_back({v.position.x,v.position.y,v.position.z,
                          v.normal.x,v.normal.y,v.normal.z,
                          v.tangent.x,v.tangent.y,v.tangent.z,v.tangent.w,
                          v.texcoord.x,v.texcoord.y,
                          v.color.x,v.color.y,v.color.z,v.color.w});
    }
    return result;
}
std::vector<GpuPolygonDrawRecord> pack_draws(const CookedPolygonAsset& asset) {
    std::vector<GpuPolygonDrawRecord> result;
    result.reserve(asset.submeshes.size());
    for (const PolygonSubmesh& s : asset.submeshes) {
        const PolygonMaterialBinding& binding = asset.materialBindings[s.materialIndex];
        std::uint32_t flags = binding.doubleSided ? 1U : 0U;
        if (binding.baseColor.texture) flags |= 2U;
        result.push_back({s.firstIndex,s.indexCount,s.materialIndex,flags});
    }
    return result;
}
}

MeshRhiMirror::~MeshRhiMirror() { reset(); }

bool MeshRhiMirror::ensure_buffer(rhi::BufferHandle& handle, std::size_t& capacity,
                                  std::size_t requiredBytes, rhi::BufferUsage usage,
                                  std::string_view debugName, std::string* error) {
    const std::size_t required = std::max<std::size_t>(requiredBytes, 4U);
    if (handle && capacity >= required) return true;
    if (handle && !device_.destroy_buffer(handle,error)) return false;
    capacity = grown_capacity(0U, required);
    rhi::BufferDesc desc;
    desc.bytes = capacity;
    desc.usage = usage | rhi::BufferUsage::CopySource | rhi::BufferUsage::CopyDestination;
    desc.memory = rhi::MemoryDomain::DeviceLocal;
    desc.debugName.assign(debugName.begin(),debugName.end());
    handle = device_.create_buffer(desc,error);
    if (!handle) { capacity=0U; return false; }
    ++stats_.reallocations;
    return true;
}

bool MeshRhiMirror::upload(const CookedPolygonAsset& asset, std::string* error) {
    const auto valid = validate_polygon_asset(asset);
    if (!valid) { set_error(error,valid.message); return false; }
    const auto vertices = pack_vertices(asset);
    const auto draws = pack_draws(asset);
    const auto materials = build_gpu_material_table(asset.materials,error);
    if (!materials) return false;
    const auto vertexBytes=std::as_bytes(std::span(vertices));
    const auto indexBytes=std::as_bytes(std::span(asset.indices));
    const auto drawBytes=std::as_bytes(std::span(draws));
    const auto materialBytes=std::as_bytes(std::span(materials->data(),materials->size()));
    if(!ensure_buffer(vertexBuffer_,stats_.vertexCapacityBytes,vertexBytes.size(),rhi::BufferUsage::Vertex|rhi::BufferUsage::Storage,"DVE polygon vertices",error)||
       !ensure_buffer(indexBuffer_,stats_.indexCapacityBytes,indexBytes.size(),rhi::BufferUsage::Index|rhi::BufferUsage::Storage,"DVE polygon indices",error)||
       !ensure_buffer(drawBuffer_,stats_.drawCapacityBytes,drawBytes.size(),rhi::BufferUsage::Storage|rhi::BufferUsage::Indirect,"DVE polygon draws",error)||
       !ensure_buffer(materialBuffer_,stats_.materialCapacityBytes,materialBytes.size(),rhi::BufferUsage::Storage,"DVE polygon materials",error))return false;
    if((!vertexBytes.empty()&&!device_.write_buffer(vertexBuffer_,0,vertexBytes,error))||
       (!indexBytes.empty()&&!device_.write_buffer(indexBuffer_,0,indexBytes,error))||
       (!drawBytes.empty()&&!device_.write_buffer(drawBuffer_,0,drawBytes,error))||
       (!materialBytes.empty()&&!device_.write_buffer(materialBuffer_,0,materialBytes,error)))return false;
    vertexBytes_=vertexBytes.size();indexBytes_=indexBytes.size();drawBytes_=drawBytes.size();materialBytes_=materialBytes.size();stats_.uploadedBytes+=vertexBytes_+indexBytes_+drawBytes_+materialBytes_;++stats_.publications;return true;
}

bool MeshRhiMirror::readback_matches(const CookedPolygonAsset& asset, std::string* error) {
    const auto vertices=pack_vertices(asset);const auto draws=pack_draws(asset);const auto materials=build_gpu_material_table(asset.materials,error);if(!materials)return false;
    const std::array<std::pair<rhi::BufferHandle,std::span<const std::byte>>,4> expected{{
        {vertexBuffer_,std::as_bytes(std::span(vertices))},
        {indexBuffer_,std::as_bytes(std::span(asset.indices))},
        {drawBuffer_,std::as_bytes(std::span(draws))},
        {materialBuffer_,std::as_bytes(std::span(materials->data(),materials->size()))}}};
    const std::array<std::size_t,4> sizes{vertexBytes_,indexBytes_,drawBytes_,materialBytes_};
    for(std::size_t i=0;i<expected.size();++i){if(expected[i].second.size()!=sizes[i]){set_error(error,"polygon RHI mirror byte count differs");return false;}std::vector<std::byte> actual(sizes[i]);if(!actual.empty()&&!device_.read_buffer(expected[i].first,0,actual,error))return false;if(!std::equal(actual.begin(),actual.end(),expected[i].second.begin(),expected[i].second.end())){set_error(error,"polygon RHI readback differs from CPU publication");return false;}}
    return true;
}

void MeshRhiMirror::reset() noexcept {
    std::string ignored;
    if (vertexBuffer_) {
        device_.destroy_buffer(vertexBuffer_, &ignored);
    }
    if (indexBuffer_) {
        device_.destroy_buffer(indexBuffer_, &ignored);
    }
    if (drawBuffer_) {
        device_.destroy_buffer(drawBuffer_, &ignored);
    }
    if (materialBuffer_) {
        device_.destroy_buffer(materialBuffer_, &ignored);
    }
    vertexBuffer_ = {};
    indexBuffer_ = {};
    drawBuffer_ = {};
    materialBuffer_ = {};
    vertexBytes_ = 0;
    indexBytes_ = 0;
    drawBytes_ = 0;
    materialBytes_ = 0;
    stats_.vertexCapacityBytes = 0;
    stats_.indexCapacityBytes = 0;
    stats_.drawCapacityBytes = 0;
    stats_.materialCapacityBytes = 0;
}

} // namespace dve::render
