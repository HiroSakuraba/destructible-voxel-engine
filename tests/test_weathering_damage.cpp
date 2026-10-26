// Weathering recipes, damage marks and decals on voxels:
//   - marks: exposure inside the crater, soot fading from the rim to the reach, crack lines in
//     the ring only; other objects untouched; strongest of overlapping marks; the list keeps the
//     newest up to its limit; sub-pixel cracks fade;
//   - shading: soot darkens to 7 %, exposure lightens and dulls;
//   - GameWorld damage events become marks in the object's own space (as the CPU player does),
//     and the marks move with the object;
//   - on voxels: a crater's marks shade the floor around it, and decals project onto voxels
//     exactly (coverage read from the emission pass);
//   - weathering recipes: snow tops, moss in hollows, rust on edges, wetness multiplies; amounts
//     widen the bands; add_weathering refuses a full layer stack.
#include "dve/game_world.hpp"
#include "dve/material_decal.hpp"
#include "dve/render/voxel_reference_renderer.hpp"
#include "dve/rigid_body_adapter.hpp"
#include "dve/surface_marks.hpp"
#include "dve/transform.hpp"
#include "dve/weathering.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {
using namespace dve;
using namespace dve::render;

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::printf("FAIL: %s\n", message.c_str()); ++g_failures; }
}

SurfaceMark crater(std::uint64_t object, Float3 c, float r) {
    SurfaceMark m;
    m.objectId = object;
    m.centerMeters = c;
    m.radiusMeters = r;
    m.seed = 3;
    return m;
}

void test_marks() {
    const std::vector<SurfaceMark> marks{crater(7, {0, 0, 0}, 1.0F)};
    const SurfaceMarkSample inside = sample_surface_marks(marks, 7, {0.0F, -0.98F, 0.0F});
    check(inside.exposure == 0.6F && inside.scorch == 0.85F, "the crater wall is freshly exposed and scorched");
    check(inside.cracks == 0.0F, "no cracks inside the crater");
    // Soot falls off from the rim to 1.8 r.
    float previous = 2.0F;
    bool falls = true;
    for (int i = 0; i <= 10; ++i) {
        const float d = 1.0F + 0.09F * static_cast<float>(i);
        float mean = 0.0F;
        for (int k = 0; k < 64; ++k) {
            const float a = 0.0982F * static_cast<float>(k);
            mean += sample_surface_marks(marks, 7, {d * std::cos(a), 0.0F, d * std::sin(a)}).scorch / 64.0F;
        }
        falls = falls && mean <= previous + 1e-4F;
        previous = mean;
    }
    check(falls && previous < 0.1F, "soot fades from the rim to the reach");
    check(!sample_surface_marks(marks, 7, {2.5F, 0, 0}).any(), "nothing beyond the reach");
    check(!sample_surface_marks(marks, 8, {0, -0.98F, 0}).any(), "marks belong to their object");
    // Cracks: some lines in the ring, thin (a small share of the ring).
    std::size_t lines = 0, samples = 0;
    for (int i = 0; i < 4000; ++i) {
        const float a = 0.0157F * static_cast<float>(i), d = 1.1F + 0.9F * static_cast<float>(i % 97) / 97.0F;
        const SurfaceMarkSample s = sample_surface_marks(marks, 7, {d * std::cos(a), 0.05F, d * std::sin(a)});
        lines += s.cracks > 0.3F ? 1U : 0U;
        ++samples;
    }
    const double share = static_cast<double>(lines) / static_cast<double>(samples);
    std::printf("cracks: %.3f of the ring on a line\n", share);
    check(share > 0.01 && share < 0.25, "cracks are thin lines through the ring");
    // Sub-pixel cracks fade.
    float near = 0.0F, far = 0.0F;
    for (int i = 0; i < 4000; ++i) {
        const float a = 0.0157F * static_cast<float>(i), d = 1.1F + 0.9F * static_cast<float>(i % 97) / 97.0F;
        near = std::max(near, sample_surface_marks(marks, 7, {d * std::cos(a), 0.05F, d * std::sin(a)}, 0.001F).cracks);
        far = std::max(far, sample_surface_marks(marks, 7, {d * std::cos(a), 0.05F, d * std::sin(a)}, 0.5F).cracks);
    }
    check(near > 0.5F && far < 0.05F, "cracks thinner than a pixel fade");
    // Overlap: the strongest of each effect.
    std::vector<SurfaceMark> two = marks;
    two.push_back(crater(7, {3, 0, 0}, 1.0F));
    two[1].scorch = 0.3F;
    check(sample_surface_marks(two, 7, {0, -0.98F, 0}).scorch == 0.85F, "overlapping marks keep the strongest soot");

    // Shading.
    VoxelMaterialDefinition m;
    m.baseColor = {0.5F, 0.4F, 0.3F, 1.0F};
    m.roughness = 0.3F;
    VoxelMaterialDefinition sooty = m;
    apply_surface_marks(sooty, {0.0F, 1.0F, 0.0F});
    check(std::abs(sooty.baseColor.x - 0.5F * 0.07F) < 1e-6F && sooty.roughness == 0.95F, "full soot darkens to 7 % and roughens");
    VoxelMaterialDefinition fresh = m;
    apply_surface_marks(fresh, {1.0F, 0.0F, 0.0F});
    check(fresh.baseColor.x > m.baseColor.x && fresh.roughness >= 0.9F, "exposure lightens and dulls");

    // The list keeps the newest marks.
    SurfaceMarkSettings settings;
    settings.maximumMarks = 3;
    SurfaceMarkList list(settings);
    for (int i = 0; i < 5; ++i) list.add_crater(1, {static_cast<float>(i), 0, 0}, 0.5F);
    check(list.marks().size() == 3 && list.marks().front().centerMeters.x == 2.0F && list.marks().back().centerMeters.x == 4.0F,
          "the list keeps the newest marks");
    list.add_crater(1, {0, 0, 0}, -1.0F);
    check(list.marks().size() == 3, "invalid marks are ignored");
    settings.enabled = false;
    list.set_settings(settings);
    list.add_crater(1, {9, 0, 0}, 0.5F);
    check(list.marks().back().centerMeters.x == 4.0F, "a disabled list adds nothing");
}

// GameWorld damage -> marks in the object's own space, as the CPU player hooks it.
void test_world_events() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameObjectDesc desc;
    desc.name = "Wall";
    desc.voxelSizeMeters = 0.5F;
    desc.dynamic = false;
    desc.voxels = std::make_unique<VoxelObject>(1U);
    for (int x = 0; x < 8; ++x)
        for (int y = 0; y < 8; ++y)
            for (int z = 0; z < 2; ++z) desc.voxels->set_voxel({x, y, z}, 1);
    desc.transform = make_rigid_transform({10.0F, 0.0F, 0.0F}, quaternion_from_axis_angle({0, 1, 0}, 0.5F));
    std::string error;
    const GameObjectId id = world.create_object(std::move(desc), &error);
    check(id != kInvalidGameObjectId, "create wall: " + error);
    auto marks = std::make_shared<SurfaceMarkList>();
    GameWorld* w = &world;
    world.on_damage([w, marks](const GameDamageEvent& event) {
        if (event.destroyed || event.removedVoxelCount == 0U) return;
        if (const auto t = w->transform(event.objectId)) marks->add_crater(event.objectId, inverse_transform_point(*t, event.worldCenter), event.radius);
    });
    const RigidTransform pose = *world.transform(id);
    const Float3 local{2.0F, 2.0F, 0.5F};
    const auto removed = world.damage_sphere(id, transform_point(pose, local), 0.8F);
    check(removed && *removed > 0U && marks->marks().size() == 1U, "a carve leaves one mark");
    if (!marks->marks().empty()) {
        const SurfaceMark& m = marks->marks()[0];
        check(m.objectId == id && length(subtract(m.centerMeters, local)) < 1e-4F && m.radiusMeters == 0.8F,
              "the mark sits at the crater, in the object's own space");
    }
}

// --- On voxels ---------------------------------------------------------------------------------

VoxelMaterialDefinition emitter(Float3 c) {
    VoxelMaterialDefinition m;
    m.baseColor = {0.5F, 0.5F, 0.5F, 1.0F};
    m.specular = 0.0F;
    m.emissive = c;
    return m;
}
struct Floor {
    VoxelObject object{1U};
    std::array<VoxelMaterialDefinition, 2> materials{};
    std::vector<VoxelReferenceInstance> instances;
    explicit Floor(const RigidTransform& pose = {}) {
        materials[1] = emitter({0, 0, 1});
        for (int x = -30; x < 30; ++x)
            for (int z = -30; z < 30; ++z)
                for (int y = -2; y <= 0; ++y) object.set_voxel({x, y, z}, 1);
        instances.push_back({5U, &object, pose, materials, true, {1, 1, 1}});
    }
};
PolygonRenderTarget draw(const Floor& floor, const PolygonCamera& c, std::span<const SurfaceMark> marks, std::span<const MaterialDecal> decals,
                         Float3 sunDirection = normalize(Float3{0.3F, 1.0F, 0.2F})) {
    RenderEnvironment e;
    e.sunIntensity = 2.0F;
    e.sunDirection = sunDirection;
    e.shadowMode = ShadowMode::Off;
    e.globalIlluminationMode = GlobalIlluminationMode::Off;
    e.ambientOcclusionSamples = 0;
    PolygonRenderTarget t;
    t.resize(48, 48);
    t.enable_passes();
    ReferenceVoxelRenderer r;
    r.threadCount = 2;
    r.surfaceMarks = marks;
    r.decals = decals;
    (void)r.render(floor.instances, c, e, t);
    return t;
}
PolygonCamera down(Float3 at) {
    PolygonCamera c;
    c.position = add(at, {0.0F, 20.0F, 0.01F});
    c.target = at;
    c.up = {0, 0, 1};
    c.verticalFieldOfViewRadians = 0.9F;
    c.nearPlane = 0.1F;
    c.farPlane = 200.0F;
    return c;
}

void test_render() {
    // A mark 1 m across at the floor's top centre (object metres: voxel units x 0.1).
    const std::vector<SurfaceMark> marks{crater(5U, {0.05F, 0.1F, 0.05F}, 0.5F)};
    const Floor floor;
    const PolygonRenderTarget plain = draw(floor, down({0.5F, 1.0F, 0.5F}), {}, {});
    const PolygonRenderTarget marked = draw(floor, down({0.5F, 1.0F, 0.5F}), marks, {});
    const std::size_t centre = 24U * 48U + 24U, corner = 2U * 48U + 2U;
    const float darkening = marked.passes.directDiffuse[centre].x / plain.passes.directDiffuse[centre].x;
    std::printf("damage mark: centre light x %.3f, far corner x %.3f\n", darkening,
                marked.passes.directDiffuse[corner].x / plain.passes.directDiffuse[corner].x);
    check(darkening < 0.3F, "the crater's soot darkens the floor around it");
    check(marked.passes.directDiffuse[corner].x == plain.passes.directDiffuse[corner].x, "beyond the reach the floor is untouched");
    // The marks move with the object: shift and turn the floor and the camera together.
    const RigidTransform pose{{7.0F, -3.0F, 4.0F}, quaternion_from_axis_angle({0, 1, 0}, 0.9F)};
    const Floor moved(pose);
    PolygonCamera c = down({0.5F, 1.0F, 0.5F});
    PolygonCamera movedCamera = c;
    movedCamera.position = transform_point(pose, c.position);
    movedCamera.target = transform_point(pose, c.target);
    movedCamera.up = rotate(pose.rotation, c.up);
    const PolygonRenderTarget follows = draw(moved, movedCamera, marks, {}, rotate(pose.rotation, normalize(Float3{0.3F, 1.0F, 0.2F})));
    float worst = 0.0F;
    for (std::size_t i = 0; i < follows.hdrColor.size(); ++i)
        worst = std::max(worst, std::abs(follows.passes.directDiffuse[i].x - marked.passes.directDiffuse[i].x));
    std::printf("damage mark on a moved floor: largest difference %.5f\n", worst);
    check(worst < 2e-3F * std::max(1.0F, plain.passes.directDiffuse[centre].x), "the marks move with their object");

    // Decals project onto voxels: a 1 x 1 unit box decal emitting red over the floor top.
    MaterialDecal decal;
    decal.id = 1;
    decal.center = {0.5F, 1.0F, 0.5F};
    decal.tangent = {1, 0, 0};
    decal.bitangent = {0, 0, 1};
    decal.projectionNormal = {0, 1, 0};
    decal.halfExtents = {3.0F, 3.0F, 0.5F};
    decal.edgeFeather = 0.0F;
    decal.layer.surface.baseColor = {0.5F, 0.5F, 0.5F, 1.0F};
    decal.layer.surface.emissive = {1, 0, 0};
    decal.layer.authoredWeight = 1.0F;
    decal.layer.mask = 1.0F;
    decal.layer.heightBlendStrength = 0.0F;
    decal.channels = MaterialDecalChannel::Emissive;
    const std::vector<MaterialDecal> decals{decal};
    const PolygonRenderTarget decaled = draw(floor, down({0.5F, 1.0F, 0.5F}), {}, decals);
    std::size_t inside = 0, outside = 0;
    bool exact = true;
    for (std::size_t i = 0; i < decaled.hdrColor.size(); ++i) {
        const bool in = decaled.passes.emission[i].x > 0.5F;
        (in ? inside : outside) += 1U;
        exact = exact && (decaled.passes.emission[i].x == 1.0F || decaled.passes.emission[i].x == 0.0F);
    }
    std::printf("voxel decal: %zu pixels inside, %zu outside\n", inside, outside);
    check(inside > 100 && outside > 100 && exact, "decals project onto voxel surfaces");
}

// --- Weathering ----------------------------------------------------------------------------------

void test_weathering() {
    MaterialLayerRuleContext top, wall, hollow, edge;
    top.worldNormal = {0, 1, 0};
    wall.worldNormal = {1, 0, 0};
    hollow.worldNormal = {0, 1, 0};
    hollow.ambientOcclusion = 0.3F;
    edge.edgeDistanceMeters = 0.001F;
    const auto factor = [](WeatheringKind k, float amount, const MaterialLayerRuleContext& c) {
        return material_layer_rule_factor(make_weathering_layer(k, amount).layer.rules, c);
    };
    // Snow: tops, not walls; the band opens with amount.
    check(factor(WeatheringKind::Snow, 0.5F, top) > 0.6F && factor(WeatheringKind::Snow, 0.5F, wall) == 0.0F, "snow on tops, not walls");
    MaterialLayerRuleContext slope;
    slope.worldNormal = normalize(Float3{1.0F, 1.0F, 0.0F});   // cos 0.71
    check(factor(WeatheringKind::Snow, 0.0F, slope) == 0.0F && factor(WeatheringKind::Snow, 1.0F, slope) > 0.6F, "snow spreads to slopes with amount");
    // Moss: in hollows, not in the open.
    check(factor(WeatheringKind::Moss, 0.5F, hollow) > 0.6F && factor(WeatheringKind::Moss, 0.5F, top) == 0.0F, "moss in hollows");
    // Rust and edge wear: edges only.
    check(factor(WeatheringKind::Rust, 0.5F, edge) > 0.5F && factor(WeatheringKind::Rust, 0.5F, top) == 0.0F, "rust along edges");
    check(factor(WeatheringKind::EdgeWear, 1.0F, edge) > 0.4F && factor(WeatheringKind::EdgeWear, 1.0F, top) == 0.0F, "wear along edges");
    check(make_weathering_layer(WeatheringKind::Wetness, 1.0F).layer.blendMode == MaterialLayerBlendMode::Multiply, "wetness multiplies");
    for (int k = 0; k <= 5; ++k)
        check(material_layer_rules_valid(make_weathering_layer(static_cast<WeatheringKind>(k), 0.7F).layer.rules),
              std::string("valid recipe ") + weathering_kind_label(static_cast<WeatheringKind>(k)));
    // add_weathering.
    std::vector<VoxelMaterialDefinition> table(2);
    std::string error;
    check(add_weathering(table, 1, WeatheringKind::Snow, 0.5F, {0, 1, 0}, 1, &error) && table.size() == 3 && table[1].layers.size() == 1 &&
              table[1].layers[0].sourceMaterial == 2 && table[2].name == "Snow",
          "add_weathering appends the source and the layer");
    for (int i = 0; i < 3; ++i) check(add_weathering(table, 1, WeatheringKind::Dust, 0.5F, {0, 1, 0}, 1, &error), "more layers");
    const std::size_t before = table.size();
    check(!add_weathering(table, 1, WeatheringKind::Moss, 0.5F, {0, 1, 0}, 1, &error) && table.size() == before, "a full layer stack is refused");
    check(!add_weathering(table, 40, WeatheringKind::Moss, 0.5F, {0, 1, 0}, 1, &error), "an unknown target is refused");

    // Rendered: snow on a weathered floor's top is white, its bare base blue (emission).
    Floor floor;
    std::vector<VoxelMaterialDefinition> materials(floor.materials.begin(), floor.materials.end());
    check(add_weathering(materials, 1, WeatheringKind::Snow, 1.0F, {0, 1, 0}, 1, &error), "weather the floor");
    materials[2].emissive = {1, 0, 0};
    floor.instances[0].materials = materials;
    const PolygonRenderTarget t = draw(floor, down({0.5F, 1.0F, 0.5F}), {}, {});
    float lo = 1.0F;
    for (std::size_t i = 0; i < t.hdrColor.size(); ++i) lo = std::min(lo, t.passes.emission[i].x);
    check(lo > 0.3F, "snow covers the floor's top");
}

} // namespace

int main() {
    test_marks();
    test_world_events();
    test_render();
    test_weathering();
    if (g_failures == 0) std::printf("dve_weathering_damage_tests: PASS\n");
    return g_failures == 0 ? 0 : 1;
}
