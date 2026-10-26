// R0/R1 opt-in headless probe. Run with a stable machine and compare to an
// earlier baseline; numbers here are measurements, not claimed speedups.
#include "dve/region_catalog.hpp"
#include "dve/voxel_object.hpp"
#include "dve/gpu_brick_mirror.hpp"
#include "dve/packed_brickmap.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using namespace dve;

double milliseconds(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

void bench_catalog(RegionIndexBackend backend, std::size_t objects) {
    RegionCatalog catalog({16, backend, 65536});
    std::mt19937 rng(0xb337beef);
    std::uniform_int_distribution<int> coord(-256, 256);
    auto t0 = Clock::now();
    for (std::uint64_t id = 1; id <= objects; ++id) {
        RegionKey key{1,0,coord(rng),coord(rng),coord(rng)};
        if (!catalog.set_object_regions(id, std::span<const RegionKey>(&key, 1)))
            throw std::runtime_error("invalid catalog workload");
    }
    auto t1 = Clock::now();
    std::uint64_t checksum = 0;
    for (std::size_t j=0; j<100000; ++j) {
        RegionKey key{1,0,coord(rng),coord(rng),coord(rng)};
        checksum += catalog.region_objects(key).size();
    }
    auto t2 = Clock::now();
    for (std::size_t j=0; j<120; ++j) {
        auto x = coord(rng), y = coord(rng), z = coord(rng);
        const auto scanned = catalog.query(1,0,{x,y,z},{x+12,y+12,z+12});
        if (!scanned) throw std::runtime_error("unexpected region scan budget");
        checksum += scanned->size();
    }
    auto t3 = Clock::now();
    std::cout << "{\"kind\":\"region_catalog\",\"backend\":\""
              << (backend==RegionIndexBackend::HashSorted ? "hash_sorted" : "std_map")
              << "\",\"objects\":" << objects
              << ",\"regions\":" << catalog.region_count()
              << ",\"build_ms\":" << milliseconds(t0,t1)
              << ",\"lookup_ms\":" << milliseconds(t1,t2)
              << ",\"scan_ms\":" << milliseconds(t2,t3)
              << ",\"checksum\":" << checksum << "}\n";
}

void bench_existing_bricks() {
    VoxelObject object(7);
    object.reserve_bricks(4096);
    const auto start = Clock::now();
    for (int z=0; z<16; ++z)
        for (int y=0; y<16; ++y)
            for (int x=0; x<16; ++x)
                object.fill_brick({x,y,z}, 1);
    const auto filled = Clock::now();
    const auto originalHash = object.state_hash();
    const auto hashDone = Clock::now();
    std::uint64_t checksum = 0;
    for (int i=0; i<100000; ++i) {
        const VoxelObject& constObject = object; // mutable find_brick changes revision!
        const Brick* brick = constObject.find_brick({i%16,(i/16)%16,(i/256)%16});
        if (brick) checksum += brick->generation();
    }
    const auto lookedUp = Clock::now();
    GpuBrickMirror gpu;
    gpu.rebuild(object);
    const auto mirrored = Clock::now();
    PackedBrickmapScene packed;
    packed.rebuild(object);
    const auto packedDone = Clock::now();
    BrickMutation mutation;
    mutation.removeMask.set(0);
    const AppliedBrickEdit edit = object.apply({3,3,3}, mutation);
    const auto gpuDelta = gpu.update(object, std::span<const AppliedBrickEdit>(&edit, 1));
    const auto packedDelta = packed.update(object, std::span<const AppliedBrickEdit>(&edit, 1));
    const bool gpuValid = gpu.validate_against(object);
    const bool packedValid = packed.validate_against(object);
    if (!gpuValid || !packedValid) throw std::runtime_error("DVE GPU mirror mismatch after incremental edit");
    std::cout << "{\"kind\":\"dve_bricks\",\"bricks\":" << object.brick_count()
              << ",\"fill_ms\":" << milliseconds(start,filled)
              << ",\"state_hash_ms\":" << milliseconds(filled,hashDone)
              << ",\"find_ms\":" << milliseconds(hashDone,lookedUp)
              << ",\"mirror_build_ms\":" << milliseconds(lookedUp,mirrored)
              << ",\"packed_build_ms\":" << milliseconds(mirrored,packedDone)
              << ",\"state_hash\":" << originalHash
              << ",\"readback_hash\":" << packed.readback_hash()
              << ",\"checksum\":" << checksum
              << ",\"gpu_delta_published\":" << gpuDelta.published
              << ",\"gpu_delta_bytes\":" << gpuDelta.uploadedBytes
              << ",\"packed_delta_published\":" << packedDelta.published
              << ",\"gpu_valid\":" << (gpuValid ? "true" : "false")
              << ",\"packed_valid\":" << (packedValid ? "true" : "false") << "}\n";
}
} // namespace

int main(int argc, char** argv) {
    std::size_t objects = 12000;
    for (int i = 1; i < argc; ++i)
        if (std::string_view(argv[i]) == "--large") objects = 50000;
    bench_existing_bricks();
    bench_catalog(dve::RegionIndexBackend::HashSorted, objects);
    bench_catalog(dve::RegionIndexBackend::OrderedMap, objects);
}