// Exact convex collision (dve.collision_convex_set).
//
// Always: the text codec and its limits, baking by voxel scale, exact prism mass
// properties against closed forms, the polygon body descriptors, and GameWorld's
// precedence and rebuilds (reference solver). With Jolt (DVE_HAVE_JOLT): a peg dropped
// onto a plate with a square hole falls into it when the plate is a convex set and
// rests on top when the plate is one hull, at 1 mm and 0.1 mm clearance.

#include "dve/collision_convex_set.hpp"
#include "dve/collision_hull.hpp"
#include "dve/game_world.hpp"
#include "dve/polygon_collision.hpp"
#include "dve/rigid_body_adapter.hpp"
#if defined(DVE_HAVE_JOLT)
#include "dve/physics_jolt_backend.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <optional>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dve;

namespace {

void check(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
bool near(double a, double b, double tol) { return std::abs(a - b) <= tol; }

// Axis-aligned box [x0,x1] x [y0,y1] x [z0,z1] as a prism along y.
ConvexPrism box(float x0, float x1, float y0, float y1, float z0, float z1) {
    ConvexPrism p;
    p.bottom = {{x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}};
    p.top = {{x0, y1, z0}, {x1, y1, z0}, {x1, y1, z1}, {x0, y1, z1}};
    return p;
}

// Square plate of half-size a with a square hole of half-size b, thickness t (y in [0, t]).
std::vector<ConvexPrism> frame(float a, float b, float t) {
    return {box(-a, -b, 0, t, -a, a), box(b, a, 0, t, -a, a), box(-b, b, 0, t, -a, -b), box(-b, b, 0, t, b, a)};
}

CookedPolygonAsset tiny_asset(Float3 lo, Float3 hi) {
    CookedPolygonAsset a;
    a.objectId = 1;
    for (Float3 p : {lo, Float3{hi.x, lo.y, lo.z}, hi}) {
        PolygonVertex v;
        v.position = p;
        v.normal = {0, 1, 0};
        a.vertices.push_back(v);
    }
    a.indices = {0, 1, 2};
    a.submeshes.push_back({"tri", 0, 3, 0});
    a.materials.emplace_back();
    a.materialBindings.resize(1);
    a.bounds = {lo, hi};
    return a;
}

void codec_and_bake() {
    const auto pieces = frame(0.025F, 0.01F, 0.01F);
    const auto text = encode_convex_set(pieces);
    const auto back = decode_convex_set(text);
    check(back && back->size() == 4 && back->at(1).top[2].x == pieces[1].top[2].x, "codec round trip");
    check(!decode_convex_set(text + "1"), "trailing data refused");
    check(!decode_convex_set("DVE_CONVEX_SET 1 0\n"), "empty set refused");
    auto flat = pieces;
    flat[0].top = flat[0].bottom;
    check(!validate_convex_set(flat), "a flat piece is refused");
    Component c;
    c.id = 1;
    c.type = std::string(kConvexSetComponent);
    c.properties["pieces"] = text;
    const auto baked = convex_set_component_pieces(bake_convex_set_component(c, {2, 1, 0.5F}));
    check(baked && near(baked->at(1).top[2].x, 0.05, 1e-7) && near(baked->at(1).top[2].z, 0.0125, 1e-7),
          "bake multiplies by voxel scale");
    std::vector<Component> list{c};
    check(active_convex_set(list).size() == 4, "active set");
    list[0].enabled = false;
    check(active_convex_set(list).empty(), "disabled set is ignored");
}

void mass_properties() {
    // one box 1 x 2 x 3 at an offset: volume 6, center, inertia m/12 (b^2 + c^2)
    const std::vector<ConvexPrism> one{box(1, 2, 10, 12, -1, 2)};
    const auto p = convex_set_mass_properties(one);
    check(p && near(p->volumeCubicMeters, 6, 1e-9), "box volume");
    check(near(p->centerOfMass.x, 1.5, 1e-6) && near(p->centerOfMass.y, 11, 1e-6) && near(p->centerOfMass.z, 0.5, 1e-6),
          "box center");
    const auto& i = p->inertiaPerDensity;
    check(near(i.xx, 6.0 / 12 * (4 + 9), 1e-6) && near(i.yy, 6.0 / 12 * (1 + 9), 1e-6) &&
              near(i.zz, 6.0 / 12 * (1 + 4), 1e-6) && near(i.xy, 0, 1e-6),
          "box inertia");
    // the frame (plate minus hole) equals the full plate minus the hole block
    const auto f = convex_set_mass_properties(frame(0.025F, 0.01F, 0.01F));
    const double full = 0.05 * 0.05 * 0.01, hole = 0.02 * 0.02 * 0.01;
    check(f && near(f->volumeCubicMeters, full - hole, 1e-12), "frame volume");
    const double ixx = full / 12 * (0.01 * 0.01 + 0.05 * 0.05) - hole / 12 * (0.01 * 0.01 + 0.02 * 0.02);
    check(near(f->inertiaPerDensity.xx, ixx, 1e-12) && near(f->centerOfMass.y, 0.005, 1e-7), "frame inertia");
    // reversed ring order gives the same solid
    auto rev = one;
    std::reverse(rev[0].bottom.begin(), rev[0].bottom.end());
    std::reverse(rev[0].top.begin(), rev[0].top.end());
    check(near(convex_set_mass_properties(rev)->volumeCubicMeters, 6, 1e-9), "ring orientation does not matter");
    // a product of inertia: an L of two boxes
    const std::vector<ConvexPrism> el{box(0, 2, 0, 1, 0, 1), box(0, 1, 0, 1, 1, 2)};
    const auto l = convex_set_mass_properties(el);
    // closed form by parallel axis: centers (1, .5, .5) mass 2 and (.5, .5, 1.5) mass 1
    const double gx = (2 * 1 + 1 * 0.5) / 3, gz = (2 * 0.5 + 1 * 1.5) / 3;
    const double ixz = -(2 * (1 - gx) * (0.5 - gz) + 1 * (0.5 - gx) * (1.5 - gz));
    check(l && near(l->inertiaPerDensity.xz, ixz, 1e-9), "product of inertia sign and value");
}

void descriptors() {
    const auto pieces = frame(0.025F, 0.01F, 0.01F);
    const auto asset = tiny_asset({-0.025F, 0, -0.025F}, {0.025F, 0.01F, 0.025F});
    const auto st = make_polygon_static_body_desc(asset, {}, {}, pieces);
    check(st && st->convexPieces.size() == 4 && st->convexHullPoints.empty() && st->boxes.size() == 1,
          "static descriptor carries the pieces");
    PolygonCollisionOptions options;
    options.convexPieces = pieces;
    options.densityKilogramsPerCubicMeter = 2000;
    Float3 com{};
    const auto dyn = make_polygon_dynamic_body_desc(asset, {}, options, &com);
    check(dyn && near(dyn->massKilograms, 2000 * (0.05 * 0.05 * 0.01 - 0.02 * 0.02 * 0.01), 1e-9),
          "mass from the pieces, not the bounds");
    check(near(com.y, 0.005, 1e-7) && near(dyn->transform.position.y, 0.005, 1e-7), "center of mass");
    for (const auto& piece : dyn->convexPieces)
        for (auto q : piece) check(q.y >= -0.0050001F && q.y <= 0.0050001F, "pieces are relative to the center");
}

// Mass from an impulse: dv = J / m.
std::optional<double> mass_of(GameWorld& world, GameObjectId id) {
    if (!world.set_linear_velocity(id, {0, 0, 0}) || !world.apply_impulse(id, {1, 0, 0})) return std::nullopt;
    const auto v = world.linear_velocity(id);
    (void)world.set_linear_velocity(id, {0, 0, 0});
    if (!v || !(v->x > 0)) return std::nullopt;
    return 1.0 / v->x;
}

void game_world_precedence() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    std::string error;
    const auto id = world.spawn_cooked_polygon_asset(tiny_asset({-0.025F, 0, -0.025F}, {0.025F, 0.01F, 0.025F}),
                                                     "Plate", {}, true, true, &error);
    check(id != kInvalidGameObjectId, "spawn: " + error);
    const auto mass0 = mass_of(world, id);
    Component set;
    set.id = 7;
    set.type = std::string(kConvexSetComponent);
    set.properties["pieces"] = encode_convex_set(frame(0.025F, 0.01F, 0.01F));
    check(world.add_component(id, set, &error) != nullptr, "add convex set: " + error);
    const auto mass1 = mass_of(world, id);
    check(mass0 && mass1 && *mass1 < *mass0, "the convex set replaces the bounds box (less mass: the hole)");
    Component again = set;
    again.id = 8;
    check(world.add_component(id, again, &error) == nullptr, "only one convex set per object");
    check(world.set_component_enabled(id, 7, false), "disable");
    const auto mass2 = mass_of(world, id);
    check(mass2 && near(*mass2, *mass0, 1e-6 * *mass0), "disabled set falls back to the bounds box");
    check(world.set_component_enabled(id, 7, true) && world.remove_component(id, 7), "enable and remove");
    Component bad = set;
    bad.id = 9;
    bad.properties["pieces"] = std::string("DVE_CONVEX_SET 1 1\n3\n0 0 0\n");
    check(world.add_component(id, bad, &error) == nullptr, "invalid pieces refused");
}

#if defined(DVE_HAVE_JOLT)
// Drops a square peg (half-width w, length 0.04) onto the plate; returns the peg's
// final bottom height after 2 s.
float drop(bool convexSet, float w, float offsetX, JoltWorldConfig config = {}) {
    JoltRigidBodyWorld world(config);
    world.set_gravity({0.0F, -9.81F, 0.0F});
    StaticRigidBodyCreateDesc floor;
    floor.boxes.push_back({{0, -0.06F, 0}, {0.2F, 0.01F, 0.2F}});
    check(world.create_static_body(floor) != kInvalidRigidBodyHandle, "floor");
    StaticRigidBodyCreateDesc plate;
    plate.boxes.push_back({{0, 0.005F, 0}, {0.025F, 0.005F, 0.025F}});
    const auto pieces = frame(0.025F, 0.01F, 0.01F);
    if (convexSet) {
        for (const auto& p : pieces) plate.convexPieces.push_back(p.points());
    } else {
        for (const auto& p : pieces)
            for (auto q : p.points()) plate.convexHullPoints.push_back(q);
    }
    check(world.create_static_body(plate) != kInvalidRigidBodyHandle, "plate");
    RigidBodyCreateDesc peg;
    peg.transform.position = {offsetX, 0.0102F + 0.02F, 0};
    peg.massKilograms = 0.03;
    const double m = peg.massKilograms, a = w, c = 0.02;
    peg.inertiaKilogramMetersSquared = {m * (c * c + a * a) / 3, m * (2 * a * a) / 3, m * (c * c + a * a) / 3, 0, 0, 0};
    peg.boxes.push_back({{}, {w, 0.02F, w}});
    const auto handle = world.create_body(peg);
    check(handle != kInvalidRigidBodyHandle, "peg");
    for (int i = 0; i < 960; ++i) world.step(1.0F / 480.0F);
    return world.state(handle)->currentTransform.position.y - 0.02F;
}

void jolt_tuning_probe() {
    // which contact setting holds a 0.1 mm peg at the rim?
    for (const auto [spec, slop] : {std::pair{0.02F, 0.02F}, std::pair{0.0002F, 0.02F}, std::pair{0.00005F, 0.02F},
                                    std::pair{0.0005F, 0.0001F}, std::pair{0.0002F, 0.0001F}, std::pair{0.0001F, 0.00005F}}) {
        JoltWorldConfig c;
        c.speculativeContactDistanceMeters = spec;
        c.penetrationSlopMeters = slop;
        const float w = 0.0099F;
        std::printf("probe speculative %.4f slop %.4f: peg bottom %.4f\n", spec, slop, drop(true, w, (0.01F - w) / 3, c));
    }
}

void jolt_drop() {
    JoltWorldConfig precise;                  // the project settings to use for fine fits
    precise.speculativeContactDistanceMeters = 0.0005F;
    precise.penetrationSlopMeters = 0.0001F;
    struct Case { float w; const char* label; bool defaults; };
    for (const Case c : {Case{0.009F, "1 mm, default contacts", true}, Case{0.0099F, "0.1 mm, precise contacts", false},
                         Case{0.00998F, "20 um, precise contacts", false}}) {
        const JoltWorldConfig config = c.defaults ? JoltWorldConfig{} : precise;
        const float offset = (0.01F - c.w) / 3;      // off center by a third of the clearance
        const float inHole = drop(true, c.w, offset, config);
        const float onHull = drop(false, c.w, offset, config);
        std::printf("Jolt, clearance %s: peg bottom ends at %.4f m with the convex set, %.4f m with one hull "
                    "(plate top 0.0100, floor -0.0500)\n", c.label, inHole, onHull);
        check(onHull > 0.009F, std::string("one hull fills the hole: the peg rests on top (") + c.label + ")");
        check(inHole < -0.045F, std::string("the convex set keeps the hole open: the peg drops through (") + c.label + ")");
    }
    // Jolt's game-scale default contact distance (2 cm) catches a 0.1 mm peg on the rim;
    // recorded here, not asserted, because it is Jolt's behavior rather than the shape's
    const float stuck = drop(true, 0.0099F, 0.0001F / 3);
    std::printf("Jolt, clearance 0.1 mm with default contacts: peg bottom ends at %.4f m\n", stuck);
}

#endif

} // namespace

int main() {
    try {
        codec_and_bake();
        mass_properties();
        descriptors();
        game_world_precedence();
#if defined(DVE_HAVE_JOLT)
        if (std::getenv("DVE_JOLT_PROBE")) jolt_tuning_probe();
        jolt_drop();
#else
        std::cout << "SKIP Jolt drop test: built without DVE_ENABLE_JOLT\n";
#endif
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
    std::cout << "convex collision set tests passed\n";
    return 0;
}
