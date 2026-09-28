// Tests for watertight print export (v1): surface extraction, manifold
// validity, and binary STL output.

#include "dve/print_export.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace dve;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr MaterialId kStone = 1;

VoxelObject make_solid_cube(std::int32_t edge) {
    VoxelObject object(1);
    for (std::int32_t x = 0; x < edge; ++x)
        for (std::int32_t y = 0; y < edge; ++y)
            for (std::int32_t z = 0; z < edge; ++z)
                object.set_voxel({x, y, z}, kStone);
    return object;
}

void test_single_voxel_extracts_twelve_triangles() {
    VoxelObject object(1);
    object.set_voxel({0, 0, 0}, kStone);

    const PrintMeshSoup soup = extract_print_surface(object);
    require(soup.triangles.size() / 3 == 12, "single voxel must emit 12 triangles");
    require(soup.positions.size() / 3 == 6 * 4,
            "single voxel soup must carry 24 vertices (6 quads)");
}

void test_shared_faces_are_not_emitted() {
    // Two adjacent voxels share one face: 12 + 12 - 4 = 20 triangles.
    VoxelObject object(1);
    object.set_voxel({0, 0, 0}, kStone);
    object.set_voxel({1, 0, 0}, kStone);

    const PrintMeshSoup soup = extract_print_surface(object);
    require(soup.triangles.size() / 3 == 20,
            "two adjacent voxels must emit 20 triangles, shared face skipped");
}

void test_export_writes_valid_binary_stl() {
    VoxelObject object = make_solid_cube(2);

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "dve_print_export_test.stl";
    const PrintExportResult result = export_print_stl(object, path);
    require(result.success, "export failed: " + result.error);
    require(result.triangleCount > 0, "export reported zero triangles");

    // Binary STL layout: 80-byte header + u32 count + 50 bytes per triangle.
    const std::uintmax_t size = std::filesystem::file_size(path);
    const std::uintmax_t expected = 80 + 4 + 50 * result.triangleCount;
    require(size == expected, "STL file size does not match triangle count");

    std::ifstream in(path, std::ios::binary);
    require(static_cast<bool>(in), "cannot re-open exported STL");
    char header[80] = {};
    in.read(header, 80);
    require(in.gcount() == 80, "STL header is short");
    std::uint32_t count = 0;
    in.read(reinterpret_cast<char*>(&count), 4);
    require(count == result.triangleCount, "STL count field disagrees");

    std::remove(path.c_str());
}

void test_dimensions_and_volume() {
    // 2x2x2 solid cube at 0.5 m voxels: 1 m on a side, 1 m^3 volume.
    VoxelObject object = make_solid_cube(2);

    PrintExportOptions options;
    options.voxelSizeMeters = 0.5;
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "dve_print_export_dims.stl";
    const PrintExportResult result = export_print_stl(object, path, options);
    require(result.success, "export failed: " + result.error);

    const double eps = 1e-9;
    require(std::abs(result.sizeXMeters - 1.0) < eps, "wrong X size");
    require(std::abs(result.sizeYMeters - 1.0) < eps, "wrong Y size");
    require(std::abs(result.sizeZMeters - 1.0) < eps, "wrong Z size");
    require(std::abs(result.volumeCubicMeters - 1.0) < 1e-6, "wrong volume");

    std::remove(path.c_str());
}

void test_hollow_shell_stays_watertight() {
    // 3x3x3 shell with an air center: outer surface plus the inner cavity
    // must both come out as closed, consistently wound shells.
    VoxelObject object = make_solid_cube(3);
    object.set_voxel({1, 1, 1}, kAirMaterial);

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "dve_print_export_hollow.stl";
    const PrintExportResult result = export_print_stl(object, path);
    require(result.success, "hollow shell export failed: " + result.error);

    std::remove(path.c_str());
}

void test_cross_brick_boundary_faces_merge() {
    // Voxels on opposite sides of a brick boundary (x=7 and x=8) must merge
    // into one solid, not two touching shells.
    VoxelObject object(1);
    for (std::int32_t x = 6; x <= 9; ++x)
        for (std::int32_t y = 6; y <= 9; ++y)
            for (std::int32_t z = 6; z <= 9; ++z)
                object.set_voxel({x, y, z}, kStone);

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "dve_print_export_brick.stl";
    const PrintExportResult result = export_print_stl(object, path);
    require(result.success, "cross-brick export failed: " + result.error);

    std::remove(path.c_str());
}

void test_empty_model_reports_error() {
    VoxelObject object(1);
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "dve_print_export_empty.stl";
    const PrintExportResult result = export_print_stl(object, path);
    require(!result.success, "empty model export must fail");
    require(!result.error.empty(), "empty model export must explain why");
    require(!std::filesystem::exists(path), "failed export must not leave a file");
}

}  // namespace

int main() {
    try {
        test_single_voxel_extracts_twelve_triangles();
        test_shared_faces_are_not_emitted();
        test_export_writes_valid_binary_stl();
        test_dimensions_and_volume();
        test_hollow_shell_stays_watertight();
        test_cross_brick_boundary_faces_merge();
        test_empty_model_reports_error();
        std::cout << "print export tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
