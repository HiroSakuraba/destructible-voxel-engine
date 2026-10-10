// Unit tests for the backend-independent voxel Boolean core (ART-060).
#include "dve/voxel_boolean.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string("CHECK failed: ") + #x + " (line " + std::to_string(__LINE__) + ")"); } while (false)

namespace {
using namespace dve;

void fill_box(VoxelObject& object, Int3 lo, Int3 hiExclusive, MaterialId material) {
    for (std::int32_t z = lo.z; z < hiExclusive.z; ++z)
        for (std::int32_t y = lo.y; y < hiExclusive.y; ++y)
            for (std::int32_t x = lo.x; x < hiExclusive.x; ++x) (void)object.set_voxel({x, y, z}, material);
}

std::map<Int3, MaterialId> snapshot(const VoxelObject& object) {
    std::map<Int3, MaterialId> result;
    for (const auto& [key, brick] : object.bricks())
        brick.occupancy().for_each_set([&](std::uint16_t index) {
            result[global_from_local(key, local_from_index_unchecked(index))] = brick.material(index);
        });
    return result;
}

std::map<Int3, MaterialId> apply_to_copy(const VoxelObject& source, const VoxelBooleanResult& result) {
    std::map<Int3, MaterialId> state = snapshot(source);
    for (const VoxelBooleanChange& change : result.changes) {
        if (change.after == kAirMaterial) state.erase(change.voxel);
        else state[change.voxel] = change.after;
    }
    return state;
}

bool has_code(const VoxelBooleanResult& result, VoxelBooleanDiagnosticCode code) {
    for (const auto& d : result.diagnostics) if (d.code == code) return true;
    return false;
}

VoxelBooleanVolume volume(const VoxelObject& object, RigidTransform transform = {}, float size = 0.1F,
                          std::span<const Int3> anchors = {}, std::string_view name = {}) {
    return {&object, transform, size, anchors, name};
}

VoxelBooleanResult run(const VoxelBooleanVolume& a, std::vector<VoxelBooleanVolume> operands,
                       VoxelBooleanOperation op, VoxelBooleanOptions options = {}) {
    options.operation = op;
    return compute_voxel_boolean(a, operands, options);
}

// Brute-force reference: a primary cell is inside an operand when its centre maps into an
// occupied operand voxel (the documented sampling rule).
bool inside(const VoxelBooleanVolume& operand, const VoxelBooleanVolume& primary, Int3 cell) {
    const float s = primary.voxelSizeMeters;
    const Float3 local{(cell.x + 0.5F) * s, (cell.y + 0.5F) * s, (cell.z + 0.5F) * s};
    const Float3 world = transform_point(primary.transform, local);
    const Float3 o = inverse_transform_point(operand.transform, world);
    const float t = operand.voxelSizeMeters;
    const Int3 v{static_cast<std::int32_t>(std::floor(o.x / t)), static_cast<std::int32_t>(std::floor(o.y / t)),
                 static_cast<std::int32_t>(std::floor(o.z / t))};
    return operand.voxels->occupied_at(v);
}

void check_against_reference(const VoxelBooleanVolume& a, const VoxelBooleanVolume& b, VoxelBooleanOperation op,
                             Int3 lo, Int3 hi, std::size_t allowedMismatches = 0) {
    const VoxelBooleanResult result = run(a, {b}, op);
    CHECK(result.computed);
    const auto state = apply_to_copy(*a.voxels, result);
    std::size_t mismatches = 0;
    for (std::int32_t z = lo.z; z <= hi.z; ++z)
        for (std::int32_t y = lo.y; y <= hi.y; ++y)
            for (std::int32_t x = lo.x; x <= hi.x; ++x) {
                const Int3 cell{x, y, z};
                const bool inA = a.voxels->occupied_at(cell);
                const bool inB = inside(b, a, cell);
                const bool expected = op == VoxelBooleanOperation::Union ? (inA || inB)
                                    : op == VoxelBooleanOperation::Difference ? (inA && !inB) : (inA && inB);
                if (expected != state.contains(cell)) ++mismatches;
            }
    // Every result voxel must lie in the scanned window.
    for (const auto& [cell, material] : state) {
        (void)material;
        CHECK(cell.x >= lo.x && cell.x <= hi.x && cell.y >= lo.y && cell.y <= hi.y && cell.z >= lo.z && cell.z <= hi.z);
    }
    // float vs double centre evaluation may disagree on cells whose centre lies exactly on an
    // operand voxel face; the aligned fast path must match exactly.
    if (mismatches > allowedMismatches)
        throw std::runtime_error("reference mismatch: " + std::to_string(mismatches) + " cells, op " +
                                 std::string(voxel_boolean_operation_name(op)));
}

void test_aligned_basic() {
    VoxelObject a(1), b(2);
    fill_box(a, {0, 0, 0}, {4, 4, 4}, 1);
    fill_box(b, {0, 0, 0}, {4, 4, 4}, 2);
    const RigidTransform shifted = make_rigid_transform({0.2F, 0.0F, 0.0F}, {});
    const auto A = volume(a, {}, 0.1F, {}, "A");
    const auto B = volume(b, shifted, 0.1F, {}, "B");

    auto u = run(A, {B}, VoxelBooleanOperation::Union);
    CHECK(u.can_commit());
    CHECK(u.stats.alignedOperands == 1 && u.stats.resampledOperands == 0);
    CHECK(u.stats.overlapVoxels == 32 && u.stats.addedVoxels == 32 && u.stats.resultVoxels == 96);
    CHECK(u.changes.size() == 32);
    for (const auto& c : u.changes) CHECK(c.before == kAirMaterial && c.after == 2 && c.voxel.x >= 4 && c.voxel.x <= 5);
    const auto unionState = apply_to_copy(a, u);
    CHECK(unionState.size() == 96);
    CHECK(unionState.at({1, 1, 1}) == 1);   // overlap keeps A's material by default
    CHECK(unionState.at({3, 1, 1}) == 1);
    CHECK(unionState.at({5, 1, 1}) == 2);

    VoxelBooleanOptions take;
    take.overlapMaterial = VoxelBooleanOverlapMaterial::TakeOperand;
    auto u2 = run(A, {B}, VoxelBooleanOperation::Union, take);
    CHECK(u2.stats.recoloredVoxels == 32 && u2.changes.size() == 64);
    CHECK(apply_to_copy(a, u2).at({3, 1, 1}) == 2);
    CHECK(apply_to_copy(a, u2).at({1, 1, 1}) == 1);

    auto d = run(A, {B}, VoxelBooleanOperation::Difference);
    CHECK(d.can_commit());
    CHECK(d.stats.removedVoxels == 32 && d.stats.resultVoxels == 32 && d.stats.overlapVoxels == 32);
    const auto diffState = apply_to_copy(a, d);
    for (const auto& [cell, material] : diffState) CHECK(cell.x <= 1 && material == 1);

    auto i = run(A, {B}, VoxelBooleanOperation::Intersection);
    CHECK(i.can_commit());
    CHECK(i.stats.resultVoxels == 32 && i.stats.removedVoxels == 32);
    for (const auto& [cell, material] : apply_to_copy(a, i)) CHECK(cell.x >= 2 && cell.x <= 3 && material == 1);

    // Changes are sorted and unique.
    for (std::size_t k = 1; k < u.changes.size(); ++k) CHECK(u.changes[k - 1].voxel < u.changes[k].voxel);

    // Deterministic.
    CHECK(run(A, {B}, VoxelBooleanOperation::Union).changes == u.changes);
}

void test_primary_transform_is_respected() {
    // Both objects moved together: the result in A's grid is identical.
    VoxelObject a(1), b(2);
    fill_box(a, {0, 0, 0}, {4, 4, 4}, 1);
    fill_box(b, {0, 0, 0}, {4, 4, 4}, 2);
    const Quaternion q = quaternion_from_axis_angle({0, 1, 0}, 0.7F);
    const RigidTransform ta = make_rigid_transform({3.0F, 1.0F, -2.0F}, q);
    const RigidTransform tb = compose_rigid_transforms(ta, make_rigid_transform({0.2F, 0.0F, 0.0F}, {}));
    auto r = run(volume(a, ta), {volume(b, tb)}, VoxelBooleanOperation::Union);
    CHECK(r.stats.alignedOperands == 1);
    CHECK(r.stats.addedVoxels == 32 && r.stats.resultVoxels == 96);
}

void test_rotated_90_is_exact() {
    VoxelObject a(1), b(2);
    fill_box(a, {0, 0, 0}, {6, 4, 6}, 1);
    fill_box(b, {0, 0, 0}, {2, 4, 8}, 2); // long along z
    // Rotate 90 degrees about Y and place so the bar crosses A along x.
    const RigidTransform tb = make_rigid_transform({-0.1F, 0.0F, 0.3F}, quaternion_from_axis_angle({0, 1, 0}, 1.5707963F));
    const auto A = volume(a);
    const auto B = volume(b, tb);
    for (auto op : {VoxelBooleanOperation::Union, VoxelBooleanOperation::Difference, VoxelBooleanOperation::Intersection}) {
        const auto r = run(A, {B}, op);
        CHECK(r.stats.alignedOperands == 1);
        check_against_reference(A, B, op, {-4, -2, -4}, {12, 6, 12});
    }
}

void test_resampled_rotation_and_scale() {
    VoxelObject a(1), b(2);
    fill_box(a, {0, 0, 0}, {16, 8, 16}, 1);
    fill_box(b, {0, 0, 0}, {10, 10, 10}, 3);
    const RigidTransform rotated = make_rigid_transform({0.55F, 0.13F, 0.4F}, quaternion_from_axis_angle(normalize(Float3{0.3F, 1.0F, 0.2F}), 0.52F));
    const auto A = volume(a);
    const auto B = volume(b, rotated, 0.1F, {}, "Rotated");
    for (auto op : {VoxelBooleanOperation::Union, VoxelBooleanOperation::Difference, VoxelBooleanOperation::Intersection}) {
        const auto r = run(A, {B}, op);
        CHECK(r.computed);
        CHECK(r.stats.resampledOperands == 1);
        CHECK(has_code(r, VoxelBooleanDiagnosticCode::ResampledOperand));
        check_against_reference(A, B, op, {-12, -12, -12}, {30, 30, 30}, 2);
    }
    // Different voxel size (finer operand, unrotated): resampled, materials from the operand.
    VoxelObject fine(3);
    fill_box(fine, {0, 0, 0}, {8, 8, 8}, 4); // 0.4 m cube at 0.05 m voxels
    const auto F = volume(fine, make_rigid_transform({1.4F, 0.0F, 0.0F}, {}), 0.05F, {}, "Fine");
    const auto u = run(A, {F}, VoxelBooleanOperation::Union);
    CHECK(u.stats.resampledOperands == 1);
    CHECK(u.stats.addedVoxels == 2 * 4 * 4); // x cells 16,17 (1.6..1.8 m) are new, 14,15 overlap
    CHECK(u.stats.overlapVoxels == 2 * 4 * 4);
    for (const auto& c : u.changes) CHECK(c.after == 4);
    check_against_reference(A, F, VoxelBooleanOperation::Difference, {-2, -2, -2}, {20, 10, 20}, 2);
    // Coarser operand.
    VoxelObject coarse(4);
    fill_box(coarse, {0, 0, 0}, {2, 2, 2}, 5); // 0.4 m cube at 0.2 m voxels
    const auto C = volume(coarse, make_rigid_transform({0.0F, 0.0F, 0.0F}, {}), 0.2F);
    const auto d = run(A, {C}, VoxelBooleanOperation::Difference);
    CHECK(d.stats.removedVoxels == 4 * 4 * 4);
}

void test_empty_and_no_change_results() {
    VoxelObject a(1), b(2), empty(3);
    fill_box(a, {0, 0, 0}, {4, 4, 4}, 1);
    fill_box(b, {0, 0, 0}, {2, 2, 2}, 2);
    const auto A = volume(a, {}, 0.1F, {}, "A");
    const auto far = volume(b, make_rigid_transform({5.0F, 0.0F, 0.0F}, {}), 0.1F, {}, "B");

    auto i = run(A, {far}, VoxelBooleanOperation::Intersection);
    CHECK(i.computed && !i.can_commit());
    CHECK(has_code(i, VoxelBooleanDiagnosticCode::NoOverlap));
    CHECK(i.blocking_reason().find("Intersection would be empty") != std::string::npos);

    auto d = run(A, {far}, VoxelBooleanOperation::Difference);
    CHECK(!d.can_commit() && has_code(d, VoxelBooleanDiagnosticCode::NoChange));
    CHECK(d.blocking_reason().find("does not overlap") != std::string::npos);

    auto u = run(A, {far}, VoxelBooleanOperation::Union);
    CHECK(u.can_commit());   // disjoint union is allowed, with a warning
    CHECK(has_code(u, VoxelBooleanDiagnosticCode::NoOverlap) && !u.has_errors());

    const auto inside = volume(b, make_rigid_transform({0.1F, 0.1F, 0.1F}, {}));
    auto u2 = run(A, {inside}, VoxelBooleanOperation::Union);
    CHECK(!u2.can_commit() && has_code(u2, VoxelBooleanDiagnosticCode::NoChange));

    auto i2 = run(volume(b), {A}, VoxelBooleanOperation::Intersection);
    CHECK(!i2.can_commit() && has_code(i2, VoxelBooleanDiagnosticCode::NoChange));

    VoxelObject big(4);
    fill_box(big, {-1, -1, -1}, {6, 6, 6}, 2);
    auto d2 = run(A, {volume(big)}, VoxelBooleanOperation::Difference);
    CHECK(!d2.can_commit() && has_code(d2, VoxelBooleanDiagnosticCode::EmptyResult));
    CHECK(d2.stats.resultVoxels == 0);

    // Empty operand: ignored with a warning; all-empty operands are an error.
    auto e = run(A, {volume(empty), far}, VoxelBooleanOperation::Union);
    CHECK(e.computed && has_code(e, VoxelBooleanDiagnosticCode::EmptyOperand));
    auto e2 = run(A, {volume(empty)}, VoxelBooleanOperation::Union);
    CHECK(!e2.computed && !e2.can_commit() && e2.has_errors());

    // Empty primary: union adopts the operand, the others report empty / no change.
    VoxelObject none(5);
    auto ue = run(volume(none), {A}, VoxelBooleanOperation::Union);
    CHECK(ue.can_commit() && ue.stats.resultVoxels == 64);
    CHECK(!run(volume(none), {A}, VoxelBooleanOperation::Intersection).can_commit());
}

void test_invalid_inputs_and_cost() {
    VoxelObject a(1), b(2);
    fill_box(a, {0, 0, 0}, {4, 4, 4}, 1);
    fill_box(b, {0, 0, 0}, {4, 4, 4}, 2);
    CHECK(has_code(run(volume(a), {volume(a)}, VoxelBooleanOperation::Union), VoxelBooleanDiagnosticCode::InvalidOperand));
    CHECK(has_code(run(volume(a), {volume(b, {}, 0.0F)}, VoxelBooleanOperation::Union), VoxelBooleanDiagnosticCode::InvalidOperand));
    RigidTransform bad{};
    bad.position.x = std::nanf("");
    CHECK(has_code(run(volume(a), {volume(b, bad)}, VoxelBooleanOperation::Union), VoxelBooleanDiagnosticCode::InvalidOperand));
    CHECK(has_code(run(volume(a, bad), {volume(b)}, VoxelBooleanOperation::Union), VoxelBooleanDiagnosticCode::InvalidPrimary));
    CHECK(has_code(run(VoxelBooleanVolume{}, {volume(b)}, VoxelBooleanOperation::Union), VoxelBooleanDiagnosticCode::InvalidPrimary));
    CHECK(!run(volume(a), {}, VoxelBooleanOperation::Union).computed);

    VoxelBooleanOptions limited;
    limited.maximumWork = 10;
    const auto r = run(volume(a), {volume(b)}, VoxelBooleanOperation::Difference, limited);
    CHECK(!r.computed && has_code(r, VoxelBooleanDiagnosticCode::ExcessiveCost) && r.changes.empty());
    CHECK(r.stats.estimatedWork == 64);
}

void test_anchors() {
    VoxelObject a(1), b(2);
    fill_box(a, {0, 0, 0}, {4, 4, 4}, 1);
    fill_box(b, {0, 0, 0}, {4, 4, 4}, 2);
    const std::vector<Int3> anchorsA{{0, 0, 0}, {3, 0, 0}};
    const std::vector<Int3> anchorsB{{3, 0, 0}, {1, 0, 0}};
    const auto A = volume(a, {}, 0.1F, anchorsA);
    const auto B = volume(b, make_rigid_transform({0.2F, 0, 0}, {}), 0.1F, anchorsB);

    auto d = run(A, {B}, VoxelBooleanOperation::Difference);
    CHECK((d.anchors == std::vector<Int3>{{0, 0, 0}}));
    CHECK(d.stats.anchorsKept == 1 && d.stats.anchorsDropped == 1 && d.stats.anchorsTransferred == 0);

    auto i = run(A, {B}, VoxelBooleanOperation::Intersection);
    CHECK((i.anchors == std::vector<Int3>{{3, 0, 0}}));

    auto u = run(A, {B}, VoxelBooleanOperation::Union);
    // B's (3,0,0) maps to A's (5,0,0) (new); B's (1,0,0) maps to (3,0,0), already anchored.
    CHECK((u.anchors == std::vector<Int3>{{0, 0, 0}, {3, 0, 0}, {5, 0, 0}}));
    CHECK(u.stats.anchorsTransferred == 1);

    VoxelBooleanOptions noTransfer;
    noTransfer.transferOperandAnchors = false;
    CHECK(run(A, {B}, VoxelBooleanOperation::Union, noTransfer).anchors.size() == 2);
}

void test_multiple_operands() {
    VoxelObject a(1), b1(2), b2(3);
    fill_box(a, {0, 0, 0}, {8, 2, 2}, 1);
    fill_box(b1, {0, 0, 0}, {2, 2, 2}, 2);
    fill_box(b2, {0, 0, 0}, {2, 2, 2}, 3);
    const auto A = volume(a);
    const auto B1 = volume(b1, make_rigid_transform({0.0F, 0, 0}, {}));
    const auto B2 = volume(b2, make_rigid_transform({0.6F, 0, 0}, {}));
    auto d = run(A, {B1, B2}, VoxelBooleanOperation::Difference);
    CHECK(d.stats.removedVoxels == 16 && d.stats.resultVoxels == 16);
    auto i = run(A, {B1, B2}, VoxelBooleanOperation::Intersection);
    CHECK(!i.can_commit() && i.stats.resultVoxels == 0); // nothing is inside both operands

    // Union of two overlapping operands into empty space: the earlier operand's material wins.
    const auto C1 = volume(b2, make_rigid_transform({1.0F, 0, 0}, {}));  // material 3, cells 10..11
    const auto C2 = volume(b1, make_rigid_transform({1.1F, 0, 0}, {}));  // material 2, cells 11..12
    auto u = run(A, {C1, C2}, VoxelBooleanOperation::Union);
    const auto state = apply_to_copy(a, u);
    CHECK(state.at({10, 0, 0}) == 3 && state.at({11, 0, 0}) == 3 && state.at({12, 0, 0}) == 2);
    CHECK(u.stats.addedVoxels == 12);
}

void test_apply_round_trip() {
    VoxelObject a(1), b(2);
    fill_box(a, {0, 0, 0}, {6, 6, 6}, 1);
    fill_box(b, {0, 0, 0}, {6, 6, 6}, 2);
    const auto before = snapshot(a);
    const auto beforeHash = a.occupied_voxel_count();
    auto r = run(volume(a), {volume(b, make_rigid_transform({0.3F, 0.3F, 0.0F}, {}))}, VoxelBooleanOperation::Union);
    const auto expected = apply_to_copy(a, r);
    CHECK(apply_voxel_boolean_changes(a, r.changes, true));
    CHECK(snapshot(a) == expected);
    CHECK(a.occupied_voxel_count() == r.stats.resultVoxels);
    CHECK(!apply_voxel_boolean_changes(a, r.changes, true)); // precondition no longer holds
    CHECK(a.occupied_voxel_count() == r.stats.resultVoxels);  // and nothing changed
    CHECK(apply_voxel_boolean_changes(a, r.changes, false));
    CHECK(snapshot(a) == before && a.occupied_voxel_count() == beforeHash);
    CHECK(a.validate());
}

void test_large_volume() {
    VoxelObject a(1), b(2);
    fill_box(a, {0, 0, 0}, {64, 64, 64}, 1);
    fill_box(b, {0, 0, 0}, {64, 64, 64}, 2);
    const auto start = std::chrono::steady_clock::now();
    const auto A = volume(a);
    const auto B = volume(b, make_rigid_transform({3.2F, 0.0F, 0.0F}, {}));
    auto u = run(A, {B}, VoxelBooleanOperation::Union);
    CHECK(u.stats.addedVoxels == 32ULL * 64 * 64 && u.stats.resultVoxels == 96ULL * 64 * 64);
    auto d = run(A, {B}, VoxelBooleanOperation::Difference);
    CHECK(d.stats.removedVoxels == 32ULL * 64 * 64);
    auto i = run(A, {B}, VoxelBooleanOperation::Intersection);
    CHECK(i.stats.resultVoxels == 32ULL * 64 * 64);
    // Resampled large operand (rotated 45 degrees) stays well inside the default work budget.
    const auto R = volume(b, make_rigid_transform({3.2F, 0.0F, 0.0F}, quaternion_from_axis_angle({0, 1, 0}, 0.785398F)));
    auto ur = run(A, {R}, VoxelBooleanOperation::Union);
    CHECK(ur.computed && ur.stats.resampledOperands == 1 && ur.stats.addedVoxels > 0);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << "large volume (4 x 262144-voxel operations): " << seconds << " s\n";
    CHECK(seconds < 60.0);
}

void test_overlap_cells() {
    VoxelObject a(1), b(2);
    fill_box(a, {0, 0, 0}, {4, 4, 4}, 1);
    fill_box(b, {0, 0, 0}, {4, 4, 4}, 2);
    VoxelBooleanOptions options;
    options.collectOverlapCells = true;
    options.maximumOverlapCells = 10;
    auto r = run(volume(a), {volume(b, make_rigid_transform({0.2F, 0, 0}, {}))}, VoxelBooleanOperation::Union, options);
    CHECK(r.overlapCells.size() == 10);
    options.maximumOverlapCells = 1000;
    r = run(volume(a), {volume(b, make_rigid_transform({0.2F, 0, 0}, {}))}, VoxelBooleanOperation::Difference, options);
    CHECK(r.overlapCells.size() == 32);
    for (const Int3 c : r.overlapCells) CHECK(c.x >= 2 && c.x <= 3);
}

} // namespace

int main() {
    try {
        test_aligned_basic();
        test_primary_transform_is_respected();
        test_rotated_90_is_exact();
        test_resampled_rotation_and_scale();
        test_empty_and_no_change_results();
        test_invalid_inputs_and_cost();
        test_anchors();
        test_multiple_operands();
        test_apply_round_trip();
        test_overlap_cells();
        test_large_volume();
        std::cout << "voxel boolean tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
