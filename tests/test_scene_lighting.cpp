#include "dve/component.hpp"
#include "dve/render/cpu_fog.hpp"
#include "dve/render/scene_lighting.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace dve;
using namespace dve::render;
namespace {
void check(bool ok, const char *msg) {
  if (!ok)
    throw std::runtime_error(msg);
}
void near(float a, float b, float eps, const char *msg) {
  if (std::abs(a - b) > eps) {
    std::cerr << a << " != " << b << '\n';
    throw std::runtime_error(msg);
  }
}
RenderEnvironment dark() {
  RenderEnvironment e;
  e.sunIntensity = 0;
  e.skyColor = {};
  e.groundColor = {};
  e.shadowBiasMeters = 0;
  e.globalIlluminationIntensity = 1;
  e.globalIlluminationMode = GlobalIlluminationMode::VoxelOneBounce;
  e.bloomIntensity = 0;
  return e;
}
VoxelMaterialDefinition white() {
  VoxelMaterialDefinition m;
  m.baseColor = {1, 1, 1, 1};
  m.specular = 0;
  m.metallic = 0;
  m.roughness = 1;
  return m;
}
Float3 total(ShadingLobes l) {
  return {l.diffuse.x + l.specular.x, l.diffuse.y + l.specular.y,
          l.diffuse.z + l.specular.z};
}
CookedPolygonAsset quad(Float3 a, Float3 b, Float3 c, Float3 d,
                        VoxelMaterialDefinition m) {
  CookedPolygonAsset p;
  p.materials = {m};
  p.materialBindings.resize(1);
  p.vertices = {{a}, {b}, {c}, {d}};
  p.indices = {0, 1, 2, 0, 2, 3};
  p.submeshes = {{"face", 0, 6, 0}};
  p.bounds = {{-10, -10, -10}, {10, 10, 10}};
  return p;
}
void analytic_lights() {
  auto e = dark();
  SceneLight l;
  l.positionMeters = {0, 2, 0};
  l.intensity = 4;
  e.lights = {l};
  SceneLighting scene({}, {}, e, 1);
  near(total(scene.direct({}, {0, 1, 0}, {0, 1, 0}, white(), 2)).x,
       1 / 3.14159265F, 2e-5F, "point inverse square");
  SceneLighting scaled({}, {}, e, .1F);
  near(total(scaled.direct({}, {0, 1, 0}, {0, 1, 0}, white(), 2)).x,
       1 / 3.14159265F, 2e-5F, "metric light scale");
  e.lights[0].kind = SceneLightKind::Spot;
  e.lights[0].direction = {0, -1, 0};
  SceneLighting spot({}, {}, e, 1);
  near(total(spot.direct({}, {0, 1, 0}, {0, 1, 0}, white(), 1)).x,
       1 / 3.14159265F, 2e-5F, "spot axis");
  near(total(spot.direct({4, 0, 0}, {0, 1, 0}, {0, 1, 0}, white(), 1)).x, 0,
       1e-6F, "spot outside cone");
  e.lights[0].kind = SceneLightKind::Disc;
  e.lights[0].edgeU = {1, 0, 0};
  e.lights[0].edgeV = {0, 0, 1};
  e.lights[0].intensity = 1;
  e.localLightSamples = 64;
  SceneLighting disc({}, {}, e, 1);
  double mean = 0;
  for (unsigned k = 0; k < 3000; ++k)
    mean += total(disc.direct({}, {0, 1, 0}, {0, 1, 0}, white(), k)).x;
  near(float(mean / 3000), .2F, .002F, "disc analytic R2/(R2+h2)");
  e.localLightSamples = 1;
  SceneLighting single({}, {}, e, 1);
  e.localLightSamples = 2;
  SceneLighting doubleSamples({}, {}, e, 1);
  double sumOne = 0, sumTwo = 0, squareOne = 0, squareTwo = 0;
  for (unsigned k = 0; k < 6000; ++k) {
    const auto
        a = total(single.direct({}, {0, 1, 0}, {0, 1, 0}, white(), k)).x,
        b = total(doubleSamples.direct({}, {0, 1, 0}, {0, 1, 0}, white(), k)).x;
    sumOne += a;
    sumTwo += b;
    squareOne += a * a;
    squareTwo += b * b;
  }
  const double varOne = squareOne / 6000 - std::pow(sumOne / 6000, 2),
               varTwo = squareTwo / 6000 - std::pow(sumTwo / 6000, 2);
  near(float(varTwo / varOne), .5F, .06F,
       "doubling emitter samples halves variance");
  e.lights[0].kind = SceneLightKind::Quad;
  e.lights[0].intensity = 0;
  SceneLighting zero({}, {}, e, 1);
  near(total(zero.direct({}, {0, 1, 0}, {0, 1, 0}, white(), 3)).x, 0, 0,
       "zero light");
  // Invalid area and malformed codec are refused atomically.
  e.lights[0].edgeV = e.lights[0].edgeU;
  check(!e.validate(), "degenerate area validation");
}
void shadows_and_emission() {
  auto e = dark();
  SceneLight l;
  l.positionMeters = {0, 3, 0};
  e.lights = {l};
  auto blocker = quad({-2, 1, -2}, {2, 1, -2}, {2, 1, 2}, {-2, 1, 2}, white());
  PolygonRenderInstance p{1, &blocker};
  SceneLighting blocked({}, std::span(&p, 1), e, 1);
  near(total(blocked.direct({}, {0, 1, 0}, {0, 1, 0}, white(), 1)).x, 0, 0,
       "polygon local shadow");
  auto sunEnv = e;
  sunEnv.sunDirection = {0, 1, 0};
  sunEnv.shadowMode = ShadowMode::Hard;
  sunEnv.shadowStrength = 1;
  SceneLighting sunBlocked({}, std::span(&p, 1), sunEnv, 1);
  near(sunBlocked.sun_visibility({}, {0, 1, 0}, 1), 0, 0,
       "hybrid sun visibility");
  VoxelObject voxel;
  voxel.set_voxel({0, 1, 0}, 1);
  std::vector<VoxelMaterialDefinition> mats(2, white());
  VoxelReferenceInstance v{2, &voxel, {}, mats};
  e.lights[0].positionMeters = {.5F, 3, .5F};
  SceneLighting vb(std::span(&v, 1), {}, e, 1);
  near(total(vb.direct({.5F, 0, .5F}, {0, 1, 0}, {0, 1, 0}, white(), 1)).x, 0,
       0, "voxel local shadow");
  mats[1].shadingModel = MaterialShadingModel::Emissive;
  mats[1].baseColor = {0, 0, 0, 1};
  mats[1].emissive = {5, 0, 0};
  e.lights.clear();
  SceneLighting emitter(std::span(&v, 1), {}, e, 1);
  check(emitter.emitter_count() == 6, "six exposed emitting faces");
  Float3 red{};
  for (unsigned k = 0; k < 300; ++k) {
    const auto c =
        total(emitter.direct({.5F, 0, .5F}, {0, 1, 0}, {0, 1, 0}, white(), k));
    red.x += c.x;
    red.y += c.y;
    red.z += c.z;
  }
  check(red.x > 0 && red.y == 0 && red.z == 0,
        "emissive voxel lights neighbour");
  voxel.set_voxel({1, 1, 0}, 1);
  SceneLighting pair(std::span(&v, 1), {}, e, 1);
  check(pair.emitter_count() == 10, "internal emissive faces excluded");
}
void furnace() {
  auto m = white();
  m.baseColor = {.8F, .8F, .8F, 1};
  m.emissive = {1, 1, 1};
  std::vector<CookedPolygonAsset> faces;
  faces.push_back(quad({-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1}, m));
  faces.push_back(quad({-1, 1, -1}, {-1, 1, 1}, {1, 1, 1}, {1, 1, -1}, m));
  faces.push_back(quad({-1, -1, -1}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}, m));
  faces.push_back(quad({1, -1, -1}, {1, 1, -1}, {1, 1, 1}, {1, -1, 1}, m));
  faces.push_back(quad({-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}, {1, -1, -1}, m));
  faces.push_back(quad({-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}, m));
  std::vector<PolygonRenderInstance> instances;
  for (auto &face : faces)
    instances.push_back({std::uint64_t(instances.size() + 1), &face});
  auto e = dark();
  e.pathTraceBounces = 32;
  e.pathTraceSamples = 256;
  e.localLightSamples = 4;
  e.pathTraceRoulette = false;
  SceneLighting fixed({}, instances, e, 1);
  e.pathTraceRoulette = true;
  SceneLighting roulette({}, instances, e, 1);
  double a = 0, b = 0;
  for (unsigned i = 0; i < 80; ++i) {
    auto d = total(fixed.direct({}, {0, 1, 0}, {0, 1, 0}, m, i));
    a += d.x + fixed.indirect({}, {0, 1, 0}, {0, 1, 0}, m, i).x;
    b += d.x + roulette.indirect({}, {0, 1, 0}, {0, 1, 0}, m, i).x;
  }
  // rho * (1 + rho + ...), excludes the supplied primary surface's own
  // emission.
  near(float(a / 80), 4, .04F, "multi-bounce furnace energy");
  near(float(b / 80), float(a / 80), .04F, "roulette preserves mean");
  std::cout << "furnace fixed=" << a / 80 << " roulette=" << b / 80 << '\n';
}
void persistence_and_glow() {
  auto e = dark();
  SceneLight candle;
  candle.kind = SceneLightKind::Candle;
  candle.color = {1, .45F, .12F};
  candle.seed = 34;
  e.lights = {candle};
  e.pathTraceBounces = 7;
  e.lightingTimeSeconds = 1234.56789;
  const auto text = encode_scene_lighting(e);
  RenderEnvironment restored;
  check(decode_scene_lighting(text, restored), "lighting decode");
  check(encode_scene_lighting(restored) == text, "lighting exact round trip");
  check(!decode_scene_lighting(text + "junk", restored),
        "reject trailing data");
  check(encode_scene_lighting(restored) == text, "bad decode atomic");
  near(candle_light_multiplier(candle, 42), candle_light_multiplier(candle, 42),
       0, "deterministic candle");
  for (int k = 0; k < 1000; ++k) {
    float v = candle_light_multiplier(candle, k * .01);
    check(v >= .85F && v <= 1.15F, "bounded flicker");
  }
  PolygonRenderTarget target;
  target.resize(9, 9);
  target.enable_passes();
  target.clear({0, 0, 0, 1});
  const int mid = 40;
  target.hdrColor[mid] = {10, 5, 2, 1};
  target.passes.emission[mid] = {10, 5, 2};
  e.bloomThreshold = 1;
  e.bloomIntensity = .3F;
  e.bloomRadius = 2;
  check(apply_cpu_bloom(target, e), "bloom");
  check(target.hdrColor[39].x > 0, "glow reaches neighbour");
  for (std::size_t i = 0; i < target.hdrColor.size(); ++i)
    near(target.passes.light_sum(i).x, target.hdrColor[i].x, 1e-6F,
         "glow AOV reconstruction");
  auto before = target.hdrColor;
  e.bloomRadius = std::numeric_limits<float>::infinity();
  check(!apply_cpu_bloom(target, e), "invalid bloom refused");
  near(target.hdrColor[mid].x, before[mid].x, 0, "bloom failure atomic");
}
void indirect_lobe_split() {
  auto e = dark();
  e.pathTraceBounces = 1;
  e.pathTraceSamples = 256;
  e.skyColor = {1, 1, 1};
  e.groundColor = {1, 1, 1};
  auto m = white();
  m.baseColor = {.8F, .8F, .8F, 1};
  SceneLighting scene({}, {}, e, 1);
  auto diffuse = scene.indirect_lobes({}, {0, 1, 0}, {0, 1, 0}, m, 1);
  near(diffuse.diffuse.x, .8F, 1e-5F, "diffuse white furnace");
  near(diffuse.specular.x, 0, 0, "diffuse transport AOV");
  m.metallic = 1;
  m.specular = .5F;
  auto metal = scene.indirect_lobes({}, {0, 1, 0}, {0, 1, 0}, m, 1);
  near(metal.diffuse.x, 0, 0, "metal has no indirect diffuse");
  check(metal.specular.x > 0, "rough metallic indirect specular");
}

void components_and_scattering() {
  auto registry = ComponentTypeRegistry::make_default();
  check(registry.find(kSceneLightComponent) != nullptr,
        "light authoring schema");
  Component c;
  c.id = 1;
  c.type = kSceneLightComponent;
  c.properties = {{"kind", std::string("candle")},
                  {"intensity", 2.0},
                  {"position", Float3{1, 0, 0}}};
  check(registry.validate(c), "valid light component");
  RigidTransform frame;
  frame.position = {4, 5, 6};
  const auto l = scene_light_from_component(c, frame);
  check(l.has_value(), "component resolves");
  near(l->positionMeters.x, 5, 0, "light follows object frame");
  c.properties["intensity"] = -1.0;
  check(!registry.validate(c), "negative authored intensity refused");
  c.properties["intensity"] = 2.0;
  c.enabled = false;
  check(!scene_light_from_component(c, frame), "disabled light");
  auto e = dark();
  SceneLight point;
  point.positionMeters = {0, 0, 2};
  point.intensity = 4;
  e.lights = {point};
  SceneLighting scene({}, {}, e, 1);
  const auto scattered = scene.scattering({}, {0, 0, 1}, 0, 1);
  near(scattered.x, 1 / (4 * 3.14159265F), 1e-6F, "isotropic point scattering");
  near(scene.scattering({}, {0, 0, 1}, 0, 1, [](Float3, float) { return 0.F; })
           .x,
       0, 0, "medium shadows emitters");
  GaborVolumeAsset fog;
  GaborVolumePrimitive primitive;
  primitive.center = {0, 0, 1};
  primitive.scale = {.3F, .3F, .3F};
  primitive.opacity = 1;
  fog.primitives = {primitive};
  fog.recompute_bounds_and_hash();
  CpuFogVolume volume{&fog};
  CpuFogSettings settings;
  settings.scattering = true;
  settings.sceneLighting = &scene;
  settings.maximumSteps = 128;
  settings.stepMeters = .02F;
  const auto sample =
      trace_cpu_fog({}, {0, 0, 1}, 0, 3, 1, std::span(&volume, 1), e, settings);
  check(sample.radiance.x > 0, "scene emitters light fog");
}

void voxel_render_threads() {
  auto e = dark();
  e.pathTraceBounces = 2;
  e.pathTraceSamples = 2;
  SceneLight l;
  l.positionMeters = {.5F, .5F, 4};
  l.intensity = 20;
  e.lights = {l};
  VoxelObject object;
  object.set_voxel({0, 0, 0}, 1);
  std::vector<VoxelMaterialDefinition> materials(2, white());
  VoxelReferenceInstance instance{7, &object, {}, materials};
  PolygonCamera camera;
  camera.position = {.5F, .5F, 4};
  camera.target = {.5F, .5F, 0};
  camera.nearPlane = .01F;
  PolygonRenderTarget one, many;
  one.resize(12, 12);
  many.resize(12, 12);
  one.enable_passes();
  many.enable_passes();
  ReferenceVoxelRenderer renderer;
  renderer.metersPerVoxel = 1;
  renderer.threadCount = 1;
  renderer.render(std::span(&instance, 1), camera, e, one);
  renderer.threadCount = 3;
  renderer.render(std::span(&instance, 1), camera, e, many);
  bool lit = false;
  for (std::size_t i = 0; i < one.hdrColor.size(); ++i) {
    near(one.hdrColor[i].x, many.hdrColor[i].x, 0,
         "thread independent lighting");
    near(one.passes.light_sum(i).x, one.hdrColor[i].x, 1e-6F,
         "voxel light AOV sum");
    if (one.objectId[i] == 7)
      lit |= one.passes.directDiffuse[i].x > .1F;
  }
  check(lit, "local lights reach voxel render");
}

void integrated_render() {
  auto e = dark();
  e.pathTraceBounces = 2;
  e.pathTraceSamples = 4;
  SceneLight l;
  l.positionMeters = {0, 0, 3};
  l.intensity = 20;
  e.lights = {l};
  auto m = white();
  auto mesh = quad({-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}, m);
  for (auto &v : mesh.vertices)
    v.normal = {0, 0, 1};
  PolygonRenderInstance p{8, &mesh};
  PolygonCamera camera;
  camera.position = {0, 0, 4};
  camera.target = {};
  camera.nearPlane = .01F;
  PolygonRenderTarget target;
  target.resize(12, 12);
  target.enable_passes();
  render_hybrid_reference({}, std::span(&p, 1), camera, e, target, {}, 1, 1);
  bool lit = false;
  for (std::size_t i = 0; i < target.hdrColor.size(); ++i) {
    if (target.objectId[i] == 8) {
      lit |= target.passes.directDiffuse[i].x > .01F;
      near(target.passes.light_sum(i).x, target.hdrColor[i].x, 1e-5F,
           "integrated AOV sum");
    }
  }
  check(lit, "local lights reach polygon render");
}
} // namespace
int main() {
  try {
    analytic_lights();
    shadows_and_emission();
    furnace();
    persistence_and_glow();
    indirect_lobe_split();
    components_and_scattering();
    voxel_render_threads();
    integrated_render();
    std::cout << "scene lighting tests passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
