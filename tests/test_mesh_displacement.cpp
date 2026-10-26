// Analytic tests for bake_editor_mesh_displacement: the editor bake of
// material height displacement. Expectations are closed-form: the fixture is
// a flat quad facing +Y with exact UVs, so a height sample h moves a vertex
// to y = (h - midpoint) * strength, and the engine's bilinear red-channel
// sampler reproduces linear ramps exactly.
#include "dve/editor_mesh_authoring.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace dve;
using namespace dve::editor;

namespace {
void check(bool x, const char* message) { if (!x) throw std::runtime_error(message); }
void near(double a, double b, double tolerance, const char* message) {
    if (std::abs(a - b) > tolerance)
        throw std::runtime_error(std::string(message) + " (got " + std::to_string(a) +
                                 ", expected " + std::to_string(b) + ")");
}

double volume(const CookedPolygonAsset& m) {
    double out = 0;
    for (std::size_t i = 0; i + 2 < m.indices.size(); i += 3) {
        auto a = m.vertices[m.indices[i]].position, b = m.vertices[m.indices[i + 1]].position,
             c = m.vertices[m.indices[i + 2]].position;
        out += double(a.x) * (double(b.y) * c.z - double(b.z) * c.y) +
               double(a.y) * (double(b.z) * c.x - double(b.x) * c.z) +
               double(a.z) * (double(b.x) * c.y - double(b.y) * c.x);
    }
    return out / 6;
}

// Flat 2x2 quad in the XZ plane, welded corners, normal +Y, uv = (x+1)/2,
// (z+1)/2. When bindHeight is set, material 0 carries a height texture whose
// red channel is redAt(u, v) sampled on a width x height texel grid.
CookedPolygonAsset quad(std::uint32_t width, std::uint32_t height,
                        const std::vector<std::uint8_t>& red, bool bindHeight = true) {
    CookedPolygonAsset mesh;
    mesh.objectId = 7;
    mesh.materials.resize(1);
    mesh.materials[0].name = "Height";
    mesh.materialBindings.resize(1);
    mesh.vertices = {
        {{-1, 0, -1}, {0, 1, 0}, {1, 0, 0, 1}, {0, 0}, {1, 1, 1, 1}, {}},
        {{-1, 0, 1}, {0, 1, 0}, {1, 0, 0, 1}, {0, 1}, {1, 1, 1, 1}, {}},
        {{1, 0, 1}, {0, 1, 0}, {1, 0, 0, 1}, {1, 1}, {1, 1, 1, 1}, {}},
        {{1, 0, -1}, {0, 1, 0}, {1, 0, 0, 1}, {1, 0}, {1, 1, 1, 1}, {}},
    };
    mesh.indices = {0, 1, 2, 0, 2, 3};
    mesh.submeshes = {{"Quad", 0, 6, 0}};
    if (bindHeight) {
        PolygonImage image;
        image.name = "height";
        image.width = width;
        image.height = height;
        image.rgba8.resize(std::size_t(width) * height * 4);
        for (std::size_t i = 0; i < red.size(); ++i) {
            image.rgba8[i * 4] = red[i];
            image.rgba8[i * 4 + 1] = red[i];
            image.rgba8[i * 4 + 2] = red[i];
            image.rgba8[i * 4 + 3] = 255;
        }
        mesh.images.push_back(std::move(image));
        // Clamp-to-edge sampling: with the default Repeat wrap, u = 1 would
        // sample u = 0, which is right for tiling textures but not for a
        // heightfield that spans the mesh exactly once.
        PolygonSampler sampler;
        sampler.wrapS = ImportedWrapMode::ClampToEdge;
        sampler.wrapT = ImportedWrapMode::ClampToEdge;
        mesh.samplers.push_back(sampler);
        mesh.textures.push_back({"height", 0, 0});
        mesh.materialBindings[0].height.texture = 0;
        mesh.materialBindings[0].height.texcoord = 0;
    }
    mesh.bounds = {{-1, 0, -1}, {1, 0, 1}};
    mesh.contentHash = polygon_asset_content_hash(mesh);
    return mesh;
}

std::vector<std::uint8_t> constant_image(std::uint32_t w, std::uint32_t h, std::uint8_t value) {
    return std::vector<std::uint8_t>(std::size_t(w) * h, value);
}

const PolygonVertex* vertex_at_uv(const CookedPolygonAsset& mesh, float u, float v) {
    for (const auto& vertex : mesh.vertices)
        if (std::abs(vertex.texcoord.x - u) < 1e-6F && std::abs(vertex.texcoord.y - v) < 1e-6F)
            return &vertex;
    return nullptr;
}

void constant_offsets() {
    auto mesh = quad(4, 4, constant_image(4, 4, 255));
    MeshDisplacementBakeSettings settings;
    settings.strengthMeters = 0.2F;
    settings.midpoint = 0.5F;
    settings.subdivisionLevels = 0;
    auto result = bake_editor_mesh_displacement(mesh, settings);
    if (!result) throw std::runtime_error(result.polygon.error);
    check(result.stats.displacedVisualVertices == 4, "constant: displaced vertex count");
    check(result.stats.outputTriangles == 2, "constant: triangle count");
    near(result.stats.maximumAbsoluteDisplacement, 0.1, 1e-6, "constant: max displacement");
    for (const auto& vertex : result.polygon.mesh.vertices)
        near(vertex.position.y, 0.1, 1e-6, "constant: uniform +0.1 offset");
    check(mesh.vertices[0].position.y == 0.0F, "constant: source mesh untouched");
}

void midpoint_identity() {
    auto mesh = quad(4, 4, constant_image(4, 4, 128));
    MeshDisplacementBakeSettings settings;
    settings.strengthMeters = 0.2F;
    settings.midpoint = 128.0F / 255.0F; // exactly the texel value
    settings.subdivisionLevels = 1;
    auto result = bake_editor_mesh_displacement(mesh, settings);
    if (!result) throw std::runtime_error(result.polygon.error);
    for (const auto& vertex : result.polygon.mesh.vertices)
        near(vertex.position.y, 0.0, 1e-7, "midpoint: surface untouched at the midpoint value");
}

void ramp_plane_and_normals() {
    // red = column index over 256 columns: the bilinear sampler returns
    // h(u) = u exactly, so the quad becomes the plane y = (u - 0.5) * 0.2.
    std::vector<std::uint8_t> ramp(256);
    for (std::uint32_t i = 0; i < 256; ++i) ramp[i] = std::uint8_t(i);
    auto mesh = quad(256, 1, ramp);
    MeshDisplacementBakeSettings settings;
    settings.strengthMeters = 0.2F;
    settings.midpoint = 0.5F;
    settings.subdivisionLevels = 1;
    auto result = bake_editor_mesh_displacement(mesh, settings);
    if (!result) throw std::runtime_error(result.polygon.error);
    const auto& baked = result.polygon.mesh;
    const auto* west = vertex_at_uv(baked, 0.0F, 0.0F);
    const auto* east = vertex_at_uv(baked, 1.0F, 0.0F);
    const auto* center = vertex_at_uv(baked, 0.5F, 0.5F);
    check(west && east && center, "ramp: expected vertices exist after subdivision");
    near(west->position.y, -0.1, 1e-6, "ramp: u=0 sits at -strength/2");
    near(east->position.y, 0.1, 1e-6, "ramp: u=1 sits at +strength/2");
    near(center->position.y, 0.0, 1e-5, "ramp: center crosses zero");
    // dy/dx = 0.2 * (du/dx = 1/2) = 0.1, so every recomputed normal is
    // normalize(-0.1, 1, 0).
    const double nx = -0.1 / std::sqrt(1.01), ny = 1.0 / std::sqrt(1.01);
    for (const auto& vertex : baked.vertices) {
        near(vertex.normal.x, nx, 5e-3, "ramp: normal tilt x");
        near(vertex.normal.y, ny, 5e-3, "ramp: normal tilt y");
        near(vertex.normal.z, 0.0, 5e-3, "ramp: normal has no z tilt");
    }
}

void sine_relief() {
    constexpr std::uint32_t size = 513;
    std::vector<std::uint8_t> sine(std::size_t(size) * size);
    for (std::uint32_t y = 0; y < size; ++y)
        for (std::uint32_t x = 0; x < size; ++x) {
            const double s = std::sin(2.0 * 3.141592653589793 * double(x) / double(size - 1));
            sine[std::size_t(y) * size + x] = std::uint8_t(std::lround(127.5 + 127.5 * s));
        }
    auto mesh = quad(size, size, sine);
    MeshDisplacementBakeSettings settings;
    settings.strengthMeters = 0.2F;
    settings.midpoint = 0.5F;
    settings.subdivisionLevels = 3; // 8x8 cells: vertices land on u = k/8
    auto result = bake_editor_mesh_displacement(mesh, settings);
    if (!result) throw std::runtime_error(result.polygon.error);
    const auto& baked = result.polygon.mesh;
    const auto* peak = vertex_at_uv(baked, 0.25F, 0.5F);
    const auto* zero = vertex_at_uv(baked, 0.5F, 0.5F);
    const auto* trough = vertex_at_uv(baked, 0.75F, 0.5F);
    check(peak && zero && trough, "sine: expected vertices exist");
    near(peak->position.y, 0.1, 2e-3, "sine: peak rises to +strength/2");
    near(zero->position.y, 0.0, 2e-3, "sine: zero crossing stays at the midpoint");
    near(trough->position.y, -0.1, 2e-3, "sine: trough recesses to -strength/2");
}

void subdivision_counts_and_uvs() {
    auto mesh = quad(4, 4, constant_image(4, 4, 128));
    MeshDisplacementBakeSettings settings;
    settings.midpoint = 128.0F / 255.0F;
    settings.subdivisionLevels = 2;
    auto result = bake_editor_mesh_displacement(mesh, settings);
    if (!result) throw std::runtime_error(result.polygon.error);
    check(result.polygon.mesh.vertices.size() == 25, "subdivision: 4 -> 25 welded vertices");
    check(result.polygon.mesh.indices.size() == 32 * 3, "subdivision: 2 -> 32 triangles");
    check(vertex_at_uv(result.polygon.mesh, 0.5F, 0.5F) != nullptr,
          "subdivision: midpoint vertex carries the averaged UV");
    check(vertex_at_uv(result.polygon.mesh, 0.25F, 0.75F) != nullptr,
          "subdivision: quarter vertex carries the interpolated UV");
}

void cube_volume() {
    auto mesh = make_editor_polygon_cube(9, 1.0F);
    PolygonImage image;
    image.name = "height";
    image.width = image.height = 2;
    image.rgba8.assign(2 * 2 * 4, 255);
    mesh.images.push_back(image);
    mesh.textures.push_back({"height", 0, std::nullopt});
    mesh.materialBindings[0].height.texture = 0;
    mesh.contentHash = polygon_asset_content_hash(mesh);
    MeshDisplacementBakeSettings settings;
    settings.strengthMeters = 0.2F;
    settings.midpoint = 0.5F;
    settings.subdivisionLevels = 0;
    auto result = bake_editor_mesh_displacement(mesh, settings);
    if (!result) throw std::runtime_error(result.polygon.error);
    // Subdivision 0 keeps vertex order: every vertex must sit exactly its
    // authored normal x (1 - 0.5) x 0.2 = 0.1 from its source position.
    check(result.polygon.mesh.vertices.size() == mesh.vertices.size(), "cube: vertex count kept");
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        const auto before = mesh.vertices[i], after = result.polygon.mesh.vertices[i];
        near(after.position.x - before.position.x, before.normal.x * 0.1, 1e-6, "cube: x offset along normal");
        near(after.position.y - before.position.y, before.normal.y * 0.1, 1e-6, "cube: y offset along normal");
        near(after.position.z - before.position.z, before.normal.z * 0.1, 1e-6, "cube: z offset along normal");
    }
    // Signed-volume integral over the six independently translated faces:
    // 1.2, verified by an independent computation on the same construction
    // (the integral is not d x total area for split faces).
    near(volume(result.polygon.mesh), 1.2, 1e-4, "cube: signed volume of the offset faces");
}

void refusals() {
    MeshDisplacementBakeSettings settings;
    auto unbound = quad(4, 4, {}, false);
    check(!bake_editor_mesh_displacement(unbound, settings), "refusal: no height texture bound");
    auto mesh = quad(4, 4, constant_image(4, 4, 200));
    auto zero = settings;
    zero.strengthMeters = 0.0F;
    check(!bake_editor_mesh_displacement(mesh, zero), "refusal: zero strength");
    auto badMidpoint = settings;
    badMidpoint.midpoint = 1.5F;
    check(!bake_editor_mesh_displacement(mesh, badMidpoint), "refusal: midpoint out of range");
    auto badLevels = settings;
    badLevels.subdivisionLevels = 5;
    check(!bake_editor_mesh_displacement(mesh, badLevels), "refusal: subdivision levels out of range");
    CookedPolygonAsset empty;
    check(!bake_editor_mesh_displacement(empty, settings), "refusal: empty asset");
}
} // namespace

int main() {
    try {
        constant_offsets();
        midpoint_identity();
        ramp_plane_and_normals();
        sine_relief();
        subdivision_counts_and_uvs();
        cube_volume();
        refusals();
    } catch (const std::exception& error) {
        std::cerr << "dve_mesh_displacement_tests: " << error.what() << '\n';
        return 1;
    }
    std::cout << "dve_mesh_displacement_tests: all checks passed\n";
    return 0;
}
