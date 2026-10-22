#include "dve/render/optical_scene.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

#include "dve/polygon_bvh.hpp"
#include "dve/render/radiance_cascades_spwi.hpp"
#include "dve/voxel_metric.hpp"

namespace dve::render {
namespace {
Float3 scale3(Float3 v, float s) noexcept { return {v.x * s, v.y * s, v.z * s}; }
Float3 add3(Float3 a, Float3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Float3 sub3(Float3 a, Float3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Float3 mul3(Float3 a, Float3 b) noexcept { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
float dot3(Float3 a, Float3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
Float3 cross3(Float3 a, Float3 b) noexcept { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
Float3 unit3(Float3 v, Float3 fallback = {0.0F, 1.0F, 0.0F}) noexcept {
    const float l = std::sqrt(dot3(v, v));
    return l > 1.0e-20F ? scale3(v, 1.0F / l) : fallback;
}
void grow(Float3& lo, Float3& hi, Float3 p) noexcept {
    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
}
}  // namespace

float dielectric_fresnel(float cosI, float n1, float n2) noexcept {
    cosI = std::clamp(cosI, 0.0F, 1.0F);
    const float eta = n1 / n2;
    const float sin2T = eta * eta * (1.0F - cosI * cosI);
    if (sin2T >= 1.0F) return 1.0F;
    const float cosT = std::sqrt(1.0F - sin2T);
    const float rs = (n1 * cosI - n2 * cosT) / (n1 * cosI + n2 * cosT);
    const float rp = (n2 * cosI - n1 * cosT) / (n2 * cosI + n1 * cosT);
    return 0.5F * (rs * rs + rp * rp);
}

std::optional<Float3> refract_direction(Float3 direction, Float3 normal, float eta) noexcept {
    const float cosI = -dot3(direction, normal);
    const float sin2T = eta * eta * (1.0F - cosI * cosI);
    if (sin2T > 1.0F) return std::nullopt;
    const float cosT = std::sqrt(1.0F - sin2T);
    return unit3(add3(scale3(direction, eta), scale3(normal, eta * cosI - cosT)), direction);
}

Float3 reflect_direction(Float3 direction, Float3 normal) noexcept {
    return unit3(sub3(direction, scale3(normal, 2.0F * dot3(direction, normal))), direction);
}

Float3 medium_transmittance(const VoxelMaterialDefinition& material, float distance, float metersPerUnit) noexcept {
    if (!(material.attenuationDistanceMeters > 0.0F) || !(distance > 0.0F)) return {1.0F, 1.0F, 1.0F};
    const float k = distance * metersPerUnit / material.attenuationDistanceMeters;
    const auto channel = [&](float c) { return c <= 0.0F ? 0.0F : std::pow(c, k); };
    return {channel(material.attenuationColor.x), channel(material.attenuationColor.y), channel(material.attenuationColor.z)};
}

struct OpticalScene::PolygonData {
    const OpticalPolygonInstance* instance{};
    std::shared_ptr<const PolygonBvh> bvh;
    Float3 minimum{}, maximum{};   // world bounds
    bool refractive{};
};

OpticalScene::OpticalScene(std::span<const VoxelReferenceInstance> voxels, std::span<const OpticalPolygonInstance> polygons,
                           float metersPerUnit)
    : voxels_(voxels), tracer_(std::make_unique<spwi::VoxelSceneTracer>(voxels)),
      metersPerUnit_(metersPerUnit > 0.0F && std::isfinite(metersPerUnit) ? metersPerUnit : kDefaultMetersPerVoxel) {
    std::map<const CookedPolygonAsset*, std::shared_ptr<const PolygonBvh>> built;
    for (const OpticalPolygonInstance& instance : polygons) {
        if (!instance.visible || !instance.asset) continue;
        auto& bvh = built[instance.asset];
        if (!bvh) {
            auto fresh = std::make_shared<PolygonBvh>();
            if (!fresh->build(*instance.asset)) continue;
            bvh = std::move(fresh);
        }
        PolygonData data;
        data.instance = &instance;
        data.bvh = bvh;
        data.minimum = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        data.maximum = scale3(data.minimum, -1.0F);
        for (const PolygonVertex& v : instance.asset->vertices) grow(data.minimum, data.maximum, transform_point(instance.transform, v.position));
        data.refractive = std::any_of(instance.asset->materials.begin(), instance.asset->materials.end(),
                                      [](const VoxelMaterialDefinition& m) { return material_is_refractive(m); });
        polygons_.push_back(std::move(data));
    }
}

OpticalScene::~OpticalScene() = default;

std::optional<OpticalHit> OpticalScene::trace(Float3 origin, Float3 direction, float maximumDistance) const {
    std::optional<OpticalHit> best;
    if (const auto hit = tracer_->trace(origin, direction, maximumDistance)) {
        OpticalHit h;
        h.distance = hit->objectHit.distance;
        h.position = hit->worldPosition;
        h.geometricNormal = unit3(hit->worldNormal);
        h.shadingNormal = h.geometricNormal;
        h.material = &tracer_->material(hit->instance, hit->objectHit.material);
        h.objectId = voxels_[hit->instance].objectId;
        h.voxelInstance = static_cast<std::int32_t>(hit->instance);
        h.voxel = hit->objectHit.voxel;
        h.materialId = hit->objectHit.material;
        best = h;
    }
    if (auto polygon = trace_polygons(origin, direction, best ? best->distance : maximumDistance);
        polygon && (!best || polygon->distance < best->distance))
        best = polygon;
    return best;
}

std::optional<OpticalHit> OpticalScene::trace_polygons(Float3 origin, Float3 direction, float maximumDistance) const {
    std::optional<OpticalHit> best;
    for (std::size_t i = 0; i < polygons_.size(); ++i) {
        const PolygonData& p = polygons_[i];
        const RigidTransform& pose = p.instance->transform;
        const float limit = best ? best->distance : maximumDistance;
        const auto hit = p.bvh->raycast(inverse_transform_point(pose, origin), inverse_transform_vector(pose, direction), limit);
        if (!hit || (best && hit->distance >= best->distance)) continue;
        const CookedPolygonAsset& asset = *p.instance->asset;
        const std::size_t t = hit->firstIndex;   // the triangle's place in the index buffer
        Float3 shading = hit->normal;
        Float3 geometric = hit->normal;
        if (t + 2U < asset.indices.size()) {
            const PolygonVertex& a = asset.vertices[asset.indices[t]];
            const PolygonVertex& b = asset.vertices[asset.indices[t + 1U]];
            const PolygonVertex& c = asset.vertices[asset.indices[t + 2U]];
            const Float3 e1 = sub3(b.position, a.position), e2 = sub3(c.position, a.position);
            geometric = unit3(cross3(e1, e2), hit->normal);
            // Barycentric weights of the hit point.
            const Float3 q = sub3(hit->position, a.position);
            const float d00 = dot3(e1, e1), d01 = dot3(e1, e2), d11 = dot3(e2, e2), d20 = dot3(q, e1), d21 = dot3(q, e2);
            const float denominator = d00 * d11 - d01 * d01;
            if (std::abs(denominator) > 1.0e-20F) {
                const float v = (d11 * d20 - d01 * d21) / denominator, w = (d00 * d21 - d01 * d20) / denominator, u = 1.0F - v - w;
                shading = unit3(add3(add3(scale3(a.normal, u), scale3(b.normal, v)), scale3(c.normal, w)), geometric);
            }
            // The vertex normals say which side is outside.
            if (dot3(geometric, shading) < 0.0F) geometric = scale3(geometric, -1.0F);
        }
        OpticalHit h;
        h.distance = hit->distance;
        h.position = transform_point(pose, hit->position);
        h.geometricNormal = unit3(transform_vector(pose, geometric));
        h.shadingNormal = unit3(transform_vector(pose, shading));
        h.material = hit->materialIndex < asset.materials.size() ? &asset.materials[hit->materialIndex] : nullptr;
        h.objectId = p.instance->objectId;
        h.polygonInstance = static_cast<std::int32_t>(i);
        best = h;
    }
    return best;
}

std::optional<OpticalHit> OpticalScene::trace_inside_voxels(const OpticalHit& inside, Float3 origin, Float3 direction,
                                                            float maximumDistance) const {
    if (inside.voxelInstance < 0 || static_cast<std::size_t>(inside.voxelInstance) >= voxels_.size()) return trace(origin, direction, maximumDistance);
    const std::size_t k = static_cast<std::size_t>(inside.voxelInstance);
    const VoxelReferenceInstance& instance = voxels_[k];
    const Float3 p0 = world_to_voxel(instance.transform, instance.spacing, origin);
    const Float3 dl = divide_components(inverse_transform_vector(instance.transform, direction), instance.spacing);
    Int3 cell{static_cast<std::int32_t>(std::floor(p0.x)), static_cast<std::int32_t>(std::floor(p0.y)),
              static_cast<std::int32_t>(std::floor(p0.z))};
    const float inf = std::numeric_limits<float>::infinity();
    const auto axis_setup = [&](float p, float d, std::int32_t c, std::int32_t& step, float& tMax, float& tDelta) {
        if (d > 0.0F) { step = 1; tMax = (static_cast<float>(c) + 1.0F - p) / d; tDelta = 1.0F / d; }
        else if (d < 0.0F) { step = -1; tMax = (static_cast<float>(c) - p) / d; tDelta = -1.0F / d; }
        else { step = 0; tMax = inf; tDelta = inf; }
    };
    Int3 step{};
    float tMax[3], tDelta[3];
    axis_setup(p0.x, dl.x, cell.x, step.x, tMax[0], tDelta[0]);
    axis_setup(p0.y, dl.y, cell.y, step.y, tMax[1], tDelta[1]);
    axis_setup(p0.z, dl.z, cell.z, step.z, tMax[2], tDelta[2]);
    std::optional<OpticalHit> boundary;
    float t = 0.0F;
    if (instance.object->material_at(cell) != inside.materialId) {
        boundary = OpticalHit{};   // already out (numerical edge): the boundary is here
        boundary->distance = 0.0F;
        boundary->position = origin;
        boundary->geometricNormal = scale3(direction, 1.0F);
        boundary->shadingNormal = boundary->geometricNormal;
        const MaterialId beyond = instance.object->material_at(cell);
        boundary->material = beyond == kAirMaterial ? nullptr : &tracer_->material(static_cast<std::uint32_t>(k), beyond);
        boundary->materialId = beyond;
    }
    for (int guard = 0; !boundary && guard < 1 << 16; ++guard) {
        const int axis = tMax[0] <= tMax[1] && tMax[0] <= tMax[2] ? 0 : (tMax[1] <= tMax[2] ? 1 : 2);
        t = tMax[axis];
        if (!(t <= maximumDistance)) break;
        Int3 localNormal{};
        if (axis == 0) { cell.x += step.x; tMax[0] += tDelta[0]; localNormal.x = step.x; }
        if (axis == 1) { cell.y += step.y; tMax[1] += tDelta[1]; localNormal.y = step.y; }
        if (axis == 2) { cell.z += step.z; tMax[2] += tDelta[2]; localNormal.z = step.z; }
        const MaterialId beyond = instance.object->material_at(cell);
        if (beyond == inside.materialId) continue;
        OpticalHit h;
        h.distance = t;
        h.position = add3(origin, scale3(direction, t));
        // Outward of the medium, toward the cell beyond.
        h.geometricNormal = voxel_normal_to_world(instance.transform, instance.spacing,
                                                  {static_cast<float>(localNormal.x), static_cast<float>(localNormal.y),
                                                   static_cast<float>(localNormal.z)});
        h.shadingNormal = h.geometricNormal;
        h.material = beyond == kAirMaterial ? nullptr : &tracer_->material(static_cast<std::uint32_t>(k), beyond);
        h.objectId = instance.objectId;
        h.voxelInstance = static_cast<std::int32_t>(k);
        h.voxel = cell;
        h.materialId = beyond;
        boundary = h;
    }
    // Anything else (another instance, a polygon) in the medium before its boundary.
    const float limit = boundary ? boundary->distance : maximumDistance;
    std::optional<OpticalHit> other;
    for (std::size_t j = 0; j < voxels_.size(); ++j) {
        if (j == k) continue;
        const auto hit = tracer_->trace_instance(j, origin, direction, other ? other->distance : limit);
        if (!hit || hit->objectHit.distance >= (other ? other->distance : limit)) continue;
        OpticalHit h;
        h.distance = hit->objectHit.distance;
        h.position = hit->worldPosition;
        h.geometricNormal = unit3(hit->worldNormal);
        h.shadingNormal = h.geometricNormal;
        h.material = &tracer_->material(static_cast<std::uint32_t>(j), hit->objectHit.material);
        h.objectId = voxels_[j].objectId;
        h.voxelInstance = static_cast<std::int32_t>(j);
        h.voxel = hit->objectHit.voxel;
        h.materialId = hit->objectHit.material;
        other = h;
    }
    if (auto polygon = trace_polygons(origin, direction, other ? other->distance : limit);
        polygon && (!other || polygon->distance < other->distance))
        other = polygon;
    if (other && (!boundary || other->distance < boundary->distance)) return other;
    return boundary;
}

bool OpticalScene::bounds(Float3& minimum, Float3& maximum) const {
    bool any = false;
    minimum = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    maximum = scale3(minimum, -1.0F);
    for (const VoxelReferenceInstance& v : voxels_) {
        if (!v.visible || !v.object) continue;
        const auto b = v.object->occupied_bounds();
        if (!b.valid) continue;
        for (int c = 0; c < 8; ++c) {
            const Float3 corner{static_cast<float>((c & 1) ? b.maximum.x + 1 : b.minimum.x),
                                static_cast<float>((c & 2) ? b.maximum.y + 1 : b.minimum.y),
                                static_cast<float>((c & 4) ? b.maximum.z + 1 : b.minimum.z)};
            grow(minimum, maximum, voxel_to_world(v.transform, v.spacing, corner));
        }
        any = true;
    }
    for (const PolygonData& p : polygons_) { grow(minimum, maximum, p.minimum); grow(minimum, maximum, p.maximum); any = true; }
    return any;
}

bool OpticalScene::refractive_bounds(Float3& minimum, Float3& maximum) const {
    bool any = false;
    minimum = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    maximum = scale3(minimum, -1.0F);
    for (std::size_t k = 0; k < voxels_.size(); ++k) {
        const VoxelReferenceInstance& v = voxels_[k];
        if (!v.visible || !v.object) continue;
        // Cells of refractive materials only.
        bool hasRefractive = false;
        for (std::size_t m = 0; m < v.materials.size(); ++m) hasRefractive = hasRefractive || material_is_refractive(v.materials[m]);
        if (!hasRefractive) continue;
        // Occupied cells only, brick by brick: a sparse object's bounding box can be enormous.
        for (const auto& [key, brick] : v.object->bricks()) {
            brick.occupancy().for_each_set([&](std::uint16_t index) {
                const MaterialId id = brick.material(index);
                if (id == kAirMaterial || id >= v.materials.size() || !material_is_refractive(v.materials[id])) return;
                const Int3 cell = global_from_local(key, local_from_index_unchecked(index));
                for (int c = 0; c < 8; ++c)
                    grow(minimum, maximum, voxel_to_world(v.transform, v.spacing,
                                                          {static_cast<float>(cell.x + ((c & 1) ? 1 : 0)),
                                                           static_cast<float>(cell.y + ((c & 2) ? 1 : 0)),
                                                           static_cast<float>(cell.z + ((c & 4) ? 1 : 0))}));
                any = true;
            });
        }
    }
    for (const PolygonData& p : polygons_)
        if (p.refractive) { grow(minimum, maximum, p.minimum); grow(minimum, maximum, p.maximum); any = true; }
    return any;
}

LightPathResult follow_light_transmitted(const OpticalScene& scene, Float3 origin, Float3 direction,
                                         const LightPathSettings& settings) {
    return follow_light_impl(scene, origin, direction, settings, false, nullptr, nullptr);
}

LightPathResult follow_light_impl(const OpticalScene& scene, Float3 origin, Float3 direction,
                                  const LightPathSettings& settings, bool sampled, float (*uniform)(void*), void* state) {
    struct Medium {
        const VoxelMaterialDefinition* material{};
        OpticalHit entry;   // voxel media: the entry hit (instance, material id)
    };
    std::vector<Medium> media;   // empty: air
    LightPathResult result;
    Float3 position = origin;
    Float3 dir = unit3(direction);
    float sinceInterface = 0.0F;
    const auto current_ior = [&] { return media.empty() ? 1.0F : media.back().material->indexOfRefraction; };
    for (std::uint32_t interaction = 0; interaction <= settings.maximumInteractions; ++interaction) {
        const bool inVoxelMedium = !media.empty() && media.back().entry.voxelInstance >= 0;
        const auto hit = inVoxelMedium ? scene.trace_inside_voxels(media.back().entry, position, dir, settings.maximumDistance)
                                       : scene.trace(position, dir, settings.maximumDistance);
        if (!hit) return result;
        if (!media.empty())
            result.throughput = mul3(result.throughput, medium_transmittance(*media.back().material, hit->distance, scene.meters_per_unit()));
        sinceInterface += hit->distance;

        // What is on the other side of this surface?
        enum class Kind { Receiver, Enter, Exit } kind = Kind::Receiver;
        const VoxelMaterialDefinition* next = nullptr;
        if (inVoxelMedium && hit->voxelInstance == media.back().entry.voxelInstance) {
            // The medium's own boundary: beyond is air, another medium, or an opaque cell.
            if (!hit->material) kind = Kind::Exit;
            else if (material_is_refractive(*hit->material)) { kind = Kind::Enter; next = hit->material; }
            else kind = Kind::Receiver;
        } else if (!media.empty() && hit->polygonInstance >= 0 && hit->polygonInstance == media.back().entry.polygonInstance &&
                   hit->material && material_is_refractive(*hit->material)) {
            // Leaving a polygon medium through its own surface. Decided by the mesh, not the
            // material slot: a closed mesh may use different refractive materials on different
            // faces. (An opaque face of the same mesh is a receiver.)
            kind = Kind::Exit;
        } else if (hit->material && material_is_refractive(*hit->material)) {
            kind = Kind::Enter;
            next = hit->material;
        }
        if (kind == Kind::Receiver) {
            result.landed = true;
            result.position = hit->position;
            Float3 n = hit->shadingNormal;
            if (dot3(n, dir) > 0.0F) n = scale3(n, -1.0F);
            result.normal = n;
            result.direction = dir;
            result.receiverObject = hit->objectId;
            result.pathLength = sinceInterface;
            return result;
        }
        if (interaction == settings.maximumInteractions) return result;

        const float n1 = current_ior();
        float n2 = 1.0F;
        if (kind == Kind::Enter) n2 = next->indexOfRefraction;
        else if (media.size() >= 2U) n2 = media[media.size() - 2U].material->indexOfRefraction;
        Float3 normal = hit->shadingNormal;
        if (dot3(normal, dir) > 0.0F) normal = scale3(normal, -1.0F);
        const float cosI = std::clamp(-dot3(dir, normal), 0.0F, 1.0F);
        const float fresnel = dielectric_fresnel(cosI, n1, n2);
        const auto refracted = refract_direction(dir, normal, n1 / n2);
        bool reflect = !refracted.has_value();
        if (!reflect && sampled) reflect = uniform(state) < fresnel;
        if (reflect) {
            dir = reflect_direction(dir, normal);
            ++result.reflections;
        } else {
            if (!sampled) result.throughput = scale3(result.throughput, 1.0F - fresnel);
            if (kind == Kind::Enter) {
                result.throughput = scale3(result.throughput, next->transmission);
                if (inVoxelMedium && hit->voxelInstance == media.back().entry.voxelInstance) media.pop_back();   // voxel media swap
                Medium m;
                m.material = next;
                m.entry = *hit;
                media.push_back(m);
            } else {
                media.pop_back();
            }
            dir = *refracted;
            ++result.refractions;
            sinceInterface = 0.0F;
        }
        position = add3(hit->position, scale3(dir, settings.surfaceOffset));
    }
    return result;
}

} // namespace dve::render
