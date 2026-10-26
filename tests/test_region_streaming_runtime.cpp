#include "dve/region_streaming_runtime.hpp"
#include <stdexcept>
#include <vector>
using namespace dve;
static void require(bool condition) { if (!condition) throw std::runtime_error("R2 bridge failed"); }

int main() {
    RegionCatalog catalog;
    require(catalog.set_object_regions(101, std::vector<RegionKey>{{1,0,0,0,0}}));
    require(catalog.set_object_regions(102, std::vector<RegionKey>{{1,0,0,0,0}}));
    require(catalog.set_object_regions(103, std::vector<RegionKey>{{1,0,100,0,0}}));
    RuntimeSceneWorld world;
    world.store.list = {{{101,true},false},{{102,false},false},{{103,true},true}};
    RuntimeSceneStagingExecutor jobs;
    RegionStreamingRuntimeConfig config;
    config.planner = {1,3,100,2,2,1};
    config.maximumInFlight = 2;
    config.maximumPublicationsPerTick = 1;
    config.retryDelayTicks = 1;
    config.enableStaticUnloads = true;
    RegionStreamingRuntime bridge(catalog, world, jobs, 1, config);
    auto stats = bridge.tick({1,0,0,0,0}, 0);
    require(stats.submitted == 2 && stats.pending == 2);
    require(bridge.tick({1,0,0,0,0}, 1).submitted == 0);
    // A distant camera cancels staging; ready results are discarded, not published.
    stats = bridge.tick({1,0,99,0,0}, 2);
    require(stats.cancelled == 2 && jobs.tokens.at(101).cancellation_requested());
    jobs.complete(101);
    jobs.complete(102);
    stats = bridge.tick({1,0,99,0,0}, 3);
    require(stats.discarded == 2 && world.published == 0 && bridge.pending_count() == 0);
    // Return and stage again. Complete both, but allow only one publication/tick.
    stats = bridge.tick({1,0,0,0,0}, 4);
    require(stats.submitted == 2);
    jobs.complete(101);
    jobs.complete(102);
    stats = bridge.tick({1,0,0,0,0}, 5);
    require(stats.published == 1 && stats.pending == 1);
    stats = bridge.tick({1,0,0,0,0}, 6);
    require(stats.published == 1 && stats.pending == 0 && world.published == 2);
    // Dynamic object 102 is never eligible for automatic unload.
    bridge.allow_static_unload(101, true);
    stats = bridge.tick({1,0,99,0,0}, 8);
    require(stats.unloaded == 1 && world.unloaded == 1);
    require(world.store.find_object(102)->loaded());
    bridge.set_pinned(101,true);
    require(bridge.tick({1,0,0,0,0}, 10).submitted == 1);
    jobs.complete(101);
    require(bridge.tick({1,0,0,0,0}, 11).published == 1);
    require(bridge.tick({1,0,99,0,0}, 20).unloaded == 0);
}

