#include "dve/render/rdv_bbv.hpp"
#include "dve/render/rdv_viewport.hpp"
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
#include <span>
#include <string>
#include <vector>

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

void test_viewport_frame_bridge() {
    VoxelObject object(2049);
    object.fill_brick({0,0,0}, 9);
    PackedBrickmapScene source;
    source.rebuild(object);
    rhi::NullDevice device;
    {
        BbvViewportBridge bridge(device, {4,2,2});
        dve::render::CameraFramePlan frame;
        dve::render::CameraViewportRenderPlan first;
        first.frame.viewport.id = 11;
        first.frame.pose.position = {1, 1, 1};
        first.history.resetOcclusion = true;
        dve::render::CameraViewportRenderPlan second = first;
        second.frame.viewport.id = 12;
        second.frame.pose.position = {9, 1, 1};
        frame.viewports = {first, second};
        std::string error;
        check(bridge.advance(frame, source, 1.0F, 1U, {}, false, {}, &error),
              error.c_str());
        check(bridge.size() == 2 && bridge.buffer(11) && bridge.buffer(12),
              "independent per-viewport GPU buffers were not published");
        check(bridge.clipmap(11)->occupied_at({0,0,0}) &&
              bridge.clipmap(12)->occupied_at({0,0,0}),
              "viewport bridge did not source authoritative bricks");
        check(bridge.reset_radiance_history(11), "new viewport should reset temporal history");
        const auto afterFirst = device.statistics().uploadedBytes;

        frame.viewports[0].history = {};
        frame.viewports[1].history = {};
        check(bridge.advance(frame, source, 1.0F, 1U, {}, false, {}, &error),
              error.c_str());
        check(device.statistics().uploadedBytes == afterFirst,
              "unchanged viewports uploaded redundant occupancy data");
        check(!bridge.reset_radiance_history(11), "stable camera reset radiance history");

        // Incomplete edit list must conservatively reconcile a new source revision.
        (void)object.set_voxel({0,0,0}, kAirMaterial);
        source.rebuild(object);
        check(bridge.advance(frame, source, 1.0F, 2U, {}, false, {}, &error),
              error.c_str());
        check(!bridge.clipmap(11)->occupied_at({0,0,0}) &&
              !bridge.clipmap(12)->occupied_at({0,0,0}),
              "revision without edit keys left stale voxel data");

        (void)object.set_voxel({1,1,1}, kAirMaterial);
        source.rebuild(object);
        const BrickKey key{0,0,0};
        const auto beforeEdit = device.statistics().uploadedBytes;
        check(bridge.advance(frame, source, 1.0F, 3U, std::span(&key,1), true, {}, &error),
              error.c_str());
        check(!bridge.clipmap(11)->occupied_at({1,1,1}),
              "complete dirty-key publication ignored destruction");
        check(device.statistics().uploadedBytes - beforeEdit == 2U*sizeof(BbvBrick),
              "complete dirty key should upload exactly one record per viewport");

        frame.viewports[0].history.resetTaa = true;
        const auto beforeCut = device.statistics().uploadedBytes;
        check(bridge.advance(frame, source, 1.0F, 3U, {}, false, {}, &error),
              error.c_str());
        check(bridge.reset_radiance_history(11) &&
              device.statistics().uploadedBytes == beforeCut,
              "camera cut should reset history without rebuilding occupancy");

        frame.viewports.pop_back();
        frame.viewports[0].history = {};
        check(bridge.advance(frame, source, 1.0F, 3U, {}, false, {}, &error),
              error.c_str());
        check(bridge.size() == 1 && !bridge.buffer(12),
              "retired viewport retained its GPU buffer");
        bridge.reset();
        check(bridge.size() == 0 && !bridge.buffer(11),
              "viewport bridge did not release its state");
    }
    check(device.statistics().buffersCreated == device.statistics().buffersDestroyed,
          "viewport bridge leaked GPU allocations");
}

#ifdef DVE_RDV_HAS_VULKAN
std::vector<std::byte> load_trace_shader(const char* path) {
    std::ifstream stream(path, std::ios::binary);
    check(bool(stream), "RDV GPU shader file cannot be opened");
    const std::string binary((std::istreambuf_iterator<char>(stream)), {});
    check(!binary.empty() && binary.size() % 4U == 0U, "RDV GPU SPIR-V file is malformed");
    std::vector<std::byte> shader;
    shader.reserve(binary.size());
    for (unsigned char c : binary) shader.push_back(static_cast<std::byte>(c));
    return shader;
}

// Compares every ray of one GPU dispatch against the CPU oracle: hit/miss, voxel, normal, distance.
// On failure the message names the case, ray and both payloads so a CI log is self-diagnosing.
std::size_t compare_gpu_to_cpu(const BbvClipmap& clipmap, std::span<const GpuTraceRay> rays,
                               const std::vector<BbvGpuHit>& hits, const char* label) {
    check(hits.size() == rays.size(), "native GPU BBV returned the wrong number of results");
    std::size_t hitCount = 0U;
    for (std::size_t i = 0; i < rays.size(); ++i) {
        const auto reference = clipmap.trace(rays[i].origin, rays[i].direction, rays[i].maxDistance);
        const bool gpuHit = hits[i].hit != 0U;
        bool ok = reference.has_value() == gpuHit;
        if (ok && reference) {
            ok = reference->voxel == hits[i].voxel && reference->normal == hits[i].normal &&
                 std::abs(reference->distance - hits[i].distance) < 1.0e-3F;
            ++hitCount;
        }
        if (!ok) {
            std::string message = std::string("native GPU BBV parity failed [") + label +
                "] ray " + std::to_string(i) + ": cpu=";
            if (reference) {
                message += "hit v(" + std::to_string(reference->voxel.x) + "," +
                    std::to_string(reference->voxel.y) + "," + std::to_string(reference->voxel.z) +
                    ") n(" + std::to_string(reference->normal.x) + "," +
                    std::to_string(reference->normal.y) + "," + std::to_string(reference->normal.z) +
                    ") d=" + std::to_string(reference->distance);
            } else {
                message += "miss";
            }
            message += " gpu=";
            if (gpuHit) {
                message += "hit v(" + std::to_string(hits[i].voxel.x) + "," +
                    std::to_string(hits[i].voxel.y) + "," + std::to_string(hits[i].voxel.z) +
                    ") n(" + std::to_string(hits[i].normal.x) + "," +
                    std::to_string(hits[i].normal.y) + "," + std::to_string(hits[i].normal.z) +
                    ") d=" + std::to_string(hits[i].distance);
            } else {
                message += "miss (steps=" + std::to_string(hits[i].voxelSteps) +
                    ", lookups=" + std::to_string(hits[i].brickLookups) + ")";
            }
            throw std::runtime_error(message);
        }
    }
    return hitCount;
}

// A larger sparse scene than the minimal two-ray check: bricks on both sides of the origin
// (negative brick keys), carved voxels, a non-cubic clipmap, scrolls that wrap the toroidal
// offset on every axis in both directions, an incremental edit, and a teleport. Every phase
// is checked against the CPU oracle with hundreds of rays that start inside and outside the
// volume, so a wrong physical slot, byte address or stale slab shows up as a mismatch.
void test_native_gpu_scene_parity(const std::vector<std::byte>& shader) {
    VoxelObject object(8128);
    std::uint32_t seed = 0x51F15EEDU;
    const auto next = [&] {
        seed = seed * 1664525U + 1013904223U;
        return seed >> 8U;
    };
    for (int z = -5; z <= 4; ++z)
        for (int y = -4; y <= 3; ++y)
            for (int x = -6; x <= 5; ++x)
                if (next() % 100U < 22U)
                    object.fill_brick({x, y, z}, static_cast<MaterialId>(1U + next() % 6U));
    for (int i = 0; i < 400; ++i)
        (void)object.set_voxel({int(next() % 96U) - 48, int(next() % 64U) - 32,
                                int(next() % 80U) - 40}, kAirMaterial);
    PackedBrickmapScene packed;
    packed.rebuild(object);

    rhi::VulkanDevice gpu;
    if (gpu.status() != rhi::DeviceStatus::Ready)
        throw std::runtime_error(std::string("Vulkan device initialization failed: ") +
                                 std::string(gpu.device_loss_reason()));
    BbvClipmap clipmap({6, 4, 5});
    BbvRhiMirror mirror(gpu);
    std::string error;
    std::vector<BbvGpuHit> hits;

    const auto make_rays = [&](std::size_t count) {
        std::vector<GpuTraceRay> rays(count);
        const Int3 d = clipmap.dimensions();
        const BrickKey m = clipmap.minimum_key();
        for (auto& ray : rays) {
            // Origins span the clipmap plus a margin so some rays enter from outside.
            ray.origin = {
                float(m.x * 8) - 12.0F + float(next() % 100000U) * 0.00001F * float(d.x * 8 + 24),
                float(m.y * 8) - 12.0F + float(next() % 100000U) * 0.00001F * float(d.y * 8 + 24),
                float(m.z * 8) - 12.0F + float(next() % 100000U) * 0.00001F * float(d.z * 8 + 24)};
            ray.direction = {float(int(next() % 2001U) - 1000),
                             float(int(next() % 2001U) - 1000),
                             float(int(next() % 2001U) - 1000)};
            if (length_squared(ray.direction) < 1.0F) ray.direction = {1.0F, 0.25F, -0.5F};
            ray.maxDistance = 90.0F;
        }
        // Axis-aligned rays exercise the zero-component branches on every axis.
        for (std::size_t i = 0; i < 6U && i < rays.size(); ++i) {
            rays[i].direction = {0.0F, 0.0F, 0.0F};
            (&rays[i].direction.x)[i / 2U] = (i % 2U) ? -1.0F : 1.0F;
        }
        return rays;
    };
    const auto run_phase = [&](const char* label, bool mayBeEmpty = false) {
        const auto rays = make_rays(600U);
        check(execute_bbv_gpu_trace(gpu, mirror, clipmap, rays, shader, hits, &error),
              error.c_str());
        const auto hitCount = compare_gpu_to_cpu(clipmap, rays, hits, label);
        // A suite where nothing is ever hit (or everything is) proves nothing about occupancy.
        check(mayBeEmpty ? hitCount == 0U : (hitCount > 60U && hitCount < 590U), ("native GPU BBV scene produced a degenerate hit ratio in " + std::string(label) + ": " + std::to_string(hitCount) + "/600").c_str());
    };

    (void)clipmap.recenter({0, 0, 0}, packed);
    run_phase("initial negative-coordinate window");
    check(mirror.readback_matches(clipmap, &error), error.c_str());

    const BrickKey centers[] = {{-1, 0, 0}, {-3, -1, 0}, {-3, -1, -2}, {2, 1, 1}, {3, 2, -1},
                                {-5, -3, 3}, {0, 0, 0}, {1, 0, 0}};
    for (std::size_t i = 0; i < std::size(centers); ++i) {
        (void)clipmap.recenter(centers[i], packed);
        const std::string label = "scroll " + std::to_string(i);
        run_phase(label.c_str());
    }
    check(mirror.readback_matches(clipmap, &error), error.c_str());

    // Incremental edit: carve one voxel in a retained brick; the GPU must see only that change.
    const BrickKey edited{0, 0, 0};
    object.fill_brick(edited, 5);
    (void)object.set_voxel({3, 3, 3}, kAirMaterial);
    packed.rebuild(object);
    (void)clipmap.refresh(std::span(&edited, 1), packed);
    run_phase("incremental edit");
    check(mirror.readback_matches(clipmap, &error), error.c_str());

    // Teleport far outside the scene: every slot is replaced and no stale occupancy may remain.
    (void)clipmap.recenter({-20, 9, 14}, packed);
    run_phase("teleport to empty space", true);
    check(mirror.readback_matches(clipmap, &error), error.c_str());
    // ...and back into the populated region.
    (void)clipmap.recenter({-4, -2, -3}, packed);
    run_phase("teleport back");
    check(mirror.readback_matches(clipmap, &error), error.c_str());
    mirror.reset();
}
#endif

// A clipmap that has been scrolled (including toward negative brick keys and past the
// toroidal wrap on each axis) must be indistinguishable from one centred there from scratch.
void test_scroll_matches_fresh_clipmap() {
    VoxelObject object(311);
    std::uint32_t seed = 0xD15EA5E5U;
    const auto next = [&] {
        seed = seed * 1664525U + 1013904223U;
        return seed >> 8U;
    };
    for (int z = -6; z <= 5; ++z)
        for (int y = -6; y <= 5; ++y)
            for (int x = -6; x <= 5; ++x)
                if (next() % 100U < 30U)
                    object.fill_brick({x, y, z}, static_cast<MaterialId>(1U + next() % 5U));
    for (int i = 0; i < 300; ++i)
        (void)object.set_voxel({int(next() % 96U) - 48, int(next() % 96U) - 48,
                                int(next() % 96U) - 48}, kAirMaterial);
    PackedBrickmapScene source;
    source.rebuild(object);

    rhi::NullDevice device;
    BbvClipmap scrolled({3, 4, 5});
    BbvRhiMirror mirror(device);
    std::string error;
    (void)scrolled.recenter({0, 0, 0}, source);
    check(mirror.publish(scrolled, &error), error.c_str());
    const BrickKey path[] = {{-1, 0, 0}, {-2, -1, 0}, {-2, -1, -1}, {-5, -2, -3}, {-4, -2, -3},
                             {1, 3, 2}, {0, 0, 0}, {7, -4, 1}, {-3, -3, -3}, {-3, -3, -2},
                             {-3, -4, -2}, {-4, -4, -2}, {2, 0, 1}};
    for (const BrickKey center : path) {
        (void)scrolled.recenter(center, source);
        check(mirror.publish(scrolled, &error), error.c_str());
        check(mirror.readback_matches(scrolled, &error), error.c_str());

        BbvClipmap fresh({3, 4, 5});
        (void)fresh.recenter(center, source);
        check(fresh.minimum_key() == scrolled.minimum_key(), "scrolled minimum key diverged");
        const BrickKey low = fresh.minimum_key();
        for (int z = 0; z < 5 * 8; ++z)
            for (int y = 0; y < 4 * 8; ++y)
                for (int x = 0; x < 3 * 8; ++x) {
                    const Int3 voxel{low.x * 8 + x, low.y * 8 + y, low.z * 8 + z};
                    check(fresh.occupied_at(voxel) == scrolled.occupied_at(voxel),
                          "scrolled clipmap disagrees with a freshly centred clipmap");
                }
        // Voxels one brick outside the window must be invisible on both.
        check(!scrolled.occupied_at({low.x * 8 - 1, low.y * 8, low.z * 8}) &&
              !scrolled.occupied_at({low.x * 8, low.y * 8 + 4 * 8, low.z * 8}),
              "scrolled clipmap exposes bricks outside its window");
    }
}

// Dirty slots are written as the fewest contiguous byte ranges, never one write per brick.
void test_dirty_ranges_are_coalesced() {
    VoxelObject object(77);
    for (int x = 0; x < 4; ++x) object.fill_brick({x, 0, 0}, 3);
    PackedBrickmapScene source;
    source.rebuild(object);
    rhi::NullDevice device;
    BbvRhiMirror mirror(device);
    BbvClipmap map({4, 2, 2});
    std::string error;
    (void)map.recenter({2, 1, 1}, source);   // window x:[0,3] y:[0,1] z:[0,1]
    check(mirror.publish(map, &error), error.c_str());
    check(mirror.upload_ranges() == 1U, "initial upload should be one range");

    // Four bricks that are adjacent in the physical buffer -> exactly one write.
    for (int x = 0; x < 4; ++x) (void)object.set_voxel({x * 8, 0, 0}, kAirMaterial);
    source.rebuild(object);
    std::vector<BrickKey> keys;
    for (int x = 0; x < 4; ++x) keys.push_back({x, 0, 0});
    check(map.refresh(keys, source).changedBricks == 4U, "adjacent edits not reconciled");
    const auto beforeRanges = mirror.upload_ranges();
    const auto beforeBytes = mirror.bytes_uploaded();
    check(mirror.publish(map, &error), error.c_str());
    check(mirror.upload_ranges() - beforeRanges == 1U &&
          mirror.bytes_uploaded() - beforeBytes == 4U * sizeof(BbvBrick),
          "four adjacent dirty bricks were not coalesced into one range");

    // Two separated bricks -> two writes of one record each; no gap bytes are re-sent.
    object.fill_brick({0, 0, 0}, 3);
    object.fill_brick({3, 0, 0}, 3);
    source.rebuild(object);
    const BrickKey separated[] = {{0, 0, 0}, {3, 0, 0}};
    check(map.refresh(separated, source).changedBricks == 2U, "separated edits not reconciled");
    const auto rangesBefore = mirror.upload_ranges();
    const auto bytesBefore = mirror.bytes_uploaded();
    check(mirror.publish(map, &error), error.c_str());
    check(mirror.upload_ranges() - rangesBefore == 2U &&
          mirror.bytes_uploaded() - bytesBefore == 2U * sizeof(BbvBrick),
          "separated dirty bricks must not upload the clean bricks between them");
    check(mirror.readback_matches(map, &error), error.c_str());
}

// A failed advance() must not let a later, "complete" key list hide edits that the failed
// call carried: the viewport that never saw that revision has to reconcile conservatively.
void test_bridge_failed_advance_keeps_edits() {
    VoxelObject object(901);
    object.fill_brick({0, 0, 0}, 4);
    object.fill_brick({1, 0, 0}, 5);
    PackedBrickmapScene source;
    source.rebuild(object);
    rhi::NullDevice device;
    BbvViewportBridge bridge(device, {4, 2, 2});
    dve::render::CameraFramePlan frame;
    dve::render::CameraViewportRenderPlan a;
    a.frame.viewport.id = 21;
    a.frame.pose.position = {1, 1, 1};
    dve::render::CameraViewportRenderPlan b = a;
    b.frame.viewport.id = 22;
    frame.viewports = {a, b};
    std::string error;
    check(bridge.advance(frame, source, 1.0F, 1U, {}, false, {}, &error), error.c_str());

    // Revision 2 destroys a voxel in brick {0,0,0}, but viewport 22 has an unusable camera.
    (void)object.set_voxel({1, 1, 1}, kAirMaterial);
    source.rebuild(object);
    const BrickKey first{0, 0, 0};
    auto broken = frame;
    broken.viewports[1].frame.pose.position = {3.0e30F, 0.0F, 0.0F};
    check(!bridge.advance(broken, source, 1.0F, 2U, std::span(&first, 1), true, {}, &error),
          "out-of-range camera should fail the advance");

    // Revision 3 edits a different brick and supplies only its own (complete-for-3) keys.
    (void)object.set_voxel({9, 1, 1}, kAirMaterial);
    source.rebuild(object);
    const BrickKey second{1, 0, 0};
    check(bridge.advance(frame, source, 1.0F, 3U, std::span(&second, 1), true, {}, &error),
          error.c_str());
    for (const camera::CameraViewportId id : {camera::CameraViewportId{21}, camera::CameraViewportId{22}}) {
        check(!bridge.clipmap(id)->occupied_at({1, 1, 1}),
              "an edit from a failed advance was lost by a later complete-key refresh");
        check(!bridge.clipmap(id)->occupied_at({9, 1, 1}), "latest edit missing");
        check(bridge.clipmap(id)->occupied_at({2, 2, 2}), "unrelated voxel lost");
    }
}

void test_bridge_viewport_limit() {
    VoxelObject object(902);
    object.fill_brick({0, 0, 0}, 4);
    PackedBrickmapScene source;
    source.rebuild(object);
    rhi::NullDevice device;
    BbvViewportBridge bridge(device, {2, 2, 2});
    dve::render::CameraFramePlan frame;
    std::string error;
    for (camera::CameraViewportId id = 1; id <= 16; ++id) {
        dve::render::CameraViewportRenderPlan view;
        view.frame.viewport.id = id;
        view.frame.pose.position = {1, 1, 1};
        frame.viewports.push_back(view);
    }
    check(bridge.advance(frame, source, 1.0F, 1U, {}, false, {}, &error), error.c_str());
    check(bridge.size() == 16U, "sixteen viewports must all be tracked");
    dve::render::CameraViewportRenderPlan extra;
    extra.frame.viewport.id = 17;
    extra.frame.pose.position = {1, 1, 1};
    frame.viewports.push_back(extra);
    check(!bridge.advance(frame, source, 1.0F, 1U, {}, false, {}, &error) && bridge.size() == 16U &&
          !bridge.buffer(17), "a seventeenth viewport must be rejected without disturbing the first sixteen");
    // Retiring down to a single view releases the other fifteen allocations.
    frame.viewports.resize(1);
    check(bridge.advance(frame, source, 1.0F, 1U, {}, false, {}, &error), error.c_str());
    check(bridge.size() == 1U && !bridge.buffer(2), "retired viewports kept their buffers");
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
    // Opt-in real GPU parity: compile shaders/rdv_bbv_trace.hlsl to SPIR-V with descriptor
    // bindings b0=0, t0/t1=16/17, u0=32 (see docs/RDV_BBV_FOUNDATION.md) and set
    // DVE_RDV_TRACE_SPV. DVE_REQUIRE_VULKAN=1 turns a missing shader/device into a failure.
    const char* path = std::getenv("DVE_RDV_TRACE_SPV");
    if (!path && std::getenv("DVE_REQUIRE_VULKAN") != nullptr)
        throw std::runtime_error("DVE_REQUIRE_VULKAN is set but DVE_RDV_TRACE_SPV names no compiled shader");
    if (path) {
        const std::vector<std::byte> shader = load_trace_shader(path);

        VoxelObject geometry(444);
        geometry.fill_brick({0, 0, 0}, 7);
        PackedBrickmapScene packed;
        packed.rebuild(geometry);
        BbvClipmap gpuClipmap({2, 2, 2});
        (void)gpuClipmap.recenter({0, 0, 0}, packed);
        rhi::VulkanDevice gpu;
        if (gpu.status() != rhi::DeviceStatus::Ready)
            throw std::runtime_error(std::string("Vulkan device initialization failed: ") +
                                     std::string(gpu.device_loss_reason()));
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
        // Repeat across a deterministic ray batch (including misses, points
        // inside solid voxels, outside-grid entry and asymmetric directions).
        std::uint32_t seed = 0xCB4D5A21U;
        auto next = [&] {
            seed = seed * 1664525U + 1013904223U;
            return seed;
        };
        std::vector<GpuTraceRay> batch(256U);
        for (auto& ray : batch) {
            ray.origin = {
                float(int(next() % 2048U) - 1024) / 64.0F + 0.125F,
                float(int(next() % 2048U) - 1024) / 64.0F + 0.125F,
                float(int(next() % 2048U) - 1024) / 64.0F + 0.125F};
            ray.direction = {
                float(int(next() % 2001U) - 1000),
                float(int(next() % 2001U) - 1000),
                float(int(next() % 2001U) - 1000)};
            if (length_squared(ray.direction) < 1.0F)
                ray.direction = {1.0F, 0.125F, -0.5F};
            ray.maxDistance = 48.0F;
        }
        check(execute_bbv_gpu_trace(gpu, gpuMirror, gpuClipmap, batch, shader, hits, &error),
              error.c_str());
        for (std::size_t i = 0; i < batch.size(); ++i) {
            const auto reference = gpuClipmap.trace(
                batch[i].origin, batch[i].direction, batch[i].maxDistance);
            check(reference.has_value() == (hits[i].hit != 0U),
                  "native GPU BBV randomized ray hit/miss parity failed");
            if (reference) {
                check(reference->voxel == hits[i].voxel &&
                      reference->normal == hits[i].normal &&
                      std::abs(reference->distance - hits[i].distance) < 1.0e-3F,
                      "native GPU BBV randomized ray payload parity failed");
            }
        }
        // After toroidal scroll, recheck that a retained solid maps to
        // the correct physical GPU slot and no previous occupied slot aliases it.
        (void)gpuClipmap.recenter({1, 0, 0}, packed);
        check(execute_bbv_gpu_trace(gpu, gpuMirror, gpuClipmap, gpuRays,
                                    shader, hits, &error), error.c_str());
        for (std::size_t i = 0; i < gpuRays.size(); ++i) {
            const auto reference = gpuClipmap.trace(
                gpuRays[i].origin, gpuRays[i].direction, gpuRays[i].maxDistance);
            check(reference.has_value() == (hits[i].hit != 0U),
                  "native GPU BBV toroidal scroll hit parity failed");
            if (reference)
                check(reference->voxel == hits[i].voxel,
                      "native GPU BBV toroidal scroll mapped the wrong occupied slot");
        }
        test_native_gpu_scene_parity(shader);
    }
#else
    if (std::getenv("DVE_REQUIRE_VULKAN") != nullptr)
        throw std::runtime_error("DVE_REQUIRE_VULKAN is set but the Vulkan RHI backend was not built");
#endif
}
} // namespace

int main() {
    try {
        test_clip_map_publication_and_rays();
        test_viewport_frame_bridge();
        test_bridge_failed_advance_keeps_edits();
        test_bridge_viewport_limit();
        test_scroll_matches_fresh_clipmap();
        test_dirty_ranges_are_coalesced();
        test_validation();
        std::cout << "dve_rdv_bbv_tests: PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "dve_rdv_bbv_tests: FAIL: " << e.what() << '\n';
        return 1;
    }
}
