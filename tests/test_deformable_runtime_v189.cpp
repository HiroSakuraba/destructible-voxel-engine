#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>

#include "dve/component.hpp"
#include "dve/deformable_runtime.hpp"
#include "dve/editor_asset_browser.hpp"
#include "dve/editor_file_workflow.hpp"
#include "dve/game_world.hpp"
#include "dve/rigid_body_adapter.hpp"

namespace {
using namespace dve;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::filesystem::path temporary_root() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("dve_v189_deformable_" + std::to_string(stamp));
    std::filesystem::create_directories(root);
    return root;
}

std::string bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

SoftBodyAsset cloth_asset() {
    return make_soft_body_cloth({8U, 6U, 0.12F, 0.02F, 1.0e-7F, 1.0e-4F, true});
}

void test_asset_persistence_and_routing(const std::filesystem::path& root) {
    const SoftBodyAsset cloth = cloth_asset();
    const auto pathA = root / "banner.dvesoft";
    const auto pathB = root / "banner_copy.dvesoft";
    std::string error;
    require(write_dvesoft(pathA, cloth, &error), error);
    const SoftBodyAssetReadResult loaded = read_dvesoft(pathA);
    require(static_cast<bool>(loaded), loaded.error);
    require(loaded.asset.contentHash == soft_body_asset_content_hash(loaded.asset),
            "soft-body content hash did not round-trip");
    require(loaded.asset.vertices.size() == cloth.vertices.size() &&
            loaded.asset.dihedralConstraints.size() == cloth.dihedralConstraints.size(),
            "soft-body topology did not round-trip");
    require(write_dvesoft(pathB, loaded.asset, &error), error);
    require(bytes(pathA) == bytes(pathB), "soft-body serialization is not canonical");
    SoftBodyAsset stale = loaded.asset;
    stale.name = "Stale Hash";
    require(!write_dvesoft(root / "stale.dvesoft", stale, &error),
            "writer accepted a stale soft-body content hash");
    require(!read_dvesoft(pathA, 32U), "soft-body parser ignored its byte bound");
    {
        std::ofstream append(pathB, std::ios::binary | std::ios::app);
        append << "trailing\n";
    }
    require(!read_dvesoft(pathB), "soft-body parser accepted trailing data");
    require(editor::classify_editor_asset(pathA) == editor::EditorAssetKind::Deformable,
            "asset browser did not classify .dvesoft");
    const auto route = editor::classify_editor_file(pathA);
    require(route.supported && route.intent == editor::EditorFileIntent::OpenDeformableAsset,
            "file workflow did not route .dvesoft");
    const ComponentTypeRegistry registry = ComponentTypeRegistry::make_default();
    const auto* schema = registry.find("dve.soft_body");
    require(schema != nullptr && schema->properties.size() >= 8U,
            "soft-body component schema is missing or incomplete");

    const SoftBodyAsset box = make_soft_body_deformable_box({{2U, 2U, 2U}, 0.12F, 0.05F, 1.0e-6F, 1.0e-7F});
    require(!box.faces.empty() && !box.volumeConstraints.empty(),
            "deformable prop does not publish both surface and volume topology");
}

void test_runtime_packets_and_interaction() {
    DeformableRuntime runtime;
    std::string error;
    SoftBodyAsset rope = make_soft_body_rope({10U, 0.1F, 0.02F, 1.0e-7F, 1.0e-4F, true});
    require(runtime.bind(1U, rope, make_rigid_transform({0.0F, 2.0F, 0.0F}, {}), {}, &error), error);
    const DeformableRenderPacket* ropePacket = runtime.render_packet(1U);
    require(ropePacket && ropePacket->primitive == DeformablePrimitiveKind::Lines &&
            ropePacket->indices.size() == rope.stretchConstraints.size() * 2U,
            "rope line render packet is incomplete");
    require(runtime.set_vertex_target(1U, 0U, {1.0F, 2.0F, 0.0F}),
            "pinned-vertex target update failed");
    require(!runtime.set_vertex_target(1U, 1U, {1.0F, 2.0F, 0.0F}),
            "dynamic vertex was accepted as a kinematic target");
    require(runtime.apply_impulse(1U, {0.2F, 0.0F, 0.0F}), "uniform impulse failed");
    require(runtime.apply_radial_impulse(1U, {0.0F, 1.5F, 0.0F}, 3.0F, 0.5F),
            "radial impulse failed");
    const auto ropeTelemetry = runtime.tick(1.0F / 60.0F);
    require(ropeTelemetry.solver.bodiesStepped == 1U && ropeTelemetry.renderPacketsBuilt == 1U,
            "rope was not simulated and republished");
    require(runtime.state(1U)->positions.front().x == 1.0F,
            "pinned target was not preserved by the solver");

    DeformableBindOptions smooth;
    smooth.smoothCloth = true;
    smooth.clothColumns = 8U;
    smooth.clothRows = 6U;
    smooth.embeddedColumns = 20U;
    smooth.embeddedRows = 16U;
    smooth.evaluateSmoothEnergy = true;
    smooth.simulation.groundHeight = -3.0F;
    require(runtime.bind(2U, cloth_asset(), make_rigid_transform({3.0F, 2.0F, 0.0F}, {}), smooth, &error), error);
    const DeformableRenderPacket* smoothPacket = runtime.render_packet(2U);
    require(smoothPacket && smoothPacket->smoothProxy &&
            smoothPacket->vertices.size() == 20U * 16U &&
            smoothPacket->primitive == DeformablePrimitiveKind::Triangles,
            "smooth cloth render packet is incomplete");
    const DeformableCollisionProxy* collision = runtime.collision_proxy(2U);
    require(collision && collision->particles.size() == 8U * 6U &&
            collision->surfaceTriangles.size() == cloth_asset().faces.size(),
            "cloth collision proxy did not retain solver topology");
    const std::uint64_t before = smoothPacket->deformationHash;
    const auto telemetry = runtime.tick(1.0F / 30.0F);
    smoothPacket = runtime.render_packet(2U);
    collision = runtime.collision_proxy(2U);
    require(smoothPacket && smoothPacket->simulationFrame > 0U &&
            smoothPacket->deformationHash != before,
            "smooth render mesh was not updated from simulation");
    require(collision && collision->simulationFrame == smoothPacket->simulationFrame,
            "render and collision publications are from different simulation frames");
    require(telemetry.renderVerticesPublished >= 20U * 16U &&
            telemetry.collisionParticlesPublished >= 8U * 6U,
            "deformation publication telemetry is incomplete");

    smooth.clothColumns = 7U;
    require(!runtime.bind(3U, cloth_asset(), {}, smooth, &error),
            "mismatched smooth-cloth dimensions were accepted");
}

void test_game_world_lifecycle(const std::filesystem::path& root) {
    const auto path = root / "world_banner.dvesoft";
    std::string error;
    require(write_dvesoft(path, cloth_asset(), &error), error);
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameObjectDesc desc;
    desc.name = "Runtime Banner";
    desc.transform = make_rigid_transform({0.0F, 2.0F, 0.0F}, {});
    const GameObjectId owner = world.create_object(std::move(desc), &error);
    require(owner != kInvalidGameObjectId, error);
    DeformableBindOptions options;
    options.smoothCloth = true;
    options.clothColumns = 8U;
    options.clothRows = 6U;
    options.embeddedColumns = 16U;
    options.embeddedRows = 12U;
    options.simulation.groundHeight = -2.0F;
    require(world.bind_deformable_asset(owner, path, options, &error), error);
    require(world.geometry_kind(owner) == GameGeometryKind::Deformable,
            "GameWorld did not expose deformable geometry ownership");
    require(!world.set_position(owner, {2.0F, 2.0F, 0.0F}),
            "GameWorld allowed the owner transform to diverge from live solver state");
    require(world.apply_impulse(owner, {0.2F, 0.0F, 0.0F}),
            "generic gameplay impulse was not routed to the deformable");
    world.tick(1.0F / 60.0F);
    const DeformableRenderPacket* packet = world.deformables().render_packet(owner);
    require(packet && packet->simulationFrame > 0U &&
            world.last_deformable_telemetry().renderPacketsBuilt == 1U,
            "GameWorld did not step and publish its deformable");
    const DeformableCollisionProxy* proxy = world.deformables().collision_proxy(owner);
    require(proxy && !proxy->surfaceTriangles.empty(), "GameWorld deformable collision proxy is empty");
    const auto triangle = proxy->surfaceTriangles.front();
    const Float3 center = multiply(add(add(proxy->particles[triangle[0]], proxy->particles[triangle[1]]),
                                       proxy->particles[triangle[2]]), 1.0F / 3.0F);
    const auto hit = world.raycast(add(center, {0.0F, 0.0F, 1.0F}), {0.0F, 0.0F, -1.0F}, 2.0F);
    require(hit && hit->objectId == owner, "gameplay raycast did not hit live deformable topology");
    const auto overlaps = world.sphere_overlap(proxy->particles.front(), 0.05F);
    require(std::find(overlaps.begin(), overlaps.end(), owner) != overlaps.end(),
            "gameplay sphere overlap did not hit deformable particles");
    const std::uint64_t pausedFrame = packet->simulationFrame;
    require(world.set_enabled(owner, false), "could not disable deformable owner");
    world.tick(1.0F / 60.0F);
    require(world.deformables().render_packet(owner)->simulationFrame == pausedFrame,
            "disabled deformable continued simulating");
    require(world.set_enabled(owner, true), "could not re-enable deformable owner");
    world.tick(1.0F / 60.0F);
    require(world.deformables().render_packet(owner)->simulationFrame > pausedFrame,
            "re-enabled deformable did not resume");

    GameObjectDesc parentDesc;
    parentDesc.name = "Parent";
    const GameObjectId parent = world.create_object(std::move(parentDesc), &error);
    require(parent != kInvalidGameObjectId, error);
    require(!world.attach_object(owner, parent, true, {}, true, true, &error),
            "live deformable owner was attached despite independent particle state");
    require(world.destroy_object(owner), "could not destroy deformable owner");
    require(!world.deformables().contains(owner) && world.deformables().render_packet(owner) == nullptr,
            "destroying owner leaked deformable runtime state");
}

} // namespace

int main() {
    try {
        const auto root = temporary_root();
        test_asset_persistence_and_routing(root);
        test_runtime_packets_and_interaction();
        test_game_world_lifecycle(root);
        std::cout << "DVE v1.89 deformable runtime tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "DVE v1.89 deformable runtime tests failed: " << exception.what() << '\n';
        return 1;
    }
}
