#include "dve/surface.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace dve {

namespace {

constexpr std::uint64_t kColumn0 = 0x0101010101010101ULL;
constexpr std::uint64_t kColumn7 = 0x8080808080808080ULL;
constexpr std::uint64_t kNotColumn0 = ~kColumn0;
constexpr std::uint64_t kNotColumn7 = ~kColumn7;
constexpr std::uint64_t kRow0 = 0x00000000000000FFULL;
constexpr std::uint64_t kRow7 = 0xFF00000000000000ULL;

[[nodiscard]] Bitset512 occupancy_or_empty(const VoxelObject& object, BrickKey key) {
    const Brick* brick = object.find_brick(key);
    return brick == nullptr ? Bitset512{} : brick->occupancy();
}

[[nodiscard]] constexpr BrickKey offset(BrickKey key, int x, int y, int z) noexcept {
    return {key.x + x, key.y + y, key.z + z};
}

} // namespace

std::vector<SurfaceQuad> extract_brick_surface_reference(const VoxelObject& object, BrickKey key) {
    const Brick* brick = object.find_brick(key);
    if (brick == nullptr || brick->empty()) return {};

    struct Neighbor {
        Int3 delta;
        FaceDirection face;
    };
    constexpr std::array<Neighbor, 6> neighbors{{
        {{-1, 0, 0}, FaceDirection::NegX},
        {{1, 0, 0}, FaceDirection::PosX},
        {{0, -1, 0}, FaceDirection::NegY},
        {{0, 1, 0}, FaceDirection::PosY},
        {{0, 0, -1}, FaceDirection::NegZ},
        {{0, 0, 1}, FaceDirection::PosZ},
    }};

    std::vector<SurfaceQuad> quads;
    quads.reserve(static_cast<std::size_t>(brick->occupied_count()) * 2U);
    const Bitset512 occupancy = brick->occupancy();
    occupancy.for_each_set([&](std::uint16_t index) {
        const Int3 local = local_from_index_unchecked(index);
        const Int3 global = global_from_local(key, local);
        const MaterialId materialId = brick->material(index);
        for (const Neighbor& neighbor : neighbors) {
            const Int3 adjacent{
                global.x + neighbor.delta.x,
                global.y + neighbor.delta.y,
                global.z + neighbor.delta.z,
            };
            if (!object.occupied_at(adjacent)) quads.push_back({global, neighbor.face, materialId});
        }
    });
    return quads;
}

BrickFaceMasks extract_brick_face_masks(const VoxelObject& object, BrickKey key) {
    BrickFaceMasks faces{};
    const Brick* brick = object.find_brick(key);
    if (brick == nullptr || brick->empty()) return faces;

    const Bitset512 occupancy = brick->occupancy();
    const Bitset512 negXHalo = occupancy_or_empty(object, offset(key, -1, 0, 0));
    const Bitset512 posXHalo = occupancy_or_empty(object, offset(key, 1, 0, 0));
    const Bitset512 negYHalo = occupancy_or_empty(object, offset(key, 0, -1, 0));
    const Bitset512 posYHalo = occupancy_or_empty(object, offset(key, 0, 1, 0));
    const Bitset512 negZHalo = occupancy_or_empty(object, offset(key, 0, 0, -1));
    const Bitset512 posZHalo = occupancy_or_empty(object, offset(key, 0, 0, 1));

    for (std::size_t z = 0; z < occupancy.words.size(); ++z) {
        const std::uint64_t occupied = occupancy.words[z];
        const std::uint64_t solidNegX = ((occupied & kNotColumn7) << 1U) |
                                         ((negXHalo.words[z] & kColumn7) >> 7U);
        const std::uint64_t solidPosX = ((occupied & kNotColumn0) >> 1U) |
                                         ((posXHalo.words[z] & kColumn0) << 7U);
        const std::uint64_t solidNegY = (occupied << 8U) |
                                         ((negYHalo.words[z] & kRow7) >> 56U);
        const std::uint64_t solidPosY = (occupied >> 8U) |
                                         ((posYHalo.words[z] & kRow0) << 56U);
        const std::uint64_t solidNegZ = z == 0 ? negZHalo.words[7] : occupancy.words[z - 1];
        const std::uint64_t solidPosZ = z == 7 ? posZHalo.words[0] : occupancy.words[z + 1];

        faces[static_cast<std::size_t>(FaceDirection::NegX)].words[z] = occupied & ~solidNegX;
        faces[static_cast<std::size_t>(FaceDirection::PosX)].words[z] = occupied & ~solidPosX;
        faces[static_cast<std::size_t>(FaceDirection::NegY)].words[z] = occupied & ~solidNegY;
        faces[static_cast<std::size_t>(FaceDirection::PosY)].words[z] = occupied & ~solidPosY;
        faces[static_cast<std::size_t>(FaceDirection::NegZ)].words[z] = occupied & ~solidNegZ;
        faces[static_cast<std::size_t>(FaceDirection::PosZ)].words[z] = occupied & ~solidPosZ;
    }
    return faces;
}

std::vector<SurfaceQuad> extract_brick_surface_bitwise(const VoxelObject& object, BrickKey key) {
    const Brick* brick = object.find_brick(key);
    if (brick == nullptr || brick->empty()) return {};

    const BrickFaceMasks faces = extract_brick_face_masks(object, key);
    std::vector<SurfaceQuad> quads;
    std::size_t faceCount = 0;
    for (const Bitset512& mask : faces) faceCount += mask.count();
    quads.reserve(faceCount);

    const Bitset512 occupancy = brick->occupancy();
    occupancy.for_each_set([&](std::uint16_t index) {
        const Int3 global = global_from_local(key, local_from_index_unchecked(index));
        const MaterialId material = brick->material(index);
        for (std::size_t direction = 0; direction < faces.size(); ++direction) {
            if (faces[direction].test(index)) {
                quads.push_back({global, static_cast<FaceDirection>(direction), material});
            }
        }
    });
    return quads;
}

std::uint64_t surface_hash(const std::vector<SurfaceQuad>& quads) {
    constexpr std::uint64_t offsetValue = 1469598103934665603ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::uint64_t hash = offsetValue;
    auto mix = [&](std::uint64_t value) {
        for (int i = 0; i < 8; ++i) {
            hash ^= static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFU);
            hash *= prime;
        }
    };
    for (const auto& quad : quads) {
        mix(static_cast<std::uint32_t>(quad.voxel.x));
        mix(static_cast<std::uint32_t>(quad.voxel.y));
        mix(static_cast<std::uint32_t>(quad.voxel.z));
        mix(static_cast<std::uint8_t>(quad.face));
        mix(quad.material);
    }
    return hash;
}

} // namespace dve
