#include "dve/render/cascaded_shadow_map.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace dve::render {
namespace {
Float3 vadd(Float3 a, Float3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Float3 vsub(Float3 a, Float3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Float3 vmul(Float3 value, float scalar) noexcept {
    return {value.x * scalar, value.y * scalar, value.z * scalar};
}
float dot3(Float3 a, Float3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
Float3 cross3(Float3 a, Float3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float length3(Float3 value) noexcept { return std::sqrt(std::max(0.0F, dot3(value, value))); }
Float3 normalize3(Float3 value, Float3 fallback) noexcept {
    const float length = length3(value);
    return length > 1.0e-7F ? vmul(value, 1.0F / length) : fallback;
}
bool finite3(Float3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
float round_to(float value, float step) noexcept {
    return step > 0.0F ? std::round(value / step) * step : value;
}

struct CameraBasis {
    Float3 forward{};
    Float3 right{};
    Float3 up{};
};

CameraBasis camera_basis(const camera::CameraPose& camera) noexcept {
    CameraBasis basis;
    basis.forward = normalize3(vsub(camera.target, camera.position), {0.0F, 0.0F, -1.0F});
    basis.right = normalize3(cross3(basis.forward, camera.worldUp), {1.0F, 0.0F, 0.0F});
    basis.up = normalize3(cross3(basis.right, basis.forward), {0.0F, 1.0F, 0.0F});
    return basis;
}

std::array<Float3, 8> frustum_corners(const camera::CameraPose& camera,
                                      const CameraBasis& basis, float nearDistance,
                                      float farDistance) noexcept {
    std::array<Float3, 8> output{};
    float nearHalfHeight = 0.0F;
    float nearHalfWidth = 0.0F;
    float farHalfHeight = 0.0F;
    float farHalfWidth = 0.0F;
    if (camera.lens.projection == camera::CameraProjection::Orthographic) {
        nearHalfHeight = 0.5F * camera.lens.orthographicHeightMeters;
        farHalfHeight = nearHalfHeight;
        nearHalfWidth = nearHalfHeight * camera.lens.aspectRatio;
        farHalfWidth = nearHalfWidth;
    } else {
        const float tangent =
            std::tan(camera.lens.effective_vertical_field_of_view_radians() * 0.5F);
        nearHalfHeight = nearDistance * tangent;
        nearHalfWidth = nearHalfHeight * camera.lens.aspectRatio;
        farHalfHeight = farDistance * tangent;
        farHalfWidth = farHalfHeight * camera.lens.aspectRatio;
    }
    const Float3 nearCenter = vadd(camera.position, vmul(basis.forward, nearDistance));
    const Float3 farCenter = vadd(camera.position, vmul(basis.forward, farDistance));
    const auto set_corner = [&](std::size_t index, Float3 center, float x, float y) {
        output[index] = vadd(vadd(center, vmul(basis.right, x)), vmul(basis.up, y));
    };
    set_corner(0U, nearCenter, -nearHalfWidth, -nearHalfHeight);
    set_corner(1U, nearCenter, nearHalfWidth, -nearHalfHeight);
    set_corner(2U, nearCenter, nearHalfWidth, nearHalfHeight);
    set_corner(3U, nearCenter, -nearHalfWidth, nearHalfHeight);
    set_corner(4U, farCenter, -farHalfWidth, -farHalfHeight);
    set_corner(5U, farCenter, farHalfWidth, -farHalfHeight);
    set_corner(6U, farCenter, farHalfWidth, farHalfHeight);
    set_corner(7U, farCenter, -farHalfWidth, farHalfHeight);
    return output;
}
} // namespace

bool CascadedShadowSettings::validate(std::string* error) const noexcept {
    const auto fail = [&](const char* message) {
        if (error) *error = message;
        return false;
    };
    if (cascadeCount == 0U || cascadeCount > kMaximumShadowCascades)
        return fail("cascade count must be 1..4");
    if (cascadeResolution < 64U || cascadeResolution > 16384U)
        return fail("cascade resolution is outside 64..16384");
    if (!std::isfinite(maximumDistanceMeters) || maximumDistanceMeters <= 0.0F ||
        !std::isfinite(splitLambda) || splitLambda < 0.0F || splitLambda > 1.0F ||
        !std::isfinite(blendFraction) || blendFraction < 0.0F || blendFraction > 0.5F ||
        !std::isfinite(depthPaddingMeters) || depthPaddingMeters < 0.0F ||
        !std::isfinite(receiverBiasMeters) || receiverBiasMeters < 0.0F ||
        !std::isfinite(normalBiasMeters) || normalBiasMeters < 0.0F || pcfRadius > 4U)
        return fail("invalid cascaded shadow settings");
    return true;
}

bool CascadedShadowPlan::validate(std::string* error) const noexcept {
    if (!settings.validate(error)) return false;
    const auto fail = [&](const char* message) {
        if (error) *error = message;
        return false;
    };
    if (cascades.size() != settings.cascadeCount) return fail("cascade plan count mismatch");
    if (atlasWidth == 0U || atlasHeight == 0U)
        return fail("shadow atlas dimensions must be nonzero");
    float previousFar = 0.0F;
    for (std::size_t index = 0; index < cascades.size(); ++index) {
        const auto& cascade = cascades[index];
        if (cascade.index != index || !std::isfinite(cascade.splitNearMeters) ||
            !std::isfinite(cascade.splitFarMeters) || cascade.splitNearMeters < previousFar ||
            cascade.splitFarMeters <= cascade.splitNearMeters || !finite3(cascade.center) ||
            !finite3(cascade.snappedCenter) || !finite3(cascade.lightRight) ||
            !finite3(cascade.lightUp) || !finite3(cascade.lightForward) ||
            cascade.radiusMeters <= 0.0F || cascade.texelSizeMeters <= 0.0F ||
            cascade.atlas.width != settings.cascadeResolution ||
            cascade.atlas.height != settings.cascadeResolution)
            return fail("invalid shadow cascade");
        previousFar = cascade.splitFarMeters;
    }
    return true;
}

CascadedShadowPlan make_cascaded_shadow_plan(const camera::CameraPose& camera,
                                              Float3 directionTowardSun,
                                              const CascadedShadowSettings& settings) {
    std::string error;
    if (!camera.lens.validate(&error) || !settings.validate(&error))
        throw std::invalid_argument(error);

    CascadedShadowPlan plan;
    plan.settings = settings;
    const std::uint32_t columns = settings.cascadeCount == 1U ? 1U : 2U;
    const std::uint32_t rows = (settings.cascadeCount + columns - 1U) / columns;
    plan.atlasWidth = columns * settings.cascadeResolution;
    plan.atlasHeight = rows * settings.cascadeResolution;

    const float nearPlane = std::max(0.001F, camera.lens.nearPlaneMeters);
    const float farPlane =
        std::min(camera.lens.farPlaneMeters, settings.maximumDistanceMeters);
    if (farPlane <= nearPlane)
        throw std::invalid_argument("shadow distance must exceed camera near plane");

    std::vector<float> splits(settings.cascadeCount + 1U);
    splits[0] = nearPlane;
    for (std::uint32_t index = 1U; index <= settings.cascadeCount; ++index) {
        const float fraction =
            static_cast<float>(index) / static_cast<float>(settings.cascadeCount);
        const float logarithmic = nearPlane * std::pow(farPlane / nearPlane, fraction);
        const float uniform = nearPlane + (farPlane - nearPlane) * fraction;
        splits[index] = std::lerp(uniform, logarithmic, settings.splitLambda);
    }

    const CameraBasis basis = camera_basis(camera);
    const Float3 lightForward =
        normalize3(vmul(directionTowardSun, -1.0F), {0.0F, -1.0F, 0.0F});
    const Float3 axis = std::abs(lightForward.y) < 0.95F
                            ? Float3{0.0F, 1.0F, 0.0F}
                            : Float3{1.0F, 0.0F, 0.0F};
    const Float3 lightRight = normalize3(cross3(axis, lightForward), {1.0F, 0.0F, 0.0F});
    const Float3 lightUp =
        normalize3(cross3(lightForward, lightRight), {0.0F, 1.0F, 0.0F});

    for (std::uint32_t index = 0; index < settings.cascadeCount; ++index) {
        const auto corners = frustum_corners(camera, basis, splits[index], splits[index + 1U]);
        Float3 center{};
        for (const Float3 corner : corners) center = vadd(center, corner);
        center = vmul(center, 1.0F / 8.0F);

        float radius = 0.0F;
        float minimumDepth = std::numeric_limits<float>::max();
        float maximumDepth = -std::numeric_limits<float>::max();
        for (const Float3 corner : corners) {
            radius = std::max(radius, length3(vsub(corner, center)));
            const float depth = dot3(corner, lightForward);
            minimumDepth = std::min(minimumDepth, depth);
            maximumDepth = std::max(maximumDepth, depth);
        }
        radius = std::ceil(radius * 16.0F) / 16.0F;
        const float texelSize =
            (2.0F * radius) / static_cast<float>(settings.cascadeResolution);
        const float centerX = dot3(center, lightRight);
        const float centerY = dot3(center, lightUp);
        const float centerZ = dot3(center, lightForward);
        const Float3 snappedCenter =
            vadd(vadd(vmul(lightRight, round_to(centerX, texelSize)),
                      vmul(lightUp, round_to(centerY, texelSize))),
                 vmul(lightForward, centerZ));

        CascadedShadowCascade cascade;
        cascade.index = index;
        cascade.splitNearMeters = splits[index];
        cascade.splitFarMeters = splits[index + 1U];
        cascade.blendStartMeters = std::max(
            cascade.splitNearMeters,
            cascade.splitFarMeters -
                (cascade.splitFarMeters - cascade.splitNearMeters) * settings.blendFraction);
        cascade.center = center;
        cascade.snappedCenter = snappedCenter;
        cascade.lightRight = lightRight;
        cascade.lightUp = lightUp;
        cascade.lightForward = lightForward;
        cascade.radiusMeters = radius;
        cascade.texelSizeMeters = texelSize;
        cascade.minimumLightDepth = minimumDepth - settings.depthPaddingMeters;
        cascade.maximumLightDepth = maximumDepth + settings.depthPaddingMeters;
        cascade.atlas = {(index % columns) * settings.cascadeResolution,
                         (index / columns) * settings.cascadeResolution,
                         settings.cascadeResolution, settings.cascadeResolution};
        plan.cascades.push_back(cascade);
    }
    return plan;
}

CascadeSelection select_shadow_cascade(const CascadedShadowPlan& plan,
                                        float viewDepthMeters) noexcept {
    CascadeSelection output;
    if (plan.cascades.empty() || !std::isfinite(viewDepthMeters) || viewDepthMeters < 0.0F) {
        output.beyondShadowDistance = true;
        return output;
    }
    for (std::size_t index = 0; index < plan.cascades.size(); ++index) {
        const auto& cascade = plan.cascades[index];
        if (viewDepthMeters <= cascade.splitFarMeters) {
            output.primaryCascade = static_cast<std::uint32_t>(index);
            output.secondaryCascade = output.primaryCascade;
            if (index + 1U < plan.cascades.size() &&
                viewDepthMeters > cascade.blendStartMeters) {
                output.secondaryCascade = static_cast<std::uint32_t>(index + 1U);
                output.secondaryWeight = std::clamp(
                    (viewDepthMeters - cascade.blendStartMeters) /
                        std::max(1.0e-6F,
                                 cascade.splitFarMeters - cascade.blendStartMeters),
                    0.0F, 1.0F);
            }
            return output;
        }
    }
    output.primaryCascade = static_cast<std::uint32_t>(plan.cascades.size() - 1U);
    output.secondaryCascade = output.primaryCascade;
    output.beyondShadowDistance = true;
    return output;
}

std::vector<std::uint32_t> shadow_cascades_intersecting_sphere(
    const CascadedShadowPlan& plan, Float3 center, float radiusMeters) {
    std::vector<std::uint32_t> output;
    radiusMeters = std::max(0.0F, radiusMeters);
    for (const auto& cascade : plan.cascades) {
        const Float3 delta = vsub(center, cascade.snappedCenter);
        const float x = std::abs(dot3(delta, cascade.lightRight));
        const float y = std::abs(dot3(delta, cascade.lightUp));
        const float z = dot3(center, cascade.lightForward);
        if (x <= cascade.radiusMeters + radiusMeters &&
            y <= cascade.radiusMeters + radiusMeters &&
            z + radiusMeters >= cascade.minimumLightDepth &&
            z - radiusMeters <= cascade.maximumLightDepth)
            output.push_back(cascade.index);
    }
    return output;
}

Float3 cascaded_shadow_caster_clip_coordinate(
    const CascadedShadowCascade& cascade, Float3 worldPosition) noexcept {
    const Float3 delta = vsub(worldPosition, cascade.snappedCenter);
    const float radius = std::max(cascade.radiusMeters, 1.0e-6F);
    const float depthRange =
        std::max(1.0e-6F, cascade.maximumLightDepth - cascade.minimumLightDepth);
    const float depth =
        (dot3(worldPosition, cascade.lightForward) - cascade.minimumLightDepth) / depthRange;
    return {dot3(delta, cascade.lightRight) / radius,
            -dot3(delta, cascade.lightUp) / radius,
            std::max(depth, 0.0F)};
}

ShadowAtlasUvBounds cascaded_shadow_atlas_uv_bounds(const CascadedShadowPlan& plan,
                                                     std::uint32_t cascadeIndex) noexcept {
    if (cascadeIndex >= plan.cascades.size() || plan.atlasWidth == 0U ||
        plan.atlasHeight == 0U)
        return {-1.0F, -1.0F, -1.0F, -1.0F};
    const auto& atlas = plan.cascades[cascadeIndex].atlas;
    return {(static_cast<float>(atlas.x) + 0.5F) / static_cast<float>(plan.atlasWidth),
            (static_cast<float>(atlas.y) + 0.5F) / static_cast<float>(plan.atlasHeight),
            (static_cast<float>(atlas.x + atlas.width) - 0.5F) /
                static_cast<float>(plan.atlasWidth),
            (static_cast<float>(atlas.y + atlas.height) - 0.5F) /
                static_cast<float>(plan.atlasHeight)};
}

Float3 cascaded_shadow_atlas_coordinate(const CascadedShadowPlan& plan,
                                        std::uint32_t cascadeIndex,
                                        Float3 worldPosition) noexcept {
    return cascaded_shadow_atlas_coordinate(plan, cascadeIndex, worldPosition, {});
}

Float3 cascaded_shadow_atlas_coordinate(const CascadedShadowPlan& plan,
                                        std::uint32_t cascadeIndex, Float3 worldPosition,
                                        Float3 geometricNormal) noexcept {
    if (cascadeIndex >= plan.cascades.size() || plan.atlasWidth == 0U ||
        plan.atlasHeight == 0U)
        return {-1.0F, -1.0F, 2.0F};
    const auto& cascade = plan.cascades[cascadeIndex];
    if (length3(geometricNormal) > 1.0e-7F) {
        Float3 normal = normalize3(geometricNormal, {0.0F, 0.0F, 0.0F});
        const Float3 towardLight = vmul(cascade.lightForward, -1.0F);
        if (dot3(normal, towardLight) < 0.0F) normal = vmul(normal, -1.0F);
        worldPosition = vadd(worldPosition, vmul(normal, plan.settings.normalBiasMeters));
    }
    const Float3 delta = vsub(worldPosition, cascade.snappedCenter);
    const float localU =
        0.5F + 0.5F * dot3(delta, cascade.lightRight) / cascade.radiusMeters;
    const float localV =
        0.5F - 0.5F * dot3(delta, cascade.lightUp) / cascade.radiusMeters;
    if (localU < 0.0F || localU > 1.0F || localV < 0.0F || localV > 1.0F)
        return {-1.0F, -1.0F, 2.0F};
    const float depthRange =
        std::max(1.0e-6F, cascade.maximumLightDepth - cascade.minimumLightDepth);
    const float depth =
        (dot3(worldPosition, cascade.lightForward) - cascade.minimumLightDepth -
         plan.settings.receiverBiasMeters) /
        depthRange;
    return {(static_cast<float>(cascade.atlas.x) +
             localU * static_cast<float>(cascade.atlas.width)) /
                static_cast<float>(plan.atlasWidth),
            (static_cast<float>(cascade.atlas.y) +
             localV * static_cast<float>(cascade.atlas.height)) /
                static_cast<float>(plan.atlasHeight),
            depth};
}
} // namespace dve::render
