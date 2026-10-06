// Damage radius clipping: rasterization is clipped to the object's voxel bounds
// and radii are globally capped, so a huge sphere costs O(object), not O(r^2).
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "dve/damage.hpp"
#include "dve/game_world.hpp"
#include "dve/rigid_body_adapter.hpp"
#include "dve/transform.hpp"

using namespace dve;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

namespace {

std::unique_ptr<VoxelObject> make_block(int size, Int3 origin = {}) {
    auto vox = std::make_unique<VoxelObject>(1);
    for (int x = 0; x < size; ++x)
        for (int y = 0; y < size; ++y)
            for (int z = 0; z < size; ++z)
                if (((x * 7 + y * 3 + z * 5) % 11) != 0) vox->set_voxel({origin.x + x, origin.y + y, origin.z + z}, 1);
    return vox;
}

std::size_t occupied(const VoxelObject& object, int size, Int3 origin) {
    std::size_t n = 0;
    for (int x = -2; x < size + 2; ++x)
        for (int y = -2; y < size + 2; ++y)
            for (int z = -2; z < size + 2; ++z)
                n += object.occupied_at({origin.x + x, origin.y + y, origin.z + z}) ? 1U : 0U;
    return n;
}

// Clipped rasterization must remove exactly what the unclipped reference removes.
void test_clip_matches_unclipped() {
    const Int3 origin{-5, 3, -12};
    const int size = 20;
    const std::vector<SphereDamageCommand> cases = {
        {{0.0F, 10.0F, -5.0F}, 4.5F, 1},   {{-5.0F, 3.0F, -12.0F}, 9.0F, 2},
        {{30.0F, 30.0F, 30.0F}, 3.0F, 3},  {{5.3F, 13.7F, -2.1F}, 40.0F, 4},
        {{14.0F, 22.0F, 7.9F}, 2.25F, 5},  {{-100.0F, 10.0F, 0.0F}, 97.0F, 6},
    };
    for (const auto& command : cases) {
        auto clipped = make_block(size, origin);
        auto reference = make_block(size, origin);
        const DamageApplyReport clippedReport = apply_damage_commands(*clipped, {command});
        // Reference: unclipped batches applied directly.
        std::uint64_t referenceRemoved = 0;
        for (const auto& [key, mutation] : build_damage_batches(std::vector<SphereDamageCommand>{command})) {
            const Brick* before = reference->find_brick(key);
            if (before == nullptr) continue;
            const auto oldCount = before->occupied_count();
            (void)reference->apply(key, mutation);
            const Brick* after = reference->find_brick(key);
            const auto newCount = after == nullptr ? 0 : after->occupied_count();
            referenceRemoved += oldCount - newCount;
        }
        CHECK(clippedReport.removedVoxelCount == referenceRemoved);
        CHECK(occupied(*clipped, size, origin) == occupied(*reference, size, origin));
    }
}

void test_empty_object_and_far_sphere() {
    VoxelObject empty(1);
    DamageBatchWorkspace workspace;
    const SphereDamageCommand command{{0, 0, 0}, 1.0e6F, 1};
    const auto view = apply_damage_commands(empty, std::span<const SphereDamageCommand>(&command, 1), workspace);
    CHECK(view.removedVoxelCount == 0U);
    auto block = make_block(8);
    const SphereDamageCommand far{{1.0e9F, 1.0e9F, 1.0e9F}, 5.0F, 1};
    CHECK(apply_damage_commands(*block, {far}).removedVoxelCount == 0U);
}

void test_global_radius_cap() {
    const auto q = quantize_damage_command({{0, 0, 0}, 1.0e20F, 1});
    CHECK(q.radius == kMaxDamageRadiusVoxels * kDamageSubvoxelScale);
    CHECK(quantize_damage_command({{0, 0, 0}, -3.0F, 1}).radius == 0);
}

double time_world_damage(int blockSize, float radiusMeters, std::uint64_t* removed) {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameObjectDesc desc;
    desc.name = "target";
    desc.voxelSizeMeters = 0.1F;
    desc.dynamic = false;
    desc.voxels = make_block(blockSize);
    desc.transform = make_rigid_transform({0, 0, 0}, {});
    std::string error;
    const auto id = world.create_object(std::move(desc), &error);
    const float c = 0.05F * static_cast<float>(blockSize);
    const auto t0 = std::chrono::steady_clock::now();
    const auto result = world.damage_sphere(id, {c, c, c}, radiusMeters);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    *removed = result.value_or(0);
    return ms;
}

// At 0.1 m voxels a 30 m sphere used to take ~80 s and 1e20 was only bounded by the
// 1000 m GameWorld cap (still minutes). Both must now be bounded by the object size.
void test_huge_radius_is_fast() {
    for (int blockSize : {4, 32}) {
        for (float radius : {30.0F, 1000.0F, 1.0e20F}) {
            std::uint64_t removed = 0;
            const double ms = time_world_damage(blockSize, radius, &removed);
            std::printf("  block %d^3 @0.1 m, radius %g m: removed %llu in %.3f ms\n", blockSize, radius,
                        static_cast<unsigned long long>(removed), ms);
            auto expected = make_block(blockSize);
            CHECK(removed == occupied(*expected, blockSize, {}));  // everything is inside the sphere
            CHECK(ms < 2000.0);
        }
    }
}

} // namespace

int main() {
    test_clip_matches_unclipped();
    test_empty_object_and_far_sphere();
    test_global_radius_cap();
    test_huge_radius_is_fast();
    if (g_failures == 0) std::printf("dve_damage_clip_tests: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
