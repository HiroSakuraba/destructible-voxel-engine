#include "dve/render/scene_lighting.hpp"
#include "dve/polygon_bvh.hpp"
#include "dve/render/radiance_cascades_spwi.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
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
float peak(Float3 a) { return std::max({a.x, a.y, a.z}); }
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
Float3 cosine(Float3 n, Rng &r) {
  const float z = std::sqrt(r.next()), a = 2 * pi * r.next();
  const float q = std::sqrt(std::max(0.F, 1 - z * z));
  const auto t =
      unit(cross(std::abs(n.y) < .9F ? Float3{0, 1, 0} : Float3{1, 0, 0}, n));
  return sum3(mul(n, z),
              sum3(mul(t, q * std::cos(a)), mul(cross(n, t), q * std::sin(a))));
}
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
};
struct Hit {
  float distance{};
  Float3 p{}, n{};
  VoxelMaterialDefinition material;
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
  };
  std::vector<Mesh> meshes;
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
        bias(std::max(1e-5F, e.shadowBiasMeters / scale)),
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
      meshes.push_back({&i, bvh});
      for (const auto &sm : i.asset->submeshes) {
        if (sm.materialIndex >= i.asset->materials.size())
          continue;
        const auto &material = i.asset->materials[sm.materialIndex];
        const auto le = emission(material);
        if (peak(le) <= 0 || material.blendMode != MaterialBlendMode::Opaque)
          continue;
        for (std::size_t k = sm.firstIndex;
             k + 2 <
             std::min<std::size_t>(i.asset->indices.size(),
                                   std::size_t(sm.firstIndex) + sm.indexCount);
             k += 3) {
          const auto ia = i.asset->indices[k], ib = i.asset->indices[k + 1],
                     ic = i.asset->indices[k + 2];
          if (std::max({ia, ib, ic}) >= i.asset->vertices.size())
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
          light.p = a;
          light.u = sub(b, a);
          light.v = sub(c, a);
          light.area = -1;
          light.radiance = product(le, {i.tint.x, i.tint.y, i.tint.z});
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
  float light_pdf(Float3 from, const Hit &h) const {
    const auto delta = sub(h.p, from);
    const float d2 = dot3(delta, delta);
    const auto dir = unit(delta);
    double pdf = 0;
    for (const auto &e : emitters) {
      if (e.area <= 0)
        continue;
      if (auto t = intersect(e, from, dir);
          t &&
          std::abs(*t - h.distance) < std::max(1e-4F, h.distance * 1e-4F)) {
        const float c = dot3(e.n, mul(dir, -1));
        if (e.twoSided ? std::abs(c) > 1e-7F : c > 1e-7F)
          pdf += e.weight / power * d2 / (e.area * std::abs(c));
      }
    }
    return float(pdf);
  }
  ShadingLobes direct(Float3 p, Float3 n, Float3 view,
                      const VoxelMaterialDefinition &m, Rng &rng,
                      bool useMis) const {
    ShadingLobes out;
    if (power <= 0)
      return out;
    const auto origin = sum3(p, mul(n, bias));
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
      if (dot3(n, d) <= 0)
        continue;
      const float c = dot3(e.n, mul(d, -1));
      if (e.area > 0 && (!e.twoSided && c <= 0))
        continue;
      float pdf = float(e.weight / power), factor = 1 / d2;
      if (e.area > 0) {
        if (std::abs(c) < 1e-7F)
          continue;
        pdf *= d2 / (e.area * std::abs(c));
        factor = useMis
                     ? mis(float(count) * pdf, std::max(0.F, dot3(n, d)) / pi)
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
      if (factor <= 0 ||
          trace(origin, d,
                std::max(0.F, distance - std::max(1e-5F, distance * 1e-5F))))
        continue;
      const auto li = mul(e.radiance, factor / pdf / float(count));
      const auto l =
          m.shadingModel == MaterialShadingModel::ClearCoat
              ? shade_clear_coat_direct(n, view, d, li, base(m), m.metallic,
                                        m.roughness, m.specular, m.clearCoat,
                                        m.clearCoatRoughness)
              : shade_direct_light(n, view, d, li, base(m), m.metallic,
                                   m.roughness, m.specular);
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
      if (trace(p, d, distance - std::max(1e-5F, distance * 1e-5F)))
        continue;
      if (transmission)
        factor *= std::clamp(transmission(d, distance), 0.F, 1.F);
      const float denominator =
          std::max(1e-6F, 1 + g * g - 2 * g * dot3(unit(ray), d));
      factor *= (1 - g * g) / (4 * pi * denominator * std::sqrt(denominator));
      result = sum3(
          result, mul(e.radiance, factor / pdf / float(env.localLightSamples)));
    }
    return result;
  }
  ShadingLobes indirect(Float3 p, Float3 n, Float3 view,
                        const VoxelMaterialDefinition &primary,
                        Rng &rng) const {
    ShadingLobes sum;
    for (unsigned sample = 0; sample < env.pathTraceSamples; ++sample) {
      Float3 diffuseShare{};
      Float3 throughput{1, 1, 1}, result{};
      auto m = primary;
      auto position = p, normal = n, v = view;
      for (unsigned depth = 0; depth < env.pathTraceBounces; ++depth) {
        const auto d = cosine(normal, rng);
        const float cosTheta = std::max(0.F, dot3(normal, d));
        if (cosTheta <= 0)
          break;
        // Cosine sampling covers the entire hemisphere: diffuse and rough
        // GGX/coat are unbiased, though glossy paths can have high variance.
        const auto lobes =
            m.shadingModel == MaterialShadingModel::ClearCoat
                ? shade_clear_coat_direct(normal, v, d, {1, 1, 1}, base(m),
                                          m.metallic, m.roughness, m.specular,
                                          m.clearCoat, m.clearCoatRoughness)
                : shade_direct_light(normal, v, d, {1, 1, 1}, base(m),
                                     m.metallic, m.roughness, m.specular);
        if (depth == 0) {
          const auto total = sum3(lobes.diffuse, lobes.specular);
          diffuseShare = {total.x > 0 ? lobes.diffuse.x / total.x : 0,
                          total.y > 0 ? lobes.diffuse.y / total.y : 0,
                          total.z > 0 ? lobes.diffuse.z / total.z : 0};
        }
        throughput =
            product(throughput,
                    mul(sum3(lobes.diffuse, lobes.specular), pi / cosTheta));
        if (peak(throughput) <= 0)
          break;
        const auto origin = sum3(position, mul(normal, bias));
        const auto hit = trace(origin, d, maximum);
        if (!hit) {
          result = sum3(
              result, product(throughput, spwi::environment_radiance(env, d)));
          break;
        }
        auto le = emission(hit->material);
        const float lp = light_pdf(origin, *hit);
        if (lp > 0)
          le = depth == 0 ? Float3{}
                          : mul(le, mis(cosTheta / pi,
                                        float(env.localLightSamples) * lp));
        result = sum3(result, product(throughput, le));
        if (hit->material.shadingModel == MaterialShadingModel::Emissive ||
            hit->material.shadingModel == MaterialShadingModel::Unlit)
          break;
        m = hit->material;
        position = hit->p;
        normal = unit(hit->n);
        if (dot3(normal, d) > 0)
          normal = mul(normal, -1);
        v = mul(d, -1);
        // At the terminal vertex there is no competing continuation sample.
        const bool continuation = depth + 1 < env.pathTraceBounces;
        const auto local = direct(position, normal, v, m, rng, continuation);
        Float3 lighting = sum3(local.diffuse, local.specular);
        const auto sun = unit(env.sunDirection);
        if (env.sunIntensity > 0 && dot3(normal, sun) > 0) {
          const auto l = shade_direct_light(
              normal, v, sun,
              mul(env.sunColor, env.sunIntensity * pi *
                                    sun_visibility(position, normal, rng)),
              base(m), m.metallic, m.roughness, m.specular);
          lighting = sum3(lighting, sum3(l.diffuse, l.specular));
        }
        result = sum3(result, product(throughput, lighting));
        if (env.pathTraceRoulette && depth >= 2) {
          const float survival = std::clamp(peak(throughput), .05F, .95F);
          if (rng.next() >= survival)
            break;
          throughput = mul(throughput, 1 / survival);
        }
      }
      const auto diffuse = product(result, diffuseShare);
      sum.diffuse = sum3(sum.diffuse, diffuse);
      sum.specular = sum3(sum.specular, sub(result, diffuse));
    }
    const float gain =
        env.globalIlluminationIntensity / float(env.pathTraceSamples);
    return {mul(sum.diffuse, gain), mul(sum.specular, gain)};
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
  const int radius = int(std::min(64.F, std::ceil(e.bloomRadius)));
  const float sigma = std::max(.5F, float(radius) * .5F);
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
                    l > 0 ? std::max(0.F, l - e.bloomThreshold) / l : 0);
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
