#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "dve/voxel_object.hpp"

namespace dve {

enum class FaceDirection : std::uint8_t { NegX, PosX, NegY, PosY, NegZ, PosZ };

struct SurfaceQuad {
    Int3 voxel{};
    FaceDirection face{};
    MaterialId material{};
    auto operator<=>(const SurfaceQuad&) const = default;
};

using BrickFaceMasks = std::array<Bitset512, 6>;

// Readable per-voxel oracle retained for tests and offline validation.
[[nodiscard]] std::vector<SurfaceQuad> extract_brick_surface_reference(const VoxelObject& object, BrickKey key);

// Production CPU path: derives all six face masks with whole-plane shifts and six halo reads.
[[nodiscard]] BrickFaceMasks extract_brick_face_masks(const VoxelObject& object, BrickKey key);
[[nodiscard]] std::vector<SurfaceQuad> extract_brick_surface_bitwise(const VoxelObject& object, BrickKey key);

// Default API selects the production path.
[[nodiscard]] inline std::vector<SurfaceQuad> extract_brick_surface(const VoxelObject& object, BrickKey key) {
    return extract_brick_surface_bitwise(object, key);
}

[[nodiscard]] std::uint64_t surface_hash(const std::vector<SurfaceQuad>& quads);

} // namespace dve
