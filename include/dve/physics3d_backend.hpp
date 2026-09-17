#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "dve/rigid_body_adapter.hpp"

namespace dve {

enum class Physics3DBackend : std::uint8_t {
    Automatic,
    Reference,
    Jolt,
    Box3D,
};


struct Physics3DWorldConfig {
    // Zero keeps the selected backend's own conservative default.
    std::uint32_t workerThreads{};
    std::uint32_t subStepCount{};
    std::uint32_t velocityIterations{};
    std::uint32_t positionIterations{};
    bool deterministicSimulation{true};
    bool allowSleeping{true};
    bool continuousCollision{true};
};

struct Physics3DBackendAvailability {
    bool reference{true};
    bool jolt{};
    bool box3d{};
};

[[nodiscard]] Physics3DBackendAvailability physics3d_backend_availability() noexcept;
[[nodiscard]] bool physics3d_backend_available(Physics3DBackend backend) noexcept;
[[nodiscard]] std::string_view physics3d_backend_label(Physics3DBackend backend) noexcept;

// Automatic preserves the established preference order: Jolt, then Box3D, then the
// deterministic reference adapter. Explicit unavailable selections fail instead of silently
// substituting another production solver.
[[nodiscard]] std::unique_ptr<IRigidBodyWorld> create_physics3d_world(
    Physics3DBackend backend,
    Physics3DBackend* resolvedBackend = nullptr,
    std::string* error = nullptr,
    Physics3DWorldConfig config = {});

} // namespace dve
