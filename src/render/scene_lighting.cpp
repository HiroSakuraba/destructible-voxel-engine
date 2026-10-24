#include "dve/render/scene_lighting.hpp"
#include "dve/render/ray_offset.hpp"
#include "dve/render/subsurface_random_walk.hpp"
#include "dve/voxel_normal_map.hpp"
#include "dve/polygon_bvh.hpp"
#include "dve/render/material_bsdf.hpp"
#include "dve/render/optical_scene.hpp"
#include "dve/render/radiance_cascades_spwi.hpp"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <deque>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_map>

namespace dve::render {
namespace {
constexpr float pi = 3.14159265358979323846F;
Float3 sum3(Float3 a, Float3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Float3 sub(Float3 a, Float3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Float3 mul(Float3 a, float b) { return {a.x * b, a.y * b, a.z * b}; }
Float3 product(Float3 a, Float3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
float dot3(Float3 a, Float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Float3 cross(Float3 a, Float3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Float3 unit(Float3 a) {
  float l = std::sqrt(dot3(a, a));
  return l > 1e-12F ? mul(a, 1 / l) : Float3{0, 1, 0};
}
float peak(Float3 a) { return (std::max)({a.x, a.y, a.z}); }
float luminance(Float3 a) { return .2126F * a.x + .7152F * a.y + .0722F * a.z; }
Float3 base(const VoxelMaterialDefinition &m) {
  return {m.baseColor.x, m.baseColor.y, m.baseColor.z};
}
Float3 emission(const VoxelMaterialDefinition &m) {
  return m.shadingModel == MaterialShadingModel::Emissive
             ? sum3(base(m), m.emissive)
             : m.emissive;
}
struct Rng {
  std::uint32_t state;
  float next() {
    state += 0x9e3779b9U;
    auto v = state;
    v = (v ^ (v >> 16)) * 0x21f0aaadU;
    v = (v ^ (v >> 15)) * 0x735a2d97U;
    v ^= v >> 15;
    return float(v >> 8) * 0x1p-24F;
  }
};

float mis(float a, float b) {
  const double x = a, y = b;
  return float(x * x / (x * x + y * y));
}
struct Emitter {
  enum Kind { Point, Spot, Quad, Disc, Triangle } kind{Point};
  Float3 p{}, u{}, v{}, n{}, radiance{};
  float area{}, inner{}, outer{};
  bool twoSided{};
  double cumulative{}, weight{};
  int polygon{-1};
  std::uint32_t firstIndex{},materialIndex{};
  std::shared_ptr<const IesProfile> ies{};
};
std::shared_ptr<const std::vector<PolygonTextureMipChain>> surface_mips(const CookedPolygonAsset& asset) {
    using Chains=std::vector<PolygonTextureMipChain>;
    struct Cache {std::mutex mutex;std::unordered_map<std::uint64_t,std::shared_ptr<const Chains>> entries;std::deque<std::pair<std::uint64_t,std::size_t>> order;std::size_t bytes{};};
    static Cache cache;const auto key=asset.contentHash?asset.contentHash:polygon_asset_content_hash(asset);
    {std::lock_guard lock(cache.mutex);if(auto it=cache.entries.find(key);it!=cache.entries.end())return it->second;}
    auto result=std::make_shared<Chains>();std::size_t bytes=0;
    for(std::size_t image=0;image<asset.images.size();++image){bool normals=false,srgb=false;
        const auto uses=[&](const PolygonTextureBinding& b){return b.texture&&*b.texture<asset.textures.size()&&asset.textures[*b.texture].imageIndex==image;};
        for(const auto& b:asset.materialBindings){normals=normals||uses(b.normal)||uses(b.detailNormal);srgb=srgb||(uses(b.baseColor)&&b.baseColor.colorSpace==PolygonTextureColorSpace::Srgb)||(uses(b.emissive)&&b.emissive.colorSpace==PolygonTextureColorSpace::Srgb)||(uses(b.detailBaseColor)&&b.detailBaseColor.colorSpace==PolygonTextureColorSpace::Srgb);}
        auto chain=generate_texture_mips(asset.images[image]);
        if(normals)chain.normalLevels=generate_texture_mips(asset.images[image],true,false).levels;
        if(srgb)chain.srgbLevels=generate_texture_mips(asset.images[image],false,true).levels;
        for(const auto* levels:{&chain.levels,&chain.normalLevels,&chain.srgbLevels})
            for(const auto& mip:*levels)bytes+=mip.rgba8.size();
        result->push_back(std::move(chain));
    }
    constexpr std::size_t budget=64U*1024U*1024U;
    if(bytes<=budget){std::lock_guard lock(cache.mutex);if(auto it=cache.entries.find(key);it!=cache.entries.end())return it->second;
        while(cache.bytes+bytes>budget&&!cache.order.empty()){cache.bytes-=cache.order.front().second;cache.entries.erase(cache.order.front().first);cache.order.pop_front();}
        cache.entries.emplace(key,result);cache.order.emplace_back(key,bytes);cache.bytes+=bytes;}
    return result;
}
struct Hit {
  float distance{};
  Float3 p{}, n{};
  VoxelMaterialDefinition material;
  Float3 geometricNormal{};
  OpticalHit optical{};
  bool geometric{};
  float alphaCutoff{.5F};
};
std::optional<float> intersect(const Emitter &e, Float3 o, Float3 d) {
  if (e.area <= 0)
    return {};
  const float den = dot3(e.n, d);
  if (std::abs(den) < 1e-8F)
    return {};
  const float t = dot3(sub(e.p, o), e.n) / den;
  if (t <= 1e-5F)
    return {};
  const auto q = sub(sum3(o, mul(d, t)), e.p);
  const float uu = dot3(e.u, e.u), vv = dot3(e.v, e.v), uv = dot3(e.u, e.v),
              qu = dot3(q, e.u), qv = dot3(q, e.v);
  const float det = uu * vv - uv * uv;
  const float a = (qu * vv - qv * uv) / det, b = (qv * uu - qu * uv) / det;
  const bool inside = e.kind == Emitter::Disc ? a * a + b * b <= 1
                      : e.kind == Emitter::Triangle
                          ? (a >= 0 && b >= 0 && a + b <= 1)
                          : (std::abs(a) <= .5F && std::abs(b) <= .5F);
  return inside ? std::optional<float>(t) : std::nullopt;
}
} // namespace
bool material_uses_camera_transport(const VoxelMaterialDefinition &m) noexcept {
  return m.transmission > 0 || (m.metallic >= 1 && m.roughness <= .001F);
}
bool scene_uses_camera_transport(
    std::span<const VoxelReferenceInstance> voxels,
    std::span<const PolygonRenderInstance> polygons) noexcept {
  for (const auto &v : voxels)
    if (v.visible && v.object &&
        std::any_of(v.materials.begin(), v.materials.end(),
                    material_uses_camera_transport))
      return true;
  for (const auto &p : polygons)
    if (p.visible && p.asset &&
        std::any_of(p.asset->materials.begin(), p.asset->materials.end(),
                    material_uses_camera_transport))
      return true;
  return false;
}
float candle_light_multiplier(const SceneLight &light, double time) noexcept {
  if (light.kind != SceneLightKind::Candle || !std::isfinite(time))
    return 1;
  const double phase = double(light.seed) * 2.399963229728653;
  const double t =
      std::remainder(time * light.flickerFrequencyHz, 1e6) * 2 * pi;
  return float(1 +
               light.flickerAmount * (.6 * std::sin(t + phase) +
                                      .3 * std::sin(t * 1.713 + phase * 2) +
                                      .1 * std::sin(t * 3.117 + phase * 3)));
}
struct SceneLighting::Impl {
  RenderEnvironment env;
  float scale{}, bias{}, maximum{};
  spwi::VoxelSceneTracer tracer;
  struct Mesh {
    const PolygonRenderInstance *instance;
    std::shared_ptr<PolygonBvh> bvh;
    std::shared_ptr<const std::vector<PolygonTextureMipChain>> opacityMips;
  };
  std::vector<Mesh> meshes;
  std::vector<OpticalPolygonInstance> opticalInstances;
  std::unique_ptr<OpticalScene> opticalScene;
  struct Medium {
    VoxelMaterialDefinition material;
    OpticalHit entry;
    bool coverageOnly{};
  };
  using Media = std::vector<Medium>;
  std::vector<Emitter> emitters;
  std::size_t authoredCount{};
  double power{};
  void append(Emitter e) {
    if (e.area != 0)
      e.n = unit(cross(e.u, e.v));
    if (e.area < 0)
      e.area = std::sqrt(dot3(cross(e.u, e.v), cross(e.u, e.v))) *
               (e.kind == Emitter::Triangle ? .5F
                : e.kind == Emitter::Disc   ? pi
                                            : 1.F);
    e.weight =
        double(luminance(e.radiance)) *
        (e.area > 0                ? e.area * pi * (e.twoSided ? 2 : 1)
         : e.kind == Emitter::Spot ? 2 * pi * (1 - .5F * (e.inner + e.outer))
                                   : 4 * pi);
    if (!(e.weight > 0))
      return;
    power += e.weight;
    e.cumulative = power;
    emitters.push_back(e);
  }
  Impl(std::span<const VoxelReferenceInstance> voxels,
       std::span<const PolygonRenderInstance> polygons,
       const RenderEnvironment &e, float s)
      : env(e), scale(resolve_meters_per_voxel(s)),
        bias((std::max)(1e-5F, e.shadowBiasMeters / scale)),
        maximum(e.globalIlluminationMaxDistanceMeters / scale), tracer(voxels) {
    std::string error;
    if (!env.validate(&error))
      throw std::invalid_argument(error);
    for (const auto &l : env.lights) {
      Emitter a;
      a.kind = l.kind == SceneLightKind::Quad   ? Emitter::Quad
               : l.kind == SceneLightKind::Disc ? Emitter::Disc
               : l.kind == SceneLightKind::Spot ? Emitter::Spot
                                                : Emitter::Point;
      a.p = mul(l.positionMeters, 1 / scale);
      a.u = mul(l.edgeU, 1 / scale);
      a.v = mul(l.edgeV, 1 / scale);
      a.n = unit(l.direction);
      a.radiance =
          mul(l.color,
              l.intensity * candle_light_multiplier(l, e.lightingTimeSeconds));
      // Point intensity / squared metric distance; area radiance is scale
      // independent.
      if (a.kind == Emitter::Point || a.kind == Emitter::Spot)
        a.radiance = mul(a.radiance, 1 / (scale * scale));
      a.inner = std::cos(l.innerConeRadians);
      a.outer = std::cos(l.outerConeRadians);
      a.ies = l.iesProfile;
      a.twoSided = l.twoSided;
      a.area =
          (a.kind == Emitter::Quad || a.kind == Emitter::Disc) ? -1.F : 0.F;
      append(a);
    }
    authoredCount = emitters.size();
    std::unordered_map<const CookedPolygonAsset *, std::shared_ptr<PolygonBvh>>
        cache;
    for (const auto &i : polygons) {
      if (!i.visible || !i.asset)
        continue;
      auto &bvh = cache[i.asset];
      if (!bvh) {
        bvh = std::make_shared<PolygonBvh>();
        if (!bvh->build(*i.asset))
          continue;
      }
      Mesh mesh{&i, bvh, {}};
      mesh.opacityMips=surface_mips(*i.asset);
      meshes.push_back(std::move(mesh));
      for (const auto &sm : i.asset->submeshes) {
        if (sm.materialIndex >= i.asset->materials.size())
          continue;
        const auto &material = i.asset->materials[sm.materialIndex];
        const auto le = emission(material);
        if (peak(le) <= 0 || material.blendMode != MaterialBlendMode::Opaque)
          continue;
        for (std::size_t k = sm.firstIndex;
             k + 2 < (std::min<std::size_t>)(i.asset->indices.size(),
                                             std::size_t(sm.firstIndex) +
                                                 sm.indexCount);
             k += 3) {
          const auto ia = i.asset->indices[k], ib = i.asset->indices[k + 1],
                     ic = i.asset->indices[k + 2];
          if ((std::max)({ia, ib, ic}) >= i.asset->vertices.size())
            continue;
          const auto a = transform_point(i.transform,
                                         i.asset->vertices[ia].position),
                     b = transform_point(i.transform,
                                         i.asset->vertices[ib].position),
                     c = transform_point(i.transform,
                                         i.asset->vertices[ic].position);
          if (dot3(cross(sub(b, a), sub(c, a)), cross(sub(b, a), sub(c, a))) <=
              1e-16F)
            continue;
          Emitter light;
          light.kind = Emitter::Triangle;
          light.polygon=static_cast<int>(meshes.size()-1);light.firstIndex=static_cast<std::uint32_t>(k);light.materialIndex=sm.materialIndex;
          light.p = a;
          light.u = sub(b, a);
          light.v = sub(c, a);
          light.area = -1;
          // Match the beauty pass: instance tint scales base colour only, never
          // material.emissive.
          light.radiance =
              material.shadingModel == MaterialShadingModel::Emissive
                  ? sum3(
                        product(base(material), {i.tint.x, i.tint.y, i.tint.z}),
                        material.emissive)
                  : material.emissive;
          light.twoSided = true;
          append(light);
        }
      }
    }
    // Enumerate exposed emitting voxel faces, never internal faces or air. Work
    // follows allocated bricks, not the potentially enormous empty
    // occupied-bounds volume.
    for (const auto &i : voxels) {
      if (!i.visible || !i.object ||
          std::none_of(i.materials.begin(), i.materials.end(),
                       [](const auto &m) { return peak(emission(m)) > 0; }))
        continue;
      for (const auto &entry : i.object->bricks()) {
        const auto o = brick_origin(entry.first);
        for (int z = 0; z < kBrickDim; ++z)
          for (int y = 0; y < kBrickDim; ++y)
            for (int x = 0; x < kBrickDim; ++x) {
              Int3 cell{o.x + x, o.y + y, o.z + z};
              const auto id = i.object->material_at(cell);
              if (id == kAirMaterial || id >= i.materials.size())
                continue;
              const auto &m = i.materials[id];
              const auto le = emission(m);
              if (peak(le) <= 0 || m.blendMode != MaterialBlendMode::Opaque)
                continue;
              const Float3 center{(cell.x + .5F) * i.spacing.x,
                                  (cell.y + .5F) * i.spacing.y,
                                  (cell.z + .5F) * i.spacing.z};
              for (int axis = 0; axis < 3; ++axis)
                for (int sign : {-1, 1}) {
                  Int3 next = cell;
                  if (axis == 0)
                    next.x += sign;
                  else if (axis == 1)
                    next.y += sign;
                  else
                    next.z += sign;
                  if (i.object->occupied_at(next))
                    continue;
                  Float3 n = axis == 0   ? Float3{float(sign), 0, 0}
                             : axis == 1 ? Float3{0, float(sign), 0}
                                         : Float3{0, 0, float(sign)};
                  const auto u = axis == 0   ? Float3{0, i.spacing.y, 0}
                                 : axis == 1 ? Float3{0, 0, i.spacing.z}
                                             : Float3{i.spacing.x, 0, 0};
                  const auto v = axis == 0   ? Float3{0, 0, i.spacing.z}
                                 : axis == 1 ? Float3{i.spacing.x, 0, 0}
                                             : Float3{0, i.spacing.y, 0};
                  const auto off = product(n, mul(i.spacing, .5F));
                  Emitter a;
                  a.kind = Emitter::Quad;
                  a.p = transform_point(i.transform, sum3(center, off));
                  a.u = transform_vector(i.transform, u);
                  a.v = transform_vector(i.transform, mul(v, float(sign)));
                  a.radiance = le;
                  a.area = -1;
                  append(a);
                }
            }
      }
    }

    for (const auto &mesh : meshes) {
      const auto &i = *mesh.instance;
      opticalInstances.push_back({i.objectId, i.asset, i.transform, true});
    }
    opticalScene =
        std::make_unique<OpticalScene>(voxels, opticalInstances, scale);
  }
  std::optional<Hit> trace(Float3 p, Float3 d, float max) const {
    std::optional<Hit> best;
    if (auto h = tracer.trace(p, d, max)) {
      best = Hit{h->objectHit.distance, h->worldPosition, h->worldNormal,
                 tracer.material(h->instance, h->objectHit.material)};
      max = best->distance;
    }
    for (const auto &mesh : meshes) {
      const auto &i = *mesh.instance;
      if (auto h = mesh.bvh->raycast(inverse_transform_point(i.transform, p),
                                     inverse_transform_vector(i.transform, d),
                                     max)) {
        if (h->materialIndex >= i.asset->materials.size())
          continue;
        auto m = i.asset->materials[h->materialIndex];
        m.baseColor.x *= i.tint.x;
        m.baseColor.y *= i.tint.y;
        m.baseColor.z *= i.tint.z;
        m.emissive = product(m.emissive, {i.tint.x, i.tint.y, i.tint.z});
        best = Hit{h->distance, transform_point(i.transform, h->position),
                   unit(transform_vector(i.transform, h->normal)), m};
        max = h->distance;
      }
    }
    // Authored area emitters participate in secondary visibility and MIS even
    // though the raster primary image has no authored area-light proxy
    // geometry.
    for (std::size_t k = 0; k < authoredCount; ++k) {
      const auto &e = emitters[k];
      if (auto t = intersect(e, p, d); t && *t < max) {
        VoxelMaterialDefinition m;
        m.baseColor = {0, 0, 0, 1};
        m.shadingModel = MaterialShadingModel::Emissive;
        m.emissive = (e.twoSided || dot3(e.n, d) < 0) ? e.radiance : Float3{};
        best = Hit{*t, sum3(p, mul(d, *t)), e.n, m};
        max = *t;
      }
    }
    return best;
  }
  Float3 spawn(Float3 p, Float3 geometricNormal, Float3 direction) const {
    const float numerical =
        (std::max)(1e-5F,
                   (std::max)({std::abs(p.x), std::abs(p.y), std::abs(p.z)}) *
                       2e-7F);
    const float epsilon =
        (std::max)(numerical, (std::min)(bias, 1e-4F / scale));
    return offset_ray_origin(
        p, mul(geometricNormal, dp_sign(geometricNormal, direction) * epsilon));
  }
  static float dp_sign(Float3 n, Float3 d) {
    return dot3(n, d) < 0 ? -1.F : 1.F;
  }
  std::optional<Hit> transport_trace(Float3 p, Float3 d, float limit,
                                     const Media &media) const {
    const auto h =
        !media.empty() && media.back().entry.voxelInstance >= 0
            ? opticalScene->trace_inside_voxels(media.back().entry, p, d, limit)
            : opticalScene->trace(p, d, limit);
    std::optional<Hit> result;
    if (h) {
      VoxelMaterialDefinition material;
      if (h->material)
        material = *h->material;
      else if (!media.empty())
        material = media.back().material;
      float cutoff = .5F;
      Float3 detailedNormal=h->shadingNormal;
      if (h->polygonInstance >= 0 &&
          std::size_t(h->polygonInstance) < meshes.size()) {
        const auto &mesh = meshes[h->polygonInstance];
        const auto &i = *mesh.instance;
        PolygonBvhHit coverage;coverage.position=inverse_transform_point(i.transform,h->position);coverage.normal=inverse_transform_vector(i.transform,h->shadingNormal);coverage.firstIndex=h->firstIndex;coverage.materialIndex=h->materialIndex;
        const auto evaluated=sample_polygon_hit_material(*i.asset,coverage,i.transform,h->shadingNormal,*mesh.opacityMips,scale,h->distance*scale*.006F,h->distance*scale);
        material=evaluated.material;detailedNormal=evaluated.normal;
        if (h->materialIndex < i.asset->materialBindings.size())
          cutoff = i.asset->materialBindings[h->materialIndex].alphaCutoff;
        material.baseColor.x *= i.tint.x;
        material.baseColor.y *= i.tint.y;
        material.baseColor.z *= i.tint.z;
        material.baseColor.w *= i.tint.w;
      }
      // An exiting voxel boundary reports what is beyond. Its interface is the
      // current medium.
      if (!media.empty() && !media.back().coverageOnly &&
          h->voxelInstance == media.back().entry.voxelInstance &&
          h->voxelInstance >= 0 &&
          (!h->material || material_is_refractive(*h->material)))
        material = media.back().material;
      if(h->voxelInstance>=0 && std::size_t(h->voxelInstance)<tracer.instances().size()){
        const auto& pose=tracer.instances()[h->voxelInstance].transform;
        if(material.normalMap)detailedNormal=unit(transform_vector(pose,voxel_normal_map_normal(material,mul(inverse_transform_point(pose,h->position),scale),inverse_transform_vector(pose,detailedNormal))));
        apply_material_surface_detail(material,detailedNormal,{mul(inverse_transform_point(pose,h->position),scale),mul(h->position,scale),{},{},false,pose,{1,0,0},{0,1,0},h->distance*scale*.006F});
      }
      result = Hit{h->distance,
                   h->position,
                   detailedNormal,
                   material,
                   h->geometricNormal,
                   *h,
                   true,
                   cutoff};
      limit = h->distance;
    }
    for (std::size_t k = 0; k < authoredCount; ++k) {
      const auto &e = emitters[k];
      if (auto t = intersect(e, p, d); t && *t < limit) {
        VoxelMaterialDefinition m;
        m.baseColor = {0, 0, 0, 1};
        m.shadingModel = MaterialShadingModel::Emissive;
        m.emissive = e.twoSided || dot3(e.n, d) < 0 ? e.radiance : Float3{};
        result = Hit{*t, sum3(p, mul(d, *t)), e.n, m, e.n, {}, false};
        limit = *t;
      }
    }
    return result;
  }
  Media initial_media(Float3 position) const {
    Media result;
    const auto instances = tracer.instances();
    for (std::size_t k = 0; k < instances.size(); ++k) {
      const auto &i = instances[k];
      if (!i.visible || !i.object)
        continue;
      auto p = inverse_transform_point(i.transform, position);
      p = {p.x / i.spacing.x, p.y / i.spacing.y, p.z / i.spacing.z};
      if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
          (std::max)({std::abs(p.x), std::abs(p.y), std::abs(p.z)}) > 2e9F)
        continue;
      Int3 cell{int(std::floor(p.x)), int(std::floor(p.y)),
                int(std::floor(p.z))};
      auto id = i.object->material_at(cell);
      if (id == kAirMaterial || id >= i.materials.size() ||
          !material_is_refractive(i.materials[id]))
        continue;
      OpticalHit entry;
      entry.voxelInstance = static_cast<int>(k);
      entry.materialId = id;
      entry.objectId = i.objectId;
      entry.voxel = cell;
      entry.material = &i.materials[id];
      result.push_back({i.materials[id], entry});
    }
    return result;
  }
  // Coverage-transparent voxel cells are skipped as one contiguous material
  // region. Pushing a temporary record lets the exact DDA find its far
  // boundary, without repeatedly intersecting the occupied entry cell at
  // distance zero.
  bool end_coverage(std::optional<Hit> &hit, Media &media, Float3 &origin,
                    Float3 direction) const {
    if (!hit || media.empty() || !media.back().coverageOnly ||
        hit->optical.voxelInstance != media.back().entry.voxelInstance)
      return false;
    const bool air = hit->optical.material == nullptr;
    media.pop_back();
    if (air) {
      origin = spawn(hit->p, hit->geometricNormal, direction);
      return true;
    }
    hit->geometricNormal = mul(hit->geometricNormal, -1);
    hit->n = mul(hit->n, -1);
    return false;
  }
  struct Interface {
    float incident{1}, transmitted{1};
    bool exiting{}, swap{}, inferredInside{};
  };
  Interface interface(const Hit &hit, Float3 direction,
                      const Media &media) const {
    Interface i;
    i.incident = media.empty() ? 1 : media.back().material.indexOfRefraction;
    const auto &h = hit.optical;
    const bool same =
        !media.empty() &&
        ((h.voxelInstance >= 0 &&
          h.voxelInstance == media.back().entry.voxelInstance) ||
         (h.polygonInstance >= 0 &&
          h.polygonInstance == media.back().entry.polygonInstance));
    if (same) {
      i.exiting = true;
      i.transmitted = media.size() > 1
                          ? media[media.size() - 2].material.indexOfRefraction
                          : 1;
      if (h.voxelInstance >= 0 && h.material &&
          material_is_refractive(*h.material)) {
        i.swap = true;
        i.transmitted = h.material->indexOfRefraction;
      }
    } else if (material_is_refractive(hit.material)) {
      if (media.empty() && dot3(direction, hit.geometricNormal) > 0) {
        i.incident = hit.material.indexOfRefraction;
        i.transmitted = 1;
        i.exiting = true;
        i.inferredInside = true;
      } else
        i.transmitted = hit.material.indexOfRefraction;
    }
    return i;
  }
  void cross_interface(const Hit &hit, const Interface &i, Media &media) const {
    if (i.exiting) {
      if (!media.empty())
        media.pop_back();
      if (i.swap && hit.optical.material)
        media.push_back({*hit.optical.material, hit.optical});
    } else
      media.push_back({hit.material, hit.optical});
  }
  // Straight shadow transmission is a visibility approximation: camera paths
  // refract; caustics still require the separate photon solver. Closed volumes
  // receive both Fresnel factors and Beer-Lambert absorption, rather than
  // looking like opaque blockers.
  Float3 shadow_transmission(Float3 origin, Float3 d, float limit) const {
    Float3 weight{1, 1, 1};
    Media media = initial_media(origin);
    float remaining = limit;
    for (unsigned layer = 0; layer < 64; ++layer) {
      auto hit = transport_trace(origin, d, remaining, media);
      if (!hit)
        return !media.empty() && !media.back().coverageOnly
                   ? product(weight, medium_transmittance(media.back().material,
                                                          remaining, scale))
                   : weight;
      const float distance = hit->distance;
      if (end_coverage(hit, media, origin, d)) {
        remaining -= distance;
        if (remaining <= 0)
          return weight;
        continue;
      }
      const auto &m = hit->material;
      if (!media.empty() && !media.back().coverageOnly)
        weight = product(weight, medium_transmittance(media.back().material,
                                                      hit->distance, scale));
      if (!material_is_refractive(m)) {
        const float opacity = std::clamp(m.baseColor.w, 0.F, 1.F);
        if (m.blendMode == MaterialBlendMode::Masked &&
            opacity < hit->alphaCutoff) {
        } else if (m.blendMode == MaterialBlendMode::Translucent)
          weight = mul(weight, 1 - opacity);
        else
          return {};
        if (hit->optical.voxelInstance >= 0 &&
            dot3(hit->geometricNormal, d) < 0)
          media.push_back({m, hit->optical, true});
      } else {
        const auto i = interface(*hit, d, media);
        const auto f = material_interface_reflectance(m,std::abs(dot3(unit(hit->n), d)),i.incident,i.transmitted);
        weight = product(weight,mul({1-f.x,1-f.y,1-f.z},m.transmission));
        if (i.inferredInside)
          weight =
              product(weight, medium_transmittance(m, hit->distance, scale));
        cross_interface(*hit, i, media);
      }
      if (peak(weight) < 1e-6F)
        return {};
      const auto next = spawn(hit->p, hit->geometricNormal, d);
      remaining -=
          hit->distance + std::sqrt(dot3(sub(next, hit->p), sub(next, hit->p)));
      if (remaining <= 0)
        return weight;
      origin = next;
    }
    return {};
  }
  float light_pdf(Float3 from, const Hit &h) const {
    const auto delta = sub(h.p, from);
    const float d2 = dot3(delta, delta);
    const auto dir = unit(delta);
    double pdf = 0;
    for (const auto &e : emitters) {
      if (e.area <= 0)
        continue;
      if (auto t = intersect(e, from, dir);
          t && std::abs(*t - std::sqrt(d2)) <
                   (std::max)(1e-4F, std::sqrt(d2) * 1e-4F)) {
        const float c = dot3(e.n, mul(dir, -1));
        if (e.twoSided ? std::abs(c) > 1e-7F : c > 1e-7F)
          pdf += e.weight / power * d2 / (e.area * std::abs(c));
      }
    }
    return float(pdf);
  }
  ShadingLobes direct(Float3 p, Float3 n, Float3 view,
                      const VoxelMaterialDefinition &m, Rng &rng, bool useMis,
                      float etaI = 1, float etaT = 1.5F) const {
    ShadingLobes out;
    if (power <= 0)
      return out;
    const auto origin = p;
    const unsigned count = env.localLightSamples;
    for (unsigned sample = 0; sample < count; ++sample) {
      const double r = rng.next() * power;
      auto it = std::upper_bound(
          emitters.begin(), emitters.end(), r,
          [](double a, const Emitter &b) { return a < b.cumulative; });
      if (it == emitters.end())
        it = emitters.end() - 1;
      const auto &e = *it;
      Float3 q = e.p;
      if (e.area > 0) {
        float a = rng.next(), b = rng.next();
        if (e.kind == Emitter::Disc) {
          float radius = std::sqrt(a), angle = 2 * pi * b;
          a = radius * std::cos(angle);
          b = radius * std::sin(angle);
        } else if (e.kind == Emitter::Triangle) {
          float root = std::sqrt(a);
          a = 1 - root;
          b *= root;
        } else {
          a -= .5F;
          b -= .5F;
        }
        q = sum3(q, sum3(mul(e.u, a), mul(e.v, b)));
      }
      const auto delta = sub(q, origin);
      const float d2 = dot3(delta, delta);
      if (d2 < 1e-12F)
        continue;
      const float distance = std::sqrt(d2);
      const auto d = mul(delta, 1 / distance);

      const float c = dot3(e.n, mul(d, -1));
      if (e.area > 0 && (!e.twoSided && c <= 0))
        continue;
      float pdf = float(e.weight / power), factor = 1 / d2;
      if (e.area > 0) {
        if (std::abs(c) < 1e-7F)
          continue;
        pdf *= d2 / (e.area * std::abs(c));
        factor =
            useMis ? mis(float(count) * pdf,
                         evaluate_material_bsdf(m, n, view, d, etaI, etaT).pdf)
                   : 1.F;
      }
      if (e.kind == Emitter::Spot) {
        const float angle = dot3(e.n, mul(d, -1));
        const float t =
            e.inner > e.outer
                ? std::clamp((angle - e.outer) / (e.inner - e.outer), 0.F, 1.F)
                : (angle >= e.outer ? 1.F : 0.F);
        factor *= t * t * (3 - 2 * t);
      }
      if (e.ies && e.area <= 0)
        factor *= e.ies->sample(std::acos(std::clamp(c, -1.F, 1.F)));
      if (factor <= 0)
        continue;
      const auto shadowOrigin = spawn(origin, n, d);
      const auto shadowDelta = sub(q, shadowOrigin);
      const float shadowDistance = std::sqrt(dot3(shadowDelta, shadowDelta));
      const auto visibility = shadow_transmission(
          shadowOrigin, unit(shadowDelta),
          (std::max)(0.F, shadowDistance -
                              (std::max)(1e-5F, shadowDistance * 1e-5F)));
      auto radiance=e.radiance;
      if(e.polygon>=0 && std::size_t(e.polygon)<meshes.size()){
        const auto& mesh=meshes[e.polygon];const auto& instance=*mesh.instance;
        PolygonBvhHit hit;hit.position=inverse_transform_point(instance.transform,q);hit.firstIndex=e.firstIndex;hit.materialIndex=e.materialIndex;
        auto value=sample_polygon_hit_material(*instance.asset,hit,instance.transform,e.n,*mesh.opacityMips,scale,0,distance*scale).material;
        value.baseColor.x*=instance.tint.x;value.baseColor.y*=instance.tint.y;value.baseColor.z*=instance.tint.z;
        radiance=emission(value);
      }
      const auto li = product(mul(radiance, factor / pdf / float(count)), visibility);
      auto l = evaluate_material_bsdf(m, n, view, d, etaI, etaT).value;
      l.diffuse = product(l.diffuse, li);
      l.specular = product(l.specular, li);
      out.diffuse = sum3(out.diffuse, l.diffuse);
      out.specular = sum3(out.specular, l.specular);
    }
    return out;
  }
  float sun_visibility(Float3 p, Float3 normal, Rng &rng) const {
    if (env.shadowMode == ShadowMode::Off || env.shadowStrength == 0)
      return 1;
    const auto sun = unit(env.sunDirection);
    const auto t = unit(
        cross(std::abs(sun.y) < .9F ? Float3{0, 1, 0} : Float3{1, 0, 0}, sun));
    const auto b = cross(sun, t);
    const bool soft = env.shadowMode == ShadowMode::Soft ||
                      env.shadowMode == ShadowMode::Hybrid;
    const unsigned samples = soft ? env.shadowSamples : 1;
    unsigned blocked = 0;
    for (unsigned k = 0; k < samples; ++k) {
      auto direction = sun;
      if (soft) {
        float r = std::tan(env.shadowSoftnessRadians) * std::sqrt(rng.next()),
              a = 2 * pi * rng.next();
        direction = unit(
            sum3(sun, sum3(mul(t, r * std::cos(a)), mul(b, r * std::sin(a)))));
      }
      const float maximum = (env.shadowMode == ShadowMode::Contact
                                 ? env.contactShadowDistanceMeters
                                 : env.shadowMaxDistanceMeters) /
                            scale;
      if (trace(sum3(p, mul(normal, bias)), direction, maximum))
        ++blocked;
    }
    return 1 - env.shadowStrength * float(blocked) / float(samples);
  }
  Float3
  scattering(Float3 p, Float3 ray, float g, Rng &rng,
             const std::function<float(Float3, float)> &transmission) const {
    Float3 result{};
    if (power <= 0)
      return result;
    g = std::clamp(g, -.95F, .95F);
    for (unsigned k = 0; k < env.localLightSamples; ++k) {
      auto it = std::upper_bound(
          emitters.begin(), emitters.end(), rng.next() * power,
          [](double a, const Emitter &b) { return a < b.cumulative; });
      if (it == emitters.end())
        it = emitters.end() - 1;
      const auto &e = *it;
      auto q = e.p;
      if (e.area > 0) {
        float a = rng.next(), b = rng.next();
        if (e.kind == Emitter::Disc) {
          float r = std::sqrt(a), t = 2 * pi * b;
          a = r * std::cos(t);
          b = r * std::sin(t);
        } else if (e.kind == Emitter::Triangle) {
          float r = std::sqrt(a);
          a = 1 - r;
          b *= r;
        } else {
          a -= .5F;
          b -= .5F;
        }
        q = sum3(q, sum3(mul(e.u, a), mul(e.v, b)));
      }
      const auto delta = sub(q, p);
      const float d2 = dot3(delta, delta);
      if (d2 < 1e-10F)
        continue;
      const float distance = std::sqrt(d2);
      const auto d = mul(delta, 1 / distance);
      const float c = dot3(e.n, mul(d, -1));
      float factor = 1 / d2, pdf = float(e.weight / power);
      if (e.area > 0) {
        if ((!e.twoSided && c <= 0) || std::abs(c) < 1e-7F)
          continue;
        factor = e.area * std::abs(c) / d2;
      }
      if (e.kind == Emitter::Spot) {
        float t =
            e.inner > e.outer
                ? std::clamp((c - e.outer) / (e.inner - e.outer), 0.F, 1.F)
                : (c >= e.outer ? 1.F : 0.F);
        factor *= t * t * (3 - 2 * t);
      }
      if (e.ies && e.area <= 0)
        factor *= e.ies->sample(std::acos(std::clamp(c, -1.F, 1.F)));
      if (trace(p, d, distance - (std::max)(1e-5F, distance * 1e-5F)))
        continue;
      if (transmission)
        factor *= std::clamp(transmission(d, distance), 0.F, 1.F);
      const float denominator =
          (std::max)(1e-6F, 1 + g * g - 2 * g * dot3(unit(ray), d));
      factor *= (1 - g * g) / (4 * pi * denominator * std::sqrt(denominator));
      result = sum3(
          result, mul(e.radiance, factor / pdf / float(env.localLightSamples)));
    }
    return result;
  }
  struct PathValue {
    Float3 radiance{};
    bool firstTransmission{}, firstSpecular{};
  };
  PathValue path(Float3 origin, Float3 direction, float rayMaximum, Rng &rng,
                 unsigned depthLimit, float previousPdf = 0,
                 bool previousDelta = true, bool suppressFirstEmitter = false,
                 const VoxelMaterialDefinition *primaryOverride = nullptr,
                 Float3 primaryNormal = {},
                 float primaryDistance = -1.0F) const {
    PathValue out;
    Float3 throughput{1, 1, 1};
    Media media = initial_media(origin);
    Float3 previousPoint = origin;
    float etaScale = 1;
    unsigned depth = 0, skips = 0;
    while (depth <= depthLimit && skips < 128) {
      auto hit = transport_trace(origin, direction, rayMaximum, media);
      if (hit && depth == 0 && skips == 0 && primaryOverride) {
        hit->material = *primaryOverride;
        // Only trust the caller's (e.g. normal-mapped raster) normal when it
        // belongs to the surface actually hit first. Facing is irrelevant: the
        // normal is flipped toward the incoming ray below.
        const bool sameSurface =
            primaryDistance < 0 ||
            std::abs(hit->distance - primaryDistance) <=
                (std::max)(1e-4F, primaryDistance * 1e-3F);
        if (sameSurface && dot3(primaryNormal, primaryNormal) > 0)
          hit->n = unit(primaryNormal);
      }
      if (!hit) {
        out.radiance = sum3(
            out.radiance,
            product(throughput, spwi::environment_radiance(env, direction)));
        break;
      }
      if (end_coverage(hit, media, origin, direction)) {
        ++skips;
        continue;
      }
      const auto &m = hit->material;
      if (!media.empty() && !media.back().coverageOnly)
        throughput =
            product(throughput, medium_transmittance(media.back().material,
                                                     hit->distance, scale));
      const float opacity = std::clamp(m.baseColor.w, 0.F, 1.F);
      const bool skip = (m.blendMode == MaterialBlendMode::Masked &&
                         opacity < hit->alphaCutoff) ||
                        (m.blendMode == MaterialBlendMode::Translucent &&
                         rng.next() >= opacity);
      if (skip) {
        if (hit->optical.voxelInstance >= 0 &&
            dot3(hit->geometricNormal, direction) < 0)
          media.push_back({m, hit->optical, true});
        origin = spawn(hit->p, hit->geometricNormal, direction);
        ++skips;
        continue;
      }
      auto normal = unit(hit->n), geometric = unit(hit->geometricNormal);
      if (dot3(normal, direction) > 0)
        normal = mul(normal, -1);
      const auto info = interface(*hit, direction, media);
      if (info.inferredInside)
        throughput =
            product(throughput, medium_transmittance(m, hit->distance, scale));
      auto le =
          m.shadingModel == MaterialShadingModel::Unlit ? base(m) : emission(m);
      const float lightPdf = light_pdf(previousPoint, *hit);
      if (lightPdf > 0 && !previousDelta)
        le = mul(le, (depth == 0 && suppressFirstEmitter)
                         ? 0
                         : mis(previousPdf,
                               float(env.localLightSamples) * lightPdf));
      out.radiance = sum3(out.radiance, product(throughput, le));
      if (m.shadingModel == MaterialShadingModel::Emissive ||
          m.shadingModel == MaterialShadingModel::Unlit)
        break;
      // Camera/path rays through closed subsurface volumes use the existing bounded
      // random-walk solver. Sample one RGB channel, reweight by three: absorption and
      // scattering length remain spectral instead of tinting an opaque surface.
      if (material_has_subsurface(m) && dot3(geometric,direction)<0 && depth<depthLimit) {
        std::unique_ptr<SubsurfaceDomain> domain;
        const auto& optical=hit->optical;
        if(optical.polygonInstance>=0 && std::size_t(optical.polygonInstance)<meshes.size()) {
          const auto& mesh=meshes[optical.polygonInstance];
          domain=std::make_unique<PolygonSubsurfaceDomain>(*mesh.bvh,*mesh.instance->asset,mesh.instance->transform);
        } else if(optical.voxelInstance>=0 && std::size_t(optical.voxelInstance)<tracer.instances().size()) {
          domain=std::make_unique<VoxelSubsurfaceDomain>(tracer.instances()[optical.voxelInstance],optical.materialId);
        }
        if(domain) {
          auto boundaryMaterial=m;boundaryMaterial.transmission=1;
          const auto boundarySample=sample_material_bsdf(boundaryMaterial,geometric,mul(direction,-1),1,m.indexOfRefraction,0,rng.next(),rng.next(),rng.next());
          if(!boundarySample.valid)break;
          const bool reflected=!boundarySample.transmitted;
          throughput=product(throughput,boundarySample.weight);
          // The solver samples exit Fresnel in importance form; cancel the entry
          // radiance eta factor for the complete outside-to-outside volume event.
          if(!reflected)throughput=mul(throughput,m.indexOfRefraction*m.indexOfRefraction);
          Float3 continuationPoint=hit->p;
          float continuationPdf=1;
          if(reflected) {
            direction=boundarySample.direction;
            origin=spawn(hit->p,geometric,direction);
          } else {
            const unsigned channel=(std::min)(2U,unsigned(rng.next()*3));
            const float albedos[]{m.subsurfaceColor.x*m.baseColor.x,m.subsurfaceColor.y*m.baseColor.y,m.subsurfaceColor.z*m.baseColor.z};
            auto medium=subsurface_medium_from_albedo(albedos[channel],m.subsurfaceScatterDistanceMeters,scale);
            medium.boundaryIndex=m.indexOfRefraction;
            RandomWalkSettings settings;settings.maximumEvents=256;settings.surfaceOffset=(std::max)(1e-7F,(std::min)(1e-5F/scale,m.subsurfaceScatterDistanceMeters*.001F/scale));
            const auto walk=random_walk_subsurface(*domain,medium,hit->p,geometric,boundarySample.direction,[&]{return rng.next();},settings);
            if(walk.end!=RandomWalkResult::End::Exited)break;
            throughput=product(throughput,channel==0?Float3{3,0,0}:channel==1?Float3{0,3,0}:Float3{0,0,3});
            // Diffuse exit approximation: use the sampled spatial endpoint, then
            // Lambertian exit scattering. This admits next-event estimates for sun
            // and finite emitters; it is not the solver's exact exit-angle law.
            const auto exitNormal=unit(walk.normal);
            VoxelMaterialDefinition exitMaterial;exitMaterial.baseColor={1,1,1,1};exitMaterial.specular=0;
            const auto localExit=direct(walk.position,exitNormal,exitNormal,exitMaterial,rng,true);
            auto exitLight=sum3(localExit.diffuse,localExit.specular);
            if(env.sunIntensity>0) {
              const auto sun=unit(env.sunDirection);
              const auto visibility=shadow_transmission(spawn(walk.position,exitNormal,sun),sun,env.shadowMaxDistanceMeters/scale);
              exitLight=sum3(exitLight,product(mul(env.sunColor,env.sunIntensity*(std::max)(0.F,dot3(exitNormal,sun))),visibility));
            }
            out.radiance=sum3(out.radiance,product(throughput,exitLight));
            const float u=rng.next(),phi=2*pi*rng.next(),radius=std::sqrt(u);
            const auto tangent=unit(cross(std::abs(exitNormal.y)<.9F?Float3{0,1,0}:Float3{1,0,0},exitNormal));
            const auto bitangent=cross(exitNormal,tangent);
            direction=unit(sum3(sum3(mul(tangent,radius*std::cos(phi)),mul(bitangent,radius*std::sin(phi))),mul(exitNormal,std::sqrt(1-u))));
            origin=spawn(walk.position,exitNormal,direction);
            continuationPoint=walk.position;continuationPdf=(std::max)(0.F,dot3(exitNormal,direction))/pi;
          }
          if(depth==0)out.firstSpecular=reflected;
          previousPoint=continuationPoint;previousPdf=continuationPdf;previousDelta=reflected;
          rayMaximum=maximum;++depth;continue;
        }
      }
      const auto view = mul(direction, -1);
      const bool continuing = depth < depthLimit;
      const auto local = direct(hit->p, normal, view, m, rng, continuing,
                                info.incident, info.transmitted);
      auto lit = sum3(local.diffuse, local.specular);
      if (env.sunIntensity > 0) {
        const auto sun = unit(env.sunDirection);
        const auto value =
            evaluate_material_bsdf(m, normal, view, sun, info.incident,
                                   info.transmitted)
                .value;
        const auto visibility =
            shadow_transmission(spawn(hit->p, geometric, sun), sun,
                                env.shadowMaxDistanceMeters / scale);
        lit =
            sum3(lit, product(sum3(value.diffuse, value.specular),
                              product(mul(env.sunColor, env.sunIntensity * pi),
                                      visibility)));
      }
      out.radiance = sum3(out.radiance, product(throughput, lit));
      if (!continuing)
        break;
      const auto sampled =
          sample_material_bsdf(m, normal, view, info.incident, info.transmitted,
                               rng.next(), rng.next(), rng.next(), rng.next());
      if (!sampled.valid)
        break;
      // Shading normals must not send a ray to the wrong geometric side.
      const float sideBefore = dot3(geometric, view),
                  sideAfter = dot3(geometric, sampled.direction);
      if (sampled.transmitted ? sideBefore * sideAfter >= 0
                              : sideBefore * sideAfter <= 0)
        break;
      if (depth == 0) {
        out.firstTransmission = sampled.transmitted;
        out.firstSpecular = sampled.delta || peak(sampled.diffuseShare) < .5F;
      }
      throughput = product(throughput, sampled.weight);
      if (sampled.transmitted) {
        cross_interface(*hit, info, media);
        etaScale *= info.transmitted * info.transmitted /
                    (info.incident * info.incident);
      }
      previousPoint = hit->p;
      previousPdf = sampled.pdf;
      previousDelta = sampled.delta;
      direction = sampled.direction;
      origin = spawn(hit->p, geometric, direction);
      rayMaximum = maximum;
      ++depth;
      if (env.pathTraceRoulette && depth >= 3) {
        const float survival =
            std::clamp(peak(throughput) * etaScale, .05F, .95F);
        if (rng.next() >= survival)
          break;
        throughput = mul(throughput, 1 / survival);
      }
      if (peak(throughput) <= 0)
        break;
    }
    return out;
  }
  ShadingLobes indirect(Float3 p, Float3 n, Float3 view,
                        const VoxelMaterialDefinition &primary,
                        Rng &rng) const {
    ShadingLobes sum;
    for (unsigned sample = 0; sample < env.pathTraceSamples; ++sample) {
      const auto sampled =
          sample_material_bsdf(primary, n, view, 1, primary.indexOfRefraction,
                               rng.next(), rng.next(), rng.next(), rng.next());
      if (!sampled.valid)
        continue;
      auto value = path(spawn(p, n, sampled.direction), sampled.direction,
                        maximum, rng, env.pathTraceBounces - 1, sampled.pdf,
                        sampled.delta, !sampled.delta)
                       .radiance;
      value = product(value, sampled.weight);
      const auto diffuse = product(value, sampled.diffuseShare);
      sum.diffuse = sum3(sum.diffuse, diffuse);
      sum.specular = sum3(sum.specular, sub(value, diffuse));
    }
    const float gain =
        env.globalIlluminationIntensity / float(env.pathTraceSamples);
    return {mul(sum.diffuse, gain), mul(sum.specular, gain)};
  }
  LightPasses camera(Float3 origin, Float3 direction, float maximumDistance,
                     Rng &rng, const VoxelMaterialDefinition *primaryMaterial,
                     Float3 primaryNormal, float primaryDistance) const {
    LightPasses out;
    const unsigned depth =
        env.pathTraceBounces > 0 ? (std::max)(env.pathTraceBounces, 4U) : 12U;
    for (unsigned sample = 0; sample < env.pathTraceSamples; ++sample) {
      const auto value =
          path(origin, unit(direction), maximumDistance, rng, depth, 0, true,
               false, primaryMaterial, primaryNormal, primaryDistance);
      if (value.firstTransmission)
        out.transmission = sum3(out.transmission, value.radiance);
      else if (value.firstSpecular)
        out.indirectSpecular = sum3(out.indirectSpecular, value.radiance);
      else
        out.directDiffuse = sum3(out.directDiffuse, value.radiance);
    }
    const float gain = 1 / float(env.pathTraceSamples);
    out.transmission = mul(out.transmission, gain);
    out.indirectSpecular = mul(out.indirectSpecular, gain);
    out.directDiffuse = mul(out.directDiffuse, gain);
    return out;
  }
};
SceneLighting::SceneLighting(std::span<const VoxelReferenceInstance> v,
                             std::span<const PolygonRenderInstance> p,
                             const RenderEnvironment &e, float s)
    : impl_(std::make_unique<Impl>(v, p, e, s)) {}
SceneLighting::~SceneLighting() = default;
std::size_t SceneLighting::emitter_count() const noexcept {
  return impl_->emitters.size();
}
LightPasses SceneLighting::camera_transport(
    Float3 origin, Float3 direction, float limit, std::uint32_t seed,
    const VoxelMaterialDefinition *material, Float3 normal,
    float distance) const {
  Rng rng{seed ^ 0x245fa213U};
  return impl_->camera(origin, direction, limit, rng, material, normal,
                       distance);
}
float SceneLighting::sun_visibility(Float3 p, Float3 n,
                                    std::uint32_t seed) const {
  Rng rng{seed ^ 0x8a91c38dU};
  return impl_->sun_visibility(p, unit(n), rng);
}
ShadingLobes SceneLighting::direct(Float3 p, Float3 n, Float3 v,
                                   const VoxelMaterialDefinition &m,
                                   std::uint32_t seed) const {
  if (m.shadingModel == MaterialShadingModel::Unlit ||
      m.shadingModel == MaterialShadingModel::Emissive)
    return {};
  Rng r{seed};
  return impl_->direct(p, unit(n), unit(v), m, r, false);
}
ShadingLobes SceneLighting::indirect_lobes(Float3 p, Float3 n, Float3 v,
                                           const VoxelMaterialDefinition &m,
                                           std::uint32_t seed) const {
  if (!impl_->env.pathTraceBounces ||
      impl_->env.globalIlluminationMode == GlobalIlluminationMode::Off ||
      m.shadingModel == MaterialShadingModel::Unlit ||
      m.shadingModel == MaterialShadingModel::Emissive)
    return {};
  Rng r{seed ^ 0xa73b5e19U};
  return impl_->indirect(p, unit(n), unit(v), m, r);
}

Float3 SceneLighting::indirect(Float3 p, Float3 n, Float3 v,
                               const VoxelMaterialDefinition &m,
                               std::uint32_t seed) const {
  const auto l = indirect_lobes(p, n, v, m, seed);
  return sum3(l.diffuse, l.specular);
}

Float3
SceneLighting::scattering(Float3 p, Float3 ray, float g, std::uint32_t seed,
                          const std::function<float(Float3, float)> &t) const {
  Rng rng{seed};
  return impl_->scattering(p, ray, g, rng, t);
}

bool apply_cpu_bloom(PolygonRenderTarget &target, const RenderEnvironment &e) {
  if (!target.valid() || !std::isfinite(e.bloomThreshold) ||
      e.bloomThreshold < 0 || !std::isfinite(e.bloomIntensity) ||
      e.bloomIntensity < 0 || !std::isfinite(e.bloomRadius) ||
      e.bloomRadius < 0)
    return false;
  if (e.bloomIntensity == 0 || e.bloomRadius == 0)
    return true;
  const int radius = int((std::min)(64.F, std::ceil(e.bloomRadius)));
  const float sigma = (std::max)(.5F, float(radius) * .5F);
  std::vector<float> kernel(2 * radius + 1);
  float total = 0;
  for (int j = -radius; j <= radius; ++j) {
    float w = std::exp(-.5F * j * j / (sigma * sigma));
    kernel[j + radius] = w;
    total += w;
  }
  for (auto &w : kernel)
    w /= total;
  std::vector<Float3> source(target.hdrColor.size()), temp(source.size());
  for (std::size_t i = 0; i < source.size(); ++i) {
    auto c = target.hdrColor[i];
    if (!std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z))
      return false;
    float l = luminance({c.x, c.y, c.z});
    source[i] = mul({c.x, c.y, c.z},
                    l > 0 ? (std::max)(0.F, l - e.bloomThreshold) / l : 0);
  }
  for (int axis = 0; axis < 2; ++axis) {
    for (unsigned y = 0; y < target.height; ++y)
      for (unsigned x = 0; x < target.width; ++x) {
        Float3 s{};
        for (int j = -radius; j <= radius; ++j) {
          int xx = int(x) + (axis == 0 ? j : 0),
              yy = int(y) + (axis == 1 ? j : 0);
          if (xx < 0 || yy < 0 || xx >= int(target.width) ||
              yy >= int(target.height))
            continue;
          s = sum3(s, mul(source[std::size_t(yy) * target.width + xx],
                          kernel[j + radius]));
        }
        temp[std::size_t(y) * target.width + x] = s;
      }
    source.swap(temp);
  }
  for (std::size_t i = 0; i < source.size(); ++i) {
    auto glow = mul(source[i], e.bloomIntensity);
    auto &c = target.hdrColor[i];
    c.x += glow.x;
    c.y += glow.y;
    c.z += glow.z;
    if (target.passes.enabled)
      target.passes.emission[i] = sum3(target.passes.emission[i], glow);
  }
  return true;
}
} // namespace dve::render
