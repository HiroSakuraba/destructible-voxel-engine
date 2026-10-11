#include "dve/editor_voxel_join.hpp"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <map>
#include <memory>
#include <unordered_map>

#include "dve/editor_voxel_boolean.hpp"
#include "dve/editor_workspace.hpp"

namespace dve::editor {
namespace {

struct Int3Hash {
    std::size_t operator()(const Int3& v) const noexcept {
        std::uint64_t h = static_cast<std::uint32_t>(v.x);
        h = h * 0x9E3779B97F4A7C15ULL ^ static_cast<std::uint32_t>(v.y);
        h = h * 0xBF58476D1CE4E5B9ULL ^ static_cast<std::uint32_t>(v.z);
        return static_cast<std::size_t>(h ^ (h >> 29U));
    }
};

std::string quoted(const EditorObject& object) {
    return "'" + (object.name.empty() ? "object " + std::to_string(object.id) : object.name) + "'";
}

std::string voxel_problem(const EditorObject& object) {
    if (object.text3d) return quoted(object) + " is a 3D text object";
    if (object.gaborVolume) return quoted(object) + " is a Gabor volume";
    if (!object.voxels || object.voxels->occupied_voxel_count() == 0) return quoted(object) + " has no voxels";
    if (!(object.voxelSizeMeters > 0.0F)) return quoted(object) + " has an invalid voxel size";
    return {};
}

bool has_owned_components(const EditorObject& object) {
    // Tags, Layer and Groups live in the membership component; everything else (scripts, audio,
    // ...) is data a removed object would lose.
    return std::any_of(object.components.begin(), object.components.end(),
                       [](const Component& c) { return c.type != "dve.membership"; });
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

} // namespace

// --- Separate Islands --------------------------------------------------------------------------

std::vector<std::vector<SparseVoxelStateEntry>> find_voxel_islands(const VoxelObject& voxels) {
    std::vector<SparseVoxelStateEntry> cells;
    cells.reserve(static_cast<std::size_t>(voxels.occupied_voxel_count()));
    for (const auto& [key, brick] : voxels.bricks())
        brick.occupancy().for_each_set([&](std::uint16_t index) {
            cells.push_back({global_from_local(key, local_from_index_unchecked(index)), brick.material(index)});
        });
    std::sort(cells.begin(), cells.end(),
              [](const SparseVoxelStateEntry& a, const SparseVoxelStateEntry& b) { return a.voxel < b.voxel; });
    std::unordered_map<Int3, std::size_t, Int3Hash> indexOf;
    indexOf.reserve(cells.size());
    for (std::size_t i = 0; i < cells.size(); ++i) indexOf.emplace(cells[i].voxel, i);
    std::vector<bool> visited(cells.size(), false);
    std::vector<std::vector<SparseVoxelStateEntry>> islands;
    std::vector<std::size_t> stack;
    static constexpr Int3 kSteps[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    // Seeds in sorted order, so piece order and contents are deterministic.
    for (std::size_t seed = 0; seed < cells.size(); ++seed) {
        if (visited[seed]) continue;
        std::vector<SparseVoxelStateEntry> island;
        visited[seed] = true;
        stack.assign(1, seed);
        while (!stack.empty()) {
            const std::size_t at = stack.back();
            stack.pop_back();
            island.push_back(cells[at]);
            for (const Int3 step : kSteps) {
                const Int3 next{cells[at].voxel.x + step.x, cells[at].voxel.y + step.y, cells[at].voxel.z + step.z};
                const auto found = indexOf.find(next);
                if (found == indexOf.end() || visited[found->second]) continue;
                visited[found->second] = true;
                stack.push_back(found->second);
            }
        }
        std::sort(island.begin(), island.end(),
                  [](const SparseVoxelStateEntry& a, const SparseVoxelStateEntry& b) { return a.voxel < b.voxel; });
        islands.push_back(std::move(island));
    }
    // Largest first; equal sizes keep discovery order (smallest first voxel first).
    std::stable_sort(islands.begin(), islands.end(), [](const auto& a, const auto& b) { return a.size() > b.size(); });
    return islands;
}

std::string separate_islands_problem(const EditorDocument& document, EditorObjectId id) {
    const EditorObject* object = document.find_object(id);
    if (!object) return "the object no longer exists";
    if (std::string problem = voxel_problem(*object); !problem.empty()) return problem;
    if (object->flags.locked) return quoted(*object) + " is locked";
    // Same rules as Slice's Separate (#97): a prefab instance's pieces would silently diverge from
    // the prefab, and an attached object's pieces would all hang off the same socket.
    if (object->prefabLink) return quoted(*object) + " is a prefab instance; unpack it first";
    if (object->attachment) return quoted(*object) + " is attached to a socket; detach it first";
    if (object->voxels->occupied_voxel_count() > kMaxSeparateVoxels)
        return quoted(*object) + " has more than " + std::to_string(kMaxSeparateVoxels) + " voxels";
    return {};
}

SeparateIslandsBuild build_separate_islands_command(EditorDocument& document, const std::vector<EditorObjectId>& ids) {
    SeparateIslandsBuild build;
    auto command = std::make_unique<CompoundCommand>("Separate islands");
    std::size_t added = 0;
    for (const EditorObjectId id : ids) {
        if (std::string problem = separate_islands_problem(document, id); !problem.empty()) {
            build.skipped.push_back(problem);
            continue;
        }
        const EditorObject& source = *document.find_object(id);
        const auto islands = find_voxel_islands(*source.voxels);
        if (islands.size() < 2) {
            build.skipped.push_back(quoted(source) + " is already one piece");
            continue;
        }
        if (islands.size() > kMaxSeparateIslands) {
            build.skipped.push_back(quoted(source) + " has " + std::to_string(islands.size()) +
                                    " separate pieces; Separate Islands makes at most " +
                                    std::to_string(kMaxSeparateIslands) + ". Clean up stray voxels first");
            continue;
        }
        // The largest island stays in the original; every other island leaves it.
        std::vector<VoxelBooleanChange> changes;
        std::vector<Int3> anchorsBefore(source.anchors.begin(), source.anchors.end());
        std::vector<Int3> anchorsAfter;
        for (const auto& cell : islands.front())
            if (source.anchors.contains(cell.voxel)) anchorsAfter.push_back(cell.voxel);
        for (std::size_t piece = 1; piece < islands.size(); ++piece) {
            EditorObject part = clone_editor_object(source);
            part.id = document.allocate_object_id();
            part.name = source.name + " (piece " + std::to_string(piece + 1) + ")";
            part.voxels = std::make_unique<VoxelObject>(part.id);
            part.anchors.clear();
            // Geometry no longer matches an import source or a prefab; the original keeps those,
            // its components and its children. Pieces keep flags, tags, groups and layer.
            part.sourceAsset.clear();
            part.importRecipe.clear();
            part.prefabLink.reset();
            part.attachment.reset();
            std::erase_if(part.components, [](const Component& c) { return c.type != "dve.membership" && c.type != "dve.tags"; });
            for (const auto& cell : islands[piece]) {
                (void)part.voxels->set_voxel(cell.voxel, cell.material);
                changes.push_back({cell.voxel, cell.material, kAirMaterial});
                if (source.anchors.contains(cell.voxel)) part.anchors.insert(cell.voxel);
            }
            build.newObjects.push_back(part.id);
            build.newObjectSources.push_back(id);
            command->add(std::make_unique<AddObjectCommand>(std::move(part), "Add separated piece"));
            ++added;
        }
        std::sort(changes.begin(), changes.end());
        // Remove the pieces from the original before the new objects appear (order inside one
        // compound only matters for validation; both happen in the same undo step).
        command->add(std::make_unique<ApplyVoxelBooleanCommand>(id, std::move(changes), std::move(anchorsBefore),
                                                                std::move(anchorsAfter), "Remove separated pieces"));
        ++build.separatedObjects;
    }
    if (added > 0) build.command = std::move(command);
    return build;
}

// --- Join --------------------------------------------------------------------------------------

std::string EditorVoxelJoinSession::selection_problem(const EditorDocument& document,
                                                      const std::set<EditorObjectId>& selection,
                                                      std::optional<EditorObjectId> primary) {
    if (selection.size() < 2U)
        return "Select two or more voxel objects to join; the active (last clicked) one keeps its name and grid.";
    const EditorObjectId target = primary && selection.contains(*primary) ? *primary : *selection.begin();
    for (const EditorObjectId id : selection) {
        const EditorObject* object = document.find_object(id);
        if (!object) return "A selected object no longer exists.";
        if (std::string problem = voxel_problem(*object); !problem.empty())
            return problem + "; Join merges voxel objects only. Deselect it, or use Group.";
        if (object->flags.locked) return quoted(*object) + " is locked. Turn off Locked first.";
        if (!object->flags.visible) return quoted(*object) + " is hidden. Show it first, or deselect it.";
        if (id != target) {
            // The operands are removed: refuse what that would silently throw away.
            if (has_owned_components(*object))
                return quoted(*object) + " has components that Join would delete. Click it last to make it the "
                                         "target, or move its components first.";
            if (object->prefabLink)
                return quoted(*object) + " is a prefab instance that Join would delete. Click it last to make it the target, "
                                         "or unpack it first.";
            for (const EditorObjectId child : document.children_of(id)) {
                const EditorObject* moved = document.find_object(child);
                if (moved && !selection.contains(child) && moved->flags.locked)
                    return quoted(*object) + " has a locked child (" + quoted(*moved) +
                           ") that Join would move under the target. Unlock it first.";
            }
        }
        if (id != target && is_descendant_of(document, target, id))
            return "The target is inside " + quoted(*object) + ", which Join would remove. Make that object the target "
                   "(Tab in the preview) or detach the target first.";
    }
    return {};
}

bool EditorVoxelJoinSession::begin(const EditorDocument& document, const std::set<EditorObjectId>& selection,
                                   std::optional<EditorObjectId> primary, std::string* error) {
    cancel();
    if (std::string problem = selection_problem(document, selection, primary); !problem.empty()) {
        if (error) *error = problem;
        return false;
    }
    target_ = primary && selection.contains(*primary) ? *primary : *selection.begin();
    for (const EditorObjectId id : selection)
        if (id != target_) operands_.push_back({id, 0, {}});
    active_ = true;
    recompute(document);
    return true;
}

std::vector<EditorObjectId> EditorVoxelJoinSession::participants() const {
    std::vector<EditorObjectId> ids{target_};
    for (const VoxelJoinOperand& operand : operands_) ids.push_back(operand.id);
    std::sort(ids.begin(), ids.end());
    return ids;
}

bool EditorVoxelJoinSession::has_mismatch() const noexcept {
    return std::any_of(operands_.begin(), operands_.end(), [](const VoxelJoinOperand& o) { return !o.mismatch.empty(); });
}

void EditorVoxelJoinSession::cycle_target(const EditorDocument& document) {
    if (!active_) return;
    const std::vector<EditorObjectId> ids = participants();
    const auto at = std::find(ids.begin(), ids.end(), target_);
    const EditorObjectId next = (at == ids.end() || std::next(at) == ids.end()) ? ids.front() : *std::next(at);
    operands_.clear();
    for (const EditorObjectId id : ids)
        if (id != next) operands_.push_back({id, 0, {}});
    target_ = next;
    resampleChosen_ = false;  // a different target can change which objects mismatch: ask again
    recompute(document);
}

std::uint64_t EditorVoxelJoinSession::fingerprint(const EditorDocument& document) const noexcept {
    std::uint64_t h = 0x9E3779B97F4A7C15ULL;
    const auto mix = [&](std::uint64_t v) {
        h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6U) + (h >> 2U);
        h *= 0xBF58476D1CE4E5B9ULL;
    };
    for (const EditorObjectId id : participants()) {
        mix(id);
        const EditorObject* object = document.find_object(id);
        if (!object) { mix(0xDEAD); continue; }
        mix(object->voxels ? object->voxels->revision() : 0U);
        mix(std::bit_cast<std::uint32_t>(object->voxelSizeMeters));
        for (const float f : {object->transform.position.x, object->transform.position.y, object->transform.position.z,
                              object->transform.rotation.x, object->transform.rotation.y, object->transform.rotation.z,
                              object->transform.rotation.w})
            mix(std::bit_cast<std::uint32_t>(f));
        mix(object->anchors.size());
    }
    return h;
}

void EditorVoxelJoinSession::recompute(const EditorDocument& document) {
    result_ = {};
    fingerprint_ = fingerprint(document);
    const EditorObject* target = document.find_object(target_);
    if (!target || !target->voxels) return;
    std::vector<Int3> targetAnchors(target->anchors.begin(), target->anchors.end());
    std::vector<std::vector<Int3>> operandAnchors;
    operandAnchors.reserve(operands_.size());
    std::vector<VoxelBooleanVolume> volumes;
    for (VoxelJoinOperand& operand : operands_) {
        const EditorObject* object = document.find_object(operand.id);
        if (!object || !object->voxels) return;
        operand.voxels = object->voxels->occupied_voxel_count();
        operand.mismatch.clear();
        operandAnchors.emplace_back(object->anchors.begin(), object->anchors.end());
        volumes.push_back({object->voxels.get(), object->transform, object->voxelSizeMeters, operandAnchors.back(),
                           object->name});
    }
    VoxelBooleanOptions options;
    options.operation = VoxelBooleanOperation::Union;
    options.overlapMaterial = VoxelBooleanOverlapMaterial::KeepPrimary;
    result_ = compute_voxel_boolean({target->voxels.get(), target->transform, target->voxelSizeMeters, targetAnchors,
                                     target->name},
                                    volumes, options);
    for (const VoxelBooleanDiagnostic& diagnostic : result_.diagnostics) {
        if (diagnostic.code != VoxelBooleanDiagnosticCode::ResampledOperand || diagnostic.operandIndex >= operands_.size())
            continue;
        const EditorObject* object = document.find_object(operands_[diagnostic.operandIndex].id);
        std::string reason;
        if (object && object->voxelSizeMeters != target->voxelSizeMeters)
            reason = "voxel size " + format_size(object->voxelSizeMeters) + " vs " + format_size(target->voxelSizeMeters) +
                     (object->voxelSizeMeters < target->voxelSizeMeters ? " (finer: thin detail may be lost)"
                                                                        : " (coarser: blocks are re-gridded)");
        else
            reason = "turned or offset off the target's grid (edges will be re-gridded)";
        operands_[diagnostic.operandIndex].mismatch = reason;
    }
}

bool EditorVoxelJoinSession::refresh(const EditorDocument& document) {
    if (!active_) return false;
    for (const EditorObjectId id : participants()) {
        const EditorObject* object = document.find_object(id);
        if (!object || !voxel_problem(*object).empty()) { cancel(); return false; }
    }
    if (fingerprint(document) != fingerprint_) recompute(document);
    return true;
}

std::string EditorVoxelJoinSession::blocked_reason(const EditorDocument& document) const {
    if (!active_) return "No Join preview is open.";
    std::set<EditorObjectId> selection;
    for (const EditorObjectId id : participants()) selection.insert(id);
    if (std::string problem = selection_problem(document, selection, target_); !problem.empty()) return problem;
    if (needs_choice()) return "Some objects are off the target's grid: press R to resample them, G to group instead, "
                               "or Tab for another target.";
    if (!result_.computed || result_.has_errors()) {
        // Operands entirely inside the target still join (they are removed); only real errors block.
        for (const VoxelBooleanDiagnostic& d : result_.diagnostics)
            if (d.severity == VoxelBooleanSeverity::Error && d.code != VoxelBooleanDiagnosticCode::NoChange) return d.message;
        if (!result_.computed) return result_.blocking_reason();
    }
    return {};
}

std::vector<std::string> EditorVoxelJoinSession::describe(const EditorDocument& document) const {
    std::vector<std::string> lines;
    if (!active_) return lines;
    const EditorObject* target = document.find_object(target_);
    lines.push_back("Join " + std::to_string(operands_.size() + 1) + " voxel objects into one (preview, scene unchanged)");
    if (target)
        lines.push_back("Target: " + quoted(*target) + "  " + std::to_string(target->voxels->occupied_voxel_count()) +
                        " voxels @ " + format_size(target->voxelSizeMeters) + "  (keeps its name, grid and place)");
    for (const VoxelJoinOperand& operand : operands_) {
        const EditorObject* object = document.find_object(operand.id);
        if (!object) continue;
        lines.push_back((operand.mismatch.empty() ? "  + " : "  ! ") + quoted(*object) + "  " +
                        std::to_string(operand.voxels) + " voxels" +
                        (operand.mismatch.empty() ? "  (on the grid: exact)" : "  " + operand.mismatch));
    }
    const VoxelBooleanStats& stats = result_.stats;
    lines.push_back("Result: " + std::to_string(stats.resultVoxels) + " voxels (" + std::to_string(stats.addedVoxels) +
                    " added, " + std::to_string(stats.overlapVoxels) + " overlapping keep the target's material)");
    std::size_t children = 0;
    const std::vector<EditorObjectId> all = participants();
    for (const VoxelJoinOperand& operand : operands_)
        for (const EditorObjectId child : document.children_of(operand.id))
            if (std::find(all.begin(), all.end(), child) == all.end()) ++children;
    if (children > 0) lines.push_back(std::to_string(children) + " child object(s) of the joined objects move under the target");
    // The result is one object with the target's flags: say so when an operand's differ.
    if (target) {
        std::size_t differing = 0;
        for (const VoxelJoinOperand& operand : operands_) {
            const EditorObject* object = document.find_object(operand.id);
            if (object && (object->flags.collisionEnabled != target->flags.collisionEnabled ||
                           object->flags.structural != target->flags.structural ||
                           object->flags.anchored != target->flags.anchored ||
                           object->flags.decorative != target->flags.decorative || object->layer != target->layer))
                ++differing;
        }
        if (differing > 0)
            lines.push_back(std::to_string(differing) + " object(s) differ in collision/structural/anchored/decorative/layer; "
                            "the target's settings apply to the result");
    }
    if (has_mismatch())
        lines.push_back(resampleChosen_ ? "Resampling into the target's grid (chosen)"
                                        : "Off-grid objects: R resample   G group instead   Tab next target");
    if (const std::string reason = blocked_reason(document); !reason.empty() && !needs_choice())
        lines.push_back("Cannot commit: " + reason);
    lines.push_back("Enter join   Esc cancel   Tab next target");
    return lines;
}

CommandResult EditorVoxelJoinSession::commit(EditorWorkspace& workspace) {
    EditorDocument& document = workspace.document();
    if (!refresh(document)) return CommandResult::fail("A Join participant was removed; the preview was closed.");
    if (std::string reason = blocked_reason(document); !reason.empty()) return CommandResult::fail(reason);
    const EditorObject* target = document.find_object(target_);
    const std::vector<EditorObjectId> all = participants();
    const std::string label = "Join into " + quoted(*target);
    auto compound = std::make_unique<CompoundCommand>(label);
    std::vector<Int3> anchorsBefore(target->anchors.begin(), target->anchors.end());
    if (!result_.changes.empty() || anchorsBefore != result_.anchors)
        compound->add(std::make_unique<ApplyVoxelBooleanCommand>(target_, result_.changes, std::move(anchorsBefore),
                                                                 result_.anchors, label));
    // Children of the joined objects (that are not joined themselves) move under the target.
    std::vector<ObjectReparentChange> moves;
    std::vector<EditorObjectId> removed;
    for (const VoxelJoinOperand& operand : operands_) {
        // Remove only the outermost joined objects; joined descendants go with them.
        bool insideJoined = false;
        for (const VoxelJoinOperand& other : operands_)
            insideJoined = insideJoined || (other.id != operand.id && is_descendant_of(document, operand.id, other.id));
        if (!insideJoined) removed.push_back(operand.id);
        for (const EditorObjectId child : document.children_of(operand.id))
            if (std::find(all.begin(), all.end(), child) == all.end()) moves.push_back({child, operand.id, target_});
    }
    if (!moves.empty()) compound->add(std::make_unique<ReparentObjectsCommand>(std::move(moves), "Move children to the join target"));
    compound->add(std::make_unique<RemoveObjectsCommand>(std::move(removed), "Remove joined objects"));
    const CommandResult executed = workspace.commands().execute(document, std::move(compound));
    if (!executed.success) return executed;
    workspace.select_object(target_);
    cancel();
    return executed;
}

void EditorVoxelJoinSession::cancel() noexcept {
    active_ = false;
    resampleChosen_ = false;
    target_ = 0;
    operands_.clear();
    result_ = {};
    fingerprint_ = 0;
}

} // namespace dve::editor
