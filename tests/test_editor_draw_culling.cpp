// VoxelObject content revisions, the O(objects) scene fingerprint built on them, and
// culling of buried voxels from the editor voxel draw list.
//
// The culling contract is visual: painting the culled list exactly the way
// render_native_editor paints voxels (filled squares + anchor crosses, far to near)
// must give the same pixels as painting the full list.
#include "dve/editor_document.hpp"
#include "dve/editor_materials.hpp"
#include "dve/editor_viewport.hpp"
#include "dve/voxel_object.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace {
using namespace dve;
using namespace dve::editor;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

// ---------------------------------------------------------------- revisions

void test_voxel_revisions() {
    VoxelObject a(1);
    VoxelObject b(2);
    require(a.revision() != b.revision(), "new objects share a revision");

    auto revision = a.revision();
    (void)a.set_voxel({1, 2, 3}, 4);
    require(a.revision() != revision, "set_voxel did not change the revision");
    revision = a.revision();
    (void)a.set_voxel({1, 2, 3}, 4);
    require(a.revision() == revision, "a no-op set_voxel changed the revision");
    (void)a.set_voxel({1, 2, 3}, 5);
    require(a.revision() != revision, "material change did not change the revision");

    revision = a.revision();
    (void)std::as_const(a).find_brick(brick_key_from_voxel({1, 2, 3}));
    (void)a.material_at({1, 2, 3});
    require(a.revision() == revision, "const reads changed the revision");
    (void)a.find_brick(brick_key_from_voxel({1, 2, 3}));
    require(a.revision() != revision, "mutable find_brick did not change the revision");

    revision = a.revision();
    a.fill_brick({4, 0, 0}, 2);
    require(a.revision() != revision, "fill_brick did not change the revision");

    revision = a.revision();
    BrickMutation removal;
    removal.removeMask.set(voxel_index_unchecked(local_voxel_from_global({1, 2, 3})));
    (void)a.apply(brick_key_from_voxel({1, 2, 3}), removal);
    require(a.revision() != revision, "apply did not change the revision");
    revision = a.revision();
    (void)a.apply(brick_key_from_voxel({1, 2, 3}), removal);
    require(a.revision() == revision, "a no-op apply on an existing brick changed the revision");

    revision = a.revision();
    VoxelBrickSnapshot snapshot = a.snapshot_brick({4, 0, 0});
    snapshot.key = {9, 9, 9};
    require(a.replace_brick(snapshot), "replace_brick failed");
    require(a.revision() != revision, "replace_brick did not change the revision");

    // Moves: the destination names the moved content; the source must not keep it.
    revision = a.revision();
    VoxelObject moved(std::move(a));
    require(moved.revision() == revision, "move construction changed the content revision");
    require(a.revision() != revision, "moved-from object kept the content revision");
    VoxelObject target(3);
    (void)target.set_voxel({0, 0, 0}, 1);
    const auto targetBefore = target.revision();
    target = std::move(moved);
    require(target.revision() == revision, "move assignment lost the content revision");
    require(target.revision() != targetBefore, "move assignment kept the overwritten revision");
    require(moved.revision() != revision, "move-assigned-from object kept the content revision");
    std::printf("voxel revisions: OK\n");
}

void test_scene_fingerprint() {
    EditorDocument document("fingerprint");
    for (EditorObjectId id = 1; id <= 3; ++id) {
        EditorObject object(id, "o");
        object.voxels = std::make_unique<VoxelObject>(id);
        for (int x = 0; x < 20; ++x) (void)object.voxels->set_voxel({x, 0, static_cast<int>(id)}, 1);
        document.add_object(std::move(object));
    }
    const auto base = editor_scene_render_fingerprint(document);
    require(editor_scene_render_fingerprint(document) == base, "fingerprint is not deterministic");

    EditorObject* object = document.find_object(2);
    (void)object->voxels->set_voxel({3, 0, 2}, 1);  // no-op: already this material
    require(editor_scene_render_fingerprint(document) == base, "no-op edit changed the fingerprint");
    (void)object->voxels->set_voxel({3, 0, 2}, 2);
    const auto edited = editor_scene_render_fingerprint(document);
    require(edited != base, "voxel material edit did not change the fingerprint");
    (void)object->voxels->set_voxel({3, 0, 2}, kAirMaterial);
    require(editor_scene_render_fingerprint(document) != edited, "voxel removal did not change the fingerprint");

    // Replacing an object's voxels with identical content still invalidates: caches
    // keyed on the old object must not be reused for a different one.
    const auto beforeSwap = editor_scene_render_fingerprint(document);
    auto replacement = std::make_unique<VoxelObject>(2);
    for (const auto& [key, brick] : object->voxels->bricks())
        require(replacement->replace_brick(object->voxels->snapshot_brick(key)), "copy failed");
    object->voxels = std::move(replacement);
    require(editor_scene_render_fingerprint(document) != beforeSwap, "voxel object replacement kept the fingerprint");

    const auto beforeMove = editor_scene_render_fingerprint(document);
    object->transform.position.x += 0.25F;
    require(editor_scene_render_fingerprint(document) != beforeMove, "transform change kept the fingerprint");
    const auto beforeFlags = editor_scene_render_fingerprint(document);
    object->flags.visible = false;
    require(editor_scene_render_fingerprint(document) != beforeFlags, "visibility change kept the fingerprint");
    std::printf("scene fingerprint: OK\n");
}

// ---------------------------------------------------------------- culling

using Shape = std::function<MaterialId(int, int, int)>;

EditorObject make_object(EditorObjectId id, int lo, int hi, const Shape& shape, float voxelSize = 0.1F) {
    EditorObject object(id, "shape");
    object.voxels = std::make_unique<VoxelObject>(id);
    object.voxelSizeMeters = voxelSize;
    for (int x = lo; x < hi; ++x)
        for (int y = lo; y < hi; ++y)
            for (int z = lo; z < hi; ++z)
                if (const MaterialId material = shape(x, y, z); material != kAirMaterial)
                    (void)object.voxels->set_voxel({x, y, z}, material);
    return object;
}

std::vector<std::uint32_t> paint(const std::vector<EditorVoxelDrawItem>& items, UiRect viewport) {
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(viewport.width * viewport.height), 0U);
    const auto fill = [&](int x, int y, int w, int h, std::uint32_t colour) {
        for (int yy = std::max(0, y); yy < std::min(viewport.height, y + h); ++yy)
            for (int xx = std::max(0, x); xx < std::min(viewport.width, x + w); ++xx)
                pixels[static_cast<std::size_t>(yy * viewport.width + xx)] = colour;
    };
    for (const EditorVoxelDrawItem& item : items) {
        const float shade = std::clamp(1.1F - item.depth * 0.018F, 0.42F, 1.0F);
        const std::uint32_t colour = (static_cast<std::uint32_t>(item.material) << 24U) ^
                                     static_cast<std::uint32_t>(shade * 65535.0F) ^ (item.selected ? 0x800000U : 0U);
        const int radius = std::max(1, static_cast<int>(item.pixelRadius));
        const int x = static_cast<int>(item.screenX) - viewport.x;
        const int y = static_cast<int>(item.screenY) - viewport.y;
        fill(x - radius, y - radius, radius * 2 + 1, radius * 2 + 1, colour | 1U);
        if (item.anchored) {
            fill(x - 3, y - 1, 7, 2, 0xFFDA48U);
            fill(x - 1, y - 3, 2, 7, 0xFFDA48U);
        }
    }
    return pixels;
}

std::vector<EditorVoxelDrawItem> draw_list(const EditorDocument& document, const EditorCamera& camera, UiRect viewport,
                                           bool cull, std::size_t cap = 120000) {
    EditorViewportSettings settings;
    settings.cullEnclosedVoxels = cull;
    settings.maximumDrawVoxels = cap;
    const EditorMaterialLibrary materials;
    return build_voxel_draw_list(document, materials, camera, viewport, settings, std::set<EditorObjectId>{1});
}

EditorCamera orbit(float yaw, float pitch, float distance, Float3 target) {
    EditorCamera camera;
    camera.target = target;
    camera.position = {target.x + distance * std::cos(pitch) * std::cos(yaw), target.y + distance * std::sin(pitch),
                       target.z + distance * std::cos(pitch) * std::sin(yaw)};
    return camera;
}

std::uint32_t mix(int x, int y, int z) {
    std::uint32_t h = static_cast<std::uint32_t>(x * 73856093) ^ static_cast<std::uint32_t>(y * 19349663) ^
                      static_cast<std::uint32_t>(z * 83492791);
    h ^= h >> 13U;
    h *= 0x5bd1e995U;
    return h ^ (h >> 15U);
}

// The culled set is exactly "within two voxels of empty space", across brick
// boundaries and negative coordinates.
void test_exposed_set_matches_reference() {
    for (int trial = 0; trial < 12; ++trial) {
        const int lo = -19 + trial % 5;
        const int hi = lo + 22 + trial % 9;
        const int density = 60 + (trial * 7) % 40;
        EditorDocument document("reference");
        EditorObject object = make_object(1, lo, hi, [&](int x, int y, int z) {
            return static_cast<int>(mix(x + trial, y, z) % 100U) < density ? static_cast<MaterialId>(1 + mix(x, y, z) % 3U)
                                                                           : kAirMaterial;
        }, 0.05F);
        std::set<std::tuple<int, int, int>> expected;
        for (const auto& [key, brick] : object.voxels->bricks()) {
            brick.occupancy().for_each_set([&](std::uint16_t index) {
                const Int3 v = global_from_local(key, local_from_index_unchecked(index));
                for (int dx = -2; dx <= 2; ++dx)
                    for (int dy = -2; dy <= 2; ++dy)
                        for (int dz = -2; dz <= 2; ++dz)
                            if ((dx || dy || dz) && !object.voxels->occupied_at({v.x + dx, v.y + dy, v.z + dz})) {
                                expected.insert({v.x, v.y, v.z});
                                return;
                            }
            });
        }
        document.add_object(std::move(object));
        EditorCamera camera;  // far orthographic view: culling active, whole object on screen
        camera.projection = EditorProjection::Orthographic;
        camera.orthographicHeight = 40.0F;
        camera.target = {0.0F, 0.0F, 0.0F};
        camera.position = {30.0F, 22.0F, 26.0F};
        const auto items = draw_list(document, camera, {0, 0, 800, 800}, true, 10'000'000);
        std::set<std::tuple<int, int, int>> actual;
        for (const auto& item : items) actual.insert({item.voxel.x, item.voxel.y, item.voxel.z});
        require(actual == expected, "culled set differs from the two-layer reference in trial " + std::to_string(trial));
    }
    std::printf("exposed set reference: OK\n");
}

void test_culling_is_pixel_identical() {
    const std::vector<std::pair<std::string, Shape>> shapes{
        {"sphere", [](int x, int y, int z) { return x * x + y * y + z * z <= 225 ? MaterialId{2} : kAirMaterial; }},
        {"notched cube", [](int x, int y, int z) {
             if (x >= -8 && y >= -8 && z >= 4) return kAirMaterial;
             if (x >= -4 && x < 0 && z < -6) return kAirMaterial;
             return static_cast<MaterialId>(1 + ((x / 4 + y / 4 + z / 4) & 3));
         }},
        {"porous blob", [](int x, int y, int z) {
             if (x * x + y * y + z * z > 196) return kAirMaterial;
             return mix(x, y, z) % 100U < 18U ? kAirMaterial : static_cast<MaterialId>(1 + mix(x, y, z) % 4U);
         }},
    };
    std::size_t framesCulled = 0, before = 0, after = 0;
    for (const auto& [name, shape] : shapes) {
        EditorDocument document("pixels");
        EditorObject object = make_object(1, -16, 16, shape);
        object.anchors.insert({0, 0, 0});  // buried anchor
        object.anchors.insert({-15, 0, 0});
        document.add_object(std::move(object));
        for (int pose = 0; pose < 96; ++pose) {
            const UiRect viewport = pose % 4 == 3 ? UiRect{10, 20, 320, 180} : UiRect{0, 0, 960, 600};
            EditorCamera camera = orbit(pose * 0.61F, -1.1F + (pose % 9) * 0.27F, 2.0F + (pose % 13) * 0.9F, {0, 0, 0});
            if (pose % 11 == 5) {
                camera.projection = EditorProjection::Orthographic;
                camera.orthographicHeight = 2.0F + pose % 5;
            }
            const auto full = draw_list(document, camera, viewport, false);
            const auto culled = draw_list(document, camera, viewport, true);
            require(full.size() < 120000U, "test scene hit the draw cap");
            before += full.size();
            after += culled.size();
            if (culled.size() < full.size()) ++framesCulled;
            require(paint(full, viewport) == paint(culled, viewport),
                    name + ": culling changed pixels at pose " + std::to_string(pose));
        }
    }
    require(framesCulled > 100U, "culling was rarely active; the test is not exercising it");
    std::printf("culling pixel identity: OK (%zu culled frames, %zu -> %zu items)\n", framesCulled, before, after);
}

void test_culling_guards() {
    EditorDocument document("guards");
    EditorObject object = make_object(1, 0, 32, [](int, int, int) { return MaterialId{1}; });
    object.anchors.insert({16, 16, 16});
    document.add_object(std::move(object));
    const Float3 centre{1.6F, 1.6F, 1.6F};
    const UiRect viewport{0, 0, 960, 600};

    // Normal distance: buried voxels are dropped, the buried anchor is kept.
    EditorCamera camera = orbit(0.7F, 0.5F, 8.0F, centre);
    auto culled = draw_list(document, camera, viewport, true);
    auto full = draw_list(document, camera, viewport, false);
    require(culled.size() < full.size(), "culling inactive for a solid cube at normal distance");
    require(std::any_of(culled.begin(), culled.end(), [](const auto& item) {
                return item.anchored && item.voxel == Int3{16, 16, 16};
            }), "buried anchor was culled");
    // Every culled item is also in the full list, unchanged.
    for (const auto& item : culled)
        require(std::any_of(full.begin(), full.end(), [&](const auto& other) {
                    return other.voxel == item.voxel && other.screenX == item.screenX && other.screenY == item.screenY &&
                           other.depth == item.depth && other.pixelRadius == item.pixelRadius;
                }), "culling altered a kept item");

    // Camera inside the object (near plane cuts the surface): nothing culled.
    camera = orbit(0.3F, 0.2F, 0.5F, centre);
    require(draw_list(document, camera, viewport, true).size() == draw_list(document, camera, viewport, false).size(),
            "culled with the camera inside the object");
    camera.projection = EditorProjection::Orthographic;
    camera.orthographicHeight = 4.0F;
    require(draw_list(document, camera, viewport, true).size() == draw_list(document, camera, viewport, false).size(),
            "culled with an orthographic camera inside the object");

    // Very close (voxels larger than the splat clamp covers): nothing culled.
    camera = orbit(0.3F, 0.2F, 2.2F, centre);
    require(draw_list(document, camera, viewport, true).size() == draw_list(document, camera, viewport, false).size(),
            "culled while voxels project larger than the splats cover");

    // Zoomed physical lens (narrower than the splat field of view): nothing culled.
    camera = orbit(0.7F, 0.5F, 8.0F, centre);
    camera.physicalLens.enabled = true;
    camera.physicalLens.focalLengthMillimeters = 200.0F;
    require(draw_list(document, camera, viewport, true).size() == draw_list(document, camera, viewport, false).size(),
            "culled under a zoomed physical lens");
    std::printf("culling guards: OK\n");
}

// Culled voxels no longer consume the draw cap, so a second object behind a large
// solid one is drawn instead of being cut off.
void test_cap_no_longer_starved() {
    EditorDocument document("cap");
    document.add_object(make_object(1, 0, 40, [](int, int, int) { return MaterialId{1}; }));
    EditorObject second = make_object(2, 0, 6, [](int, int, int) { return MaterialId{2}; });
    second.transform.position = {5.0F, 0.0F, 0.0F};
    document.add_object(std::move(second));
    const EditorCamera camera = orbit(0.2F, 0.4F, 11.0F, {3.0F, 2.0F, 2.0F});
    const std::size_t cap = 30000;
    const auto full = draw_list(document, camera, {0, 0, 960, 600}, false, cap);
    const auto culled = draw_list(document, camera, {0, 0, 960, 600}, true, cap);
    const auto hasSecond = [](const std::vector<EditorVoxelDrawItem>& items) {
        return std::any_of(items.begin(), items.end(), [](const auto& item) { return item.objectId == 2; });
    };
    require(full.size() == cap && !hasSecond(full), "precondition: the full list should be cap-starved");
    require(culled.size() < cap && hasSecond(culled), "culling did not free the draw cap for the second object");
    std::printf("draw cap: OK\n");
}

// Toggling culling must rebuild a cached list even when nothing else changed.
void test_cache_key_includes_culling() {
    EditorDocument document("cache");
    document.add_object(make_object(1, 0, 32, [](int, int, int) { return MaterialId{1}; }));
    const EditorCamera camera = orbit(0.7F, 0.5F, 8.0F, {1.6F, 1.6F, 1.6F});
    const EditorMaterialLibrary materials;
    const UiRect viewport{0, 0, 960, 600};
    const std::set<EditorObjectId> selection;
    const auto fingerprint = editor_scene_render_fingerprint(document);
    EditorVoxelDrawListCache cache;
    EditorViewportSettings settings;
    settings.cullEnclosedVoxels = true;
    const std::size_t culled = cache.get(document, materials, camera, viewport, settings, selection, fingerprint).size();
    settings.cullEnclosedVoxels = false;
    const std::size_t full = cache.get(document, materials, camera, viewport, settings, selection, fingerprint).size();
    require(cache.rebuild_count() == 2U, "toggling culling did not rebuild the cached draw list");
    require(full > culled, "cached list kept the culled contents after culling was turned off");
    (void)cache.get(document, materials, camera, viewport, settings, selection, fingerprint);
    require(cache.rebuild_count() == 2U, "unchanged settings rebuilt the cached draw list");
    std::printf("draw cache key: OK\n");
}

} // namespace

// ------------------------------------------------------- derived bounds cache

namespace {

bool same_bounds(const VoxelObject::DerivedBounds& a, const VoxelObject::DerivedBounds& b) {
    return a.valid == b.valid && a.minimum == b.minimum && a.maximum == b.maximum;
}

VoxelObject::DerivedBounds reference_extent(const VoxelObject& object) {
    VoxelObject::DerivedBounds result;
    Int3 minimum = kInt3Max, maximum = kInt3Min;
    for (const auto& entry : object.bricks()) {
        const Int3 origin = brick_origin(entry.first);
        minimum = min_components(minimum, origin);
        maximum = max_components(maximum, {origin.x + kBrickDim, origin.y + kBrickDim, origin.z + kBrickDim});
        result.valid = true;
    }
    result.minimum = minimum;
    result.maximum = maximum;
    return result;
}

VoxelObject::DerivedBounds reference_occupied(const VoxelObject& object) {
    VoxelObject::DerivedBounds result;
    Int3 minimum = kInt3Max, maximum = kInt3Min;
    for (const auto& entry : object.bricks()) {
        Int3 brickMinimum, brickMaximum;
        if (!entry.second.occupancy().bounds(brickMinimum, brickMaximum)) continue;
        minimum = min_components(minimum, global_from_local(entry.first, brickMinimum));
        maximum = max_components(maximum, global_from_local(entry.first, brickMaximum));
        result.valid = true;
    }
    result.minimum = minimum;
    result.maximum = maximum;
    return result;
}

std::uint64_t reference_count(const VoxelObject& object) {
    std::uint64_t total = 0;
    for (const auto& entry : object.bricks()) total += entry.second.occupied_count();
    return total;
}

void require_derived_matches(const VoxelObject& object, const std::string& what) {
    // Query twice: the second call takes the cached path and must agree.
    for (int pass = 0; pass < 2; ++pass) {
        require(same_bounds(object.brick_extent_bounds(), reference_extent(object)),
                what + ": extent bounds diverged from the brick walk");
        require(same_bounds(object.occupied_bounds(), reference_occupied(object)),
                what + ": occupied bounds diverged from the brick walk");
        require(object.occupied_voxel_count() == reference_count(object),
                what + ": occupied count diverged from the brick walk");
    }
}

} // namespace

void test_derived_bounds_cache() {
    VoxelObject empty(10);
    require(!empty.brick_extent_bounds().valid, "empty object reports extent bounds");
    require(!empty.occupied_bounds().valid, "empty object reports occupied bounds");
    require(empty.occupied_voxel_count() == 0, "empty object reports voxels");

    VoxelObject object(11);
    std::mt19937 rng(4242);
    std::uniform_int_distribution<int> coord(-20, 20);
    std::uniform_int_distribution<int> material(1, 5);
    for (int i = 0; i < 400; ++i)
        (void)object.set_voxel({coord(rng), coord(rng), coord(rng)},
                               static_cast<MaterialId>(material(rng)));
    object.fill_brick({-3, 2, 0}, 2);
    object.fill_brick({2, -3, 1}, 3);
    // An emptied brick stays in the map: it counts toward extent, not occupancy.
    (void)object.set_voxel({100, 100, 100}, 1);
    (void)object.set_voxel({100, 100, 100}, kAirMaterial);
    require_derived_matches(object, "random fixture");

    // Edits after the cache is warm must be observed.
    (void)object.set_voxel({-80, 0, 0}, 1);
    require_derived_matches(object, "after growing set_voxel");
    (void)object.set_voxel({-80, 0, 0}, kAirMaterial);
    require_derived_matches(object, "after shrinking set_voxel");
    (void)object.find_brick(brick_key_from_voxel({0, 0, 0}));
    require_derived_matches(object, "after mutable find_brick");

    // Moves carry the content; the source is left empty.
    const auto extentBefore = object.brick_extent_bounds();
    const auto occupiedBefore = object.occupied_bounds();
    const auto countBefore = object.occupied_voxel_count();
    VoxelObject moved(std::move(object));
    require(same_bounds(moved.brick_extent_bounds(), extentBefore), "move ctor lost extent bounds");
    require(same_bounds(moved.occupied_bounds(), occupiedBefore), "move ctor lost occupied bounds");
    require(moved.occupied_voxel_count() == countBefore, "move ctor lost occupied count");
    require(!object.brick_extent_bounds().valid, "moved-from object still reports extent bounds");
    require(object.occupied_voxel_count() == 0, "moved-from object still reports voxels");

    VoxelObject assigned(12);
    assigned = std::move(moved);
    require(same_bounds(assigned.brick_extent_bounds(), extentBefore), "move assign lost extent bounds");
    require(same_bounds(assigned.occupied_bounds(), occupiedBefore), "move assign lost occupied bounds");
    require(assigned.occupied_voxel_count() == countBefore, "move assign lost occupied count");
    require_derived_matches(assigned, "after move assignment");
    std::printf("derived bounds cache: OK\n");
}

// Const queries on one object from several threads at once: the first query after an edit
// fills the cache, and concurrent readers must neither race on it nor see a half-written
// box. (Run under ThreadSanitizer this reported a race before the cache took a lock.)
void test_derived_cache_concurrent_reads() {
    VoxelObject object(13);
    std::mt19937 rng(77);
    std::uniform_int_distribution<int> coord(-40, 40);
    for (int i = 0; i < 2000; ++i) (void)object.set_voxel({coord(rng), coord(rng), coord(rng)}, 1);
    for (int round = 0; round < 20; ++round) {
        (void)object.set_voxel({round, 0, 0}, 2);  // edit: the next query refreshes the cache
        const auto roundExtent = reference_extent(object);
        const auto roundOccupied = reference_occupied(object);
        const auto roundCount = reference_count(object);
        std::atomic<int> mismatches{0};
        std::vector<std::thread> readers;
        for (int t = 0; t < 4; ++t) {
            readers.emplace_back([&] {
                for (int i = 0; i < 200; ++i) {
                    if (!same_bounds(object.brick_extent_bounds(), roundExtent) ||
                        !same_bounds(object.occupied_bounds(), roundOccupied) ||
                        object.occupied_voxel_count() != roundCount)
                        mismatches.fetch_add(1);
                }
            });
        }
        for (std::thread& reader : readers) reader.join();
        require(mismatches.load() == 0, "concurrent readers saw a wrong derived value");
    }
    std::printf("derived cache concurrent reads: OK\n");
}

void test_draw_sort_matches_reference() {
    const auto referenceLess = [](const EditorVoxelDrawItem& a, const EditorVoxelDrawItem& b) {
        if (a.depth != b.depth) return a.depth > b.depth;
        if (a.objectId != b.objectId) return a.objectId < b.objectId;
        return a.voxel < b.voxel;
    };
    const auto sameItem = [](const EditorVoxelDrawItem& a, const EditorVoxelDrawItem& b) {
        return a.objectId == b.objectId && a.voxel == b.voxel && a.material == b.material &&
               std::bit_cast<std::uint32_t>(a.depth) == std::bit_cast<std::uint32_t>(b.depth) &&
               std::bit_cast<std::uint32_t>(a.screenX) == std::bit_cast<std::uint32_t>(b.screenX) &&
               std::bit_cast<std::uint32_t>(a.screenY) == std::bit_cast<std::uint32_t>(b.screenY) &&
               std::bit_cast<std::uint32_t>(a.pixelRadius) == std::bit_cast<std::uint32_t>(b.pixelRadius) &&
               a.selected == b.selected && a.anchored == b.anchored;
    };
    const auto check = [&](EditorDocument& document, const EditorCamera& camera, bool cull,
                           const std::string& what, bool requireRadixPath) {
        const auto items = draw_list(document, camera, UiRect{0, 0, 960, 600}, cull);
        if (requireRadixPath)
            require(items.size() > 8192U, what + ": scene too small to exercise the large-list sort");
        auto reference = items;
        std::stable_sort(reference.begin(), reference.end(), referenceLess);
        require(items.size() == reference.size(), what + ": size changed");
        for (std::size_t i = 0; i < items.size(); ++i)
            require(sameItem(items[i], reference[i]),
                    what + ": item " + std::to_string(i) + " of " + std::to_string(items.size()) +
                        " differs from the reference order");
    };

    // Adversarial ties: an orthographic camera looking straight down -Z gives
    // every voxel in a z layer the exact same depth, so each layer is one
    // giant equal-depth run decided entirely by the tie-break keys.
    {
        EditorDocument document("slab");
        document.add_object(make_object(1, -40, 40, [](int, int, int z) {
            return z >= 0 && z < 4 ? MaterialId{1} : kAirMaterial;
        }));
        EditorObject second = make_object(2, -40, 40, [](int, int, int z) {
            return z >= 0 && z < 4 ? MaterialId{2} : kAirMaterial;
        });
        second.transform.position = {3.0F, 0.0F, 0.0F};
        document.add_object(std::move(second));
        EditorCamera camera;
        camera.position = {0.0F, 0.0F, 10.0F};
        camera.target = {0.0F, 0.0F, 0.0F};
        camera.projection = EditorProjection::Orthographic;
        check(document, camera, false, "ortho slab unculled", true);
        check(document, camera, true, "ortho slab culled", false);
    }

    // Realistic depth distribution: a solid sphere in perspective, both modes.
    {
        EditorDocument document("sphere");
        document.add_object(make_object(1, -28, 28, [](int x, int y, int z) {
            return x * x + y * y + z * z <= 784 ? MaterialId{3} : kAirMaterial;
        }));
        const EditorCamera camera = orbit(0.7F, 0.5F, 8.0F, {0, 0, 0});
        check(document, camera, false, "perspective sphere unculled", true);
        check(document, camera, true, "perspective sphere culled", true);
    }
    std::printf("draw sort reference equality: OK\n");
}

void test_world_bounds_use_tight_box() {    EditorObject object(21, "bounds");
    object.voxels = std::make_unique<VoxelObject>(21);
    object.voxelSizeMeters = 0.5F;
    (void)object.voxels->set_voxel({3, -2, 5}, 1);
    (void)object.voxels->set_voxel({10, 4, -7}, 1);
    (void)object.voxels->set_voxel({-100, -100, -100}, 1);
    (void)object.voxels->set_voxel({-100, -100, -100}, kAirMaterial);
    const EditorObjectBounds bounds = object_world_bounds(object);
    require(bounds.valid, "world bounds invalid for occupied object");
    require(bounds.minimum.x == 3 * 0.5F && bounds.minimum.y == -2 * 0.5F && bounds.minimum.z == -7 * 0.5F,
            "world bounds minimum is not the tight occupied box");
    require(bounds.maximum.x == 11 * 0.5F && bounds.maximum.y == 5 * 0.5F && bounds.maximum.z == 6 * 0.5F,
            "world bounds maximum is not the tight occupied box");
    std::printf("world bounds tight box: OK\n");
}

int main() {
    try {
        test_voxel_revisions();
        test_derived_bounds_cache();
        test_derived_cache_concurrent_reads();
        test_world_bounds_use_tight_box();
        test_draw_sort_matches_reference();
        test_scene_fingerprint();
        test_exposed_set_matches_reference();
        test_culling_is_pixel_identical();
        test_culling_guards();
        test_cap_no_longer_starved();
        test_cache_key_includes_culling();
        std::printf("editor draw culling tests passed\n");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
