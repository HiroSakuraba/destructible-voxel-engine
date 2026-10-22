#include "dve/render/atmosphere.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>

#include "dve/transform.hpp"

namespace dve::render {
namespace {
constexpr double kPi = 3.14159265358979323846;

// Geometry in double: radii are ~6.4e6 m and the shell is 1e5 m thick, beyond float precision.
struct D3 {
    double x{}, y{}, z{};
};
D3 d3(Float3 v) noexcept { return {v.x, v.y, v.z}; }
double dot(D3 a, D3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
D3 add(D3 a, D3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
D3 scale(D3 a, double s) noexcept { return {a.x * s, a.y * s, a.z * s}; }
double len(D3 a) noexcept { return std::sqrt(dot(a, a)); }
D3 unit(D3 a) noexcept { const double l = len(a); return l > 0.0 ? scale(a, 1.0 / l) : D3{0, 1, 0}; }

struct C3 {   // colour in double
    double r{}, g{}, b{};
};
C3 c3(Float3 v) noexcept { return {v.x, v.y, v.z}; }
Float3 f3(C3 c) noexcept { return {static_cast<float>(c.r), static_cast<float>(c.g), static_cast<float>(c.b)}; }
C3 operator+(C3 a, C3 b) noexcept { return {a.r + b.r, a.g + b.g, a.b + b.b}; }
C3 operator*(C3 a, C3 b) noexcept { return {a.r * b.r, a.g * b.g, a.b * b.b}; }
C3 operator*(C3 a, double s) noexcept { return {a.r * s, a.g * s, a.b * s}; }
C3 exp3(C3 a) noexcept { return {std::exp(-a.r), std::exp(-a.g), std::exp(-a.b)}; }

double top_distance(double R, double T, double r, double mu) noexcept {
    const double disc = r * r * (mu * mu - 1.0) + T * T;
    return std::max(0.0, -r * mu + std::sqrt(std::max(0.0, disc)));
}
double ground_distance(double R, double r, double mu) noexcept {
    const double disc = r * r * (mu * mu - 1.0) + R * R;
    if (mu >= 0.0 || disc < 0.0) return -1.0;
    return std::max(0.0, -r * mu - std::sqrt(disc));
}

struct MediumD {
    C3 rayleigh;
    double mie{};
    C3 extinction;
};
MediumD medium(const AtmosphereParameters& p, double altitude) noexcept {
    altitude = std::max(0.0, altitude);
    const double rd = std::exp(-altitude / p.rayleighScaleHeightMeters);
    const double md = std::exp(-altitude / p.mieScaleHeightMeters);
    const double od = std::max(0.0, 1.0 - std::abs(altitude - p.ozoneCenterAltitudeMeters) / p.ozoneHalfWidthMeters);
    MediumD m;
    m.rayleigh = c3(p.rayleighScattering) * rd;
    m.mie = p.mieScattering * md;
    const double mieExtinction = (p.mieScattering + p.mieAbsorption) * md;
    m.extinction = m.rayleigh + C3{mieExtinction, mieExtinction, mieExtinction} + c3(p.ozoneAbsorption) * od;
    return m;
}

double rayleigh_phase_d(double c) noexcept { return 3.0 / (16.0 * kPi) * (1.0 + c * c); }
double cornette_shanks_d(double g, double c) noexcept {
    const double k = 3.0 / (8.0 * kPi) * (1.0 - g * g) / (2.0 + g * g);
    return k * (1.0 + c * c) / std::pow(std::max(1.0e-12, 1.0 + g * g - 2.0 * g * c), 1.5);
}

// Optical depth along a straight line from radius r (zenith cosine mu) for `length`.
C3 optical_depth(const AtmosphereParameters& p, double r, double mu, double length, std::uint32_t steps) noexcept {
    C3 depth{};
    const double dt = length / steps;
    for (std::uint32_t i = 0; i < steps; ++i) {
        const double t = (i + 0.5) * dt;
        const double ri = std::sqrt(r * r + t * t + 2.0 * r * mu * t);
        depth = depth + medium(p, ri - p.bottomRadiusMeters).extinction * dt;
    }
    return depth;
}

// The same, with samples bunched where the air is thickest: at the start, and at the lowest point
// (perigee) of a ray that first descends. Two quadratic-spaced pieces of `steps` each.
C3 optical_depth_fine(const AtmosphereParameters& p, double r, double mu, double length, std::uint32_t steps) noexcept {
    const auto piece = [&](double a, double b, bool denseAtA) {
        C3 depth{};
        double previous = 0.0;
        for (std::uint32_t i = 0; i < steps; ++i) {
            const double q = static_cast<double>(i + 1U) / steps;
            const double f = denseAtA ? q * q : 1.0 - (1.0 - q) * (1.0 - q);
            const double dt = (f - previous) * (b - a), tm = a + (previous + 0.5 * (f - previous)) * (b - a);
            previous = f;
            const double ri = std::sqrt(r * r + tm * tm + 2.0 * r * mu * tm);
            depth = depth + medium(p, ri - p.bottomRadiusMeters).extinction * dt;
        }
        return depth;
    };
    const double perigee = -r * mu;
    if (perigee > 0.0 && perigee < length) return piece(0.0, perigee, false) + piece(perigee, length, true);
    return piece(0.0, length, true);
}

float clamp01(float v) noexcept { return std::clamp(v, 0.0F, 1.0F); }

Float3 bilinear(const std::vector<Float3>& table, std::uint32_t w, std::uint32_t h, float u, float v) noexcept {
    const float x = std::clamp(u * w - 0.5F, 0.0F, static_cast<float>(w - 1U));
    const float y = std::clamp(v * h - 0.5F, 0.0F, static_cast<float>(h - 1U));
    const auto x0 = std::min(static_cast<std::uint32_t>(x), w - 2U), y0 = std::min(static_cast<std::uint32_t>(y), h - 2U);
    const float tx = x - x0, ty = y - y0;
    const auto at = [&](std::uint32_t i, std::uint32_t j) { return table[static_cast<std::size_t>(j) * w + i]; };
    const auto mix = [](Float3 a, Float3 b, float t) { return Float3{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t}; };
    return mix(mix(at(x0, y0), at(x0 + 1U, y0), tx), mix(at(x0, y0 + 1U), at(x0 + 1U, y0 + 1U), tx), ty);
}

// The scattering integral along a ray, shared by the tables and the brute-force reference.
// sunAt(r, muS) gives the sunlight reaching a point; ms(r, muS) the multiple-scattering source
// (or zero). Samples sit on a quadratic distribution (dense near the start, where the air is
// thickest along horizontal rays).
template <class SunAt, class Ms>
C3 march(const AtmosphereParameters& p, double r, D3 view, D3 sun, std::uint32_t steps, const SunAt& sunAt, const Ms& ms,
         C3* transmittanceOut = nullptr, double maxLength = -1.0, bool groundTerm = true) {
    const double R = p.bottomRadiusMeters, T = p.topRadiusMeters;
    const double mu = view.y;
    const double ground = ground_distance(R, r, mu);
    double length = ground >= 0.0 ? ground : top_distance(R, T, r, mu);
    bool hitsGround = ground >= 0.0;
    if (maxLength >= 0.0 && maxLength < length) { length = maxLength; hitsGround = false; }
    const double nu = dot(view, sun);
    const double phaseR = rayleigh_phase_d(nu), phaseM = cornette_shanks_d(p.mieAnisotropy, nu);
    const D3 origin{0.0, r, 0.0};
    C3 L{}, throughput{1.0, 1.0, 1.0};
    double previous = 0.0;
    for (std::uint32_t i = 0; i < steps; ++i) {
        const double a = static_cast<double>(i + 1U) / steps;
        const double t1 = length * a * a;
        const double dt = t1 - previous;
        const double tm = previous + 0.5 * dt;
        previous = t1;
        const D3 pos = add(origin, scale(view, tm));
        const double rp = len(pos);
        const double muS = dot(pos, sun) / rp;
        const MediumD m = medium(p, rp - R);
        const C3 sunLight = c3(sunAt(rp, muS));
        const C3 scatter = m.rayleigh + C3{m.mie, m.mie, m.mie};
        const C3 single = (m.rayleigh * phaseR + C3{m.mie, m.mie, m.mie} * phaseM) * sunLight;
        const C3 source = single + c3(ms(rp, muS)) * scatter;
        const C3 segment = exp3(m.extinction * dt);
        // Analytic integral of a constant source across the step: S (1 - e^{-sigma dt}) / sigma.
        const auto part = [&](double s, double e, double tr) { return e > 1.0e-14 ? s * (1.0 - tr) / e : s * dt; };
        L = L + throughput * C3{part(source.r, m.extinction.r, segment.r), part(source.g, m.extinction.g, segment.g),
                                part(source.b, m.extinction.b, segment.b)};
        throughput = throughput * segment;
    }
    if (hitsGround && groundTerm) {
        const D3 pos = add(origin, scale(view, length));
        const D3 up = unit(pos);
        const double muS = dot(up, sun);
        L = L + throughput * c3(sunAt(R, muS)) * (std::max(0.0, muS) * p.groundAlbedo / kPi);
    }
    if (transmittanceOut) *transmittanceOut = throughput;
    return L;
}

D3 sun_frame(double muS) noexcept { return {std::sqrt(std::max(0.0, 1.0 - muS * muS)), muS, 0.0}; }
} // namespace

AtmosphereParameters atmosphere_parameters(const RenderEnvironment& e) noexcept {
    AtmosphereParameters p;
    p.rayleighScattering = multiply(p.rayleighScattering, e.atmosphereRayleighScale);
    p.mieScattering *= e.atmosphereMieScale;
    p.mieAbsorption *= e.atmosphereMieScale;
    p.ozoneAbsorption = multiply(p.ozoneAbsorption, e.atmosphereOzoneScale);
    p.mieAnisotropy = e.atmosphereMieAnisotropy;
    p.groundAlbedo = e.atmosphereGroundAlbedo;
    return p;
}

AtmosphereMedium atmosphere_medium(const AtmosphereParameters& p, float altitudeMeters) noexcept {
    const MediumD m = medium(p, altitudeMeters);
    return {f3(m.rayleigh), static_cast<float>(m.mie), f3(m.extinction)};
}
float rayleigh_phase(float c) noexcept { return static_cast<float>(rayleigh_phase_d(c)); }
float cornette_shanks_phase(float g, float c) noexcept { return static_cast<float>(cornette_shanks_d(g, c)); }
float distance_to_top(const AtmosphereParameters& p, float r, float mu) noexcept {
    return static_cast<float>(top_distance(p.bottomRadiusMeters, p.topRadiusMeters, r, mu));
}
float distance_to_ground(const AtmosphereParameters& p, float r, float mu) noexcept {
    return static_cast<float>(ground_distance(p.bottomRadiusMeters, r, mu));
}

// Bruneton's transmittance parameterisation (rays that do not hit the ground).
AtmosphereUv transmittance_uv(const AtmosphereParameters& p, float rf, float muf) noexcept {
    const double R = p.bottomRadiusMeters, T = p.topRadiusMeters;
    const double r = std::clamp<double>(rf, R, T), mu = std::clamp<double>(muf, -1.0, 1.0);
    const double H = std::sqrt(T * T - R * R);
    const double rho = std::sqrt(std::max(0.0, r * r - R * R));
    const double d = top_distance(R, T, r, mu);
    const double dMin = T - r, dMax = rho + H;
    return {static_cast<float>(dMax > dMin ? (d - dMin) / (dMax - dMin) : 0.0), static_cast<float>(rho / H)};
}
void transmittance_r_mu(const AtmosphereParameters& p, AtmosphereUv uv, float& rOut, float& muOut) noexcept {
    const double R = p.bottomRadiusMeters, T = p.topRadiusMeters;
    const double H = std::sqrt(T * T - R * R);
    const double rho = H * uv.v;
    const double r = std::sqrt(rho * rho + R * R);
    const double dMin = T - r, dMax = rho + H;
    const double d = dMin + uv.u * (dMax - dMin);
    const double mu = d == 0.0 ? 1.0 : std::clamp((H * H - rho * rho - d * d) / (2.0 * r * d), -1.0, 1.0);
    rOut = static_cast<float>(r);
    muOut = static_cast<float>(mu);
}

// Sky view: u = azimuth from the sun / pi; v below 0.5 above the horizon, above 0.5 below it,
// square-root spaced toward the horizon on both sides.
AtmosphereUv sky_view_uv(const AtmosphereParameters& p, float r, float zenithCos, float azimuth) noexcept {
    const double R = p.bottomRadiusMeters;
    const double horizon = kPi - std::asin(std::clamp(R / std::max<double>(r, R), 0.0, 1.0));   // zenith angle of the horizon
    const double zenith = std::acos(std::clamp<double>(zenithCos, -1.0, 1.0));
    double v;
    if (zenith < horizon) {
        const double t = zenith / horizon;
        v = 0.5 * (1.0 - std::sqrt(std::max(0.0, 1.0 - t)));
    } else {
        const double t = (zenith - horizon) / (kPi - horizon);
        v = 0.5 + 0.5 * std::sqrt(std::max(0.0, t));
    }
    return {static_cast<float>(std::clamp<double>(azimuth, 0.0, kPi) / kPi), static_cast<float>(v)};
}
void sky_view_angles(const AtmosphereParameters& p, float r, AtmosphereUv uv, float& zenithCos, float& azimuth) noexcept {
    const double R = p.bottomRadiusMeters;
    const double horizon = kPi - std::asin(std::clamp(R / std::max<double>(r, R), 0.0, 1.0));
    double zenith;
    if (uv.v < 0.5F) {
        const double s = 1.0 - 2.0 * uv.v;
        zenith = horizon * (1.0 - s * s);
    } else {
        const double s = 2.0 * uv.v - 1.0;
        zenith = horizon + (kPi - horizon) * s * s;
    }
    zenithCos = static_cast<float>(std::cos(zenith));
    azimuth = static_cast<float>(uv.u * kPi);
}

AtmosphereTables::AtmosphereTables(const AtmosphereParameters& parameters) : parameters_(parameters) {
    const AtmosphereParameters& p = parameters_;
    transmittance_.resize(static_cast<std::size_t>(kTransmittanceWidth) * kTransmittanceHeight);
    for (std::uint32_t j = 0; j < kTransmittanceHeight; ++j)
        for (std::uint32_t i = 0; i < kTransmittanceWidth; ++i) {
            float r, mu;
            // Node-centred (Bruneton): the first and last texels sit exactly on the range ends.
            transmittance_r_mu(p, {static_cast<float>(i) / (kTransmittanceWidth - 1U), static_cast<float>(j) / (kTransmittanceHeight - 1U)}, r, mu);
            transmittance_[static_cast<std::size_t>(j) * kTransmittanceWidth + i] =
                f3(exp3(optical_depth_fine(p, r, mu, top_distance(p.bottomRadiusMeters, p.topRadiusMeters, r, mu), 192U)));
        }
    // Multiple scattering (Hillaire 2020, section 5.5): for each altitude and sun elevation,
    // second-order light L2 and the transfer f_ms averaged over 64 directions with an isotropic
    // phase; psi_ms = L2 / (1 - f_ms) sums all higher orders as a geometric series.
    multipleScattering_.resize(static_cast<std::size_t>(kMultipleScatteringSize) * kMultipleScatteringSize);
    const auto sunAt = [this](double r, double muS) { return sun_transmittance(static_cast<float>(r), static_cast<float>(muS)); };
    constexpr std::uint32_t kDirections = 16U, kSteps = 32U;
    const double R = p.bottomRadiusMeters, T = p.topRadiusMeters;
    const double isoPhase = 1.0 / (4.0 * kPi);
    for (std::uint32_t j = 0; j < kMultipleScatteringSize; ++j)
        for (std::uint32_t i = 0; i < kMultipleScatteringSize; ++i) {
            const double muS = -1.0 + 2.0 * (i + 0.5) / kMultipleScatteringSize;
            const double r = R + (T - R) * (j + 0.5) / kMultipleScatteringSize;
            const D3 sun = sun_frame(muS);
            const D3 origin{0.0, r, 0.0};
            C3 secondOrder{}, transfer{};
            for (std::uint32_t a = 0; a < kDirections; ++a)
                for (std::uint32_t c = 0; c < kDirections; ++c) {
                    // Stratified uniform directions over the sphere.
                    const double cosTheta = 1.0 - 2.0 * (a + 0.5) / kDirections;
                    const double phi = 2.0 * kPi * (c + 0.5) / kDirections;
                    const double sinTheta = std::sqrt(std::max(0.0, 1.0 - cosTheta * cosTheta));
                    const D3 view{sinTheta * std::cos(phi), cosTheta, sinTheta * std::sin(phi)};
                    const double ground = ground_distance(R, r, view.y);
                    const double length = ground >= 0.0 ? ground : top_distance(R, T, r, view.y);
                    C3 throughput{1.0, 1.0, 1.0}, L{}, f{};
                    double previous = 0.0;
                    for (std::uint32_t k = 0; k < kSteps; ++k) {
                        const double q = static_cast<double>(k + 1U) / kSteps;
                        const double t1 = length * q * q, dt = t1 - previous, tm = previous + 0.5 * dt;
                        previous = t1;
                        const D3 pos = add(origin, scale(view, tm));
                        const double rp = len(pos);
                        const MediumD m = medium(p, rp - R);
                        const C3 scatter = m.rayleigh + C3{m.mie, m.mie, m.mie};
                        const C3 segment = exp3(m.extinction * dt);
                        const auto part = [&](C3 src) {
                            const auto one = [&](double sv, double e, double tr) { return e > 1.0e-14 ? sv * (1.0 - tr) / e : sv * dt; };
                            return C3{one(src.r, m.extinction.r, segment.r), one(src.g, m.extinction.g, segment.g),
                                      one(src.b, m.extinction.b, segment.b)};
                        };
                        L = L + throughput * part(scatter * c3(sunAt(rp, dot(pos, sun) / rp)) * isoPhase);
                        f = f + throughput * part(scatter);
                        throughput = throughput * segment;
                    }
                    if (ground >= 0.0) {
                        const D3 up = unit(add(origin, scale(view, length)));
                        const double muG = dot(up, sun);
                        L = L + throughput * c3(sunAt(R, muG)) * (std::max(0.0, muG) * p.groundAlbedo / kPi);
                    }
                    secondOrder = secondOrder + L * (1.0 / (kDirections * kDirections));
                    transfer = transfer + f * (isoPhase * 4.0 * kPi / (kDirections * kDirections));
                }
            const C3 psi{secondOrder.r / std::max(1.0e-6, 1.0 - transfer.r), secondOrder.g / std::max(1.0e-6, 1.0 - transfer.g),
                         secondOrder.b / std::max(1.0e-6, 1.0 - transfer.b)};
            multipleScattering_[static_cast<std::size_t>(j) * kMultipleScatteringSize + i] = f3(psi);
        }
}

Float3 AtmosphereTables::transmittance_to_top(float r, float mu) const noexcept {
    const AtmosphereUv uv = transmittance_uv(parameters_, r, mu);
    return bilinear(transmittance_, kTransmittanceWidth, kTransmittanceHeight, (uv.u * (kTransmittanceWidth - 1U) + 0.5F) / kTransmittanceWidth,
                    (uv.v * (kTransmittanceHeight - 1U) + 0.5F) / kTransmittanceHeight);
}
Float3 AtmosphereTables::sun_transmittance(float r, float muSun) const noexcept {
    if (ground_distance(parameters_.bottomRadiusMeters, std::max<double>(r, parameters_.bottomRadiusMeters), muSun) >= 0.0) return {};
    return transmittance_to_top(r, muSun);
}
Float3 AtmosphereTables::multiple_scattering(float r, float muSun) const noexcept {
    const float u = clamp01(0.5F + 0.5F * muSun);
    const float v = clamp01((r - parameters_.bottomRadiusMeters) / (parameters_.topRadiusMeters - parameters_.bottomRadiusMeters));
    return bilinear(multipleScattering_, kMultipleScatteringSize, kMultipleScatteringSize, u, v);
}

Float3 integrate_transmittance(const AtmosphereParameters& p, float r, float mu, float lengthMeters, std::uint32_t steps) noexcept {
    return f3(exp3(optical_depth(p, r, mu, lengthMeters, std::max(1U, steps))));
}

Float3 integrate_sky_radiance(const AtmosphereParameters& p, const AtmosphereTables* tables, float r, Float3 view, Float3 sun,
                              std::uint32_t steps) noexcept {
    const auto sunAt = [&](double rp, double muS) -> Float3 {
        if (tables) return tables->sun_transmittance(static_cast<float>(rp), static_cast<float>(muS));
        if (ground_distance(p.bottomRadiusMeters, rp, muS) >= 0.0) return {};
        return f3(exp3(optical_depth(p, rp, muS, top_distance(p.bottomRadiusMeters, p.topRadiusMeters, rp, muS), 256U)));
    };
    const auto ms = [&](double rp, double muS) -> Float3 {
        return tables ? tables->multiple_scattering(static_cast<float>(rp), static_cast<float>(muS)) : Float3{};
    };
    return f3(march(p, r, unit(d3(view)), unit(d3(sun)), std::max(1U, steps), sunAt, ms));
}

namespace {
// Sky-view lookup for a direction in the sun's frame (sun at azimuth 0, +x).
Float3 sky_lookup(const AtmosphereParameters& p, float r, const std::vector<Float3>& table, D3 d) noexcept {
    d = unit(d);
    const double azimuth = std::atan2(std::abs(d.z), d.x);   // symmetric about the sun's plane
    const AtmosphereUv uv = sky_view_uv(p, r, static_cast<float>(d.y), static_cast<float>(azimuth));
    return bilinear(table, AtmosphereModel::kSkyViewWidth, AtmosphereModel::kSkyViewHeight, uv.u, uv.v);
}
// A world direction in the sun's frame: same height, azimuth measured from the sun's.
D3 to_sun_frame(Float3 sunWorld, Float3 direction) noexcept {
    const D3 d = unit(d3(direction));
    const double sx = sunWorld.x, sz = sunWorld.z, h = std::sqrt(sx * sx + sz * sz);
    if (h < 1.0e-9) return d;   // sun at the zenith or nadir: every azimuth is the same
    const double cx = sx / h, cz = sz / h;
    return {d.x * cx + d.z * cz, d.y, -d.x * cz + d.z * cx};
}
} // namespace

AtmosphereModel::AtmosphereModel(std::shared_ptr<const AtmosphereTables> tables, float observerAltitudeMeters, Float3 sunDirection)
    : tables_(std::move(tables)) {
    const AtmosphereParameters& p = tables_->parameters();
    observerRadius_ = p.bottomRadiusMeters + std::clamp(observerAltitudeMeters, 1.0F, p.topRadiusMeters - p.bottomRadiusMeters - 1.0F);
    sun_ = normalize(sunDirection);
    const D3 sun = sun_frame(std::clamp<double>(sun_.y, -1.0, 1.0));
    skyView_.resize(static_cast<std::size_t>(kSkyViewWidth) * kSkyViewHeight);
    for (std::uint32_t j = 0; j < kSkyViewHeight; ++j)
        for (std::uint32_t i = 0; i < kSkyViewWidth; ++i) {
            float zenithCos, azimuth;
            sky_view_angles(p, observerRadius_, {(i + 0.5F) / kSkyViewWidth, (j + 0.5F) / kSkyViewHeight}, zenithCos, azimuth);
            const double s = std::sqrt(std::max(0.0, 1.0 - static_cast<double>(zenithCos) * zenithCos));
            const Float3 view{static_cast<float>(s * std::cos(azimuth)), zenithCos, static_cast<float>(s * std::sin(azimuth))};
            skyView_[static_cast<std::size_t>(j) * kSkyViewWidth + i] =
                integrate_sky_radiance(p, tables_.get(), observerRadius_, view, f3(C3{sun.x, sun.y, sun.z}), 32U);
        }
    // Irradiance by normal: the sky-view radiance over the normal's hemisphere, cosine weighted
    // (pi x the mean over cosine-distributed directions).
    irradiance_.resize(static_cast<std::size_t>(kIrradianceWidth) * kIrradianceHeight);
    constexpr std::uint32_t kA = 32U, kB = 64U;
    for (std::uint32_t j = 0; j < kIrradianceHeight; ++j)
        for (std::uint32_t i = 0; i < kIrradianceWidth; ++i) {
            // Node-centred: the first and last columns are the nadir and zenith exactly.
            const double ny = -1.0 + 2.0 * i / (kIrradianceWidth - 1U);
            const double az = kPi * j / (kIrradianceHeight - 1U);
            const double ns = std::sqrt(std::max(0.0, 1.0 - ny * ny));
            const D3 n{ns * std::cos(az), ny, ns * std::sin(az)};
            // Orthonormal basis around n.
            const D3 helper = std::abs(n.y) < 0.9 ? D3{0, 1, 0} : D3{1, 0, 0};
            const D3 t = unit(D3{helper.y * n.z - helper.z * n.y, helper.z * n.x - helper.x * n.z, helper.x * n.y - helper.y * n.x});
            const D3 b{n.y * t.z - n.z * t.y, n.z * t.x - n.x * t.z, n.x * t.y - n.y * t.x};
            C3 sum{};
            for (std::uint32_t a = 0; a < kA; ++a)
                for (std::uint32_t c = 0; c < kB; ++c) {
                    const double u1 = (a + 0.5) / kA, u2 = (c + 0.5) / kB;
                    const double rr = std::sqrt(u1), ph = 2.0 * kPi * u2;
                    const D3 d = add(add(scale(t, rr * std::cos(ph)), scale(b, rr * std::sin(ph))), scale(n, std::sqrt(1.0 - u1)));
                    // Directions are relative to a sun at azimuth 0 (+x).
                    sum = sum + c3(sky_lookup(p, observerRadius_, skyView_, d));
                }
            irradiance_[static_cast<std::size_t>(j) * kIrradianceWidth + i] = f3(sum * (kPi / (kA * kB)));
        }
}

} // namespace dve::render

namespace dve::render {

Float3 AtmosphereModel::sky_radiance(Float3 direction) const noexcept {
    return sky_lookup(parameters(), observerRadius_, skyView_, to_sun_frame(sun_, direction));
}

Float3 AtmosphereModel::sky_irradiance(Float3 normal) const noexcept {
    const D3 n = to_sun_frame(sun_, normal);
    // Node-centred table: (W - 1) and (H - 1) intervals.
    const float u = clamp01(static_cast<float>(0.5 + 0.5 * n.y)), v = clamp01(static_cast<float>(std::atan2(std::abs(n.z), n.x) / kPi));
    return bilinear(irradiance_, kIrradianceWidth, kIrradianceHeight, (u * (kIrradianceWidth - 1U) + 0.5F) / kIrradianceWidth,
                    (v * (kIrradianceHeight - 1U) + 0.5F) / kIrradianceHeight);
}

Float3 AtmosphereModel::sun_transmittance() const noexcept { return tables_->sun_transmittance(observerRadius_, sun_.y); }

AtmosphereModel::Segment AtmosphereModel::segment(float altitudeMeters, Float3 direction, float lengthMeters,
                                                  std::uint32_t steps) const noexcept {
    Segment out;
    if (!(lengthMeters > 0.0F)) return out;
    const AtmosphereParameters& p = parameters();
    const double r = p.bottomRadiusMeters + std::clamp<double>(altitudeMeters, 0.0, p.topRadiusMeters - p.bottomRadiusMeters);
    // In the sun's frame the observer is at (0, r, 0) with +y up; the march is the sky integral
    // cut at the segment's end, without the ground.
    const D3 view = to_sun_frame(sun_, direction);
    const D3 sun = sun_frame(std::clamp<double>(sun_.y, -1.0, 1.0));
    const auto sunAt = [this](double rp, double muS) { return tables_->sun_transmittance(static_cast<float>(rp), static_cast<float>(muS)); };
    const auto ms = [this](double rp, double muS) { return tables_->multiple_scattering(static_cast<float>(rp), static_cast<float>(muS)); };
    C3 transmittance{};
    out.inscattered = f3(march(p, r, view, sun, std::max(1U, steps), sunAt, ms, &transmittance, lengthMeters, false));
    out.transmittance = f3(transmittance);
    return out;
}

namespace {
struct CachedTables {
    AtmosphereParameters parameters;
    std::shared_ptr<const AtmosphereTables> tables;
};
struct CachedModel {
    AtmosphereParameters parameters;
    float altitude{};
    Float3 sun{};
    std::shared_ptr<const AtmosphereModel> model;
};
std::mutex& cache_mutex() { static std::mutex m; return m; }
std::deque<CachedTables>& tables_cache() { static std::deque<CachedTables> c; return c; }
std::deque<CachedModel>& model_cache() { static std::deque<CachedModel> c; return c; }

std::shared_ptr<const AtmosphereModel> model_for(const RenderEnvironment& environment) {
    if (environment.skyMode != SkyMode::Physical) return nullptr;
    const AtmosphereParameters p = atmosphere_parameters(environment);
    const Float3 sun = normalize(environment.sunDirection);
    const float altitude = environment.observerAltitudeMeters;
    // Renderers ask once per pixel or ray: answer the last model this thread used without a lock.
    thread_local CachedModel last;
    if (last.model && last.parameters == p && last.altitude == altitude && last.sun.x == sun.x && last.sun.y == sun.y &&
        last.sun.z == sun.z)
        return last.model;
    const auto remember = [&](std::shared_ptr<const AtmosphereModel> m) {
        last = {p, altitude, sun, m};
        return m;
    };
    const auto cached = [&]() -> std::shared_ptr<const AtmosphereModel> {
        std::lock_guard lock(cache_mutex());
        for (const CachedModel& m : model_cache())
            if (m.parameters == p && m.altitude == altitude && m.sun.x == sun.x && m.sun.y == sun.y && m.sun.z == sun.z) return m.model;
        return nullptr;
    };
    if (auto m = cached()) return remember(m);
    // One builder at a time: threads that arrive together wait for the first one's tables.
    static std::mutex buildMutex;
    std::lock_guard build(buildMutex);
    if (auto m = cached()) return remember(m);
    std::shared_ptr<const AtmosphereTables> tables;
    {
        std::lock_guard lock(cache_mutex());
        for (const CachedTables& t : tables_cache())
            if (t.parameters == p) tables = t.tables;
    }
    if (!tables) {
        tables = std::make_shared<const AtmosphereTables>(p);
        std::lock_guard lock(cache_mutex());
        tables_cache().push_front({p, tables});
        if (tables_cache().size() > 4U) tables_cache().pop_back();
    }
    auto model = std::make_shared<const AtmosphereModel>(tables, altitude, sun);
    std::lock_guard lock(cache_mutex());
    model_cache().push_front({p, altitude, sun, model});
    if (model_cache().size() > 8U) model_cache().pop_back();
    return remember(model);
}

Float3 colour_blend(const RenderEnvironment& environment, Float3 direction) noexcept {
    const float t = std::clamp(0.5F + 0.5F * normalize(direction).y, 0.0F, 1.0F);
    return add(multiply(environment.groundColor, 1.0F - t), multiply(environment.skyColor, t));
}
Float3 sun_top(const RenderEnvironment& environment) noexcept { return multiply(environment.sunColor, environment.sunIntensity); }
Float3 mul(Float3 a, Float3 b) noexcept { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
} // namespace

std::shared_ptr<const AtmosphereModel> atmosphere_model(const RenderEnvironment& environment) { return model_for(environment); }

Float3 sky_radiance(const RenderEnvironment& environment, Float3 direction) noexcept {
    if (environment.skyMode != SkyMode::Physical) return colour_blend(environment, direction);
    const auto model = model_for(environment);
    return multiply(mul(model->sky_radiance(direction), sun_top(environment)), static_cast<float>(kPi));
}

Float3 sky_irradiance(const RenderEnvironment& environment, Float3 normal) noexcept {
    if (environment.skyMode != SkyMode::Physical) return colour_blend(environment, normal);
    const auto model = model_for(environment);
    return mul(model->sky_irradiance(normal), sun_top(environment));
}

Float3 sun_irradiance(const RenderEnvironment& environment) noexcept {
    if (environment.skyMode != SkyMode::Physical) return sun_top(environment);
    return mul(model_for(environment)->sun_transmittance(), sun_top(environment));
}

Float3 background_radiance(const RenderEnvironment& environment, Float3 direction) noexcept {
    Float3 sky = sky_radiance(environment, direction);
    if (environment.skyMode != SkyMode::Physical || !(environment.sunDisc > 0.0F)) return sky;
    const Float3 d = normalize(direction), s = normalize(environment.sunDirection);
    if (dot(d, s) < std::cos(kSunAngularRadius)) return sky;
    // The disc's radiance is the sun's irradiance over its solid angle; drawn as pi x radiance.
    const double solidAngle = 2.0 * kPi * (1.0 - std::cos(static_cast<double>(kSunAngularRadius)));
    const Float3 disc = mul(sun_irradiance(environment), Float3{1.0F, 1.0F, 1.0F});
    return add(sky, multiply(disc, static_cast<float>(kPi / solidAngle) * environment.sunDisc));
}

} // namespace dve::render
