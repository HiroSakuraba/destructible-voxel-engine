#include "dve/render/mesh_heap.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <unordered_map>

namespace dve::render {
namespace {
void set_error(std::string* error, std::string_view value) {
    if (error != nullptr) error->assign(value.begin(), value.end());
}
template <class T>
void append_bytes(std::vector<std::byte>& destination, std::span<T> values) {
    const auto bytes = std::as_bytes(values);
    destination.insert(destination.end(), bytes.begin(), bytes.end());
}
GpuPolygonVertex pack_vertex(const PolygonVertex& v) {
    return {v.position.x,v.position.y,v.position.z,v.normal.x,v.normal.y,v.normal.z,
            v.tangent.x,v.tangent.y,v.tangent.z,v.tangent.w,v.texcoord.x,v.texcoord.y,
            v.color.x,v.color.y,v.color.z,v.color.w};
}
}

ImmutableMeshHeap::~ImmutableMeshHeap() { reset(); }

bool ImmutableMeshHeap::replace_buffer(rhi::BufferHandle& handle,
                                       std::span<const std::byte> bytes,
                                       rhi::BufferUsage usage,
                                       std::string_view name,
                                       std::string* error) {
    if (handle && !device_.destroy_buffer(handle, error)) return false;
    rhi::BufferDesc desc;
    desc.bytes = std::max<std::size_t>(bytes.size(), 4U);
    desc.usage = usage | rhi::BufferUsage::CopySource | rhi::BufferUsage::CopyDestination;
    desc.memory = rhi::MemoryDomain::DeviceLocal;
    desc.debugName.assign(name.begin(), name.end());
    handle = device_.create_buffer(desc, error);
    return handle && (bytes.empty() || device_.write_buffer(handle, 0, bytes, error));
}

bool ImmutableMeshHeap::rebuild(std::span<const CookedPolygonAsset* const> assets,
                                std::string* error) {
    allocations_.clear(); expectedVertices_.clear(); expectedIndices_.clear();
    expectedDraws_.clear(); expectedMaterials_.clear(); expectedInstances_.clear();
    stats_.sourceAssets = assets.size(); stats_.uniqueAssets = 0; stats_.deduplicatedAssets = 0;
    std::unordered_map<std::uint64_t, std::uint32_t> byHash;
    std::vector<GpuPolygonVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<GpuPolygonDrawRecord> draws;
    std::vector<GpuMaterialRecord> materials;
    for (const CookedPolygonAsset* source : assets) {
        if (source == nullptr) { set_error(error, "mesh heap contains a null asset"); return false; }
        const auto valid = validate_polygon_asset(*source);
        if (!valid) { set_error(error, valid.message); return false; }
        const std::uint64_t hash = polygon_asset_content_hash(*source);
        if (const auto found = byHash.find(hash); found != byHash.end()) {
            ++stats_.deduplicatedAssets;
            continue;
        }
        if (vertices.size() > std::numeric_limits<std::uint32_t>::max() - source->vertices.size() ||
            indices.size() > std::numeric_limits<std::uint32_t>::max() - source->indices.size() ||
            draws.size() > std::numeric_limits<std::uint32_t>::max() - source->submeshes.size() ||
            materials.size() > std::numeric_limits<std::uint32_t>::max() - source->materials.size()) {
            set_error(error, "mesh heap exceeds 32-bit draw offsets"); return false;
        }
        MeshHeapAllocation allocation;
        allocation.contentHash = hash;
        allocation.vertexOffset = static_cast<std::uint32_t>(vertices.size());
        allocation.vertexCount = static_cast<std::uint32_t>(source->vertices.size());
        allocation.indexOffset = static_cast<std::uint32_t>(indices.size());
        allocation.indexCount = static_cast<std::uint32_t>(source->indices.size());
        allocation.drawOffset = static_cast<std::uint32_t>(draws.size());
        allocation.drawCount = static_cast<std::uint32_t>(source->submeshes.size());
        allocation.materialOffset = static_cast<std::uint32_t>(materials.size());
        allocation.materialCount = static_cast<std::uint32_t>(source->materials.size());
        for (const PolygonVertex& vertex : source->vertices) vertices.push_back(pack_vertex(vertex));
        for (std::uint32_t index : source->indices) indices.push_back(index + allocation.vertexOffset);
        for (const PolygonSubmesh& submesh : source->submeshes) {
            const auto& binding = source->materialBindings[submesh.materialIndex];
            std::uint32_t flags = binding.doubleSided ? 1U : 0U;
            if (binding.baseColor.texture) flags |= 2U;
            draws.push_back({allocation.indexOffset + submesh.firstIndex, submesh.indexCount,
                             allocation.materialOffset + submesh.materialIndex, flags});
        }
        const auto packed = build_gpu_material_table(source->materials, error);
        if (!packed) return false;
        materials.insert(materials.end(), packed->begin(), packed->end());
        const auto index = static_cast<std::uint32_t>(allocations_.size());
        allocations_.push_back(allocation); byHash.emplace(hash, index); ++stats_.uniqueAssets;
    }
    append_bytes(expectedVertices_, std::span(vertices));
    append_bytes(expectedIndices_, std::span(indices));
    append_bytes(expectedDraws_, std::span(draws));
    append_bytes(expectedMaterials_, std::span(materials));
    if (!replace_buffer(vertexBuffer_, expectedVertices_, rhi::BufferUsage::Vertex | rhi::BufferUsage::Storage,
                        "DVE immutable mesh heap vertices", error) ||
        !replace_buffer(indexBuffer_, expectedIndices_, rhi::BufferUsage::Index | rhi::BufferUsage::Storage,
                        "DVE immutable mesh heap indices", error) ||
        !replace_buffer(drawBuffer_, expectedDraws_, rhi::BufferUsage::Storage | rhi::BufferUsage::Indirect,
                        "DVE immutable mesh heap draws", error) ||
        !replace_buffer(materialBuffer_, expectedMaterials_, rhi::BufferUsage::Storage,
                        "DVE immutable mesh heap materials", error)) return false;
    stats_.vertexBytes = expectedVertices_.size(); stats_.indexBytes = expectedIndices_.size();
    stats_.drawBytes = expectedDraws_.size(); stats_.materialBytes = expectedMaterials_.size();
    ++stats_.rebuilds; return true;
}

bool ImmutableMeshHeap::upload_instances(std::span<const MeshHeapInstance> instances,
                                         std::string* error) {
    std::vector<GpuMeshInstance> packed; packed.reserve(instances.size());
    for (const MeshHeapInstance& instance : instances) {
        if (instance.allocationIndex >= allocations_.size()) {
            set_error(error, "mesh instance references an unknown heap allocation"); return false;
        }
        const Quaternion q = normalize(instance.transform.rotation);
        packed.push_back({instance.transform.position.x,instance.transform.position.y,instance.transform.position.z,q.x,
                          q.y,q.z,q.w,instance.allocationIndex,instance.lodLevel,instance.flags,
                          static_cast<std::uint32_t>(instance.objectId),static_cast<std::uint32_t>(instance.objectId>>32U)});
    }
    expectedInstances_.clear(); append_bytes(expectedInstances_, std::span(packed));
    if (!replace_buffer(instanceBuffer_, expectedInstances_, rhi::BufferUsage::Storage | rhi::BufferUsage::Vertex,
                        "DVE mesh instances", error)) return false;
    stats_.instanceBytes = expectedInstances_.size(); ++stats_.instanceUploads; return true;
}

bool ImmutableMeshHeap::readback_matches(std::string* error) {
    const std::array<std::pair<rhi::BufferHandle,const std::vector<std::byte>*>,5> buffers{{
        {vertexBuffer_,&expectedVertices_},{indexBuffer_,&expectedIndices_},{drawBuffer_,&expectedDraws_},
        {materialBuffer_,&expectedMaterials_},{instanceBuffer_,&expectedInstances_}}};
    for (const auto& [handle, expected] : buffers) {
        if (expected->empty() && !handle) continue;
        if (!handle) { set_error(error, "mesh heap is missing an expected buffer"); return false; }
        std::vector<std::byte> actual(expected->size());
        if (!actual.empty() && !device_.read_buffer(handle, 0, actual, error)) return false;
        if (actual != *expected) { set_error(error, "mesh heap readback differs"); return false; }
    }
    return true;
}

void ImmutableMeshHeap::reset() noexcept {
    std::string ignored;
    for (rhi::BufferHandle* handle : {&vertexBuffer_,&indexBuffer_,&drawBuffer_,&materialBuffer_,&instanceBuffer_}) {
        if (*handle) {
            (void)device_.destroy_buffer(*handle, &ignored);
        }
        *handle = {};
    }
    allocations_.clear(); expectedVertices_.clear(); expectedIndices_.clear(); expectedDraws_.clear();
    expectedMaterials_.clear(); expectedInstances_.clear();
}

} // namespace dve::render
