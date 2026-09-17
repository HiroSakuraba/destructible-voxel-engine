#include "dve/bspline_cloth.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <utility>

namespace dve {
namespace {

constexpr float kEpsilon = 1.0e-6F;
constexpr std::uint32_t kDegree = 2U;

void set_error(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

[[nodiscard]] bool finite_float2(const BSplineFloat2& value) noexcept {
    return std::isfinite(value[0]) && std::isfinite(value[1]);
}

[[nodiscard]] bool finite_float3(Float3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] Float3 cross(Float3 a, Float3 b) noexcept {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

void hash_bytes(std::uint64_t& hash, const void* data, std::size_t size) noexcept {
    constexpr std::uint64_t prime = 1099511628211ULL;
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t byteIndex = 0U; byteIndex < size; ++byteIndex) {
        hash ^= static_cast<std::uint64_t>(bytes[byteIndex]);
        hash *= prime;
    }
}

template <class T>
void hash_value(std::uint64_t& hash, const T& value) noexcept {
    hash_bytes(hash, &value, sizeof(T));
}

[[nodiscard]] std::vector<float> make_open_uniform_knots(std::uint32_t controlCount) {
    const std::uint32_t knotCount = controlCount + kDegree + 1U;
    std::vector<float> knots(knotCount, 0.0F);
    const float domainMaximum = quadratic_open_uniform_bspline_domain(controlCount);
    for (std::uint32_t knotIndex = 0U; knotIndex < knotCount; ++knotIndex) {
        if (knotIndex <= kDegree) {
            knots[knotIndex] = 0.0F;
        } else if (knotIndex >= controlCount) {
            knots[knotIndex] = domainMaximum;
        } else {
            knots[knotIndex] = static_cast<float>(knotIndex - kDegree);
        }
    }
    return knots;
}

[[nodiscard]] float safe_ratio(float numerator, float denominator) noexcept {
    return std::abs(denominator) > kEpsilon ? numerator / denominator : 0.0F;
}

[[nodiscard]] float clamp_coordinate(float coordinate, float domainMaximum) noexcept {
    if (!std::isfinite(coordinate)) return 0.0F;
    return std::clamp(coordinate, 0.0F, domainMaximum);
}

[[nodiscard]] std::uint32_t control_index(const BSplineClothPatch& patch,
                                          std::uint32_t column,
                                          std::uint32_t row) noexcept {
    return row * patch.columns + column;
}

struct RestInverseDerivatives {
    float ux{};
    float uy{};
    float vx{};
    float vy{};
    float traceUSecond{};
    float traceVSecond{};
    float determinant{};
    bool valid{};
};

[[nodiscard]] RestInverseDerivatives inverse_rest_derivatives(
    const BSplineClothSurfaceSample& sample) noexcept {
    RestInverseDerivatives result;
    const float xu = sample.restDu[0];
    const float yu = sample.restDu[1];
    const float xv = sample.restDv[0];
    const float yv = sample.restDv[1];
    const float determinant = xu * yv - xv * yu;
    result.determinant = determinant;
    if (!std::isfinite(determinant) || std::abs(determinant) <= 1.0e-10F) return result;

    const float inverseDeterminant = 1.0F / determinant;
    result.ux = yv * inverseDeterminant;
    result.uy = -xv * inverseDeterminant;
    result.vx = -yu * inverseDeterminant;
    result.vy = xu * inverseDeterminant;

    const auto inverse_second = [&](float firstU, float firstV) {
        const float qx = sample.restDuu[0] * firstU * firstU +
                         2.0F * sample.restDuv[0] * firstU * firstV +
                         sample.restDvv[0] * firstV * firstV;
        const float qy = sample.restDuu[1] * firstU * firstU +
                         2.0F * sample.restDuv[1] * firstU * firstV +
                         sample.restDvv[1] * firstV * firstV;
        return BSplineFloat2{
            -(result.ux * qx + result.uy * qy),
            -(result.vx * qx + result.vy * qy)};
    };

    const BSplineFloat2 secondX = inverse_second(result.ux, result.vx);
    const BSplineFloat2 secondY = inverse_second(result.uy, result.vy);
    result.traceUSecond = secondX[0] + secondY[0];
    result.traceVSecond = secondX[1] + secondY[1];
    result.valid = std::isfinite(result.traceUSecond) &&
                   std::isfinite(result.traceVSecond);
    return result;
}

struct GaussRule {
    std::array<float, 3> nodes{};
    std::array<float, 3> weights{};
    std::uint32_t count{};
};

[[nodiscard]] GaussRule gauss_rule(std::uint32_t order) noexcept {
    GaussRule result;
    if (order <= 1U) {
        result.nodes[0] = 0.0F;
        result.weights[0] = 2.0F;
        result.count = 1U;
        return result;
    }
    if (order == 2U) {
        constexpr float node = 0.57735026918962576451F;
        result.nodes = {-node, node, 0.0F};
        result.weights = {1.0F, 1.0F, 0.0F};
        result.count = 2U;
        return result;
    }
    constexpr float node = 0.77459666924148337704F;
    result.nodes = {-node, 0.0F, node};
    result.weights = {5.0F / 9.0F, 8.0F / 9.0F, 5.0F / 9.0F};
    result.count = 3U;
    return result;
}

[[nodiscard]] float map_node(float node, float minimum, float maximum) noexcept {
    return 0.5F * ((maximum - minimum) * node + maximum + minimum);
}

[[nodiscard]] float map_weight(float weight, float minimum, float maximum) noexcept {
    return 0.5F * (maximum - minimum) * weight;
}

[[nodiscard]] float rest_jacobian_measure(const BSplineClothPatch& patch,
                                          float u, float v) noexcept {
    const BSplineClothSurfaceSample sample = evaluate_bspline_cloth_surface(patch, u, v);
    const float determinant = sample.restDu[0] * sample.restDv[1] -
                              sample.restDv[0] * sample.restDu[1];
    return std::isfinite(determinant) ? std::abs(determinant) : 0.0F;
}

[[nodiscard]] BSplineClothSupport compact_quadrature_support(
    BSplineClothSupport support, bool includeSecondDerivatives) noexcept {
    BSplineClothSupport compact;
    for (std::uint32_t source = 0U; source < support.count; ++source) {
        const bool activeFirstOrder = std::abs(support.weights[source]) > kEpsilon ||
            std::abs(support.du[source]) > kEpsilon ||
            std::abs(support.dv[source]) > kEpsilon;
        const bool activeSecondOrder = std::abs(support.duu[source]) > kEpsilon ||
            std::abs(support.duv[source]) > kEpsilon ||
            std::abs(support.dvv[source]) > kEpsilon;
        if (!activeFirstOrder && !(includeSecondDerivatives && activeSecondOrder)) continue;
        const std::uint32_t destination = compact.count++;
        compact.indices[destination] = support.indices[source];
        compact.weights[destination] = support.weights[source];
        compact.du[destination] = support.du[source];
        compact.dv[destination] = support.dv[source];
        compact.duu[destination] = support.duu[source];
        compact.duv[destination] = support.duv[source];
        compact.dvv[destination] = support.dvv[source];
    }
    return compact;
}

void append_quadrature_point(const BSplineClothPatch& patch,
                             std::vector<BSplineClothQuadraturePoint>& points,
                             float u, float v, float parametricWeight,
                             bool boundary, bool includeSecondDerivatives) {
    const float materialWeight = parametricWeight * rest_jacobian_measure(patch, u, v);
    if (!(materialWeight > 0.0F) || !std::isfinite(materialWeight)) return;
    BSplineClothQuadraturePoint point;
    point.u = u;
    point.v = v;
    point.materialWeight = materialWeight;
    point.support = compact_quadrature_support(
        evaluate_bspline_cloth_support(patch, u, v), includeSecondDerivatives);
    point.boundary = boundary;
    points.push_back(point);
}

void append_tensor_rule(const BSplineClothPatch& patch,
                        std::vector<BSplineClothQuadraturePoint>& points,
                        float uMinimum, float uMaximum,
                        float vMinimum, float vMaximum,
                        std::uint32_t orderU, std::uint32_t orderV,
                        bool boundary, bool includeSecondDerivatives) {
    const GaussRule ruleU = gauss_rule(orderU);
    const GaussRule ruleV = gauss_rule(orderV);
    for (std::uint32_t uIndex = 0U; uIndex < ruleU.count; ++uIndex) {
        for (std::uint32_t vIndex = 0U; vIndex < ruleV.count; ++vIndex) {
            const float u = map_node(ruleU.nodes[uIndex], uMinimum, uMaximum);
            const float v = map_node(ruleV.nodes[vIndex], vMinimum, vMaximum);
            const float weight = map_weight(ruleU.weights[uIndex], uMinimum, uMaximum) *
                                 map_weight(ruleV.weights[vIndex], vMinimum, vMaximum);
            append_quadrature_point(patch, points, u, v, weight, boundary,
                                    includeSecondDerivatives);
        }
    }
}

[[nodiscard]] float dual_grid_coordinate(std::uint32_t gridIndex,
                                         std::uint32_t gridSize) noexcept {
    if (gridIndex == 1U) return 1.0F;
    if (gridIndex + 1U == gridSize) return static_cast<float>(gridIndex - 1U);
    return static_cast<float>(gridIndex) - 0.5F;
}

[[nodiscard]] BSplineFloat2 rest_position(const BSplineClothPatch& patch,
                                          float u, float v) noexcept {
    const BSplineClothSupport support = evaluate_bspline_cloth_support(patch, u, v);
    BSplineFloat2 result{};
    for (std::uint32_t supportIndex = 0U; supportIndex < support.count; ++supportIndex) {
        const std::uint32_t controlPoint = support.indices[supportIndex];
        result[0] += patch.restControlPoints[controlPoint][0] * support.weights[supportIndex];
        result[1] += patch.restControlPoints[controlPoint][1] * support.weights[supportIndex];
    }
    return result;
}

[[nodiscard]] float triangle_area(BSplineFloat2 a, BSplineFloat2 b,
                                  BSplineFloat2 c) noexcept {
    const float abx = b[0] - a[0];
    const float aby = b[1] - a[1];
    const float acx = c[0] - a[0];
    const float acy = c[1] - a[1];
    return 0.5F * std::abs(abx * acy - aby * acx);
}

[[nodiscard]] float rest_span_area(const BSplineClothPatch& patch,
                                   std::uint32_t spanU,
                                   std::uint32_t spanV) noexcept {
    const float u0 = static_cast<float>(spanU);
    const float u1 = static_cast<float>(spanU + 1U);
    const float v0 = static_cast<float>(spanV);
    const float v1 = static_cast<float>(spanV + 1U);
    const BSplineFloat2 lowerLeft = rest_position(patch, u0, v0);
    const BSplineFloat2 lowerRight = rest_position(patch, u1, v0);
    const BSplineFloat2 upperLeft = rest_position(patch, u0, v1);
    const BSplineFloat2 upperRight = rest_position(patch, u1, v1);
    return triangle_area(lowerLeft, upperLeft, lowerRight) +
           triangle_area(upperLeft, upperRight, lowerRight);
}

[[nodiscard]] std::uint64_t hash_support(const BSplineClothSupport& support) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    hash_value(hash, support.count);
    for (std::uint32_t supportIndex = 0U; supportIndex < support.count; ++supportIndex) {
        hash_value(hash, support.indices[supportIndex]);
        hash_value(hash, support.weights[supportIndex]);
        hash_value(hash, support.du[supportIndex]);
        hash_value(hash, support.dv[supportIndex]);
        hash_value(hash, support.duu[supportIndex]);
        hash_value(hash, support.duv[supportIndex]);
        hash_value(hash, support.dvv[supportIndex]);
    }
    return hash;
}

void finalize_quadrature_plan(const BSplineClothPatch& patch,
                              BSplineClothQuadraturePlan& plan) {
    const std::size_t controlPointCount = patch.controlPoints.size();
    plan.membranePointsByControlPoint.assign(controlPointCount, {});
    plan.bendingPointsByControlPoint.assign(controlPointCount, {});
    auto map_points = [&](const std::vector<BSplineClothQuadraturePoint>& points,
                          std::vector<std::vector<std::uint32_t>>& mapping,
                          std::uint32_t& maximumSupport) {
        for (std::size_t pointIndex = 0U; pointIndex < points.size(); ++pointIndex) {
            const auto& point = points[pointIndex];
            maximumSupport = std::max(maximumSupport, point.support.count);
            for (std::uint32_t supportIndex = 0U;
                 supportIndex < point.support.count; ++supportIndex) {
                mapping[point.support.indices[supportIndex]].push_back(
                    static_cast<std::uint32_t>(pointIndex));
            }
        }
    };
    map_points(plan.membranePoints, plan.membranePointsByControlPoint,
               plan.maximumMembraneSupport);
    map_points(plan.bendingPoints, plan.bendingPointsByControlPoint,
               plan.maximumBendingSupport);

    std::uint64_t hash = 1469598103934665603ULL;
    hash_value(hash, patch.contentHash);
    for (const auto& point : plan.membranePoints) {
        hash_value(hash, point.u);
        hash_value(hash, point.v);
        hash_value(hash, point.materialWeight);
        hash_value(hash, point.boundary);
        const std::uint64_t supportHash = hash_support(point.support);
        hash_value(hash, supportHash);
    }
    for (const auto& point : plan.bendingPoints) {
        hash_value(hash, point.u);
        hash_value(hash, point.v);
        hash_value(hash, point.materialWeight);
        hash_value(hash, point.boundary);
        const std::uint64_t supportHash = hash_support(point.support);
        hash_value(hash, supportHash);
    }
    plan.contentHash = hash == 0U ? 1U : hash;
}

[[nodiscard]] std::array<float, 9> laplacian_coefficients(
    const BSplineClothQuadraturePoint& point,
    const BSplineClothSurfaceSample& sample,
    bool& valid) noexcept {
    std::array<float, 9> coefficients{};
    const RestInverseDerivatives inverse = inverse_rest_derivatives(sample);
    valid = inverse.valid;
    if (!valid) return coefficients;
    const float firstSecondCoefficient = inverse.ux * inverse.ux + inverse.uy * inverse.uy;
    const float secondSecondCoefficient = inverse.vx * inverse.vx + inverse.vy * inverse.vy;
    const float mixedCoefficient = 2.0F *
        (inverse.ux * inverse.vx + inverse.uy * inverse.vy);
    for (std::uint32_t supportIndex = 0U;
         supportIndex < point.support.count; ++supportIndex) {
        coefficients[supportIndex] =
            firstSecondCoefficient * point.support.duu[supportIndex] +
            secondSecondCoefficient * point.support.dvv[supportIndex] +
            mixedCoefficient * point.support.duv[supportIndex] +
            inverse.traceUSecond * point.support.du[supportIndex] +
            inverse.traceVSecond * point.support.dv[supportIndex];
        if (!std::isfinite(coefficients[supportIndex])) valid = false;
    }
    return coefficients;
}

} // namespace

bool BSplineBasis1D::validate(std::string* error) const {
    if (count == 0U || count > indices.size()) {
        set_error(error, "B-spline basis support count is invalid");
        return false;
    }
    float valueSum = 0.0F;
    float firstSum = 0.0F;
    float secondSum = 0.0F;
    for (std::uint32_t supportIndex = 0U; supportIndex < count; ++supportIndex) {
        if (!std::isfinite(values[supportIndex]) ||
            !std::isfinite(firstDerivatives[supportIndex]) ||
            !std::isfinite(secondDerivatives[supportIndex])) {
            set_error(error, "B-spline basis contains a non-finite coefficient");
            return false;
        }
        valueSum += values[supportIndex];
        firstSum += firstDerivatives[supportIndex];
        secondSum += secondDerivatives[supportIndex];
    }
    if (std::abs(valueSum - 1.0F) > 2.0e-4F || std::abs(firstSum) > 2.0e-4F ||
        std::abs(secondSum) > 2.0e-3F) {
        set_error(error, "B-spline basis violates partition or derivative identities");
        return false;
    }
    return true;
}

float quadratic_open_uniform_bspline_domain(std::uint32_t controlPointCount) noexcept {
    return controlPointCount >= 3U ? static_cast<float>(controlPointCount - 2U) : 0.0F;
}

BSplineBasis1D evaluate_quadratic_open_uniform_bspline(
    std::uint32_t controlPointCount, float coordinate) noexcept {
    BSplineBasis1D result;
    if (controlPointCount < 3U) return result;
    const std::vector<float> knots = make_open_uniform_knots(controlPointCount);
    const std::size_t zeroCount = knots.size() - 1U;
    std::array<std::vector<float>, 3> basis;
    std::array<std::vector<float>, 3> first;
    std::array<std::vector<float>, 3> second;
    for (std::uint32_t degreeIndex = 0U; degreeIndex <= kDegree; ++degreeIndex) {
        basis[degreeIndex].assign(zeroCount, 0.0F);
        first[degreeIndex].assign(zeroCount, 0.0F);
        second[degreeIndex].assign(zeroCount, 0.0F);
    }
    const float domainMaximum = quadratic_open_uniform_bspline_domain(controlPointCount);
    const float clamped = clamp_coordinate(coordinate, domainMaximum);
    // Cox-de Boor uses half-open knot spans. Evaluate the clamped endpoint from
    // the left so the final quadratic basis remains one and its derivatives are
    // the well-defined one-sided derivatives used by surface and seam queries.
    const float u = clamped == domainMaximum
        ? std::nextafter(domainMaximum, 0.0F)
        : clamped;
    for (std::size_t basisIndex = 0U; basisIndex < zeroCount; ++basisIndex) {
        const bool inside = knots[basisIndex] <= u && u < knots[basisIndex + 1U];
        basis[0][basisIndex] = inside ? 1.0F : 0.0F;
    }
    for (std::uint32_t degreeIndex = 1U; degreeIndex <= kDegree; ++degreeIndex) {
        const std::size_t basisCount = knots.size() - degreeIndex - 1U;
        for (std::size_t basisIndex = 0U; basisIndex < basisCount; ++basisIndex) {
            const float leftDenominator =
                knots[basisIndex + degreeIndex] - knots[basisIndex];
            const float rightDenominator =
                knots[basisIndex + degreeIndex + 1U] - knots[basisIndex + 1U];
            basis[degreeIndex][basisIndex] =
                safe_ratio(u - knots[basisIndex], leftDenominator) *
                    basis[degreeIndex - 1U][basisIndex] +
                safe_ratio(knots[basisIndex + degreeIndex + 1U] - u,
                           rightDenominator) *
                    basis[degreeIndex - 1U][basisIndex + 1U];
            first[degreeIndex][basisIndex] =
                safe_ratio(static_cast<float>(degreeIndex), leftDenominator) *
                    basis[degreeIndex - 1U][basisIndex] -
                safe_ratio(static_cast<float>(degreeIndex), rightDenominator) *
                    basis[degreeIndex - 1U][basisIndex + 1U];
            second[degreeIndex][basisIndex] =
                safe_ratio(static_cast<float>(degreeIndex), leftDenominator) *
                    first[degreeIndex - 1U][basisIndex] -
                safe_ratio(static_cast<float>(degreeIndex), rightDenominator) *
                    first[degreeIndex - 1U][basisIndex + 1U];
        }
    }
    for (std::uint32_t basisIndex = 0U; basisIndex < controlPointCount; ++basisIndex) {
        const float value = basis[kDegree][basisIndex];
        const float derivative = first[kDegree][basisIndex];
        const float secondDerivative = second[kDegree][basisIndex];
        if (std::abs(value) <= kEpsilon && std::abs(derivative) <= kEpsilon &&
            std::abs(secondDerivative) <= kEpsilon) {
            continue;
        }
        if (result.count >= result.indices.size()) break;
        result.indices[result.count] = basisIndex;
        result.values[result.count] = value;
        result.firstDerivatives[result.count] = derivative;
        result.secondDerivatives[result.count] = secondDerivative;
        ++result.count;
    }
    return result;
}

bool BSplineClothMaterial::validate(std::string* error) const {
    if (!(density > 0.0F) || !(thickness > 0.0F) || stretchStiffness < 0.0F ||
        shearStiffness < 0.0F || bendingStiffness < 0.0F || strainRate < 0.0F ||
        !std::isfinite(density) || !std::isfinite(thickness) ||
        !std::isfinite(stretchStiffness) || !std::isfinite(shearStiffness) ||
        !std::isfinite(bendingStiffness) || !std::isfinite(strainRate)) {
        set_error(error, "B-spline cloth material parameters are invalid");
        return false;
    }
    return true;
}

float BSplineClothPatch::domain_u() const noexcept {
    return quadratic_open_uniform_bspline_domain(columns);
}

float BSplineClothPatch::domain_v() const noexcept {
    return quadratic_open_uniform_bspline_domain(rows);
}

bool BSplineClothPatch::validate(std::string* error) const {
    if (columns < 3U || rows < 3U) {
        set_error(error, "B-spline cloth needs at least 3x3 control points");
        return false;
    }
    const std::size_t expected = static_cast<std::size_t>(columns) * rows;
    if (controlPoints.size() != expected || restControlPoints.size() != expected ||
        inverseMasses.size() != expected) {
        set_error(error, "B-spline cloth control arrays do not match the grid dimensions");
        return false;
    }
    if (embeddedColumns < 2U || embeddedRows < 2U) {
        set_error(error, "B-spline cloth embedded mesh resolution is invalid");
        return false;
    }
    if (!material.validate(error)) return false;
    for (std::size_t controlPoint = 0U; controlPoint < expected; ++controlPoint) {
        if (!finite_float3(controlPoints[controlPoint]) ||
            !finite_float2(restControlPoints[controlPoint]) ||
            inverseMasses[controlPoint] < 0.0F ||
            !std::isfinite(inverseMasses[controlPoint])) {
            set_error(error, "B-spline cloth contains an invalid control point");
            return false;
        }
    }
    const std::uint32_t spanCountU = columns - 2U;
    const std::uint32_t spanCountV = rows - 2U;
    for (std::uint32_t spanV = 0U; spanV < spanCountV; ++spanV) {
        for (std::uint32_t spanU = 0U; spanU < spanCountU; ++spanU) {
            const float measure = rest_jacobian_measure(
                *this, static_cast<float>(spanU) + 0.5F,
                static_cast<float>(spanV) + 0.5F);
            if (!(measure > 1.0e-9F)) {
                set_error(error, "B-spline cloth rest mapping is singular");
                return false;
            }
        }
    }
    return true;
}

void BSplineClothPatch::recompute_hash() noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    hash_bytes(hash, name.data(), name.size());
    hash_value(hash, columns);
    hash_value(hash, rows);
    hash_value(hash, material.density);
    hash_value(hash, material.thickness);
    hash_value(hash, material.stretchStiffness);
    hash_value(hash, material.shearStiffness);
    hash_value(hash, material.bendingStiffness);
    hash_value(hash, material.strainRate);
    hash_value(hash, quadratureScheme);
    hash_value(hash, embeddedColumns);
    hash_value(hash, embeddedRows);
    hash_value(hash, doubleSided);
    for (const Float3 point : controlPoints) hash_value(hash, point);
    for (const BSplineFloat2& point : restControlPoints) hash_value(hash, point);
    for (const float inverseMass : inverseMasses) hash_value(hash, inverseMass);
    contentHash = hash == 0U ? 1U : hash;
}

BSplineClothSupport evaluate_bspline_cloth_support(
    const BSplineClothPatch& patch, float u, float v) noexcept {
    BSplineClothSupport result;
    const BSplineBasis1D basisU =
        evaluate_quadratic_open_uniform_bspline(patch.columns, u);
    const BSplineBasis1D basisV =
        evaluate_quadratic_open_uniform_bspline(patch.rows, v);
    for (std::uint32_t vSupport = 0U; vSupport < basisV.count; ++vSupport) {
        for (std::uint32_t uSupport = 0U; uSupport < basisU.count; ++uSupport) {
            if (result.count >= result.indices.size()) return result;
            const float valueU = basisU.values[uSupport];
            const float valueV = basisV.values[vSupport];
            const float firstU = basisU.firstDerivatives[uSupport];
            const float firstV = basisV.firstDerivatives[vSupport];
            const float secondU = basisU.secondDerivatives[uSupport];
            const float secondV = basisV.secondDerivatives[vSupport];
            const std::uint32_t destination = result.count++;
            result.indices[destination] = control_index(
                patch, basisU.indices[uSupport], basisV.indices[vSupport]);
            result.weights[destination] = valueU * valueV;
            result.du[destination] = firstU * valueV;
            result.dv[destination] = valueU * firstV;
            result.duu[destination] = secondU * valueV;
            result.duv[destination] = firstU * firstV;
            result.dvv[destination] = valueU * secondV;
        }
    }
    return result;
}

BSplineClothSurfaceSample evaluate_bspline_cloth_surface(
    const BSplineClothPatch& patch, float u, float v) noexcept {
    BSplineClothSurfaceSample sample;
    sample.support = evaluate_bspline_cloth_support(patch, u, v);
    for (std::uint32_t supportIndex = 0U;
         supportIndex < sample.support.count; ++supportIndex) {
        const std::uint32_t pointIndex = sample.support.indices[supportIndex];
        if (pointIndex >= patch.controlPoints.size() ||
            pointIndex >= patch.restControlPoints.size()) {
            sample.finite = false;
            continue;
        }
        const Float3 world = patch.controlPoints[pointIndex];
        const BSplineFloat2 rest = patch.restControlPoints[pointIndex];
        const float weight = sample.support.weights[supportIndex];
        const float derivativeU = sample.support.du[supportIndex];
        const float derivativeV = sample.support.dv[supportIndex];
        const float derivativeUU = sample.support.duu[supportIndex];
        const float derivativeUV = sample.support.duv[supportIndex];
        const float derivativeVV = sample.support.dvv[supportIndex];
        sample.position = add(sample.position, multiply(world, weight));
        sample.du = add(sample.du, multiply(world, derivativeU));
        sample.dv = add(sample.dv, multiply(world, derivativeV));
        sample.duu = add(sample.duu, multiply(world, derivativeUU));
        sample.duv = add(sample.duv, multiply(world, derivativeUV));
        sample.dvv = add(sample.dvv, multiply(world, derivativeVV));
        sample.restPosition[0] += rest[0] * weight;
        sample.restPosition[1] += rest[1] * weight;
        sample.restDu[0] += rest[0] * derivativeU;
        sample.restDu[1] += rest[1] * derivativeU;
        sample.restDv[0] += rest[0] * derivativeV;
        sample.restDv[1] += rest[1] * derivativeV;
        sample.restDuu[0] += rest[0] * derivativeUU;
        sample.restDuu[1] += rest[1] * derivativeUU;
        sample.restDuv[0] += rest[0] * derivativeUV;
        sample.restDuv[1] += rest[1] * derivativeUV;
        sample.restDvv[0] += rest[0] * derivativeVV;
        sample.restDvv[1] += rest[1] * derivativeVV;
    }
    const Float3 rawNormal = cross(sample.du, sample.dv);
    sample.normal = normalize(rawNormal);
    sample.finite = sample.finite && finite_float3(sample.position) &&
                    finite_float3(sample.du) && finite_float3(sample.dv) &&
                    finite_float3(sample.duu) && finite_float3(sample.duv) &&
                    finite_float3(sample.dvv) && finite_float3(sample.normal) &&
                    finite_float2(sample.restPosition) && finite_float2(sample.restDu) &&
                    finite_float2(sample.restDv) && finite_float2(sample.restDuu) &&
                    finite_float2(sample.restDuv) && finite_float2(sample.restDvv);
    return sample;
}

bool BSplineClothQuadraturePlan::validate(const BSplineClothPatch& patch,
                                          std::string* error) const {
    if (membranePoints.empty() || bendingPoints.empty() || contentHash == 0U) {
        set_error(error, "B-spline cloth quadrature plan is empty or unhashed");
        return false;
    }
    if (membranePointsByControlPoint.size() != patch.controlPoints.size() ||
        bendingPointsByControlPoint.size() != patch.controlPoints.size()) {
        set_error(error, "B-spline cloth quadrature incidence maps have the wrong size");
        return false;
    }
    auto validate_points = [&](const std::vector<BSplineClothQuadraturePoint>& points) {
        for (const auto& point : points) {
            if (!(point.materialWeight > 0.0F) || !std::isfinite(point.materialWeight) ||
                point.support.count == 0U || point.support.count > 9U) return false;
            for (std::uint32_t supportIndex = 0U;
                 supportIndex < point.support.count; ++supportIndex) {
                if (point.support.indices[supportIndex] >= patch.controlPoints.size())
                    return false;
            }
        }
        return true;
    };
    if (!validate_points(membranePoints) || !validate_points(bendingPoints)) {
        set_error(error, "B-spline cloth quadrature contains an invalid point");
        return false;
    }
    return true;
}

BSplineClothQuadraturePlan build_bspline_cloth_quadrature(
    const BSplineClothPatch& patch) {
    BSplineClothQuadraturePlan plan;
    if (patch.columns < 3U || patch.rows < 3U) return plan;
    const std::uint32_t spanCountU = patch.columns - 2U;
    const std::uint32_t spanCountV = patch.rows - 2U;

    if (patch.quadratureScheme == BSplineClothQuadratureScheme::FullPerKnotSpan) {
        for (std::uint32_t spanV = 0U; spanV < spanCountV; ++spanV) {
            for (std::uint32_t spanU = 0U; spanU < spanCountU; ++spanU) {
                const bool boundary = spanU == 0U || spanV == 0U ||
                    spanU + 1U == spanCountU || spanV + 1U == spanCountV;
                append_tensor_rule(patch, plan.membranePoints,
                                   static_cast<float>(spanU),
                                   static_cast<float>(spanU + 1U),
                                   static_cast<float>(spanV),
                                   static_cast<float>(spanV + 1U),
                                   3U, 3U, boundary, false);
                append_tensor_rule(patch, plan.bendingPoints,
                                   static_cast<float>(spanU),
                                   static_cast<float>(spanU + 1U),
                                   static_cast<float>(spanV),
                                   static_cast<float>(spanV + 1U),
                                   3U, 3U, boundary, true);
            }
        }
        finalize_quadrature_plan(patch, plan);
        return plan;
    }

    for (std::uint32_t spanV = 0U; spanV < spanCountV; ++spanV) {
        for (std::uint32_t spanU = 0U; spanU < spanCountU; ++spanU) {
            const bool boundaryU = spanU == 0U || spanU + 1U == spanCountU;
            const bool boundaryV = spanV == 0U || spanV + 1U == spanCountV;
            if (!boundaryU && !boundaryV) continue;
            append_tensor_rule(patch, plan.membranePoints,
                               static_cast<float>(spanU),
                               static_cast<float>(spanU + 1U),
                               static_cast<float>(spanV),
                               static_cast<float>(spanV + 1U),
                               boundaryU ? 3U : 2U,
                               boundaryV ? 3U : 2U,
                               true, false);
            append_quadrature_point(patch, plan.bendingPoints,
                                    static_cast<float>(spanU) + 0.5F,
                                    static_cast<float>(spanV) + 0.5F,
                                    1.0F, true, true);
        }
    }

    const std::uint32_t dualGridSizeU = spanCountU + 1U;
    const std::uint32_t dualGridSizeV = spanCountV + 1U;
    if (spanCountU > 1U && spanCountV > 1U) {
        for (std::uint32_t gridV = 1U; gridV + 1U < dualGridSizeV; ++gridV) {
            for (std::uint32_t gridU = 1U; gridU + 1U < dualGridSizeU; ++gridU) {
                const float uMinimum = dual_grid_coordinate(gridU, dualGridSizeU);
                const float uMaximum = dual_grid_coordinate(gridU + 1U, dualGridSizeU);
                const float vMinimum = dual_grid_coordinate(gridV, dualGridSizeV);
                const float vMaximum = dual_grid_coordinate(gridV + 1U, dualGridSizeV);
                if (!(uMaximum > uMinimum) || !(vMaximum > vMinimum)) continue;
                const bool edgeU = gridU == 1U || gridU + 2U == dualGridSizeU;
                const bool edgeV = gridV == 1U || gridV + 2U == dualGridSizeV;
                if (edgeU && edgeV) {
                    append_tensor_rule(patch, plan.membranePoints,
                                       uMinimum, uMaximum, vMinimum, vMaximum,
                                       1U, 1U, true, false);
                } else if (edgeU) {
                    const float u = gridU == 1U ? uMinimum : uMaximum;
                    const GaussRule rule = gauss_rule(2U);
                    for (std::uint32_t pointIndex = 0U; pointIndex < rule.count;
                         ++pointIndex) {
                        const float v = map_node(rule.nodes[pointIndex], vMinimum, vMaximum);
                        const float weight = (uMaximum - uMinimum) *
                            map_weight(rule.weights[pointIndex], vMinimum, vMaximum);
                        append_quadrature_point(patch, plan.membranePoints,
                                                u, v, weight, true, false);
                    }
                } else if (edgeV) {
                    const float v = gridV == 1U ? vMinimum : vMaximum;
                    const GaussRule rule = gauss_rule(2U);
                    for (std::uint32_t pointIndex = 0U; pointIndex < rule.count;
                         ++pointIndex) {
                        const float u = map_node(rule.nodes[pointIndex], uMinimum, uMaximum);
                        const float weight = map_weight(rule.weights[pointIndex],
                                                        uMinimum, uMaximum) *
                                             (vMaximum - vMinimum);
                        append_quadrature_point(patch, plan.membranePoints,
                                                u, v, weight, true, false);
                    }
                } else if (((gridU + gridV) & 1U) == 0U) {
                    append_tensor_rule(patch, plan.membranePoints,
                                       uMinimum, uMaximum, vMinimum, vMaximum,
                                       2U, 1U, false, false);
                } else {
                    append_tensor_rule(patch, plan.membranePoints,
                                       uMinimum, uMaximum, vMinimum, vMaximum,
                                       1U, 2U, false, false);
                }
            }
        }
    }

    for (std::uint32_t interiorV = 1U; interiorV < spanCountV; ++interiorV) {
        for (std::uint32_t interiorU = 1U; interiorU < spanCountU; ++interiorU) {
            float materialWeight = 0.0F;
            for (std::int32_t offsetV = -1; offsetV <= 0; ++offsetV) {
                for (std::int32_t offsetU = -1; offsetU <= 0; ++offsetU) {
                    const std::int32_t spanU = static_cast<std::int32_t>(interiorU) + offsetU;
                    const std::int32_t spanV = static_cast<std::int32_t>(interiorV) + offsetV;
                    if (spanU < 0 || spanV < 0 ||
                        spanU >= static_cast<std::int32_t>(spanCountU) ||
                        spanV >= static_cast<std::int32_t>(spanCountV)) continue;
                    materialWeight += rest_span_area(
                        patch, static_cast<std::uint32_t>(spanU),
                        static_cast<std::uint32_t>(spanV)) * 0.25F;
                }
            }
            if (!(materialWeight > 0.0F)) continue;
            BSplineClothQuadraturePoint point;
            point.u = static_cast<float>(interiorU);
            point.v = static_cast<float>(interiorV);
            point.materialWeight = materialWeight;
            point.support = evaluate_bspline_cloth_support(patch, point.u, point.v);
            point.boundary = false;
            plan.bendingPoints.push_back(point);
        }
    }

    finalize_quadrature_plan(patch, plan);
    return plan;
}

std::vector<BSplineClothBendingStencil>
precompute_bspline_cloth_bending_stencils(
    const BSplineClothPatch& patch,
    const BSplineClothQuadraturePlan& quadrature) {
    std::vector<BSplineClothBendingStencil> stencils;
    stencils.reserve(quadrature.bendingPoints.size());
    for (const BSplineClothQuadraturePoint& point : quadrature.bendingPoints) {
        const BSplineClothSurfaceSample sample =
            evaluate_bspline_cloth_surface(patch, point.u, point.v);
        bool valid = false;
        const std::array<float, 9> coefficients =
            laplacian_coefficients(point, sample, valid);
        if (!valid) continue;
        BSplineClothBendingStencil stencil;
        stencil.count = point.support.count;
        stencil.weightedStiffness =
            point.materialWeight * patch.material.bendingStiffness;
        for (std::uint32_t supportIndex = 0U;
             supportIndex < stencil.count; ++supportIndex) {
            stencil.indices[supportIndex] = point.support.indices[supportIndex];
            stencil.laplacianCoefficients[supportIndex] = coefficients[supportIndex];
        }
        stencils.push_back(stencil);
    }
    return stencils;
}

BSplineClothEnergySummary evaluate_bspline_cloth_energy(
    const BSplineClothPatch& patch,
    const BSplineClothQuadraturePlan& quadrature) {
    BSplineClothEnergySummary summary;
    for (const BSplineClothQuadraturePoint& point : quadrature.membranePoints) {
        const BSplineClothSurfaceSample sample =
            evaluate_bspline_cloth_surface(patch, point.u, point.v);
        const RestInverseDerivatives inverse = inverse_rest_derivatives(sample);
        if (!sample.finite || !inverse.valid) {
            ++summary.invalidSamples;
            continue;
        }
        const Float3 firstMaterialDirection = add(
            multiply(sample.du, inverse.ux), multiply(sample.dv, inverse.vx));
        const Float3 secondMaterialDirection = add(
            multiply(sample.du, inverse.uy), multiply(sample.dv, inverse.vy));
        const double firstLength = static_cast<double>(length(firstMaterialDirection));
        const double secondLength = static_cast<double>(length(secondMaterialDirection));
        const double firstStrain = firstLength - 1.0;
        const double secondStrain = secondLength - 1.0;
        const double cubicFirst = firstStrain > 0.0
            ? static_cast<double>(patch.material.strainRate) * firstStrain * firstStrain * firstStrain
            : 0.0;
        const double cubicSecond = secondStrain > 0.0
            ? static_cast<double>(patch.material.strainRate) * secondStrain * secondStrain * secondStrain
            : 0.0;
        const double stretch = firstStrain * firstStrain + secondStrain * secondStrain +
                               cubicFirst + cubicSecond;
        const double shearValue = static_cast<double>(
            dot(firstMaterialDirection, secondMaterialDirection));
        const double density = static_cast<double>(patch.material.thickness) *
            (static_cast<double>(patch.material.stretchStiffness) * stretch +
             static_cast<double>(patch.material.shearStiffness) *
                 shearValue * shearValue);
        const double contribution = density * static_cast<double>(point.materialWeight);
        if (!std::isfinite(contribution)) {
            ++summary.invalidSamples;
            continue;
        }
        summary.membraneEnergy += contribution;
        ++summary.membraneSamples;
    }

    const std::vector<BSplineClothBendingStencil> stencils =
        precompute_bspline_cloth_bending_stencils(patch, quadrature);
    for (const BSplineClothBendingStencil& stencil : stencils) {
        Float3 laplacian{};
        for (std::uint32_t supportIndex = 0U;
             supportIndex < stencil.count; ++supportIndex) {
            laplacian = add(laplacian,
                            multiply(patch.controlPoints[stencil.indices[supportIndex]],
                                     stencil.laplacianCoefficients[supportIndex]));
        }
        const double contribution = 0.5 * static_cast<double>(stencil.weightedStiffness) *
                                    static_cast<double>(length_squared(laplacian));
        if (!std::isfinite(contribution)) {
            ++summary.invalidSamples;
            continue;
        }
        summary.bendingEnergy += contribution;
        ++summary.bendingSamples;
    }
    summary.totalEnergy = summary.membraneEnergy + summary.bendingEnergy;
    return summary;
}

std::vector<float> compute_bspline_cloth_lumped_masses(
    const BSplineClothPatch& patch,
    const BSplineClothQuadraturePlan& quadrature) {
    std::vector<float> masses(patch.controlPoints.size(), 0.0F);
    const double densityThickness = static_cast<double>(patch.material.density) *
                                    static_cast<double>(patch.material.thickness);
    for (const BSplineClothQuadraturePoint& point : quadrature.membranePoints) {
        for (std::uint32_t supportIndex = 0U;
             supportIndex < point.support.count; ++supportIndex) {
            const std::uint32_t controlPoint = point.support.indices[supportIndex];
            const double contribution = densityThickness *
                static_cast<double>(point.materialWeight) *
                static_cast<double>(point.support.weights[supportIndex]);
            if (contribution > 0.0 && std::isfinite(contribution)) {
                masses[controlPoint] += static_cast<float>(contribution);
            }
        }
    }
    return masses;
}

bool BSplineClothEmbeddedMesh::validate(const BSplineClothPatch& patch,
                                        std::string* error) const {
    if (columns < 2U || rows < 2U ||
        vertices.size() != static_cast<std::size_t>(columns) * rows ||
        triangles.size() != static_cast<std::size_t>(columns - 1U) *
                            (rows - 1U) * 2U ||
        supportHash == 0U) {
        set_error(error, "B-spline embedded mesh dimensions are invalid");
        return false;
    }
    for (const auto& vertex : vertices) {
        if (!finite_float3(vertex.position) || !finite_float3(vertex.normal) ||
            vertex.support.count == 0U || vertex.support.count > 9U) {
            set_error(error, "B-spline embedded mesh contains an invalid vertex");
            return false;
        }
        for (std::uint32_t supportIndex = 0U;
             supportIndex < vertex.support.count; ++supportIndex) {
            if (vertex.support.indices[supportIndex] >= patch.controlPoints.size()) {
                set_error(error, "B-spline embedded vertex references an invalid control point");
                return false;
            }
        }
    }
    for (const auto& triangle : triangles) {
        if (triangle[0] >= vertices.size() || triangle[1] >= vertices.size() ||
            triangle[2] >= vertices.size()) {
            set_error(error, "B-spline embedded mesh contains an invalid triangle");
            return false;
        }
    }
    return true;
}

BSplineClothEmbeddedMesh build_bspline_cloth_embedded_mesh(
    const BSplineClothPatch& patch,
    std::uint32_t columns,
    std::uint32_t rows) {
    BSplineClothEmbeddedMesh mesh;
    mesh.columns = std::max(columns, 2U);
    mesh.rows = std::max(rows, 2U);
    mesh.vertices.reserve(static_cast<std::size_t>(mesh.columns) * mesh.rows);
    const float domainU = patch.domain_u();
    const float domainV = patch.domain_v();
    std::uint64_t supportHash = 1469598103934665603ULL;
    for (std::uint32_t row = 0U; row < mesh.rows; ++row) {
        for (std::uint32_t column = 0U; column < mesh.columns; ++column) {
            const float u = domainU * static_cast<float>(column) /
                            static_cast<float>(mesh.columns - 1U);
            const float v = domainV * static_cast<float>(row) /
                            static_cast<float>(mesh.rows - 1U);
            const BSplineClothSurfaceSample sample =
                evaluate_bspline_cloth_surface(patch, u, v);
            mesh.vertices.push_back({u, v, sample.position, sample.normal, sample.support});
            const std::uint64_t vertexSupportHash = hash_support(sample.support);
            hash_value(supportHash, vertexSupportHash);
        }
    }
    for (std::uint32_t row = 0U; row + 1U < mesh.rows; ++row) {
        for (std::uint32_t column = 0U; column + 1U < mesh.columns; ++column) {
            const std::uint32_t lowerLeft = row * mesh.columns + column;
            const std::uint32_t lowerRight = lowerLeft + 1U;
            const std::uint32_t upperLeft = lowerLeft + mesh.columns;
            const std::uint32_t upperRight = upperLeft + 1U;
            mesh.triangles.push_back({lowerLeft, upperLeft, lowerRight});
            mesh.triangles.push_back({lowerRight, upperLeft, upperRight});
        }
    }
    mesh.supportHash = supportHash == 0U ? 1U : supportHash;
    return mesh;
}

void update_bspline_cloth_embedded_mesh(
    const BSplineClothPatch& patch,
    BSplineClothEmbeddedMesh& mesh) noexcept {
    for (auto& vertex : mesh.vertices) {
        Float3 position{};
        Float3 derivativeU{};
        Float3 derivativeV{};
        for (std::uint32_t supportIndex = 0U;
             supportIndex < vertex.support.count; ++supportIndex) {
            const std::uint32_t controlPoint = vertex.support.indices[supportIndex];
            if (controlPoint >= patch.controlPoints.size()) continue;
            const Float3 value = patch.controlPoints[controlPoint];
            position = add(position,
                           multiply(value, vertex.support.weights[supportIndex]));
            derivativeU = add(derivativeU,
                              multiply(value, vertex.support.du[supportIndex]));
            derivativeV = add(derivativeV,
                              multiply(value, vertex.support.dv[supportIndex]));
        }
        vertex.position = position;
        vertex.normal = normalize(cross(derivativeU, derivativeV));
    }
}

BSplineClothSeamEvaluation evaluate_bspline_cloth_seam(
    const BSplineClothPatch& patchA,
    const BSplineClothPatch& patchB,
    const BSplineClothSeamAnchor& seam) noexcept {
    BSplineClothSeamEvaluation result;
    const BSplineClothSurfaceSample sampleA =
        evaluate_bspline_cloth_surface(patchA, seam.uA, seam.vA);
    const BSplineClothSurfaceSample sampleB =
        evaluate_bspline_cloth_surface(patchB, seam.uB, seam.vB);
    result.positionA = sampleA.position;
    result.positionB = sampleB.position;
    result.gap = subtract(result.positionA, result.positionB);
    result.supportA = sampleA.support;
    result.supportB = sampleB.support;
    result.energy = static_cast<double>(std::max(seam.stiffness, 0.0F)) *
                    static_cast<double>(length_squared(result.gap));
    result.finite = sampleA.finite && sampleB.finite &&
                    finite_float3(result.gap) && std::isfinite(result.energy);
    return result;
}

BSplineClothPatch make_bspline_cloth_proxy(
    const SoftBodyAsset& cloth,
    std::uint32_t columns,
    std::uint32_t rows,
    std::uint32_t embeddedColumns,
    std::uint32_t embeddedRows,
    std::string* error) {
    BSplineClothPatch patch;
    if (columns < 3U || rows < 3U ||
        static_cast<std::size_t>(columns) * rows != cloth.vertices.size()) {
        set_error(error, "soft-body cloth does not match the requested B-spline control grid");
        return patch;
    }
    if (cloth.kind != SoftBodyKind::Cloth) {
        set_error(error, "only cloth soft bodies can create a B-spline cloth proxy");
        return patch;
    }
    patch.name = cloth.name + " B-Spline Proxy";
    patch.columns = columns;
    patch.rows = rows;
    patch.embeddedColumns = std::max(embeddedColumns, 2U);
    patch.embeddedRows = std::max(embeddedRows, 2U);
    patch.doubleSided = cloth.doubleSided;
    patch.controlPoints.reserve(cloth.vertices.size());
    patch.inverseMasses.reserve(cloth.vertices.size());
    patch.restControlPoints.resize(cloth.vertices.size());
    for (const SoftBodyVertex& vertex : cloth.vertices) {
        patch.controlPoints.push_back(vertex.position);
        patch.inverseMasses.push_back(vertex.inverseMass);
    }

    float horizontalSpacing = 0.0F;
    std::uint64_t horizontalEdges = 0U;
    for (std::uint32_t row = 0U; row < rows; ++row) {
        for (std::uint32_t column = 0U; column + 1U < columns; ++column) {
            const Float3 a = cloth.vertices[static_cast<std::size_t>(row) * columns + column].position;
            const Float3 b = cloth.vertices[static_cast<std::size_t>(row) * columns + column + 1U].position;
            horizontalSpacing += length(subtract(b, a));
            ++horizontalEdges;
        }
    }
    float verticalSpacing = 0.0F;
    std::uint64_t verticalEdges = 0U;
    for (std::uint32_t row = 0U; row + 1U < rows; ++row) {
        for (std::uint32_t column = 0U; column < columns; ++column) {
            const Float3 a = cloth.vertices[static_cast<std::size_t>(row) * columns + column].position;
            const Float3 b = cloth.vertices[static_cast<std::size_t>(row + 1U) * columns + column].position;
            verticalSpacing += length(subtract(b, a));
            ++verticalEdges;
        }
    }
    horizontalSpacing = horizontalEdges > 0U
        ? horizontalSpacing / static_cast<float>(horizontalEdges) : 1.0F;
    verticalSpacing = verticalEdges > 0U
        ? verticalSpacing / static_cast<float>(verticalEdges) : 1.0F;
    if (!(horizontalSpacing > 0.0F)) horizontalSpacing = 1.0F;
    if (!(verticalSpacing > 0.0F)) verticalSpacing = 1.0F;
    for (std::uint32_t row = 0U; row < rows; ++row) {
        for (std::uint32_t column = 0U; column < columns; ++column) {
            patch.restControlPoints[static_cast<std::size_t>(row) * columns + column] = {
                static_cast<float>(column) * horizontalSpacing,
                static_cast<float>(row) * verticalSpacing};
        }
    }
    patch.recompute_hash();
    std::string validation;
    if (!patch.validate(&validation)) {
        set_error(error, validation);
        return BSplineClothPatch{};
    }
    return patch;
}

bool update_bspline_cloth_proxy(
    BSplineClothPatch& patch,
    std::span<const Float3> controlPositions,
    std::string* error) {
    if (controlPositions.size() != patch.controlPoints.size()) {
        set_error(error, "B-spline proxy update has the wrong control-point count");
        return false;
    }
    for (const Float3 point : controlPositions) {
        if (!finite_float3(point)) {
            set_error(error, "B-spline proxy update contains a non-finite point");
            return false;
        }
    }
    std::copy(controlPositions.begin(), controlPositions.end(), patch.controlPoints.begin());
    patch.recompute_hash();
    return true;
}

bool BSplineClothRuntimeProxy::validate(std::string* error) const {
    if (!patch.validate(error) || !quadrature.validate(patch, error) ||
        !embeddedMesh.validate(patch, error)) {
        return false;
    }
    if (bendingStencils.size() != quadrature.bendingPoints.size()) {
        set_error(error, "B-spline cloth runtime proxy has incomplete bending stencils");
        return false;
    }
    return true;
}

bool BSplineClothRuntimeProxy::update(
    std::span<const Float3> controlPositions, bool evaluateEnergy,
    std::string* error) {
    if (!update_bspline_cloth_proxy(patch, controlPositions, error)) return false;
    update_bspline_cloth_embedded_mesh(patch, embeddedMesh);
    if (evaluateEnergy) lastEnergy = evaluate_bspline_cloth_energy(patch, quadrature);
    ++updateCount;
    return true;
}

BSplineClothRuntimeProxy make_bspline_cloth_runtime_proxy(
    const SoftBodyAsset& cloth, std::uint32_t columns, std::uint32_t rows,
    std::uint32_t embeddedColumns, std::uint32_t embeddedRows,
    std::string* error) {
    BSplineClothRuntimeProxy proxy;
    proxy.patch = make_bspline_cloth_proxy(
        cloth, columns, rows, embeddedColumns, embeddedRows, error);
    std::string validation;
    if (!proxy.patch.validate(&validation)) {
        if (error != nullptr && error->empty()) *error = validation;
        return {};
    }
    proxy.quadrature = build_bspline_cloth_quadrature(proxy.patch);
    proxy.bendingStencils = precompute_bspline_cloth_bending_stencils(
        proxy.patch, proxy.quadrature);
    proxy.embeddedMesh = build_bspline_cloth_embedded_mesh(
        proxy.patch, embeddedColumns, embeddedRows);
    proxy.lastEnergy = evaluate_bspline_cloth_energy(proxy.patch, proxy.quadrature);
    if (!proxy.validate(&validation)) {
        set_error(error, validation);
        return {};
    }
    return proxy;
}

} // namespace dve
