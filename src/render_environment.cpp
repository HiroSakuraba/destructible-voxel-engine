#include "dve/render_environment.hpp"

#include "dve/component.hpp"

#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace dve {
namespace {
bool finite3(Float3 v) noexcept { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
}

bool SceneLight::validate(std::string *error) const noexcept {
  const auto fail = [&](const char *m) {
    if (error)
      *error = m;
    return false;
  };
  if (static_cast<unsigned>(kind) > 4)
    return fail("invalid scene light kind");
  if (!finite3(positionMeters) || !finite3(direction) || !finite3(edgeU) ||
      !finite3(edgeV) || !finite3(color))
    return fail("light vectors must be finite");
  if (color.x < 0 || color.y < 0 || color.z < 0 || !std::isfinite(intensity) ||
      intensity < 0)
    return fail("light radiance must be nonnegative and finite");
  const double d = double(direction.x) * direction.x +
                   double(direction.y) * direction.y +
                   double(direction.z) * direction.z;
  if (!(d > 1e-12 && d < 1e24))
    return fail("invalid light direction");
  const double x = double(edgeU.y) * edgeV.z - double(edgeU.z) * edgeV.y;
  const double y = double(edgeU.z) * edgeV.x - double(edgeU.x) * edgeV.z;
  const double z = double(edgeU.x) * edgeV.y - double(edgeU.y) * edgeV.x;
  if ((kind == SceneLightKind::Quad || kind == SceneLightKind::Disc) &&
      !(x * x + y * y + z * z > 1e-16 && x * x + y * y + z * z < 1e24))
    return fail("area light edges must span a finite area");
  if (!std::isfinite(innerConeRadians) || !std::isfinite(outerConeRadians) ||
      innerConeRadians < 0 || innerConeRadians > outerConeRadians ||
      outerConeRadians >= 1.5707963F)
    return fail("invalid light cone");
  if (!std::isfinite(flickerAmount) || flickerAmount < 0 || flickerAmount > 1 ||
      !std::isfinite(flickerFrequencyHz) || flickerFrequencyHz < 0 ||
      flickerFrequencyHz > 1000)
    return fail("invalid candle flicker");
  if (iesProfile) {
    for (const float s : iesProfile->samples)
      if (!std::isfinite(s) || s < 0)
        return fail("IES profile samples must be finite and nonnegative");
    if (iesProfile->empty())
      return fail("IES profile has no positive samples");
  }
  return true;
}

bool RenderEnvironment::validate(std::string* error) const noexcept {
    auto fail = [&](const char* message) { if (error) *error = message; return false; };
    if (lights.size() > kMaximumSceneLights || localLightSamples < 1 ||
        localLightSamples > 64 || pathTraceBounces > 64 ||
        pathTraceSamples < 1 || pathTraceSamples > 256 ||
        !std::isfinite(lightingTimeSeconds))
      return fail("invalid lighting budget or time");
    for (const auto &light : lights)
      if (!light.validate(error))
        return false;
    switch (globalIlluminationMode) {
        case GlobalIlluminationMode::Off:
        case GlobalIlluminationMode::AmbientHemisphere:
        case GlobalIlluminationMode::VoxelOneBounce:
        case GlobalIlluminationMode::RadianceCascades: break;
        default: return fail("globalIlluminationMode is invalid");
    }
    switch (shadowMode) {
        case ShadowMode::Off:
        case ShadowMode::Hard:
        case ShadowMode::Soft:
        case ShadowMode::Contact:
        case ShadowMode::Hybrid: break;
        default: return fail("shadowMode is invalid");
    }
    if (!finite3(sunDirection)) return fail("sunDirection must be finite");
    if (sunDirection.x == 0.0F && sunDirection.y == 0.0F && sunDirection.z == 0.0F)
        return fail("sunDirection must not be the zero vector");
    if (!std::isfinite(sunIntensity) || sunIntensity < 0.0F) return fail("sunIntensity must be finite and non-negative");
    if (!finite3(sunColor) || sunColor.x < 0.0F || sunColor.y < 0.0F || sunColor.z < 0.0F)
        return fail("sunColor must be finite and non-negative");
    if (!std::isfinite(subsurfaceMaxDistanceMeters) || subsurfaceMaxDistanceMeters < 0.0F)
        return fail("subsurfaceMaxDistanceMeters must be finite and non-negative");
    if (!finite3(skyColor) || skyColor.x < 0.0F || skyColor.y < 0.0F || skyColor.z < 0.0F)
        return fail("skyColor must be finite and non-negative");
    if (!std::isfinite(exposure) || exposure <= 0.0F) return fail("exposure must be finite and positive");
    if (!finite3(groundColor) || groundColor.x < 0.0F || groundColor.y < 0.0F || groundColor.z < 0.0F)
        return fail("groundColor must be finite and non-negative");
    if (!finite3(globalTint) || globalTint.x < 0.0F || globalTint.y < 0.0F || globalTint.z < 0.0F)
        return fail("globalTint must be finite and non-negative");
    if (!std::isfinite(bloomThreshold) || bloomThreshold < 0.0F) return fail("bloomThreshold must be finite and non-negative");
    if (!std::isfinite(bloomIntensity) || bloomIntensity < 0.0F) return fail("bloomIntensity must be finite and non-negative");
    if (!std::isfinite(bloomRadius) || bloomRadius < 0.0F) return fail("bloomRadius must be finite and non-negative");
    if (!std::isfinite(globalIlluminationIntensity) || globalIlluminationIntensity < 0.0F || globalIlluminationIntensity > 4.0F)
        return fail("globalIlluminationIntensity must be finite and in [0,4]");
    if (!std::isfinite(globalIlluminationMaxDistanceMeters) || globalIlluminationMaxDistanceMeters <= 0.0F)
        return fail("globalIlluminationMaxDistanceMeters must be finite and positive");
    if (globalIlluminationSamples == 0U || globalIlluminationSamples > 16U)
        return fail("globalIlluminationSamples must be in [1,16]");
    if (ambientOcclusionSamples > kMaximumAmbientOcclusionSamples)
        return fail("ambientOcclusionSamples must be in [0,64]");
    if (!std::isfinite(ambientOcclusionMaxDistanceMeters) || ambientOcclusionMaxDistanceMeters < 0.0F)
        return fail("ambientOcclusionMaxDistanceMeters must be finite and non-negative");
    if (!std::isfinite(fogDensityPerMeter) || fogDensityPerMeter < 0.0F || fogDensityPerMeter > 10.0F)
        return fail("fogDensityPerMeter must be finite and in [0,10]");
    if (!std::isfinite(fogStartMeters) || fogStartMeters < 0.0F) return fail("fogStartMeters must be finite and non-negative");
    if (skyMode != SkyMode::Colors && skyMode != SkyMode::Physical) return fail("skyMode must be Colors or Physical");
    const auto in = [](float v, float lo, float hi) { return v >= lo && v <= hi; };   // false for NaN
    if (!in(atmosphereRayleighScale, 0.0F, 100.0F) || !in(atmosphereMieScale, 0.0F, 100.0F) || !in(atmosphereOzoneScale, 0.0F, 100.0F))
        return fail("atmosphere density scales must be in [0,100]");
    if (!in(atmosphereMieAnisotropy, -0.99F, 0.99F)) return fail("atmosphereMieAnisotropy must be in [-0.99,0.99]");
    if (!in(atmosphereGroundAlbedo, 0.0F, 1.0F)) return fail("atmosphereGroundAlbedo must be in [0,1]");
    if (!in(observerAltitudeMeters, 0.0F, 90000.0F)) return fail("observerAltitudeMeters must be in [0,90000]");
    if (!in(aerialPerspectiveScale, 0.0F, 1.0e6F)) return fail("aerialPerspectiveScale must be in [0,1e6]");
    if (!in(sunDisc, 0.0F, 100.0F)) return fail("sunDisc must be in [0,100]");
    if (!std::isfinite(fogColor.x) || !std::isfinite(fogColor.y) || !std::isfinite(fogColor.z) || fogColor.x < 0.0F ||
        fogColor.y < 0.0F || fogColor.z < 0.0F)
        return fail("fogColor must be finite and non-negative");
    if (!std::isfinite(fogHeightFalloffPerMeter) || fogHeightFalloffPerMeter < 0.0F || fogHeightFalloffPerMeter > 10.0F)
        return fail("fogHeightFalloffPerMeter must be finite and in [0,10]");
    if (!std::isfinite(fogBaseHeightMeters)) return fail("fogBaseHeightMeters must be finite");
    if (!std::isfinite(shadowStrength) || shadowStrength < 0.0F || shadowStrength > 1.0F)
        return fail("shadowStrength must be finite and in [0,1]");
    if (!std::isfinite(shadowSoftnessRadians) || shadowSoftnessRadians < 0.0F || shadowSoftnessRadians > 0.25F)
        return fail("shadowSoftnessRadians must be finite and in [0,0.25]");
    if (shadowSamples == 0U || shadowSamples > 16U) return fail("shadowSamples must be in [1,16]");
    if (!std::isfinite(shadowMaxDistanceMeters) || shadowMaxDistanceMeters <= 0.0F)
        return fail("shadowMaxDistanceMeters must be finite and positive");
    if (!std::isfinite(contactShadowDistanceMeters) || contactShadowDistanceMeters <= 0.0F)
        return fail("contactShadowDistanceMeters must be finite and positive");
    if (!std::isfinite(shadowBiasMeters) || shadowBiasMeters < 0.0F || shadowBiasMeters > 1.0F)
        return fail("shadowBiasMeters must be finite and in [0,1]");
    return true;
}

std::optional<SceneLight>
scene_light_from_component(const Component &c, const RigidTransform &frame,
                           std::string *error) {
  if (c.type != kSceneLightComponent || !c.enabled)
    return std::nullopt;
  SceneLight light;
  bool valid = true;
  const auto property = [&]<class T>(const char *name, T &out) {
    auto it = c.properties.find(name);
    if (it == c.properties.end())
      return;
    if (const auto *value = std::get_if<T>(&it->second))
      out = *value;
    else
      valid = false;
  };
  std::string kind = "point";
  property("kind", kind);
  if (kind == "point")
    light.kind = SceneLightKind::Point;
  else if (kind == "spot")
    light.kind = SceneLightKind::Spot;
  else if (kind == "quad")
    light.kind = SceneLightKind::Quad;
  else if (kind == "disc")
    light.kind = SceneLightKind::Disc;
  else if (kind == "candle")
    light.kind = SceneLightKind::Candle;
  else
    valid = false;
  property("position", light.positionMeters);
  property("direction", light.direction);
  property("edge_u", light.edgeU);
  property("edge_v", light.edgeV);
  property("color", light.color);
  property("two_sided", light.twoSided);
  const auto scalar = [&](const char *key, float &out) {
    double value = out;
    property(key, value);
    out = static_cast<float>(value);
  };
  scalar("intensity", light.intensity);
  scalar("inner_cone", light.innerConeRadians);
  scalar("outer_cone", light.outerConeRadians);
  scalar("flicker_amount", light.flickerAmount);
  scalar("flicker_frequency", light.flickerFrequencyHz);
  std::int64_t seed = light.seed;
  property("seed", seed);
  if (seed < 0 || seed > 0xffffffffLL)
    valid = false;
  else
    light.seed = static_cast<std::uint32_t>(seed);
  light.positionMeters = transform_point(frame, light.positionMeters);
  light.direction = transform_vector(frame, light.direction);
  light.edgeU = transform_vector(frame, light.edgeU);
  light.edgeV = transform_vector(frame, light.edgeV);
  if (!valid) {
    if (error)
      *error = "invalid light component property type or kind";
    return std::nullopt;
  }
  if (!light.validate(error))
    return std::nullopt;
  return light;
}

std::string encode_scene_lighting(const RenderEnvironment &e) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(std::numeric_limits<double>::max_digits10);
  // Version 2 adds an optional IES profile per light; files without any
  // profile keep the version 1 layout byte-for-byte.
  bool anyIes = false;
  for (const auto &l : e.lights) anyIes = anyIes || static_cast<bool>(l.iesProfile);
  out << "DVE_LIGHTING " << (anyIes ? 2 : 1) << ' ' << e.localLightSamples << ' ' << e.pathTraceBounces
      << ' ' << e.pathTraceSamples << ' ' << e.pathTraceRoulette << ' '
      << e.lightingTimeSeconds << ' ' << e.lights.size() << '\n';
  const auto v = [&](Float3 a) {
    out << a.x << ' ' << a.y << ' ' << a.z << ' ';
  };
  for (const auto &l : e.lights) {
    out << static_cast<unsigned>(l.kind) << ' ';
    v(l.positionMeters);
    v(l.direction);
    v(l.edgeU);
    v(l.edgeV);
    v(l.color);
    out << l.intensity << ' ' << l.innerConeRadians << ' ' << l.outerConeRadians
        << ' ' << l.twoSided << ' ' << l.flickerAmount << ' '
        << l.flickerFrequencyHz << ' ' << l.seed;
    if (anyIes) {
      out << ' ' << (l.iesProfile ? 1 : 0);
      if (l.iesProfile)
        for (const float s : l.iesProfile->samples) out << ' ' << s;
    }
    out << '\n';
  }
  return out.str();
}
bool decode_scene_lighting(const std::string &text, RenderEnvironment &e,
                           std::string *error) {
  const auto fail = [&]() {
    if (error)
      *error = "invalid DVE lighting extension";
    return false;
  };
  if (text.size() > 2 * 1024 * 1024)
    return fail();
  std::istringstream in(text);
  in.imbue(std::locale::classic());
  std::string magic;
  unsigned version, roulette, count;
  auto next = e;
  if (!(in >> magic >> version) || magic != "DVE_LIGHTING" || (version != 1 && version != 2) ||
      !(in >> next.localLightSamples >> next.pathTraceBounces >>
        next.pathTraceSamples >> roulette >> next.lightingTimeSeconds >>
        count) ||
      roulette > 1 || count > 4096)
    return fail();
  next.pathTraceRoulette = roulette != 0;
  next.lights.clear();
  next.lights.resize(count);
  const auto v = [&](Float3 &a) { return bool(in >> a.x >> a.y >> a.z); };
  for (auto &l : next.lights) {
    unsigned kind, two;
    if (!(in >> kind) || kind > 4 || !v(l.positionMeters) || !v(l.direction) ||
        !v(l.edgeU) || !v(l.edgeV) || !v(l.color) ||
        !(in >> l.intensity >> l.innerConeRadians >> l.outerConeRadians >>
          two >> l.flickerAmount >> l.flickerFrequencyHz >> l.seed) ||
        two > 1)
      return fail();
    l.kind = static_cast<SceneLightKind>(kind);
    l.twoSided = two != 0;
    if (version == 2) {
      unsigned hasIes = 0;
      if (!(in >> hasIes) || hasIes > 1)
        return fail();
      if (hasIes) {
        auto profile = std::make_shared<render::IesProfile>();
        for (float &s : profile->samples)
          if (!(in >> s))
            return fail();
        l.iesProfile = std::move(profile);
      }
    }
  }
  in >> std::ws;
  if (!in.eof() || !next.validate(error))
    return fail();
  e = std::move(next);
  return true;
}
} // namespace dve
