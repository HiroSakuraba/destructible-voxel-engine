#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "dve/soft_body.hpp"
#include "dve/transform.hpp"

namespace dve {

using BSplineFloat2 = std::array<float, 2>;

enum class BSplineClothQuadratureScheme : std::uint8_t {
    FullPerKnotSpan,
    ReducedDualGrid,
};

struct BSplineBasis1D {
    std::array<std::uint32_t, 3> indices{};
    std::array<float, 3> values{};
    std::array<float, 3> firstDerivatives{};
    std::array<float, 3> secondDerivatives{};
    std::uint32_t count{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

[[nodiscard]] float quadratic_open_uniform_bspline_domain(std::uint32_t controlPointCount) noexcept;
[[nodiscard]] BSplineBasis1D evaluate_quadratic_open_uniform_bspline(
    std::uint32_t controlPointCount, float coordinate) noexcept;

struct BSplineClothSupport {
    std::array<std::uint32_t, 9> indices{};
    std::array<float, 9> weights{};
    std::array<float, 9> du{};
    std::array<float, 9> dv{};
    std::array<float, 9> duu{};
    std::array<float, 9> duv{};
    std::array<float, 9> dvv{};
    std::uint32_t count{};
};

struct BSplineClothMaterial {
    float density{472.6F};
    float thickness{0.000318F};
    float stretchStiffness{2.0e6F};
    float shearStiffness{1.0e4F};
    float bendingStiffness{2.0e-4F};
    float strainRate{0.0F};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct BSplineClothPatch {
    std::string name{"B-Spline Cloth"};
    std::uint32_t columns{8U};
    std::uint32_t rows{8U};
    std::vector<Float3> controlPoints;
    std::vector<BSplineFloat2> restControlPoints;
    std::vector<float> inverseMasses;
    BSplineClothMaterial material{};
    BSplineClothQuadratureScheme quadratureScheme{
        BSplineClothQuadratureScheme::ReducedDualGrid};
    std::uint32_t embeddedColumns{32U};
    std::uint32_t embeddedRows{32U};
    bool doubleSided{true};
    std::uint64_t contentHash{};

    [[nodiscard]] float domain_u() const noexcept;
    [[nodiscard]] float domain_v() const noexcept;
    [[nodiscard]] bool validate(std::string* error = nullptr) const;
    void recompute_hash() noexcept;
};

struct BSplineClothSurfaceSample {
    Float3 position{};
    Float3 du{};
    Float3 dv{};
    Float3 duu{};
    Float3 duv{};
    Float3 dvv{};
    Float3 normal{};
    BSplineFloat2 restPosition{};
    BSplineFloat2 restDu{};
    BSplineFloat2 restDv{};
    BSplineFloat2 restDuu{};
    BSplineFloat2 restDuv{};
    BSplineFloat2 restDvv{};
    BSplineClothSupport support{};
    bool finite{true};
};

[[nodiscard]] BSplineClothSupport evaluate_bspline_cloth_support(
    const BSplineClothPatch& patch, float u, float v) noexcept;
[[nodiscard]] BSplineClothSurfaceSample evaluate_bspline_cloth_surface(
    const BSplineClothPatch& patch, float u, float v) noexcept;

struct BSplineClothQuadraturePoint {
    float u{};
    float v{};
    float materialWeight{};
    BSplineClothSupport support{};
    bool boundary{};
};

struct BSplineClothQuadraturePlan {
    std::vector<BSplineClothQuadraturePoint> membranePoints;
    std::vector<BSplineClothQuadraturePoint> bendingPoints;
    std::vector<std::vector<std::uint32_t>> membranePointsByControlPoint;
    std::vector<std::vector<std::uint32_t>> bendingPointsByControlPoint;
    std::uint32_t maximumMembraneSupport{};
    std::uint32_t maximumBendingSupport{};
    std::uint64_t contentHash{};

    [[nodiscard]] bool validate(const BSplineClothPatch& patch,
                                std::string* error = nullptr) const;
};

[[nodiscard]] BSplineClothQuadraturePlan build_bspline_cloth_quadrature(
    const BSplineClothPatch& patch);

struct BSplineClothBendingStencil {
    std::array<std::uint32_t, 9> indices{};
    std::array<float, 9> laplacianCoefficients{};
    std::uint32_t count{};
    float weightedStiffness{};
};

[[nodiscard]] std::vector<BSplineClothBendingStencil>
precompute_bspline_cloth_bending_stencils(
    const BSplineClothPatch& patch,
    const BSplineClothQuadraturePlan& quadrature);

struct BSplineClothEnergySummary {
    double membraneEnergy{};
    double bendingEnergy{};
    double totalEnergy{};
    std::uint64_t membraneSamples{};
    std::uint64_t bendingSamples{};
    std::uint64_t invalidSamples{};
};

[[nodiscard]] BSplineClothEnergySummary evaluate_bspline_cloth_energy(
    const BSplineClothPatch& patch,
    const BSplineClothQuadraturePlan& quadrature);

[[nodiscard]] std::vector<float> compute_bspline_cloth_lumped_masses(
    const BSplineClothPatch& patch,
    const BSplineClothQuadraturePlan& quadrature);

struct BSplineClothEmbeddedVertex {
    float u{};
    float v{};
    Float3 position{};
    Float3 normal{};
    BSplineClothSupport support{};
};

struct BSplineClothEmbeddedMesh {
    std::uint32_t columns{};
    std::uint32_t rows{};
    std::vector<BSplineClothEmbeddedVertex> vertices;
    std::vector<std::array<std::uint32_t, 3>> triangles;
    std::uint64_t supportHash{};

    [[nodiscard]] bool validate(const BSplineClothPatch& patch,
                                std::string* error = nullptr) const;
};

[[nodiscard]] BSplineClothEmbeddedMesh build_bspline_cloth_embedded_mesh(
    const BSplineClothPatch& patch,
    std::uint32_t columns,
    std::uint32_t rows);
void update_bspline_cloth_embedded_mesh(
    const BSplineClothPatch& patch,
    BSplineClothEmbeddedMesh& mesh) noexcept;

struct BSplineClothSeamAnchor {
    float uA{};
    float vA{};
    float uB{};
    float vB{};
    float stiffness{1.0e4F};
};

struct BSplineClothSeamEvaluation {
    Float3 positionA{};
    Float3 positionB{};
    Float3 gap{};
    double energy{};
    BSplineClothSupport supportA{};
    BSplineClothSupport supportB{};
    bool finite{true};
};

[[nodiscard]] BSplineClothSeamEvaluation evaluate_bspline_cloth_seam(
    const BSplineClothPatch& patchA,
    const BSplineClothPatch& patchB,
    const BSplineClothSeamAnchor& seam) noexcept;

[[nodiscard]] BSplineClothPatch make_bspline_cloth_proxy(
    const SoftBodyAsset& cloth,
    std::uint32_t columns,
    std::uint32_t rows,
    std::uint32_t embeddedColumns = 32U,
    std::uint32_t embeddedRows = 32U,
    std::string* error = nullptr);

bool update_bspline_cloth_proxy(
    BSplineClothPatch& patch,
    std::span<const Float3> controlPositions,
    std::string* error = nullptr);

struct BSplineClothRuntimeProxy {
    BSplineClothPatch patch;
    BSplineClothQuadraturePlan quadrature;
    std::vector<BSplineClothBendingStencil> bendingStencils;
    BSplineClothEmbeddedMesh embeddedMesh;
    BSplineClothEnergySummary lastEnergy;
    std::uint64_t updateCount{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
    bool update(std::span<const Float3> controlPositions,
                bool evaluateEnergy = false,
                std::string* error = nullptr);
};

[[nodiscard]] BSplineClothRuntimeProxy make_bspline_cloth_runtime_proxy(
    const SoftBodyAsset& cloth,
    std::uint32_t columns,
    std::uint32_t rows,
    std::uint32_t embeddedColumns = 32U,
    std::uint32_t embeddedRows = 32U,
    std::string* error = nullptr);

} // namespace dve
