// Curvature, G0/G1/G2 continuity, G2 boundary matching and seams, the G2 corner blend, and the
// smooth (C2) camera dolly.
#include "dve/camera_sequence.hpp"
#include "dve/editor_camera_sequencer.hpp"
#include "dve/editor_native.hpp"
#include "dve/nurbs_attachment.hpp"
#include "dve/nurbs_curvature.hpp"
#include "dve/nurbs_modeling.hpp"
#include "dve/nurbs_topology.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>

using namespace dve;
using namespace dve::editor;

namespace {
void check(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
bool near(double a, double b, double tolerance) { return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= tolerance; }
using V = std::array<double,3>;
double dist(V a, V b) { return std::hypot(a[0]-b[0], a[1]-b[1], a[2]-b[2]); }

NurbsSource polyline(std::initializer_list<V> points) {
    NurbsSource s; s.columns = unsigned(points.size()); s.u.degree = 1;
    s.u.knots.assign(2, 0.0);
    for (unsigned i = 1; i + 1 < s.columns; ++i) s.u.knots.push_back(double(i) / (s.columns - 1));
    s.u.knots.insert(s.u.knots.end(), 2, 1.0);
    for (auto p : points) s.controls.push_back({p[0], p[1], p[2], 1});
    return s;
}
NurbsSource cubic(std::vector<double> knots, std::initializer_list<V> points) {
    NurbsSource s; s.columns = unsigned(points.size()); s.u.degree = 3; s.u.knots = std::move(knots);
    for (auto p : points) s.controls.push_back({p[0], p[1], p[2], 1});
    return s;
}

void curvature_queries() {
    std::string error;
    // A rational quadratic quarter circle of radius 2 has curvature 1/2 everywhere.
    const auto arc = make_nurbs_arc(2, 1.5707963267948966);
    for (double u : {0.0, 0.2, 0.5, 0.9, 1.0}) {
        auto p = nurbs_curve_curvature(arc, std::lerp(arc.u.knots.front(), arc.u.knots.back(), u), false, &error);
        check(p && near(p->curvature, 0.5, 1e-9), "circle curvature is 1/r: " + error);
        // The curvature vector points at the centre (the origin).
        const V toCentre{-p->position[0] / 2, -p->position[1] / 2, -p->position[2] / 2};
        check(dist(p->curvatureVector, {toCentre[0] / 2, toCentre[1] / 2, toCentre[2] / 2}) < 1e-9, "curvature vector points at the centre");
    }
    auto comb = nurbs_curvature_comb(arc, 8, &error);
    check(comb && near(comb->maximumCurvature, 0.5, 1e-9) && comb->samples.size() >= 9, "circle comb");
    // A straight segment has none.
    auto line = nurbs_curve_curvature(polyline({{0,0,0},{3,4,0}}), 0.3);
    check(line && line->curvature == 0, "straight curvature");
    // Bezier start curvature: (p-1)/p * |(P1-P0) x (P2-P1)| / |P1-P0|^3 = 2/3 here.
    const auto bezier = cubic({0,0,0,0,1,1,1,1}, {{0,0,0},{1,0,0},{2,1,0},{3,1,0}});
    auto start = nurbs_curve_curvature(bezier, 0);
    check(start && near(start->curvature, 2.0 / 3.0, 1e-12), "Bezier end curvature formula");
    // Second derivatives agree with central differences of first derivatives (rational surface).
    auto patch = make_nurbs_patch(2);
    for (auto& p : patch.controls) p.y = .3 * p.x * p.x - .2 * p.x * p.z + .1 * p.z * p.z;
    patch.controls[5].weight = .7; patch.controls[10].weight = 1.4;
    const double u = .37, v = .61, h = 1e-5;
    auto s = evaluate_nurbs_second(patch, u, v, false, false, &error);
    auto up = evaluate_nurbs(patch, u + h, v), um = evaluate_nurbs(patch, u - h, v), vp = evaluate_nurbs(patch, u, v + h), vm = evaluate_nurbs(patch, u, v - h);
    check(s && up && um && vp && vm, "second-derivative fixture");
    for (unsigned k = 0; k < 3; ++k) {
        check(near(s->duu[k], (up->du[k] - um->du[k]) / (2 * h), 1e-5), "duu matches differences");
        check(near(s->dvv[k], (vp->dv[k] - vm->dv[k]) / (2 * h), 1e-5), "dvv matches differences");
        check(near(s->duv[k], (vp->du[k] - vm->du[k]) / (2 * h), 1e-5), "duv matches differences");
    }
    check(!nurbs_curve_curvature(patch, .5), "surface refuses a curve query");
}

void internal_continuity() {
    std::string error;
    // A cubic with a double interior knot is only C1: the tangent carries on, curvature jumps.
    auto c1 = cubic({0,0,0,0,.5,.5,1,1,1,1}, {{0,0,0},{1,0,0},{2,1,0},{3,2,0},{4,2.5,0},{5,2,0}});
    auto report = nurbs_curve_internal_continuity(c1, {}, &error);
    check(report && report->level == NurbsContinuity::G1 && report->places.size() == 1, "double knot is G1: " + (report ? report->description : error));
    // Simple knots in a cubic are C2.
    auto smooth = cubic({0,0,0,0,.3,.6,1,1,1,1}, {{0,0,0},{1,2,0},{2,-1,1},{3,2,0},{4,0,1},{5,1,0}});
    report = nurbs_curve_internal_continuity(smooth, {}, &error);
    check(report && report->level == NurbsContinuity::G2 && report->places.size() == 2, "simple knots are G2");
    // A degree-one corner is only G0.
    report = nurbs_curve_internal_continuity(polyline({{0,0,0},{1,0,0},{1,1,0}}), {}, &error);
    check(report && report->level == NurbsContinuity::G0, "polyline corner is G0");
    check(report->description.find("G0") != std::string::npos, "description names the level");
}

void corner_blend_and_joins() {
    std::string error;
    const auto corner = polyline({{4,0,0},{0,0,0},{0,4,0}});
    // Setback mode: starts 1 along each leg; X, B, B, Y.
    auto blend = blend_nurbs_corner(corner, NurbsCornerBlendSize::Setback, 1, &error);
    check(blend && blend->source.columns == 4 && blend->setback == 1, "setback blend: " + error);
    check(blend->source.controls[1] == blend->source.controls[2] && blend->source.controls[1].x == 0 && blend->source.controls[1].y == 0, "inner controls on the corner");
    for (double u : {0.0, 1.0}) {
        auto p = nurbs_curve_curvature(blend->source, u, u == 1.0);
        check(p && p->curvature < 1e-12, "zero curvature where the blend meets each leg");
    }
    auto middle = evaluate_nurbs(blend->source, .5);
    check(middle && near(middle->position[0], .125, 1e-12) && near(middle->position[1], .125, 1e-12), "midpoint (X+Y)/8 + 3B/4");
    check(blend->peakCurvature > 0 && near(blend->peakCurvature, nurbs_curvature_comb(blend->source, 64)->maximumCurvature, 1e-6 * blend->peakCurvature), "peak curvature reported");

    // Radius mode: as round as the circular fillet of that radius (same midpoint), reaching further.
    const double r = .5;
    auto matched = blend_nurbs_corner(corner, NurbsCornerBlendSize::MatchRadius, r, &error);
    auto arc = fillet_nurbs_corner(corner, r, &error);
    check(matched && arc, "radius blend and fillet: " + error);
    auto bm = evaluate_nurbs(matched->source, .5), am = evaluate_nurbs(*arc, std::midpoint(arc->u.knots.front(), arc->u.knots.back()));
    check(near(std::hypot(bm->position[0], bm->position[1]), std::hypot(am->position[0], am->position[1]), 1e-12), "same distance from the corner as the fillet");
    check(near(matched->setback, r * 4 / (1 + std::sin(.7853981633974483)), 1e-12), "reaches 4/(1+sin(theta/2)) times as far");
    check(near(matched->arcCurvature, 1 / r, 1e-12), "arc curvature reported");
    check(!blend_nurbs_corner(corner, NurbsCornerBlendSize::MatchRadius, 3, &error) && error.find("past the end of a leg") != std::string::npos, "too large refuses");
    check(!blend_nurbs_corner(polyline({{0,0,0},{1,0,0},{2,0,0}}), NurbsCornerBlendSize::Setback, .5), "straight corner refuses");
    check(!blend_nurbs_corner(corner, NurbsCornerBlendSize::Setback, -1), "negative size refuses");

    // Joins: the leg into the circular fillet is G1 (0 to 1/r); into the blend it is G2.
    const double d = r / std::tan(.7853981633974483);
    const auto legToArc = polyline({{4,0,0},{d,0,0}});
    auto join = nurbs_curve_join_continuity(legToArc, *arc, {}, &error);
    check(join && join->level == NurbsContinuity::G1 && join->endOfA && !join->endOfB, "leg into fillet is G1: " + (join ? join->description : error));
    check(near(join->maximumCurvatureDifference, 1 / r, 1e-9), "jump is 1/r");
    const auto legToBlend = polyline({{4,0,0},{matched->setback,0,0}});
    join = nurbs_curve_join_continuity(legToBlend, matched->source, {}, &error);
    check(join && join->level == NurbsContinuity::G2, "leg into blend is G2: " + (join ? join->description : error));
    // Either order and either direction finds the same join.
    auto reversed = reverse_nurbs_axis(matched->source, false);
    join = nurbs_curve_join_continuity(*reversed, legToBlend, {}, &error);
    check(join && join->level == NurbsContinuity::G2 && join->endOfA && join->endOfB, "reversed join still G2");
    // A real corner and a gap.
    join = nurbs_curve_join_continuity(polyline({{0,0,0},{1,0,0}}), polyline({{1,0,0},{1,1,0}}));
    check(join && join->level == NurbsContinuity::G0, "corner join is G0");
    join = nurbs_curve_join_continuity(polyline({{0,0,0},{1,0,0}}), polyline({{1.5,0,0},{2,0,0}}));
    check(join && join->level == NurbsContinuity::None, "gap is not connected");
}

void bridges() {
    std::string error;
    // A C-bend: a quarter arc (curvature 1/2) bridged to a line.
    const auto arc = make_nurbs_arc(2, 1.5707963267948966);          // XZ plane: (2,0,0) to (0,0,2)
    const auto line = polyline({{-2, 0, 1}, {-5, 0, -1}});
    auto check_bridge = [&](const NurbsSource& a, const NurbsSource& b, const std::string& name) {
        auto bridge = bridge_nurbs_curves_g2(a, b, &error);
        check(bool(bridge), name + ": " + error);
        check(bridge->source.columns == 4 && bridge->solutions >= 1 && bridge->handleA > 0 && bridge->handleB > 0, name + " shape");
        auto ja = nurbs_curve_join_continuity(a, bridge->source, {}, &error), jb = nurbs_curve_join_continuity(b, bridge->source, {}, &error);
        check(ja && ja->level == NurbsContinuity::G2, name + " meets A in G2: " + (ja ? ja->description : error));
        check(jb && jb->level == NurbsContinuity::G2, name + " meets B in G2: " + (jb ? jb->description : error));
        return *bridge;
    };
    auto first = check_bridge(arc, line, "arc to line");
    check(first.endOfA && !first.endOfB, "nearest ends chosen");
    // Either order and either direction of B.
    (void)check_bridge(line, arc, "line to arc");
    (void)check_bridge(arc, *reverse_nurbs_axis(line, false), "arc to reversed line");
    // An S-bend: curvature changes sign across the bridge.
    const auto down = cubic({0,0,0,0,1,1,1,1}, {{0,0,0},{1,0,0},{2,-.5,0},{3,-.5,0}});
    const auto up = cubic({0,0,0,0,1,1,1,1}, {{5,1.5,0},{6,1.5,0},{7,1.1,0},{8,1.2,0}});
    (void)check_bridge(down, up, "S-bend");
    // Straight ends at an angle: zero curvature at both ends of the bridge.
    auto corner = check_bridge(polyline({{0,0,0},{2,0,0}}), polyline({{4,2,0},{4,5,0}}), "straight ends");
    for (double u : {0.0, 1.0}) check(nurbs_curve_curvature(corner.source, u, u == 1.0)->curvature < 1e-9, "straight ends leave the bridge straight");
    // Collinear straight ends: the straight segment.
    (void)check_bridge(polyline({{0,0,0},{1,0,0}}), polyline({{3,0,0},{4,0,0}}), "collinear");
    // Out of plane, a sideways offset between parallel straight ends, and touching ends refuse.
    auto lifted = line; for (auto& p : lifted.controls) p.y = 1;
    check(!bridge_nurbs_curves_g2(arc, lifted, &error) && error.find("one plane") != std::string::npos, "non-planar refuses");
    check(!bridge_nurbs_curves_g2(polyline({{0,0,0},{1,0,0}}), polyline({{3,1,0},{4,1,0}}), &error) && error.find("No cubic bridge") != std::string::npos, "parallel offset refuses: " + error);
    check(!bridge_nurbs_curves_g2(polyline({{0,0,0},{1,0,0}}), polyline({{1,0,0},{1,1,0}}), &error) && error.find("already touch") != std::string::npos, "touching refuses");
    // With the fallback allowed, impossible cases give a tangent-only (G1) bridge and say why.
    for (const auto& [a, b, why] : {std::tuple{polyline({{0,0,0},{1,0,0}}), polyline({{3,1,0},{4,1,0}}), "No cubic bridge"},
                                    std::tuple{arc, lifted, "one plane"}}) {
        auto bridge = bridge_nurbs_curves_g2(a, b, &error, true);
        check(bridge && bridge->tangentOnly && bridge->fallbackReason.find(why) != std::string::npos, std::string("fallback: ") + why);
        auto ja = nurbs_curve_join_continuity(a, bridge->source), jb = nurbs_curve_join_continuity(b, bridge->source);
        check(ja && jb && ja->level >= NurbsContinuity::G1 && jb->level >= NurbsContinuity::G1, std::string("fallback keeps tangents: ") + why);
        const double chord = std::hypot(bridge->source.controls[3].x - bridge->source.controls[0].x, bridge->source.controls[3].y - bridge->source.controls[0].y, bridge->source.controls[3].z - bridge->source.controls[0].z);
        check(near(bridge->handleA, chord / 3, 1e-12) && near(bridge->handleB, chord / 3, 1e-12), "fallback handles are a third of the chord");
    }
    check(!bridge_nurbs_curves_g2(polyline({{0,0,0},{1,0,0}}), polyline({{1,0,0},{1,1,0}}), &error, true), "touching still refuses with the fallback");
    // A possible G2 bridge is unchanged by allowing the fallback.
    auto same = bridge_nurbs_curves_g2(arc, line, &error, true);
    check(same && !same->tangentOnly && same->source == first.source, "fallback only when needed");
}

NurbsSource reference_patch() {
    auto s = make_nurbs_patch(2);
    // Bent across the boundary (in x) and along it (in z), and rational.
    for (auto& p : s.controls) p.y = .25 * p.x * p.x + .1 * p.z * p.z;
    for (unsigned j = 0; j < 4; ++j) s.controls[j * 4 + 2].weight = .9;
    return s;
}
NurbsSource moving_patch() {
    auto s = make_nurbs_patch(2);
    for (auto& p : s.controls) { p.y = .1 * p.z * p.z - .2 * (p.x + 1) * (p.x + 1); p.x += 2; }
    return s;
}
NurbsSource oriented(NurbsSource source, NurbsBoundary edge, bool reference, bool reverse) {
    const bool v = edge == NurbsBoundary::VMin || edge == NurbsBoundary::VMax, minimum = edge == NurbsBoundary::UMin || edge == NurbsBoundary::VMin;
    if (reverse) source = *reverse_nurbs_axis(source, true);
    if (minimum == reference) source = *reverse_nurbs_axis(source, false);
    if (v) source = *transpose_nurbs(source);
    return source;
}

void surface_g2() {
    std::string error;
    const auto ref = reference_patch(), mov = moving_patch();
    auto c1 = attach_nurbs_u_continuity(ref, mov, 1, &error), c2 = attach_nurbs_u_continuity(ref, mov, 2, &error);
    check(c1 && c2, "C1 and C2 attach: " + error);
    for (unsigned i = 0; i <= 8; ++i) {
        const double v = double(i) / 8;
        auto a = evaluate_nurbs_second(ref, 1, v), b = evaluate_nurbs_second(*c2, 0, v);
        for (unsigned k = 0; k < 3; ++k) {
            check(near(a->position[k], b->position[k], 1e-12) && near(a->du[k], b->du[k], 1e-10), "C2 keeps C1");
            check(near(a->duu[k], b->duu[k], 1e-8 * (1 + std::abs(a->duu[k]))), "C2 matches second derivatives");
        }
    }
    // The far edge is untouched.
    for (unsigned j = 0; j < 4; ++j) check(c2->controls[j * 4 + 3] == mov.controls[j * 4 + 3], "C2 keeps the far edge");
    auto g1 = nurbs_surface_join_continuity(ref, *c1, 9, {}, &error), g2 = nurbs_surface_join_continuity(ref, *c2, 9, {}, &error);
    check(g1 && g1->level == NurbsContinuity::G1, "C1 attach reads as G1: " + (g1 ? g1->description : error));
    check(g2 && g2->level == NurbsContinuity::G2, "C2 attach reads as G2: " + (g2 ? g2->description : error));
    check(g2->boundaryA == unsigned(NurbsBoundary::UMax) && g2->boundaryB == unsigned(NurbsBoundary::UMin) && !g2->reversed, "the matched boundaries are found");
    // Refusals for too few controls / linear patches at this level.
    auto linear = mov; linear.u.degree = 1; linear.columns = 2; linear.u.knots = {0,0,1,1};
    linear.controls.clear();
    for (unsigned j = 0; j < 4; ++j) { linear.controls.push_back(mov.controls[j * 4]); linear.controls.push_back(mov.controls[j * 4 + 3]); }
    check(!attach_nurbs_u_continuity(ref, linear, 2, &error) && error.find("C2 attachment needs") != std::string::npos, "low-level C2 refuses a linear patch");
    // The boundary attachment elevates instead, in every orientation.
    NurbsAttachmentOptions options; options.c2 = true;
    auto attached = attach_nurbs_boundaries(ref, linear, options, &error);
    check(attached && attached->moving.columns >= 4 && attached->moving.u.degree >= 2, "boundary attach elevates for C2: " + error);
    auto report = nurbs_surface_join_continuity(attached->reference, attached->moving, 9, {}, &error);
    check(report && report->level == NurbsContinuity::G2, "elevated C2 attach is G2");
    for (unsigned a = 0; a < 4; ++a)
        for (unsigned b = 0; b < 4; ++b)
            for (bool reverse : {false, true}) {
                const auto edgeA = static_cast<NurbsBoundary>(a), edgeB = static_cast<NurbsBoundary>(b);
                NurbsAttachmentOptions o{edgeA, edgeB, reverse, false};
                o.c2 = true;
                auto result = attach_nurbs_boundaries(oriented(ref, edgeA, true, false), oriented(mov, edgeB, false, reverse), o, &error);
                check(bool(result), "orientation " + std::to_string(a) + "/" + std::to_string(b) + ": " + error);
                auto r = nurbs_surface_join_continuity(result->reference, result->moving, 9, {}, &error);
                check(r && r->level == NurbsContinuity::G2, "G2 in orientation " + std::to_string(a) + "/" + std::to_string(b) + (reverse ? " reversed: " : ": ") + (r ? r->description : error));
            }
}

void add(EditorDocument& document, EditorObjectId id, const NurbsSource& source) {
    std::string error;
    auto object = make_editor_nurbs(id, source, &error);
    check(bool(object), error);
    document.add_object(std::move(*object));
}

void seam_g2() {
    EditorDocument document("G2 seam");
    add(document, 1, reference_patch());
    add(document, 2, moving_patch());
    NurbsAttachmentOptions options; options.c1 = true; options.c2 = true;
    auto result = link_nurbs_seam_command(1, 2, options)->execute(document);
    check(result.success, "G2 seam link: " + result.message);
    std::string error;
    check(validate_editor_nurbs_seams(document, &error), "G2 seam validates: " + error);
    auto seam = editor_nurbs_seam(*document.find_object(2), &error);
    check(seam && seam->options.c1 && seam->options.c2, "G2 seam round-trips through the saved property");
    check(nurbs_seam_controls_driven(*document.find_object(2), {2}) && !nurbs_seam_controls_driven(*document.find_object(2), {3}), "third column is driven, the far edge is not");
    auto a = world_editor_nurbs_source(document, *document.find_object(1)), b = world_editor_nurbs_source(document, *document.find_object(2));
    auto report = nurbs_surface_join_continuity(*a, *b, 17, {}, &error);
    check(report && report->level == NurbsContinuity::G2, "linked seam is G2: " + (report ? report->description : error));
    // Rebuilding keeps G2.
    result = refresh_nurbs_seam_command(1)->execute(document);
    check(result.success && validate_editor_nurbs_seams(document, &error), "G2 seam rebuild: " + result.message + error);
}

void editor_actions() {
    NativeEditorController c(EditorWorkspace(EditorDocument("Curvature")));
    c.resize(1600, 1000);
    auto& document = c.workspace().document();
    add(document, 1, reference_patch());
    add(document, 2, moving_patch());
    add(document, 3, polyline({{4,0,0},{0,0,0},{0,4,0}}));
    // G2 attach from the menu, then the report on the pair.
    c.workspace().select_object(2);
    c.workspace().add_to_selection(1);
    if (c.workspace().selected_object() != EditorObjectId{1}) { c.workspace().toggle_selection(1); c.workspace().add_to_selection(1); }
    check(c.workspace().selected_object() == EditorObjectId{1}, "reference active");
    const auto before = document.objects().size();
    check(c.dispatch_action("nurbs.source_attach_g2"), "attach G2: " + c.status().text);
    check(document.objects().size() == before + 1 && c.status().text.find("G2") != std::string::npos, "G2 patch added");
    const auto made = *c.workspace().selected_object();
    c.workspace().select_object(made);
    c.workspace().add_to_selection(1);
    if (c.workspace().selected_object() != EditorObjectId{1}) { c.workspace().toggle_selection(1); c.workspace().add_to_selection(1); }
    check(c.dispatch_action("nurbs.source_continuity"), "report: " + c.status().text);
    check(c.status().text.starts_with("G2"), "pair reports G2: " + c.status().text);
    c.workspace().select_object(2);
    c.workspace().add_to_selection(1);
    check(c.dispatch_action("nurbs.source_continuity") && c.status().text.starts_with("Not connected"), "unmatched originals are apart: " + c.status().text);
    // The default corner leg fraction (0.3) fits "as round as the fillet": 1.2 x 4/(1+sin 45).
    c.workspace().select_object(3);
    check(c.dispatch_action("nurbs.source_blend_corner_radius") && c.status().text.find("setback 2.812") != std::string::npos, "default fraction fits: " + c.status().text);
    check(c.dispatch_action("edit.undo"), "undo default blend");
    // Corner leg fraction 0.25 (its own settings field): fillet setback 1 on these 4-unit legs.
    c.workspace().select_object(3);
    check(c.dispatch_action("nurbs.construct_settings"), "settings");
    const auto view = c.layout().viewport;
    c.pointer_down(PointerButton::Primary, view.x + 25, view.y + 8 + 125 + 5 * 32 + 12, 0);
    c.text_input("0.25");
    c.key_down("enter", false, false, false);
    // Both corner blends from the menu.
    for (const char* action : {"nurbs.source_blend_corner_setback", "nurbs.source_blend_corner_radius"}) {
        c.workspace().select_object(3);
        const auto count = document.objects().size();
        check(c.dispatch_action(action), std::string(action) + ": " + c.status().text);
        check(document.objects().size() == count + 1, "blend added");
        auto source = editor_nurbs_source(*document.find_object(*c.workspace().selected_object()));
        check(source && source->columns == 4 && source->u.degree == 3, "blend is a cubic");
        check(c.status().text.find("peak curvature") != std::string::npos, "blend status reports curvature");
        check(c.dispatch_action("nurbs.source_continuity") && c.status().text.find("no interior knots") != std::string::npos, "one-piece blend report");
        check(c.dispatch_action("edit.undo") && document.objects().size() == count, "blend undo");
    }
    // Setback mode starts where the fillet starts; radius mode reaches 4/(1+sin 45) times as far.
    c.workspace().select_object(3);
    check(c.dispatch_action("nurbs.source_blend_corner_setback") && c.status().text.find("setback 1 ") != std::string::npos, "setback mode: " + c.status().text);
    check(c.dispatch_action("edit.undo"), "undo");
    c.workspace().select_object(3);
    check(c.dispatch_action("nurbs.source_blend_corner_radius") && c.status().text.find("setback 2.343") != std::string::npos, "radius mode: " + c.status().text);
    // Bridge two curves from the menu: identity-pose object, both ends G2, one undo step.
    add(document, 10, make_nurbs_arc(2, 1.5707963267948966));
    add(document, 11, polyline({{-2, 0, 1}, {-5, 0, -1}}));
    document.find_object(10)->transform.position = {1, 0, 0};
    document.find_object(11)->transform.position = {1, 0, 0};
    c.workspace().select_object(10);
    c.workspace().add_to_selection(11);
    const auto beforeBridge = document.objects().size();
    check(c.dispatch_action("nurbs.bridge_g2"), "bridge: " + c.status().text);
    check(document.objects().size() == beforeBridge + 1 && c.status().text.find("G2 (curvature continuous) / G2") != std::string::npos, "bridge status: " + c.status().text);
    auto bridgeSource = editor_nurbs_source(*document.find_object(*c.workspace().selected_object()));
    check(bridgeSource && std::any_of(bridgeSource->controls.begin(), bridgeSource->controls.end(), [](const NurbsPoint& p) { return near(p.x, 1, 1e-9) && near(p.y, 0, 1e-9) && near(p.z, 2, 1e-9); }), "bridge built in world space (arc end moved by the pose)");
    check(c.dispatch_action("edit.undo") && document.objects().size() == beforeBridge, "bridge undo");
    // Parallel straight ends offset sideways: a G1 bridge with a warning.
    add(document, 12, polyline({{0,0,10},{1,0,10}}));
    add(document, 13, polyline({{3,1,10},{4,1,10}}));
    c.workspace().select_object(12);
    c.workspace().add_to_selection(13);
    check(c.dispatch_action("nurbs.bridge_g2") && c.status().text.starts_with("G1 bridge added (tangent only") &&
          document.find_object(*c.workspace().selected_object())->name == "NURBS G1 Bridge", "G1 fallback in the editor: " + c.status().text);
    // The comb toggle.
    check(c.dispatch_action("nurbs.show_curvature") && c.dispatch_action("nurbs.show_curvature"), "comb toggles");
    // Mixed selections refuse.
    c.workspace().select_object(3);
    c.workspace().add_to_selection(1);
    check(!c.dispatch_action("nurbs.source_continuity"), "curve with surface refuses");
}

void dolly() {
    using namespace dve::camera;
    CameraDollySpline d;
    d.id = 1; d.name = "Path";
    const float times[]{0, 1, 3, 4};
    const Float3 points[]{{0,0,0},{1,2,0},{4,2,1},{5,0,0}};
    for (int i = 0; i < 4; ++i) {
        CameraDollyKey k; k.timeSeconds = times[i]; k.pose.position = points[i];
        k.pose.target = {points[i].x, 0, points[i].z + 5};
        d.keys.push_back(k);
    }
    std::string error;
    check(d.validate(&error), error);
    auto position = [&](float t) { return d.evaluate(t).pose.position; };
    auto jumps = [&](float key, float h) {
        const Float3 l2 = position(key - 2 * h), l1 = position(key - h), c = position(key), r1 = position(key + h), r2 = position(key + 2 * h);
        const Float3 vl = multiply(subtract(c, l1), 1 / h), vr = multiply(subtract(r1, c), 1 / h);
        const Float3 al = multiply(add(subtract(c, multiply(l1, 2)), l2), 1 / (h * h)), ar = multiply(add(subtract(r2, multiply(r1, 2)), c), 1 / (h * h));
        return std::pair{length(subtract(vl, vr)), length(subtract(al, ar))};
    };
    for (auto interpolation : {CameraDollyInterpolation::CatmullRom, CameraDollyInterpolation::Smooth}) {
        d.interpolation = interpolation;
        for (int i = 0; i < 4; ++i) check(length(subtract(position(times[i]), points[i])) < 1e-5F, "passes through every key");
    }
    d.interpolation = CameraDollyInterpolation::CatmullRom;
    auto [cv, ca] = jumps(1, .01F);
    check(cv > .5F && ca > 1, "Catmull-Rom jumps in speed and acceleration at an unevenly timed key");
    d.interpolation = CameraDollyInterpolation::Smooth;
    auto [sv, sa] = jumps(1, .01F);
    check(sv < .05F && sa < .2F, "smooth is continuous in velocity and acceleration: " + std::to_string(sv) + " " + std::to_string(sa));
    std::tie(sv, sa) = jumps(3, .01F);
    check(sv < .05F && sa < .2F, "smooth at the second interior key");
    // Natural ends: zero acceleration at the start.
    const Float3 a0 = multiply(add(subtract(position(.02F), multiply(position(.01F), 2)), position(0)), 1 / (.01F * .01F));
    check(length(a0) < .5F, "natural end");
    // Constant-speed sampling and length use the same smooth path.
    check(length(subtract(d.evaluate_progress(1, true).pose.position, points[3])) < 1e-4F && d.approximate_length() > 7, "progress and length");

    // Saved as v4; v3 files still read (Catmull-Rom); unknown values refuse.
    CameraSequence sequence; sequence.name = "Smooth"; sequence.durationSeconds = 4; sequence.dollies.push_back(d);
    CameraShot shot; shot.id = 2; shot.name = "Move"; shot.durationSeconds = 4; shot.dollyId = 1; sequence.shots.push_back(shot);
    check(sequence.validate(&error), error);
    const auto text = sequence.serialize();
    check(text.starts_with("DVE_CAMERA_SEQUENCE 4\n") && text.find(" smooth\n") != std::string::npos, "v4 writes the interpolation");
    auto parsed = CameraSequence::parse(text, &error);
    check(parsed && parsed->dollies[0].interpolation == CameraDollyInterpolation::Smooth, "smooth round-trips: " + error);
    auto v3 = text;
    v3.replace(v3.find(" 4\n"), 3, " 3\n");
    v3.replace(v3.find(" smooth\n"), 8, "\n");
    parsed = CameraSequence::parse(v3, &error);
    check(parsed && parsed->dollies[0].interpolation == CameraDollyInterpolation::CatmullRom, "v3 reads as Catmull-Rom: " + error);
    auto bad = text;
    bad.replace(bad.find(" smooth\n"), 8, " wobbly\n");
    check(!CameraSequence::parse(bad, &error), "unknown interpolation refuses");

    // The sequencer toggles it as one undo step.
    EditorCameraSequencer sequencer;
    sequence.dollies[0].interpolation = CameraDollyInterpolation::CatmullRom;
    check(sequencer.set_sequence(sequence, &error), error);
    check(sequencer.set_dolly_interpolation(1, CameraDollyInterpolation::Smooth, &error) &&
          sequencer.sequence().dollies[0].interpolation == CameraDollyInterpolation::Smooth, "sequencer sets smooth");
    check(sequencer.undo() && sequencer.sequence().dollies[0].interpolation == CameraDollyInterpolation::CatmullRom, "undo");
    check(!sequencer.set_dolly_interpolation(9, CameraDollyInterpolation::Smooth, &error), "unknown dolly refuses");
}
} // namespace

int main() {
    try {
        curvature_queries();
        internal_continuity();
        corner_blend_and_joins();
        bridges();
        surface_g2();
        seam_g2();
        editor_actions();
        dolly();
    } catch (const std::exception& e) {
        std::cerr << "dve_nurbs_curvature_tests failed: " << e.what() << '\n';
        return 1;
    }
    std::cout << "dve_nurbs_curvature_tests passed\n";
    return 0;
}
