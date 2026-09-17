#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "dve/query.hpp"
#include "dve/surface.hpp"

namespace dve {

struct GpuBrickUpload {
    BrickKey key{};
    std::uint32_t generation{};
    BrickEncoding encoding{BrickEncoding::Empty};
    MaterialId uniformMaterial{};
    std::array<std::uint32_t, 16> occupancy32{};
    std::array<std::uint32_t, 12> halo32{}; // two uint32 words per face
    std::vector<std::uint8_t> materialPayload{};
};

[[nodiscard]] GpuBrickUpload build_gpu_brick_upload(const VoxelObject& object, BrickKey key);
[[nodiscard]] MaterialId decode_gpu_upload_material(const GpuBrickUpload& upload, std::uint16_t index);

struct GpuMirrorUpdateStats {
    std::size_t submitted{};
    std::size_t published{};
    std::size_t staleDropped{};
    std::size_t insertedSlots{};
    std::size_t uploadedBytes{};
};

// CPU reference for the future D3D12/Vulkan brickmap mirror. Slots are sorted by BrickKey and
// generation-stamped. It intentionally models publication and stale-result rejection before
// device code is introduced.
class GpuBrickMirror {
public:
    void rebuild(const VoxelObject& object);
    [[nodiscard]] GpuMirrorUpdateStats update(
        const VoxelObject& object,
        std::span<const AppliedBrickEdit> edits);

    [[nodiscard]] const GpuBrickUpload* find(BrickKey key) const noexcept;
    [[nodiscard]] MaterialId material_at(Int3 globalVoxel) const noexcept;
    [[nodiscard]] bool occupied_at(Int3 globalVoxel) const noexcept {
        return material_at(globalVoxel) != kAirMaterial;
    }
    [[nodiscard]] std::size_t slot_count() const noexcept { return slots_.size(); }
    [[nodiscard]] std::size_t payload_bytes() const noexcept;
    [[nodiscard]] bool validate_against(const VoxelObject& object) const;

private:
    std::vector<GpuBrickUpload> slots_{};
};

[[nodiscard]] std::optional<RayHit> raycast_gpu_brick_mirror(
    const GpuBrickMirror& mirror,
    Float3 origin,
    Float3 direction,
    float maxDistance);

} // namespace dve
