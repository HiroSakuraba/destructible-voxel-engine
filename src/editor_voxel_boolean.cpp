#include "dve/editor_voxel_boolean.hpp"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <memory>
#include <utility>

#include "dve/editor_workspace.hpp"
#include "dve/fragment.hpp"

namespace dve::editor {
namespace {

std::string quoted(const EditorObject& object) {
    return "'" + (object.name.empty() ? "Object " + std::to_string(object.id) : object.name) + "'";
}

// Empty string when `object` can take part in a Boolean, otherwise why not.
std::string voxel_participant_problem(const EditorObject& object) {
    if (object.text3d) return quoted(object) + " is a 3D text object";
    if (object.gaborVolume) return quoted(object) + " is a Gabor volume";
    if (!object.voxels || object.voxels->occupied_voxel_count() == 0) return quoted(object) + " has no voxels";
    if (!(object.voxelSizeMeters > 0.0F)) return quoted(object) + " has an invalid voxel size";
    return {};
}

bool is_descendant_of(const EditorDocument& document, EditorObjectId id, EditorObjectId ancestor) {
    const EditorObject* object = document.find_object(id);
    for (std::size_t guard = 0; object && object->parent && guard < 4096; ++guard) {
        if (*object->parent == ancestor) return true;
        object = document.find_object(*object->parent);
    }
    return false;
}

std::string format_size(float meters) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.3g m", static_cast<double>(meters));
    return buffer;
}

struct Hasher {
    std::uint64_t h{0x9E3779B97F4A7C15ULL};
    void u64(std::uint64_t v) noexcept {
        h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6U) + (h >> 2U);
        h *= 0xBF58476D1CE4E5B9ULL;
        h ^= h >> 31U;
    }
    void f32(float v) noexcept { u64(std::bit_cast<std::uint32_t>(v)); }
};

} // namespace

std::string_view voxel_boolean_operand_policy_name(VoxelBooleanOperandPolicy policy) noexcept {
    switch (policy) {
    case VoxelBooleanOperandPolicy::Hide: return "Hide";
    case VoxelBooleanOperandPolicy::Delete: return "Delete";
    case VoxelBooleanOperandPolicy::Keep: return "Keep";
    }
    return "Hide";
}

VoxelBooleanSelection evaluate_voxel_boolean_selection(const EditorDocument& document,
                                                       const std::set<EditorObjectId>& selection,
                                                       std::optional<EditorObjectId> primary,
                                                       VoxelBooleanOperandPolicy policy) {
    VoxelBooleanSelection result;
    if (selection.size() < 2U) {
        result.reason = selection.empty()
            ? "Select two or more voxel objects: the active object is the target (A), the others are operands (B). "
              "Ctrl+click objects in the viewport or Scene Hierarchy."
            : "Only one object is selected. Ctrl+click a second voxel object to use as the operand (B); "
              "the active object stays the target (A).";
        return result;
    }
    result.target = primary && selection.contains(*primary) ? *primary : *selection.begin();
    for (const EditorObjectId id : selection) {
        const EditorObject* object = document.find_object(id);
        if (!object) {
            result.reason = "A selected object no longer exists. Reselect the voxel objects to combine.";
            return result;
        }
        if (const std::string problem = voxel_participant_problem(*object); !problem.empty()) {
            result.reason = problem + "; Booleans combine voxel objects only. Ctrl+click it to deselect it.";
            return result;
        }
        if (id != result.target) result.operands.push_back(id);
    }
    const EditorObject* target = document.find_object(result.target);
    if (target->flags.locked) {
        result.reason = "Target " + quoted(*target) + " is locked. Turn off Locked in the Inspector first.";
        return result;
    }
    if (policy == VoxelBooleanOperandPolicy::Delete) {
        for (const EditorObjectId id : result.operands) {
            const EditorObject* operand = document.find_object(id);
            if (operand->flags.locked) {
                result.reason = "Operand " + quoted(*operand) +
                                " is locked and operands are set to Delete. Unlock it, or set operands to Hide or Keep.";
                return result;
            }
            if (is_descendant_of(document, result.target, id)) {
                result.reason = "Target " + quoted(*target) + " is a child of operand " + quoted(*operand) +
                                "; Delete would remove it. Set operands to Hide or Keep, or detach the target.";
                return result;
            }
            if (!document.children_of(id).empty()) {
                result.reason = "Operand " + quoted(*operand) +
                                " has child objects that Delete would also remove. Set operands to Hide or Keep, "
                                "or detach the children.";
                return result;
            }
        }
    }
    result.valid = true;
    return result;
}

ApplyVoxelBooleanCommand::ApplyVoxelBooleanCommand(EditorObjectId target, std::vector<VoxelBooleanChange> changes,
                                                   std::vector<Int3> anchorsBefore, std::vector<Int3> anchorsAfter,
                                                   std::string label)
    : target_(target), changes_(std::move(changes)), anchorsBefore_(std::move(anchorsBefore)),
      anchorsAfter_(std::move(anchorsAfter)), label_(std::move(label)) {}

CommandResult ApplyVoxelBooleanCommand::apply(EditorDocument& document, bool forward) {
    EditorObject* object = document.find_object(target_);
    if (!object || !object->voxels) return CommandResult::fail("Boolean target no longer exists");
    if (object->flags.locked) return CommandResult::fail("Boolean target is locked");
    const std::vector<Int3>& expectedAnchors = forward ? anchorsBefore_ : anchorsAfter_;
    if (!std::equal(object->anchors.begin(), object->anchors.end(), expectedAnchors.begin(), expectedAnchors.end()))
        return CommandResult::fail("Boolean target anchors changed since the operation was computed");
    if (!apply_voxel_boolean_changes(*object->voxels, changes_, forward))
        return CommandResult::fail("Boolean target voxels changed since the operation was computed");
    const std::vector<Int3>& anchors = forward ? anchorsAfter_ : anchorsBefore_;
    object->anchors = std::set<Int3>(anchors.begin(), anchors.end());
    document.mark_dirty();
    return CommandResult::ok();
}
CommandResult ApplyVoxelBooleanCommand::execute(EditorDocument& document) { return apply(document, true); }
CommandResult ApplyVoxelBooleanCommand::undo(EditorDocument& document) { return apply(document, false); }

bool EditorVoxelBooleanSession::begin(const EditorDocument& document, const VoxelBooleanSelection& selection,
                                      VoxelBooleanOperation operation, VoxelBooleanOperandPolicy policy,
                                      std::string* error) {
    if (!selection.valid) {
        if (error) *error = selection.reason;
        return false;
    }
    active_ = true;
    target_ = selection.target;
    operands_ = selection.operands;
    operation_ = operation;
    policy_ = policy;
    recompute(document);
    return true;
}

void EditorVoxelBooleanSession::set_operation(const EditorDocument& document, VoxelBooleanOperation operation) {
    if (!active_ || operation == operation_) return;
    operation_ = operation;
    recompute(document);
}

void EditorVoxelBooleanSession::set_overlap_material(const EditorDocument& document,
                                                     VoxelBooleanOverlapMaterial material) {
    if (overlapMaterial_ == material) return;
    overlapMaterial_ = material;
    if (active_) recompute(document);
}

std::uint64_t EditorVoxelBooleanSession::participants_fingerprint(const EditorDocument& document) const noexcept {
    Hasher hash;
    const auto add = [&](EditorObjectId id) {
        hash.u64(id);
        const EditorObject* object = document.find_object(id);
        if (!object) { hash.u64(0xDEADU); return; }
        hash.u64(object->voxels ? object->voxels->revision() : 0U);
        hash.f32(object->voxelSizeMeters);
        hash.f32(object->transform.position.x); hash.f32(object->transform.position.y); hash.f32(object->transform.position.z);
        hash.f32(object->transform.rotation.x); hash.f32(object->transform.rotation.y);
        hash.f32(object->transform.rotation.z); hash.f32(object->transform.rotation.w);
        hash.u64(object->anchors.size());
        for (const Int3 anchor : object->anchors) {
            hash.u64(static_cast<std::uint32_t>(anchor.x)); hash.u64(static_cast<std::uint32_t>(anchor.y));
            hash.u64(static_cast<std::uint32_t>(anchor.z));
        }
        hash.u64(object->flags.locked ? 1U : 0U);
    };
    add(target_);
    for (const EditorObjectId id : operands_) add(id);
    return hash.h;
}

void EditorVoxelBooleanSession::recompute(const EditorDocument& document) {
    result_ = {};
    fingerprint_ = participants_fingerprint(document);
    const EditorObject* target = document.find_object(target_);
    if (!target) return;
    std::vector<Int3> targetAnchors(target->anchors.begin(), target->anchors.end());
    std::vector<std::vector<Int3>> operandAnchors;
    operandAnchors.reserve(operands_.size());
    std::vector<VoxelBooleanVolume> volumes;
    for (const EditorObjectId id : operands_) {
        const EditorObject* operand = document.find_object(id);
        if (!operand) return;
        operandAnchors.emplace_back(operand->anchors.begin(), operand->anchors.end());
        volumes.push_back({operand->voxels.get(), operand->transform, operand->voxelSizeMeters,
                           operandAnchors.back(), operand->name});
    }
    VoxelBooleanOptions options;
    options.operation = operation_;
    options.overlapMaterial = overlapMaterial_;
    options.collectOverlapCells = true;
    options.maximumOverlapCells = 32768;
    result_ = compute_voxel_boolean({target->voxels.get(), target->transform, target->voxelSizeMeters,
                                     targetAnchors, target->name},
                                    volumes, options);
}

bool EditorVoxelBooleanSession::refresh(const EditorDocument& document) {
    if (!active_) return false;
    for (EditorObjectId id : operands_) {
        const EditorObject* object = document.find_object(id);
        if (!object || !voxel_participant_problem(*object).empty()) { cancel(); return false; }
    }
    const EditorObject* target = document.find_object(target_);
    if (!target || target->text3d || target->gaborVolume || !target->voxels) { cancel(); return false; }
    if (participants_fingerprint(document) != fingerprint_) recompute(document);
    return true;
}

std::string EditorVoxelBooleanSession::commit_blocked_reason() const {
    if (!active_) return "No Boolean preview is open. Select voxel objects and choose Tools > Voxel > Boolean.";
    return result_.can_commit() ? std::string{} : result_.blocking_reason();
}

CommandResult EditorVoxelBooleanSession::commit(EditorWorkspace& workspace) {
    if (!active_) return CommandResult::fail(commit_blocked_reason());
    EditorDocument& document = workspace.document();
    if (!refresh(document)) return CommandResult::fail("A Boolean participant was removed; the preview was closed.");
    // Re-validate against the live document (locks or hierarchy may have changed while previewing).
    std::set<EditorObjectId> participants(operands_.begin(), operands_.end());
    participants.insert(target_);
    const VoxelBooleanSelection selection = evaluate_voxel_boolean_selection(document, participants, target_, policy_);
    if (!selection.valid) return CommandResult::fail(selection.reason);
    if (!result_.can_commit()) return CommandResult::fail(result_.blocking_reason());

    const EditorObject* target = document.find_object(target_);
    std::vector<Int3> anchorsBefore(target->anchors.begin(), target->anchors.end());
    std::string label = "Boolean " + std::string(voxel_boolean_operation_name(operation_)) + " (" +
                        (target->name.empty() ? "target" : target->name) + ")";
    auto compound = std::make_unique<CompoundCommand>(label);
    compound->add(std::make_unique<ApplyVoxelBooleanCommand>(target_, result_.changes, std::move(anchorsBefore),
                                                             result_.anchors, label));
    if (policy_ == VoxelBooleanOperandPolicy::Hide) {
        std::vector<ObjectFlagsChange> flags;
        for (const EditorObjectId id : operands_) {
            const EditorObject* operand = document.find_object(id);
            if (!operand->flags.visible) continue;
            EditorObjectFlags after = operand->flags;
            after.visible = false;
            flags.push_back({id, operand->flags, after});
        }
        if (!flags.empty()) compound->add(std::make_unique<SetObjectFlagsBatchCommand>(std::move(flags)));
    } else if (policy_ == VoxelBooleanOperandPolicy::Delete) {
        compound->add(std::make_unique<RemoveObjectsCommand>(operands_, label));
    }
    const CommandResult executed = workspace.commands().execute(document, std::move(compound));
    if (!executed.success) return executed;

    VoxelBooleanCommitReport report;
    report.target = target_;
    report.operands = operands_;
    report.operation = operation_;
    report.policy = policy_;
    report.stats = result_.stats;
    // Rebuild the target's collision proxy now so its cost is known at commit time; play and
    // simulate sessions build the same merged box proxy from the committed voxels.
    if (const EditorObject* committed = document.find_object(target_); committed && committed->voxels) {
        report.collisionEnabled = committed->flags.collisionEnabled;
        if (report.collisionEnabled) report.collisionBoxes = build_merged_object_box_proxy(*committed->voxels).size();
    }
    lastCommit_ = std::move(report);
    workspace.select_object(target_);
    cancel();
    return executed;
}

void EditorVoxelBooleanSession::cancel() noexcept {
    active_ = false;
    operands_.clear();
    target_ = 0;
    result_ = {};
    fingerprint_ = 0;
}

std::vector<std::string> EditorVoxelBooleanSession::describe(const EditorDocument& document) const {
    std::vector<std::string> lines;
    if (!active_) return lines;
    const VoxelBooleanStats& stats = result_.stats;
    lines.push_back("Boolean " + std::string(voxel_boolean_operation_name(operation_)) + "   " +
                    std::string(voxel_boolean_operation_symbol(operation_)));
    const auto object_line = [&](std::string prefix, EditorObjectId id) {
        const EditorObject* object = document.find_object(id);
        if (!object) return prefix + "(missing)";
        return prefix + quoted(*object) + "  " + std::to_string(object->voxels ? object->voxels->occupied_voxel_count() : 0U) +
               " voxels @ " + format_size(object->voxelSizeMeters);
    };
    lines.push_back(object_line("A target:  ", target_));
    const std::size_t shown = std::min<std::size_t>(operands_.size(), 3U);
    for (std::size_t i = 0; i < shown; ++i) {
        std::string line = object_line(operands_.size() == 1 ? "B operand: " : "B" + std::to_string(i + 1) + " operand: ",
                                       operands_[i]);
        for (const VoxelBooleanDiagnostic& d : result_.diagnostics)
            if (d.code == VoxelBooleanDiagnosticCode::ResampledOperand && d.operandIndex == i) line += "  (resampled)";
        lines.push_back(std::move(line));
    }
    if (operands_.size() > shown) lines.push_back("... and " + std::to_string(operands_.size() - shown) + " more operands");
    if (result_.computed) {
        lines.push_back("Overlap " + std::to_string(stats.overlapVoxels) + "   Result " + std::to_string(stats.resultVoxels) +
                        " voxels   +" + std::to_string(stats.addedVoxels) + " / -" + std::to_string(stats.removedVoxels) +
                        (stats.recoloredVoxels ? " / repaint " + std::to_string(stats.recoloredVoxels) : std::string{}));
        lines.push_back("Anchors kept " + std::to_string(stats.anchorsKept) + ", dropped " +
                        std::to_string(stats.anchorsDropped) + ", transferred " + std::to_string(stats.anchorsTransferred));
    }
    lines.push_back("Overlap material: " + std::string(overlapMaterial_ == VoxelBooleanOverlapMaterial::KeepPrimary
                                                            ? "keep A" : "take B") +
                    "   Operands after commit: " + std::string(voxel_boolean_operand_policy_name(policy_)));
    for (const VoxelBooleanDiagnostic& d : result_.diagnostics) {
        lines.push_back((d.severity == VoxelBooleanSeverity::Error ? "Cannot commit: "
                         : d.severity == VoxelBooleanSeverity::Warning ? "Warning: " : "Note: ") + d.message);
    }
    lines.push_back(result_.can_commit() ? "Enter commit   Esc cancel   1/2/3 operation   S swap A/B   O operands   M material"
                                         : "Esc cancel   1/2/3 operation   S swap A/B   O operands   M material");
    return lines;
}

} // namespace dve::editor
