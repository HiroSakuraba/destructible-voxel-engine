#include "dve/region_streaming_runtime.hpp"
#include <stdexcept>
#include <future>
#include <vector>
using namespace dve;
static void require(bool condition) { if (!condition) throw std::runtime_error("R2 bridge failed"); }

// Regression (Codex P2 4381912405): objects in retry backoff must not use up the per-tick load
// slots. With loadsPerTick=1 the failed nearest object used to be the only candidate, starving
// a third object although maximumInFlight had free capacity.
static void retry_backoff_does_not_consume_load_slots() {
    RegionCatalog catalog;
    for (std::uint64_t id : {201, 202, 203})
        require(catalog.set_object_regions(id, std::vector<RegionKey>{{1,0,0,0,0}}));
    RuntimeSceneWorld world;
    world.store.list = {{{201,false},false},{{202,false},false},{{203,false},false}};
    RuntimeSceneStagingExecutor jobs;
    RegionStreamingRuntimeConfig config;
    config.planner = {1,3,100,1,1,1}; // one load per tick
    config.maximumInFlight = 3;
    config.maximumPublicationsPerTick = 3;
    config.retryDelayTicks = 60;
    RegionStreamingRuntime streamer(catalog, world, jobs, 1, config);
    const RegionKey camera{1,0,0,0,0};
    require(streamer.tick(camera, 0).submitted == 1 && jobs.queued.contains(201));
    jobs.complete(201, true); // 201 fails and enters 60-tick backoff
    auto stats = streamer.tick(camera, 1); // consumes the failure; 202 is the planned load
    require(stats.errors == 1 && stats.submitted == 1 && jobs.queued.contains(202));
    // Tick 2: 201 is the nearest/lowest-id candidate but backed off. 203 must take the slot.
    stats = streamer.tick(camera, 2);
    require(stats.submitted == 1);
    require(jobs.queued.contains(203));
    require(jobs.submissions == 3 && !jobs.queued.contains(201));
    // 201 stays blocked for the full delay and is retried afterwards.
    require(streamer.tick(camera, 30).submitted == 0 && jobs.submissions == 3);
    jobs.complete(202); jobs.complete(203);
    (void)streamer.tick(camera, 31);
    require(streamer.tick(camera, 62).submitted == 1 && jobs.submissions == 4);
}

// Regression (Codex P2 4381912415): after the scene unloads, delivered staging results (which carry
// retained-memory reservations) must be drained by tick(), not left in pending_ forever.
static void scene_unload_drains_staging_futures() {
    RegionCatalog catalog;
    require(catalog.set_object_regions(301, std::vector<RegionKey>{{1,0,0,0,0}}));
    require(catalog.set_object_regions(302, std::vector<RegionKey>{{1,0,0,0,0}}));
    RuntimeSceneWorld world;
    world.store.list = {{{301,false},false},{{302,false},false}};
    RuntimeSceneStagingExecutor jobs;
    RegionStreamingRuntimeConfig config;
    config.planner = {1,3,100,2,2,1};
    RegionStreamingRuntime streamer(catalog, world, jobs, 1, config);
    const RegionKey camera{1,0,0,0,0};
    require(streamer.tick(camera, 0).submitted == 2);
    jobs.complete(301); // delivered before the scene goes away
    world.present = false;
    auto stats = streamer.tick(camera, 1);
    require(stats.sceneMissing && stats.cancelled == 2 && stats.discarded == 1);
    require(jobs.tokens.at(302).cancellation_requested());
    require(streamer.pending_count() == 1);
    require(jobs.reservations.at(301).expired()); // reservation released, not held by pending_
    jobs.complete(302); // late completion is drained on a later tick too
    stats = streamer.tick(camera, 2);
    require(stats.sceneMissing && stats.discarded == 1 && stats.cancelled == 0);
    require(streamer.pending_count() == 0 && jobs.reservations.at(302).expired());
    require(world.published == 0);
}

int main() {
    retry_backoff_does_not_consume_load_slots();
    scene_unload_drains_staging_futures();
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
    auto wrong = std::async(std::launch::async, [&] { return bridge.tick({1,0,0,0,0}, 0); }).get();
    require(wrong.wrongThread && jobs.submissions == 0);
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

