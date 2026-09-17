#ifdef _WIN32

#include <algorithm>
#include <iostream>
#include <string>

#include "dve/gpu/d3d12_brickmap_backend.hpp"

int main() {
    using namespace dve;
    VoxelObject object(14002);
    object.fill_brick({0, 0, 0}, 1);
    object.fill_brick({1, 0, 0}, 2);
    (void)object.set_voxel({8, 0, 0}, kAirMaterial);
    (void)object.set_voxel({1, 1, 1}, 3);

    PackedBrickmapScene scene;
    scene.rebuild(object);
    std::string error;

    gpu::D3D12BrickmapBackend singleScene;
    if (!singleScene.initialize(&error)) { std::cerr << error << '\n'; return 1; }
    if (!singleScene.reserve_scene_capacity(
            scene.index_grid().size_bytes() * 2U,
            scene.records().size_bytes() * 2U,
            std::max<std::size_t>(scene.material_arena().size_bytes() * 2U, 4096U), &error)) {
        std::cerr << error << '\n'; return 2;
    }
    if (!singleScene.upload_scene(scene, &error)) { std::cerr << error << '\n'; return 3; }
    if (!singleScene.readback_matches(scene, &error)) { std::cerr << error << '\n'; return 4; }

    gpu::D3D12RuntimeBrickmapWorldConfig config;
    config.heaps.indexHeapBytes = 4U * 1024U * 1024U;
    config.heaps.recordHeapBytes = 8U * 1024U * 1024U;
    config.heaps.materialHeapBytes = 8U * 1024U * 1024U;
    config.uploadRingBytes = 4U * 1024U * 1024U;
    config.readbackBytes = 4U * 1024U * 1024U;
    gpu::D3D12RuntimeBrickmapWorld persistent(config);
    if (!persistent.initialize(&error)) { std::cerr << error << '\n'; return 5; }
    const RuntimeBrickmapHandle handle = persistent.create_object({object.id(), {}, &scene});
    if (handle == kInvalidRuntimeBrickmapHandle) {
        std::cerr << "persistent D3D12 object publication failed\n";
        return 6;
    }
    const auto hash = persistent.readback_hash(handle);
    if (!hash || *hash != scene.readback_hash()) {
        std::cerr << "persistent D3D12 readback hash mismatch\n";
        return 7;
    }

    const BrickApplyResult change = object.set_voxel({2, 2, 2}, 4);
    const AppliedBrickEdit edit{{0, 0, 0}, change.changedMask, change.generation};
    (void)scene.update(object, std::span<const AppliedBrickEdit>(&edit, 1U));
    if (!persistent.update_object(handle, {object.id(), {}, &scene})) {
        std::cerr << "persistent D3D12 dirty update failed\n";
        return 8;
    }
    const auto updatedHash = persistent.readback_hash(handle);
    if (!updatedHash || *updatedHash != scene.readback_hash()) {
        std::cerr << "persistent D3D12 dirty readback hash mismatch\n";
        return 9;
    }

    const auto& feature = persistent.feature_report();
    const auto stats = persistent.stats();
    std::wcout << L"D3D12 persistent brickmap publication passed on " << feature.adapterName
               << L"; uploaded bytes=" << stats.uploadedBytes
               << L", copy batches=" << stats.submittedCopyBatches << L"\n";
    return 0;
}

#endif
