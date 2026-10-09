// GameWorld destruction benchmark: carve, find pieces, split, build bodies and step, end to
// end, for both damage paths. Every scene is a fixed list of hits, so the counts in the
// output are deterministic; only the timings vary between runs and machines.
//
//   dve_collapse_bench            full scenes
//   dve_collapse_bench --quick    smaller scenes for CI
//   dve_collapse_bench --out f    also write the JSON to file f
//
// "sync" uses GameWorld::damage_sphere, which finishes inside the call. "queued" uses
// GameWorld::queue_damage_sphere, which spreads the work over ticks with a logical unit
// budget (set_destruction_units_per_tick). For the queued path, ticksToDrain is the visible
// delay between the last hit and its commit.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "dve/game_world.hpp"

namespace {

using namespace dve;
using Clock = std::chrono::steady_clock;

constexpr float kStep = 1.0F / 60.0F;

struct Hit {
    int tick{};
    Float3 center{};
    float radius{};
};

struct Scene {
    std::string name;
    std::function<std::unique_ptr<VoxelObject>()> build;
    bool dynamic{};
    std::vector<Hit> hits;
    int settleTicks{};
};

struct Result {
    std::string scene;
    std::string mode;
    std::uint64_t voxelsAtStart{};
    std::size_t hits{};
    int ticks{};
    int ticksToDrain{};
    std::uint64_t damageEvents{};
    std::uint64_t fragments{};
    std::uint64_t removedVoxels{};
    std::uint64_t rejected{};
    std::size_t objectsAtEnd{};
    std::uint64_t voxelsAtEnd{};
    double totalMs{};
    double damageCallMaxMs{};
    double tickP50Ms{};
    double tickP95Ms{};
    double tickP99Ms{};
    double tickMaxMs{};
};

double milliseconds(Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(fraction * static_cast<double>(values.size() - 1U));
    return values[index];
}

std::unique_ptr<VoxelObject> box(std::uint64_t id, Int3 size, Int3 origin = {}) {
    auto voxels = std::make_unique<VoxelObject>(id);
    for (int x = 0; x < size.x; ++x)
        for (int y = 0; y < size.y; ++y)
            for (int z = 0; z < size.z; ++z) voxels->set_voxel({origin.x + x, origin.y + y, origin.z + z}, 1);
    return voxels;
}

std::vector<Scene> scenes(bool quick) {
    const int scale = quick ? 2 : 1;
    std::vector<Scene> list;

    {   // Many small hits on a wall that never split it: one body rebuild per hit.
        const int sx = 64 / scale, sy = 32 / scale, sz = 8;
        Scene scene{"pockmark", [=] { return box(1, {sx, sy, sz}); }, false, {}, 30};
        const int count = 60 / scale;
        for (int i = 0; i < count; ++i) {
            const float x = 3.0F + static_cast<float>((i * 7) % (sx - 6));
            const float y = 3.0F + static_cast<float>((i * 5) % (sy - 6));
            scene.hits.push_back({i, {x, y, 1.0F}, 1.5F});
        }
        list.push_back(std::move(scene));
    }
    {   // A long span cut in six places over 30 ticks: several splits, shared bricks.
        const int length = 128 / scale;
        Scene scene{"bridge", [=] { return box(2, {length, 4, 4}); }, false, {}, 120};
        for (int i = 0; i < 6; ++i) {
            const float x = static_cast<float>(length) * static_cast<float>(i + 1) / 7.0F;
            scene.hits.push_back({i * 5, {x, 2.0F, 2.0F}, 2.6F});
        }
        list.push_back(std::move(scene));
    }
    {   // A tower on a narrow neck. One hit cuts the neck; the top breaks off as one large
        // dynamic piece, falls onto the base and settles. The base stays the static primary
        // because it is the larger piece.
        const int width = 32 / scale, base = 48 / scale, top = 40 / scale, neck = 4;
        const int mid = width / 2;
        Scene scene{"tower",
                    [=] {
                        auto voxels = box(3, {width, base, width});
                        for (int x = mid - 2; x < mid + 2; ++x)
                            for (int y = base; y < base + neck; ++y)
                                for (int z = mid - 2; z < mid + 2; ++z) voxels->set_voxel({x, y, z}, 1);
                        for (int x = 0; x < width; ++x)
                            for (int y = base + neck; y < base + neck + top; ++y)
                                for (int z = 0; z < width; ++z) voxels->set_voxel({x, y, z}, 1);
                        return voxels;
                    },
                    false, {}, quick ? 120 : 300};
        scene.hits.push_back({0, {static_cast<float>(mid), static_cast<float>(base + 2), static_cast<float>(mid)}, 3.2F});
        list.push_back(std::move(scene));
    }
    {   // Twelve hits on one object in the same tick, five times.
        const int size = 64 / scale;
        Scene scene{"spray", [=] { return box(4, {size, size, 8}); }, false, {}, 30};
        for (int burst = 0; burst < 5; ++burst)
            for (int i = 0; i < 12; ++i) {
                const float x = 3.0F + static_cast<float>((i * 5 + burst * 3) % (size - 6));
                const float y = 3.0F + static_cast<float>((i * 11 + burst * 7) % (size - 6));
                scene.hits.push_back({burst * 10, {x, y, 4.0F}, 1.5F});
            }
        list.push_back(std::move(scene));
    }
    return list;
}

std::uint64_t total_voxels(const GameWorld& world) {
    std::uint64_t total = 0;
    for (const GameObjectId id : world.object_ids()) total += world.voxel_count(id).value_or(0);
    return total;
}

Result run(const Scene& scene, bool queued) {
    Result result;
    result.scene = scene.name;
    result.mode = queued ? "queued" : "sync";
    result.hits = scene.hits.size();

    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameObjectDesc desc;
    desc.name = scene.name;
    desc.dynamic = scene.dynamic;
    desc.voxelSizeMeters = 1.0F;
    desc.voxels = scene.build();
    const GameObjectId target = world.create_object(std::move(desc));
    if (target == kInvalidGameObjectId) {
        std::cerr << "could not create scene " << scene.name << "\n";
        return result;
    }
    result.voxelsAtStart = total_voxels(world);
    world.on_damage([&](const GameDamageEvent& event) {
        ++result.damageEvents;
        result.fragments += event.newFragmentIds.size();
        result.removedVoxels += event.removedVoxelCount;
    });

    std::vector<double> tickMs;
    const int lastHitTick = scene.hits.empty() ? 0 : scene.hits.back().tick;
    const int maximumTicks = lastHitTick + scene.settleTicks + 20000;
    std::size_t nextHit = 0;
    const auto started = Clock::now();
    int tick = 0;
    for (; tick < maximumTicks; ++tick) {
        const auto tickStarted = Clock::now();
        while (nextHit < scene.hits.size() && scene.hits[nextHit].tick == tick) {
            const Hit& hit = scene.hits[nextHit++];
            if (!world.has_object(target)) continue;
            if (queued) {
                (void)world.queue_damage_sphere(target, hit.center, hit.radius);
            } else {
                const auto callStarted = Clock::now();
                (void)world.damage_sphere(target, hit.center, hit.radius);
                result.damageCallMaxMs = std::max(result.damageCallMaxMs, milliseconds(Clock::now() - callStarted));
            }
        }
        world.tick(kStep);
        tickMs.push_back(milliseconds(Clock::now() - tickStarted));
        const bool hitsDone = nextHit == scene.hits.size();
        if (hitsDone && world.pending_destruction_count() == 0 && result.ticksToDrain == 0)
            result.ticksToDrain = tick - lastHitTick + 1;
        if (hitsDone && world.pending_destruction_count() == 0 && tick >= lastHitTick + scene.settleTicks) {
            ++tick;
            break;
        }
    }
    result.totalMs = milliseconds(Clock::now() - started);
    result.ticks = tick;
    result.rejected = world.rejected_destruction_requests();
    result.objectsAtEnd = world.object_count();
    result.voxelsAtEnd = total_voxels(world);
    result.tickP50Ms = percentile(tickMs, 0.50);
    result.tickP95Ms = percentile(tickMs, 0.95);
    result.tickP99Ms = percentile(tickMs, 0.99);
    result.tickMaxMs = tickMs.empty() ? 0.0 : *std::max_element(tickMs.begin(), tickMs.end());
    return result;
}

std::string json(const std::vector<Result>& results, bool quick) {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(3);
    out << "{\n  \"benchmark\": \"dve_collapse_bench\",\n  \"quick\": " << (quick ? "true" : "false")
        << ",\n  \"tickHz\": 60,\n  \"results\": [\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
        const Result& r = results[i];
        out << "    {\"scene\": \"" << r.scene << "\", \"mode\": \"" << r.mode << "\""
            << ", \"voxelsAtStart\": " << r.voxelsAtStart << ", \"hits\": " << r.hits
            << ", \"ticks\": " << r.ticks << ", \"ticksToDrain\": " << r.ticksToDrain
            << ", \"damageEvents\": " << r.damageEvents << ", \"fragments\": " << r.fragments
            << ", \"removedVoxels\": " << r.removedVoxels << ", \"rejected\": " << r.rejected
            << ", \"objectsAtEnd\": " << r.objectsAtEnd << ", \"voxelsAtEnd\": " << r.voxelsAtEnd
            << ", \"totalMs\": " << r.totalMs << ", \"damageCallMaxMs\": " << r.damageCallMaxMs
            << ", \"tickP50Ms\": " << r.tickP50Ms << ", \"tickP95Ms\": " << r.tickP95Ms
            << ", \"tickP99Ms\": " << r.tickP99Ms << ", \"tickMaxMs\": " << r.tickMaxMs << "}"
            << (i + 1 < results.size() ? ",\n" : "\n");
    }
    out << "  ]\n}\n";
    return out.str();
}

} // namespace

int main(int argc, char** argv) {
    bool quick = false;
    std::string outPath;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--quick") {
            quick = true;
        } else if (arg == "--out" && i + 1 < argc) {
            outPath = argv[++i];
        } else {
            std::cerr << "usage: dve_collapse_bench [--quick] [--out file.json]\n";
            return 2;
        }
    }
    std::vector<Result> results;
    for (const Scene& scene : scenes(quick)) {
        results.push_back(run(scene, false));
        results.push_back(run(scene, true));
    }
    const std::string text = json(results, quick);
    std::cout << text;
    if (!outPath.empty()) {
        std::ofstream file(outPath);
        file << text;
        if (!file) {
            std::cerr << "could not write " << outPath << "\n";
            return 1;
        }
    }
    return 0;
}
