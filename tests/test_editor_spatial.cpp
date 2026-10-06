#include "dve/editor_diagnostics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <set>
#include <stdexcept>
#include <vector>

namespace {
using namespace dve;
using namespace dve::editor;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
bool equal(Float3 a, Float3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
constexpr std::array<Int3, 6> neighbors{{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}}};
Int3 plus(Int3 a, Int3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
std::set<Int3> occupied(const VoxelObject& object) {
    std::set<Int3> result;
    for (const auto& [key, brick] : object.bricks()) brick.occupancy().for_each_set([&](auto index) {
        result.insert(global_from_local(key, local_from_index_unchecked(index)));
    });
    return result;
}
bool reference_validate(const VoxelObject& object, const std::vector<VoxelBox>& boxes) {
    auto remaining = occupied(object);
    for (const auto& box : boxes) {
        if (box.min.x >= box.maxExclusive.x || box.min.y >= box.maxExclusive.y || box.min.z >= box.maxExclusive.z) return false;
        for (auto z = box.min.z; z < box.maxExclusive.z; ++z)
            for (auto y = box.min.y; y < box.maxExclusive.y; ++y)
                for (auto x = box.min.x; x < box.maxExclusive.x; ++x)
                    if (remaining.erase({x,y,z}) != 1) return false;
    }
    return remaining.empty();
}
void check_bounds(const Bitset512& bits) {
    Int3 expectedMin = kInt3Max, expectedMax = kInt3Min;
    bits.for_each_set([&](auto index) {
        const auto local = local_from_index_unchecked(index);
        expectedMin = min_components(expectedMin, local);
        expectedMax = max_components(expectedMax, local);
    });
    Int3 actualMin, actualMax;
    require(bits.bounds(actualMin, actualMax) == bits.any(), "bitset bounds empty status");
    require(actualMin == expectedMin && actualMax == expectedMax, "bitset bounds differ from voxel enumeration");
}
void test_bitset_bounds() {
    check_bounds({});
    check_bounds(Bitset512::full());
    for (std::uint16_t i = 0; i < kBrickVoxelCount; ++i) { Bitset512 bits; bits.set(i); check_bounds(bits); }
    std::mt19937_64 random(12345);
    for (int trial = 0; trial < 500; ++trial) {
        Bitset512 bits;
        for (auto& word : bits.words) word = random() & random();
        check_bounds(bits);
    }
}
void check_diagnostics(EditorObject& object) {
    const auto materialLibrary = EditorMaterialLibrary::make_default();
    const auto result = analyze_editor_object(object, materialLibrary);
    const auto all = occupied(*object.voxels);
    auto unvisited = all;
    std::size_t surface = 0, components = 0, anchoredComponents = 0;
    for (auto voxel : all) for (auto delta : neighbors) {
        if (!all.contains(plus(voxel, delta))) { ++surface; break; }
    }
    while (!unvisited.empty()) {
        ++components;
        bool anchored = object.flags.anchored;
        std::vector<Int3> pending{*unvisited.begin()};
        unvisited.erase(unvisited.begin());
        while (!pending.empty()) {
            const auto voxel = pending.back(); pending.pop_back();
            anchored = anchored || object.anchors.contains(voxel);
            for (auto delta : neighbors) {
                const auto adjacent = plus(voxel, delta);
                if (unvisited.erase(adjacent)) pending.push_back(adjacent);
            }
        }
        anchoredComponents += anchored;
    }
    require(result.occupiedVoxels == all.size() && result.surfaceVoxels == surface, "diagnostic voxel counts differ");
    require(result.connectedComponents == components && result.anchoredComponents == anchoredComponents &&
        result.detachedComponents == components - anchoredComponents, "diagnostic connectivity differs");
    require(result.collisionProxyValid && result.finite, "invalid diagnostic physics results");
    const auto boxes = build_object_box_proxy(*object.voxels);
    require(reference_validate(*object.voxels, boxes), "generated boxes differ from exact reference coverage");
    const auto bounds = object_world_bounds(object);
    require(bounds.valid == !all.empty(), "world bounds empty status");
    if (!all.empty()) {
        Int3 minimum = kInt3Max, maximum = kInt3Min;
        for (auto voxel : all) { minimum = min_components(minimum, voxel); maximum = max_components(maximum, voxel); }
        const Float3 localMin{minimum.x*object.voxelSizeMeters, minimum.y*object.voxelSizeMeters, minimum.z*object.voxelSizeMeters};
        const Float3 localMax{(maximum.x+1)*object.voxelSizeMeters, (maximum.y+1)*object.voxelSizeMeters, (maximum.z+1)*object.voxelSizeMeters};
        Float3 expectedMin{INFINITY,INFINITY,INFINITY}, expectedMax{-INFINITY,-INFINITY,-INFINITY};
        for (int corner = 0; corner < 8; ++corner) {
            const auto world = transform_point(object.transform, {corner&1 ? localMax.x : localMin.x,
                corner&2 ? localMax.y : localMin.y, corner&4 ? localMax.z : localMin.z});
            expectedMin = {std::min(expectedMin.x,world.x),std::min(expectedMin.y,world.y),std::min(expectedMin.z,world.z)};
            expectedMax = {std::max(expectedMax.x,world.x),std::max(expectedMax.y,world.y),std::max(expectedMax.z,world.z)};
        }
        require(equal(bounds.minimum, expectedMin) && equal(bounds.maximum, expectedMax), "transformed tight world bounds differ");
    }
}
void test_objects() {
    EditorObject empty;
    empty.voxels->fill_brick({0,0,0}, 1);
    BrickMutation remove; remove.removeMask = Bitset512::full();
    empty.voxels->apply({0,0,0}, remove);
    check_diagnostics(empty); // Empty brick headers are retained.
    std::mt19937 random(54321);
    for (int trial = 0; trial < 30; ++trial) {
        EditorObject object;
        object.transform.position = {3,-2,7};
        object.transform.rotation = normalize(Quaternion{0.2F,0.3F,0.1F,0.9F});
        object.flags.anchored = trial % 7 == 0;
        object.flags.collisionEnabled = trial % 5 != 0;
        for (int z = -9; z < 10; ++z) for (int y = -9; y < 10; ++y) for (int x = -9; x < 10; ++x) {
            const bool solid = trial == 0;
            const bool checkerboard = trial == 1 && ((x+y+z) & 1) == 0;
            if (solid || checkerboard || (trial > 1 && random() % 5 == 0)) {
                object.voxels->set_voxel({x,y,z}, static_cast<MaterialId>(1 + random()%3));
                if (random()%37 == 0) object.anchors.insert({x,y,z});
            }
        }
        check_diagnostics(object);
        auto boxes = build_object_box_proxy(*object.voxels);
        require(validate_box_proxy(*object.voxels, boxes), "valid boxes rejected");
        boxes.push_back(boxes.front());
        require(!validate_box_proxy(*object.voxels, boxes), "overlapping boxes accepted");
        boxes.pop_back(); boxes.pop_back();
        require(!validate_box_proxy(*object.voxels, boxes), "missing coverage accepted");
        for (int mutation = 0; mutation < 10; ++mutation) {
            boxes = build_object_box_proxy(*object.voxels);
            const auto i = random() % boxes.size();
            boxes[i].maxExclusive.x += static_cast<int>(random()%3) - 1;
            require(validate_box_proxy(*object.voxels, boxes) == reference_validate(*object.voxels, boxes), "mutated coverage differs from reference");
        }
    }
}
void test_box_boundaries() {
    VoxelObject object;
    for (int z = -1; z <= 1; ++z) for (int y = -1; y <= 1; ++y) for (int x = -9; x <= 9; ++x) object.set_voxel({x,y,z}, 1);
    require(validate_box_proxy(object, {{{-9,-1,-1},{10,2,2}}}), "box spanning negative brick boundaries rejected");
    require(!validate_box_proxy(object, {{{-10,-1,-1},{10,2,2}}}), "air voxel accepted");
    require(!validate_box_proxy(object, {{{0,0,0},{0,1,1}}}), "degenerate box accepted");
    VoxelObject extreme;
    const auto low = std::numeric_limits<std::int32_t>::min(), high = std::numeric_limits<std::int32_t>::max();
    extreme.set_voxel({low,0,0}, 1); extreme.set_voxel({high-1,0,0}, 1);
    require(validate_box_proxy(extreme, {{{low,0,0},{low+1,1,1}},{{high-1,0,0},{high,1,1}}}), "extreme coordinate clipping rejected");
}
}
int main() {
    try { test_bitset_bounds(); test_objects(); test_box_boundaries(); std::cout << "editor spatial tests passed\n"; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
