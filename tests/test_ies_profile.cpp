#include "dve/render/ies_profile.hpp"
#include "dve/render/scene_lighting.hpp"
#include "dve/render_environment.hpp"

#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

using namespace dve;
using namespace dve::render;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void near(float actual, float expected, float tolerance, const char* message) {
    if (std::abs(actual - expected) > tolerance) {
        std::cerr << actual << " != " << expected << '\n';
        throw std::runtime_error(message);
    }
}

const char* kBasicIes =
    "IESNA:LM-63-1995\n"
    "[TEST] synthetic fixture\n"
    "TILT=NONE\n"
    "1 1000 1.0 5 1 1 2 0 0 0\n"
    "1.0 1.0 0.0\n"
    "0 45 90 135 180\n"
    "0\n"
    "100 50 25 0 0\n";

void test_parser() {
    IesProfile profile;
    std::string error;
    require(parse_ies_profile(kBasicIes, profile, &error), error.c_str());
    near(profile.samples[0], 1.0F, 1e-6F, "peak must normalize to 1");
    near(profile.samples[45], 0.5F, 1e-6F, "45-degree sample wrong");
    near(profile.samples[90], 0.25F, 1e-6F, "90-degree sample wrong");
    near(profile.samples[135], 0.0F, 1e-6F, "135-degree sample wrong");
    near(profile.samples[180], 0.0F, 1e-6F, "180-degree sample wrong");
    // Sampling interpolates between the 1-degree grid points.
    near(profile.sample(0.0F), 1.0F, 1e-6F, "on-axis sample wrong");
    near(profile.sample(0.3926991F), 0.75F, 1e-5F, "22.5-degree interpolation wrong");
    near(profile.sample(1.5707963F), 0.25F, 1e-5F, "90-degree sample() wrong");
    require(profile.sample(-1.0F) == profile.sample(0.0F), "negative angle must clamp");
    require(profile.sample(4.0F) == profile.sample(3.14159265F), "angle > pi must clamp");

    // Candela multiplier and ballast factor scale every value, so the
    // normalized curve is unchanged.
    const std::string scaled =
        "IESNA:LM-63-1995\nTILT=NONE\n"
        "1 1000 2.0 5 1 1 2 0 0 0\n1.0 1.0 0.0\n0 45 90 135 180\n0\n100 50 25 0 0\n";
    IesProfile scaledProfile;
    require(parse_ies_profile(scaled, scaledProfile, &error), error.c_str());
    for (std::size_t i = 0; i < kIesProfileSampleCount; ++i)
        near(scaledProfile.samples[i], profile.samples[i], 1e-6F,
             "multiplier changed the normalized curve");

    // Two horizontal planes average azimuthally: (100 + 60) / 2 = 80 peak.
    const std::string twoPlanes =
        "IESNA:LM-63-1995\nTILT=NONE\n"
        "1 1000 1.0 3 2 1 2 0 0 0\n1.0 1.0 0.0\n0 90 180\n0 90\n"
        "100 40 0 60 20 0\n";
    IesProfile averaged;
    require(parse_ies_profile(twoPlanes, averaged, &error), error.c_str());
    near(averaged.samples[0], 1.0F, 1e-6F, "averaged peak wrong");
    near(averaged.samples[90], 0.375F, 1e-6F, "azimuthal average wrong"); // (40+20)/2 / 80

    // TILT=INCLUDE blocks are consumed, not applied.
    const std::string tilt =
        "IESNA:LM-63-1995\nTILT=INCLUDE\n1 2\n0 45\n1.0 1.0\n"
        "1 1000 1.0 5 1 1 2 0 0 0\n1.0 1.0 0.0\n0 45 90 135 180\n0\n100 50 25 0 0\n";
    IesProfile tilted;
    require(parse_ies_profile(tilt, tilted, &error), error.c_str());
    near(tilted.samples[45], 0.5F, 1e-6F, "TILT=INCLUDE file parsed wrong");

    // Malformed inputs fail cleanly and leave the output untouched.
    IesProfile sentinel;
    sentinel.samples.fill(7.0F);
    IesProfile out = sentinel;
    require(!parse_ies_profile("not an ies file", out, &error), "garbage parsed as IES");
    require(out.samples[0] == 7.0F, "failed parse mutated the output");
    require(!parse_ies_profile("TILT=NONE\n1 1000 1.0 5 1 1 2 0 0 0\n1 1 0\n0 45 90 135 180\n0\n100 50\n",
                                out, &error),
            "truncated candela parsed as IES");
    require(!parse_ies_profile("TILT=NONE\n1 1000 1.0 3 1 1 2 0 0 0\n1 1 0\n0 90 180\n0\n0 0 0\n",
                                out, &error),
            "all-zero candela parsed as IES");
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
float total(ShadingLobes lobes) { return lobes.diffuse.x + lobes.specular.x; }

void test_lighting_integration() {
    auto profile = std::make_shared<IesProfile>();
    require(parse_ies_profile(kBasicIes, *profile), "fixture must parse");
    auto e = dark();
    SceneLight light;
    light.positionMeters = {0, 2, 0};
    light.direction = {0, -1, 0};
    light.intensity = 4;
    e.lights = {light};
    SceneLighting plain({}, {}, e, 1);
    const float onAxis = total(plain.direct({}, {0, 1, 0}, {0, 1, 0}, white(), 1));
    near(onAxis, 1.0F / 3.14159265F, 2e-5F, "plain point light baseline wrong");

    e.lights[0].iesProfile = profile;
    SceneLighting shaped({}, {}, e, 1);
    // On axis the profile is 1: identical to the plain light.
    near(total(shaped.direct({}, {0, 1, 0}, {0, 1, 0}, white(), 1)), onAxis, 2e-5F,
         "on-axis IES light must match the plain light");
    // At 90 degrees off axis the profile is 0.25 of peak: a point level with
    // the light, facing it, gets exactly a quarter of the plain value.
    const float side = total(plain.direct({2, 2, 0}, {-1, 0, 0}, {-1, 0, 0}, white(), 1));
    near(total(shaped.direct({2, 2, 0}, {-1, 0, 0}, {-1, 0, 0}, white(), 1)), side * 0.25F,
         2e-5F, "90-degree IES shaping wrong");
    // Past 135 degrees the profile is zero: no light at all.
    near(total(shaped.direct({0, 4, 0}, {0, -1, 0}, {0, -1, 0}, white(), 1)), 0.0F, 1e-6F,
         "IES profile must cut off behind the fixture");

    // Validation rejects an all-zero profile attached by hand.
    auto bad = std::make_shared<IesProfile>();
    e.lights[0].iesProfile = bad;
    std::string error;
    require(!e.lights[0].validate(&error), "empty IES profile must fail validation");
}

void test_persistence() {
    auto profile = std::make_shared<IesProfile>();
    require(parse_ies_profile(kBasicIes, *profile), "fixture must parse");
    auto e = dark();
    SceneLight light;
    light.kind = SceneLightKind::Spot;
    light.iesProfile = profile;
    e.lights = {light, SceneLight{}};
    const std::string encoded = encode_scene_lighting(e);
    require(encoded.starts_with("DVE_LIGHTING 2"), "profiled lights must encode as version 2");
    RenderEnvironment decoded;
    std::string error;
    require(decode_scene_lighting(encoded, decoded, &error), error.c_str());
    require(decoded.lights.size() == 2U, "round trip lost a light");
    require(decoded.lights[0].iesProfile != nullptr, "round trip lost the profile");
    require(decoded.lights[1].iesProfile == nullptr, "unprofiled light gained a profile");
    for (std::size_t i = 0; i < kIesProfileSampleCount; ++i)
        require(decoded.lights[0].iesProfile->samples[i] == profile->samples[i],
                "round trip changed a profile sample");
    // Without profiles the encoding stays version 1, byte-stable for old files.
    e.lights[0].iesProfile.reset();
    require(encode_scene_lighting(e).starts_with("DVE_LIGHTING 1"),
            "profile-free lights must keep the version 1 layout");
}
} // namespace

int main() {
    try {
        test_parser();
        test_lighting_integration();
        test_persistence();
        std::cout << "ies profile tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
