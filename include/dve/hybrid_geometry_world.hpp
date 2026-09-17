#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>

#include "dve/geometry_build.hpp"
#include "dve/runtime_brickmap_world.hpp"
#include "dve/runtime_mesh_world.hpp"

namespace dve {

using RuntimeGeometryHandle = std::uint32_t;
inline constexpr RuntimeGeometryHandle kInvalidRuntimeGeometryHandle = 0xFFFFFFFFU;

struct RuntimeGeometryObjectInfo {
    std::uint64_t objectId{};
    GeometryKind kind{GeometryKind::Voxel};
    RigidTransform worldTransform{};
    RuntimeBrickmapHandle brickmapHandle{kInvalidRuntimeBrickmapHandle};
    RuntimeMeshHandle meshHandle{kInvalidRuntimeMeshHandle};
};

struct HybridGeometryWorldStats {
    std::size_t objects{};
    std::size_t voxelObjects{};
    std::size_t polygonObjects{};
    std::uint64_t rejectedByBuildProfile{};
};

class HybridGeometryWorld {
public:
    HybridGeometryWorld(IRuntimeBrickmapWorld* voxelWorld, IRuntimeMeshWorld* meshWorld) noexcept
        : voxelWorld_(voxelWorld), meshWorld_(meshWorld) {}

    [[nodiscard]] RuntimeGeometryHandle create_voxel_object(
        const RuntimeBrickmapCreateDesc& desc,
        std::string* error = nullptr);
    [[nodiscard]] RuntimeGeometryHandle create_polygon_object(
        const RuntimeMeshCreateDesc& desc,
        std::string* error = nullptr);
    bool update_transform(RuntimeGeometryHandle handle, const RigidTransform& transform,
                          std::string* error = nullptr);
    bool destroy_object(RuntimeGeometryHandle handle);

    [[nodiscard]] std::optional<RuntimeGeometryObjectInfo> object(
        RuntimeGeometryHandle handle) const noexcept;
    [[nodiscard]] HybridGeometryWorldStats stats() const noexcept;

private:
    struct Slot {
        bool alive{};
        RuntimeGeometryObjectInfo info{};
        const PackedBrickmapScene* voxelScene{};
        const CookedPolygonAsset* polygonAsset{};
    };

    [[nodiscard]] RuntimeGeometryHandle allocate_slot();
    void reject_profile(std::string* error, GeometryKind kind);

    IRuntimeBrickmapWorld* voxelWorld_{};
    IRuntimeMeshWorld* meshWorld_{};
    std::vector<Slot> slots_;
    std::vector<RuntimeGeometryHandle> freeHandles_;
    std::uint64_t rejectedByBuildProfile_{};
};

} // namespace dve
