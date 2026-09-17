#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "dve/types.hpp"

namespace dve {

using NavigationPolygonId = std::uint32_t;
constexpr NavigationPolygonId kInvalidNavigationPolygon = std::numeric_limits<NavigationPolygonId>::max();

struct NavigationBounds {
    Float3 minimum{};
    Float3 maximum{};
};

struct NavigationTriangle {
    std::array<Float3, 3> vertices{};
    std::uint16_t area{1U};
    std::uint16_t flags{0xFFFFU};
    std::uint64_t sourceId{};
};

struct NavigationBuildSettings {
    float maximumSlopeDegrees{45.0F};
    float agentRadiusMeters{0.30F};
    float agentHeightMeters{1.80F};
    float maximumStepHeightMeters{0.45F};
    float edgeMatchToleranceMeters{0.001F};
    float tileSizeMeters{16.0F};
    std::uint32_t maximumPolygons{1'000'000U};

    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
};

struct NavigationPortal {
    Float3 left{};
    Float3 right{};
    NavigationPolygonId neighbour{kInvalidNavigationPolygon};
};

struct NavigationPolygon {
    std::array<Float3, 3> vertices{};
    Float3 center{};
    Float3 normal{};
    NavigationBounds bounds{};
    std::vector<NavigationPortal> portals;
    std::uint16_t area{1U};
    std::uint16_t flags{0xFFFFU};
    std::uint64_t sourceId{};
    std::int32_t tileX{};
    std::int32_t tileZ{};
};

struct NavigationOffMeshLink {
    Float3 start{};
    Float3 end{};
    float radiusMeters{0.5F};
    float traversalCost{1.0F};
    bool bidirectional{true};
    std::uint16_t area{1U};
    std::uint16_t flags{0xFFFFU};
    std::uint64_t userId{};
};

struct NavigationMesh {
    NavigationBuildSettings settings{};
    NavigationBounds bounds{};
    std::vector<NavigationPolygon> polygons;
    std::vector<NavigationOffMeshLink> offMeshLinks;
    std::uint64_t contentHash{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
};

struct NavigationBuildDiagnostic {
    enum class Severity : std::uint8_t { Information, Warning, Error };
    Severity severity{Severity::Information};
    std::string code;
    std::string message;
};

struct NavigationBuildResult {
    NavigationMesh mesh{};
    std::vector<NavigationBuildDiagnostic> diagnostics;
    [[nodiscard]] bool success() const noexcept;
};

struct NavigationQueryFilter {
    std::uint16_t includeFlags{0xFFFFU};
    std::uint16_t excludeFlags{};
    std::array<float, 16> areaCosts{1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F,
                                    1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F};
    float nearestSearchRadiusMeters{4.0F};
    std::uint32_t maximumVisitedPolygons{65536U};
};

struct NavigationObstacle {
    NavigationBounds bounds{};
    std::uint16_t blockedFlags{0xFFFFU};
};

enum class NavigationPathPointKind : std::uint8_t { Surface, OffMeshStart, OffMeshEnd };

struct NavigationPathPoint {
    Float3 position{};
    NavigationPathPointKind kind{NavigationPathPointKind::Surface};
    std::uint64_t userId{};
};

struct NavigationPath {
    std::vector<NavigationPathPoint> points;
    std::vector<NavigationPolygonId> corridor;
    float totalCost{};
    bool partial{};
    std::string error;
    [[nodiscard]] explicit operator bool() const noexcept { return !points.empty() && error.empty(); }
};

struct NavigationNearestPoint {
    Float3 position{};
    NavigationPolygonId polygon{kInvalidNavigationPolygon};
    float distanceSquared{std::numeric_limits<float>::infinity()};
    [[nodiscard]] explicit operator bool() const noexcept { return polygon != kInvalidNavigationPolygon; }
};

[[nodiscard]] NavigationBuildResult build_navigation_mesh(
    std::span<const NavigationTriangle> triangles,
    const NavigationBuildSettings& settings = {},
    std::span<const NavigationOffMeshLink> offMeshLinks = {});

[[nodiscard]] NavigationNearestPoint nearest_navigation_point(
    const NavigationMesh& mesh,
    Float3 point,
    const NavigationQueryFilter& filter = {},
    std::span<const NavigationObstacle> obstacles = {});

[[nodiscard]] NavigationPath find_navigation_path(
    const NavigationMesh& mesh,
    Float3 start,
    Float3 end,
    const NavigationQueryFilter& filter = {},
    std::span<const NavigationObstacle> obstacles = {});

[[nodiscard]] std::uint64_t navigation_mesh_content_hash(const NavigationMesh& mesh) noexcept;
[[nodiscard]] bool write_dnav(const std::filesystem::path& path,
                              const NavigationMesh& mesh,
                              std::string* error = nullptr);
[[nodiscard]] std::optional<NavigationMesh> read_dnav(
    const std::filesystem::path& path,
    std::string* error = nullptr,
    std::uint64_t maximumBytes = 512ULL * 1024ULL * 1024ULL);

} // namespace dve
