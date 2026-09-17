#include "dve/deformable_runtime.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace dve {
namespace {

void set_error(std::string* error, std::string message) {
    if (error) *error = std::move(message);
}

Float3 cross(Float3 a, Float3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

DeformableBounds bounds_of(const std::vector<Float3>& positions) noexcept {
    DeformableBounds bounds;
    if (positions.empty()) return bounds;
    bounds.minimum = positions.front();
    bounds.maximum = positions.front();
    for (const Float3 position : positions) {
        bounds.minimum.x = std::min(bounds.minimum.x, position.x);
        bounds.minimum.y = std::min(bounds.minimum.y, position.y);
        bounds.minimum.z = std::min(bounds.minimum.z, position.z);
        bounds.maximum.x = std::max(bounds.maximum.x, position.x);
        bounds.maximum.y = std::max(bounds.maximum.y, position.y);
        bounds.maximum.z = std::max(bounds.maximum.z, position.z);
    }
    return bounds;
}

std::uint64_t packet_hash(const DeformableRenderPacket& packet) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto bytes = [&](const void* data, std::size_t size) {
        const auto* raw = static_cast<const std::uint8_t*>(data);
        for (std::size_t index = 0U; index < size; ++index) {
            hash ^= raw[index]; hash *= 1099511628211ULL;
        }
    };
    bytes(&packet.owner, sizeof(packet.owner));
    for (const DeformableVertex& vertex : packet.vertices) {
        bytes(&vertex.position.x, sizeof(float)); bytes(&vertex.position.y, sizeof(float));
        bytes(&vertex.position.z, sizeof(float)); bytes(&vertex.normal.x, sizeof(float));
        bytes(&vertex.normal.y, sizeof(float)); bytes(&vertex.normal.z, sizeof(float));
    }
    for (std::uint32_t index : packet.indices) bytes(&index, sizeof(index));
    return hash == 0U ? 1U : hash;
}

void compute_normals(DeformableRenderPacket& packet) noexcept {
    for (DeformableVertex& vertex : packet.vertices) vertex.normal = {};
    if (packet.primitive == DeformablePrimitiveKind::Triangles) {
        for (std::size_t index = 0U; index + 2U < packet.indices.size(); index += 3U) {
            const std::uint32_t a = packet.indices[index];
            const std::uint32_t b = packet.indices[index + 1U];
            const std::uint32_t c = packet.indices[index + 2U];
            if (a >= packet.vertices.size() || b >= packet.vertices.size() || c >= packet.vertices.size()) continue;
            const Float3 normal = cross(subtract(packet.vertices[b].position, packet.vertices[a].position),
                                        subtract(packet.vertices[c].position, packet.vertices[a].position));
            packet.vertices[a].normal = add(packet.vertices[a].normal, normal);
            packet.vertices[b].normal = add(packet.vertices[b].normal, normal);
            packet.vertices[c].normal = add(packet.vertices[c].normal, normal);
        }
        for (DeformableVertex& vertex : packet.vertices) vertex.normal = normalize(vertex.normal);
    } else {
        for (DeformableVertex& vertex : packet.vertices) vertex.normal = {0.0F, 0.0F, 1.0F};
    }
}

} // namespace

bool DeformableRuntime::bind(DeformableOwnerId owner, SoftBodyAsset asset,
                             const RigidTransform& worldTransform,
                             DeformableBindOptions options, std::string* error) {
    if (owner == 0U || records_.contains(owner)) {
        set_error(error, "deformable owner is invalid or already bound");
        return false;
    }
    if (!asset.validate(error)) return false;
    options.simulation.transform = worldTransform;
    std::optional<BSplineClothRuntimeProxy> smooth;
    if (options.smoothCloth) {
        if (asset.kind != SoftBodyKind::Cloth || options.clothColumns < 3U || options.clothRows < 3U ||
            static_cast<std::size_t>(options.clothColumns) * options.clothRows != asset.vertices.size()) {
            set_error(error, "smooth cloth requires matching three-by-three-or-larger control dimensions");
            return false;
        }
        std::string proxyError;
        BSplineClothRuntimeProxy proxy = make_bspline_cloth_runtime_proxy(
            asset, options.clothColumns, options.clothRows,
            options.embeddedColumns, options.embeddedRows, &proxyError);
        if (!proxy.validate(&proxyError)) {
            set_error(error, "could not build smooth cloth proxy: " + proxyError);
            return false;
        }
        smooth = std::move(proxy);
    }
    const RuntimeSoftBodyId body = world_.create(asset, options.simulation, error);
    if (body == 0U) return false;
    records_.emplace(owner, Record{body, std::move(asset), options, std::move(smooth)});
    DeformableTickTelemetry ignored;
    rebuild_packets(ignored);
    return true;
}

bool DeformableRuntime::bind_asset(DeformableOwnerId owner,
                                   const std::filesystem::path& path,
                                   const RigidTransform& worldTransform,
                                   DeformableBindOptions options, std::string* error) {
    SoftBodyAssetReadResult loaded = read_dvesoft(path);
    if (!loaded) { set_error(error, loaded.error); return false; }
    return bind(owner, std::move(loaded.asset), worldTransform, options, error);
}

bool DeformableRuntime::unbind(DeformableOwnerId owner) noexcept {
    const auto found = records_.find(owner);
    if (found == records_.end()) return false;
    (void)world_.destroy(found->second.body);
    records_.erase(found);
    DeformableTickTelemetry ignored;
    rebuild_packets(ignored);
    return true;
}

bool DeformableRuntime::contains(DeformableOwnerId owner) const noexcept { return records_.contains(owner); }

bool DeformableRuntime::set_running(DeformableOwnerId owner, bool running) noexcept {
    const auto found = records_.find(owner);
    if (found == records_.end()) return false;
    return running ? world_.play(found->second.body) : world_.pause(found->second.body);
}

bool DeformableRuntime::reset(DeformableOwnerId owner) noexcept {
    const auto found = records_.find(owner);
    return found != records_.end() && world_.reset(found->second.body);
}

bool DeformableRuntime::apply_impulse(DeformableOwnerId owner, Float3 impulse) noexcept {
    const auto found = records_.find(owner);
    return found != records_.end() && world_.apply_impulse(found->second.body, impulse);
}

bool DeformableRuntime::apply_radial_impulse(
    DeformableOwnerId owner, Float3 center, float radius, float strength) noexcept {
    const auto found = records_.find(owner);
    return found != records_.end() && world_.apply_radial_impulse(found->second.body, center, radius, strength);
}

bool DeformableRuntime::set_vertex_target(
    DeformableOwnerId owner, std::uint32_t vertex, Float3 position, bool clearVelocity) noexcept {
    const auto found = records_.find(owner);
    return found != records_.end() &&
        world_.set_vertex_target(found->second.body, vertex, position, clearVelocity);
}

DeformableTickTelemetry DeformableRuntime::tick(float deltaSeconds, Float3 gravity) {
    DeformableTickTelemetry telemetry;
    telemetry.solver = world_.step(deltaSeconds, gravity);
    rebuild_packets(telemetry);
    return telemetry;
}

const RuntimeSoftBodyState* DeformableRuntime::state(DeformableOwnerId owner) const noexcept {
    const auto found = records_.find(owner);
    return found == records_.end() ? nullptr : world_.find_state(found->second.body);
}

const DeformableRenderPacket* DeformableRuntime::render_packet(DeformableOwnerId owner) const noexcept {
    const auto found = std::find_if(renderPackets_.begin(), renderPackets_.end(),
        [owner](const DeformableRenderPacket& packet) { return packet.owner == owner; });
    return found == renderPackets_.end() ? nullptr : &*found;
}

const DeformableCollisionProxy* DeformableRuntime::collision_proxy(DeformableOwnerId owner) const noexcept {
    const auto found = std::find_if(collisionProxies_.begin(), collisionProxies_.end(),
        [owner](const DeformableCollisionProxy& proxy) { return proxy.owner == owner; });
    return found == collisionProxies_.end() ? nullptr : &*found;
}

std::vector<DeformableOwnerId> DeformableRuntime::owners() const {
    std::vector<DeformableOwnerId> result;
    result.reserve(records_.size());
    for (const auto& [owner, record] : records_) { (void)record; result.push_back(owner); }
    return result;
}

void DeformableRuntime::rebuild_packets(DeformableTickTelemetry& telemetry) {
    renderPackets_.clear();
    collisionProxies_.clear();
    renderPackets_.reserve(records_.size());
    collisionProxies_.reserve(records_.size());
    for (auto& [owner, record] : records_) {
        const RuntimeSoftBodyState* state = world_.find_state(record.body);
        if (!state) continue;
        DeformableRenderPacket packet;
        packet.owner = owner; packet.runtimeBody = record.body; packet.kind = record.asset.kind;
        packet.simulationFrame = state->simulationFrame; packet.doubleSided = record.asset.doubleSided;
        if (record.smoothProxy) {
            std::string ignored;
            if (!record.smoothProxy->update(state->positions, record.options.evaluateSmoothEnergy, &ignored)) {
                ++telemetry.failedProxyUpdates; continue;
            }
            packet.smoothProxy = true;
            packet.primitive = DeformablePrimitiveKind::Triangles;
            packet.vertices.reserve(record.smoothProxy->embeddedMesh.vertices.size());
            for (const auto& vertex : record.smoothProxy->embeddedMesh.vertices)
                packet.vertices.push_back({vertex.position, vertex.normal});
            packet.indices.reserve(record.smoothProxy->embeddedMesh.triangles.size() * 3U);
            for (const auto& triangle : record.smoothProxy->embeddedMesh.triangles)
                packet.indices.insert(packet.indices.end(), triangle.begin(), triangle.end());
        } else {
            packet.vertices.reserve(state->positions.size());
            for (Float3 position : state->positions) packet.vertices.push_back({position, {}});
            if (!record.asset.faces.empty()) {
                packet.primitive = DeformablePrimitiveKind::Triangles;
                packet.indices.reserve(record.asset.faces.size() * 3U);
                for (const SoftBodyFace& face : record.asset.faces)
                    packet.indices.insert(packet.indices.end(), face.vertices.begin(), face.vertices.end());
            } else {
                packet.primitive = DeformablePrimitiveKind::Lines;
                packet.indices.reserve(record.asset.stretchConstraints.size() * 2U);
                for (const auto& edge : record.asset.stretchConstraints) {
                    packet.indices.push_back(edge.vertexA); packet.indices.push_back(edge.vertexB);
                }
            }
            compute_normals(packet);
        }
        std::vector<Float3> renderPositions;
        renderPositions.reserve(packet.vertices.size());
        for (const DeformableVertex& vertex : packet.vertices) renderPositions.push_back(vertex.position);
        packet.bounds = bounds_of(renderPositions);
        packet.deformationHash = packet_hash(packet);
        telemetry.renderVerticesPublished += packet.vertices.size();
        ++telemetry.renderPacketsBuilt;
        renderPackets_.push_back(std::move(packet));

        DeformableCollisionProxy collision;
        collision.owner = owner; collision.particles = state->positions;
        collision.simulationFrame = state->simulationFrame; collision.bounds = bounds_of(collision.particles);
        collision.radii.reserve(record.asset.vertices.size());
        for (const SoftBodyVertex& vertex : record.asset.vertices) collision.radii.push_back(vertex.radius);
        collision.edges.reserve(record.asset.stretchConstraints.size());
        for (const auto& edge : record.asset.stretchConstraints)
            collision.edges.push_back({edge.vertexA, edge.vertexB});
        collision.surfaceTriangles.reserve(record.asset.faces.size());
        for (const SoftBodyFace& face : record.asset.faces) collision.surfaceTriangles.push_back(face.vertices);
        telemetry.collisionParticlesPublished += collision.particles.size();
        collisionProxies_.push_back(std::move(collision));
    }
}

} // namespace dve
