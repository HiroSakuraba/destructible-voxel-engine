#include "dve/cpu_hair_runtime.hpp"

#include <algorithm>
#include <utility>

namespace dve {

CpuHairRuntime::CpuHairRuntime(std::size_t workerCount) : world_(workerCount) {}

bool CpuHairRuntime::bind(CpuHairOwnerId owner, HairAsset asset,
                          const RigidTransform& worldTransform,
                          CpuHairBindOptions options, std::string* error) {
    if (owner == 0U) {
        if (error) *error = "CPU hair owner id must be nonzero";
        return false;
    }
    if (records_.contains(owner)) {
        if (error) *error = "CPU hair owner is already bound";
        return false;
    }
    options.simulation.rootTransform = worldTransform;
    const CpuHairId id = world_.create(std::move(asset), std::move(options.simulation), error);
    if (id == kInvalidCpuHairId) return false;
    records_.emplace(owner, id);
    ownerIds_.insert(std::lower_bound(ownerIds_.begin(), ownerIds_.end(), owner), owner);
    return true;
}

bool CpuHairRuntime::bind_asset(CpuHairOwnerId owner, const std::filesystem::path& path,
                                const RigidTransform& worldTransform,
                                CpuHairBindOptions options, std::string* error) {
    if (owner == 0U) {
        if (error) *error = "CPU hair owner id must be nonzero";
        return false;
    }
    if (records_.contains(owner)) {
        if (error) *error = "CPU hair owner is already bound";
        return false;
    }
    options.simulation.rootTransform = worldTransform;
    const CpuHairId id = world_.create_from_asset(path, std::move(options.simulation), error);
    if (id == kInvalidCpuHairId) return false;
    records_.emplace(owner, id);
    ownerIds_.insert(std::lower_bound(ownerIds_.begin(), ownerIds_.end(), owner), owner);
    return true;
}

bool CpuHairRuntime::unbind(CpuHairOwnerId owner) noexcept {
    const auto found = records_.find(owner);
    if (found == records_.end()) return false;
    const bool destroyed = world_.destroy(found->second);
    records_.erase(found);
    const auto ownerFound = std::lower_bound(ownerIds_.begin(), ownerIds_.end(), owner);
    if (ownerFound != ownerIds_.end() && *ownerFound == owner) ownerIds_.erase(ownerFound);
    return destroyed;
}

bool CpuHairRuntime::contains(CpuHairOwnerId owner) const noexcept {
    return records_.contains(owner);
}

CpuHairId CpuHairRuntime::id_for(CpuHairOwnerId owner) const noexcept {
    const auto found = records_.find(owner);
    return found == records_.end() ? kInvalidCpuHairId : found->second;
}

bool CpuHairRuntime::set_running(CpuHairOwnerId owner, bool running) noexcept {
    return world_.set_running(id_for(owner), running);
}

bool CpuHairRuntime::set_visible(CpuHairOwnerId owner, bool visible) noexcept {
    return world_.set_visible(id_for(owner), visible);
}

bool CpuHairRuntime::set_root_transform(CpuHairOwnerId owner,
                                        const RigidTransform& transform,
                                        bool teleport) noexcept {
    return world_.set_root_transform(id_for(owner), transform, teleport);
}

bool CpuHairRuntime::set_root_targets(CpuHairOwnerId owner,
                                      std::span<const HairRootTarget> targets,
                                      bool teleport) noexcept {
    return world_.set_root_targets(id_for(owner), targets, teleport);
}

bool CpuHairRuntime::clear_root_targets(CpuHairOwnerId owner) noexcept {
    return world_.clear_root_targets(id_for(owner));
}

bool CpuHairRuntime::set_wind(CpuHairOwnerId owner, Float3 windVelocity) noexcept {
    return world_.set_wind(id_for(owner), windVelocity);
}

bool CpuHairRuntime::set_gravity(CpuHairOwnerId owner, Float3 gravity) noexcept {
    return world_.set_gravity(id_for(owner), gravity);
}

bool CpuHairRuntime::set_collision(CpuHairOwnerId owner, HairCollisionSet collision) {
    return world_.set_collision(id_for(owner), std::move(collision));
}

bool CpuHairRuntime::set_solver_settings(CpuHairOwnerId owner,
                                         CpuHairSolverSettings settings,
                                         std::string* error) {
    return world_.set_solver_settings(id_for(owner), settings, error);
}

bool CpuHairRuntime::wake(CpuHairOwnerId owner) noexcept {
    return world_.wake(id_for(owner));
}

bool CpuHairRuntime::reset(CpuHairOwnerId owner) noexcept {
    return world_.reset(id_for(owner));
}

bool CpuHairRuntime::apply_impulse(CpuHairOwnerId owner, Float3 impulse) noexcept {
    return world_.apply_impulse(id_for(owner), impulse);
}

CpuHairStepTelemetry CpuHairRuntime::tick(float deltaSeconds) {
    return world_.step(deltaSeconds);
}

CpuHairView CpuHairRuntime::view(CpuHairOwnerId owner) const noexcept {
    return world_.view(id_for(owner));
}

const HairAsset* CpuHairRuntime::asset(CpuHairOwnerId owner) const noexcept {
    return world_.asset(id_for(owner));
}

std::vector<CpuHairOwnerId> CpuHairRuntime::owners() const {
    return ownerIds_;
}

} // namespace dve
