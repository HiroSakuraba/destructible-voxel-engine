#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "dve/text3d.hpp"
#include "dve/transform.hpp"

namespace dve {

using RuntimeText3DHandle = std::uint32_t;
inline constexpr RuntimeText3DHandle kInvalidRuntimeText3DHandle = 0xFFFFFFFFU;

struct RuntimeText3DCreateDesc {
    std::uint64_t objectId{};
    RigidTransform worldTransform{};
    const CookedText3DAsset* asset{};
    bool visible{true};
    bool selected{};
    bool castShadows{true};
    bool receiveGlobalIllumination{true};
};
using RuntimeText3DUpdateDesc = RuntimeText3DCreateDesc;

struct RuntimeText3DCounts {
    std::size_t objects{};
    std::size_t glyphs{};
    std::size_t curves{};
    std::size_t sideTriangles{};
    std::size_t residentBytes{};
};

struct RuntimeText3DInstanceSnapshot {
    std::uint64_t objectId{};
    std::uint64_t assetId{};
    RigidTransform worldTransform{};
    bool visible{true};
    bool selected{};
    bool castShadows{true};
    bool receiveGlobalIllumination{true};
};

struct RuntimeText3DSceneSnapshot {
    std::vector<CookedText3DAsset> assets;
    std::vector<RuntimeText3DInstanceSnapshot> instances;
};

class IRuntimeText3DWorld {
public:
    virtual ~IRuntimeText3DWorld() = default;
    [[nodiscard]] virtual RuntimeText3DHandle create_object(const RuntimeText3DCreateDesc& desc) = 0;
    virtual bool destroy_object(RuntimeText3DHandle handle) = 0;
    virtual bool update_object(RuntimeText3DHandle handle, const RuntimeText3DUpdateDesc& desc) = 0;
    [[nodiscard]] virtual std::optional<std::uint64_t> readback_hash(RuntimeText3DHandle handle) const = 0;
    [[nodiscard]] virtual RuntimeText3DSceneSnapshot snapshot() const = 0;

    [[nodiscard]] std::vector<RuntimeText3DHandle> create_objects(
        std::span<const RuntimeText3DCreateDesc> descs);
    bool destroy_objects(std::span<const RuntimeText3DHandle> handles);
};

class ReferenceRuntimeText3DWorld final : public IRuntimeText3DWorld {
public:
    [[nodiscard]] RuntimeText3DHandle create_object(const RuntimeText3DCreateDesc& desc) override;
    bool destroy_object(RuntimeText3DHandle handle) override;
    bool update_object(RuntimeText3DHandle handle, const RuntimeText3DUpdateDesc& desc) override;
    [[nodiscard]] std::optional<std::uint64_t> readback_hash(RuntimeText3DHandle handle) const override;
    [[nodiscard]] RuntimeText3DSceneSnapshot snapshot() const override;

    [[nodiscard]] RuntimeText3DCounts counts() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> object_id(RuntimeText3DHandle handle) const noexcept;
    [[nodiscard]] std::optional<RigidTransform> world_transform(RuntimeText3DHandle handle) const noexcept;
    [[nodiscard]] const CookedText3DAsset* asset(RuntimeText3DHandle handle) const noexcept;

private:
    struct Slot {
        bool alive{};
        RuntimeText3DInstanceSnapshot instance{};
        CookedText3DAsset asset{};
    };
    std::vector<Slot> slots_;
    std::vector<RuntimeText3DHandle> freeHandles_;
};

} // namespace dve
