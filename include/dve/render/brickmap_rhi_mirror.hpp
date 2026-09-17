#pragma once

#include <cstddef>
#include <string>

#include "dve/packed_brickmap.hpp"
#include "dve/rhi/device.hpp"

namespace dve::render {

struct BrickmapRhiMirrorStats {
    std::size_t indexCapacityBytes{};
    std::size_t recordCapacityBytes{};
    std::size_t materialCapacityBytes{};
    std::size_t uploadedBytes{};
    std::uint64_t publications{};
    std::uint64_t reallocations{};
};

// Cross-backend publication seam for the CPU-authoritative packed brickmap. Direct3D 12,
// Vulkan, Metal, and the Null validation RHI all receive the exact same three logical buffers.
class PackedBrickmapRhiMirror {
public:
    explicit PackedBrickmapRhiMirror(rhi::IDevice& device) : device_(device) {}
    ~PackedBrickmapRhiMirror();

    PackedBrickmapRhiMirror(const PackedBrickmapRhiMirror&) = delete;
    PackedBrickmapRhiMirror& operator=(const PackedBrickmapRhiMirror&) = delete;

    bool upload(const PackedBrickmapScene& scene, std::string* error = nullptr);
    [[nodiscard]] bool readback_matches(const PackedBrickmapScene& scene,
                                        std::string* error = nullptr);
    void reset() noexcept;

    [[nodiscard]] rhi::BufferHandle index_buffer() const noexcept { return indexBuffer_; }
    [[nodiscard]] rhi::BufferHandle record_buffer() const noexcept { return recordBuffer_; }
    [[nodiscard]] rhi::BufferHandle material_buffer() const noexcept { return materialBuffer_; }
    [[nodiscard]] const BrickmapRhiMirrorStats& stats() const noexcept { return stats_; }

private:
    bool ensure_buffer(rhi::BufferHandle& handle, std::size_t& capacity,
                       std::size_t requiredBytes, std::string_view debugName,
                       std::string* error);

    rhi::IDevice& device_;
    rhi::BufferHandle indexBuffer_{};
    rhi::BufferHandle recordBuffer_{};
    rhi::BufferHandle materialBuffer_{};
    std::size_t indexBytes_{};
    std::size_t recordBytes_{};
    std::size_t materialBytes_{};
    BrickmapRhiMirrorStats stats_{};
};

} // namespace dve::render
