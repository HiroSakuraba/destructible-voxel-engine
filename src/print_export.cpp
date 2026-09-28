// Watertight print export for voxel models.
//
// The pipeline is: walk the brick map, emit one quad (two CCW triangles) per
// solid/air voxel face, then hand the soup to manifold3d which deduplicates
// the shared corner vertices, merges coplanar faces, and proves the result
// is a valid 2-manifold before anything is written to disk.

#include "dve/print_export.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>

#include <manifold/manifold.h>

#include "dve/types.hpp"

namespace dve {
namespace {

// One corner of the unit voxel cube, in voxel units.
struct Corner {
    std::int32_t x, y, z;
};

// Six faces. Each lists four corners in counter-clockwise order as seen from
// OUTSIDE the voxel (cross(e1, e2) of the first triangle points along the
// outward face normal). Verified by hand: see the winding table below.
//
//   +X: A(1,0,0) B(1,1,0) C(1,1,1) -> cross = (1,0,0)
//   -X: A(0,0,0) B(0,0,1) C(0,1,1) -> cross = (-1,0,0)
//   +Y: A(0,1,0) B(0,1,1) C(1,1,1) -> cross = (0,1,0)
//   -Y: A(0,0,0) B(1,0,0) C(1,0,1) -> cross = (0,-1,0)
//   +Z: A(0,0,1) B(1,0,1) C(1,1,1) -> cross = (0,0,1)
//   -Z: A(0,0,0) B(0,1,0) C(1,1,0) -> cross = (0,0,-1)
struct Face {
    Int3 neighborOffset;
    std::array<Corner, 4> corners;
};

constexpr std::array<Face, 6> kFaces{{
    {{1, 0, 0}, {{{1, 0, 0}, {1, 1, 0}, {1, 1, 1}, {1, 0, 1}}}},
    {{-1, 0, 0}, {{{0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}}}},
    {{0, 1, 0}, {{{0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0}}}},
    {{0, -1, 0}, {{{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}}}},
    {{0, 0, 1}, {{{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}}}},
    {{0, 0, -1}, {{{0, 0, 0}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}}}},
}};

// Debug-only assertion of the winding table: cross products must match the
// face's outward normal. Runs once per process.
bool winding_table_ok() {
    for (const Face& face : kFaces) {
        const Corner& a = face.corners[0];
        const Corner& b = face.corners[1];
        const Corner& c = face.corners[2];
        const std::int64_t e1x = b.x - a.x, e1y = b.y - a.y, e1z = b.z - a.z;
        const std::int64_t e2x = c.x - a.x, e2y = c.y - a.y, e2z = c.z - a.z;
        const std::int64_t nx = e1y * e2z - e1z * e2y;
        const std::int64_t ny = e1z * e2x - e1x * e2z;
        const std::int64_t nz = e1x * e2y - e1y * e2x;
        const Int3& n = face.neighborOffset;  // unit outward normal
        if (nx != n.x || ny != n.y || nz != n.z) return false;
    }
    return true;
}

// Build the manifold solid from a voxel model, or return an error string.
manifold::Manifold build_solid(const VoxelObject& object,
                               const PrintExportOptions& options,
                               std::string& errorOut) {
    static const bool kWindingOk = winding_table_ok();
    if (!kWindingOk) {
        errorOut = "internal error: face winding table is inconsistent";
        return {};
    }
    if (!(options.voxelSizeMeters > 0.0) || !(options.scale > 0.0)) {
        errorOut = "voxelSizeMeters and scale must be positive";
        return {};
    }

    const double unit = options.voxelSizeMeters * options.scale;

    manifold::MeshGL64 mesh;
    mesh.numProp = 3;
    mesh.vertProperties.reserve(4096);
    mesh.triVerts.reserve(4096);

    for (const auto& [key, brick] : object.bricks()) {
        if (brick.empty()) continue;
        const std::int64_t ox = static_cast<std::int64_t>(key.x) * kBrickDim;
        const std::int64_t oy = static_cast<std::int64_t>(key.y) * kBrickDim;
        const std::int64_t oz = static_cast<std::int64_t>(key.z) * kBrickDim;

        for (std::uint16_t index = 0; index < kBrickVoxelCount; ++index) {
            if (!brick.occupied(index)) continue;
            const Int3 local = local_from_index_unchecked(index);
            const std::int64_t vx = ox + local.x;
            const std::int64_t vy = oy + local.y;
            const std::int64_t vz = oz + local.z;

            for (const Face& face : kFaces) {
                const Int3 neighbor{
                    static_cast<std::int32_t>(vx + face.neighborOffset.x),
                    static_cast<std::int32_t>(vy + face.neighborOffset.y),
                    static_cast<std::int32_t>(vz + face.neighborOffset.z)};
                if (object.material_at(neighbor) != kAirMaterial) continue;

                // Exposed face: emit one quad as two CCW triangles.
                const std::uint64_t base =
                    static_cast<std::uint64_t>(mesh.vertProperties.size() / 3);
                for (const Corner& corner : face.corners) {
                    mesh.vertProperties.push_back(
                        (static_cast<double>(vx + corner.x)) * unit);
                    mesh.vertProperties.push_back(
                        (static_cast<double>(vy + corner.y)) * unit);
                    mesh.vertProperties.push_back(
                        (static_cast<double>(vz + corner.z)) * unit);
                }
                mesh.triVerts.insert(mesh.triVerts.end(),
                                     {base, base + 1, base + 2,
                                      base, base + 2, base + 3});
            }
        }
    }

    if (mesh.triVerts.empty()) {
        errorOut = "model is empty: no exposed voxel faces to export";
        return {};
    }

    mesh.Merge();  // dedup shared corners, merge coplanar faces
    manifold::Manifold solid(std::move(mesh));
    if (solid.Status() != manifold::Manifold::Error::NoError) {
        errorOut = "extracted surface is not watertight (manifold status "
                   + std::to_string(static_cast<int>(solid.Status())) + ")";
        return {};
    }
    return solid;
}

void write_u32_le(std::ofstream& out, std::uint32_t value) {
    out.put(static_cast<char>(value & 0xFF));
    out.put(static_cast<char>((value >> 8) & 0xFF));
    out.put(static_cast<char>((value >> 16) & 0xFF));
    out.put(static_cast<char>((value >> 24) & 0xFF));
}

void write_f32_le(std::ofstream& out, float value) {
    static_assert(sizeof(float) == 4);
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    write_u32_le(out, bits);
}

bool write_binary_stl(const manifold::MeshGL64& mesh,
                      const std::filesystem::path& path,
                      std::string& errorOut) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        errorOut = "cannot open output file: " + path.string();
        return false;
    }

    const char header[80] = "Binary STL exported by the destructible voxel engine";
    out.write(header, 80);

    const std::size_t triCount = mesh.triVerts.size() / 3;
    write_u32_le(out, static_cast<std::uint32_t>(triCount));

    const double* pos = mesh.vertProperties.data();
    for (std::size_t t = 0; t < triCount; ++t) {
        const std::uint64_t i0 = mesh.triVerts[3 * t];
        const std::uint64_t i1 = mesh.triVerts[3 * t + 1];
        const std::uint64_t i2 = mesh.triVerts[3 * t + 2];
        const double ax = pos[3 * i0], ay = pos[3 * i0 + 1], az = pos[3 * i0 + 2];
        const double bx = pos[3 * i1], by = pos[3 * i1 + 1], bz = pos[3 * i1 + 2];
        const double cx = pos[3 * i2], cy = pos[3 * i2 + 1], cz = pos[3 * i2 + 2];

        // Facet normal from the (CCW) winding.
        const double e1x = bx - ax, e1y = by - ay, e1z = bz - az;
        const double e2x = cx - ax, e2y = cy - ay, e2z = cz - az;
        double nx = e1y * e2z - e1z * e2y;
        double ny = e1z * e2x - e1x * e2z;
        double nz = e1x * e2y - e1y * e2x;
        const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 0.0) {
            nx /= len;
            ny /= len;
            nz /= len;
        }
        write_f32_le(out, static_cast<float>(nx));
        write_f32_le(out, static_cast<float>(ny));
        write_f32_le(out, static_cast<float>(nz));
        write_f32_le(out, static_cast<float>(ax));
        write_f32_le(out, static_cast<float>(ay));
        write_f32_le(out, static_cast<float>(az));
        write_f32_le(out, static_cast<float>(bx));
        write_f32_le(out, static_cast<float>(by));
        write_f32_le(out, static_cast<float>(bz));
        write_f32_le(out, static_cast<float>(cx));
        write_f32_le(out, static_cast<float>(cy));
        write_f32_le(out, static_cast<float>(cz));
        out.put(0);
        out.put(0);  // attribute byte count
    }

    out.close();
    if (!out) {
        errorOut = "failed while writing: " + path.string();
        return false;
    }
    return true;
}

}  // namespace

PrintMeshSoup extract_print_surface(const VoxelObject& object,
                                    const PrintExportOptions& options) {
    // Reuse the same face walk as the STL path, but return the raw soup.
    static const bool kWindingOk = winding_table_ok();
    PrintMeshSoup soup;
    if (!kWindingOk) return soup;
    const double unit = (options.voxelSizeMeters > 0.0 && options.scale > 0.0)
                            ? options.voxelSizeMeters * options.scale
                            : 0.10;

    for (const auto& [key, brick] : object.bricks()) {
        if (brick.empty()) continue;
        const std::int64_t ox = static_cast<std::int64_t>(key.x) * kBrickDim;
        const std::int64_t oy = static_cast<std::int64_t>(key.y) * kBrickDim;
        const std::int64_t oz = static_cast<std::int64_t>(key.z) * kBrickDim;

        for (std::uint16_t index = 0; index < kBrickVoxelCount; ++index) {
            if (!brick.occupied(index)) continue;
            const Int3 local = local_from_index_unchecked(index);
            const std::int64_t vx = ox + local.x;
            const std::int64_t vy = oy + local.y;
            const std::int64_t vz = oz + local.z;

            for (const Face& face : kFaces) {
                const Int3 neighbor{
                    static_cast<std::int32_t>(vx + face.neighborOffset.x),
                    static_cast<std::int32_t>(vy + face.neighborOffset.y),
                    static_cast<std::int32_t>(vz + face.neighborOffset.z)};
                if (object.material_at(neighbor) != kAirMaterial) continue;

                const std::uint32_t base =
                    static_cast<std::uint32_t>(soup.positions.size() / 3);
                for (const Corner& corner : face.corners) {
                    soup.positions.push_back(
                        static_cast<double>(vx + corner.x) * unit);
                    soup.positions.push_back(
                        static_cast<double>(vy + corner.y) * unit);
                    soup.positions.push_back(
                        static_cast<double>(vz + corner.z) * unit);
                }
                soup.triangles.insert(soup.triangles.end(),
                                      {base, base + 1, base + 2,
                                       base, base + 2, base + 3});
            }
        }
    }
    return soup;
}

PrintExportResult export_print_stl(const VoxelObject& object,
                                   const std::filesystem::path& path,
                                   const PrintExportOptions& options) {
    PrintExportResult result;
    std::string error;
    manifold::Manifold solid = build_solid(object, options, error);
    if (!error.empty()) {
        result.error = error;
        return result;
    }

    manifold::MeshGL64 mesh = solid.GetMeshGL64();
    if (!write_binary_stl(mesh, path, error)) {
        result.error = error;
        return result;
    }

    const auto props_volume = solid.Volume();
    const auto bbox = solid.BoundingBox();
    result.success = true;
    result.triangleCount = mesh.triVerts.size() / 3;
    result.sizeXMeters = bbox.max[0] - bbox.min[0];
    result.sizeYMeters = bbox.max[1] - bbox.min[1];
    result.sizeZMeters = bbox.max[2] - bbox.min[2];
    result.volumeCubicMeters = props_volume;
    return result;
}

}  // namespace dve
