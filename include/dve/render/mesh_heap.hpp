#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "dve/render/mesh_rhi_mirror.hpp"
#include "dve/transform.hpp"

namespace dve::render {

struct MeshHeapAllocation {
    std::uint64_t contentHash{};
    std::uint32_t vertexOffset{};
    std::uint32_t vertexCount{};
    std::uint32_t indexOffset{};
    std::uint32_t indexCount{};
    std::uint32_t drawOffset{};
    std::uint32_t drawCount{};
    std::uint32_t materialOffset{};
    std::uint32_t materialCount{};
};

struct MeshHeapInstance {
    std::uint64_t objectId{};
    std::uint32_t allocationIndex{};
    RigidTransform transform{};
    std::uint32_t lodLevel{};
    std::uint32_t flags{};
};

struct GpuMeshInstance {
    float positionX{}, positionY{}, positionZ{}, rotationX{};
    float rotationY{}, rotationZ{}, rotationW{1.0F};
    std::uint32_t allocationIndex{};
    std::uint32_t lodLevel{};
    std::uint32_t flags{};
    std::uint32_t objectIdLow{};
    std::uint32_t objectIdHigh{};
};
static_assert(sizeof(GpuMeshInstance) == 48U);

struct MeshHeapStats {
    std::uint64_t sourceAssets{};
    std::uint64_t uniqueAssets{};
    std::uint64_t deduplicatedAssets{};
    std::uint64_t rebuilds{};
    std::uint64_t instanceUploads{};
    std::size_t vertexBytes{};
    std::size_t indexBytes{};
    std::size_t drawBytes{};
    std::size_t materialBytes{};
    std::size_t instanceBytes{};
};

class ImmutableMeshHeap {
public:
    explicit ImmutableMeshHeap(rhi::IDevice& device) : device_(device) {}
    ~ImmutableMeshHeap();
    ImmutableMeshHeap(const ImmutableMeshHeap&) = delete;
    ImmutableMeshHeap& operator=(const ImmutableMeshHeap&) = delete;

    bool rebuild(std::span<const CookedPolygonAsset* const> assets,
                 std::string* error = nullptr);
    bool upload_instances(std::span<const MeshHeapInstance> instances,
                          std::string* error = nullptr);
    bool readback_matches(std::string* error = nullptr);
    void reset() noexcept;

    [[nodiscard]] const std::vector<MeshHeapAllocation>& allocations() const noexcept {
        return allocations_;
    }
    [[nodiscard]] const MeshHeapStats& stats() const noexcept { return stats_; }
    [[nodiscard]] rhi::BufferHandle vertex_buffer() const noexcept { return vertexBuffer_; }
    [[nodiscard]] rhi::BufferHandle index_buffer() const noexcept { return indexBuffer_; }
    [[nodiscard]] rhi::BufferHandle draw_buffer() const noexcept { return drawBuffer_; }
    [[nodiscard]] rhi::BufferHandle material_buffer() const noexcept { return materialBuffer_; }
    [[nodiscard]] rhi::BufferHandle instance_buffer() const noexcept { return instanceBuffer_; }

private:
    bool replace_buffer(rhi::BufferHandle& handle, std::span<const std::byte> bytes,
                        rhi::BufferUsage usage, std::string_view name,
                        std::string* error);

    rhi::IDevice& device_;
    rhi::BufferHandle vertexBuffer_{};
    rhi::BufferHandle indexBuffer_{};
    rhi::BufferHandle drawBuffer_{};
    rhi::BufferHandle materialBuffer_{};
    rhi::BufferHandle instanceBuffer_{};
    std::vector<MeshHeapAllocation> allocations_;
    std::vector<std::byte> expectedVertices_;
    std::vector<std::byte> expectedIndices_;
    std::vector<std::byte> expectedDraws_;
    std::vector<std::byte> expectedMaterials_;
    std::vector<std::byte> expectedInstances_;
    MeshHeapStats stats_{};
};

} // namespace dve::render
