#include "dve/voxel_boolean.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace dve {
namespace {

struct Int3Hash {
    std::size_t operator()(Int3 v) const noexcept {
        std::uint64_t h = static_cast<std::uint32_t>(v.x);
        h = h * 0x9E3779B97F4A7C15ULL ^ static_cast<std::uint32_t>(v.y);
        h = h * 0xBF58476D1CE4E5B9ULL ^ static_cast<std::uint32_t>(v.z);
        h ^= h >> 31U;
        h *= 0x94D049BB133111EBULL;
        h ^= h >> 29U;
        return static_cast<std::size_t>(h);
    }
};

// Rigid transform in double precision: p' = R p + t.
struct Affine {
    std::array<std::array<double, 3>, 3> r{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    std::array<double, 3> t{};

    [[nodiscard]] std::array<double, 3> apply(const std::array<double, 3>& p) const noexcept {
        return {r[0][0] * p[0] + r[0][1] * p[1] + r[0][2] * p[2] + t[0],
                r[1][0] * p[0] + r[1][1] * p[1] + r[1][2] * p[2] + t[1],
                r[2][0] * p[0] + r[2][1] * p[1] + r[2][2] * p[2] + t[2]};
    }
};

Affine affine_from(const RigidTransform& transform) noexcept {
    double x = transform.rotation.x, y = transform.rotation.y, z = transform.rotation.z, w = transform.rotation.w;
    const double norm = std::sqrt(x * x + y * y + z * z + w * w);
    if (norm > 0.0) { x /= norm; y /= norm; z /= norm; w /= norm; }
    Affine a;
    a.r = {{{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
            {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
            {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}}};
    a.t = {transform.position.x, transform.position.y, transform.position.z};
    return a;
}

Affine inverse(const Affine& a) noexcept {
    Affine result;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) result.r[i][j] = a.r[j][i];
    for (int i = 0; i < 3; ++i)
        result.t[i] = -(result.r[i][0] * a.t[0] + result.r[i][1] * a.t[1] + result.r[i][2] * a.t[2]);
    return result;
}

Affine compose(const Affine& outer, const Affine& inner) noexcept {
    Affine result;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j)
            result.r[i][j] = outer.r[i][0] * inner.r[0][j] + outer.r[i][1] * inner.r[1][j] + outer.r[i][2] * inner.r[2][j];
        result.t[i] = outer.r[i][0] * inner.t[0] + outer.r[i][1] * inner.t[1] + outer.r[i][2] * inner.t[2] + outer.t[i];
    }
    return result;
}

// Maps voxel indices of a source grid to voxel indices of a destination grid by transforming the
// source voxel centre and flooring. `local` maps source-local metres to destination-local metres.
struct GridMap {
    Affine local;
    double sourceSize{};
    double destinationSize{};

    [[nodiscard]] Int3 map(Int3 v) const noexcept {
        const std::array<double, 3> centre{(v.x + 0.5) * sourceSize, (v.y + 0.5) * sourceSize, (v.z + 0.5) * sourceSize};
        const std::array<double, 3> p = local.apply(centre);
        return {static_cast<std::int32_t>(std::floor(p[0] / destinationSize)),
                static_cast<std::int32_t>(std::floor(p[1] / destinationSize)),
                static_cast<std::int32_t>(std::floor(p[2] / destinationSize))};
    }
};

bool finite_transform(const RigidTransform& t) noexcept {
    const Quaternion q = t.rotation;
    const float n = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    return std::isfinite(t.position.x) && std::isfinite(t.position.y) && std::isfinite(t.position.z) &&
           std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w) &&
           std::isfinite(n) && n > 1.0e-12F;
}

bool valid_volume(const VoxelBooleanVolume& v) noexcept {
    return v.voxels != nullptr && std::isfinite(v.voxelSizeMeters) && v.voxelSizeMeters > 0.0F &&
           finite_transform(v.transform);
}

std::string display_name(const VoxelBooleanVolume& v, std::string_view fallback) {
    return v.name.empty() ? std::string(fallback) : "'" + std::string(v.name) + "'";
}

// Why an operand cannot be mapped voxel-for-voxel, or empty when it can.
std::string misalignment_reason(const Affine& operandToPrimary, double primarySize, double operandSize,
                                double tolerance) {
    if (std::abs(primarySize - operandSize) > tolerance * primarySize) {
        std::ostringstream text;
        text << "voxel size " << operandSize << " m differs from the target's " << primarySize << " m";
        return text.str();
    }
    for (const auto& row : operandToPrimary.r)
        for (const double value : row) {
            const double nearest = std::round(value);
            if (std::abs(value - nearest) > tolerance || std::abs(nearest) > 1.0)
                return "rotation is not a multiple of 90 degrees relative to the target";
        }
    for (const double value : operandToPrimary.t) {
        const double voxels = value / primarySize;
        if (std::abs(voxels - std::round(voxels)) > tolerance)
            return "offset is not a whole number of target voxels";
    }
    return {};
}

void for_each_voxel(const VoxelObject& object, auto&& visit) {
    for (const auto& [key, brick] : object.bricks()) {
        brick.occupancy().for_each_set([&](std::uint16_t index) {
            visit(global_from_local(key, local_from_index_unchecked(index)), brick.material(index));
        });
    }
}

struct OperandPlan {
    const VoxelBooleanVolume* volume{};
    std::size_t index{};
    bool aligned{};
    GridMap toOperand;   // primary grid -> operand grid
    GridMap toPrimary;   // operand grid -> primary grid
    Affine operandToPrimaryLocal;
};

// Inclusive primary-grid cell range covering one operand brick after transformation.
std::pair<Int3, Int3> brick_cell_range(const OperandPlan& plan, BrickKey key) noexcept {
    const Int3 origin = brick_origin(key);
    const double s = plan.toPrimary.sourceSize;
    std::array<double, 3> lo{1e300, 1e300, 1e300};
    std::array<double, 3> hi{-1e300, -1e300, -1e300};
    for (int corner = 0; corner < 8; ++corner) {
        const std::array<double, 3> p{(origin.x + ((corner & 1) ? kBrickDim : 0)) * s,
                                      (origin.y + ((corner & 2) ? kBrickDim : 0)) * s,
                                      (origin.z + ((corner & 4) ? kBrickDim : 0)) * s};
        const auto q = plan.operandToPrimaryLocal.apply(p);
        for (int i = 0; i < 3; ++i) { lo[i] = std::min(lo[i], q[i]); hi[i] = std::max(hi[i], q[i]); }
    }
    const double d = plan.toPrimary.destinationSize;
    // A cell is sampled at its centre, so cells whose centre lies inside [lo, hi] suffice.
    const auto first = [&](double v) { return static_cast<std::int32_t>(std::ceil(v / d - 0.5)); };
    const auto last = [&](double v) { return static_cast<std::int32_t>(std::floor(v / d - 0.5)); };
    return {{first(lo[0]), first(lo[1]), first(lo[2])}, {last(hi[0]), last(hi[1]), last(hi[2])}};
}

std::uint64_t range_volume(const std::pair<Int3, Int3>& range) noexcept {
    const auto extent = [](std::int32_t a, std::int32_t b) -> std::uint64_t {
        return b < a ? 0U : static_cast<std::uint64_t>(static_cast<std::int64_t>(b) - a + 1);
    };
    return extent(range.first.x, range.second.x) * extent(range.first.y, range.second.y) *
           extent(range.first.z, range.second.z);
}

void add_diagnostic(VoxelBooleanResult& result, VoxelBooleanSeverity severity, VoxelBooleanDiagnosticCode code,
                    std::string message, std::size_t operand = std::numeric_limits<std::size_t>::max()) {
    result.diagnostics.push_back({severity, code, operand, std::move(message)});
}

} // namespace

std::string_view voxel_boolean_operation_name(VoxelBooleanOperation operation) noexcept {
    switch (operation) {
    case VoxelBooleanOperation::Union: return "Union";
    case VoxelBooleanOperation::Difference: return "Difference";
    case VoxelBooleanOperation::Intersection: return "Intersection";
    }
    return "Boolean";
}

std::string_view voxel_boolean_operation_symbol(VoxelBooleanOperation operation) noexcept {
    switch (operation) {
    case VoxelBooleanOperation::Union: return "A + B";
    case VoxelBooleanOperation::Difference: return "A - B";
    case VoxelBooleanOperation::Intersection: return "A & B";
    }
    return "A ? B";
}

bool VoxelBooleanResult::has_errors() const noexcept {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [](const VoxelBooleanDiagnostic& d) {
        return d.severity == VoxelBooleanSeverity::Error;
    });
}

bool VoxelBooleanResult::can_commit() const noexcept {
    return computed && !has_errors() && (!changes.empty() || stats.anchorsTransferred > 0 || stats.anchorsDropped > 0);
}

std::string VoxelBooleanResult::blocking_reason() const {
    for (const VoxelBooleanDiagnostic& d : diagnostics)
        if (d.severity == VoxelBooleanSeverity::Error) return d.message;
    if (!computed) return "The Boolean operation was not computed.";
    if (!can_commit()) return "The Boolean operation would not change anything.";
    return {};
}

VoxelBooleanResult compute_voxel_boolean(const VoxelBooleanVolume& primary,
                                         std::span<const VoxelBooleanVolume> operands,
                                         const VoxelBooleanOptions& options) {
    VoxelBooleanResult result;
    result.operation = options.operation;
    const std::string primaryName = display_name(primary, "the target");
    if (!valid_volume(primary)) {
        add_diagnostic(result, VoxelBooleanSeverity::Error, VoxelBooleanDiagnosticCode::InvalidPrimary,
                       "Target " + primaryName + " has no voxel data, an invalid voxel size or a non-finite transform.");
        return result;
    }
    const VoxelObject& a = *primary.voxels;
    result.stats.primaryVoxels = a.occupied_voxel_count();
    if (operands.empty()) {
        add_diagnostic(result, VoxelBooleanSeverity::Error, VoxelBooleanDiagnosticCode::InvalidOperand,
                       "Select at least one operand voxel object in addition to the target.");
        return result;
    }

    const double tolerance = std::max(1.0e-6, static_cast<double>(options.alignmentTolerance));
    const Affine primaryWorld = affine_from(primary.transform);
    const Affine worldToPrimary = inverse(primaryWorld);
    const double primarySize = primary.voxelSizeMeters;
    std::vector<OperandPlan> plans;
    for (std::size_t i = 0; i < operands.size(); ++i) {
        const VoxelBooleanVolume& operand = operands[i];
        const std::string name = display_name(operand, "operand " + std::to_string(i + 1));
        if (!valid_volume(operand) || operand.voxels == primary.voxels) {
            add_diagnostic(result, VoxelBooleanSeverity::Error, VoxelBooleanDiagnosticCode::InvalidOperand,
                           operand.voxels == primary.voxels && operand.voxels
                               ? "Operand " + name + " is the target itself."
                               : "Operand " + name + " has no voxel data, an invalid voxel size or a non-finite transform.",
                           i);
            continue;
        }
        const std::uint64_t count = operand.voxels->occupied_voxel_count();
        if (count == 0) {
            add_diagnostic(result, VoxelBooleanSeverity::Warning, VoxelBooleanDiagnosticCode::EmptyOperand,
                           "Operand " + name + " has no voxels and is ignored.", i);
            continue;
        }
        result.stats.operandVoxels += count;
        OperandPlan plan;
        plan.volume = &operand;
        plan.index = i;
        const Affine operandWorld = affine_from(operand.transform);
        plan.operandToPrimaryLocal = compose(worldToPrimary, operandWorld);
        const Affine primaryToOperandLocal = inverse(plan.operandToPrimaryLocal);
        plan.toPrimary = {plan.operandToPrimaryLocal, operand.voxelSizeMeters, primarySize};
        plan.toOperand = {primaryToOperandLocal, primarySize, operand.voxelSizeMeters};
        const std::string reason = misalignment_reason(plan.operandToPrimaryLocal, primarySize,
                                                       operand.voxelSizeMeters, tolerance);
        plan.aligned = reason.empty();
        if (plan.aligned) {
            ++result.stats.alignedOperands;
        } else {
            ++result.stats.resampledOperands;
            add_diagnostic(result, VoxelBooleanSeverity::Warning, VoxelBooleanDiagnosticCode::ResampledOperand,
                           "Operand " + name + " is resampled into " + primaryName + "'s grid (" + reason +
                               "); details thinner than one target voxel may be lost.",
                           i);
        }
        plans.push_back(std::move(plan));
    }
    if (result.has_errors()) return result;
    if (plans.empty()) {
        add_diagnostic(result, VoxelBooleanSeverity::Error, VoxelBooleanDiagnosticCode::InvalidOperand,
                       "Every operand is empty; there is nothing to combine with " + primaryName + ".");
        return result;
    }

    // Cost estimate before doing any work.
    std::uint64_t work = 0;
    if (options.operation == VoxelBooleanOperation::Union) {
        for (const OperandPlan& plan : plans) {
            if (plan.aligned) {
                work += plan.volume->voxels->occupied_voxel_count();
            } else {
                for (const auto& [key, brick] : plan.volume->voxels->bricks()) {
                    (void)brick;
                    work += range_volume(brick_cell_range(plan, key));
                }
            }
        }
    } else {
        work = result.stats.primaryVoxels * plans.size();
    }
    result.stats.estimatedWork = work;
    if (work > options.maximumWork) {
        std::ostringstream text;
        text << voxel_boolean_operation_name(options.operation) << " would visit about " << work
             << " voxels, above the limit of " << options.maximumWork
             << ". Use operands with the target's voxel size and rotation, or split the objects.";
        add_diagnostic(result, VoxelBooleanSeverity::Error, VoxelBooleanDiagnosticCode::ExcessiveCost, text.str());
        return result;
    }

    std::unordered_set<Int3, Int3Hash> removed;
    std::unordered_map<Int3, MaterialId, Int3Hash> added;
    std::vector<Int3> overlapCells;
    const auto collect_overlap = [&](Int3 cell) {
        if (options.collectOverlapCells && overlapCells.size() < options.maximumOverlapCells)
            overlapCells.push_back(cell);
    };

    if (options.operation == VoxelBooleanOperation::Union) {
        std::unordered_set<Int3, Int3Hash> claimed; // solid primary cells already covered by an operand
        const auto visit = [&](Int3 cell, MaterialId material) {
            const MaterialId existing = a.material_at(cell);
            if (existing != kAirMaterial) {
                if (!claimed.insert(cell).second) return;
                ++result.stats.overlapVoxels;
                collect_overlap(cell);
                if (options.overlapMaterial == VoxelBooleanOverlapMaterial::TakeOperand && material != existing) {
                    result.changes.push_back({cell, existing, material});
                    ++result.stats.recoloredVoxels;
                }
                return;
            }
            added.emplace(cell, material);
        };
        for (const OperandPlan& plan : plans) {
            const VoxelObject& b = *plan.volume->voxels;
            if (plan.aligned) {
                for_each_voxel(b, [&](Int3 voxel, MaterialId material) { visit(plan.toPrimary.map(voxel), material); });
                continue;
            }
            for (const auto& [key, brick] : b.bricks()) {
                const Bitset512 occupancy = brick.occupancy();
                if (occupancy.none()) continue;
                const auto [lo, hi] = brick_cell_range(plan, key);
                for (std::int32_t z = lo.z; z <= hi.z; ++z)
                    for (std::int32_t y = lo.y; y <= hi.y; ++y)
                        for (std::int32_t x = lo.x; x <= hi.x; ++x) {
                            const Int3 sample = plan.toOperand.map({x, y, z});
                            if (brick_key_from_voxel(sample) != key) continue;
                            const std::uint16_t index = voxel_index_unchecked(local_voxel_from_global(sample));
                            if (!occupancy.test(index)) continue;
                            visit({x, y, z}, brick.material(index));
                        }
            }
        }
        for (const auto& [cell, material] : added) result.changes.push_back({cell, kAirMaterial, material});
        result.stats.addedVoxels = added.size();
    } else {
        const bool difference = options.operation == VoxelBooleanOperation::Difference;
        for_each_voxel(a, [&](Int3 voxel, MaterialId material) {
            std::size_t inside = 0;
            for (const OperandPlan& plan : plans)
                if (plan.volume->voxels->occupied_at(plan.toOperand.map(voxel))) ++inside;
            if (inside > 0) {
                ++result.stats.overlapVoxels;
                collect_overlap(voxel);
            }
            const bool remove = difference ? inside > 0 : inside < plans.size();
            if (remove) {
                result.changes.push_back({voxel, material, kAirMaterial});
                removed.insert(voxel);
            }
        });
        result.stats.removedVoxels = removed.size();
    }
    std::sort(result.changes.begin(), result.changes.end());
    std::sort(overlapCells.begin(), overlapCells.end());
    result.overlapCells = std::move(overlapCells);
    result.stats.resultVoxels = result.stats.primaryVoxels + result.stats.addedVoxels - result.stats.removedVoxels;

    // Anchors.
    const auto solid_in_result = [&](Int3 cell) {
        if (added.contains(cell)) return true;
        return a.occupied_at(cell) && !removed.contains(cell);
    };
    std::vector<Int3> anchors;
    for (const Int3 anchor : primary.anchors) {
        if (solid_in_result(anchor)) { anchors.push_back(anchor); ++result.stats.anchorsKept; }
        else ++result.stats.anchorsDropped;
    }
    std::sort(anchors.begin(), anchors.end());
    anchors.erase(std::unique(anchors.begin(), anchors.end()), anchors.end());
    if (options.operation == VoxelBooleanOperation::Union && options.transferOperandAnchors) {
        std::vector<Int3> transferred;
        for (const OperandPlan& plan : plans)
            for (const Int3 anchor : plan.volume->anchors) {
                if (!plan.volume->voxels->occupied_at(anchor)) continue;
                const Int3 cell = plan.toPrimary.map(anchor);
                if (solid_in_result(cell) && !std::binary_search(anchors.begin(), anchors.end(), cell))
                    transferred.push_back(cell);
            }
        std::sort(transferred.begin(), transferred.end());
        transferred.erase(std::unique(transferred.begin(), transferred.end()), transferred.end());
        result.stats.anchorsTransferred = transferred.size();
        anchors.insert(anchors.end(), transferred.begin(), transferred.end());
        std::sort(anchors.begin(), anchors.end());
    }
    result.anchors = std::move(anchors);

    // Outcome diagnostics (ART-066: explain empty / no-op results before commit).
    const std::string operandWord = plans.size() == 1 ? "the operand" : "any operand";
    switch (options.operation) {
    case VoxelBooleanOperation::Union:
        if (result.stats.overlapVoxels == 0)
            add_diagnostic(result, VoxelBooleanSeverity::Warning, VoxelBooleanDiagnosticCode::NoOverlap,
                           "No operand overlaps " + primaryName + "; the union will contain separate pieces.");
        if (result.changes.empty() && result.stats.anchorsTransferred == 0)
            add_diagnostic(result, VoxelBooleanSeverity::Error, VoxelBooleanDiagnosticCode::NoChange,
                           "Union would change nothing: the operands lie entirely inside " + primaryName + ".");
        break;
    case VoxelBooleanOperation::Difference:
        if (result.stats.overlapVoxels == 0)
            add_diagnostic(result, VoxelBooleanSeverity::Error, VoxelBooleanDiagnosticCode::NoChange,
                           "Difference would change nothing: " + operandWord + " does not overlap " + primaryName +
                               ". Move the operand into the target or choose Union.");
        else if (result.stats.resultVoxels == 0)
            add_diagnostic(result, VoxelBooleanSeverity::Error, VoxelBooleanDiagnosticCode::EmptyResult,
                           "Difference would remove every voxel of " + primaryName +
                               ". Delete the object instead, or move the operand.");
        break;
    case VoxelBooleanOperation::Intersection:
        if (result.stats.resultVoxels == 0)
            add_diagnostic(result, VoxelBooleanSeverity::Error,
                           result.stats.overlapVoxels == 0 ? VoxelBooleanDiagnosticCode::NoOverlap
                                                           : VoxelBooleanDiagnosticCode::EmptyResult,
                           result.stats.overlapVoxels == 0
                               ? "Intersection would be empty: " + operandWord + " does not overlap " + primaryName +
                                     ". Move the operands so they overlap, or choose Union."
                               : "Intersection would be empty: no voxel of " + primaryName +
                                     " lies inside every operand.");
        else if (result.changes.empty())
            add_diagnostic(result, VoxelBooleanSeverity::Error, VoxelBooleanDiagnosticCode::NoChange,
                           "Intersection would change nothing: " + primaryName + " lies entirely inside the operands.");
        break;
    }
    result.computed = true;
    return result;
}

bool apply_voxel_boolean_changes(VoxelObject& target, std::span<const VoxelBooleanChange> changes, bool forward) {
    for (const VoxelBooleanChange& change : changes)
        if (target.material_at(change.voxel) != (forward ? change.before : change.after)) return false;
    for (const VoxelBooleanChange& change : changes)
        (void)target.set_voxel(change.voxel, forward ? change.after : change.before);
    return true;
}

} // namespace dve
