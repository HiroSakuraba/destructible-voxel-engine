#include "dve/region_catalog.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdlib>
#include <climits>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>
#include <vector>

// Never let Release / NDEBUG turn the checks into no-ops.
#undef assert
#define assert(expr) do { if (!(expr)) { \
    std::cerr << "FAILED " << __FILE__ << ":" << __LINE__ << " : " #expr "\n"; \
    std::abort(); \
} } while (false)

using namespace dve;

void run_backend(RegionIndexBackend backend) {
    RegionCatalog catalog({16, backend, 48});
    assert(catalog.region_of_brick(3, 1, {-1, -16, -17}) == (RegionKey{3, 1, -1, -1, -2}));
    assert(catalog.region_of_brick(3, 1, {0, 15, 16}) == (RegionKey{3, 1, 0, 0, 1}));
    assert(catalog.region_of_brick(3, 1, {INT32_MIN, INT32_MAX, 0}).x == -134217728);
    assert(catalog.region_of_brick(3, 1, {INT32_MIN, INT32_MAX, 0}).y == 134217727);
    assert(!catalog.keys_for_brick_bounds(3, 1, {{0,0,0},{INT32_MAX,INT32_MAX,INT32_MAX}}));
    assert(!catalog.keys_for_brick_bounds(3, 1, {{1,0,0},{0,1,1}}));
    auto keys = catalog.keys_for_brick_bounds(3, 1, {{-17,-1,-1},{16,0,0}});
    assert(keys && keys->size() == 16);
    assert(std::is_sorted(keys->begin(), keys->end()));

    // 90-degree rotation around Z plus translation. Both local corners must be
    // transformed before building a conservative world-space bounding box.
    RegionAffine3x4 transform{{0,-1,0,80, 1,0,0,-16, 0,0,1,0}};
    RegionWorldAabb local{{0,0,0},{16,8,8}};
    auto bounds = RegionCatalog::transformed_brick_bounds(local, transform, 1.0);
    assert(bounds && bounds->minimum == (BrickKey{9,-2,0}));
    // x' = 80 - localY is reflected: local y in [0,8) maps to (72,80], so the
    // included local y=0 face sits at world x=80 and belongs to brick 10.
    assert(bounds->maximum == (BrickKey{10,-1,0}));
    assert(!RegionCatalog::transformed_brick_bounds(local, transform, 0));
    assert(!RegionCatalog::transformed_brick_bounds({{0,0,0},{INFINITY,1,1}}, transform, 1));
    assert(!RegionCatalog::transformed_brick_bounds({{0,0,0},{1,1,1}},
                RegionAffine3x4{{1,0,0,1e100,0,1,0,0,0,0,1,0}}, 1));

    const RegionKey a{1,0,-1,0,0}, b{1,0,0,0,0}, c{1,0,1,2,3}, otherWorld{2,0,1,2,3};
    const std::array<RegionKey, 4> first{b,a,b,otherWorld};
    const std::array<RegionKey, 2> second{b,c};
    assert(!catalog.set_object_regions(0, first));
    assert(catalog.set_object_regions(100, first));
    assert(catalog.revision() == 1 && catalog.object_count() == 1 && catalog.region_count() == 3);
    assert(catalog.set_object_regions(100, first));
    assert(catalog.revision() == 1); // no-op cannot invalidate all read caches
    assert(catalog.set_object_regions(200, second));
    assert(catalog.region_objects(b) == (std::vector<std::uint64_t>{100, 200}));
    assert(catalog.object_regions(100).size() == 3);
    assert(catalog.query(1, 0, {-1,-1,-1}, {1,3,3})->size() == 3);
    assert(catalog.query(1, 0, {-1,-1,-1}, {1,3,3}, 2) == std::nullopt);
    assert(catalog.query(1, 0, {0,0,0}, {-1,0,0}) == std::nullopt);
    assert(catalog.set_object_regions(100, second));
    assert(catalog.region_objects(a).empty() && catalog.region_objects(otherWorld).empty());
    assert(catalog.region_objects(c) == (std::vector<std::uint64_t>{100, 200}));
    assert(catalog.erase_object(100));
    assert(!catalog.erase_object(100));
    assert(catalog.region_objects(c) == (std::vector<std::uint64_t>{200}));
    assert(catalog.object_count() == 1);
    assert(catalog.set_object_bounds(300, 5, 0, local, transform, 1.0));
    assert(catalog.object_regions(300) == (std::vector<RegionKey>{{5,0,0,-1,0}}));
}

// Regression (Codex P2 4381518130): reflected axes keep their inclusive upper endpoint.
void reflected_bounds_keep_inclusive_upper_endpoint(RegionIndexBackend backend) {
    // worldX = 128 - localX over local [0,8)^3, 1 m voxels (8 m bricks), default 16-brick regions.
    const RegionWorldAabb local{{0,0,0},{8,8,8}};
    const RegionAffine3x4 mirrorX{{-1,0,0,128, 0,1,0,0, 0,0,1,0}};
    auto bounds = RegionCatalog::transformed_brick_bounds(local, mirrorX, 1.0);
    // local x in [0,8) -> world x in (120,128]; x=128 is included and is in brick 16.
    assert(bounds && bounds->minimum == (BrickKey{15,0,0}));
    assert(bounds->maximum == (BrickKey{16,0,0}));

    RegionCatalog catalog({16, backend, 48});
    assert(catalog.set_object_bounds(7, 1, 0, local, mirrorX, 1.0));
    // Brick 16 is region x=1; the object must be registered in both regions 0 and 1.
    assert(catalog.object_regions(7) == (std::vector<RegionKey>{{1,0,0,0,0},{1,0,1,0,0}}));
    // query() takes inclusive *region* coordinates: region x=1 must find the object.
    auto hit = catalog.query(1, 0, {1,0,0}, {1,0,0});
    assert(hit && hit->size() == 1 && (*hit)[0].key == (RegionKey{1,0,1,0,0}) &&
           (*hit)[0].objectIds == (std::vector<std::uint64_t>{7}));

    // Every axis reflected, and the same for a reflection through a negative-coordinate boundary.
    const RegionAffine3x4 mirrorAll{{-1,0,0,0, 0,-1,0,0, 0,0,-1,0}};
    auto all = RegionCatalog::transformed_brick_bounds(local, mirrorAll, 1.0);
    // world (-8,0] on each axis: bricks -1 (exclusive low) .. 0 (inclusive 0).
    assert(all && all->minimum == (BrickKey{-1,-1,-1}) && all->maximum == (BrickKey{0,0,0}));

    // Unreflected axes remain half-open: [0,8) -> brick 0 only (no over-extension).
    const RegionAffine3x4 shift{{1,0,0,128, 0,1,0,0, 0,0,1,0}};
    auto plain = RegionCatalog::transformed_brick_bounds(local, shift, 1.0);
    assert(plain && plain->minimum == (BrickKey{16,0,0}) && plain->maximum == (BrickKey{16,0,0}));

    // Rotation about Z by 90 degrees: world x = 128 - localY (reflected), world y = localX (half-open).
    const RegionAffine3x4 rot{{0,-1,0,128, 1,0,0,0, 0,0,1,0}};
    auto rotated = RegionCatalog::transformed_brick_bounds(local, rot, 1.0);
    assert(rotated && rotated->maximum == (BrickKey{16,0,0}));

    // Mixed-sign combination never attains its supremum -> stays exclusive.
    const RegionAffine3x4 mixed{{1,-1,0,16, 0,1,0,0, 0,0,1,0}};
    auto mix = RegionCatalog::transformed_brick_bounds(local, mixed, 1.0);
    // x' = lx - ly + 16 in (9,24) -> bricks 1..2 (24 itself is not attained).
    assert(mix && mix->minimum.x == 1 && mix->maximum.x == 2);
}

void compare_backends() {
    RegionCatalog hash({8, RegionIndexBackend::HashSorted, 8192});
    RegionCatalog tree({8, RegionIndexBackend::OrderedMap, 8192});
    std::mt19937 engine(0x44b377);
    std::uniform_int_distribution<int> coord(-50, 50);
    for (std::uint64_t id = 1; id <= 1200; ++id) {
        std::array<RegionKey, 3> keys{};
        for (auto& key : keys) key = {static_cast<std::uint32_t>(id % 3),
                                        static_cast<std::uint8_t>(id % 2), coord(engine), coord(engine), coord(engine)};
        assert(hash.set_object_regions(id, keys));
        assert(tree.set_object_regions(id, keys));
        if (id % 7 == 0) {
            assert(hash.erase_object(id));
            assert(tree.erase_object(id));
        }
    }
    assert(hash.entries() == tree.entries());
    for (int i = 0; i < 200; ++i) {
        BrickKey a{coord(engine),coord(engine),coord(engine)};
        BrickKey b{a.x+20,a.y+20,a.z+20};
        assert(hash.query(1,1,a,b) == tree.query(1,1,a,b));
    }
}

int main() {
    run_backend(RegionIndexBackend::HashSorted);
    run_backend(RegionIndexBackend::OrderedMap);
    reflected_bounds_keep_inclusive_upper_endpoint(RegionIndexBackend::HashSorted);
    reflected_bounds_keep_inclusive_upper_endpoint(RegionIndexBackend::OrderedMap);
    compare_backends();
    std::cout << "region catalog tests passed\n";
}