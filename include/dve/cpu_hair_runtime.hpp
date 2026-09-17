#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "dve/cpu_hair.hpp"

namespace dve {

using CpuHairOwnerId = std::uint64_t;

struct CpuHairBindOptions {
    CpuHairInstanceDesc simulation{};
};

// Game-object-facing ownership adapter for CpuHairWorld. The solver remains a single batched
// world, while this layer maps stable engine object ids to hair instances and keeps the hot
// simulation and render views allocation-free after binding.
class CpuHairRuntime {
public:
    explicit CpuHairRuntime(std::size_t workerCount = JobSystem::default_worker_count());

    [[nodiscard]] bool bind(CpuHairOwnerId owner, HairAsset asset,
                            const RigidTransform& worldTransform,
                            CpuHairBindOptions options = {},
                            std::string* error = nullptr);
    [[nodiscard]] bool bind_asset(CpuHairOwnerId owner,
                                  const std::filesystem::path& path,
                                  const RigidTransform& worldTransform,
                                  CpuHairBindOptions options = {},
                                  std::string* error = nullptr);
    [[nodiscard]] bool unbind(CpuHairOwnerId owner) noexcept;
    [[nodiscard]] bool contains(CpuHairOwnerId owner) const noexcept;

    [[nodiscard]] bool set_running(CpuHairOwnerId owner, bool running) noexcept;
    [[nodiscard]] bool set_visible(CpuHairOwnerId owner, bool visible) noexcept;
    [[nodiscard]] bool set_root_transform(CpuHairOwnerId owner,
                                          const RigidTransform& transform,
                                          bool teleport = false) noexcept;
    [[nodiscard]] bool set_root_targets(CpuHairOwnerId owner,
                                        std::span<const HairRootTarget> targets,
                                        bool teleport = false) noexcept;
    [[nodiscard]] bool clear_root_targets(CpuHairOwnerId owner) noexcept;
    [[nodiscard]] bool set_wind(CpuHairOwnerId owner, Float3 windVelocity) noexcept;
    [[nodiscard]] bool set_gravity(CpuHairOwnerId owner, Float3 gravity) noexcept;
    [[nodiscard]] bool set_collision(CpuHairOwnerId owner, HairCollisionSet collision);
    [[nodiscard]] bool set_solver_settings(CpuHairOwnerId owner,
                                           CpuHairSolverSettings settings,
                                           std::string* error = nullptr);
    [[nodiscard]] bool wake(CpuHairOwnerId owner) noexcept;
    [[nodiscard]] bool reset(CpuHairOwnerId owner) noexcept;
    [[nodiscard]] bool apply_impulse(CpuHairOwnerId owner, Float3 impulse) noexcept;

    [[nodiscard]] CpuHairStepTelemetry tick(float deltaSeconds);
    [[nodiscard]] CpuHairView view(CpuHairOwnerId owner) const noexcept;
    [[nodiscard]] const HairAsset* asset(CpuHairOwnerId owner) const noexcept;
    [[nodiscard]] std::vector<CpuHairOwnerId> owners() const;
    [[nodiscard]] std::span<const CpuHairOwnerId> owner_span() const noexcept {
        return ownerIds_;
    }

    [[nodiscard]] CpuHairWorld& solver_world() noexcept { return world_; }
    [[nodiscard]] const CpuHairWorld& solver_world() const noexcept { return world_; }

private:
    [[nodiscard]] CpuHairId id_for(CpuHairOwnerId owner) const noexcept;

    CpuHairWorld world_;
    std::map<CpuHairOwnerId, CpuHairId> records_;
    std::vector<CpuHairOwnerId> ownerIds_;
};

} // namespace dve
