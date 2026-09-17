#include "dve/render/cpu_hair_renderer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace dve::render {
namespace {

constexpr float kEpsilon = 1.0e-7F;
constexpr float kGoldenAngle = 2.39996322972865332F;

[[nodiscard]] bool finite(Float3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] bool finite(Float4 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z) && std::isfinite(value.w);
}

[[nodiscard]] Float3 cross(Float3 a, Float3 b) noexcept {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

[[nodiscard]] Float3 normalized(Float3 value, Float3 fallback) noexcept {
    const float squared = length_squared(value);
    if (!(squared > 1.0e-14F) || !std::isfinite(squared)) return fallback;
    return multiply(value, 1.0F / std::sqrt(squared));
}

[[nodiscard]] Float4 lerp(Float4 a, Float4 b, float amount) noexcept {
    return {a.x + (b.x - a.x) * amount,
            a.y + (b.y - a.y) * amount,
            a.z + (b.z - a.z) * amount,
            a.w + (b.w - a.w) * amount};
}

[[nodiscard]] Float3 component_multiply(Float3 a, Float3 b) noexcept {
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}

[[nodiscard]] std::uint64_t mix64(std::uint64_t value) noexcept {
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31U;
    return value;
}

[[nodiscard]] float unit_float(std::uint64_t value) noexcept {
    const std::uint32_t bits = static_cast<std::uint32_t>(mix64(value) >> 40U);
    return static_cast<float>(bits) / static_cast<float>(0x00FFFFFFU);
}

void hash_append(std::uint64_t& hash, std::uint64_t value) noexcept {
    hash ^= mix64(value + 0x9e3779b97f4a7c15ULL + (hash << 6U) + (hash >> 2U));
}

[[nodiscard]] std::uint32_t sampled_point_index(std::uint32_t sample,
                                                 std::uint32_t sampleCount,
                                                 std::uint32_t sourceCount) noexcept {
    if (sampleCount >= sourceCount) return sample;
    const std::uint64_t numerator = static_cast<std::uint64_t>(sample) *
                                    static_cast<std::uint64_t>(sourceCount - 1U);
    return static_cast<std::uint32_t>(numerator /
                                      static_cast<std::uint64_t>(sampleCount - 1U));
}

[[nodiscard]] Float3 stable_perpendicular(Float3 tangent, std::uint64_t seed) noexcept {
    const std::array<Float3, 3> axes{{{1.0F, 0.0F, 0.0F},
                                      {0.0F, 1.0F, 0.0F},
                                      {0.0F, 0.0F, 1.0F}}};
    const std::size_t start = static_cast<std::size_t>(seed % axes.size());
    for (std::size_t offset = 0U; offset < axes.size(); ++offset) {
        const Float3 candidate = cross(tangent, axes[(start + offset) % axes.size()]);
        if (length_squared(candidate) > 1.0e-10F) return normalized(candidate, {1.0F, 0.0F, 0.0F});
    }
    return {1.0F, 0.0F, 0.0F};
}

[[nodiscard]] Float3 centerline_tangent(std::span<const Float3> positions,
                                         const HairStrandRange& strand,
                                         std::uint32_t sourceLocal,
                                         std::uint64_t seed,
                                         std::uint64_t& degenerateCount) noexcept {
    Float3 tangent{};
    if (sourceLocal == 0U) {
        tangent = subtract(positions[strand.firstPoint + 1U], positions[strand.firstPoint]);
    } else if (sourceLocal + 1U >= strand.pointCount) {
        tangent = subtract(positions[strand.firstPoint + sourceLocal],
                           positions[strand.firstPoint + sourceLocal - 1U]);
    } else {
        tangent = subtract(positions[strand.firstPoint + sourceLocal + 1U],
                           positions[strand.firstPoint + sourceLocal - 1U]);
    }
    if (length_squared(tangent) <= 1.0e-14F || !finite(tangent)) {
        ++degenerateCount;
        return stable_perpendicular({0.0F, 0.0F, 1.0F}, seed);
    }
    return normalized(tangent, {0.0F, -1.0F, 0.0F});
}

[[nodiscard]] std::uint32_t selected_guide_index(std::uint32_t selected,
                                                  std::uint32_t selectedCount,
                                                  std::uint32_t sourceCount) noexcept {
    if (selectedCount >= sourceCount) return selected;
    if (selectedCount == 1U) return sourceCount / 2U;
    const std::uint64_t numerator = static_cast<std::uint64_t>(selected) *
                                    static_cast<std::uint64_t>(sourceCount - 1U);
    return static_cast<std::uint32_t>(numerator /
                                      static_cast<std::uint64_t>(selectedCount - 1U));
}

[[nodiscard]] float edge(float ax, float ay, float bx, float by, float px, float py) noexcept {
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

[[nodiscard]] Float3 clamp_nonnegative(Float3 value) noexcept {
    return {std::max(0.0F, value.x), std::max(0.0F, value.y), std::max(0.0F, value.z)};
}

} // namespace

bool validate_cpu_hair_ribbon_settings(const CpuHairRibbonBuildSettings& settings,
                                       std::string* error) noexcept {
    const bool valid = settings.visibleStrandsPerGuide >= 1U &&
        settings.visibleStrandsPerGuide <= 64U &&
        (settings.maximumPointsPerStrand == 0U || settings.maximumPointsPerStrand >= 2U) &&
        std::isfinite(settings.rootSpreadMeters) && settings.rootSpreadMeters >= 0.0F &&
        std::isfinite(settings.tipSpreadMeters) && settings.tipSpreadMeters >= 0.0F &&
        std::isfinite(settings.rootWidthMeters) && settings.rootWidthMeters > 0.0F &&
        std::isfinite(settings.tipWidthMeters) && settings.tipWidthMeters > 0.0F &&
        std::isfinite(settings.spreadExponent) && settings.spreadExponent > 0.0F &&
        std::isfinite(settings.longitudinalJitterMeters) &&
        settings.longitudinalJitterMeters >= 0.0F &&
        finite(settings.rootColor) && finite(settings.tipColor) &&
        settings.rootColor.x >= 0.0F && settings.rootColor.y >= 0.0F &&
        settings.rootColor.z >= 0.0F && settings.rootColor.w >= 0.0F &&
        settings.rootColor.w <= 1.0F && settings.tipColor.x >= 0.0F &&
        settings.tipColor.y >= 0.0F && settings.tipColor.z >= 0.0F &&
        settings.tipColor.w >= 0.0F && settings.tipColor.w <= 1.0F;
    if (!valid && error != nullptr) *error = "invalid CPU hair ribbon settings";
    return valid;
}


std::size_t default_cpu_hair_ribbon_worker_count() noexcept {
    return std::min<std::size_t>(4U, JobSystem::default_worker_count());
}

CpuHairRibbonExpander::CpuHairRibbonExpander(std::size_t workerCount)
    : jobs_(workerCount) {}

void CpuHairRibbonExpander::clear() noexcept {
    vertices_.clear();
    indices_.clear();
    triangleLayerIds_.clear();
    strands_.clear();
    work_.clear();
    telemetry_ = {};
    boundsMinimum_ = {};
    boundsMaximum_ = {};
    sourceSimulationFrame_ = 0U;
    topologyHash_ = 0U;
    visible_ = false;
}

bool CpuHairRibbonExpander::build(const CpuHairView& sourceView,
                                  const HairAsset& asset,
                                  const PolygonCamera& camera,
                                  const CpuHairRibbonBuildSettings& settings,
                                  std::string* error) {
    clear();
    if (!validate_cpu_hair_ribbon_settings(settings, error)) return false;
    if (!asset.validate(error)) return false;
    if (sourceView.positions.size() != asset.point_count() ||
        sourceView.strands.size() != asset.guides.size()) {
        if (error != nullptr) *error = "CPU hair ribbon source view does not match its asset";
        return false;
    }
    if (!finite(camera.position) || !finite(camera.target) || !finite(camera.up)) {
        if (error != nullptr) *error = "CPU hair ribbon camera is not finite";
        return false;
    }
    telemetry_.sourceGuides = sourceView.strands.size();
    sourceSimulationFrame_ = sourceView.simulationFrame;
    visible_ = sourceView.visible;
    if (!sourceView.visible || sourceView.strands.empty()) return true;

    const std::uint32_t sourceGuideCount = static_cast<std::uint32_t>(sourceView.strands.size());
    const std::uint32_t selectedGuideCount = settings.maximumVisibleGuides == 0U
        ? sourceGuideCount
        : std::min(sourceGuideCount, settings.maximumVisibleGuides);
    telemetry_.selectedGuides = selectedGuideCount;
    telemetry_.guideBudgetDrops = sourceGuideCount - selectedGuideCount;

    const std::uint64_t requestedStrands = static_cast<std::uint64_t>(selectedGuideCount) *
                                           settings.visibleStrandsPerGuide;
    const std::uint64_t visibleStrandBudget = settings.maximumVisibleStrands == 0U
        ? requestedStrands
        : std::min(requestedStrands,
                   static_cast<std::uint64_t>(settings.maximumVisibleStrands));
    telemetry_.strandBudgetDrops = requestedStrands - visibleStrandBudget;

    constexpr std::uint64_t maximumRibbonStrands = 1ULL * 1024ULL * 1024ULL;
    constexpr std::uint64_t maximumRibbonVertices = 16ULL * 1024ULL * 1024ULL;
    constexpr std::uint64_t maximumRibbonIndices = 96ULL * 1024ULL * 1024ULL;
    if (visibleStrandBudget > maximumRibbonStrands) {
        if (error != nullptr) {
            *error = "CPU hair ribbon expansion exceeds the bounded strand count; lower visible guide or strand budgets";
        }
        clear();
        return false;
    }
    work_.reserve(static_cast<std::size_t>(visibleStrandBudget));
    std::uint64_t totalVertices = 0U;
    std::uint64_t totalIndices = 0U;
    std::uint64_t totalTriangles = 0U;
    std::uint64_t emittedStrands = 0U;
    std::uint64_t topologyHash = 0xcbf29ce484222325ULL;
    hash_append(topologyHash, asset.contentHash);
    hash_append(topologyHash, settings.visibleStrandsPerGuide);
    hash_append(topologyHash, settings.maximumVisibleGuides);
    hash_append(topologyHash, settings.maximumVisibleStrands);
    hash_append(topologyHash, settings.maximumPointsPerStrand);
    hash_append(topologyHash, settings.seed);
    hash_append(topologyHash, settings.includeGuideCenterline ? 1U : 0U);

    for (std::uint32_t selected = 0U;
         selected < selectedGuideCount && emittedStrands < visibleStrandBudget;
         ++selected) {
        const std::uint32_t guideIndex = selected_guide_index(selected, selectedGuideCount,
                                                              sourceGuideCount);
        const HairStrandRange& guide = sourceView.strands[guideIndex];
        if (guide.pointCount < 2U) continue;
        const std::uint32_t sampleCount = settings.maximumPointsPerStrand == 0U
            ? guide.pointCount
            : std::min(guide.pointCount, settings.maximumPointsPerStrand);
        hash_append(topologyHash, guideIndex);
        hash_append(topologyHash, sampleCount);
        for (std::uint32_t child = 0U;
             child < settings.visibleStrandsPerGuide && emittedStrands < visibleStrandBudget;
             ++child) {
            const std::uint64_t strandVertices = static_cast<std::uint64_t>(sampleCount) * 2U;
            const std::uint64_t strandIndices = static_cast<std::uint64_t>(sampleCount - 1U) * 6U;
            const std::uint64_t strandTriangles = static_cast<std::uint64_t>(sampleCount - 1U) * 2U;
            if (totalVertices + strandVertices > maximumRibbonVertices ||
                totalIndices + strandIndices > maximumRibbonIndices ||
                totalVertices + strandVertices > std::numeric_limits<std::uint32_t>::max() ||
                totalIndices + strandIndices > std::numeric_limits<std::uint32_t>::max() ||
                totalTriangles + strandTriangles > std::numeric_limits<std::uint32_t>::max()) {
                if (error != nullptr) {
                    *error = "CPU hair ribbon expansion exceeds the bounded packet size; lower visible guide, strand, or point budgets";
                }
                clear();
                return false;
            }
            BuildWorkItem work;
            work.guideIndex = guideIndex;
            work.childIndex = child;
            work.sampleCount = sampleCount;
            work.firstVertex = static_cast<std::uint32_t>(totalVertices);
            work.firstIndex = static_cast<std::uint32_t>(totalIndices);
            work.firstTriangle = static_cast<std::uint32_t>(totalTriangles);
            work.layerId = guide.layerId;
            work_.push_back(work);
            totalVertices += strandVertices;
            totalIndices += strandIndices;
            totalTriangles += strandTriangles;
            ++emittedStrands;
            hash_append(topologyHash, child);
        }
    }

    vertices_.resize(static_cast<std::size_t>(totalVertices));
    indices_.resize(static_cast<std::size_t>(totalIndices));
    triangleLayerIds_.resize(static_cast<std::size_t>(totalTriangles));
    strands_.resize(work_.size());

    constexpr std::size_t workItemsPerBatch = 32U;
    const std::size_t workBatchCount =
        (work_.size() + workItemsPerBatch - 1U) / workItemsPerBatch;
    telemetry_.workerBatches = workBatchCount;
    jobs_.parallel_for(workBatchCount, [&](std::size_t batchIndex) {
        const std::size_t firstWork = batchIndex * workItemsPerBatch;
        const std::size_t endWork = std::min(work_.size(), firstWork + workItemsPerBatch);
        for (std::size_t workIndex = firstWork; workIndex < endWork; ++workIndex) {
        BuildWorkItem& work = work_[workIndex];
        const HairStrandRange& guide = sourceView.strands[work.guideIndex];
        const std::uint64_t guideSeed = mix64(settings.seed ^
            (static_cast<std::uint64_t>(work.guideIndex) * 0x9e3779b97f4a7c15ULL));
        const Float3 rootTangent = centerline_tangent(sourceView.positions, guide, 0U,
                                                       guideSeed, work.degenerateTangents);
        const Float3 frameU = stable_perpendicular(rootTangent, guideSeed);
        const Float3 frameV = normalized(cross(rootTangent, frameU), {0.0F, 0.0F, 1.0F});
        const bool centerline = settings.includeGuideCenterline && work.childIndex == 0U;
        const std::uint32_t offsetIndex = settings.includeGuideCenterline
            ? (work.childIndex == 0U ? 0U : work.childIndex - 1U)
            : work.childIndex;
        const std::uint32_t offsetCount = settings.includeGuideCenterline
            ? settings.visibleStrandsPerGuide - 1U
            : settings.visibleStrandsPerGuide;
        float diskRadius = 0.0F;
        float angle = 0.0F;
        if (!centerline && offsetCount > 0U) {
            diskRadius = std::sqrt((static_cast<float>(offsetIndex) + 0.5F) /
                                   static_cast<float>(offsetCount));
            angle = kGoldenAngle * static_cast<float>(offsetIndex) +
                    6.28318530718F * unit_float(guideSeed);
        }
        const Float3 baseOffset = add(multiply(frameU, std::cos(angle)),
                                      multiply(frameV, std::sin(angle)));
        const float phase = 6.28318530718F * unit_float(guideSeed ^
            (static_cast<std::uint64_t>(work.childIndex) * 0xd1b54a32d192ed03ULL));

        CpuHairRibbonRange range;
        range.firstVertex = work.firstVertex;
        range.vertexCount = work.sampleCount * 2U;
        range.firstIndex = work.firstIndex;
        range.indexCount = (work.sampleCount - 1U) * 6U;
        range.sourceGuide = work.guideIndex;
        range.childIndex = work.childIndex;
        range.layerId = work.layerId;
        strands_[workIndex] = range;

        for (std::uint32_t sample = 0U; sample < work.sampleCount; ++sample) {
            const std::uint32_t sourceLocal = sampled_point_index(sample, work.sampleCount,
                                                                  guide.pointCount);
            const Float3 guidePosition = sourceView.positions[guide.firstPoint + sourceLocal];
            const Float3 tangent = centerline_tangent(sourceView.positions, guide, sourceLocal,
                guideSeed ^ sourceLocal, work.degenerateTangents);
            const float normalizedLength = static_cast<float>(sample) /
                                           static_cast<float>(work.sampleCount - 1U);
            const float spreadAmount = std::pow(normalizedLength, settings.spreadExponent);
            const float spread = settings.rootSpreadMeters +
                (settings.tipSpreadMeters - settings.rootSpreadMeters) * spreadAmount;
            Float3 offsetDirection = subtract(baseOffset,
                multiply(tangent, dot(baseOffset, tangent)));
            offsetDirection = normalized(offsetDirection,
                stable_perpendicular(tangent, guideSeed ^ work.childIndex ^ sourceLocal));
            const float jitter = centerline ? 0.0F :
                settings.longitudinalJitterMeters * normalizedLength *
                std::sin(phase + normalizedLength * 12.5663706144F);
            const Float3 center = add(guidePosition,
                multiply(offsetDirection, centerline ? 0.0F : diskRadius * spread + jitter));

            const Float3 towardCamera = subtract(camera.position, center);
            Float3 side = cross(tangent, towardCamera);
            if (length_squared(side) <= 1.0e-12F || !finite(side)) {
                ++work.cameraParallelFallbacks;
                side = cross(tangent, camera.up);
            }
            side = normalized(side, stable_perpendicular(tangent, guideSeed ^ sample));
            const float width = settings.rootWidthMeters +
                (settings.tipWidthMeters - settings.rootWidthMeters) * normalizedLength;
            const Float3 halfSide = multiply(side, 0.5F * width);
            const Float4 color = lerp(settings.rootColor, settings.tipColor, normalizedLength);
            const std::uint32_t baseVertex = work.firstVertex + sample * 2U;
            vertices_[baseVertex] = {subtract(center, halfSide), tangent,
                                     {0.0F, normalizedLength}, color};
            vertices_[baseVertex + 1U] = {add(center, halfSide), tangent,
                                          {1.0F, normalizedLength}, color};

            for (const Float3 position : {vertices_[baseVertex].position,
                                          vertices_[baseVertex + 1U].position}) {
                if (!work.boundsInitialized) {
                    work.boundsMinimum = position;
                    work.boundsMaximum = position;
                    work.boundsInitialized = true;
                } else {
                    work.boundsMinimum.x = std::min(work.boundsMinimum.x, position.x);
                    work.boundsMinimum.y = std::min(work.boundsMinimum.y, position.y);
                    work.boundsMinimum.z = std::min(work.boundsMinimum.z, position.z);
                    work.boundsMaximum.x = std::max(work.boundsMaximum.x, position.x);
                    work.boundsMaximum.y = std::max(work.boundsMaximum.y, position.y);
                    work.boundsMaximum.z = std::max(work.boundsMaximum.z, position.z);
                }
            }
            if (sample > 0U) {
                const std::uint32_t previous = baseVertex - 2U;
                const std::uint32_t index = work.firstIndex + (sample - 1U) * 6U;
                indices_[index] = previous;
                indices_[index + 1U] = previous + 1U;
                indices_[index + 2U] = baseVertex;
                indices_[index + 3U] = baseVertex;
                indices_[index + 4U] = previous + 1U;
                indices_[index + 5U] = baseVertex + 1U;
                const std::uint32_t triangle = work.firstTriangle + (sample - 1U) * 2U;
                triangleLayerIds_[triangle] = work.layerId;
                triangleLayerIds_[triangle + 1U] = work.layerId;
            }
        }
        }
    });

    bool boundsInitialized = false;
    for (const BuildWorkItem& work : work_) {
        telemetry_.degenerateTangents += work.degenerateTangents;
        telemetry_.cameraParallelFallbacks += work.cameraParallelFallbacks;
        telemetry_.sampledCenterlinePoints += work.sampleCount;
        if (!work.boundsInitialized) continue;
        if (!boundsInitialized) {
            boundsMinimum_ = work.boundsMinimum;
            boundsMaximum_ = work.boundsMaximum;
            boundsInitialized = true;
        } else {
            boundsMinimum_.x = std::min(boundsMinimum_.x, work.boundsMinimum.x);
            boundsMinimum_.y = std::min(boundsMinimum_.y, work.boundsMinimum.y);
            boundsMinimum_.z = std::min(boundsMinimum_.z, work.boundsMinimum.z);
            boundsMaximum_.x = std::max(boundsMaximum_.x, work.boundsMaximum.x);
            boundsMaximum_.y = std::max(boundsMaximum_.y, work.boundsMaximum.y);
            boundsMaximum_.z = std::max(boundsMaximum_.z, work.boundsMaximum.z);
        }
    }
    telemetry_.visibleStrands = strands_.size();
    telemetry_.ribbonVertices = vertices_.size();
    telemetry_.ribbonTriangles = indices_.size() / 3U;
    topologyHash_ = topologyHash;
    return true;
}

bool CpuHairRibbonExpander::build(const CpuHairRuntime& runtime,
                                  CpuHairOwnerId owner,
                                  const PolygonCamera& camera,
                                  const CpuHairRibbonBuildSettings& settings,
                                  std::string* error) {
    const HairAsset* ownerAsset = runtime.asset(owner);
    if (ownerAsset == nullptr) {
        if (error != nullptr) *error = "CPU hair render owner is not bound";
        clear();
        return false;
    }
    return build(runtime.view(owner), *ownerAsset, camera, settings, error);
}

CpuHairRibbonPacketView CpuHairRibbonExpander::view() const noexcept {
    return {vertices_, indices_, triangleLayerIds_, strands_, boundsMinimum_, boundsMaximum_,
            sourceSimulationFrame_, topologyHash_, visible_};
}

float estimate_cpu_hair_projected_diameter_pixels(
    const CpuHairView& sourceView,
    const PolygonCamera& camera,
    std::uint32_t viewportHeightPixels) noexcept {
    if (!sourceView.visible || sourceView.positions.empty() || viewportHeightPixels == 0U ||
        !finite(camera.position) || !finite(camera.target) ||
        !(camera.verticalFieldOfViewRadians > 0.0F) ||
        !(camera.verticalFieldOfViewRadians < 3.14159265359F)) return 0.0F;
    bool initialized = false;
    Float3 minimum{};
    Float3 maximum{};
    for (const Float3 position : sourceView.positions) {
        if (!finite(position)) continue;
        if (!initialized) {
            minimum = position;
            maximum = position;
            initialized = true;
        } else {
            minimum.x = std::min(minimum.x, position.x);
            minimum.y = std::min(minimum.y, position.y);
            minimum.z = std::min(minimum.z, position.z);
            maximum.x = std::max(maximum.x, position.x);
            maximum.y = std::max(maximum.y, position.y);
            maximum.z = std::max(maximum.z, position.z);
        }
    }
    if (!initialized) return 0.0F;
    const Float3 center = multiply(add(minimum, maximum), 0.5F);
    float radiusSquared = 0.0F;
    for (const Float3 position : sourceView.positions) {
        if (finite(position)) radiusSquared = std::max(radiusSquared,
            length_squared(subtract(position, center)));
    }
    const float radius = std::sqrt(std::max(0.0F, radiusSquared));
    if (!(radius > 0.0F)) return 0.0F;
    const Float3 forward = normalized(subtract(camera.target, camera.position),
                                      {0.0F, 0.0F, -1.0F});
    const float centerDepth = dot(subtract(center, camera.position), forward);
    const float closestDepth = centerDepth - radius;
    const float tanHalf = std::tan(camera.verticalFieldOfViewRadians * 0.5F);
    if (!(tanHalf > 0.0F) || !std::isfinite(tanHalf)) return 0.0F;
    if (closestDepth <= std::max(0.001F, camera.nearPlane))
        return static_cast<float>(viewportHeightPixels) * 4.0F;
    const float pixels = radius * static_cast<float>(viewportHeightPixels) /
                         (centerDepth * tanHalf);
    return std::isfinite(pixels) ? std::max(0.0F, pixels) : 0.0F;
}

CpuHairRibbonLodSelection select_cpu_hair_ribbon_lod(
    float projectedDiameterPixels,
    const CpuHairRibbonBuildSettings& nearSettings,
    const CpuHairRibbonLodPolicy& policy) noexcept {
    CpuHairRibbonLodSelection selection;
    selection.settings = nearSettings;
    if (!std::isfinite(projectedDiameterPixels) || projectedDiameterPixels < 0.0F ||
        !(policy.cullBelowProjectedPixels >= 0.0F) ||
        !(policy.farProjectedPixels >= policy.cullBelowProjectedPixels) ||
        !(policy.mediumProjectedPixels >= policy.farProjectedPixels) ||
        !(policy.nearProjectedPixels >= policy.mediumProjectedPixels)) {
        return selection;
    }
    const auto clamp_budget = [](std::uint32_t authored, std::uint32_t limit) noexcept {
        return authored == 0U ? limit : std::min(authored, limit);
    };
    if (projectedDiameterPixels < policy.cullBelowProjectedPixels) return selection;
    selection.visible = true;
    if (projectedDiameterPixels >= policy.nearProjectedPixels) {
        selection.tier = CpuHairRibbonLodTier::Near;
        return selection;
    }
    if (projectedDiameterPixels >= policy.mediumProjectedPixels) {
        selection.tier = CpuHairRibbonLodTier::Medium;
        selection.settings.visibleStrandsPerGuide =
            std::min(selection.settings.visibleStrandsPerGuide, 4U);
        selection.settings.maximumVisibleGuides =
            clamp_budget(selection.settings.maximumVisibleGuides, 1536U);
        selection.settings.maximumVisibleStrands =
            clamp_budget(selection.settings.maximumVisibleStrands, 6144U);
        selection.settings.maximumPointsPerStrand =
            clamp_budget(selection.settings.maximumPointsPerStrand, 12U);
        return selection;
    }
    if (projectedDiameterPixels >= policy.farProjectedPixels) {
        selection.tier = CpuHairRibbonLodTier::Far;
        selection.settings.visibleStrandsPerGuide =
            std::min(selection.settings.visibleStrandsPerGuide, 2U);
        selection.settings.maximumVisibleGuides =
            clamp_budget(selection.settings.maximumVisibleGuides, 768U);
        selection.settings.maximumVisibleStrands =
            clamp_budget(selection.settings.maximumVisibleStrands, 1536U);
        selection.settings.maximumPointsPerStrand =
            clamp_budget(selection.settings.maximumPointsPerStrand, 8U);
        return selection;
    }
    selection.tier = CpuHairRibbonLodTier::Distant;
    selection.settings.visibleStrandsPerGuide = 1U;
    selection.settings.maximumVisibleGuides =
        clamp_budget(selection.settings.maximumVisibleGuides, 384U);
    selection.settings.maximumVisibleStrands =
        clamp_budget(selection.settings.maximumVisibleStrands, 384U);
    selection.settings.maximumPointsPerStrand =
        clamp_budget(selection.settings.maximumPointsPerStrand, 6U);
    return selection;
}

CpuHairReferenceRenderStats ReferenceCpuHairRenderer::render(
    const CpuHairRibbonPacketView& packet,
    std::uint64_t objectId,
    const PolygonCamera& camera,
    const RenderEnvironment& environment,
    PolygonRenderTarget& target,
    const CpuHairReferenceRenderOptions& options) {
    CpuHairReferenceRenderStats stats;
    stats.submittedStrands = packet.strands.size();
    stats.submittedTriangles = packet.indices.size() / 3U;
    if (!packet.visible || packet.vertices.empty() || !target.valid() ||
        !(camera.nearPlane > 0.0F) || !(camera.farPlane > camera.nearPlane) ||
        !(camera.verticalFieldOfViewRadians > 0.0F)) return stats;
    const float alphaCutoff = std::clamp(std::isfinite(options.alphaCutoff)
        ? options.alphaCutoff : 0.015F, 0.0F, 1.0F);
    const float specularStrength = std::max(0.0F, std::isfinite(options.specularStrength)
        ? options.specularStrength : 0.35F);
    const float specularExponent = std::max(1.0F, std::isfinite(options.specularExponent)
        ? options.specularExponent : 36.0F);

    const Float3 forward = normalized(subtract(camera.target, camera.position),
                                      {0.0F, 0.0F, -1.0F});
    Float3 right = normalized(cross(forward, camera.up), {1.0F, 0.0F, 0.0F});
    if (length_squared(right) <= kEpsilon) right = {1.0F, 0.0F, 0.0F};
    const Float3 up = normalized(cross(right, forward), {0.0F, 1.0F, 0.0F});
    const float tanHalf = std::tan(camera.verticalFieldOfViewRadians * 0.5F);
    const float aspect = static_cast<float>(target.width) / static_cast<float>(target.height);

    screenVertices_.resize(packet.vertices.size());
    projected_.assign(packet.vertices.size(), 0U);
    for (std::size_t index = 0U; index < packet.vertices.size(); ++index) {
        const CpuHairRibbonVertex& input = packet.vertices[index];
        const Float3 relative = subtract(input.position, camera.position);
        const float cx = dot(relative, right);
        const float cy = dot(relative, up);
        const float cz = dot(relative, forward);
        if (!(cz > camera.nearPlane && cz < camera.farPlane) || !std::isfinite(cz)) continue;
        const float ndcX = cx / (cz * tanHalf * aspect);
        const float ndcY = cy / (cz * tanHalf);
        ScreenVertex& output = screenVertices_[index];
        output.x = (ndcX * 0.5F + 0.5F) * static_cast<float>(target.width);
        output.y = (0.5F - ndcY * 0.5F) * static_cast<float>(target.height);
        output.depth = (cz - camera.nearPlane) / (camera.farPlane - camera.nearPlane);
        output.cameraZ = cz;
        output.invZ = 1.0F / cz;
        output.worldPosition = input.position;
        output.tangent = input.tangent;
        output.color = input.color;
        if (std::isfinite(output.x) && std::isfinite(output.y) && std::isfinite(output.depth))
            projected_[index] = 1U;
    }

    triangleOrder_.clear();
    triangleOrder_.reserve(packet.indices.size() / 3U);
    for (std::uint32_t triangle = 0U;
         triangle < static_cast<std::uint32_t>(packet.indices.size() / 3U); ++triangle) {
        const std::uint32_t ia = packet.indices[static_cast<std::size_t>(triangle) * 3U];
        const std::uint32_t ib = packet.indices[static_cast<std::size_t>(triangle) * 3U + 1U];
        const std::uint32_t ic = packet.indices[static_cast<std::size_t>(triangle) * 3U + 2U];
        if (ia >= screenVertices_.size() || ib >= screenVertices_.size() ||
            ic >= screenVertices_.size() || projected_[ia] == 0U ||
            projected_[ib] == 0U || projected_[ic] == 0U) {
            ++stats.rejectedTriangles;
            continue;
        }
        const ScreenVertex& a = screenVertices_[ia];
        const ScreenVertex& b = screenVertices_[ib];
        const ScreenVertex& c = screenVertices_[ic];
        const float area = edge(a.x, a.y, b.x, b.y, c.x, c.y);
        if (std::abs(area) <= kEpsilon || !std::isfinite(area)) {
            ++stats.rejectedTriangles;
            continue;
        }
        triangleOrder_.push_back({triangle, (a.cameraZ + b.cameraZ + c.cameraZ) / 3.0F});
    }
    std::stable_sort(triangleOrder_.begin(), triangleOrder_.end(),
        [](const TriangleOrder& a, const TriangleOrder& b) { return a.cameraZ > b.cameraZ; });

    nearestHairDepth_.resize(static_cast<std::size_t>(target.width) * target.height);
    if (options.preserveExistingDepth) {
        nearestHairDepth_ = target.depth;
    } else {
        std::fill(nearestHairDepth_.begin(), nearestHairDepth_.end(), 1.0F);
    }

    const Float3 towardSun = normalized(environment.sunDirection, {0.0F, 1.0F, 0.0F});
    const Float3 ambient = component_multiply(
        add(multiply(environment.skyColor, 0.32F),
            multiply(environment.groundColor, 0.12F)), environment.globalTint);

    for (const TriangleOrder& ordered : triangleOrder_) {
        const std::size_t first = static_cast<std::size_t>(ordered.triangle) * 3U;
        const std::uint32_t ia = packet.indices[first];
        const std::uint32_t ib = packet.indices[first + 1U];
        const std::uint32_t ic = packet.indices[first + 2U];
        const ScreenVertex& a = screenVertices_[ia];
        const ScreenVertex& b = screenVertices_[ib];
        const ScreenVertex& c = screenVertices_[ic];
        const float signedArea = edge(a.x, a.y, b.x, b.y, c.x, c.y);
        const float area = std::abs(signedArea);
        if (!(area > kEpsilon)) continue;
        const float orientation = signedArea > 0.0F ? 1.0F : -1.0F;
        const int minX = std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
        const int maxX = std::min(static_cast<int>(target.width) - 1,
            static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
        const int minY = std::max(0, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
        const int maxY = std::min(static_cast<int>(target.height) - 1,
            static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
        if (minX > maxX || minY > maxY) {
            ++stats.rejectedTriangles;
            continue;
        }
        ++stats.rasterizedTriangles;
        for (int y = minY; y <= maxY; ++y) {
            for (int x = minX; x <= maxX; ++x) {
                const float px = static_cast<float>(x) + 0.5F;
                const float py = static_cast<float>(y) + 0.5F;
                const float rawA = orientation * edge(b.x, b.y, c.x, c.y, px, py);
                const float rawB = orientation * edge(c.x, c.y, a.x, a.y, px, py);
                const float rawC = orientation * edge(a.x, a.y, b.x, b.y, px, py);
                if (rawA < 0.0F || rawB < 0.0F || rawC < 0.0F) continue;
                const float baryA = rawA / area;
                const float baryB = rawB / area;
                const float baryC = rawC / area;
                const float perspective = baryA * a.invZ + baryB * b.invZ + baryC * c.invZ;
                if (!(perspective > kEpsilon)) continue;
                const float wa = baryA * a.invZ / perspective;
                const float wb = baryB * b.invZ / perspective;
                const float wc = baryC * c.invZ / perspective;
                const float cameraZ = 1.0F / perspective;
                const float depth = (cameraZ - camera.nearPlane) /
                                    (camera.farPlane - camera.nearPlane);
                const std::size_t pixel = static_cast<std::size_t>(y) * target.width +
                                          static_cast<std::size_t>(x);
                const float opaqueDepth = options.preserveExistingDepth ? target.depth[pixel] : 1.0F;
                if (depth >= opaqueDepth) {
                    ++stats.depthRejectedFragments;
                    continue;
                }
                Float4 color{
                    a.color.x * wa + b.color.x * wb + c.color.x * wc,
                    a.color.y * wa + b.color.y * wb + c.color.y * wc,
                    a.color.z * wa + b.color.z * wb + c.color.z * wc,
                    a.color.w * wa + b.color.w * wb + c.color.w * wc};
                if (!(color.w >= alphaCutoff)) {
                    ++stats.alphaRejectedFragments;
                    continue;
                }
                if (options.enableLighting) {
                    const Float3 worldPosition{
                        a.worldPosition.x * wa + b.worldPosition.x * wb + c.worldPosition.x * wc,
                        a.worldPosition.y * wa + b.worldPosition.y * wb + c.worldPosition.y * wc,
                        a.worldPosition.z * wa + b.worldPosition.z * wb + c.worldPosition.z * wc};
                    const Float3 tangent = normalized({
                        a.tangent.x * wa + b.tangent.x * wb + c.tangent.x * wc,
                        a.tangent.y * wa + b.tangent.y * wb + c.tangent.y * wc,
                        a.tangent.z * wa + b.tangent.z * wb + c.tangent.z * wc},
                        {0.0F, -1.0F, 0.0F});
                    const Float3 viewDirection = normalized(subtract(camera.position, worldPosition),
                                                            {0.0F, 0.0F, 1.0F});
                    const Float3 halfVector = normalized(add(towardSun, viewDirection), towardSun);
                    const float tangentLight = std::clamp(dot(tangent, towardSun), -1.0F, 1.0F);
                    const float diffuse = std::sqrt(std::max(0.0F, 1.0F - tangentLight * tangentLight));
                    const float tangentHalf = std::clamp(dot(tangent, halfVector), -1.0F, 1.0F);
                    const float specular = std::pow(std::max(0.0F, 1.0F - tangentHalf * tangentHalf),
                                                    specularExponent) * specularStrength;
                    const Float3 base{color.x, color.y, color.z};
                    const Float3 direct = multiply(component_multiply(environment.sunColor,
                        environment.globalTint), environment.sunIntensity * 0.38F * diffuse);
                    const Float3 lit = add(component_multiply(base, add(ambient, direct)),
                        multiply(component_multiply(environment.sunColor, environment.globalTint), specular));
                    const Float3 clamped = clamp_nonnegative(lit);
                    color.x = clamped.x;
                    color.y = clamped.y;
                    color.z = clamped.z;
                }
                const float alpha = std::clamp(color.w, 0.0F, 1.0F);
                Float4& destination = target.hdrColor[pixel];
                destination.x = color.x * alpha + destination.x * (1.0F - alpha);
                destination.y = color.y * alpha + destination.y * (1.0F - alpha);
                destination.z = color.z * alpha + destination.z * (1.0F - alpha);
                destination.w = alpha + destination.w * (1.0F - alpha);
                if (depth < nearestHairDepth_[pixel]) {
                    nearestHairDepth_[pixel] = depth;
                    if (options.writeObjectIds) {
                        target.objectId[pixel] = objectId;
                        target.materialIndex[pixel] = ordered.triangle < packet.triangleLayerIds.size()
                            ? packet.triangleLayerIds[ordered.triangle] : 0U;
                    }
                    if (options.writeDepth) target.depth[pixel] = depth;
                }
                ++stats.shadedFragments;
            }
        }
    }
    return stats;
}

} // namespace dve::render
