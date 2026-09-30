#include "dve/render/radiance_cascades_spwi.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>
#include <utility>

#include "dve/render/ray_lighting_reference.hpp"

namespace dve::render {

bool RadianceCascadeSettings::validate(std::string* error) const noexcept {
    auto fail = [&](const char* message) {
        if (error) *error = message;
        return false;
    };
    if (baseProbeSpacingPixels == 0U || baseProbeSpacingPixels > 64U)
        return fail("baseProbeSpacingPixels must be in [1,64]");
    if (baseDirectionResolution < 2U || baseDirectionResolution > 64U ||
        (baseDirectionResolution % 2U) != 0U)
        return fail("baseDirectionResolution must be even and in [2,64]");
    if (!std::isfinite(baseIntervalLength) || !(baseIntervalLength > 0.0F))
        return fail("baseIntervalLength must be finite and positive");
    if (!std::isfinite(intervalGrowth) || intervalGrowth < 1.0F || intervalGrowth > 16.0F)
        return fail("intervalGrowth must be finite and in [1,16]");
    if (!std::isfinite(intervalOverlap) || intervalOverlap < 0.0F || intervalOverlap > 4.0F)
        return fail("intervalOverlap must be finite and in [0,4]");
    if (maximumCascades > 10U) return fail("maximumCascades must be in [0,10]");
    if (merge != RadianceCascadeMerge::Vanilla && merge != RadianceCascadeMerge::BilinearFix)
        return fail("merge mode is invalid");
    if (intervalScaling != RadianceCascadeIntervalScaling::World &&
        intervalScaling != RadianceCascadeIntervalScaling::ProbeSpacing)
        return fail("interval scaling is invalid");
    if (!std::isfinite(depthToleranceProbeSpacings) || !(depthToleranceProbeSpacings > 0.0F))
        return fail("depthToleranceProbeSpacings must be finite and positive");
    if (!std::isfinite(normalPower) || normalPower < 0.0F || normalPower > 256.0F)
        return fail("normalPower must be finite and in [0,256]");
    if (threadCount > 64U) return fail("threadCount must be in [0,64]");
    return true;
}

namespace spwi {
namespace {
constexpr float kPi = 3.14159265358979323846F;
constexpr std::uint32_t kMaximumCascades = 10U;
constexpr float kWeightEpsilon = 1.0e-4F;

using Clock = std::chrono::steady_clock;
double elapsed_ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

Float3 add3(Float3 a, Float3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Float3 scale3(Float3 a, float s) noexcept { return {a.x * s, a.y * s, a.z * s}; }
Float3 cross3(Float3 a, Float3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// Copied from src/query.cpp so the DDA is bit-identical to raycast_voxels.
float int_boundary(float value, float direction) noexcept {
    if (direction > 0.0F) return (std::floor(value) + 1.0F - value) / direction;
    if (direction < 0.0F) return (value - std::floor(value)) / -direction;
    return std::numeric_limits<float>::infinity();
}

const VoxelMaterialDefinition& diagnostic_material() noexcept {
    static const VoxelMaterialDefinition material = [] {
        VoxelMaterialDefinition value;
        value.name = "Missing voxel material";
        value.baseColor = {1.0F, 0.0F, 1.0F, 1.0F};
        value.roughness = 1.0F;
        return value;
    }();
    return material;
}

std::uint32_t resolve_threads(std::uint32_t requested) noexcept {
    std::uint32_t count = requested;
    if (count == 0U) count = std::max(1U, std::thread::hardware_concurrency());
    return std::clamp(count, 1U, 16U);
}

// Static contiguous partition of [0, items) over threads. Each item is owned by one thread, so
// results do not depend on the thread count.
template <typename Function>
void parallel_for(std::uint64_t items, std::uint32_t threads, Function&& function) {
    threads = static_cast<std::uint32_t>(std::min<std::uint64_t>(threads, std::max<std::uint64_t>(items, 1U)));
    if (threads <= 1U) {
        function(0U, 0ULL, items);
        return;
    }
    std::vector<std::thread> workers;
    workers.reserve(threads);
    for (std::uint32_t t = 0; t < threads; ++t) {
        const std::uint64_t begin = items * t / threads;
        const std::uint64_t end = items * (t + 1U) / threads;
        workers.emplace_back([&function, t, begin, end] { function(t, begin, end); });
    }
    for (std::thread& worker : workers) worker.join();
}

std::uint32_t hash32(std::uint32_t value) noexcept {
    value ^= value >> 16U;
    value *= 0x7feb352dU;
    value ^= value >> 15U;
    value *= 0x846ca68bU;
    value ^= value >> 16U;
    return value;
}

void tangent_basis(Float3 normal, Float3& tangent, Float3& bitangent) noexcept {
    const Float3 helper = std::abs(normal.z) < 0.999F ? Float3{0.0F, 0.0F, 1.0F}
                                                       : Float3{0.0F, 1.0F, 0.0F};
    tangent = normalize(cross3(helper, normal));
    bitangent = normalize(cross3(normal, tangent));
}

struct Segment {
    Float3 radiance{};
    float transmittance{1.0F};
};

struct Counters {
    std::uint64_t intervalRays{};
    std::uint64_t fallbackRays{};
    std::uint64_t sunRays{};
    std::uint64_t gatherFallbackPixels{};
};

Segment trace_interval(const VoxelSceneTracer& tracer, const RenderEnvironment& environment,
                       Float3 origin, Float3 direction, float lengthWorld, Counters& counters) {
    if (!(lengthWorld > 0.0F)) return {};
    ++counters.intervalRays;
    const auto hit = tracer.trace(origin, direction, lengthWorld);
    if (!hit) return {};
    return {bounce_hit_radiance(tracer, *hit, environment, &counters.sunRays), 0.0F};
}

// Radiance along a ray that runs to the GI max distance: a miss sees the environment.
Float3 trace_to_end(const VoxelSceneTracer& tracer, const RenderEnvironment& environment,
                    Float3 origin, Float3 direction, float lengthWorld, Counters& counters) {
    const Segment segment = trace_interval(tracer, environment, origin, direction, lengthWorld,
                                           counters);
    if (segment.transmittance > 0.0F)
        return add3(segment.radiance, scale3(environment_radiance(environment, direction),
                                             segment.transmittance));
    return segment.radiance;
}

struct Probe {
    bool valid{};
    std::uint32_t pixelX{};
    std::uint32_t pixelY{};
    Float3 position{};
    Float3 normal{};
    Float3 origin{};
    float viewDistance{};
};

std::vector<Probe> make_probes(const CascadeLevelInfo& level, const VoxelGBuffer& gbuffer,
                               float bias) {
    std::vector<Probe> probes(static_cast<std::size_t>(level.probesX) * level.probesY);
    const std::uint32_t spacing = level.probeSpacingPixels;
    for (std::uint32_t py = 0; py < level.probesY; ++py) {
        for (std::uint32_t px = 0; px < level.probesX; ++px) {
            Probe& probe = probes[static_cast<std::size_t>(py) * level.probesX + px];
            probe.pixelX = std::min(px * spacing + spacing / 2U, gbuffer.width - 1U);
            probe.pixelY = std::min(py * spacing + spacing / 2U, gbuffer.height - 1U);
            const VoxelGBufferTexel& texel =
                gbuffer.texels[static_cast<std::size_t>(probe.pixelY) * gbuffer.width + probe.pixelX];
            if (!texel.valid) continue;
            probe.valid = true;
            probe.position = texel.position;
            probe.normal = texel.normal;
            probe.origin = add(texel.position, multiply(texel.normal, bias));
            probe.viewDistance = texel.viewDistance;
        }
    }
    return probes;
}

struct Neighbours {
    std::array<std::uint32_t, 4> probe{};
    std::array<float, 4> weight{};
    float total{};
};

// Screen-space bilinear weights onto the upper grid, multiplied by a plane-distance term and a
// normal term (a bilateral/"geometry-aware" upsample). Sky and mismatched probes get zero.
Neighbours upper_neighbours(float pixelCentreX, float pixelCentreY, Float3 position,
                            Float3 normal, float viewDistance, const CascadeLevelInfo& upper,
                            const std::vector<Probe>& upperProbes, const VoxelGBuffer& gbuffer,
                            const RadianceCascadeSettings& settings) {
    Neighbours result;
    const float spacing = static_cast<float>(upper.probeSpacingPixels);
    const float gx = pixelCentreX / spacing - 0.5F;
    const float gy = pixelCentreY / spacing - 0.5F;
    const float fx0 = std::floor(gx);
    const float fy0 = std::floor(gy);
    const float fx = gx - fx0;
    const float fy = gy - fy0;
    const auto x0 = static_cast<std::int64_t>(fx0);
    const auto y0 = static_cast<std::int64_t>(fy0);
    const float sigma = std::max(1.0e-4F, settings.depthToleranceProbeSpacings * spacing *
                                              gbuffer.pixelWorldScale * viewDistance);
    for (std::uint32_t k = 0; k < 4U; ++k) {
        const std::uint32_t dx = k & 1U;
        const std::uint32_t dy = k >> 1U;
        const auto ix = static_cast<std::uint32_t>(
            std::clamp<std::int64_t>(x0 + dx, 0, static_cast<std::int64_t>(upper.probesX) - 1));
        const auto iy = static_cast<std::uint32_t>(
            std::clamp<std::int64_t>(y0 + dy, 0, static_cast<std::int64_t>(upper.probesY) - 1));
        const std::uint32_t index = iy * upper.probesX + ix;
        result.probe[k] = index;
        const float bilinear = (dx != 0U ? fx : 1.0F - fx) * (dy != 0U ? fy : 1.0F - fy);
        const Probe& q = upperProbes[index];
        if (!q.valid || !(bilinear > 0.0F)) continue;
        const float plane = std::abs(dot(normal, subtract(q.position, position))) / sigma;
        const float depthWeight = std::exp(-plane * plane);
        const float alignment = std::max(0.0F, dot(normal, q.normal));
        const float normalWeight = settings.normalPower > 0.0F
            ? std::pow(alignment, settings.normalPower) : (alignment > 0.0F ? 1.0F : 0.0F);
        result.weight[k] = bilinear * depthWeight * normalWeight;
        result.total += result.weight[k];
    }
    return result;
}

// Sub-texel quadrature of the octahedral map: direction + solid angle per sub-sample.
struct DirectionSample {
    Float3 direction{};
    float solidAngle{};
};

std::uint32_t subsamples_for(std::uint32_t resolution) noexcept {
    return std::max(2U, 32U / std::max(resolution, 1U));
}

float octahedral_solid_angle_density(float u, float v) noexcept {
    // Point on the unit octahedron |x|+|y|+|z|=1; dΩ = dx·dz / |P|^3 with dx·dz = 4·du·dv,
    // for both the inner diamond (+Y) and the folded outer triangles (-Y; the fold is
    // area-preserving).
    float x = 2.0F * u - 1.0F;
    float z = 2.0F * v - 1.0F;
    float y = 1.0F - std::abs(x) - std::abs(z);
    if (y < 0.0F) {
        const float ox = x;
        x = (1.0F - std::abs(z)) * (ox >= 0.0F ? 1.0F : -1.0F);
        z = (1.0F - std::abs(ox)) * (z >= 0.0F ? 1.0F : -1.0F);
    }
    const float length = std::sqrt(x * x + y * y + z * z);
    return 4.0F / (length * length * length);
}

std::vector<std::vector<DirectionSample>> texel_quadrature(std::uint32_t resolution) {
    const std::uint32_t sub = subsamples_for(resolution);
    const float step = 1.0F / static_cast<float>(resolution * sub);
    std::vector<std::vector<DirectionSample>> texels(static_cast<std::size_t>(resolution) * resolution);
    for (std::uint32_t ty = 0; ty < resolution; ++ty) {
        for (std::uint32_t tx = 0; tx < resolution; ++tx) {
            auto& samples = texels[static_cast<std::size_t>(ty) * resolution + tx];
            samples.reserve(static_cast<std::size_t>(sub) * sub);
            for (std::uint32_t sy = 0; sy < sub; ++sy) {
                for (std::uint32_t sx = 0; sx < sub; ++sx) {
                    const float u = (static_cast<float>(tx * sub + sx) + 0.5F) * step;
                    const float v = (static_cast<float>(ty * sub + sy) + 0.5F) * step;
                    samples.push_back({octahedral_decode(u, v),
                                       octahedral_solid_angle_density(u, v) * step * step});
                }
            }
        }
    }
    return texels;
}

// Per-thread memo of cosine-lobe weights ∫_texel max(0, n·ω) dω for the distinct normals seen
// (voxel faces give six per instance). The memo never changes a value, only avoids recomputing.
class CosineWeights {
public:
    explicit CosineWeights(const std::vector<std::vector<DirectionSample>>& quadrature)
        : quadrature_(&quadrature) {}
    const std::vector<float>& get(Float3 normal) {
        for (const auto& [key, weights] : cache_)
            if (key.x == normal.x && key.y == normal.y && key.z == normal.z) return weights;
        std::vector<float> weights(quadrature_->size(), 0.0F);
        for (std::size_t t = 0; t < quadrature_->size(); ++t) {
            float sum = 0.0F;
            for (const DirectionSample& sample : (*quadrature_)[t])
                sum += std::max(0.0F, dot(normal, sample.direction)) * sample.solidAngle;
            weights[t] = sum;
        }
        if (cache_.size() >= 64U) cache_.erase(cache_.begin());
        cache_.emplace_back(normal, std::move(weights));
        return cache_.back().second;
    }

private:
    const std::vector<std::vector<DirectionSample>>* quadrature_;
    std::vector<std::pair<Float3, std::vector<float>>> cache_;
};

} // namespace

// ---- VoxelSceneTracer -----------------------------------------------------------------------

VoxelSceneTracer::VoxelSceneTracer(std::span<const VoxelReferenceInstance> instances,
                                   std::uint64_t maximumDenseVoxelsPerInstance)
    : instances_(instances), grids_(instances.size()) {
    for (std::size_t k = 0; k < instances.size(); ++k) {
        const VoxelReferenceInstance& instance = instances[k];
        Grid& grid = grids_[k];
        grid.object = instance.object;
        if (!instance.visible || instance.object == nullptr) continue;
        Int3 minimum{std::numeric_limits<std::int32_t>::max(), std::numeric_limits<std::int32_t>::max(),
                     std::numeric_limits<std::int32_t>::max()};
        Int3 maximum{std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::min(),
                     std::numeric_limits<std::int32_t>::min()};
        bool any = false;
        for (const auto& entry : instance.object->bricks()) {
            const Int3 origin = brick_origin(entry.first);
            minimum = {std::min(minimum.x, origin.x), std::min(minimum.y, origin.y),
                       std::min(minimum.z, origin.z)};
            maximum = {std::max(maximum.x, origin.x + kBrickDim - 1),
                       std::max(maximum.y, origin.y + kBrickDim - 1),
                       std::max(maximum.z, origin.z + kBrickDim - 1)};
            any = true;
        }
        if (!any) continue;
        grid.active = true;
        grid.minimum = minimum;
        grid.maximum = maximum;
        grid.extent = {maximum.x - minimum.x + 1, maximum.y - minimum.y + 1,
                       maximum.z - minimum.z + 1};
        const std::uint64_t volume = static_cast<std::uint64_t>(grid.extent.x) *
                                     static_cast<std::uint64_t>(grid.extent.y) *
                                     static_cast<std::uint64_t>(grid.extent.z);
        if (volume > maximumDenseVoxelsPerInstance) continue;
        grid.dense = true;
        grid.cells.assign(volume, kAirMaterial);
        for (const auto& entry : instance.object->bricks()) {
            const Int3 origin = brick_origin(entry.first);
            for (std::int32_t z = 0; z < kBrickDim; ++z)
                for (std::int32_t y = 0; y < kBrickDim; ++y)
                    for (std::int32_t x = 0; x < kBrickDim; ++x) {
                        const Int3 voxel{origin.x + x, origin.y + y, origin.z + z};
                        const MaterialId material = instance.object->material_at(voxel);
                        if (material == kAirMaterial) continue;
                        const std::uint64_t index =
                            (static_cast<std::uint64_t>(voxel.z - minimum.z) * grid.extent.y +
                             static_cast<std::uint64_t>(voxel.y - minimum.y)) * grid.extent.x +
                            static_cast<std::uint64_t>(voxel.x - minimum.x);
                        grid.cells[index] = material;
                    }
        }
    }
}

std::uint64_t VoxelSceneTracer::dense_bytes() const noexcept {
    std::uint64_t bytes = 0;
    for (const Grid& grid : grids_) bytes += grid.cells.size() * sizeof(MaterialId);
    return bytes;
}

const VoxelMaterialDefinition& VoxelSceneTracer::material(std::uint32_t instance,
                                                          MaterialId id) const noexcept {
    if (instance < instances_.size()) {
        const auto& materials = instances_[instance].materials;
        if (static_cast<std::size_t>(id) < materials.size()) return materials[id];
    }
    return diagnostic_material();
}

std::optional<RayHit> VoxelSceneTracer::raycast_local(const Grid& grid, Float3 origin,
                                                      Float3 direction,
                                                      float maximumDistance) const {
    // Same arithmetic, order and hit rule as raycast_voxels (src/query.cpp) and TraceVoxelRay.
    const float magnitude = length(direction);
    if (!(magnitude > 0.0F) || !(maximumDistance >= 0.0F)) return std::nullopt;
    const float maximumParameter = maximumDistance / magnitude;
    Int3 voxel{static_cast<std::int32_t>(std::floor(origin.x)),
               static_cast<std::int32_t>(std::floor(origin.y)),
               static_cast<std::int32_t>(std::floor(origin.z))};
    const Int3 step{direction.x > 0.0F ? 1 : (direction.x < 0.0F ? -1 : 0),
                    direction.y > 0.0F ? 1 : (direction.y < 0.0F ? -1 : 0),
                    direction.z > 0.0F ? 1 : (direction.z < 0.0F ? -1 : 0)};
    Float3 tMax{int_boundary(origin.x, direction.x), int_boundary(origin.y, direction.y),
                int_boundary(origin.z, direction.z)};
    const Float3 tDelta{
        direction.x == 0.0F ? std::numeric_limits<float>::infinity() : std::abs(1.0F / direction.x),
        direction.y == 0.0F ? std::numeric_limits<float>::infinity() : std::abs(1.0F / direction.y),
        direction.z == 0.0F ? std::numeric_limits<float>::infinity() : std::abs(1.0F / direction.z)};

    const Int3 lo = grid.minimum;
    const Int3 hi = grid.maximum;
    float parameter = 0.0F;
    Int3 normal{};
    while (parameter <= maximumParameter) {
        const bool inside = voxel.x >= lo.x && voxel.x <= hi.x && voxel.y >= lo.y &&
                            voxel.y <= hi.y && voxel.z >= lo.z && voxel.z <= hi.z;
        if (inside) {
            MaterialId material = kAirMaterial;
            if (grid.dense) {
                const std::uint64_t index =
                    (static_cast<std::uint64_t>(voxel.z - lo.z) * grid.extent.y +
                     static_cast<std::uint64_t>(voxel.y - lo.y)) * grid.extent.x +
                    static_cast<std::uint64_t>(voxel.x - lo.x);
                material = grid.cells[index];
            } else {
                material = grid.object->material_at(voxel);
            }
            if (material != kAirMaterial) return RayHit{voxel, normal, parameter * magnitude, material};
        } else {
            // Exact early out: this axis can never re-enter the allocated bricks.
            if ((voxel.x < lo.x && step.x <= 0) || (voxel.x > hi.x && step.x >= 0) ||
                (voxel.y < lo.y && step.y <= 0) || (voxel.y > hi.y && step.y >= 0) ||
                (voxel.z < lo.z && step.z <= 0) || (voxel.z > hi.z && step.z >= 0))
                return std::nullopt;
        }
        if (tMax.x <= tMax.y && tMax.x <= tMax.z) {
            voxel.x += step.x;
            parameter = tMax.x;
            tMax.x += tDelta.x;
            normal = {-step.x, 0, 0};
        } else if (tMax.y <= tMax.z) {
            voxel.y += step.y;
            parameter = tMax.y;
            tMax.y += tDelta.y;
            normal = {0, -step.y, 0};
        } else {
            voxel.z += step.z;
            parameter = tMax.z;
            tMax.z += tDelta.z;
            normal = {0, 0, -step.z};
        }
    }
    return std::nullopt;
}

std::optional<VoxelTraceHit> VoxelSceneTracer::trace_instance(std::size_t instanceIndex,
                                                              Float3 origin, Float3 direction,
                                                              float maximumDistance) const {
    if (instanceIndex >= instances_.size()) return std::nullopt;
    const VoxelReferenceInstance& instance = instances_[instanceIndex];
    const Grid& grid = grids_[instanceIndex];
    if (!instance.visible || instance.object == nullptr || !grid.active) return std::nullopt;
    // Same sequence as raycast_voxels_transformed.
    const Float3 localOrigin = inverse_transform_point(instance.transform, origin);
    const Float3 localDirection = inverse_transform_vector(instance.transform, direction);
    const auto hit = raycast_local(grid, localOrigin, localDirection, maximumDistance);
    if (!hit) return std::nullopt;
    const Float3 localPosition = add(localOrigin, multiply(normalize(localDirection), hit->distance));
    const Float3 localNormal{static_cast<float>(hit->normal.x), static_cast<float>(hit->normal.y),
                             static_cast<float>(hit->normal.z)};
    return VoxelTraceHit{static_cast<std::uint32_t>(instanceIndex), *hit,
                         transform_point(instance.transform, localPosition),
                         normalize(transform_vector(instance.transform, localNormal))};
}

std::optional<VoxelTraceHit> VoxelSceneTracer::trace(Float3 origin, Float3 direction,
                                                     float maximumDistance) const {
    std::optional<VoxelTraceHit> nearest;
    float nearestDistance = maximumDistance;
    for (std::size_t k = 0; k < instances_.size(); ++k) {
        const auto hit = trace_instance(k, origin, direction, nearestDistance);
        if (!hit || hit->objectHit.distance >= nearestDistance) continue;
        nearestDistance = hit->objectHit.distance;
        nearest = hit;
    }
    return nearest;
}

// ---- Radiance model ------------------------------------------------------------------------

Float3 environment_radiance(const RenderEnvironment& environment, Float3 direction) noexcept {
    const Float3 normalized = normalize(direction);
    const float t = std::clamp(0.5F + 0.5F * normalized.y, 0.0F, 1.0F);
    return add3(multiply(environment.groundColor, 1.0F - t), multiply(environment.skyColor, t));
}

Float3 bounce_hit_radiance(const VoxelSceneTracer& tracer, const VoxelTraceHit& hit,
                           const RenderEnvironment& environment, std::uint64_t* sunRays) {
    const VoxelMaterialDefinition& material = tracer.material(hit.instance, hit.objectHit.material);
    const Float3 base{material.baseColor.x, material.baseColor.y, material.baseColor.z};
    const Float3 normal = hit.worldNormal;
    const Float3 sun = normalize(environment.sunDirection);
    float sunVisibility = 0.0F;
    // A sun ray only matters when it can contribute (same result as always tracing it).
    if (environment.sunIntensity > 0.0F && dot(normal, sun) > 0.0F) {
        const Float3 origin =
            add(hit.worldPosition, multiply(normal, std::max(1.0e-3F, environment.shadowBiasMeters)));
        if (sunRays) ++*sunRays;
        sunVisibility = tracer.occluded(origin, sun, environment.shadowMaxDistanceMeters) ? 0.0F : 1.0F;
    }
    return one_bounce_diffuse_radiance(base, material.metallic, material.emissive,
                                       environment_radiance(environment, normal),
                                       environment.sunColor, environment.sunIntensity, normal,
                                       environment.sunDirection, sunVisibility);
}

// ---- Octahedral maps -----------------------------------------------------------------------

Float3 octahedral_decode(float u, float v) noexcept {
    float x = 2.0F * u - 1.0F;
    float z = 2.0F * v - 1.0F;
    const float y = 1.0F - std::abs(x) - std::abs(z);
    if (y < 0.0F) {
        const float ox = x;
        x = (1.0F - std::abs(z)) * (ox >= 0.0F ? 1.0F : -1.0F);
        z = (1.0F - std::abs(ox)) * (z >= 0.0F ? 1.0F : -1.0F);
    }
    return normalize(Float3{x, y, z});
}

Float3 octahedral_texel_direction(std::uint32_t tx, std::uint32_t ty,
                                  std::uint32_t resolution) noexcept {
    const float inv = 1.0F / static_cast<float>(std::max(resolution, 1U));
    return octahedral_decode((static_cast<float>(tx) + 0.5F) * inv,
                             (static_cast<float>(ty) + 0.5F) * inv);
}

std::vector<float> octahedral_texel_solid_angles(std::uint32_t resolution) {
    const auto quadrature = texel_quadrature(resolution);
    std::vector<float> result(quadrature.size(), 0.0F);
    for (std::size_t t = 0; t < quadrature.size(); ++t)
        for (const DirectionSample& sample : quadrature[t]) result[t] += sample.solidAngle;
    return result;
}

// ---- Cascades ------------------------------------------------------------------------------

std::vector<CascadeLevelInfo> describe_cascades(const RadianceCascadeSettings& settings,
                                                const RenderEnvironment& environment,
                                                std::uint32_t width, std::uint32_t height,
                                                float worldUnitsPerIntervalUnit) {
    std::vector<CascadeLevelInfo> levels;
    if (!settings.validate() || width == 0U || height == 0U) return levels;
    if (!std::isfinite(worldUnitsPerIntervalUnit) || !(worldUnitsPerIntervalUnit > 0.0F))
        return levels;
    const float maximumDistance = environment.globalIlluminationMaxDistanceMeters;
    if (!(maximumDistance > 0.0F)) return levels;
    const std::uint32_t cap = settings.maximumCascades == 0U
        ? kMaximumCascades : std::min(settings.maximumCascades, kMaximumCascades);
    float start = 0.0F;
    float length = settings.baseIntervalLength * worldUnitsPerIntervalUnit;
    const std::uint32_t longestSide = std::max(width, height);
    for (std::uint32_t i = 0; i < cap; ++i) {
        CascadeLevelInfo level;
        level.level = i;
        level.probeSpacingPixels = settings.baseProbeSpacingPixels << i;
        level.probesX = (width + level.probeSpacingPixels - 1U) / level.probeSpacingPixels;
        level.probesY = (height + level.probeSpacingPixels - 1U) / level.probeSpacingPixels;
        level.directionResolution = settings.baseDirectionResolution << i;
        level.intervalStart = start;
        float end = start + length;
        // Stop at the GI max distance, the cascade cap, or once the next level would have fewer
        // than two probes along the longest screen axis.
        const bool last = end >= maximumDistance || i + 1U == cap ||
                          (level.probeSpacingPixels << 1U) * 2U > longestSide;
        if (last) end = maximumDistance;
        level.intervalEnd = end;
        level.texels = static_cast<std::uint64_t>(level.probesX) * level.probesY *
                       level.directionResolution * level.directionResolution;
        level.bytes = level.texels * (sizeof(Float3) + sizeof(std::uint8_t));
        levels.push_back(level);
        if (last) break;
        start = end;
        length *= settings.intervalGrowth;
    }
    return levels;
}

IndirectResult solve_radiance_cascades(const VoxelSceneTracer& tracer, const VoxelGBuffer& gbuffer,
                                       const RenderEnvironment& environment,
                                       const RadianceCascadeSettings& settings) {
    const auto started = Clock::now();
    IndirectResult result;
    result.width = gbuffer.width;
    result.height = gbuffer.height;
    result.indirect.assign(static_cast<std::size_t>(gbuffer.width) * gbuffer.height, Float3{});
    if (gbuffer.texels.size() != result.indirect.size()) return result;
    // Interval unit per probe: 1 world unit, or the cascade-0 cell size at the probe's depth.
    const bool depthScaled = settings.intervalScaling == RadianceCascadeIntervalScaling::ProbeSpacing;
    const float cellScale = static_cast<float>(settings.baseProbeSpacingPixels) * gbuffer.pixelWorldScale;
    float nearestUnit = std::numeric_limits<float>::infinity();
    if (depthScaled) {
        for (const VoxelGBufferTexel& texel : gbuffer.texels)
            if (texel.valid) nearestUnit = std::min(nearestUnit, cellScale * texel.viewDistance);
        if (!std::isfinite(nearestUnit)) nearestUnit = 1.0F;
        nearestUnit = std::max(nearestUnit, 1.0e-3F);
    } else {
        nearestUnit = 1.0F;
    }
    const std::vector<CascadeLevelInfo> levels =
        describe_cascades(settings, environment, gbuffer.width, gbuffer.height, nearestUnit);
    if (levels.empty()) return result;
    auto probe_ratio = [&](float viewDistance) {
        return depthScaled ? std::max(cellScale * viewDistance, 1.0e-3F) / nearestUnit : 1.0F;
    };
    const std::uint32_t threads = resolve_threads(settings.threadCount);
    const auto cascadeCount = static_cast<std::uint32_t>(levels.size());
    const float maximumDistance = environment.globalIlluminationMaxDistanceMeters;
    const float bias = std::max(1.0e-3F, environment.shadowBiasMeters);
    const bool bilinearFix = settings.merge == RadianceCascadeMerge::BilinearFix;

    SolveStats& stats = result.stats;
    stats.threads = threads;
    stats.cascades = cascadeCount;
    stats.levelMilliseconds.assign(cascadeCount, 0.0);
    for (std::uint32_t i = 0; i < cascadeCount; ++i) {
        stats.allLevelsBytes += levels[i].bytes;
        const std::uint64_t upperAveraged = i + 1U < cascadeCount ? levels[i + 1U].bytes / 4U : 0U;
        stats.peakBytes = std::max(stats.peakBytes, levels[i].bytes + upperAveraged);
    }
    std::vector<Counters> counters(threads);

    std::vector<Probe> upperProbes;
    std::vector<Float3> upperAveraged; // upper level pre-averaged to this level's resolution
    std::vector<std::uint8_t> upperValid;
    std::vector<Float3> levelRadiance;
    std::vector<std::uint8_t> levelValid;
    std::vector<Probe> probes;

    for (std::uint32_t i = cascadeCount; i-- > 0U;) {
        const auto levelStarted = Clock::now();
        const CascadeLevelInfo& level = levels[i];
        const bool top = i + 1U == cascadeCount;
        const CascadeLevelInfo* upper = top ? nullptr : &levels[i + 1U];
        probes = make_probes(level, gbuffer, bias);
        stats.probes += probes.size();
        const std::uint32_t resolution = level.directionResolution;
        const std::uint32_t texelCount = resolution * resolution;
        std::vector<Float3> directions(texelCount);
        for (std::uint32_t ty = 0; ty < resolution; ++ty)
            for (std::uint32_t tx = 0; tx < resolution; ++tx)
                directions[ty * resolution + tx] = octahedral_texel_direction(tx, ty, resolution);

        levelRadiance.assign(static_cast<std::size_t>(probes.size()) * texelCount, Float3{});
        levelValid.assign(levelRadiance.size(), 0U);
        const std::uint64_t items = static_cast<std::uint64_t>(probes.size()) * resolution;

        parallel_for(items, threads, [&](std::uint32_t thread, std::uint64_t begin, std::uint64_t end) {
            Counters& counter = counters[thread];
            std::uint64_t cachedProbe = std::numeric_limits<std::uint64_t>::max();
            Neighbours neighbours;
            float overlapLength = 0.0F;
            float intervalStart = 0.0F;
            float intervalLength = 0.0F;
            for (std::uint64_t item = begin; item < end; ++item) {
                const std::uint64_t probeIndex = item / resolution;
                const auto ty = static_cast<std::uint32_t>(item % resolution);
                const Probe& probe = probes[probeIndex];
                if (!probe.valid) continue;
                if (cachedProbe != probeIndex) {
                    const float ratio = probe_ratio(probe.viewDistance);
                    intervalStart = std::min(level.intervalStart * ratio, maximumDistance);
                    intervalLength = (top ? maximumDistance
                                          : std::min(level.intervalEnd * ratio, maximumDistance)) -
                                     intervalStart;
                }
                if (!top && cachedProbe != probeIndex) {
                    neighbours = upper_neighbours(static_cast<float>(probe.pixelX) + 0.5F,
                                                  static_cast<float>(probe.pixelY) + 0.5F,
                                                  probe.position, probe.normal, probe.viewDistance,
                                                  *upper, upperProbes, gbuffer, settings);
                    overlapLength = settings.intervalOverlap *
                                    static_cast<float>(upper->probeSpacingPixels) *
                                    gbuffer.pixelWorldScale * probe.viewDistance;
                }
                cachedProbe = probeIndex;
                for (std::uint32_t tx = 0; tx < resolution; ++tx) {
                    const std::uint32_t texel = ty * resolution + tx;
                    const Float3 direction = directions[texel];
                    if (!(dot(direction, probe.normal) > 0.0F)) continue;
                    const std::size_t out = probeIndex * texelCount + texel;
                    levelValid[out] = 1U;
                    const Float3 start = add(probe.origin, multiply(direction, intervalStart));
                    if (top) {
                        levelRadiance[out] = trace_to_end(tracer, environment, start, direction,
                                                          intervalLength, counter);
                        continue;
                    }
                    std::array<float, 4> weight{};
                    float total = 0.0F;
                    for (std::uint32_t k = 0; k < 4U; ++k) {
                        if (!(neighbours.weight[k] > 0.0F)) continue;
                        if (upperValid[static_cast<std::size_t>(neighbours.probe[k]) * texelCount + texel] == 0U)
                            continue;
                        weight[k] = neighbours.weight[k];
                        total += weight[k];
                    }
                    if (!(total > kWeightEpsilon)) {
                        // No compatible upper probe (silhouette, isolated surface, screen edge):
                        // finish this direction with one ray to the GI max distance.
                        ++counter.fallbackRays;
                        levelRadiance[out] = trace_to_end(tracer, environment, start, direction,
                                                          maximumDistance - intervalStart, counter);
                        continue;
                    }
                    const float inverseTotal = 1.0F / total;
                    Float3 merged{};
                    if (!bilinearFix) {
                        const Segment near = trace_interval(tracer, environment, start, direction,
                                                            intervalLength + overlapLength, counter);
                        Float3 far{};
                        if (near.transmittance > 0.0F) {
                            for (std::uint32_t k = 0; k < 4U; ++k) {
                                if (!(weight[k] > 0.0F)) continue;
                                far = add3(far, scale3(upperAveraged[static_cast<std::size_t>(neighbours.probe[k]) * texelCount + texel],
                                                       weight[k] * inverseTotal));
                            }
                        }
                        merged = add3(near.radiance, scale3(far, near.transmittance));
                    } else {
                        for (std::uint32_t k = 0; k < 4U; ++k) {
                            if (!(weight[k] > 0.0F)) continue;
                            const Probe& q = upperProbes[neighbours.probe[k]];
                            // End exactly where this upper probe's own interval starts.
                            const float upperStart = std::min(
                                upper->intervalStart * probe_ratio(q.viewDistance), maximumDistance);
                            const Float3 target =
                                add(q.origin, multiply(direction, upperStart + overlapLength));
                            const Float3 delta = subtract(target, start);
                            const float segmentLength = length(delta);
                            Segment near;
                            if (segmentLength > 1.0e-5F)
                                near = trace_interval(tracer, environment, start,
                                                      multiply(delta, 1.0F / segmentLength),
                                                      segmentLength, counter);
                            Float3 value = near.radiance;
                            if (near.transmittance > 0.0F)
                                value = add3(value, scale3(upperAveraged[static_cast<std::size_t>(neighbours.probe[k]) * texelCount + texel],
                                                           near.transmittance));
                            merged = add3(merged, scale3(value, weight[k] * inverseTotal));
                        }
                    }
                    levelRadiance[out] = merged;
                }
            }
        });

        if (i > 0U) {
            // Pre-average 2×2 child texels (solid-angle weighted, valid children only) into the
            // lower level's resolution. At lower texel centres this equals a bilinear lookup of
            // the upper octahedral map (the centre is the shared corner of the four children).
            const std::uint32_t lowerResolution = resolution / 2U;
            const std::uint32_t lowerTexels = lowerResolution * lowerResolution;
            const std::vector<float> solidAngles = octahedral_texel_solid_angles(resolution);
            upperAveraged.assign(probes.size() * lowerTexels, Float3{});
            upperValid.assign(upperAveraged.size(), 0U);
            parallel_for(probes.size(), threads, [&](std::uint32_t, std::uint64_t begin, std::uint64_t end) {
                for (std::uint64_t p = begin; p < end; ++p) {
                    if (!probes[p].valid) continue;
                    for (std::uint32_t uy = 0; uy < lowerResolution; ++uy) {
                        for (std::uint32_t ux = 0; ux < lowerResolution; ++ux) {
                            Float3 sum{};
                            float weightSum = 0.0F;
                            for (std::uint32_t c = 0; c < 4U; ++c) {
                                const std::uint32_t child =
                                    (2U * uy + (c >> 1U)) * resolution + 2U * ux + (c & 1U);
                                const std::size_t index = p * texelCount + child;
                                if (levelValid[index] == 0U) continue;
                                sum = add3(sum, scale3(levelRadiance[index], solidAngles[child]));
                                weightSum += solidAngles[child];
                            }
                            if (!(weightSum > 0.0F)) continue;
                            const std::size_t out = p * lowerTexels + uy * lowerResolution + ux;
                            upperAveraged[out] = scale3(sum, 1.0F / weightSum);
                            upperValid[out] = 1U;
                        }
                    }
                }
            });
            upperProbes = std::move(probes);
            probes.clear();
        }
        stats.levelMilliseconds[i] = elapsed_ms(levelStarted);
    }

    // Final gather: cosine-weighted integral of cascade 0 at each pixel's own normal, blended
    // over the four nearest cascade-0 probes with the same bilateral weights.
    const auto gatherStarted = Clock::now();
    const CascadeLevelInfo& base = levels.front();
    const std::uint32_t resolution = base.directionResolution;
    const std::uint32_t texelCount = resolution * resolution;
    const auto quadrature = texel_quadrature(resolution);
    std::vector<Float3> baseDirections(texelCount);
    for (std::uint32_t ty = 0; ty < resolution; ++ty)
        for (std::uint32_t tx = 0; tx < resolution; ++tx)
            baseDirections[ty * resolution + tx] = octahedral_texel_direction(tx, ty, resolution);
    const float intensity = environment.globalIlluminationIntensity;
    parallel_for(gbuffer.height, threads, [&](std::uint32_t thread, std::uint64_t begin, std::uint64_t end) {
        Counters& counter = counters[thread];
        CosineWeights cosineWeights(quadrature);
        for (std::uint64_t y = begin; y < end; ++y) {
            for (std::uint32_t x = 0; x < gbuffer.width; ++x) {
                const std::size_t pixel = static_cast<std::size_t>(y) * gbuffer.width + x;
                const VoxelGBufferTexel& texel = gbuffer.texels[pixel];
                if (!texel.valid) continue;
                const std::vector<float>& cosine = cosineWeights.get(texel.normal);
                const Neighbours neighbours = upper_neighbours(
                    static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F, texel.position,
                    texel.normal, texel.viewDistance, base, probes, gbuffer, settings);
                Float3 sum{};
                float total = 0.0F;
                for (std::uint32_t k = 0; k < 4U; ++k) {
                    if (!(neighbours.weight[k] > 0.0F)) continue;
                    Float3 irradiance{};
                    float cosineSum = 0.0F;
                    const std::size_t offset = static_cast<std::size_t>(neighbours.probe[k]) * texelCount;
                    for (std::uint32_t t = 0; t < texelCount; ++t) {
                        if (levelValid[offset + t] == 0U || !(cosine[t] > 0.0F)) continue;
                        irradiance = add3(irradiance, scale3(levelRadiance[offset + t], cosine[t]));
                        cosineSum += cosine[t];
                    }
                    if (!(cosineSum > 0.0F)) continue;
                    sum = add3(sum, scale3(irradiance, neighbours.weight[k] / cosineSum));
                    total += neighbours.weight[k];
                }
                Float3 value{};
                if (total > kWeightEpsilon) {
                    value = scale3(sum, 1.0F / total);
                } else {
                    // No compatible cascade-0 probe: integrate this pixel directly with the
                    // cascade-0 direction set and full-length rays.
                    ++counter.gatherFallbackPixels;
                    const Float3 origin = add(texel.position, multiply(texel.normal, bias));
                    float cosineSum = 0.0F;
                    for (std::uint32_t t = 0; t < texelCount; ++t) {
                        if (!(cosine[t] > 0.0F) || !(dot(baseDirections[t], texel.normal) > 0.0F)) continue;
                        ++counter.fallbackRays;
                        value = add3(value, scale3(trace_to_end(tracer, environment, origin, baseDirections[t],
                                                                maximumDistance, counter), cosine[t]));
                        cosineSum += cosine[t];
                    }
                    if (cosineSum > 0.0F) value = scale3(value, 1.0F / cosineSum);
                }
                result.indirect[pixel] = scale3(value, intensity);
            }
        }
    });
    stats.gatherMilliseconds = elapsed_ms(gatherStarted);
    for (const Counters& counter : counters) {
        stats.intervalRays += counter.intervalRays;
        stats.fallbackRays += counter.fallbackRays;
        stats.sunRays += counter.sunRays;
        stats.gatherFallbackPixels += counter.gatherFallbackPixels;
    }
    stats.milliseconds = elapsed_ms(started);
    return result;
}

IndirectResult solve_brute_force_indirect(const VoxelSceneTracer& tracer, const VoxelGBuffer& gbuffer,
                                          const RenderEnvironment& environment,
                                          std::uint32_t samples, std::uint32_t threadCount) {
    const auto started = Clock::now();
    IndirectResult result;
    result.width = gbuffer.width;
    result.height = gbuffer.height;
    result.indirect.assign(static_cast<std::size_t>(gbuffer.width) * gbuffer.height, Float3{});
    if (samples == 0U || gbuffer.texels.size() != result.indirect.size()) return result;
    const std::uint32_t threads = resolve_threads(threadCount);
    result.stats.threads = threads;
    const float maximumDistance = environment.globalIlluminationMaxDistanceMeters;
    const float bias = environment.shadowBiasMeters;
    const float scale = environment.globalIlluminationIntensity / static_cast<float>(samples);
    std::vector<Counters> counters(threads);
    parallel_for(gbuffer.height, threads, [&](std::uint32_t thread, std::uint64_t begin, std::uint64_t end) {
        Counters& counter = counters[thread];
        for (std::uint64_t y = begin; y < end; ++y) {
            for (std::uint32_t x = 0; x < gbuffer.width; ++x) {
                const std::size_t pixel = static_cast<std::size_t>(y) * gbuffer.width + x;
                const VoxelGBufferTexel& texel = gbuffer.texels[pixel];
                if (!texel.valid) continue;
                const Float3 origin = add(texel.position, multiply(texel.normal, bias));
                Float3 tangent{}, bitangent{};
                tangent_basis(texel.normal, tangent, bitangent);
                const std::uint32_t seed = hash32(static_cast<std::uint32_t>(pixel) * 747796405U + 2891336453U);
                const Float2 rotation{static_cast<float>(seed & 0xFFFFU) / 65536.0F,
                                      static_cast<float>(seed >> 16U) / 65536.0F};
                Float3 sum{};
                for (std::uint32_t s = 0; s < samples; ++s) {
                    const Float2 sample = rotated_hammersley_2d(s, samples, rotation);
                    const float radius = std::sqrt(sample.x);
                    const float angle = 2.0F * kPi * sample.y;
                    const float lx = radius * std::cos(angle);
                    const float ly = radius * std::sin(angle);
                    const float lz = std::sqrt(std::max(0.0F, 1.0F - sample.x));
                    const Float3 direction = normalize(add3(add3(scale3(tangent, lx), scale3(bitangent, ly)),
                                                            scale3(texel.normal, lz)));
                    sum = add3(sum, trace_to_end(tracer, environment, origin, direction, maximumDistance, counter));
                }
                result.indirect[pixel] = scale3(sum, scale);
            }
        }
    });
    for (const Counters& counter : counters) {
        result.stats.intervalRays += counter.intervalRays;
        result.stats.sunRays += counter.sunRays;
    }
    result.stats.milliseconds = elapsed_ms(started);
    return result;
}

float luminance(Float3 value) noexcept {
    return 0.2126F * value.x + 0.7152F * value.y + 0.0722F * value.z;
}

IndirectError compare_indirect(const IndirectResult& test, const IndirectResult& reference,
                               const VoxelGBuffer& gbuffer, std::span<const std::uint8_t> mask) {
    IndirectError error;
    const std::size_t count = gbuffer.texels.size();
    if (test.indirect.size() != count || reference.indirect.size() != count) return error;
    double sumSquared = 0.0, sumAbsolute = 0.0, sumReference = 0.0, sumBias = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        if (!gbuffer.texels[i].valid) continue;
        if (!mask.empty() && (i >= mask.size() || mask[i] == 0U)) continue;
        const double r = luminance(reference.indirect[i]);
        const double d = static_cast<double>(luminance(test.indirect[i])) - r;
        ++error.pixels;
        sumReference += r;
        sumSquared += d * d;
        sumAbsolute += std::abs(d);
        sumBias += d;
        error.maxAbsolute = std::max(error.maxAbsolute, std::abs(d));
    }
    if (error.pixels == 0U) return error;
    const double n = static_cast<double>(error.pixels);
    error.meanReference = sumReference / n;
    error.rmse = std::sqrt(sumSquared / n);
    error.meanAbsolute = sumAbsolute / n;
    error.meanBias = sumBias / n;
    if (error.meanReference > 0.0) {
        error.relativeRmse = error.rmse / error.meanReference;
        error.relativeMax = error.maxAbsolute / error.meanReference;
    }
    return error;
}

// ---- Synthetic scenes ----------------------------------------------------------------------

namespace {
void fill_box(VoxelObject& object, Int3 minimum, Int3 maximum, MaterialId material) {
    for (std::int32_t z = minimum.z; z <= maximum.z; ++z)
        for (std::int32_t y = minimum.y; y <= maximum.y; ++y)
            for (std::int32_t x = minimum.x; x <= maximum.x; ++x)
                (void)object.set_voxel({x, y, z}, material);
}

VoxelMaterialDefinition diffuse(const char* name, Float3 colour, Float3 emissive = {}) {
    VoxelMaterialDefinition material;
    material.name = name;
    material.baseColor = {colour.x, colour.y, colour.z, 1.0F};
    material.emissive = emissive;
    material.roughness = 1.0F;
    return material;
}

enum : MaterialId { kWhite = 1, kRed = 2, kGreen = 3, kLamp = 4, kBlue = 5, kConcrete = 6 };

std::vector<VoxelMaterialDefinition> scene_materials() {
    std::vector<VoxelMaterialDefinition> materials(7);
    materials[kWhite] = diffuse("white", {0.75F, 0.75F, 0.72F});
    materials[kRed] = diffuse("red", {0.80F, 0.12F, 0.10F});
    materials[kGreen] = diffuse("green", {0.12F, 0.70F, 0.18F});
    materials[kLamp] = diffuse("lamp", {1.0F, 0.6F, 0.2F}, {6.0F, 3.2F, 1.0F});
    materials[kBlue] = diffuse("blue", {0.18F, 0.30F, 0.85F});
    materials[kConcrete] = diffuse("concrete", {0.55F, 0.55F, 0.58F});
    return materials;
}
} // namespace

std::uint32_t remove_box(VoxelObject& object, Int3 minimum, Int3 maximum) {
    std::uint32_t changed = 0;
    for (std::int32_t z = minimum.z; z <= maximum.z; ++z)
        for (std::int32_t y = minimum.y; y <= maximum.y; ++y)
            for (std::int32_t x = minimum.x; x <= maximum.x; ++x) {
                if (object.material_at({x, y, z}) == kAirMaterial) continue;
                (void)object.set_voxel({x, y, z}, kAirMaterial);
                ++changed;
            }
    return changed;
}

SyntheticSceneSetup make_synthetic_scene(SyntheticScene scene) {
    SyntheticSceneSetup setup;
    setup.object = std::make_unique<VoxelObject>(7000U + static_cast<std::uint64_t>(scene));
    setup.materials = scene_materials();
    VoxelObject& object = *setup.object;
    RenderEnvironment& environment = setup.environment;
    environment.shadowMaxDistanceMeters = 200.0F;
    environment.globalIlluminationIntensity = 1.0F;
    environment.globalIlluminationMaxDistanceMeters = 48.0F;
    environment.globalIlluminationSamples = 16U;
    PolygonCamera& camera = setup.camera;
    camera.nearPlane = 0.1F;
    camera.farPlane = 150.0F;
    switch (scene) {
    case SyntheticScene::Courtyard:
        fill_box(object, {0, 0, 0}, {39, 0, 39}, kWhite);
        fill_box(object, {0, 1, 0}, {1, 12, 39}, kRed);          // west wall
        fill_box(object, {2, 1, 38}, {39, 10, 39}, kGreen);      // north wall
        fill_box(object, {2, 12, 0}, {13, 12, 37}, kConcrete);   // overhang off the red wall
        fill_box(object, {16, 1, 14}, {23, 8, 21}, kWhite);      // block
        fill_box(object, {30, 1, 8}, {32, 14, 10}, kBlue);       // pillar
        environment.sunDirection = {0.55F, 0.70F, -0.45F};
        environment.sunIntensity = 3.0F;
        environment.skyColor = {0.20F, 0.26F, 0.36F};
        environment.groundColor = {0.08F, 0.08F, 0.09F};
        camera.position = {50.0F, 30.0F, -16.0F};
        camera.target = {17.0F, 2.0F, 20.0F};
        break;
    case SyntheticScene::ThinWall:
        fill_box(object, {0, 0, 0}, {47, 0, 23}, kWhite);
        setup.wallX = 24;
        fill_box(object, {24, 1, 0}, {24, 12, 23}, kWhite);      // 1 voxel = 0.1 m
        // Emissive panel hanging beside the wall on the left, below the wall top, so no point
        // on the right floor can see it over the wall.
        fill_box(object, {19, 6, 4}, {21, 9, 19}, kLamp);
        environment.sunIntensity = 0.0F;
        environment.skyColor = {0.0F, 0.0F, 0.0F};
        environment.groundColor = {0.0F, 0.0F, 0.0F};
        camera.position = {24.0F, 40.0F, 12.0F};
        camera.target = {24.0F, 0.0F, 12.0F};
        camera.up = {0.0F, 0.0F, -1.0F};
        break;
    case SyntheticScene::Bunker:
        fill_box(object, {0, 0, 0}, {40, 0, 24}, kWhite);        // yard + room floor
        setup.wallX = 24;
        fill_box(object, {24, 1, 0}, {24, 9, 24}, kConcrete);    // shared west wall (1 voxel)
        fill_box(object, {40, 1, 0}, {40, 9, 24}, kConcrete);
        fill_box(object, {25, 1, 0}, {39, 9, 0}, kConcrete);
        fill_box(object, {25, 1, 24}, {39, 9, 24}, kConcrete);
        fill_box(object, {24, 10, 0}, {40, 10, 24}, kConcrete);  // roof
        fill_box(object, {33, 1, 16}, {36, 4, 19}, kRed);        // crate inside
        fill_box(object, {0, 1, 0}, {0, 8, 24}, kGreen);         // far yard wall
        setup.removableMinimum = {24, 1, 8};
        setup.removableMaximum = {24, 6, 16};
        environment.sunDirection = {-0.60F, 0.70F, 0.25F};
        environment.sunIntensity = 3.0F;
        environment.skyColor = {0.04F, 0.05F, 0.07F};
        environment.groundColor = {0.02F, 0.02F, 0.02F};
        camera.position = {38.5F, 6.5F, 12.5F};
        camera.target = {25.0F, 2.0F, 12.0F};
        break;
    }
    return setup;
}

} // namespace spwi
} // namespace dve::render
