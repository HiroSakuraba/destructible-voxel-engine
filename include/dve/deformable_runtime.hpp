#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <vector>

#include "dve/bspline_cloth.hpp"
#include "dve/soft_body.hpp"

namespace dve {

using DeformableOwnerId = std::uint64_t;

enum class DeformablePrimitiveKind : std::uint8_t { Triangles, Lines };

struct DeformableVertex {
    Float3 position{};
    Float3 normal{};
};

struct DeformableBounds {
    Float3 minimum{};
    Float3 maximum{};
};

struct DeformableRenderPacket {
    DeformableOwnerId owner{};
    RuntimeSoftBodyId runtimeBody{};
    SoftBodyKind kind{SoftBodyKind::Cloth};
    DeformablePrimitiveKind primitive{DeformablePrimitiveKind::Triangles};
    std::vector<DeformableVertex> vertices;
    std::vector<std::uint32_t> indices;
    DeformableBounds bounds{};
    std::uint64_t simulationFrame{};
    std::uint64_t deformationHash{};
    bool smoothProxy{};
    bool doubleSided{true};
};

struct DeformableCollisionProxy {
    DeformableOwnerId owner{};
    std::vector<Float3> particles;
    std::vector<float> radii;
    std::vector<std::array<std::uint32_t, 2>> edges;
    std::vector<std::array<std::uint32_t, 3>> surfaceTriangles;
    DeformableBounds bounds{};
    std::uint64_t simulationFrame{};
};

struct DeformableBindOptions {
    RuntimeSoftBodyInstance simulation{};
    bool smoothCloth{};
    std::uint32_t clothColumns{};
    std::uint32_t clothRows{};
    std::uint32_t embeddedColumns{32U};
    std::uint32_t embeddedRows{32U};
    bool evaluateSmoothEnergy{};
};

struct DeformableTickTelemetry {
    SoftBodyStepTelemetry solver{};
    std::uint64_t renderPacketsBuilt{};
    std::uint64_t renderVerticesPublished{};
    std::uint64_t collisionParticlesPublished{};
    std::uint64_t failedProxyUpdates{};
};

class DeformableRuntime {
public:
    [[nodiscard]] bool bind(DeformableOwnerId owner, SoftBodyAsset asset,
                            const RigidTransform& worldTransform,
                            DeformableBindOptions options = {},
                            std::string* error = nullptr);
    [[nodiscard]] bool bind_asset(DeformableOwnerId owner,
                                  const std::filesystem::path& path,
                                  const RigidTransform& worldTransform,
                                  DeformableBindOptions options = {},
                                  std::string* error = nullptr);
    [[nodiscard]] bool unbind(DeformableOwnerId owner) noexcept;
    [[nodiscard]] bool contains(DeformableOwnerId owner) const noexcept;
    [[nodiscard]] bool set_running(DeformableOwnerId owner, bool running) noexcept;
    [[nodiscard]] bool reset(DeformableOwnerId owner) noexcept;
    [[nodiscard]] bool apply_impulse(DeformableOwnerId owner, Float3 worldImpulse) noexcept;
    [[nodiscard]] bool apply_radial_impulse(DeformableOwnerId owner, Float3 worldCenter,
                                            float radius, float strength) noexcept;
    [[nodiscard]] bool set_vertex_target(DeformableOwnerId owner, std::uint32_t vertex,
                                         Float3 worldPosition, bool clearVelocity = true) noexcept;
    [[nodiscard]] DeformableTickTelemetry tick(float deltaSeconds,
                                                Float3 gravity = {0.0F, -9.81F, 0.0F});

    [[nodiscard]] const RuntimeSoftBodyState* state(DeformableOwnerId owner) const noexcept;
    [[nodiscard]] const DeformableRenderPacket* render_packet(DeformableOwnerId owner) const noexcept;
    [[nodiscard]] const DeformableCollisionProxy* collision_proxy(DeformableOwnerId owner) const noexcept;
    [[nodiscard]] const std::vector<DeformableRenderPacket>& render_packets() const noexcept {
        return renderPackets_;
    }
    [[nodiscard]] const std::vector<DeformableCollisionProxy>& collision_proxies() const noexcept {
        return collisionProxies_;
    }
    [[nodiscard]] std::vector<DeformableOwnerId> owners() const;
    [[nodiscard]] const SoftBodyWorld& solver_world() const noexcept { return world_; }

private:
    struct Record {
        RuntimeSoftBodyId body{};
        SoftBodyAsset asset;
        DeformableBindOptions options;
        std::optional<BSplineClothRuntimeProxy> smoothProxy;
    };
    void rebuild_packets(DeformableTickTelemetry& telemetry);

    SoftBodyWorld world_;
    std::map<DeformableOwnerId, Record> records_;
    std::vector<DeformableRenderPacket> renderPackets_;
    std::vector<DeformableCollisionProxy> collisionProxies_;
};

} // namespace dve
