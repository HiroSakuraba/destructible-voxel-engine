// Warmed, single-threaded microbenchmarks; allocation counters include returned data.
#include "dve/editor_diagnostics.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
#include <random>
#include <vector>
namespace { std::atomic<std::size_t> allocations{}; }
void* operator new(std::size_t n) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
int main() {
    using namespace dve;
    using namespace dve::editor;
    const auto materials = EditorMaterialLibrary::make_default();
    for (const char* name : {"solid", "damaged", "sparse", "fragmented"}) {
        EditorObject object(1, name);
        const std::string shape(name);
        std::mt19937 random(12012);
        const int width = shape == "sparse" ? 8 : shape == "fragmented" ? 2 : 4;
        for (int z = 0; z < width; ++z) for (int y = 0; y < width; ++y) for (int x = 0; x < width; ++x) {
            const BrickKey key{x - width / 2, y - width / 2, z - width / 2};
            if (shape == "sparse") object.voxels->set_voxel(global_from_local(key, {0,0,0}), 1);
            else {
                object.voxels->fill_brick(key, 1);
                BrickMutation mutation;
                for (std::uint16_t i = 0; i < kBrickVoxelCount; ++i) {
                    const auto local = local_from_index_unchecked(i);
                    if ((shape == "fragmented" && ((local.x + local.y + local.z) & 1)) ||
                        (shape == "damaged" && random() % 4 == 0)) mutation.removeMask.set(i);
                }
                object.voxels->apply(key, mutation);
            }
        }
        object.anchors.insert({0,0,0});
        const auto diagnostic = analyze_editor_object(object, materials);
        std::cout << name << " voxels=" << diagnostic.occupiedVoxels << " surface=" << diagnostic.surfaceVoxels
                  << " components=" << diagnostic.connectedComponents << " anchored=" << diagnostic.anchoredComponents
                  << " boxes=" << diagnostic.collisionBoxes << " valid=" << diagnostic.collisionProxyValid << '\n';
        auto benchmark = [&](const char* operation, int iterations, auto function) {
            (void)function();
            std::vector<double> times;
            times.reserve(static_cast<std::size_t>(iterations));
            std::size_t calls = 0, checksum = 0;
            for (int i = 0; i < iterations; ++i) {
                const auto count = allocations.load(std::memory_order_relaxed);
                const auto start = std::chrono::steady_clock::now();
                checksum += function();
                const auto end = std::chrono::steady_clock::now();
                calls += allocations.load(std::memory_order_relaxed) - count;
                times.push_back(std::chrono::duration<double, std::micro>(end - start).count());
            }
            std::sort(times.begin(), times.end());
            std::cout << name << ' ' << operation << " allocations/call=" << static_cast<double>(calls) / iterations
                      << " median_us=" << times[times.size()/2] << " checksum=" << checksum << '\n';
        };
        const auto boxes = build_object_box_proxy(*object.voxels);
        benchmark("bounds", 100, [&] { return object_world_bounds(object).valid ? std::size_t(1) : 0; });
        benchmark("validate", 8, [&] { return validate_box_proxy(*object.voxels, boxes) ? std::size_t(1) : 0; });
        benchmark("diagnostics", 8, [&] { return analyze_editor_object(object, materials).surfaceVoxels; });
    }
}
