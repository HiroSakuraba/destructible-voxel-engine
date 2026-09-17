#include "dve/gpu_brick_mirror.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace dve {

namespace {

[[nodiscard]] std::uint64_t face_plane(const Bitset512& occupancy, FaceDirection face) noexcept {
    std::uint64_t plane = 0;
    for (std::uint32_t v = 0; v < 8; ++v) {
        for (std::uint32_t u = 0; u < 8; ++u) {
            Int3 local{};
            switch (face) {
                case FaceDirection::NegX: local = {0, static_cast<int>(u), static_cast<int>(v)}; break;
                case FaceDirection::PosX: local = {7, static_cast<int>(u), static_cast<int>(v)}; break;
                case FaceDirection::NegY: local = {static_cast<int>(u), 0, static_cast<int>(v)}; break;
                case FaceDirection::PosY: local = {static_cast<int>(u), 7, static_cast<int>(v)}; break;
                case FaceDirection::NegZ: local = {static_cast<int>(u), static_cast<int>(v), 0}; break;
                case FaceDirection::PosZ: local = {static_cast<int>(u), static_cast<int>(v), 7}; break;
            }
            if (occupancy.test(voxel_index_unchecked(local))) plane |= std::uint64_t{1} << (u + 8U * v);
        }
    }
    return plane;
}

[[nodiscard]] Bitset512 occupancy_or_empty(const VoxelObject& object, BrickKey key) {
    const Brick* brick = object.find_brick(key);
    return brick == nullptr ? Bitset512{} : brick->occupancy();
}

void encode_materials(const Brick& brick, GpuBrickUpload& upload) {
    upload.encoding = brick.encoding();
    if (brick.empty()) return;

    if (brick.encoding() == BrickEncoding::UniformSolid || brick.encoding() == BrickEncoding::MaskUniform) {
        const Bitset512 occupancy = brick.occupancy();
        occupancy.for_each_set([&](std::uint16_t index) {
            if (upload.uniformMaterial == kAirMaterial) upload.uniformMaterial = brick.material(index);
        });
        return;
    }

    if (brick.encoding() == BrickEncoding::LocalPalette4) {
        std::array<MaterialId, 16> palette{};
        std::uint8_t paletteSize = 1;
        std::array<std::uint8_t, 256> packed{};
        for (std::uint16_t index = 0; index < kBrickVoxelCount; ++index) {
            const MaterialId material = brick.material(index);
            std::uint8_t paletteIndex = 0;
            if (material != kAirMaterial) {
                auto it = std::find(palette.begin() + 1, palette.begin() + paletteSize, material);
                if (it == palette.begin() + paletteSize) {
                    palette[paletteSize] = material;
                    paletteIndex = paletteSize++;
                } else {
                    paletteIndex = static_cast<std::uint8_t>(it - palette.begin());
                }
            }
            std::uint8_t& byte = packed[index >> 1U];
            if ((index & 1U) == 0) byte = static_cast<std::uint8_t>((byte & 0xF0U) | paletteIndex);
            else byte = static_cast<std::uint8_t>((byte & 0x0FU) | (paletteIndex << 4U));
        }
        upload.materialPayload.insert(upload.materialPayload.end(), packed.begin(), packed.end());
        upload.materialPayload.insert(upload.materialPayload.end(), palette.begin(), palette.end());
        return;
    }

    upload.materialPayload.resize(kBrickVoxelCount);
    for (std::uint16_t index = 0; index < kBrickVoxelCount; ++index) {
        upload.materialPayload[index] = brick.material(index);
    }
}

[[nodiscard]] float int_boundary(float value, float direction) noexcept {
    if (direction > 0.0F) return (std::floor(value) + 1.0F - value) / direction;
    if (direction < 0.0F) return (value - std::floor(value)) / -direction;
    return std::numeric_limits<float>::infinity();
}

} // namespace

GpuBrickUpload build_gpu_brick_upload(const VoxelObject& object, BrickKey key) {
    GpuBrickUpload upload;
    upload.key = key;
    const Brick* brick = object.find_brick(key);
    if (brick == nullptr) return upload;
    upload.generation = brick->generation();

    const Bitset512 occupancy = brick->occupancy();
    for (std::size_t i = 0; i < occupancy.words.size(); ++i) {
        upload.occupancy32[i * 2] = static_cast<std::uint32_t>(occupancy.words[i]);
        upload.occupancy32[i * 2 + 1] = static_cast<std::uint32_t>(occupancy.words[i] >> 32U);
    }

    constexpr std::array<BrickKey, 6> neighborDelta{{
        {-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1},
    }};
    constexpr std::array<FaceDirection, 6> neighborFace{{
        FaceDirection::PosX, FaceDirection::NegX,
        FaceDirection::PosY, FaceDirection::NegY,
        FaceDirection::PosZ, FaceDirection::NegZ,
    }};
    for (std::size_t face = 0; face < neighborDelta.size(); ++face) {
        const BrickKey neighborKey{
            key.x + neighborDelta[face].x,
            key.y + neighborDelta[face].y,
            key.z + neighborDelta[face].z,
        };
        const std::uint64_t plane = face_plane(occupancy_or_empty(object, neighborKey), neighborFace[face]);
        upload.halo32[face * 2] = static_cast<std::uint32_t>(plane);
        upload.halo32[face * 2 + 1] = static_cast<std::uint32_t>(plane >> 32U);
    }

    encode_materials(*brick, upload);
    return upload;
}

MaterialId decode_gpu_upload_material(const GpuBrickUpload& upload, std::uint16_t index) {
    const std::uint32_t word = upload.occupancy32[index >> 5U];
    if (((word >> (index & 31U)) & 1U) == 0U) return kAirMaterial;
    if (upload.encoding == BrickEncoding::UniformSolid || upload.encoding == BrickEncoding::MaskUniform) {
        return upload.uniformMaterial;
    }
    if (upload.encoding == BrickEncoding::LocalPalette4) {
        if (upload.materialPayload.size() < 272) return kAirMaterial;
        const std::uint8_t packed = upload.materialPayload[index >> 1U];
        const std::uint8_t paletteIndex = (index & 1U) == 0U ? packed & 0x0FU : packed >> 4U;
        return upload.materialPayload[256U + paletteIndex];
    }
    if (upload.encoding == BrickEncoding::Palette8 && upload.materialPayload.size() >= kBrickVoxelCount) {
        return upload.materialPayload[index];
    }
    return kAirMaterial;
}

void GpuBrickMirror::rebuild(const VoxelObject& object) {
    slots_.clear();
    slots_.reserve(object.brick_count());
    for (const BrickKey key : object.bricks().sorted_keys()) slots_.push_back(build_gpu_brick_upload(object, key));
}

GpuMirrorUpdateStats GpuBrickMirror::update(
    const VoxelObject& object,
    std::span<const AppliedBrickEdit> edits) {
    GpuMirrorUpdateStats stats;
    std::vector<BrickKey> publishKeys;
    publishKeys.reserve(edits.size() * 7U);
    constexpr std::array<BrickKey, 7> affectedDelta{{
        {0, 0, 0}, {-1, 0, 0}, {1, 0, 0}, {0, -1, 0},
        {0, 1, 0}, {0, 0, -1}, {0, 0, 1},
    }};

    for (const AppliedBrickEdit& edit : edits) {
        ++stats.submitted;
        const Brick* brick = object.find_brick(edit.key);
        if (brick == nullptr || brick->generation() != edit.generation) {
            ++stats.staleDropped;
            continue;
        }
        for (const BrickKey delta : affectedDelta) {
            const BrickKey key{edit.key.x + delta.x, edit.key.y + delta.y, edit.key.z + delta.z};
            if (object.find_brick(key) != nullptr) publishKeys.push_back(key);
        }
    }

    std::sort(publishKeys.begin(), publishKeys.end());
    publishKeys.erase(std::unique(publishKeys.begin(), publishKeys.end()), publishKeys.end());
    for (const BrickKey key : publishKeys) {
        GpuBrickUpload upload = build_gpu_brick_upload(object, key);
        stats.uploadedBytes += sizeof(upload.occupancy32) + sizeof(upload.halo32) + upload.materialPayload.size();
        auto it = std::lower_bound(slots_.begin(), slots_.end(), key,
            [](const GpuBrickUpload& slot, BrickKey candidate) { return slot.key < candidate; });
        if (it == slots_.end() || it->key != key) {
            slots_.insert(it, std::move(upload));
            ++stats.insertedSlots;
        } else {
            *it = std::move(upload);
        }
        ++stats.published;
    }
    return stats;
}

const GpuBrickUpload* GpuBrickMirror::find(BrickKey key) const noexcept {
    const auto it = std::lower_bound(slots_.begin(), slots_.end(), key,
        [](const GpuBrickUpload& slot, BrickKey candidate) { return slot.key < candidate; });
    return it == slots_.end() || it->key != key ? nullptr : &*it;
}

MaterialId GpuBrickMirror::material_at(Int3 globalVoxel) const noexcept {
    const BrickKey key = brick_key_from_voxel(globalVoxel);
    const GpuBrickUpload* upload = find(key);
    if (upload == nullptr) return kAirMaterial;
    return decode_gpu_upload_material(*upload, voxel_index_unchecked(local_voxel_from_global(globalVoxel)));
}

std::size_t GpuBrickMirror::payload_bytes() const noexcept {
    std::size_t total = 0;
    for (const GpuBrickUpload& slot : slots_) total += slot.materialPayload.size();
    return total;
}

bool GpuBrickMirror::validate_against(const VoxelObject& object) const {
    if (slots_.size() != object.brick_count()) return false;
    for (const GpuBrickUpload& slot : slots_) {
        const Brick* brick = object.find_brick(slot.key);
        if (brick == nullptr || brick->generation() != slot.generation) return false;
        for (std::uint16_t index = 0; index < kBrickVoxelCount; ++index) {
            if (decode_gpu_upload_material(slot, index) != brick->material(index)) return false;
        }
    }
    return true;
}

std::optional<RayHit> raycast_gpu_brick_mirror(
    const GpuBrickMirror& mirror,
    Float3 origin,
    Float3 direction,
    float maxDistance) {
    const float magnitude = length(direction);
    if (!(magnitude > 0.0F) || !(maxDistance >= 0.0F)) return std::nullopt;
    const float maximumParameter = maxDistance / magnitude;

    Int3 voxel{
        static_cast<std::int32_t>(std::floor(origin.x)),
        static_cast<std::int32_t>(std::floor(origin.y)),
        static_cast<std::int32_t>(std::floor(origin.z)),
    };
    const Int3 step{
        direction.x > 0.0F ? 1 : (direction.x < 0.0F ? -1 : 0),
        direction.y > 0.0F ? 1 : (direction.y < 0.0F ? -1 : 0),
        direction.z > 0.0F ? 1 : (direction.z < 0.0F ? -1 : 0),
    };
    Float3 tMax{
        int_boundary(origin.x, direction.x),
        int_boundary(origin.y, direction.y),
        int_boundary(origin.z, direction.z),
    };
    const Float3 tDelta{
        direction.x == 0.0F ? std::numeric_limits<float>::infinity() : std::abs(1.0F / direction.x),
        direction.y == 0.0F ? std::numeric_limits<float>::infinity() : std::abs(1.0F / direction.y),
        direction.z == 0.0F ? std::numeric_limits<float>::infinity() : std::abs(1.0F / direction.z),
    };

    float parameter = 0.0F;
    Int3 normal{};
    while (parameter <= maximumParameter) {
        const MaterialId material = mirror.material_at(voxel);
        if (material != kAirMaterial) return RayHit{voxel, normal, parameter * magnitude, material};
        if (tMax.x <= tMax.y && tMax.x <= tMax.z) {
            voxel.x += step.x; parameter = tMax.x; tMax.x += tDelta.x; normal = {-step.x, 0, 0};
        } else if (tMax.y <= tMax.z) {
            voxel.y += step.y; parameter = tMax.y; tMax.y += tDelta.y; normal = {0, -step.y, 0};
        } else {
            voxel.z += step.z; parameter = tMax.z; tMax.z += tDelta.z; normal = {0, 0, -step.z};
        }
    }
    return std::nullopt;
}

} // namespace dve
