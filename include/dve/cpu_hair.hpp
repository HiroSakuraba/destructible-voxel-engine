#pragma once

#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "dve/job_system.hpp"
#include "dve/transform.hpp"

namespace dve {

struct HairGuide {
    std::vector<Float3> points{};
    std::vector<std::uint8_t> anchored{};
    std::uint32_t layerId{};
};

struct HairAsset {
    std::string name{"Hair Groom"};
    std::vector<HairGuide> guides{};
    float pointRadiusMeters{0.003F};
    float stretchCompliance{2.0e-8F};
    float bendCompliance{2.0e-5F};
    float linearDampingPerSecond{2.5F};
    std::uint64_t contentHash{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
    void recompute_hash() noexcept;
    [[nodiscard]] std::size_t point_count() const noexcept;
};

struct HairAssetReadResult {
    HairAsset asset{};
    std::string error{};
    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

struct SisirPlyImportOptions {
    float scale{1.0F};
    std::uint64_t maximumBytes{256ULL * 1024ULL * 1024ULL};
    std::size_t maximumPoints{8ULL * 1024ULL * 1024ULL};
    std::size_t maximumGuides{1ULL * 1024ULL * 1024ULL};
    bool requireCurveIds{true};
    bool pinFirstPointWhenMissingAnchor{true};
};

[[nodiscard]] HairAssetReadResult read_sisir_hair_ply(
    const std::filesystem::path& path, const SisirPlyImportOptions& options = {});
[[nodiscard]] bool write_dvehair(const std::filesystem::path& path, const HairAsset& asset,
                                 std::string* error = nullptr);
[[nodiscard]] HairAssetReadResult read_dvehair(
    const std::filesystem::path& path,
    std::uint64_t maximumBytes = 256ULL * 1024ULL * 1024ULL);
[[nodiscard]] std::uint64_t hair_asset_content_hash(const HairAsset& asset) noexcept;

struct HairStrandRange {
    std::uint32_t firstPoint{};
    std::uint32_t pointCount{};
    std::uint32_t layerId{};
};

struct HairSphereCollider {
    Float3 center{};
    float radiusMeters{0.1F};
};

struct HairCapsuleCollider {
    Float3 pointA{};
    Float3 pointB{};
    float radiusMeters{0.1F};
};

struct HairPlaneCollider {
    Float3 normal{0.0F, 1.0F, 0.0F};
    float offset{}; // valid points satisfy dot(normal, point) >= offset
    bool enabled{};
};

struct HairCollisionSet {
    std::vector<HairSphereCollider> spheres{};
    std::vector<HairCapsuleCollider> capsules{};
    HairPlaneCollider plane{};
};

struct CpuHairSolverSettings {
    float fixedStepSeconds{1.0F / 120.0F};
    std::uint32_t maximumSubsteps{4U};
    std::uint32_t solverIterations{5U};
    std::uint32_t collisionIterations{2U};
    std::uint32_t workerChunkGuides{32U};
    std::uint32_t updateRateDivisor{1U};
    std::uint32_t maximumActiveGuides{}; // zero means all guides
    float maximumFrameDeltaSeconds{0.1F};
    float maximumVelocityMetersPerSecond{35.0F};
    float collisionFriction{0.15F};
    float windDragPerSecond{1.5F};
    float sleepVelocityMetersPerSecond{0.025F};
    float wakeRootMotionMeters{0.002F};
    float wakeWindMetersPerSecond{0.15F};
    float teleportDistanceMeters{0.5F};
    std::uint32_t sleepFrames{30U};
    std::uint32_t selfCollisionIterations{1U};
    std::uint32_t selfCollisionMaximumNeighbors{24U}; // hard candidate cap per point
    std::uint32_t maximumSelfCollisionGuides{}; // zero means all active guides
    float selfCollisionRadiusScale{1.0F}; // multiplies HairAsset::pointRadiusMeters
    float selfCollisionStiffness{0.85F}; // normalized Jacobi correction strength
    bool enableSleeping{true};
    bool enableBendConstraints{true};
    bool enableSelfCollision{false};
};

struct CpuHairInstanceDesc {
    RigidTransform rootTransform{};
    CpuHairSolverSettings solver{};
    Float3 gravity{0.0F, -9.81F, 0.0F};
    Float3 windVelocity{};
    HairCollisionSet collision{};
    bool running{true};
    bool visible{true};
};

using CpuHairId = std::uint64_t;
constexpr CpuHairId kInvalidCpuHairId = 0U;

struct HairRootTarget {
    std::uint32_t guideIndex{};
    Float3 worldPosition{};
};

struct CpuHairStepTelemetry {
    std::uint64_t instancesStepped{};
    std::uint64_t simulatedGuides{};
    std::uint64_t frozenGuides{};
    std::uint64_t sleepingGuides{};
    std::uint64_t pointsIntegrated{};
    std::uint64_t stretchConstraintsSolved{};
    std::uint64_t bendConstraintsSolved{};
    std::uint64_t collisionTests{};
    std::uint64_t collisionProjections{};
    std::uint64_t selfCollisionPoints{};
    std::uint64_t selfCollisionTests{};
    std::uint64_t selfCollisionProjections{};
    std::uint64_t selfCollisionHashInsertFailures{};
    std::uint64_t substeps{};
    std::uint64_t nonFiniteCorrections{};
    std::uint64_t workerBatches{};
    double droppedTimeSeconds{};
};

struct CpuHairView {
    std::span<const Float3> positions{};
    std::span<const Float3> previousPositions{};
    std::span<const HairStrandRange> strands{};
    std::span<const std::uint32_t> lineIndices{};
    std::span<const std::uint8_t> sleeping{};
    std::uint64_t simulationFrame{};
    bool visible{};
};

class CpuHairWorld {
public:
    explicit CpuHairWorld(std::size_t workerCount = JobSystem::default_worker_count());
    ~CpuHairWorld();

    CpuHairWorld(const CpuHairWorld&) = delete;
    CpuHairWorld& operator=(const CpuHairWorld&) = delete;
    CpuHairWorld(CpuHairWorld&&) noexcept;
    CpuHairWorld& operator=(CpuHairWorld&&) noexcept;

    [[nodiscard]] CpuHairId create(HairAsset asset, CpuHairInstanceDesc desc = {},
                                   std::string* error = nullptr);
    [[nodiscard]] CpuHairId create_from_asset(const std::filesystem::path& path,
                                              CpuHairInstanceDesc desc = {},
                                              std::string* error = nullptr);
    bool destroy(CpuHairId id) noexcept;
    bool set_running(CpuHairId id, bool running) noexcept;
    bool set_visible(CpuHairId id, bool visible) noexcept;
    bool set_root_transform(CpuHairId id, const RigidTransform& transform,
                            bool teleport = false) noexcept;
    bool set_root_targets(CpuHairId id, std::span<const HairRootTarget> targets,
                          bool teleport = false) noexcept;
    bool clear_root_targets(CpuHairId id) noexcept;
    bool set_wind(CpuHairId id, Float3 windVelocity) noexcept;
    bool set_gravity(CpuHairId id, Float3 gravity) noexcept;
    bool set_collision(CpuHairId id, HairCollisionSet collision);
    bool set_solver_settings(CpuHairId id, CpuHairSolverSettings settings,
                             std::string* error = nullptr);
    bool wake(CpuHairId id) noexcept;
    bool reset(CpuHairId id) noexcept;
    bool apply_impulse(CpuHairId id, Float3 impulse) noexcept;

    [[nodiscard]] CpuHairStepTelemetry step(float deltaSeconds);
    [[nodiscard]] CpuHairView view(CpuHairId id) const noexcept;
    [[nodiscard]] const HairAsset* asset(CpuHairId id) const noexcept;
    [[nodiscard]] std::size_t worker_count() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] HairAsset make_straight_hair_groom(
    std::uint32_t guideCount, std::uint32_t pointsPerGuide,
    float guideSpacingMeters = 0.01F, float segmentLengthMeters = 0.02F);

} // namespace dve
