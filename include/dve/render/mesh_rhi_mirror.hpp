#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "dve/gpu_material_buffer.hpp"
#include "dve/polygon_asset.hpp"
#include "dve/rhi/device.hpp"

namespace dve::render {

struct GpuPolygonVertex {
    float positionX{}, positionY{}, positionZ{};
    float normalX{}, normalY{}, normalZ{};
    float tangentX{1.0F}, tangentY{}, tangentZ{}, tangentW{1.0F};
    float texcoordX{}, texcoordY{};
    float colorR{1.0F}, colorG{1.0F}, colorB{1.0F}, colorA{1.0F};
};
static_assert(sizeof(GpuPolygonVertex) == 16U * sizeof(float));

struct GpuPolygonDrawRecord {
    std::uint32_t firstIndex{};
    std::uint32_t indexCount{};
    std::uint32_t materialIndex{};
    std::uint32_t flags{};
};
static_assert(sizeof(GpuPolygonDrawRecord) == 16U);

struct MeshRhiMirrorStats {
    std::size_t vertexCapacityBytes{};
    std::size_t indexCapacityBytes{};
    std::size_t drawCapacityBytes{};
    std::size_t materialCapacityBytes{};
    std::size_t uploadedBytes{};
    std::uint64_t publications{};
    std::uint64_t reallocations{};
};

class MeshRhiMirror {
public:
    explicit MeshRhiMirror(rhi::IDevice& device) : device_(device) {}
    ~MeshRhiMirror();
    MeshRhiMirror(const MeshRhiMirror&) = delete;
    MeshRhiMirror& operator=(const MeshRhiMirror&) = delete;

    bool upload(const CookedPolygonAsset& asset, std::string* error = nullptr);
    [[nodiscard]] bool readback_matches(const CookedPolygonAsset& asset,
                                        std::string* error = nullptr);
    void reset() noexcept;

    [[nodiscard]] rhi::BufferHandle vertex_buffer() const noexcept { return vertexBuffer_; }
    [[nodiscard]] rhi::BufferHandle index_buffer() const noexcept { return indexBuffer_; }
    [[nodiscard]] rhi::BufferHandle draw_buffer() const noexcept { return drawBuffer_; }
    [[nodiscard]] rhi::BufferHandle material_buffer() const noexcept { return materialBuffer_; }
    [[nodiscard]] const MeshRhiMirrorStats& stats() const noexcept { return stats_; }

private:
    bool ensure_buffer(rhi::BufferHandle& handle, std::size_t& capacity,
                       std::size_t requiredBytes, rhi::BufferUsage usage,
                       std::string_view debugName, std::string* error);

    rhi::IDevice& device_;
    rhi::BufferHandle vertexBuffer_{};
    rhi::BufferHandle indexBuffer_{};
    rhi::BufferHandle drawBuffer_{};
    rhi::BufferHandle materialBuffer_{};
    std::size_t vertexBytes_{};
    std::size_t indexBytes_{};
    std::size_t drawBytes_{};
    std::size_t materialBytes_{};
    MeshRhiMirrorStats stats_{};
};

} // namespace dve::render
