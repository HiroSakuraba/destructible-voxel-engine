// The physical atmosphere (render/atmosphere.hpp):
//   - transmittance: the vertical column matches its closed form; the table matches direct
//     integration everywhere;
//   - single scattering in a thin atmosphere matches its closed form;
//   - the sky-view table matches the direct march; the irradiance table matches integrating the
//     sky over a hemisphere;
//   - Rayleigh colours: a blue zenith, an orange sunset sun, no sun below the horizon but a
//     twilight sky;
//   - Colors mode returns exactly the old sky/ground blend and sun.
#include "dve/render/atmosphere.hpp"
#include "dve/render/polygon_renderer.hpp"
#include "dve/render/voxel_reference_renderer.hpp"
#include "dve/transform.hpp"

#include <algorithm>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <array>
#include <string>
#include <vector>

namespace {
using namespace dve;
using namespace dve::render;

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::printf("FAIL: %s\n", message.c_str()); ++g_failures; }
}
constexpr double kPi = 3.14159265358979323846;

double rel(Float3 a, Float3 b) {
    const double d = std::abs(a.x - b.x) + std::abs(a.y - b.y) + std::abs(a.z - b.z);
    return d / (1e-30 + std::abs(b.x) + std::abs(b.y) + std::abs(b.z));
}
Float3 sun_at(float elevationDegrees, float azimuthDegrees = 30.0F) {
    const float e = elevationDegrees * static_cast<float>(kPi / 180.0), a = azimuthDegrees * static_cast<float>(kPi / 180.0);
    return {std::cos(e) * std::cos(a), std::sin(e), std::cos(e) * std::sin(a)};
}

void test_transmittance(const AtmosphereTables& tables) {
    const AtmosphereParameters& p = tables.parameters();
    const double top = p.topRadiusMeters - p.bottomRadiusMeters;
    const double depthR[3] = {p.rayleighScattering.x * p.rayleighScaleHeightMeters * (1 - std::exp(-top / p.rayleighScaleHeightMeters)),
                              p.rayleighScattering.y * p.rayleighScaleHeightMeters * (1 - std::exp(-top / p.rayleighScaleHeightMeters)),
                              p.rayleighScattering.z * p.rayleighScaleHeightMeters * (1 - std::exp(-top / p.rayleighScaleHeightMeters))};
    const double depthM = (p.mieScattering + p.mieAbsorption) * p.mieScaleHeightMeters * (1 - std::exp(-top / p.mieScaleHeightMeters));
    const double ozone[3] = {p.ozoneAbsorption.x * p.ozoneHalfWidthMeters, p.ozoneAbsorption.y * p.ozoneHalfWidthMeters,
                             p.ozoneAbsorption.z * p.ozoneHalfWidthMeters};
    const Float3 exact{static_cast<float>(std::exp(-(depthR[0] + depthM + ozone[0]))), static_cast<float>(std::exp(-(depthR[1] + depthM + ozone[1]))),
                       static_cast<float>(std::exp(-(depthR[2] + depthM + ozone[2])))};
    const float r0 = p.bottomRadiusMeters;
    const Float3 integrated = integrate_transmittance(p, r0, 1.0F, static_cast<float>(top), 20000U);
    const Float3 table = tables.transmittance_to_top(r0, 1.0F);
    std::printf("vertical transmittance: exact (%.5f %.5f %.5f), integrated (%.5f %.5f %.5f), table (%.5f %.5f %.5f)\n", exact.x,
                exact.y, exact.z, integrated.x, integrated.y, integrated.z, table.x, table.y, table.z);
    check(rel(integrated, exact) < 1e-4, "the vertical column integrates to its closed form");
    check(rel(table, exact) < 2e-3, "the table holds the vertical column");
    double worst = 0.0;
    for (float h : {0.0F, 300.0F, 2000.0F, 9000.0F, 30000.0F, 80000.0F})
        for (float mu : {1.0F, 0.6F, 0.25F, 0.08F, 0.02F, -0.01F}) {
            const float r = r0 + h;
            if (distance_to_ground(p, r, mu) >= 0.0F) continue;
            const Float3 direct = integrate_transmittance(p, r, mu, distance_to_top(p, r, mu), 20000U);
            worst = std::max(worst, rel(tables.transmittance_to_top(r, mu), direct));
        }
    std::printf("transmittance table vs direct integration: worst %.2e\n", worst);
    check(worst < 5e-3, "the transmittance table matches direct integration");
}

void test_thin_single_scattering() {
    // Rayleigh only, a thousandth of Earth's: transmittance ~ 1, so the zenith radiance with the
    // sun overhead is beta H p(1) (1 - e^{-top/H}).
    AtmosphereParameters p;
    p.rayleighScattering = multiply(p.rayleighScattering, 1.0e-3F);
    p.mieScattering = p.mieAbsorption = 0.0F;
    p.ozoneAbsorption = {};
    p.groundAlbedo = 0.0F;
    const double top = p.topRadiusMeters - p.bottomRadiusMeters, H = p.rayleighScaleHeightMeters;
    const double k = H * (1.0 - std::exp(-top / H)) * 3.0 / (16.0 * kPi) * 2.0;
    const Float3 exact{static_cast<float>(p.rayleighScattering.x * k), static_cast<float>(p.rayleighScattering.y * k),
                       static_cast<float>(p.rayleighScattering.z * k)};
    const Float3 L = integrate_sky_radiance(p, nullptr, p.bottomRadiusMeters + 1.0F, {0, 1, 0}, {0, 1, 0}, 4096U);
    std::printf("thin Rayleigh zenith radiance: %.4e (exact %.4e), blue/red %.2f (lambda^-4: %.2f)\n", L.z, exact.z, L.z / L.x,
                p.rayleighScattering.z / p.rayleighScattering.x);
    check(rel(L, exact) < 3e-3, "single scattering in a thin atmosphere matches the closed form");
}

void test_tables(const RenderEnvironment& environment) {
    const auto model = atmosphere_model(environment);
    const AtmosphereParameters& p = model->parameters();
    const float r = model->observer_radius();
    const Float3 sun = normalize(environment.sunDirection);
    double worstSky = 0.0, worstHorizon = 0.0;
    for (int a = 0; a < 12; ++a)
        for (int e = -8; e <= 16; ++e) {
            const float elevation = static_cast<float>(e) * 5.5F * static_cast<float>(kPi / 180.0) + 0.013F;
            const float azimuth = static_cast<float>(a) * 0.53F;
            const Float3 d{std::cos(elevation) * std::cos(azimuth), std::sin(elevation), std::cos(elevation) * std::sin(azimuth)};
            const Float3 table = model->sky_radiance(d);
            const Float3 direct = integrate_sky_radiance(p, &model->tables(), r, d, sun, 1024U);
            const double error = rel(table, direct);
            (std::abs(elevation) < 0.1F ? worstHorizon : worstSky) = std::max(std::abs(elevation) < 0.1F ? worstHorizon : worstSky, error);
        }
    std::printf("sky-view table vs direct march: worst %.3f away from the horizon, %.3f within 6 degrees of it\n", worstSky, worstHorizon);
    check(worstSky < 0.03, "the sky-view table matches the direct march");
    check(worstHorizon < 0.08, "the sky-view table stays close at the horizon");

    // Irradiance table vs integrating the table's sky over the hemisphere (fine quadrature).
    double worstIrradiance = 0.0;
    for (const Float3 n : {Float3{0, 1, 0}, Float3{1, 0, 0}, Float3{-0.6F, 0.3F, 0.74F}, Float3{0.2F, -0.9F, 0.3F}}) {
        const Float3 nn = normalize(n);
        const Float3 helper = std::abs(nn.y) < 0.9F ? Float3{0, 1, 0} : Float3{1, 0, 0};
        const Float3 t = normalize(subtract(helper, multiply(nn, dot(helper, nn))));
        const Float3 b{nn.y * t.z - nn.z * t.y, nn.z * t.x - nn.x * t.z, nn.x * t.y - nn.y * t.x};
        Float3 sum{};
        const int A = 96, B = 192;
        for (int i = 0; i < A; ++i)
            for (int j = 0; j < B; ++j) {
                const float u1 = (i + 0.5F) / A, u2 = (j + 0.5F) / B, rr = std::sqrt(u1), ph = 2.0F * static_cast<float>(kPi) * u2;
                const Float3 d = add(add(multiply(t, rr * std::cos(ph)), multiply(b, rr * std::sin(ph))), multiply(nn, std::sqrt(1.0F - u1)));
                sum = add(sum, model->sky_radiance(d));
            }
        const Float3 exact = multiply(sum, static_cast<float>(kPi) / (A * B));
        const Float3 table = model->sky_irradiance(nn);
        std::printf("  normal (%.2f %.2f %.2f): table (%.4f %.4f %.4f) integral (%.4f %.4f %.4f)\n", nn.x, nn.y, nn.z, table.x, table.y,
                    table.z, exact.x, exact.y, exact.z);
        worstIrradiance = std::max(worstIrradiance, rel(table, exact));
    }
    std::printf("sky irradiance table vs hemisphere integral: worst %.4f\n", worstIrradiance);
    check(worstIrradiance < 0.01, "the irradiance table matches the hemisphere integral");
}

// Brute force for the multiple-scattering approximation: path tracing in the spherical shell,
// one wavelength at a time (delta tracking against a majorant, next-event estimation of the sun
// through the transmittance table, Lambertian ground), all orders of scattering.
struct Rng {
    std::uint64_t s;
    double next() {
        s ^= s << 13U; s ^= s >> 7U; s ^= s << 17U;
        return ((s >> 11U) + 0.5) * (1.0 / 9007199254740992.0);
    }
};
double path_traced_radiance(const AtmosphereTables& tables, double r0, Float3 viewF, Float3 sunF, int channel, int paths,
                            std::uint64_t seed) {
    const AtmosphereParameters& p = tables.parameters();
    const double R = p.bottomRadiusMeters, T = p.topRadiusMeters;
    const auto pick = [channel](Float3 v) { return channel == 0 ? v.x : channel == 1 ? v.y : v.z; };
    const double rayleigh0 = pick(p.rayleighScattering), ozone0 = pick(p.ozoneAbsorption);
    const double majorant = rayleigh0 + p.mieScattering + p.mieAbsorption + ozone0;
    struct V { double x, y, z; };
    const auto dotv = [](V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
    const auto lenv = [&](V a) { return std::sqrt(dotv(a, a)); };
    const V sun{sunF.x, sunF.y, sunF.z};
    const auto sunT = [&](V x) {
        const double rr = lenv(x);
        return static_cast<double>(pick(tables.sun_transmittance(static_cast<float>(rr), static_cast<float>(dotv(x, sun) / rr))));
    };
    const auto phaseR = [](double c) { return 3.0 / (16.0 * kPi) * (1.0 + c * c); };
    const auto phaseM = [&](double c) {
        const double g = p.mieAnisotropy;
        return 3.0 / (8.0 * kPi) * (1.0 - g * g) / (2.0 + g * g) * (1.0 + c * c) / std::pow(1.0 + g * g - 2.0 * g * c, 1.5);
    };
    Rng rng{seed * 0x9E3779B97F4A7C15ULL + 1U};
    const auto uniform_dir = [&]() {
        const double z = 1.0 - 2.0 * rng.next(), ph = 2.0 * kPi * rng.next(), sr = std::sqrt(std::max(0.0, 1.0 - z * z));
        return V{sr * std::cos(ph), z, sr * std::sin(ph)};
    };
    double total = 0.0;
    for (int n = 0; n < paths; ++n) {
        V x{0.0, r0, 0.0}, w{viewF.x, viewF.y, viewF.z};
        double throughput = 1.0;
        for (int bounce = 0; bounce < 64 && throughput > 1e-6; ++bounce) {
            const double rr = lenv(x), mu = dotv(x, w) / rr;
            const double disc = rr * rr * (mu * mu - 1.0) + R * R;
            const double ground = (mu < 0.0 && disc >= 0.0) ? -rr * mu - std::sqrt(disc) : -1.0;
            const double tmax = ground >= 0.0 ? ground : -rr * mu + std::sqrt(std::max(0.0, rr * rr * (mu * mu - 1.0) + T * T));
            double t = 0.0;
            bool collided = false;
            for (;;) {
                t -= std::log(rng.next()) / majorant;
                if (t >= tmax) break;
                const V q{x.x + w.x * t, x.y + w.y * t, x.z + w.z * t};
                const double h = lenv(q) - R;
                const double rd = std::exp(-h / p.rayleighScaleHeightMeters), md = std::exp(-h / p.mieScaleHeightMeters);
                const double od = std::max(0.0, 1.0 - std::abs(h - p.ozoneCenterAltitudeMeters) / p.ozoneHalfWidthMeters);
                const double sR = rayleigh0 * rd, sM = p.mieScattering * md;
                const double ext = sR + sM + p.mieAbsorption * md + ozone0 * od;
                if (rng.next() * majorant >= ext) continue;   // null collision
                // Real collision: continue as scattering with weight albedo.
                throughput *= (sR + sM) / ext;
                const double nu = dotv(w, sun);
                total += throughput * (sR * phaseR(nu) + sM * phaseM(nu)) / (sR + sM) * sunT(q);
                const V nw = uniform_dir();
                const double c = dotv(w, nw);
                throughput *= 4.0 * kPi * (sR * phaseR(c) + sM * phaseM(c)) / (sR + sM);
                x = q;
                w = nw;
                collided = true;
                break;
            }
            if (collided) continue;
            if (ground < 0.0) break;   // escaped
            const V g{x.x + w.x * ground, x.y + w.y * ground, x.z + w.z * ground};
            const double gl = lenv(g);
            const V up{g.x / gl, g.y / gl, g.z / gl};
            total += throughput * p.groundAlbedo / kPi * std::max(0.0, dotv(up, sun)) * sunT(g);
            // Cosine-weighted bounce.
            const double u1 = rng.next(), u2 = rng.next(), rc = std::sqrt(u1), ph = 2.0 * kPi * u2;
            const V tA = std::abs(up.y) < 0.9 ? V{-up.z, 0.0, up.x} : V{1.0, 0.0, 0.0};
            const double tl = lenv(tA);
            V t1{tA.x / tl, tA.y / tl, tA.z / tl};
            const double d1 = dotv(t1, up);
            t1 = {t1.x - up.x * d1, t1.y - up.y * d1, t1.z - up.z * d1};
            const double t1l = lenv(t1);
            t1 = {t1.x / t1l, t1.y / t1l, t1.z / t1l};
            const V t2{up.y * t1.z - up.z * t1.y, up.z * t1.x - up.x * t1.z, up.x * t1.y - up.y * t1.x};
            const double cz = std::sqrt(1.0 - u1);
            w = {t1.x * rc * std::cos(ph) + t2.x * rc * std::sin(ph) + up.x * cz, t1.y * rc * std::cos(ph) + t2.y * rc * std::sin(ph) + up.y * cz,
                 t1.z * rc * std::cos(ph) + t2.z * rc * std::sin(ph) + up.z * cz};
            x = {g.x + up.x * 0.01, g.y + up.y * 0.01, g.z + up.z * 0.01};
            throughput *= p.groundAlbedo;
        }
    }
    return total / paths;
}

void test_multiple_scattering_vs_path_tracing() {
    RenderEnvironment e;
    e.skyMode = SkyMode::Physical;
    e.sunDirection = sun_at(20.0F, 0.0F);
    const auto model = atmosphere_model(e);
    const float r = model->observer_radius();
    const Float3 sun = normalize(e.sunDirection);
    double worst = 0.0, worstSingleOnly = 0.0;
    for (const Float3 view : {Float3{0, 1, 0}, normalize(Float3{-0.8F, 0.35F, 0.2F}), normalize(Float3{0.5F, 0.15F, 0.8F})}) {
        const Float3 table = model->sky_radiance(view);
        const Float3 single = integrate_sky_radiance(model->parameters(), nullptr, r, view, sun, 512U);
        double pt[3];
        for (int c = 0; c < 3; ++c) pt[c] = path_traced_radiance(model->tables(), r, view, sun, c, 400000, 17U + c);
        const Float3 traced{static_cast<float>(pt[0]), static_cast<float>(pt[1]), static_cast<float>(pt[2])};
        std::printf("  view (%.2f %.2f %.2f): model (%.5f %.5f %.5f), path traced (%.5f %.5f %.5f), single only (%.5f %.5f %.5f)\n", view.x,
                    view.y, view.z, table.x, table.y, table.z, traced.x, traced.y, traced.z, single.x, single.y, single.z);
        worst = std::max(worst, rel(table, traced));
        worstSingleOnly = std::max(worstSingleOnly, rel(single, traced));
    }
    std::printf("sky radiance vs path-traced multiple scattering: worst %.3f (single scattering alone: %.3f)\n", worst, worstSingleOnly);
    // Hillaire's approximation treats the light at each point as isotropic and laterally uniform; it
    // comes out a few percent dark in the blue at low elevations.
    check(worst < 0.08, "the multiple-scattering approximation is within 8% of path tracing");
    check(worst < 0.25 * worstSingleOnly, "and far closer to it than single scattering alone");
}

void test_colours() {
    RenderEnvironment e;
    e.skyMode = SkyMode::Physical;
    e.sunColor = {1, 1, 1};
    e.sunIntensity = 1.0F;
    e.sunDirection = sun_at(60.0F);
    const Float3 zenith = sky_radiance(e, {0, 1, 0});
    const Float3 noonSun = sun_irradiance(e);
    const Float3 up = sky_irradiance(e, {0, 1, 0});
    const float diffuseShare = up.y / (up.y + noonSun.y * std::sin(60.0F * static_cast<float>(kPi / 180.0)));
    std::printf("sun at 60 deg: zenith sky (%.4f %.4f %.4f), sun (%.3f %.3f %.3f), sky share of light on the ground %.3f\n", zenith.x,
                zenith.y, zenith.z, noonSun.x, noonSun.y, noonSun.z, diffuseShare);
    check(zenith.z > zenith.y && zenith.y > zenith.x, "the zenith is blue");
    check(diffuseShare > 0.05F && diffuseShare < 0.3F, "the sky gives a clear day's share of the light");
    e.sunDirection = sun_at(2.0F);
    const Float3 sunset = sun_irradiance(e);
    const Float3 horizonUnderSun = sky_radiance(e, sun_at(3.0F));
    std::printf("sun at 2 deg: sun (%.3f %.3f %.3f), sky beside it (%.4f %.4f %.4f)\n", sunset.x, sunset.y, sunset.z, horizonUnderSun.x,
                horizonUnderSun.y, horizonUnderSun.z);
    check(sunset.x > 1.5F * sunset.y && sunset.y > 1.5F * sunset.z, "the setting sun is orange-red");
    check(horizonUnderSun.x > horizonUnderSun.z, "the sky by the setting sun is warm");
    e.sunDirection = sun_at(-3.0F);
    const Float3 below = sun_irradiance(e);
    const Float3 twilight = sky_radiance(e, {0, 1, 0});
    std::printf("sun 3 deg below: sun (%.3g %.3g %.3g), zenith (%.3g %.3g %.3g)\n", below.x, below.y, below.z, twilight.x, twilight.y,
                twilight.z);
    check(below.x == 0.0F && below.y == 0.0F && below.z == 0.0F, "no direct sun below the horizon");
    check(twilight.z > 0.0F && twilight.z > twilight.x, "a blue twilight sky remains");
    // Haze: more Mie, a whiter and brighter halo round the sun.
    RenderEnvironment hazy = e;
    hazy.sunDirection = e.sunDirection = sun_at(30.0F);
    hazy.atmosphereMieScale = 10.0F;
    const Float3 near = sun_at(33.0F);
    const Float3 clearHalo = sky_radiance(e, near), hazyHalo = sky_radiance(hazy, near);
    std::printf("3 deg from the sun: clear (%.3f %.3f %.3f), hazy (%.3f %.3f %.3f)\n", clearHalo.x, clearHalo.y, clearHalo.z, hazyHalo.x,
                hazyHalo.y, hazyHalo.z);
    check(hazyHalo.x > 2.0F * clearHalo.x && hazyHalo.z / hazyHalo.x < clearHalo.z / clearHalo.x, "haze brightens and whitens the halo");
}

void test_colours_mode() {
    RenderEnvironment e;
    e.sunDirection = {0.3F, 0.5F, 0.2F};
    const Float3 d = normalize(Float3{0.2F, 0.4F, -0.7F});
    const float t = std::clamp(0.5F + 0.5F * d.y, 0.0F, 1.0F);
    const Float3 blend = add(multiply(e.groundColor, 1.0F - t), multiply(e.skyColor, t));
    const Float3 a = sky_radiance(e, d), b = sky_irradiance(e, d), s = sun_irradiance(e), bg = background_radiance(e, d);
    check(a.x == blend.x && a.z == blend.z && b.y == blend.y && bg.x == blend.x, "Colors mode: the sky/ground blend, unchanged");
    check(s.x == e.sunColor.x * e.sunIntensity && s.z == e.sunColor.z * e.sunIntensity, "Colors mode: sun colour x intensity");
    check(!atmosphere_model(e), "Colors mode builds no atmosphere");
}

void test_segment() {
    RenderEnvironment e;
    e.skyMode = SkyMode::Physical;
    e.sunDirection = sun_at(35.0F);
    const auto model = atmosphere_model(e);
    const AtmosphereParameters& p = model->parameters();
    // A level 2 km segment at 10 m: the air is uniform along it, so T = exp(-sigma_t(10 m) d).
    const AtmosphereModel::Segment level = model->segment(10.0F, {1, 0, 0}, 2000.0F, 12U);
    const AtmosphereMedium m = atmosphere_medium(p, 10.0F);
    const Float3 exact{std::exp(-m.extinction.x * 2000.0F), std::exp(-m.extinction.y * 2000.0F), std::exp(-m.extinction.z * 2000.0F)};
    std::printf("level 2 km of air: transmittance (%.5f %.5f %.5f), exact (%.5f %.5f %.5f)\n", level.transmittance.x, level.transmittance.y,
                level.transmittance.z, exact.x, exact.y, exact.z);
    check(rel(level.transmittance, exact) < 2e-4, "segment transmittance through level air");
    // The in-scattered light converges: 12 steps vs 4096, for level, rising and falling segments.
    double worst = 0.0;
    for (const Float3 d : {Float3{1, 0, 0}, normalize(Float3{0.3F, 0.5F, -0.8F}), normalize(Float3{-0.5F, -0.05F, 0.6F})})
        for (const float length : {50.0F, 800.0F, 20000.0F}) {
            const auto coarse = model->segment(30.0F, d, length, 12U), fine = model->segment(30.0F, d, length, 4096U);
            worst = std::max(worst, rel(coarse.inscattered, fine.inscattered));
        }
    std::printf("aerial perspective, 12 steps vs 4096: worst %.4f\n", worst);
    check(worst < 0.01, "12 steps are enough for scene-scale segments");
    // Short level segment: in-scattered light is the source times the length.
    const float d = 100.0F;
    const auto shortSeg = model->segment(10.0F, {1, 0, 0}, d, 64U);
    const float r = p.bottomRadiusMeters + 10.0F;
    const Float3 sun = normalize(e.sunDirection);
    const float nu = sun.x;
    const Float3 sunT = model->tables().sun_transmittance(r, sun.y), ms = model->tables().multiple_scattering(r, sun.y);
    const float pr = rayleigh_phase(nu), pm = cornette_shanks_phase(p.mieAnisotropy, nu);
    const auto source = [&](float sR, float sun, float msc) { return (sR * pr + m.scatteringMie * pm) * sun + (sR + m.scatteringMie) * msc; };
    const Float3 thin{source(m.scatteringRayleigh.x, sunT.x, ms.x) * d, source(m.scatteringRayleigh.y, sunT.y, ms.y) * d,
                      source(m.scatteringRayleigh.z, sunT.z, ms.z) * d};
    std::printf("100 m of air: in-scattered (%.4e %.4e %.4e), source x length (%.4e %.4e %.4e)\n", shortSeg.inscattered.x,
                shortSeg.inscattered.y, shortSeg.inscattered.z, thin.x, thin.y, thin.z);
    check(rel(shortSeg.inscattered, thin) < 0.01, "a short segment scatters its source times its length");
}

VoxelMaterialDefinition lambert(float grey) {
    VoxelMaterialDefinition m;
    m.baseColor = {grey, grey, grey, 1.0F};
    m.specular = 0.0F;
    return m;
}

void test_renderers() {
    // A white floor and a far wall; camera 3 m up looking along +z.
    VoxelObject object(1U);
    for (int x = -60; x < 60; ++x)
        for (int z = -10; z < 400; ++z) object.set_voxel({x, 0, z}, 1);
    for (int x = -60; x < 60; ++x)
        for (int y = 1; y < 40; ++y) object.set_voxel({x, y, 400}, 1);
    std::array<VoxelMaterialDefinition, 2> materials{VoxelMaterialDefinition{}, lambert(0.8F)};
    const std::vector<VoxelReferenceInstance> voxels{{1U, &object, {}, materials, true, {1, 1, 1}}};
    PolygonCamera camera;
    camera.position = {0.5F, 30.0F, 0.5F};
    camera.target = {0.5F, 25.0F, 60.0F};
    camera.up = {0, 1, 0};
    camera.verticalFieldOfViewRadians = 1.0F;
    camera.nearPlane = 0.5F;
    camera.farPlane = 1000.0F;
    RenderEnvironment e;
    e.skyMode = SkyMode::Physical;
    e.sunDirection = sun_at(40.0F, 70.0F);
    e.sunColor = {1, 1, 1};
    e.sunIntensity = 3.0F;
    e.globalIlluminationMode = GlobalIlluminationMode::Off;
    e.shadowMode = ShadowMode::Off;
    e.ambientOcclusionSamples = 0;
    e.aerialPerspectiveScale = 0.0F;
    const auto draw = [&](const RenderEnvironment& env) {
        PolygonRenderTarget target;
        target.resize(64, 48);
        target.enable_passes();
        (void)render_hybrid_reference(voxels, {}, camera, env, target, {}, 0.1F, 2U);
        return target;
    };
    const PolygonRenderTarget plain = draw(e);
    // A floor pixel: 0.8 x the sun through the air x cos.
    const Float3 sunGround = sun_irradiance(e);
    const float cosine = normalize(e.sunDirection).y;
    std::size_t floorPixel = 0, skyPixel = 0;
    for (std::size_t i = 0; i < plain.hdrColor.size(); ++i) {
        if (plain.objectId[i] == 1U && plain.passes.normal[i].y > 0.5F) floorPixel = i;
        if (plain.depth[i] >= 1.0F && skyPixel == 0U) skyPixel = i;
    }
    const Float3 want = multiply(sunGround, 0.8F * cosine);
    const Float4 got = plain.hdrColor[floorPixel];
    std::printf("physical sun on the floor: (%.4f %.4f %.4f), expected (%.4f %.4f %.4f)\n", got.x, got.y, got.z, want.x, want.y, want.z);
    check(rel(Float3{got.x, got.y, got.z}, want) < 1e-5, "the floor is lit by the sun through the air");
    {
        const std::uint32_t x = static_cast<std::uint32_t>(skyPixel % 64U), y = static_cast<std::uint32_t>(skyPixel / 64U);
        const Float3 forward = normalize(subtract(camera.target, camera.position));
        const Float3 right = normalize(Float3{forward.z, 0.0F, -forward.x});
        const Float3 rightHanded = normalize(Float3{forward.y * 0.0F - forward.z * 1.0F, forward.z * 0.0F - forward.x * 0.0F,
                                                    forward.x * 1.0F - forward.y * 0.0F});
        (void)right;
        const Float3 up{rightHanded.y * forward.z - rightHanded.z * forward.y, rightHanded.z * forward.x - rightHanded.x * forward.z,
                        rightHanded.x * forward.y - rightHanded.y * forward.x};
        const float tanHalf = std::tan(0.5F), nx = (2.0F * (x + 0.5F) / 64.0F - 1.0F) * tanHalf * (64.0F / 48.0F),
                    ny = (1.0F - 2.0F * (y + 0.5F) / 48.0F) * tanHalf;
        const Float3 dir = normalize(add(forward, add(multiply(rightHanded, nx), multiply(up, ny))));
        const Float3 sky = background_radiance(e, dir);
        const Float4 px = plain.hdrColor[skyPixel];
        check(rel(Float3{px.x, px.y, px.z}, sky) < 1e-5 && px.z > px.x, "the background is the blue sky along each ray");
    }

    // Aerial perspective (exaggerated 30x): far things hazier and bluer, the passes still sum.
    RenderEnvironment hazy = e;
    hazy.aerialPerspectiveScale = 30.0F;
    const PolygonRenderTarget far = draw(hazy);
    double worstSum = 0.0;
    std::size_t fogged = 0;
    for (std::size_t i = 0; i < far.hdrColor.size(); ++i) {
        worstSum = std::max(worstSum, rel(far.passes.light_sum(i), Float3{far.hdrColor[i].x, far.hdrColor[i].y, far.hdrColor[i].z}));
        fogged += far.passes.fog[i].z > 0.0F ? 1U : 0U;
    }
    // The wall row in the image centre vs a near floor row.
    const std::size_t wall = 20U * 64U + 32U, near = 47U * 64U + 32U;
    std::printf("aerial perspective x30: wall %.0f m away (%.3f %.3f %.3f), air in front of it (%.3f %.3f %.3f); near floor air "
                "(%.4f %.4f %.4f); passes sum to %.1e\n",
                far.passes.depthMeters[wall] * 30.0F, far.hdrColor[wall].x, far.hdrColor[wall].y, far.hdrColor[wall].z, far.passes.fog[wall].x,
                far.passes.fog[wall].y, far.passes.fog[wall].z, far.passes.fog[near].x, far.passes.fog[near].y, far.passes.fog[near].z, worstSum);
    check(far.objectId[wall] == 1U && far.passes.depthMeters[wall] > 30.0F, "the centre row sees the far wall");
    check(far.hdrColor[wall].z > far.hdrColor[wall].x && far.passes.fog[wall].z > far.passes.fog[wall].x,
          "the shaded far wall takes on the blue of the air");
    check(far.passes.fog[wall].z > 3.0F * far.passes.fog[near].z, "more air in front of far things than near ones");
    check(worstSum < 1e-5 && fogged > 100U, "the in-scattered air goes to the fog pass and the passes still sum");

    RenderEnvironment bad = e;
    bad.atmosphereMieAnisotropy = 1.0F;
    check(!bad.validate(), "anisotropy 1 is rejected");
    bad = e;
    bad.observerAltitudeMeters = -5.0F;
    check(!bad.validate(), "negative altitude is rejected");
}

} // namespace

int main() {
    using Clock = std::chrono::steady_clock;
    RenderEnvironment environment;
    environment.skyMode = SkyMode::Physical;
    environment.sunDirection = sun_at(25.0F);
    const auto start = Clock::now();
    const auto model = atmosphere_model(environment);
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    const auto again = Clock::now();
    const auto cached = atmosphere_model(environment);
    const double cachedMs = std::chrono::duration<double, std::milli>(Clock::now() - again).count();
    RenderEnvironment moved = environment;
    moved.sunDirection = sun_at(40.0F);
    const auto skyStart = Clock::now();
    (void)atmosphere_model(moved);
    const double skyMs = std::chrono::duration<double, std::milli>(Clock::now() - skyStart).count();
    std::printf("atmosphere tables built in %.0f ms; a new sun position (sky view + irradiance) %.0f ms; cached lookup %.3f ms\n", ms,
                skyMs, cachedMs);
    check(model && cached.get() == model.get(), "the model is cached");
    test_transmittance(model->tables());
    test_thin_single_scattering();
    test_tables(environment);
    test_multiple_scattering_vs_path_tracing();
    test_colours();
    test_colours_mode();
    test_segment();
    test_renderers();
    if (g_failures == 0) std::printf("dve_atmosphere_tests: PASS\n");
    return g_failures == 0 ? 0 : 1;
}
