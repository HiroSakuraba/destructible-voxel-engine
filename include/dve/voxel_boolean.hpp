#pragma once

// Authored voxel Boolean operations (Union, Difference, Intersection).
//
// Backend-independent: the inputs are VoxelObjects with world transforms and voxel sizes and the
// output is a sorted change list expressed in the *primary* (target) object's voxel grid, plus the
// resulting anchor set, statistics and diagnostics. Nothing is mutated; callers decide how to
// apply the result (the editor wraps it in a single undoable command, see editor_voxel_boolean).
//
// Grid policy. The result always lives in the primary object's grid. An operand whose grid is
// "aligned" with the primary (same voxel size, a rotation that is a multiple of 90 degrees about
// the primary axes, and an integral voxel offset) is mapped voxel-for-voxel, exactly. Any other
// operand is resampled: a primary-grid cell counts as inside the operand when the cell's centre
// falls in an occupied operand voxel (nearest-voxel point sampling). Resampling is reported as a
// diagnostic because features thinner than a primary voxel can disappear or alias.
//
// Material policy. Difference and Intersection never invent material: surviving voxels keep the
// primary object's material. Union fills previously empty primary cells with the material of the
// operand voxel that covers them (earlier operands win where operands overlap each other); where
// the primary is already solid, KeepPrimary (default) keeps the primary material and
// TakeOperand repaints it with the operand material.
//
// Anchor policy. Primary anchors survive when their voxel is still solid in the result. For Union
// (when transferOperandAnchors is set) operand anchors are mapped into the primary grid and kept
// when the mapped cell is solid in the result. Difference and Intersection never import anchors.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dve/transform.hpp"
#include "dve/voxel_object.hpp"

namespace dve {

enum class VoxelBooleanOperation : std::uint8_t { Union, Difference, Intersection };

[[nodiscard]] std::string_view voxel_boolean_operation_name(VoxelBooleanOperation operation) noexcept;
// "A + B", "A - B", "A & B" style formula for status lines and command labels.
[[nodiscard]] std::string_view voxel_boolean_operation_symbol(VoxelBooleanOperation operation) noexcept;

enum class VoxelBooleanOverlapMaterial : std::uint8_t { KeepPrimary, TakeOperand };

struct VoxelBooleanVolume {
    const VoxelObject* voxels{};
    RigidTransform transform{};         // world transform of the object's local origin
    float voxelSizeMeters{0.10F};       // voxel v occupies [v, v + 1) * size in local space
    std::span<const Int3> anchors{};    // anchored voxels in this object's grid
    std::string_view name{};            // used in diagnostics only
};

struct VoxelBooleanOptions {
    VoxelBooleanOperation operation{VoxelBooleanOperation::Union};
    VoxelBooleanOverlapMaterial overlapMaterial{VoxelBooleanOverlapMaterial::KeepPrimary};
    bool transferOperandAnchors{true};
    // Upper bound on point samples / voxel visits, checked before any work is done so an
    // accidental huge resample is reported instead of stalling the editor.
    std::uint64_t maximumWork{std::uint64_t{1} << 26U};
    // Tolerance (in primary voxels, and for rotation-matrix entries) for treating an operand as
    // grid aligned.
    float alignmentTolerance{1.0e-3F};
    // Also return the primary-grid cells where the operand overlaps solid primary voxels, for
    // preview highlighting. Capped by maximumOverlapCells.
    bool collectOverlapCells{};
    std::size_t maximumOverlapCells{65536};
};

enum class VoxelBooleanSeverity : std::uint8_t { Info, Warning, Error };

enum class VoxelBooleanDiagnosticCode : std::uint8_t {
    InvalidPrimary,     // missing voxels, bad voxel size or non-finite transform
    InvalidOperand,     // same, for an operand, or the operand is the primary itself
    EmptyOperand,       // operand has no voxels; it is ignored
    ResampledOperand,   // operand grid is not aligned with the primary grid
    ExcessiveCost,      // estimated work exceeds options.maximumWork
    NoOverlap,          // no operand voxel overlaps a solid primary voxel
    EmptyResult,        // the result would have no voxels
    NoChange,           // the result equals the primary object
};

struct VoxelBooleanDiagnostic {
    VoxelBooleanSeverity severity{VoxelBooleanSeverity::Info};
    VoxelBooleanDiagnosticCode code{VoxelBooleanDiagnosticCode::NoChange};
    std::size_t operandIndex{std::numeric_limits<std::size_t>::max()};
    std::string message;
};

struct VoxelBooleanStats {
    std::uint64_t primaryVoxels{};
    std::uint64_t operandVoxels{};      // sum over (valid) operands, in their own grids
    std::uint64_t overlapVoxels{};      // primary voxels covered by at least one operand
    std::uint64_t resultVoxels{};
    std::uint64_t addedVoxels{};
    std::uint64_t removedVoxels{};
    std::uint64_t recoloredVoxels{};
    std::uint64_t estimatedWork{};
    std::size_t alignedOperands{};
    std::size_t resampledOperands{};
    std::size_t anchorsKept{};
    std::size_t anchorsDropped{};
    std::size_t anchorsTransferred{};
};

struct VoxelBooleanChange {
    Int3 voxel{};
    MaterialId before{kAirMaterial};
    MaterialId after{kAirMaterial};
    auto operator<=>(const VoxelBooleanChange&) const = default;
};

struct VoxelBooleanResult {
    VoxelBooleanOperation operation{VoxelBooleanOperation::Union};
    // False when the inputs were rejected (invalid input or excessive cost); changes are empty.
    bool computed{};
    std::vector<VoxelBooleanChange> changes;   // sorted by voxel, primary grid
    std::vector<Int3> anchors;                 // complete resulting anchor set, sorted
    std::vector<Int3> overlapCells;            // see VoxelBooleanOptions::collectOverlapCells
    VoxelBooleanStats stats;
    std::vector<VoxelBooleanDiagnostic> diagnostics;

    [[nodiscard]] bool has_errors() const noexcept;
    // True when the result may be committed: computed, no error diagnostics (an empty result or
    // a result identical to the primary is an error), and at least one voxel or anchor changes.
    [[nodiscard]] bool can_commit() const noexcept;
    // First error message, or an empty string.
    [[nodiscard]] std::string blocking_reason() const;
};

[[nodiscard]] VoxelBooleanResult compute_voxel_boolean(
    const VoxelBooleanVolume& primary,
    std::span<const VoxelBooleanVolume> operands,
    const VoxelBooleanOptions& options = {});

// Applies (forward) or reverts (!forward) a change list to a voxel object. Returns false and
// leaves the object untouched when a voxel no longer holds the expected material.
bool apply_voxel_boolean_changes(VoxelObject& target, std::span<const VoxelBooleanChange> changes,
                                 bool forward = true);

} // namespace dve
