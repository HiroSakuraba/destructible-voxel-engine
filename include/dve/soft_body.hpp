#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "dve/transform.hpp"
#include "dve/simulation_quality.hpp"

namespace dve {

enum class SoftBodyKind : std::uint8_t {
    Cloth,
    Rope,
    Vegetation,
    DeformableProp,
};

enum class SoftBodyBendModel : std::uint8_t {
    Disabled,
    Distance,
    Dihedral,
    CosseratRod,
    DihedralShell,
};

struct SoftBodyVertex {
    Float3 position{};
    float inverseMass{1.0F};
    float radius{0.01F};
};

struct SoftBodyFace {
    std::array<std::uint32_t, 3> vertices{};
};

struct SoftBodyDistanceConstraint {
    std::uint32_t vertexA{};
    std::uint32_t vertexB{};
    float restLength{};
    float compliance{};
};


struct SoftBodyDihedralConstraint {
    std::array<std::uint32_t, 4> vertices{}; // edge A/B, opposite left/right
    float restAngle{};
    float compliance{};
};

struct SoftBodyVolumeConstraint {
    std::array<std::uint32_t, 4> vertices{};
    float sixRestVolume{};
    float compliance{};
};

struct SoftBodyAsset {
    std::string name{"Soft Body"};
    SoftBodyKind kind{SoftBodyKind::Cloth};
    SoftBodyBendModel bendModel{SoftBodyBendModel::Distance};
    std::vector<SoftBodyVertex> vertices;
    std::vector<SoftBodyFace> faces;
    std::vector<SoftBodyDistanceConstraint> stretchConstraints;
    std::vector<SoftBodyDistanceConstraint> bendConstraints;
    std::vector<SoftBodyDihedralConstraint> dihedralConstraints;
    std::vector<SoftBodyVolumeConstraint> volumeConstraints;
    bool doubleSided{true};
    float linearDamping{0.02F};
    float pressure{};
    std::uint64_t contentHash{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
    void recompute_hash() noexcept;
};

struct SoftBodyAssetReadResult {
    SoftBodyAsset asset;
    std::string error;
    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

[[nodiscard]] std::uint64_t soft_body_asset_content_hash(const SoftBodyAsset& asset) noexcept;
[[nodiscard]] bool write_dvesoft(
    const std::filesystem::path& path, const SoftBodyAsset& asset,
    std::string* error = nullptr);
[[nodiscard]] SoftBodyAssetReadResult read_dvesoft(
    const std::filesystem::path& path,
    std::uint64_t maximumBytes = 64ULL * 1024ULL * 1024ULL);

struct SoftBodyClothRecipe {
    std::uint32_t columns{16U};
    std::uint32_t rows{16U};
    float spacing{0.1F};
    float vertexMass{0.02F};
    float stretchCompliance{1.0e-7F};
    float bendCompliance{1.0e-4F};
    bool pinTopEdge{true};
};

struct SoftBodyRopeRecipe {
    std::uint32_t segments{24U};
    float spacing{0.1F};
    float vertexMass{0.02F};
    float stretchCompliance{1.0e-7F};
    float bendCompliance{1.0e-4F};
    bool pinRoot{true};
};

struct SoftBodyDeformableBoxRecipe {
    std::array<std::uint32_t, 3> cells{3U, 3U, 3U};
    float spacing{0.15F};
    float vertexMass{0.05F};
    float stretchCompliance{1.0e-6F};
    float volumeCompliance{1.0e-7F};
};

[[nodiscard]] SoftBodyAsset make_soft_body_cloth(const SoftBodyClothRecipe& recipe);
[[nodiscard]] SoftBodyAsset make_soft_body_rope(const SoftBodyRopeRecipe& recipe);
[[nodiscard]] SoftBodyAsset make_soft_body_vegetation(const SoftBodyRopeRecipe& recipe);
[[nodiscard]] SoftBodyAsset make_soft_body_deformable_box(
    const SoftBodyDeformableBoxRecipe& recipe);

// Solver-neutral topology that maps directly onto Jolt SoftBodySharedSettings:
// vertices, faces, edge constraints, tetrahedral volume constraints, bend mode,
// double-sided rendering/collision, pressure and damping.
struct JoltSoftBodyRecipe {
    SoftBodyKind kind{SoftBodyKind::Cloth};
    SoftBodyBendModel bendModel{SoftBodyBendModel::Distance};
    std::vector<SoftBodyVertex> vertices;
    std::vector<SoftBodyFace> faces;
    std::vector<SoftBodyDistanceConstraint> edges;
    std::vector<SoftBodyDihedralConstraint> dihedrals;
    std::vector<SoftBodyVolumeConstraint> volumes;
    bool facesDoubleSided{true};
    float pressure{};
    float linearDamping{};
};

[[nodiscard]] JoltSoftBodyRecipe make_jolt_soft_body_recipe(const SoftBodyAsset& asset);

using RuntimeSoftBodyId = std::uint64_t;

struct RuntimeSoftBodyInstance {
    RuntimeSoftBodyId objectId{};
    std::uint64_t assetId{};
    RigidTransform transform{};
    bool running{true};
    bool visible{true};
    bool collideWithGround{true};
    float groundHeight{};
    std::uint32_t solverIterations{8U};
    std::uint32_t maximumSubsteps{8U};
    float maximumDisplacementFraction{0.35F};
    float groundContactCompliance{1.0e-8F};
    float groundFriction{0.35F};
    float contactMultiplierDecay{0.25F};
};

struct RuntimeSoftBodyState {
    std::vector<Float3> positions;
    std::vector<Float3> previousPositions;
    std::vector<Float3> velocities;
    std::vector<float> stretchLambdas;
    std::vector<float> bendLambdas;
    std::vector<float> dihedralLambdas;
    std::vector<float> volumeLambdas;
    std::vector<float> contactLambdas;
    std::uint64_t simulationFrame{};
};

struct SoftBodyStepTelemetry {
    std::uint64_t bodiesStepped{};
    std::uint64_t verticesIntegrated{};
    std::uint64_t distanceConstraintsSolved{};
    std::uint64_t dihedralConstraintsSolved{};
    std::uint64_t volumeConstraintsSolved{};
    std::uint64_t groundContacts{};
    std::uint64_t contactMultipliersUpdated{};
    std::uint64_t substeps{};
    std::uint64_t nonFiniteCorrections{};
};

class SoftBodyWorld {
public:
    [[nodiscard]] RuntimeSoftBodyId create(SoftBodyAsset asset,
                                           RuntimeSoftBodyInstance instance = {},
                                           std::string* error = nullptr);
    bool destroy(RuntimeSoftBodyId objectId) noexcept;
    bool play(RuntimeSoftBodyId objectId) noexcept;
    bool pause(RuntimeSoftBodyId objectId) noexcept;
    bool reset(RuntimeSoftBodyId objectId) noexcept;
    bool apply_impulse(RuntimeSoftBodyId objectId, Float3 worldImpulse) noexcept;
    bool apply_radial_impulse(RuntimeSoftBodyId objectId, Float3 worldCenter,
                              float radius, float strength) noexcept;
    bool set_vertex_target(RuntimeSoftBodyId objectId, std::uint32_t vertex,
                           Float3 worldPosition, bool clearVelocity = true) noexcept;
    [[nodiscard]] SoftBodyStepTelemetry step(float deltaSeconds, Float3 gravity);

    [[nodiscard]] const SoftBodyAsset* find_asset(std::uint64_t assetId) const noexcept;
    [[nodiscard]] const RuntimeSoftBodyInstance* find_instance(
        RuntimeSoftBodyId objectId) const noexcept;
    [[nodiscard]] const RuntimeSoftBodyState* find_state(
        RuntimeSoftBodyId objectId) const noexcept;

private:
    struct Entry {
        RuntimeSoftBodyInstance instance;
        RuntimeSoftBodyState state;
    };
    RuntimeSoftBodyId nextObjectId_{1U};
    std::map<std::uint64_t, SoftBodyAsset> assets_;
    std::map<RuntimeSoftBodyId, Entry> entries_;
};

[[nodiscard]] std::string soft_body_kind_name(SoftBodyKind kind);

} // namespace dve
