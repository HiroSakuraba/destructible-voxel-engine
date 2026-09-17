#include "dve/bspline_cloth.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

float vector_length(dve::Float3 value) {
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

float distance(dve::Float3 a, dve::Float3 b) {
    return vector_length({a.x - b.x, a.y - b.y, a.z - b.z});
}

} // namespace

int main() {
    using namespace dve;

    SoftBodyClothRecipe recipe;
    recipe.columns = 8U;
    recipe.rows = 6U;
    recipe.spacing = 0.12F;
    recipe.pinTopEdge = true;
    SoftBodyAsset cloth = make_soft_body_cloth(recipe);
    require(cloth.validate(), "source cloth validates");

    std::string error;
    BSplineClothPatch patch = make_bspline_cloth_proxy(
        cloth, recipe.columns, recipe.rows, 24U, 18U, &error);
    require(error.empty(), "proxy conversion reports no error");
    require(patch.validate(&error), "B-spline patch validates");
    require(patch.contentHash != 0U, "patch content hash is populated");

    for (float u : {0.0F, 0.25F, 1.0F, 2.35F, patch.domain_u()}) {
        const BSplineBasis1D basis = evaluate_quadratic_open_uniform_bspline(
            patch.columns, u);
        require(basis.validate(&error), "basis validates");
        float sum = 0.0F;
        float derivativeSum = 0.0F;
        for (std::uint32_t i = 0U; i < basis.count; ++i) {
            sum += basis.values[i];
            derivativeSum += basis.firstDerivatives[i];
        }
        require(std::abs(sum - 1.0F) < 2.0e-5F, "basis partitions unity");
        require(std::abs(derivativeSum) < 2.0e-4F,
                "basis first derivatives sum to zero");
    }

    const auto left = evaluate_bspline_cloth_surface(patch, 1.0F - 1.0e-4F, 1.3F);
    const auto right = evaluate_bspline_cloth_surface(patch, 1.0F + 1.0e-4F, 1.3F);
    require(left.finite && right.finite, "surface samples are finite");
    require(distance(left.position, right.position) < 1.0e-3F,
            "surface position is continuous across an interior knot");
    require(distance(left.du, right.du) < 2.0e-3F,
            "surface first derivative is continuous across an interior knot");

    const auto center = evaluate_bspline_cloth_surface(
        patch, patch.domain_u() * 0.5F, patch.domain_v() * 0.5F);
    require(center.finite, "center sample is finite");
    require(vector_length(center.normal) > 0.99F, "center normal is normalized");

    BSplineClothPatch fullPatch = patch;
    fullPatch.quadratureScheme = BSplineClothQuadratureScheme::FullPerKnotSpan;
    fullPatch.recompute_hash();
    const BSplineClothQuadraturePlan full = build_bspline_cloth_quadrature(fullPatch);
    const BSplineClothQuadraturePlan reduced = build_bspline_cloth_quadrature(patch);
    require(full.validate(fullPatch, &error), "full quadrature validates");
    require(reduced.validate(patch, &error), "reduced quadrature validates");
    require(reduced.membranePoints.size() < full.membranePoints.size(),
            "reduced membrane quadrature uses fewer points");

    std::uint32_t maximumInteriorMembraneSupport = 0U;
    for (const auto& point : reduced.membranePoints) {
        if (!point.boundary) {
            maximumInteriorMembraneSupport = std::max(
                maximumInteriorMembraneSupport, point.support.count);
        }
    }
    std::uint32_t maximumInteriorBendingSupport = 0U;
    for (const auto& point : reduced.bendingPoints) {
        if (!point.boundary) {
            maximumInteriorBendingSupport = std::max(
                maximumInteriorBendingSupport, point.support.count);
        }
    }
    require(maximumInteriorMembraneSupport <= 6U,
            "dual-grid membrane points touch at most six controls");
    require(maximumInteriorBendingSupport <= 9U,
            "interior bending stencils remain bounded by one tensor patch");

    const auto flatEnergy = evaluate_bspline_cloth_energy(patch, reduced);
    require(flatEnergy.invalidSamples == 0U, "flat energy has no invalid samples");
    require(flatEnergy.totalEnergy < 1.0e-5,
            "flat rest patch has negligible elastic energy");

    const auto stencils = precompute_bspline_cloth_bending_stencils(patch, reduced);
    require(stencils.size() == reduced.bendingPoints.size(),
            "one bending stencil is generated per bending point");
    require(std::all_of(stencils.begin(), stencils.end(), [](const auto& stencil) {
        return stencil.count > 0U && stencil.weightedStiffness > 0.0F;
    }), "bending stencils are usable and positively weighted");

    const std::vector<float> masses = compute_bspline_cloth_lumped_masses(patch, reduced);
    require(masses.size() == patch.controlPoints.size(), "one lumped mass per control");
    require(std::all_of(masses.begin(), masses.end(), [](float mass) {
        return std::isfinite(mass) && mass >= 0.0F;
    }), "lumped masses are finite and nonnegative");
    require(std::accumulate(masses.begin(), masses.end(), 0.0F) > 0.0F,
            "lumped mass is nonzero");

    BSplineClothEmbeddedMesh mesh = build_bspline_cloth_embedded_mesh(patch, 24U, 18U);
    require(mesh.validate(patch, &error), "embedded mesh validates");
    require(mesh.vertices.size() == 24U * 18U,
            "embedded mesh resolution is independent of controls");
    std::vector<Float3> originalEmbedded;
    originalEmbedded.reserve(mesh.vertices.size());
    for (const auto& vertex : mesh.vertices) originalEmbedded.push_back(vertex.position);

    const std::size_t displaced = static_cast<std::size_t>(recipe.rows / 2U) * recipe.columns +
                                  recipe.columns / 2U;
    patch.controlPoints[displaced].z += 0.18F;
    patch.recompute_hash();
    update_bspline_cloth_embedded_mesh(patch, mesh);
    float maximumEmbeddedDisplacement = 0.0F;
    for (std::size_t vertex = 0U; vertex < mesh.vertices.size(); ++vertex) {
        maximumEmbeddedDisplacement = std::max(
            maximumEmbeddedDisplacement,
            distance(originalEmbedded[vertex], mesh.vertices[vertex].position));
    }
    require(maximumEmbeddedDisplacement > 1.0e-4F,
            "embedded mesh follows control-point deformation");

    const auto deformedEnergy = evaluate_bspline_cloth_energy(patch, reduced);
    require(deformedEnergy.totalEnergy > flatEnergy.totalEnergy + 1.0e-7,
            "deformation raises elastic energy");
    require(deformedEnergy.bendingEnergy > flatEnergy.bendingEnergy + 1.0e-9,
            "out-of-plane deformation raises bending energy");

    BSplineClothPatch translated = patch;
    for (Float3& point : translated.controlPoints) point.x += 0.25F;
    translated.recompute_hash();
    BSplineClothSeamAnchor seam;
    seam.uA = patch.domain_u() * 0.5F;
    seam.vA = patch.domain_v() * 0.5F;
    seam.uB = seam.uA;
    seam.vB = seam.vA;
    const auto matchingSeam = evaluate_bspline_cloth_seam(patch, patch, seam);
    require(matchingSeam.finite && matchingSeam.energy < 1.0e-9,
            "matching parametric anchors have zero seam energy");
    const auto separatedSeam = evaluate_bspline_cloth_seam(patch, translated, seam);
    require(separatedSeam.finite && separatedSeam.energy > 0.0,
            "separated patches have positive seam energy");

    BSplineClothRuntimeProxy runtimeProxy = make_bspline_cloth_runtime_proxy(
        cloth, recipe.columns, recipe.rows, 20U, 16U, &error);
    require(runtimeProxy.validate(&error), "runtime B-spline proxy validates");
    std::vector<Float3> runtimePositions = runtimeProxy.patch.controlPoints;
    runtimePositions[displaced].z += 0.1F;
    require(runtimeProxy.update(runtimePositions, true, &error),
            "runtime proxy updates from solver control positions");
    require(runtimeProxy.updateCount == 1U && runtimeProxy.lastEnergy.totalEnergy > 0.0,
            "runtime proxy tracks updates and optional energy telemetry");

    std::vector<Float3> replacement = patch.controlPoints;
    replacement.front().y += 0.02F;
    require(update_bspline_cloth_proxy(patch, replacement, &error),
            "runtime proxy update accepts matching control state");
    require(!update_bspline_cloth_proxy(patch, std::span<const Float3>{}, &error),
            "runtime proxy update rejects wrong control count");

    std::cout << "B-spline cloth tests passed: controls=" << patch.controlPoints.size()
              << " reduced_membrane=" << reduced.membranePoints.size()
              << " reduced_bending=" << reduced.bendingPoints.size()
              << " embedded_vertices=" << mesh.vertices.size() << '\n';
    return 0;
}
