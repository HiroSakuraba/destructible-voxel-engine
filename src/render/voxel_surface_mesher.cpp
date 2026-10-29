#include "dve/render/voxel_surface_mesher.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

#include "dve/game_world.hpp"

namespace dve::render {
namespace {

using Dense = std::array<MaterialId, kBrickVoxelCount>;

// Face order: -X, +X, -Y, +Y, -Z, +Z. For each face the slice axis is the normal axis; u and v
// are the two in-plane axes, chosen so (u, v, normal) is right-handed (counter-clockwise quads
// seen from outside).
struct FaceFrame {
    int axis;      // normal axis
    int sign;      // -1 or +1
    int u;         // in-plane axis 1
    int v;         // in-plane axis 2
};
constexpr std::array<FaceFrame, 6> kFaces{{
    {0, -1, 2, 1}, {0, +1, 1, 2},
    {1, -1, 0, 2}, {1, +1, 2, 0},
    {2, -1, 1, 0}, {2, +1, 0, 1},
}};

int component(const Int3& value, int axis) noexcept {
    return axis == 0 ? value.x : axis == 1 ? value.y : value.z;
}
void set_component(Int3& value, int axis, int amount) noexcept {
    if (axis == 0) value.x = amount; else if (axis == 1) value.y = amount; else value.z = amount;
}
float component(const Float3& value, int axis) noexcept {
    return axis == 0 ? value.x : axis == 1 ? value.y : value.z;
}

Float3 axis_vector(int axis, float amount) noexcept {
    return {axis == 0 ? amount : 0.0F, axis == 1 ? amount : 0.0F, axis == 2 ? amount : 0.0F};
}

struct BrickNeighborhood {
    const VoxelObject& object;
    BrickKey key;
    const Dense& centre;
    std::array<Dense, 6> neighbours{};
    std::array<bool, 6> loaded{};
    std::array<bool, 6> present{};

    // Material at a local coordinate that may step one voxel outside the brick on one axis.
    MaterialId at(Int3 local) {
        int face = -1;
        if (local.x < 0) face = 0; else if (local.x >= kBrickDim) face = 1;
        else if (local.y < 0) face = 2; else if (local.y >= kBrickDim) face = 3;
        else if (local.z < 0) face = 4; else if (local.z >= kBrickDim) face = 5;
        if (face < 0) return centre[voxel_index_unchecked(local)];
        const auto f = static_cast<std::size_t>(face);
        if (!loaded[f]) {
            loaded[f] = true;
            BrickKey neighbour = key;
            const int delta = (face & 1) ? 1 : -1;
            if (face / 2 == 0) neighbour.x += delta; else if (face / 2 == 1) neighbour.y += delta; else neighbour.z += delta;
            if (const Brick* brick = object.find_brick(neighbour); brick != nullptr && !brick->empty()) {
                neighbours[f] = brick->materials();
                present[f] = true;
            }
        }
        if (!present[f]) return kAirMaterial;
        Int3 wrapped = local;
        wrapped.x = (wrapped.x + kBrickDim) % kBrickDim;
        wrapped.y = (wrapped.y + kBrickDim) % kBrickDim;
        wrapped.z = (wrapped.z + kBrickDim) % kBrickDim;
        return neighbours[f][voxel_index_unchecked(wrapped)];
    }
};

struct MeshBuilder {
    CookedPolygonAsset& asset;
    Float3 cell;
    std::array<std::int32_t, 256> materialSlot{};
    std::vector<std::vector<std::uint32_t>> indicesByMaterial;
    std::vector<MaterialId> slotMaterial;
    Float3 minimum{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    Float3 maximum{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};

    explicit MeshBuilder(CookedPolygonAsset& output, Float3 cellSize) : asset(output), cell(cellSize) {
        materialSlot.fill(-1);
    }

    std::size_t slot(MaterialId material) {
        auto& entry = materialSlot[material];
        if (entry < 0) {
            entry = static_cast<std::int32_t>(indicesByMaterial.size());
            indicesByMaterial.emplace_back();
            slotMaterial.push_back(material);
        }
        return static_cast<std::size_t>(entry);
    }

    // Quad covering voxel cells [origin, origin + extent) of a slice, on the face's outer side.
    void quad(const FaceFrame& frame, Int3 originVoxel, int width, int height, MaterialId material) {
        Int3 base = originVoxel;
        if (frame.sign > 0) set_component(base, frame.axis, component(base, frame.axis) + 1);
        const Float3 corner{static_cast<float>(base.x) * cell.x, static_cast<float>(base.y) * cell.y,
                            static_cast<float>(base.z) * cell.z};
        const Float3 du = axis_vector(frame.u, static_cast<float>(width) * component(cell, frame.u));
        const Float3 dv = axis_vector(frame.v, static_cast<float>(height) * component(cell, frame.v));
        const Float3 normal = axis_vector(frame.axis, static_cast<float>(frame.sign));
        const Float3 tangent = axis_vector(frame.u, 1.0F);
        const std::array<Float3, 4> positions{
            corner,
            {corner.x + du.x, corner.y + du.y, corner.z + du.z},
            {corner.x + du.x + dv.x, corner.y + du.y + dv.y, corner.z + du.z + dv.z},
            {corner.x + dv.x, corner.y + dv.y, corner.z + dv.z}};
        const auto first = static_cast<std::uint32_t>(asset.vertices.size());
        for (const Float3& position : positions) {
            PolygonVertex vertex;
            vertex.position = position;
            vertex.normal = normal;
            vertex.tangent = {tangent.x, tangent.y, tangent.z, 1.0F};
            // Planar metre-space UVs: texture density stays constant across merged quads.
            vertex.texcoord = {component(position, frame.u), component(position, frame.v)};
            vertex.texcoord1 = vertex.texcoord;
            asset.vertices.push_back(vertex);
            minimum = {std::min(minimum.x, position.x), std::min(minimum.y, position.y), std::min(minimum.z, position.z)};
            maximum = {std::max(maximum.x, position.x), std::max(maximum.y, position.y), std::max(maximum.z, position.z)};
        }
        auto& indices = indicesByMaterial[slot(material)];
        indices.insert(indices.end(), {first, first + 1U, first + 2U, first, first + 2U, first + 3U});
    }
};

} // namespace

bool build_voxel_surface_mesh(const VoxelObject& object,
                              std::span<const VoxelMaterialDefinition> materials,
                              const VoxelSurfaceMeshOptions& options,
                              CookedPolygonAsset& asset,
                              VoxelSurfaceMeshStats* stats,
                              std::string* error) {
    const Float3 cell = options.cellSizeMeters;
    if (!(cell.x > 0.0F) || !(cell.y > 0.0F) || !(cell.z > 0.0F) ||
        !std::isfinite(cell.x) || !std::isfinite(cell.y) || !std::isfinite(cell.z)) {
        if (error) *error = "voxel surface mesh needs a positive, finite cell size";
        return false;
    }
    asset = {};
    asset.objectId = options.objectId;
    VoxelSurfaceMeshStats counters;
    MeshBuilder builder(asset, cell);

    for (const auto& entry : object.bricks()) {
        const Brick& brick = entry.second;
        if (brick.empty()) continue;
        ++counters.bricksVisited;
        const Dense dense = brick.materials();
        BrickNeighborhood neighbourhood{object, entry.first, dense};
        const Int3 brickOrigin{entry.first.x * kBrickDim, entry.first.y * kBrickDim, entry.first.z * kBrickDim};

        for (const FaceFrame& frame : kFaces) {
            for (int depth = 0; depth < kBrickDim; ++depth) {
                // 8x8 mask of exposed faces in this slice, by material (air = none).
                std::array<MaterialId, kBrickDim * kBrickDim> mask{};
                bool any = false;
                for (int b = 0; b < kBrickDim; ++b) {
                    for (int a = 0; a < kBrickDim; ++a) {
                        Int3 local{};
                        set_component(local, frame.axis, depth);
                        set_component(local, frame.u, a);
                        set_component(local, frame.v, b);
                        const MaterialId material = dense[voxel_index_unchecked(local)];
                        if (material == kAirMaterial) continue;
                        Int3 outside = local;
                        set_component(outside, frame.axis, depth + frame.sign);
                        if (neighbourhood.at(outside) != kAirMaterial) continue;
                        mask[static_cast<std::size_t>(b * kBrickDim + a)] = material;
                        any = true;
                        ++counters.exposedVoxelFaces;
                    }
                }
                if (!any) continue;
                for (int b = 0; b < kBrickDim; ++b) {
                    for (int a = 0; a < kBrickDim;) {
                        const MaterialId material = mask[static_cast<std::size_t>(b * kBrickDim + a)];
                        if (material == kAirMaterial) { ++a; continue; }
                        int width = 1;
                        if (options.greedy)
                            while (a + width < kBrickDim &&
                                   mask[static_cast<std::size_t>(b * kBrickDim + a + width)] == material) ++width;
                        int height = 1;
                        if (options.greedy) {
                            for (; b + height < kBrickDim; ++height) {
                                bool row = true;
                                for (int k = 0; k < width && row; ++k)
                                    row = mask[static_cast<std::size_t>((b + height) * kBrickDim + a + k)] == material;
                                if (!row) break;
                            }
                        }
                        for (int y = 0; y < height; ++y)
                            for (int x = 0; x < width; ++x)
                                mask[static_cast<std::size_t>((b + y) * kBrickDim + a + x)] = kAirMaterial;
                        Int3 voxel = brickOrigin;
                        set_component(voxel, frame.axis, component(brickOrigin, frame.axis) + depth);
                        set_component(voxel, frame.u, component(brickOrigin, frame.u) + a);
                        set_component(voxel, frame.v, component(brickOrigin, frame.v) + b);
                        builder.quad(frame, voxel, width, height, material);
                        ++counters.quads;
                        a += width;
                    }
                }
            }
        }
    }

    for (std::size_t slot = 0; slot < builder.indicesByMaterial.size(); ++slot) {
        const MaterialId id = builder.slotMaterial[slot];
        asset.materials.push_back(id < materials.size() ? materials[id] : default_voxel_material(id));
        PolygonSubmesh submesh;
        submesh.name = "voxel material " + std::to_string(id);
        submesh.firstIndex = static_cast<std::uint32_t>(asset.indices.size());
        submesh.indexCount = static_cast<std::uint32_t>(builder.indicesByMaterial[slot].size());
        submesh.materialIndex = static_cast<std::uint32_t>(slot);
        asset.indices.insert(asset.indices.end(), builder.indicesByMaterial[slot].begin(),
                             builder.indicesByMaterial[slot].end());
        asset.submeshes.push_back(std::move(submesh));
    }
    asset.materialBindings.resize(asset.materials.size());
    if (!asset.vertices.empty()) asset.bounds = {builder.minimum, builder.maximum};
    asset.contentHash = polygon_asset_content_hash(asset);
    counters.materials = static_cast<std::uint32_t>(asset.materials.size());
    if (stats) *stats = counters;
    return true;
}

} // namespace dve::render
