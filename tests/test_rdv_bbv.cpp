#include "dve/render/rdv_bbv.hpp"
#include "dve/rhi/null_device.hpp"
#ifdef DVE_RDV_HAS_VULKAN
#include "dve/rhi/vulkan_device.hpp"
#endif

#include <array>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace dve;
using namespace dve::render::rdv;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void test_clip_map_publication_and_rays() {
    VoxelObject object(1981);
    object.fill_brick({-1, 0, 0}, 2);
    object.fill_brick({0, 0, 0}, 3);
    object.fill_brick({2, 0, 0}, 4);
    (void)object.set_voxel({0, 0, 0}, kAirMaterial);
    (void)object.set_voxel({-8, 1, 1}, kAirMaterial);
    PackedBrickmapScene source;
    source.rebuild(object);

    BbvClipmap map({4, 2, 2});
    auto first = map.recenter({0, 0, 0}, source);
    check(first.fullRefresh && first.changedBricks == 16 && first.dirtyBricks == 16,
          "initial clipmap must upload all physical slots");
    check(map.minimum_key() == (BrickKey{-2, -1, -1}), "wrong centered minimum");
    check(map.occupied_at({-7, 1, 1}), "negative brick occupancy was lost");
    check(!map.occupied_at({-8, 1, 1}), "negative brick destroyed voxel still occupied");
    check(!map.occupied_at({0, 0, 0}), "destroyed voxel still occupied");
    check(map.occupied_at({1, 0, 0}), "occupied neighbor missing");
    check(!map.occupied_at({16, 1, 1}), "outside-clipmap voxel was visible");

    rhi::NullDevice device;
    BbvRhiMirror mirror(device);
    std::string error;
    check(mirror.publish(map, &error), error.c_str());
    check(map.dirty_count() == 0 && mirror.bytes_uploaded() == map.bytes(),
          "first GPU publication did not upload exactly one clipmap");
    check(mirror.readback_matches(map, &error), error.c_str());
    check(mirror.publish(map, &error), error.c_str());
    check(mirror.bytes_uploaded() == map.bytes(),
          "unchanged clipmap uploaded redundant bytes");

    const auto moved = map.recenter({1, 0, 0}, source);
    check(moved.recentered && !moved.fullRefresh && moved.examinedBricks == 4,
          "one-brick scroll did not reuse the 12 overlapping physical slots");
    check(map.toroidal_offset().x == 1, "toroidal scroll offset was not advanced");
    check(map.occupied_at({16, 1, 1}), "newly exposed positive brick missing");
    check(map.occupied_at({-7, 1, 1}), "recenter damaged retained negative brick");
    const auto beforeMove = mirror.bytes_uploaded();
    check(mirror.publish(map, &error), error.c_str());
    check(mirror.bytes_uploaded() - beforeMove == 4 * sizeof(BbvBrick),
          "one-brick scroll should upload one exposed slab");
    check(mirror.readback_matches(map, &error), error.c_str());

    const BrickKey changed{2, 0, 0};
    (void)object.set_voxel({16, 1, 1}, kAirMaterial);
    source.rebuild(object);  // RDV receives authoritative bricks AFTER source publication.
    const auto edits = map.refresh(std::span(&changed, 1), source);
    check(edits.examinedBricks == 1 && edits.changedBricks == 1,
          "destroyed voxel was not incrementally reconciled");
    check(!map.occupied_at({16, 1, 1}) && map.occupied_at({17, 1, 1}),
          "incremental destruction changed unrelated occupancy");
    const auto beforeEdit = mirror.bytes_uploaded();
    check(mirror.publish(map, &error), error.c_str());
    check(mirror.bytes_uploaded() - beforeEdit == sizeof(BbvBrick),
          "single dirty brick uploaded more than its own 64-byte record");
    check(mirror.readback_matches(map, &error), error.c_str());

    const BrickKey outOfView{99, 99, 99};
    check(map.refresh(std::span(&outOfView, 1), source).examinedBricks == 0,
          "out-of-view edit was published");
    check(map.dirty_count() == 0, "unrelated edit dirtied GPU slots");

    // Independent BBV reference vs existing authoritative packed HDDA. The
    // clipmap contains every occupied brick in this deliberately bounded scene.
    std::uint32_t seed = 0x4A29D1E3u;
    auto random = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return seed;
    };
    for (int i = 0; i < 1200; ++i) {
        const Float3 origin{
            -7.75F + float(random() % 2750U) * 0.01F,
            -5.75F + float(random() % 1100U) * 0.01F,
            -5.75F + float(random() % 1100U) * 0.01F
        };
        Float3 direction{
            float(int(random() % 2001U) - 1000),
            float(int(random() % 2001U) - 1000),
            float(int(random() % 2001U) - 1000)
        };
        if (length_squared(direction) < 1.0F) direction = {1, .2F, .3F};
        const auto expected = raycast_packed_brickmap_hdda(source, origin, direction, 75.0F);
        const auto actual = map.trace(origin, direction, 75.0F);
        check(expected.has_value() == actual.has_value(), "BBV hit/miss parity failed");
        if (expected && actual) {
            check(expected->voxel == actual->voxel, "BBV hit voxel parity failed");
            check(expected->normal == actual->normal, "BBV face normal parity failed");
            check(std::abs(expected->distance - actual->distance) < 4.0e-4F,
                  "BBV distance parity failed");
        }
    }
    check(!map.trace({0, 0, 0}, {0, 0, 0}, 20).has_value(),
          "zero ray direction should return a miss");
    check(!map.trace({0, 0, 0}, {1, 0, 0}, -1).has_value(),
          "negative ray budget should return a miss");

    const auto teleported = map.recenter({40, 0, 0}, source);
    check(teleported.examinedBricks == 16 &&
          !map.occupied_at({17, 1, 1}), "teleport did not clear stale occupancy");
    check(mirror.publish(map, &error), error.c_str());
    check(mirror.readback_matches(map, &error), error.c_str());
    mirror.reset();
    check(device.statistics().buffersCreated == device.statistics().buffersDestroyed,
          "GPU publication leaked an RHI buffer");
}

void test_validation() {
    try { BbvClipmap wrong({0, 4, 4}); (void)wrong; throw std::runtime_error("bad dimensions accepted"); }
    catch (const std::invalid_argument&) {}
    try { BbvClipmap wrong({65, 1, 1}); (void)wrong; throw std::runtime_error("oversized dimensions accepted"); }
    catch (const std::invalid_argument&) {}
    PackedBrickmapScene empty;
    BbvClipmap map({2, 2, 2});
    rhi::NullDevice device;
    BbvRhiMirror mirror(device);
    std::string error;
    check(!mirror.publish(map, &error), "uninitialized BBV was uploaded");
    const auto initial = map.recenter({-2, -1, -3}, empty);
    check(initial.fullRefresh && initial.dirtyBricks == 8,
          "empty source should still initialize GPU slots");
    check(mirror.publish(map, &error), error.c_str());
    check(mirror.readback_matches(map, &error), error.c_str());
    check(!map.trace({-40, -40, -40}, {1, 1, 1}, 100).has_value(),
          "empty clipmap yielded a hit");

    std::array<GpuTraceRay, 2> rays{};
    rays[0].origin = {-11, -11, -23};
    rays[0].direction = {1, 0, 0};
    rays[0].maxDistance = 40;
    rays[1].origin = {-12, -12, -24};
    rays[1].direction = {0, 1, 0};
    rays[1].maxDistance = 40;
    const std::byte dummy[]{std::byte{0x42}};
    std::vector<BbvGpuHit> hits;
    check(execute_bbv_gpu_trace(device, mirror, map, rays, dummy, hits, &error),
          error.c_str());
    check(hits.size() == rays.size() && device.statistics().dispatchesExecuted == 1U,
          "Null-RHI did not execute the RDV dispatch contract");
    check(mirror.readback_matches(map, &error), error.c_str());

#ifdef DVE_RDV_HAS_VULKAN
    // Opt-in real GPU parity: compile shader with -fvk-b-shift 0 0,
    // -fvk-t-shift 16 0, -fvk-u-shift 32 0 and set DVE_RDV_TRACE_SPV.
    if (const char* path = std::getenv("DVE_RDV_TRACE_SPV")) {
        std::ifstream stream(path, std::ios::binary);
        check(bool(stream), "RDV GPU shader file cannot be opened");
        const std::string binary((std::istreambuf_iterator<char>(stream)), {});
        check(!binary.empty() && binary.size() % 4U == 0U,
              "RDV GPU SPIR-V file is malformed");
        std::vector<std::byte> shader;
        shader.reserve(binary.size());
        for (unsigned char c : binary) shader.push_back(static_cast<std::byte>(c));

        VoxelObject geometry(444);
        geometry.fill_brick({0, 0, 0}, 7);
        PackedBrickmapScene packed;
        packed.rebuild(geometry);
        BbvClipmap gpuClipmap({2, 2, 2});
        (void)gpuClipmap.recenter({0, 0, 0}, packed);
        rhi::VulkanDevice gpu;
        check(gpu.status() == rhi::DeviceStatus::Ready,
              "DVE_RDV_TRACE_SPV was set but Vulkan device is unavailable");
        BbvRhiMirror gpuMirror(gpu);
        std::array<GpuTraceRay, 2> gpuRays{};
        gpuRays[0].origin = {-2.25F, 1.25F, 1.25F};
        gpuRays[0].direction = {1.0F, 0.0F, 0.0F};
        gpuRays[0].maxDistance = 30.0F;
        gpuRays[1].origin = {9.25F, 1.25F, 1.25F};
        gpuRays[1].direction = {-1.0F, 0.0F, 0.0F};
        gpuRays[1].maxDistance = 30.0F;
        check(execute_bbv_gpu_trace(gpu, gpuMirror, gpuClipmap, gpuRays, shader, hits, &error),
              error.c_str());
        for (std::size_t i = 0; i < gpuRays.size(); ++i) {
            const auto reference = gpuClipmap.trace(
                gpuRays[i].origin, gpuRays[i].direction, gpuRays[i].maxDistance);
            check(reference.has_value() && hits[i].hit == 1U,
                  "GPU BBV hit did not match CPU reference");
            check(reference->voxel == hits[i].voxel &&
                  reference->normal == hits[i].normal &&
                  std::abs(reference->distance - hits[i].distance) < 4.0e-4F,
                  "native GPU BBV hit payload mismatch");
        }
    }
#endif
}
} // namespace

int main() {
    try {
        test_clip_map_publication_and_rays();
        test_validation();
        std::cout << "dve_rdv_bbv_tests: PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "dve_rdv_bbv_tests: FAIL: " << e.what() << '\n';
        return 1;
    }
}
