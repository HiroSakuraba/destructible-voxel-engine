#include "dve/collision_convex_set.hpp"

#include "dve/collision_hull.hpp"
#include "dve/voxel_metric.hpp"

#include <array>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

namespace dve {

std::vector<Float3> ConvexPrism::points() const {
    std::vector<Float3> out(bottom);
    out.insert(out.end(), top.begin(), top.end());
    return out;
}

bool validate_convex_set(std::span<const ConvexPrism> pieces) noexcept {
    if (pieces.empty() || pieces.size() > kConvexSetMaxPieces) return false;
    for (const auto& piece : pieces) {
        if (piece.bottom.size() < 3 || piece.bottom.size() > kConvexSetMaxRing ||
            piece.top.size() != piece.bottom.size())
            return false;
        // the hull of the prism must be a real solid (finite, not flat)
        if (!validate_collision_hull_points(piece.points())) return false;
    }
    return true;
}

std::string encode_convex_set(std::span<const ConvexPrism> pieces) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "DVE_CONVEX_SET 1 " << pieces.size() << '\n' << std::setprecision(9);
    for (const auto& piece : pieces) {
        out << piece.bottom.size() << '\n';
        for (const auto* ring : {&piece.bottom, &piece.top})
            for (const auto& p : *ring) out << p.x << ' ' << p.y << ' ' << p.z << '\n';
    }
    return out.str();
}

std::optional<std::vector<ConvexPrism>> decode_convex_set(std::string_view text) {
    if (text.size() > (8U << 20U)) return std::nullopt;
    std::istringstream in{std::string(text)};
    in.imbue(std::locale::classic());
    std::string header;
    unsigned version{};
    std::size_t count{};
    if (!(in >> header >> version >> count) || header != "DVE_CONVEX_SET" || version != 1 || count == 0 ||
        count > kConvexSetMaxPieces)
        return std::nullopt;
    std::vector<ConvexPrism> pieces(count);
    for (auto& piece : pieces) {
        std::size_t n{};
        if (!(in >> n) || n < 3 || n > kConvexSetMaxRing) return std::nullopt;
        piece.bottom.resize(n);
        piece.top.resize(n);
        for (auto* ring : {&piece.bottom, &piece.top})
            for (auto& p : *ring)
                if (!(in >> p.x >> p.y >> p.z)) return std::nullopt;
    }
    in >> std::ws;
    if (!in.eof() || !validate_convex_set(pieces)) return std::nullopt;
    return pieces;
}

std::optional<std::vector<ConvexPrism>> convex_set_component_pieces(const Component& component) {
    if (component.type != kConvexSetComponent) return std::nullopt;
    const auto it = component.properties.find("pieces");
    if (it == component.properties.end()) return std::nullopt;
    const auto* value = std::get_if<std::string>(&it->second);
    return value ? decode_convex_set(*value) : std::nullopt;
}

std::vector<ConvexPrism> active_convex_set(std::span<const Component> components) {
    for (const auto& c : components)
        if (c.enabled && c.type == kConvexSetComponent) {
            auto pieces = convex_set_component_pieces(c);
            return pieces ? std::move(*pieces) : std::vector<ConvexPrism>{};
        }
    return {};
}

Component bake_convex_set_component(Component component, Float3 metric) {
    if (auto pieces = convex_set_component_pieces(component)) {
        for (auto& piece : *pieces)
            for (auto* ring : {&piece.bottom, &piece.top})
                for (auto& p : *ring) p = multiply_components(p, metric);
        component.properties["pieces"] = encode_convex_set(*pieces);
    }
    return component;
}

namespace {
struct D3 { double x, y, z; };
D3 d3(Float3 p, Float3 o) { return {double(p.x) - o.x, double(p.y) - o.y, double(p.z) - o.z}; }
double det3(D3 a, D3 b, D3 c) {
    return a.x * (b.y * c.z - b.z * c.y) - a.y * (b.x * c.z - b.z * c.x) + a.z * (b.x * c.y - b.y * c.x);
}
} // namespace

std::optional<ConvexSetMassProperties> convex_set_mass_properties(std::span<const ConvexPrism> pieces) noexcept {
    if (pieces.empty()) return std::nullopt;
    const Float3 origin = pieces.front().bottom.front();     // reference point: keeps sums well scaled
    double volume = 0, mx = 0, my = 0, mz = 0;
    double cxx = 0, cyy = 0, czz = 0, cxy = 0, cxz = 0, cyz = 0;
    for (const auto& piece : pieces) {
        const std::size_t n = piece.bottom.size();
        if (n < 3 || piece.top.size() != n) return std::nullopt;
        // closed surface: two caps (fans) and the side quads, any consistent orientation
        std::vector<std::array<D3, 3>> tris;
        for (std::size_t i = 1; i + 1 < n; ++i) {
            tris.push_back({d3(piece.bottom[0], origin), d3(piece.bottom[i + 1], origin), d3(piece.bottom[i], origin)});
            tris.push_back({d3(piece.top[0], origin), d3(piece.top[i], origin), d3(piece.top[i + 1], origin)});
        }
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t j = (i + 1) % n;
            const D3 b0 = d3(piece.bottom[i], origin), b1 = d3(piece.bottom[j], origin);
            const D3 t0 = d3(piece.top[i], origin), t1 = d3(piece.top[j], origin);
            tris.push_back({b0, b1, t1});
            tris.push_back({b0, t1, t0});
        }
        double v = 0, x = 0, y = 0, z = 0, xx = 0, yy = 0, zz = 0, xy = 0, xz = 0, yz = 0;
        for (const auto& [a, b, c] : tris) {
            const double det = det3(a, b, c);
            v += det / 6.0;
            const D3 s{a.x + b.x + c.x, a.y + b.y + c.y, a.z + b.z + c.z};
            x += det / 24.0 * s.x;
            y += det / 24.0 * s.y;
            z += det / 24.0 * s.z;
            const double k = det / 120.0;
            xx += k * (a.x * a.x + b.x * b.x + c.x * c.x + s.x * s.x);
            yy += k * (a.y * a.y + b.y * b.y + c.y * c.y + s.y * s.y);
            zz += k * (a.z * a.z + b.z * b.z + c.z * c.z + s.z * s.z);
            xy += k * (a.x * a.y + b.x * b.y + c.x * c.y + s.x * s.y);
            xz += k * (a.x * a.z + b.x * b.z + c.x * c.z + s.x * s.z);
            yz += k * (a.y * a.z + b.y * b.z + c.y * c.z + s.y * s.z);
        }
        const double sign = v < 0 ? -1.0 : 1.0;          // either ring order gives the same solid
        volume += sign * v;
        mx += sign * x; my += sign * y; mz += sign * z;
        cxx += sign * xx; cyy += sign * yy; czz += sign * zz;
        cxy += sign * xy; cxz += sign * xz; cyz += sign * yz;
    }
    if (!(volume > 0) || !std::isfinite(volume)) return std::nullopt;
    const double gx = mx / volume, gy = my / volume, gz = mz / volume;
    // second moments about the center of mass
    cxx -= volume * gx * gx; cyy -= volume * gy * gy; czz -= volume * gz * gz;
    cxy -= volume * gx * gy; cxz -= volume * gx * gz; cyz -= volume * gy * gz;
    ConvexSetMassProperties out;
    out.volumeCubicMeters = volume;
    out.centerOfMass = {float(origin.x + gx), float(origin.y + gy), float(origin.z + gz)};
    out.inertiaPerDensity = {cyy + czz, cxx + czz, cxx + cyy, -cxy, -cxz, -cyz};
    return out;
}

} // namespace dve
