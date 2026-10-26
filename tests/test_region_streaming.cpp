#include "dve/region_streaming.hpp"
#include <algorithm>
#include <stdexcept>
[[maybe_unused]] static void require(bool condition) { if (!condition) throw std::runtime_error("R2 planner contract failed"); }
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>
using namespace dve;

int main() {
    for (const auto backend : {RegionIndexBackend::HashSorted, RegionIndexBackend::OrderedMap}) {
        RegionCatalog catalog({16, backend, 65536});
        const RegionKey near{7, 0, 0, 0, 0};
        const RegionKey far{7, 0, 8, 0, 0};
        const RegionKey overlap{7, 0, 2, 0, 0};
        require(catalog.set_object_regions(10, std::vector<RegionKey>{near}));
        require(catalog.set_object_regions(11, std::vector<RegionKey>{far}));
        require(catalog.set_object_regions(12, std::vector<RegionKey>{near, overlap}));
        // Another world/LOD must not leak into this planner.
        require(catalog.set_object_regions(13, std::vector<RegionKey>{{8,0,0,0,0}}));
        require(catalog.set_object_regions(14, std::vector<RegionKey>{{7,1,0,0,0}}));
        require(catalog.set_object_regions(15, std::vector<RegionKey>{{7,0,1,0,0}}));
        RegionStreamingPlanner planner({1, 3, 1000, 2, 2, 5});
        require(planner.valid());
        std::vector<RegionStreamingObject> objects{
            {10, false, false, false, 0}, {11, true, false, true, 1},
            {12, false, false, false, 0}, {13, false, false, false, 0},
            {14, false, false, false, 0}, {15, false, false, false, 0},
            {99, true, false, true, 0}};
        const auto result = planner.plan(catalog, near, objects, 10);
        require(result.has_value());
        require((result->loadIds == std::vector<std::uint64_t>{10, 12}));
        require((result->unloadIds == std::vector<std::uint64_t>{11}));
        require((result->nearIds == std::vector<std::uint64_t>{10, 12, 15}));
        objects[0].inFlight = true;
        auto skip = planner.plan(catalog, near, objects, 10);
        require(skip && (skip->loadIds == std::vector<std::uint64_t>{12,15}));
        objects[0].inFlight = false;
        // Regression (4381912405): backed-off objects are removed before the per-tick limit, so the
        // next candidate fills the slot instead of the plan being truncated to a skipped object.
        objects[0].retryBackoff = true;
        auto backoff = planner.plan(catalog, near, objects, 10);
        require(backoff && (backoff->loadIds == std::vector<std::uint64_t>{12,15}));
        require((backoff->nearIds == std::vector<std::uint64_t>{10, 12, 15})); // still near: no cancel/unload churn
        objects[0].retryBackoff = false;
        // Other world/LOD objects must not be evicted even if loaded.
        objects[3].loaded = true; objects[3].mayUnload = true;
        objects[4].loaded = true; objects[4].mayUnload = true;
        require(planner.plan(catalog, near, objects, 10)->unloadIds == std::vector<std::uint64_t>{11});
        objects[1].pinned = true;
        require(planner.plan(catalog, near, objects, 10)->unloadIds.empty());
        objects[1].pinned = false;
        objects[1].loadedSinceTick = 8;
        require(planner.plan(catalog, near, objects, 10)->unloadIds.empty());
        objects[1].loadedSinceTick = 0;
        require(!planner.plan(catalog, near, objects, 3)->unloadIds.size());
        // Moving from far back into the retention shell must not unload.
        require(planner.plan(catalog, {7,0,5,0,0}, objects, 10)->unloadIds.empty());
        // Query failure must never look like no regions (and cause mass unload).
        RegionStreamingPlanner tiny({1, 3, 1, 2, 2, 0});
        require(!tiny.plan(catalog, near, objects, 10));
        require(!RegionStreamingPlanner({2,1,100}).valid());
        // Coordinates at int32 edges are clamped for queries, no overflow.
        require(planner.plan(catalog, {7,0,std::numeric_limits<std::int32_t>::max(),0,0}, objects, 10).has_value());
        auto m = RegionStreamingPlanner::camera_region(catalog, 7, 0, {-0.1,0,0}, 1.0);
        require(m && m->x == -1 && m->y == 0 && m->z == 0);
        require(!RegionStreamingPlanner::camera_region(catalog, 7, 0, {NAN,0,0}, 1.0));
        require(!RegionStreamingPlanner::camera_region(catalog, 7, 0, {0,0,0}, 0));
        require(!RegionStreamingPlanner::camera_region(catalog, 7, 0, {1e50,0,0}, 1));
        // Signed physical origin, exact region boundaries.
        require(RegionStreamingPlanner::camera_region(catalog, 7, 0, {128.,0.,0.}, 1)->x == 1);
    }
}

