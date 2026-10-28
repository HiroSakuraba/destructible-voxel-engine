#include "dve/nurbs_curvature.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace dve {
namespace {
using V = std::array<double,3>;
V add(V a, V b) { return {a[0]+b[0], a[1]+b[1], a[2]+b[2]}; }
V sub(V a, V b) { return {a[0]-b[0], a[1]-b[1], a[2]-b[2]}; }
V mul(V a, double f) { return {a[0]*f, a[1]*f, a[2]*f}; }
double dot(V a, V b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
V cross(V a, V b) { return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]}; }
double norm(V a) { return std::hypot(a[0], a[1], a[2]); }
template<class T> std::optional<T> fail(std::string* e, std::string message) { if (e) *e = std::move(message); return std::nullopt; }

unsigned find_span(const NurbsAxis& a, unsigned n, double t, bool fromLeft) {
    if (!(t < a.knots[n])) return n - 1;
    const auto begin = a.knots.begin() + a.degree, end = a.knots.begin() + n + 1;
    const auto it = fromLeft && t > a.knots[a.degree] ? std::lower_bound(begin, end, t) : std::upper_bound(begin, end, t);
    return unsigned(it - a.knots.begin() - 1);
}

// Basis functions and their first two derivatives on one span (The NURBS Book, A2.3).
struct Ders { unsigned first{}; double d[3][8]{}; };
Ders basis_ders(const NurbsAxis& a, unsigned n, double t, bool fromLeft) {
    Ders out;
    const unsigned p = a.degree;
    if (p == 0) { out.d[0][0] = 1; return out; }
    const unsigned span = find_span(a, n, t, fromLeft);
    out.first = span - p;
    double ndu[8][8]{}, left[8]{}, right[8]{};
    ndu[0][0] = 1;
    for (unsigned j = 1; j <= p; ++j) {
        left[j] = t - a.knots[span + 1 - j];
        right[j] = a.knots[span + j] - t;
        double saved = 0;
        for (unsigned r = 0; r < j; ++r) {
            ndu[j][r] = right[r + 1] + left[j - r];
            const double temp = ndu[r][j - 1] / ndu[j][r];
            ndu[r][j] = saved + right[r + 1] * temp;
            saved = left[j - r] * temp;
        }
        ndu[j][j] = saved;
    }
    for (unsigned j = 0; j <= p; ++j) out.d[0][j] = ndu[j][p];
    const int order = int(std::min(2U, p)), pi = int(p);
    for (int r = 0; r <= pi; ++r) {
        double c[2][8]{};
        int s1 = 0, s2 = 1;
        c[0][0] = 1;
        for (int k = 1; k <= order; ++k) {
            double d = 0;
            const int rk = r - k, pk = pi - k;
            if (r >= k) { c[s2][0] = c[s1][0] / ndu[pk + 1][rk]; d = c[s2][0] * ndu[rk][pk]; }
            const int j1 = rk >= -1 ? 1 : -rk, j2 = r - 1 <= pk ? k - 1 : pi - r;
            for (int j = j1; j <= j2; ++j) {
                c[s2][j] = (c[s1][j] - c[s1][j - 1]) / ndu[pk + 1][rk + j];
                d += c[s2][j] * ndu[rk + j][pk];
            }
            if (r <= pk) { c[s2][k] = -c[s1][k - 1] / ndu[pk + 1][r]; d += c[s2][k] * ndu[r][pk]; }
            out.d[k][r] = d;
            std::swap(s1, s2);
        }
    }
    double factor = p;
    for (int k = 1; k <= order; ++k) {
        for (unsigned j = 0; j <= p; ++j) out.d[k][j] *= factor;
        factor *= double(pi - k);
    }
    return out;
}

double geometry_size(std::initializer_list<const NurbsSource*> sources) {
    V lo{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
    V hi{-lo[0], -lo[1], -lo[2]};
    for (const auto* s : sources)
        for (const auto& p : s->controls) {
            const V q{p.x, p.y, p.z};
            for (unsigned k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], q[k]); hi[k] = std::max(hi[k], q[k]); }
        }
    return std::max(norm(sub(hi, lo)), 1e-9);
}

NurbsContinuity classify(double gap, double angle, double curvatureA, double curvatureB, double difference, double size,
                         const NurbsContinuityTolerance& t) {
    if (!(gap <= t.position * size)) return NurbsContinuity::None;
    if (!(angle <= t.angleRadians)) return NurbsContinuity::G0;
    if (!(difference <= t.curvature * std::max(curvatureA, curvatureB) + t.curvature / size)) return NurbsContinuity::G1;
    return NurbsContinuity::G2;
}

void finish(NurbsContinuityReport& report, std::string_view where) {
    report.level = NurbsContinuity::G2;
    const NurbsContinuityBreak* worst = nullptr;
    for (const auto& place : report.places) {
        report.maximumGap = std::max(report.maximumGap, place.gap);
        report.maximumAngleRadians = std::max(report.maximumAngleRadians, place.angleRadians);
        report.maximumCurvatureDifference = std::max(report.maximumCurvatureDifference, place.curvatureDifference);
        report.maximumCurvature = std::max({report.maximumCurvature, place.curvatureA, place.curvatureB});
        if (!worst || place.level < worst->level) worst = &place;
    }
    if (worst) report.level = worst->level;
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(4) << nurbs_continuity_name(report.level) << ' ' << where;
    if (report.places.empty()) { out << ": nothing to measure"; report.description = out.str(); return; }
    out << ": max gap " << report.maximumGap << ", max tangent angle " << report.maximumAngleRadians * 57.29577951308232
        << " deg, max curvature jump " << report.maximumCurvatureDifference << " (1/unit)";
    if (worst && report.level != NurbsContinuity::G2) out << "; worst at " << worst->parameter;
    report.description = out.str();
}

std::optional<NurbsSecondSample> second(const NurbsSource& s, double u, double v, bool leftU, bool leftV, std::string* e) {
    return evaluate_nurbs_second(s, u, v, leftU, leftV, e);
}

double normal_curvature(const NurbsSecondSample& s, V direction, V normal, bool* ok) {
    const double E = dot(s.du, s.du), F = dot(s.du, s.dv), G = dot(s.dv, s.dv);
    const double det = E * G - F * F;
    if (!(det > 1e-24 * std::max(E * G, 1e-300))) { *ok = false; return 0; }
    const double r1 = dot(direction, s.du), r2 = dot(direction, s.dv);
    const double a = (G * r1 - F * r2) / det, b = (E * r2 - F * r1) / det;
    const V t = add(mul(s.du, a), mul(s.dv, b));
    const double I = dot(t, t);
    if (!(I > 0)) { *ok = false; return 0; }
    return (a * a * dot(s.duu, normal) + 2 * a * b * dot(s.duv, normal) + b * b * dot(s.dvv, normal)) / I;
}

bool v_edge(unsigned edge) { return edge >= 2; }
bool min_edge(unsigned edge) { return edge == 0 || edge == 2; }
std::optional<NurbsSecondSample> boundary_sample(const NurbsSource& s, unsigned edge, double fraction, std::string* e) {
    const double u0 = s.u.knots.front(), u1 = s.u.knots.back(), v0 = s.v.knots.front(), v1 = s.v.knots.back();
    if (v_edge(edge)) return second(s, std::lerp(u0, u1, fraction), min_edge(edge) ? v0 : v1, false, false, e);
    return second(s, min_edge(edge) ? u0 : u1, std::lerp(v0, v1, fraction), false, false, e);
}
} // namespace

std::optional<NurbsSecondSample> evaluate_nurbs_second(const NurbsSource& s, double u, double v, bool leftU, bool leftV, std::string* e) {
    if (!validate_nurbs(s, e)) return std::nullopt;
    if (!std::isfinite(u) || !std::isfinite(v) || u < s.u.knots[s.u.degree] || u > s.u.knots[s.columns] ||
        (!s.is_curve() && (v < s.v.knots[s.v.degree] || v > s.v.knots[s.rows])))
        return fail<NurbsSecondSample>(e, "Parameter outside NURBS domain");
    const Ders a = basis_ders(s.u, s.columns, u, leftU), b = basis_ders(s.v, s.rows, v, leftV);
    double scale = 0;
    for (const auto& p : s.controls) scale = std::max(scale, p.weight);
    // h[du order][dv order][component]
    double h[3][3][4]{};
    for (unsigned j = 0; j <= s.v.degree; ++j)
        for (unsigned i = 0; i <= s.u.degree; ++i) {
            const auto& p = s.controls[(b.first + j) * s.columns + a.first + i];
            const double w = p.weight / scale;
            const double q[4]{p.x * w, p.y * w, p.z * w, w};
            for (unsigned ku = 0; ku <= 2; ++ku)
                for (unsigned kv = 0; ku + kv <= 2; ++kv) {
                    const double f = a.d[ku][i] * b.d[kv][j];
                    for (unsigned c = 0; c < 4; ++c) h[ku][kv][c] += q[c] * f;
                }
        }
    const double w = h[0][0][3];
    if (!(w > 1e-14) || !std::isfinite(w)) return fail<NurbsSecondSample>(e, "Ill-conditioned NURBS denominator");
    NurbsSecondSample out;
    for (unsigned c = 0; c < 3; ++c) {
        const double C = h[0][0][c] / w;
        const double Cu = (h[1][0][c] - h[1][0][3] * C) / w, Cv = (h[0][1][c] - h[0][1][3] * C) / w;
        out.position[c] = C; out.du[c] = Cu; out.dv[c] = Cv;
        out.duu[c] = (h[2][0][c] - 2 * h[1][0][3] * Cu - h[2][0][3] * C) / w;
        out.duv[c] = (h[1][1][c] - h[1][0][3] * Cv - h[0][1][3] * Cu - h[1][1][3] * C) / w;
        out.dvv[c] = (h[0][2][c] - 2 * h[0][1][3] * Cv - h[0][2][3] * C) / w;
        for (double x : {C, Cu, Cv, out.duu[c], out.duv[c], out.dvv[c]})
            if (!std::isfinite(x)) return fail<NurbsSecondSample>(e, "NURBS evaluation overflow");
    }
    return out;
}

std::optional<NurbsCurvePoint> nurbs_curve_curvature(const NurbsSource& curve, double u, bool fromLeft, std::string* e) {
    if (!curve.is_curve()) return fail<NurbsCurvePoint>(e, "Curvature query needs a NURBS curve");
    auto s = evaluate_nurbs_second(curve, u, 0, fromLeft, false, e);
    if (!s) return std::nullopt;
    const double speed = norm(s->du);
    if (!(speed > 1e-12 * (1 + norm(s->position))))
        return fail<NurbsCurvePoint>(e, "Curve has zero speed here (coincident control points); curvature is undefined");
    NurbsCurvePoint out;
    out.u = u;
    out.position = s->position;
    out.tangent = mul(s->du, 1 / speed);
    out.curvatureVector = mul(sub(s->duu, mul(out.tangent, dot(s->duu, out.tangent))), 1 / (speed * speed));
    out.curvature = norm(out.curvatureVector);
    return out;
}

std::optional<NurbsCurvatureComb> nurbs_curvature_comb(const NurbsSource& curve, unsigned perSpan, std::string* e) {
    if (!validate_nurbs(curve, e)) return std::nullopt;
    if (!curve.is_curve() || perSpan < 1 || perSpan > 256) return fail<NurbsCurvatureComb>(e, "Curvature comb needs a NURBS curve and 1..256 samples per span");
    const auto& k = curve.u.knots;
    const unsigned lo = curve.u.degree, hi = curve.columns;
    NurbsCurvatureComb out;
    auto push = [&](double u, bool left) {
        auto p = nurbs_curve_curvature(curve, u, left, e);
        if (!p) return false;
        out.maximumCurvature = std::max(out.maximumCurvature, p->curvature);
        out.samples.push_back(*p);
        return true;
    };
    for (unsigned i = lo; i < hi; ++i) {
        if (!(k[i + 1] > k[i])) continue;
        if (i > lo && !push(k[i], true)) return std::nullopt;
        for (unsigned j = 0; j < perSpan; ++j)
            if (!push(std::lerp(k[i], k[i + 1], double(j) / perSpan), false)) return std::nullopt;
    }
    if (!push(k[hi], true)) return std::nullopt;
    return out;
}

const char* nurbs_continuity_name(NurbsContinuity c) noexcept {
    switch (c) {
    case NurbsContinuity::None: return "Not connected";
    case NurbsContinuity::G0: return "G0 (touching, with a corner)";
    case NurbsContinuity::G1: return "G1 (tangent, curvature jumps)";
    case NurbsContinuity::G2: return "G2 (curvature continuous)";
    }
    return "?";
}

std::optional<NurbsContinuityReport> nurbs_curve_internal_continuity(const NurbsSource& curve, NurbsContinuityTolerance t, std::string* e) {
    if (!validate_nurbs(curve, e)) return std::nullopt;
    if (!curve.is_curve()) return fail<NurbsContinuityReport>(e, "Internal continuity needs a NURBS curve");
    const double size = geometry_size({&curve});
    NurbsContinuityReport report;
    const auto& k = curve.u.knots;
    for (unsigned i = curve.u.degree + 1; i < curve.columns; ++i) {
        if (k[i] == k[i - 1]) continue;
        auto l = nurbs_curve_curvature(curve, k[i], true, e), r = nurbs_curve_curvature(curve, k[i], false, e);
        if (!l || !r) return std::nullopt;
        NurbsContinuityBreak b;
        b.parameter = k[i];
        b.gap = norm(sub(l->position, r->position));
        b.angleRadians = std::acos(std::clamp(dot(l->tangent, r->tangent), -1.0, 1.0));
        b.curvatureA = l->curvature; b.curvatureB = r->curvature;
        b.curvatureDifference = norm(sub(l->curvatureVector, r->curvatureVector));
        b.level = classify(b.gap, b.angleRadians, b.curvatureA, b.curvatureB, b.curvatureDifference, size, t);
        report.places.push_back(b);
    }
    finish(report, "at the curve's interior knots");
    return report;
}

std::optional<NurbsCurveJoinReport> nurbs_curve_join_continuity(const NurbsSource& a, const NurbsSource& b, NurbsContinuityTolerance t, std::string* e) {
    if (!validate_nurbs(a, e) || !validate_nurbs(b, e)) return std::nullopt;
    if (!a.is_curve() || !b.is_curve()) return fail<NurbsCurveJoinReport>(e, "Curve join needs two NURBS curves");
    std::optional<NurbsCurvePoint> ends[2][2];
    for (unsigned side = 0; side < 2; ++side) {
        ends[0][side] = nurbs_curve_curvature(a, side ? a.u.knots.back() : a.u.knots.front(), side == 1, e);
        ends[1][side] = nurbs_curve_curvature(b, side ? b.u.knots.back() : b.u.knots.front(), side == 1, e);
        if (!ends[0][side] || !ends[1][side]) return std::nullopt;
    }
    NurbsCurveJoinReport report;
    double best = std::numeric_limits<double>::max();
    for (unsigned sa = 0; sa < 2; ++sa)
        for (unsigned sb = 0; sb < 2; ++sb) {
            const double gap = norm(sub(ends[0][sa]->position, ends[1][sb]->position));
            if (gap < best) { best = gap; report.endOfA = sa == 1; report.endOfB = sb == 1; }
        }
    const auto& pa = *ends[0][report.endOfA];
    const auto& pb = *ends[1][report.endOfB];
    // A leaves through its end; B continues from its end inwards.
    const V out = report.endOfA ? pa.tangent : mul(pa.tangent, -1.0);
    const V in = report.endOfB ? mul(pb.tangent, -1.0) : pb.tangent;
    NurbsContinuityBreak place;
    place.parameter = 0;
    place.gap = best;
    place.angleRadians = std::acos(std::clamp(dot(out, in), -1.0, 1.0));
    place.curvatureA = pa.curvature; place.curvatureB = pb.curvature;
    place.curvatureDifference = norm(sub(pa.curvatureVector, pb.curvatureVector));
    place.level = classify(place.gap, place.angleRadians, place.curvatureA, place.curvatureB, place.curvatureDifference, geometry_size({&a, &b}), t);
    report.places.push_back(place);
    finish(report, std::string("where the ") + (report.endOfA ? "end" : "start") + " of the active curve meets the " + (report.endOfB ? "end" : "start") + " of the other");
    return report;
}

std::optional<NurbsSurfaceJoinReport> nurbs_surface_join_continuity(const NurbsSource& a, const NurbsSource& b, unsigned samples, NurbsContinuityTolerance t, std::string* e) {
    if (!validate_nurbs(a, e) || !validate_nurbs(b, e)) return std::nullopt;
    if (a.is_curve() || b.is_curve()) return fail<NurbsSurfaceJoinReport>(e, "Surface join needs two NURBS surfaces");
    if (samples < 2 || samples > 1025) return fail<NurbsSurfaceJoinReport>(e, "Seam sample count must be 2..1025");
    NurbsSurfaceJoinReport report;
    double best = std::numeric_limits<double>::max();
    for (unsigned ea = 0; ea < 4; ++ea)
        for (unsigned eb = 0; eb < 4; ++eb)
            for (unsigned rev = 0; rev < 2; ++rev) {
                double worst = 0;
                for (double f : {0.0, 0.25, 0.5, 0.75, 1.0}) {
                    auto x = boundary_sample(a, ea, f, e), y = boundary_sample(b, eb, rev ? 1 - f : f, e);
                    if (!x || !y) return std::nullopt;
                    worst = std::max(worst, norm(sub(x->position, y->position)));
                }
                if (worst < best) { best = worst; report.boundaryA = ea; report.boundaryB = eb; report.reversed = rev == 1; }
            }
    const double size = geometry_size({&a, &b});
    for (unsigned i = 0; i < samples; ++i) {
        const double f = double(i) / (samples - 1);
        auto x = boundary_sample(a, report.boundaryA, f, e), y = boundary_sample(b, report.boundaryB, report.reversed ? 1 - f : f, e);
        if (!x || !y) return std::nullopt;
        V na = cross(x->du, x->dv), nb = cross(y->du, y->dv);
        const double la = norm(na), lb = norm(nb);
        if (!(la > 0) || !(lb > 0)) return fail<NurbsSurfaceJoinReport>(e, "A surface is degenerate on the measured boundary (no normal)");
        na = mul(na, 1 / la); nb = mul(nb, 1 / lb);
        const V along = v_edge(report.boundaryA) ? x->du : x->dv;
        V across = cross(na, along);
        const double lc = norm(across);
        if (!(lc > 0)) return fail<NurbsSurfaceJoinReport>(e, "A boundary has zero speed (collapsed edge)");
        across = mul(across, 1 / lc);
        bool ok = true;
        NurbsContinuityBreak place;
        place.parameter = f;
        place.gap = norm(sub(x->position, y->position));
        place.angleRadians = std::acos(std::clamp(std::abs(dot(na, nb)), 0.0, 1.0));
        const double ka = normal_curvature(*x, across, na, &ok), kb = normal_curvature(*y, across, na, &ok);
        if (!ok) return fail<NurbsSurfaceJoinReport>(e, "A surface is degenerate on the measured boundary");
        place.curvatureA = std::abs(ka); place.curvatureB = std::abs(kb);
        place.curvatureDifference = std::abs(ka - kb);
        place.level = classify(place.gap, place.angleRadians, place.curvatureA, place.curvatureB, place.curvatureDifference, size, t);
        report.places.push_back(place);
    }
    constexpr const char* names[] = {"U-min", "U-max", "V-min", "V-max"};
    finish(report, std::string("along active ") + names[report.boundaryA] + " / other " + names[report.boundaryB] + (report.reversed ? " (reversed)" : ""));
    return report;
}

} // namespace dve
