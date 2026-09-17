#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "dve/polygon_asset.hpp"
#include "dve/transform.hpp"

namespace dve {

using RuntimeMeshHandle = std::uint32_t;
inline constexpr RuntimeMeshHandle kInvalidRuntimeMeshHandle = 0xFFFFFFFFU;

struct RuntimeMeshCreateDesc {
    std::uint64_t objectId{};
    RigidTransform worldTransform{};
    const CookedPolygonAsset* asset{};
};

using RuntimeMeshUpdateDesc = RuntimeMeshCreateDesc;

struct RuntimeMeshCounts {
    std::size_t objects{};
    std::size_t vertices{};
    std::size_t indices{};
    std::size_t submeshes{};
    std::size_t materials{};
};

class IRuntimeMeshWorld {
public:
    virtual ~IRuntimeMeshWorld() = default;
    [[nodiscard]] virtual RuntimeMeshHandle create_object(const RuntimeMeshCreateDesc& desc) = 0;
    virtual bool destroy_object(RuntimeMeshHandle handle) = 0;
    [[nodiscard]] virtual std::optional<std::uint64_t> readback_hash(RuntimeMeshHandle handle) const = 0;
    virtual bool update_object(RuntimeMeshHandle handle, const RuntimeMeshUpdateDesc& desc);
    [[nodiscard]] virtual bool supports_incremental_update() const noexcept { return false; }
    [[nodiscard]] virtual std::vector<RuntimeMeshHandle> create_objects(
        std::span<const RuntimeMeshCreateDesc> descs);
    virtual bool destroy_objects(std::span<const RuntimeMeshHandle> handles);
};

class ReferenceRuntimeMeshWorld final : public IRuntimeMeshWorld {
public:
    [[nodiscard]] RuntimeMeshHandle create_object(const RuntimeMeshCreateDesc& desc) override;
    bool destroy_object(RuntimeMeshHandle handle) override;
    [[nodiscard]] std::optional<std::uint64_t> readback_hash(RuntimeMeshHandle handle) const override;
    bool update_object(RuntimeMeshHandle handle, const RuntimeMeshUpdateDesc& desc) override;
    [[nodiscard]] bool supports_incremental_update() const noexcept override { return true; }

    [[nodiscard]] RuntimeMeshCounts counts() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> object_id(RuntimeMeshHandle handle) const noexcept;
    [[nodiscard]] std::optional<RigidTransform> world_transform(RuntimeMeshHandle handle) const noexcept;

private:
    struct Slot {
        bool alive{};
        std::uint64_t objectId{};
        RigidTransform worldTransform{};
        CookedPolygonAsset asset{};
    };
    std::vector<Slot> slots_;
    std::vector<RuntimeMeshHandle> freeHandles_;
};

} // namespace dve
