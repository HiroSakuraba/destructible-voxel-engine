// Reproducible headless material preview for the supplied Cornell text scene.
#include "dve/render/scene_lighting.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <numbers>
#include <sstream>
using namespace dve;
using namespace dve::render;
namespace {
Float3 scale(Float3 p, float s) { return {p.x * s, p.y * s, p.z * s}; }
struct Box {
    MaterialId material;
    Float3 a, b;
    float angle{};
};
struct Sphere {
    Float3 center;
    float radius, ior;
};
CookedPolygonAsset sphere_mesh(Sphere s, float units, const std::string &mode) {
    CookedPolygonAsset asset;
    VoxelMaterialDefinition m;
    m.name = "Cornell sphere";
    m.transmission = 1;
    m.indexOfRefraction = s.ior;
    m.roughness = 0;
    if (mode == "rough")
        m.roughness = .25F;
    else if (mode == "metal" || mode == "mirror") {
        m.transmission = 0;
        m.metallic = 1;
        m.baseColor = {.85F, .72F, .5F, 1};
        m.roughness = mode == "mirror" ? 0 : .15F;
    } else if (mode == "opaque") {
        m.transmission = 0;
        m.baseColor = {.73F, .73F, .73F, 1};
        m.roughness = .8F;
    } else if (mode != "glass")
        throw std::runtime_error("mode must be glass, rough, metal, mirror or opaque");
    asset.materials = {m};
    asset.materialBindings.resize(1);
    constexpr unsigned lat = 32, lon = 64;
    const auto c = scale(s.center, units);
    float r = s.radius * units;
    for (unsigned y = 0; y <= lat; ++y)
        for (unsigned x = 0; x <= lon; ++x) {
            const float theta = std::numbers::pi_v<float> * float(y) / lat,
                        phi = 2 * std::numbers::pi_v<float> * float(x) / lon;
            Float3 n{std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)};
            PolygonVertex v;
            v.position = {c.x + r * n.x, c.y + r * n.y, c.z + r * n.z};
            v.normal = n;
            asset.vertices.push_back(v);
        }
    for (unsigned y = 0; y < lat; ++y)
        for (unsigned x = 0; x < lon; ++x) {
            unsigned a = y * (lon + 1) + x, b = (y + 1) * (lon + 1) + x;
            if (y > 0)
                asset.indices.insert(asset.indices.end(), {a, a + 1, b});
            if (y + 1 < lat)
                asset.indices.insert(asset.indices.end(), {b, a + 1, b + 1});
        }
    asset.submeshes = {{"sphere", 0, static_cast<unsigned>(asset.indices.size()), 0}};
    asset.bounds = {{c.x - r, c.y - r, c.z - r}, {c.x + r, c.y + r, c.z + r}};
    return asset;
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc < 3 || argc > 7)
            throw std::runtime_error("usage: dve_cornell_material_preview input.scene output.ppm [size=128] "
                                     "[paths=16] [glass|rough|metal|mirror|opaque] [passes-directory]");
        unsigned size = argc > 3 ? static_cast<unsigned>(std::stoul(argv[3])) : 128,
                 paths = argc > 4 ? static_cast<unsigned>(std::stoul(argv[4])) : 16;
        if (size < 16 || size > 2048 || paths < 1 || paths > 256)
            throw std::runtime_error("size 16..2048 and paths 1..256 required");
        const std::string mode = argc > 5 ? argv[5] : "glass";
        std::ifstream in(argv[1]);
        if (!in)
            throw std::runtime_error("cannot open scene");
        float voxel = 2.5F, meters = .025F, exposure = .7F;
        PolygonCamera camera;
        std::vector<VoxelMaterialDefinition> materials(1);
        std::map<std::string, MaterialId> ids;
        std::vector<Box> boxes;
        std::vector<Sphere> spheres;
        float x0 = 0, z0 = 0, x1 = 0, z1 = 0, intensity = 12;
        Float3 radiance{1, .88F, .72F};
        std::string line;
        unsigned lineNumber = 0;
        while (std::getline(in, line)) {
            ++lineNumber;
            line = line.substr(0, line.find('#'));
            std::istringstream row(line);
            std::string kind;
            if (!(row >> kind))
                continue;
            if (kind == "voxel_size")
                row >> voxel;
            else if (kind == "meters_per_voxel")
                row >> meters;
            else if (kind == "exposure")
                row >> exposure;
            else if (kind == "image" || kind == "gi_samples")
                continue; // explicit CLI preview budgets
            else if (kind == "camera") {
                std::string fov;
                float degrees;
                row >> camera.position.x >> camera.position.y >> camera.position.z >> camera.target.x >>
                    camera.target.y >> camera.target.z >> fov >> degrees;
                camera.verticalFieldOfViewRadians = degrees * std::numbers::pi_v<float> / 180;
            } else if (kind == "material") {
                std::string name;
                VoxelMaterialDefinition m;
                row >> name >> m.baseColor.x >> m.baseColor.y >> m.baseColor.z;
                m.name = name;
                m.roughness = .8F;
                ids[name] = static_cast<MaterialId>(materials.size());
                materials.push_back(m);
            } else if (kind == "box") {
                std::string name;
                Box b;
                row >> name >> b.a.x >> b.a.y >> b.a.z >> b.b.x >> b.b.y >> b.b.z >> b.angle;
                if (!ids.contains(name))
                    throw std::runtime_error("unknown box material");
                b.material = ids[name];
                boxes.push_back(b);
            } else if (kind == "sphere") {
                Sphere s;
                row >> s.center.x >> s.center.y >> s.center.z >> s.radius >> s.ior;
                spheres.push_back(s);
            } else if (kind == "light") {
                std::string token;
                unsigned oldSamples;
                row >> x0 >> z0 >> x1 >> z1 >> token >> radiance.x >> radiance.y >> radiance.z >> token >>
                    oldSamples >> token >> intensity;
            } else
                throw std::runtime_error("unknown directive at line " + std::to_string(lineNumber));
            if (!row)
                throw std::runtime_error("malformed directive at line " + std::to_string(lineNumber));
        }
        if (voxel <= 0 || meters <= 0 || spheres.empty() || x1 <= x0 || z1 <= z0)
            throw std::runtime_error("invalid Cornell scene dimensions");
        const float units = 1 / voxel;
        camera.position = scale(camera.position, units);
        camera.target = scale(camera.target, units);
        camera.nearPlane = .1F;
        camera.farPlane = 10000;
        std::vector<std::unique_ptr<VoxelObject>> objects;
        std::vector<VoxelReferenceInstance> instances;
        for (const auto &b : boxes) {
            auto object = std::make_unique<VoxelObject>(objects.size() + 1);
            const Float3 extent{(b.b.x - b.a.x) * units, (b.b.y - b.a.y) * units, (b.b.z - b.a.z) * units};
            for (int z = 0; z < int(std::ceil(extent.z)); ++z)
                for (int y = 0; y < int(std::ceil(extent.y)); ++y)
                    for (int x = 0; x < int(std::ceil(extent.x)); ++x)
                        object->set_voxel({x, y, z}, b.material);
            RigidTransform transform;
            transform.rotation =
                quaternion_from_axis_angle({0, 1, 0}, b.angle * std::numbers::pi_v<float> / 180);
            auto center = scale(add(b.a, b.b), .5F * units);
            transform.position = subtract(center, transform_vector(transform, scale(extent, .5F)));
            instances.push_back({object->id(), object.get(), transform, materials});
            objects.push_back(std::move(object));
        }
        std::vector<CookedPolygonAsset> meshes;
        meshes.reserve(spheres.size() + 1);
        for (const auto &s : spheres)
            meshes.push_back(sphere_mesh(s, units, mode));
        CookedPolygonAsset lamp;
        VoxelMaterialDefinition lm;
        lm.name = "Cornell emitter";
        lm.shadingModel = MaterialShadingModel::Emissive;
        lm.baseColor = {0, 0, 0, 1};
        lm.emissive = scale(radiance, intensity);
        lamp.materials = {lm};
        lamp.materialBindings.resize(1);
        for (Float3 p :
             {Float3{x0, 549, z0}, Float3{x1, 549, z0}, Float3{x1, 549, z1}, Float3{x0, 549, z1}}) {
            PolygonVertex v;
            v.position = scale(p, units);
            v.normal = {0, -1, 0};
            lamp.vertices.push_back(v);
        }
        lamp.indices = {0, 1, 2, 0, 2, 3};
        lamp.submeshes = {{"lamp", 0, 6, 0}};
        lamp.bounds = {{x0 * units, 549 * units, z0 * units}, {x1 * units, 549 * units, z1 * units}};
        meshes.push_back(std::move(lamp));
        std::vector<PolygonRenderInstance> polygons;
        for (auto &mesh : meshes)
            polygons.push_back({1000 + polygons.size(), &mesh});
        RenderEnvironment e;
        e.sunIntensity = 0;
        e.skyColor = {};
        e.groundColor = {};
        e.globalIlluminationIntensity = 1;
        e.pathTraceBounces = 6;
        e.pathTraceSamples = paths;
        e.localLightSamples = 4;
        e.bloomIntensity = 0;
        e.shadowBiasMeters = .001F;
        e.globalIlluminationMaxDistanceMeters = 100;
        PolygonRenderTarget target;
        target.resize(size, size);
        target.enable_passes();
        const auto stats = render_hybrid_reference(instances, polygons, camera, e, target, {}, meters, 4);
        for (std::size_t i = 0; i < target.hdrColor.size(); ++i) {
            const auto c = target.hdrColor[i];
            const auto s = target.passes.light_sum(i);
            if (!std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z) ||
                std::abs(c.x - s.x) > 1e-4F * (1 + std::abs(c.x)))
                throw std::runtime_error("nonfinite output or AOV mismatch");
        }
        std::string error;
        if (argc > 6 &&
            !write_render_passes_pfm(target, argv[6], mode + "_", camera.farPlane * meters, &error))
            throw std::runtime_error(error);
        if (!write_polygon_render_ppm(argv[2], target, exposure, &error))
            throw std::runtime_error(error);
        std::cout << "Rendered " << mode << " " << size << "x" << size << " paths=" << paths
                  << " voxel hits=" << stats.voxels.hitRays << "\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
