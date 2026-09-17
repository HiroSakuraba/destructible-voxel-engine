#include "dve/control_rig_authoring.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <queue>
#include <sstream>
#include <system_error>

namespace dve::editor {
namespace {

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

bool finite(Float3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool finite(Quaternion value) noexcept {
    const float norm = value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w;
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
           std::isfinite(value.w) && norm > 0.0F;
}

bool finite(const RigidTransform& value) noexcept { return finite(value.position) && finite(value.rotation); }

bool valid_name(std::string_view value) noexcept {
    return !value.empty() && value.size() <= 255U &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= 32U && c != 127U; });
}

ControlRigControl* find_control(ControlRigAsset& rig, ControlRigControlId id) noexcept {
    const auto found = std::find_if(rig.controls.begin(), rig.controls.end(),
        [id](const ControlRigControl& control) { return control.id == id; });
    return found == rig.controls.end() ? nullptr : &*found;
}

const ControlRigControl* find_control(const ControlRigAsset& rig, ControlRigControlId id) noexcept {
    const auto found = std::find_if(rig.controls.begin(), rig.controls.end(),
        [id](const ControlRigControl& control) { return control.id == id; });
    return found == rig.controls.end() ? nullptr : &*found;
}

ControlRigNode* find_node(ControlRigAsset& rig, ControlRigNodeId id) noexcept {
    const auto found = std::find_if(rig.nodes.begin(), rig.nodes.end(),
        [id](const ControlRigNode& node) { return node.id == id; });
    return found == rig.nodes.end() ? nullptr : &*found;
}

const ControlRigNode* find_node(const ControlRigAsset& rig, ControlRigNodeId id) noexcept {
    const auto found = std::find_if(rig.nodes.begin(), rig.nodes.end(),
        [id](const ControlRigNode& node) { return node.id == id; });
    return found == rig.nodes.end() ? nullptr : &*found;
}

const ControlRigGraphPinSpec* find_pin(
    const ControlRigAuthoringDocument& document, const ControlRigGraphEndpoint& endpoint,
    std::vector<ControlRigGraphPinSpec>& storage) {
    storage.clear();
    if (endpoint.kind == ControlRigGraphEntityKind::Control) {
        const ControlRigControl* control = find_control(document.rig, endpoint.entity);
        if (!control) return nullptr;
        storage = control_rig_graph_pins(*control);
    } else {
        const ControlRigNode* node = find_node(document.rig, endpoint.entity);
        if (!node) return nullptr;
        storage = control_rig_graph_pins(*node);
    }
    const auto found = std::find_if(storage.begin(), storage.end(), [&](const auto& pin) {
        return pin.name == endpoint.pin;
    });
    return found == storage.end() ? nullptr : &*found;
}

bool validate_link(
    const ControlRigAuthoringDocument& document, const ControlRigGraphLink& link,
    std::string* error) {
    std::vector<ControlRigGraphPinSpec> fromStorage, toStorage;
    const ControlRigGraphPinSpec* from = find_pin(document, link.from, fromStorage);
    const ControlRigGraphPinSpec* to = find_pin(document, link.to, toStorage);
    if (!from || !to) return fail(error, "control rig graph link references a missing pin");
    if (from->direction != ControlRigGraphPinDirection::Output ||
        to->direction != ControlRigGraphPinDirection::Input)
        return fail(error, "control rig graph link direction is invalid");
    if (from->type != to->type) return fail(error, "control rig graph pin types do not match");
    if (from->type == ControlRigGraphPinType::Execute) {
        if (link.from.kind != ControlRigGraphEntityKind::Node ||
            link.to.kind != ControlRigGraphEntityKind::Node || link.from.entity == link.to.entity)
            return fail(error, "execute links must connect different rig nodes");
        const ControlRigNode* source = find_node(document.rig, link.from.entity);
        const ControlRigNode* target = find_node(document.rig, link.to.entity);
        if (!source || !target || static_cast<std::uint8_t>(source->phase) > static_cast<std::uint8_t>(target->phase))
            return fail(error, "execute link runs backward across solve phases");
    } else if (link.from.kind != ControlRigGraphEntityKind::Control ||
               link.to.kind != ControlRigGraphEntityKind::Node) {
        return fail(error, "data links must connect a control to a rig node");
    }
    for (const ControlRigGraphLink& existing : document.links) {
        if (existing.id == link.id) continue;
        if (!to->multiple && existing.to.kind == link.to.kind && existing.to.entity == link.to.entity &&
            existing.to.pin == link.to.pin) return fail(error, "rig node input already has a link");
        if (existing.from.kind == link.from.kind && existing.from.entity == link.from.entity &&
            existing.from.pin == link.from.pin && existing.to.kind == link.to.kind &&
            existing.to.entity == link.to.entity && existing.to.pin == link.to.pin)
            return fail(error, "duplicate control rig graph link");
    }
    return true;
}

bool sort_compiled_nodes(
    const ControlRigAuthoringDocument& document, ControlRigAsset& output, std::string* error) {
    std::vector<ControlRigNode> ordered;
    ordered.reserve(output.nodes.size());
    for (unsigned phaseValue = 0U; phaseValue <= static_cast<unsigned>(ControlRigSolvePhase::PostSolve); ++phaseValue) {
        const auto phase = static_cast<ControlRigSolvePhase>(phaseValue);
        std::vector<ControlRigNodeId> ids;
        for (const ControlRigNode& node : output.nodes) if (node.phase == phase) ids.push_back(node.id);
        std::map<ControlRigNodeId, std::size_t> authoredOrder;
        std::map<ControlRigNodeId, std::size_t> indegree;
        std::map<ControlRigNodeId, std::vector<ControlRigNodeId>> adjacency;
        for (std::size_t i = 0U; i < ids.size(); ++i) {
            authoredOrder.emplace(ids[i], i);
            indegree.emplace(ids[i], 0U);
        }
        for (const ControlRigGraphLink& link : document.links) {
            if (link.from.pin != "Out" || link.to.pin != "In") continue;
            const ControlRigNode* from = find_node(output, link.from.entity);
            const ControlRigNode* to = find_node(output, link.to.entity);
            if (!from || !to || from->phase != phase || to->phase != phase) continue;
            adjacency[from->id].push_back(to->id);
            ++indegree[to->id];
        }
        std::vector<ControlRigNodeId> ready;
        for (ControlRigNodeId id : ids) if (indegree[id] == 0U) ready.push_back(id);
        auto byAuthoredOrder = [&](ControlRigNodeId a, ControlRigNodeId b) {
            return authoredOrder[a] < authoredOrder[b];
        };
        std::stable_sort(ready.begin(), ready.end(), byAuthoredOrder);
        std::vector<ControlRigNodeId> phaseOrder;
        while (!ready.empty()) {
            const ControlRigNodeId id = ready.front();
            ready.erase(ready.begin());
            phaseOrder.push_back(id);
            for (ControlRigNodeId dependent : adjacency[id]) {
                if (--indegree[dependent] == 0U) {
                    ready.push_back(dependent);
                    std::stable_sort(ready.begin(), ready.end(), byAuthoredOrder);
                }
            }
        }
        if (phaseOrder.size() != ids.size()) return fail(error, "control rig execute links contain a cycle");
        for (ControlRigNodeId id : phaseOrder) ordered.push_back(*find_node(output, id));
    }
    output.nodes = std::move(ordered);
    return true;
}

bool write_atomic(const std::filesystem::path& path, std::string_view bytes, std::string* error) {
    std::error_code ec;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return fail(error, "could not create control rig layout directory: " + ec.message());
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return fail(error, "could not open temporary control rig layout");
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        stream.flush();
        if (!stream) {
            stream.close();
            std::filesystem::remove(temporary, ec);
            return fail(error, "could not write complete control rig layout");
        }
    }
    std::filesystem::path backup = path;
    backup += ".bak";
    const bool replacing = std::filesystem::exists(path, ec);
    if (ec) { std::filesystem::remove(temporary, ec); return fail(error, "could not inspect control rig layout"); }
    if (replacing) {
        std::filesystem::remove(backup, ec);
        ec.clear();
        std::filesystem::rename(path, backup, ec);
        if (ec) { std::filesystem::remove(temporary, ec); return fail(error, "could not stage control rig layout"); }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        if (replacing) { ec.clear(); std::filesystem::rename(backup, path, ec); }
        return fail(error, "could not publish control rig layout transactionally");
    }
    if (replacing) std::filesystem::remove(backup, ec);
    return true;
}

bool compute_local_for_space(
    const SkeletonAsset& skeleton, std::span<const RigidTransform> inputPose,
    const std::map<ControlRigControlId, RigidTransform>& controlModels,
    ControlRigControlId self, RigidTransform desiredModel,
    ControlRigSpace space, BoneIndex spaceBone, ControlRigControlId parentControl,
    RigidTransform& local, std::string* error) {
    if (space == ControlRigSpace::Model) { local = desiredModel; return true; }
    if (space == ControlRigSpace::Bone) {
        const auto model = compute_model_pose(skeleton, inputPose, error);
        if (model.empty() || spaceBone >= model.size()) return fail(error, "control space bone is invalid");
        local = relative_rigid_transform(model[spaceBone], desiredModel);
        return true;
    }
    if (parentControl == self) return fail(error, "control cannot use itself as a space");
    const auto parent = controlModels.find(parentControl);
    if (parent == controlModels.end()) return fail(error, "control parent space is unresolved");
    local = relative_rigid_transform(parent->second, desiredModel);
    return true;
}

bool validate_authoring_metadata(
    const ControlRigAuthoringDocument& document, std::string* error) {
    for (const auto& [id, layout] : document.controlLayouts) {
        if (!find_control(document.rig, id) || !std::isfinite(layout.position.x) ||
            !std::isfinite(layout.position.y) || !std::isfinite(layout.size.width) ||
            !std::isfinite(layout.size.height) || layout.size.width <= 0.0F || layout.size.height <= 0.0F)
            return fail(error, "control rig control layout is invalid");
    }
    for (const auto& [id, layout] : document.nodeLayouts) {
        if (!find_node(document.rig, id) || !std::isfinite(layout.position.x) ||
            !std::isfinite(layout.position.y) || !std::isfinite(layout.size.width) ||
            !std::isfinite(layout.size.height) || layout.size.width <= 0.0F || layout.size.height <= 0.0F)
            return fail(error, "control rig node layout is invalid");
    }
    for (const auto& [id, visual] : document.controlVisuals) {
        if (!find_control(document.rig, id) || !std::isfinite(visual.sizeMeters) ||
            visual.sizeMeters <= 0.0F || visual.sizeMeters > 1000.0F ||
            !std::isfinite(visual.color.r) || !std::isfinite(visual.color.g) ||
            !std::isfinite(visual.color.b) || !std::isfinite(visual.color.a))
            return fail(error, "control rig visual is invalid");
    }
    std::set<std::uint64_t> linkIds;
    std::uint64_t maximumLinkId{};
    for (const ControlRigGraphLink& link : document.links) {
        if (link.id == 0U || !linkIds.insert(link.id).second || !validate_link(document, link, error))
            return false;
        maximumLinkId = std::max(maximumLinkId, link.id);
    }
    std::set<std::uint64_t> commentIds;
    std::uint64_t maximumCommentId{};
    for (const ControlRigGraphComment& comment : document.comments) {
        if (comment.id == 0U || !commentIds.insert(comment.id).second || comment.text.empty() ||
            comment.text.size() > 4096U || !std::isfinite(comment.position.x) ||
            !std::isfinite(comment.position.y) || !std::isfinite(comment.size.width) ||
            !std::isfinite(comment.size.height) || comment.size.width <= 0.0F ||
            comment.size.height <= 0.0F || !std::isfinite(comment.color.r) ||
            !std::isfinite(comment.color.g) || !std::isfinite(comment.color.b) ||
            !std::isfinite(comment.color.a)) return fail(error, "control rig graph comment is invalid");
        maximumCommentId = std::max(maximumCommentId, comment.id);
    }
    if (document.nextLinkId == 0U || document.nextCommentId == 0U ||
        document.nextLinkId <= maximumLinkId || document.nextCommentId <= maximumCommentId)
        return fail(error, "control rig authoring counters are invalid");
    return true;
}

} // namespace

std::vector<ControlRigGraphPinSpec> control_rig_graph_pins(const ControlRigControl& control) {
    std::vector<ControlRigGraphPinSpec> result;
    if (control.kind == ControlRigControlKind::Transform)
        result.push_back({"Transform", ControlRigGraphPinDirection::Output, ControlRigGraphPinType::Transform});
    if (control.kind != ControlRigControlKind::Rotation)
        result.push_back({"Position", ControlRigGraphPinDirection::Output, ControlRigGraphPinType::Position});
    if (control.kind != ControlRigControlKind::Translation)
        result.push_back({"Rotation", ControlRigGraphPinDirection::Output, ControlRigGraphPinType::Rotation});
    return result;
}

std::vector<ControlRigGraphPinSpec> control_rig_graph_pins(const ControlRigNode& node) {
    std::vector<ControlRigGraphPinSpec> result{
        {"In", ControlRigGraphPinDirection::Input, ControlRigGraphPinType::Execute, false, false},
        {"Out", ControlRigGraphPinDirection::Output, ControlRigGraphPinType::Execute, false, true}};
    if (node.kind == ControlRigNodeKind::SetBoneTransform || node.kind == ControlRigNodeKind::ParentConstraint)
        result.push_back({"Target", ControlRigGraphPinDirection::Input, ControlRigGraphPinType::Transform, true, false});
    else if (node.kind == ControlRigNodeKind::AimConstraint || node.kind == ControlRigNodeKind::Fabrik)
        result.push_back({"Target", ControlRigGraphPinDirection::Input, ControlRigGraphPinType::Position, true, false});
    else if (node.kind == ControlRigNodeKind::TwoBoneIk) {
        result.push_back({"Target", ControlRigGraphPinDirection::Input, ControlRigGraphPinType::Position, true, false});
        result.push_back({"Pole", ControlRigGraphPinDirection::Input, ControlRigGraphPinType::Position, true, false});
    }
    return result;
}

std::vector<ControlRigAuthoringDiagnostic> ControlRigAuthoringDocument::diagnostics(
    const SkeletonAsset& skeleton) const {
    std::vector<ControlRigAuthoringDiagnostic> result;
    std::set<std::uint64_t> linkIds;
    for (const ControlRigGraphLink& link : links) {
        std::string error;
        if (link.id == 0U || !linkIds.insert(link.id).second)
            error = "control rig graph link ID is invalid or duplicated";
        else (void)validate_link(*this, link, &error);
        if (!error.empty()) result.push_back({ControlRigDiagnosticSeverity::Error, error, link.to.kind, link.to.entity});
    }
    ControlRigAsset compiled;
    std::string compileError;
    if (!compile(skeleton, compiled, &compileError))
        result.push_back({ControlRigDiagnosticSeverity::Error, compileError, ControlRigGraphEntityKind::Node, 0U});
    for (const ControlRigControl& control : rig.controls) {
        if (!controlLayouts.contains(control.id))
            result.push_back({ControlRigDiagnosticSeverity::Warning, "control has no graph layout", ControlRigGraphEntityKind::Control, control.id});
        if (!controlVisuals.contains(control.id))
            result.push_back({ControlRigDiagnosticSeverity::Info, "control uses the default viewport shape", ControlRigGraphEntityKind::Control, control.id});
    }
    for (const ControlRigNode& node : rig.nodes)
        if (!nodeLayouts.contains(node.id))
            result.push_back({ControlRigDiagnosticSeverity::Warning, "node has no graph layout", ControlRigGraphEntityKind::Node, node.id});
    return result;
}

bool ControlRigAuthoringDocument::compile(
    const SkeletonAsset& skeleton, ControlRigAsset& output, std::string* error) const {
    for (const ControlRigGraphLink& link : links)
        if (!validate_link(*this, link, error)) return false;
    output = rig;
    if (!sort_compiled_nodes(*this, output, error)) return false;
    const AnimationValidationResult validation = validate_control_rig(skeleton, output);
    if (!validation) return fail(error, validation.message);
    output.contentHash = control_rig_content_hash(output);
    return true;
}

ControlRigAuthoringSession::ControlRigAuthoringSession(ControlRigAuthoringDocument document)
    : document_(std::move(document)) {}

std::string_view ControlRigAuthoringSession::undo_label() const noexcept {
    return undo_.empty() ? std::string_view{} : std::string_view(undo_.back().label);
}

std::string_view ControlRigAuthoringSession::redo_label() const noexcept {
    return redo_.empty() ? std::string_view{} : std::string_view(redo_.back().label);
}

bool ControlRigAuthoringSession::undo() {
    if (undo_.empty()) return false;
    HistoryEntry entry = std::move(undo_.back());
    undo_.pop_back();
    redo_.push_back({entry.label, std::move(document_)});
    document_ = std::move(entry.state);
    return true;
}

bool ControlRigAuthoringSession::redo() {
    if (redo_.empty()) return false;
    HistoryEntry entry = std::move(redo_.back());
    redo_.pop_back();
    undo_.push_back({entry.label, std::move(document_)});
    document_ = std::move(entry.state);
    return true;
}

void ControlRigAuthoringSession::clear_history() noexcept { undo_.clear(); redo_.clear(); }

bool ControlRigAuthoringSession::mutate(
    std::string label,
    const std::function<bool(ControlRigAuthoringDocument&, std::string*)>& operation,
    std::string* error) {
    ControlRigAuthoringDocument before = document_;
    if (!operation(document_, error)) { document_ = std::move(before); return false; }
    ++document_.revision;
    undo_.push_back({std::move(label), std::move(before)});
    if (undo_.size() > historyLimit_) undo_.erase(undo_.begin());
    redo_.clear();
    return true;
}

ControlRigControlId ControlRigAuthoringSession::add_control(
    ControlRigControl control, ControlRigGraphPoint position, std::string* error) {
    ControlRigControlId assigned{};
    if (!mutate("Add Control", [&](ControlRigAuthoringDocument& document, std::string* operationError) {
        if (!valid_name(control.name)) return fail(operationError, "control name is invalid");
        if (std::any_of(document.rig.controls.begin(), document.rig.controls.end(), [&](const auto& existing) {
                return existing.name == control.name || (control.id != 0U && existing.id == control.id);
            })) return fail(operationError, "control ID or name already exists");
        if (control.id == 0U) for (const auto& existing : document.rig.controls) control.id = std::max(control.id, existing.id);
        if (control.id == 0U || std::any_of(document.rig.controls.begin(), document.rig.controls.end(),
                [&](const auto& existing) { return existing.id == control.id; })) ++control.id;
        assigned = control.id;
        document.rig.controls.push_back(control);
        document.controlLayouts[assigned].position = position;
        document.controlVisuals.try_emplace(assigned);
        document.rig.contentHash = 0U;
        return true;
    }, error)) return 0U;
    return assigned;
}

ControlRigNodeId ControlRigAuthoringSession::add_node(
    ControlRigNode node, ControlRigGraphPoint position, std::string* error) {
    ControlRigNodeId assigned{};
    if (!mutate("Add Node", [&](ControlRigAuthoringDocument& document, std::string* operationError) {
        if (!valid_name(node.name)) return fail(operationError, "node name is invalid");
        if (std::any_of(document.rig.nodes.begin(), document.rig.nodes.end(), [&](const auto& existing) {
                return existing.name == node.name || (node.id != 0U && existing.id == node.id);
            })) return fail(operationError, "node ID or name already exists");
        if (node.id == 0U) for (const auto& existing : document.rig.nodes) node.id = std::max(node.id, existing.id);
        if (node.id == 0U || std::any_of(document.rig.nodes.begin(), document.rig.nodes.end(),
                [&](const auto& existing) { return existing.id == node.id; })) ++node.id;
        assigned = node.id;
        document.rig.nodes.push_back(node);
        document.nodeLayouts[assigned].position = position;
        document.rig.contentHash = 0U;
        return true;
    }, error)) return 0U;
    return assigned;
}

bool ControlRigAuthoringSession::remove_control(ControlRigControlId control, std::string* error) {
    return mutate("Remove Control", [control](ControlRigAuthoringDocument& document, std::string* operationError) {
        if (!find_control(document.rig, control)) return fail(operationError, "control does not exist");
        for (const ControlRigNode& node : document.rig.nodes)
            if (node.targetControl == control || node.poleControl == control)
                return fail(operationError, "control is still referenced by a rig node");
        for (const ControlRigControl& candidate : document.rig.controls)
            if (candidate.parentControl == control)
                return fail(operationError, "control is still used as a parent space");
        std::erase_if(document.rig.controls, [control](const auto& value) { return value.id == control; });
        std::erase_if(document.links, [control](const auto& link) {
            return link.from.kind == ControlRigGraphEntityKind::Control && link.from.entity == control;
        });
        document.controlLayouts.erase(control); document.controlVisuals.erase(control);
        document.selectedControls.erase(control); document.rig.contentHash = 0U;
        return true;
    }, error);
}

bool ControlRigAuthoringSession::remove_node(ControlRigNodeId node, std::string* error) {
    return mutate("Remove Node", [node](ControlRigAuthoringDocument& document, std::string* operationError) {
        if (!find_node(document.rig, node)) return fail(operationError, "node does not exist");
        std::erase_if(document.rig.nodes, [node](const auto& value) { return value.id == node; });
        std::erase_if(document.links, [node](const auto& link) {
            return (link.from.kind == ControlRigGraphEntityKind::Node && link.from.entity == node) ||
                   (link.to.kind == ControlRigGraphEntityKind::Node && link.to.entity == node);
        });
        document.nodeLayouts.erase(node); document.selectedNodes.erase(node); document.rig.contentHash = 0U;
        return true;
    }, error);
}

bool ControlRigAuthoringSession::move_control(ControlRigControlId control, ControlRigGraphPoint position) {
    return mutate("Move Control", [control, position](auto& document, std::string* error) {
        if (!find_control(document.rig, control)) return fail(error, "control does not exist");
        if (!std::isfinite(position.x) || !std::isfinite(position.y)) return fail(error, "graph position is invalid");
        document.controlLayouts[control].position = position;
        return true;
    });
}

bool ControlRigAuthoringSession::move_node(ControlRigNodeId node, ControlRigGraphPoint position) {
    return mutate("Move Node", [node, position](auto& document, std::string* error) {
        if (!find_node(document.rig, node)) return fail(error, "node does not exist");
        if (!std::isfinite(position.x) || !std::isfinite(position.y)) return fail(error, "graph position is invalid");
        document.nodeLayouts[node].position = position;
        return true;
    });
}

bool ControlRigAuthoringSession::set_control_visual(
    ControlRigControlId control, ControlRigControlVisual visual, std::string* error) {
    return mutate("Set Control Shape", [control, visual](auto& document, std::string* operationError) {
        if (!find_control(document.rig, control)) return fail(operationError, "control does not exist");
        if (!std::isfinite(visual.sizeMeters) || visual.sizeMeters <= 0.0F || visual.sizeMeters > 1000.0F ||
            !std::isfinite(visual.color.r) || !std::isfinite(visual.color.g) ||
            !std::isfinite(visual.color.b) || !std::isfinite(visual.color.a))
            return fail(operationError, "control visual is invalid");
        document.controlVisuals[control] = visual;
        return true;
    }, error);
}

bool ControlRigAuthoringSession::update_controls(
    const SkeletonAsset& skeleton, std::span<const ControlRigControl> controls,
    std::string* error) {
    return mutate("Edit Controls", [&](auto& document, std::string* operationError) {
        if (controls.empty()) return fail(operationError, "control edit is empty");
        std::set<ControlRigControlId> ids;
        for (const ControlRigControl& replacement : controls) {
            if (replacement.id == kInvalidControlRigControlId || !ids.insert(replacement.id).second)
                return fail(operationError, "control edit contains an invalid or duplicate ID");
            ControlRigControl* target = find_control(document.rig, replacement.id);
            if (!target) return fail(operationError, "control edit references a missing control");
            *target = replacement;
        }
        std::set<std::string> names;
        for (const ControlRigControl& control : document.rig.controls) {
            if (!valid_name(control.name)) return fail(operationError, "control name is invalid");
            if (!names.insert(control.name).second) return fail(operationError, "control name already exists");
        }
        document.rig.contentHash = 0U;
        ControlRigAsset compiled;
        return document.compile(skeleton, compiled, operationError);
    }, error);
}

bool ControlRigAuthoringSession::update_nodes(
    const SkeletonAsset& skeleton, std::span<const ControlRigNode> nodes,
    std::string* error) {
    return mutate("Edit Nodes", [&](auto& document, std::string* operationError) {
        if (nodes.empty()) return fail(operationError, "node edit is empty");
        std::set<ControlRigNodeId> ids;
        for (const ControlRigNode& replacement : nodes) {
            if (replacement.id == kInvalidControlRigNodeId || !ids.insert(replacement.id).second)
                return fail(operationError, "node edit contains an invalid or duplicate ID");
            ControlRigNode* target = find_node(document.rig, replacement.id);
            if (!target) return fail(operationError, "node edit references a missing node");
            *target = replacement;
        }
        std::set<std::string> names;
        for (const ControlRigNode& node : document.rig.nodes) {
            if (!valid_name(node.name)) return fail(operationError, "node name is invalid");
            if (!names.insert(node.name).second) return fail(operationError, "node name already exists");
        }
        document.rig.contentHash = 0U;
        ControlRigAsset compiled;
        return document.compile(skeleton, compiled, operationError);
    }, error);
}

bool ControlRigAuthoringSession::set_control_visuals(
    const std::map<ControlRigControlId, ControlRigControlVisual>& visuals,
    std::string* error) {
    return mutate("Edit Control Visuals", [&](auto& document, std::string* operationError) {
        if (visuals.empty()) return fail(operationError, "control visual edit is empty");
        for (const auto& [id, visual] : visuals) {
            if (!find_control(document.rig, id))
                return fail(operationError, "control visual edit references a missing control");
            if (!std::isfinite(visual.sizeMeters) || visual.sizeMeters <= 0.0F ||
                visual.sizeMeters > 1000.0F || !std::isfinite(visual.color.r) ||
                !std::isfinite(visual.color.g) || !std::isfinite(visual.color.b) ||
                !std::isfinite(visual.color.a))
                return fail(operationError, "control visual is invalid");
            document.controlVisuals[id] = visual;
        }
        return true;
    }, error);
}

bool ControlRigAuthoringSession::set_control_default(
    ControlRigControlId control, RigidTransform value, std::string* error) {
    return mutate("Set Control Transform", [control, value](auto& document, std::string* operationError) {
        ControlRigControl* target = find_control(document.rig, control);
        if (!target) return fail(operationError, "control does not exist");
        if (!finite(value)) return fail(operationError, "control transform is invalid");
        target->defaultLocal = value;
        document.rig.contentHash = 0U;
        return true;
    }, error);
}

std::uint64_t ControlRigAuthoringSession::connect(
    ControlRigGraphEndpoint from, ControlRigGraphEndpoint to, std::string* error) {
    std::uint64_t assigned{};
    if (!mutate("Connect Pins", [&](auto& document, std::string* operationError) {
        ControlRigGraphLink link{document.nextLinkId++, std::move(from), std::move(to)};
        if (!validate_link(document, link, operationError)) return false;
        assigned = link.id;
        document.links.push_back(link);
        if (link.to.kind == ControlRigGraphEntityKind::Node && link.from.kind == ControlRigGraphEntityKind::Control) {
            ControlRigNode* node = find_node(document.rig, link.to.entity);
            if (link.to.pin == "Target") node->targetControl = link.from.entity;
            if (link.to.pin == "Pole") node->poleControl = link.from.entity;
            document.rig.contentHash = 0U;
        }
        std::string compileError;
        if (!sort_compiled_nodes(document, document.rig, &compileError)) return fail(operationError, compileError);
        return true;
    }, error)) return 0U;
    return assigned;
}

bool ControlRigAuthoringSession::disconnect(std::uint64_t linkId) {
    return mutate("Disconnect Pins", [linkId](auto& document, std::string* error) {
        const auto found = std::find_if(document.links.begin(), document.links.end(),
            [linkId](const auto& link) { return link.id == linkId; });
        if (found == document.links.end()) return fail(error, "graph link does not exist");
        if (found->to.kind == ControlRigGraphEntityKind::Node &&
            found->from.kind == ControlRigGraphEntityKind::Control) {
            ControlRigNode* node = find_node(document.rig, found->to.entity);
            if (found->to.pin == "Target") node->targetControl = kInvalidControlRigControlId;
            if (found->to.pin == "Pole") node->poleControl = kInvalidControlRigControlId;
            document.rig.contentHash = 0U;
        }
        document.links.erase(found);
        return true;
    });
}

std::uint64_t ControlRigAuthoringSession::add_comment(
    std::string text, ControlRigGraphPoint position, ControlRigGraphSize size, std::string* error) {
    std::uint64_t assigned{};
    if (!mutate("Add Comment", [&](auto& document, std::string* operationError) {
        if (text.empty() || text.size() > 4096U || !std::isfinite(position.x) || !std::isfinite(position.y) ||
            !std::isfinite(size.width) || !std::isfinite(size.height) || size.width <= 0.0F || size.height <= 0.0F)
            return fail(operationError, "graph comment is invalid");
        assigned = document.nextCommentId++;
        document.comments.push_back({assigned, std::move(text), position, size, {}});
        return true;
    }, error)) return 0U;
    return assigned;
}

bool ControlRigAuthoringSession::remove_comment(std::uint64_t comment) {
    return mutate("Remove Comment", [comment](auto& document, std::string* error) {
        const std::size_t before = document.comments.size();
        std::erase_if(document.comments, [comment](const auto& value) { return value.id == comment; });
        return document.comments.size() != before ? true : fail(error, "graph comment does not exist");
    });
}

bool ControlRigAuthoringSession::switch_control_space_preserve_model(
    const SkeletonAsset& skeleton, std::span<const RigidTransform> inputPose,
    const std::map<ControlRigControlId, RigidTransform>& controlLocals,
    ControlRigControlId controlId, ControlRigSpace newSpace,
    BoneIndex newSpaceBone, ControlRigControlId newParentControl, std::string* error) {
    return mutate("Switch Control Space", [&](auto& document, std::string* operationError) {
        ControlRigControl* control = find_control(document.rig, controlId);
        if (!control) return fail(operationError, "control does not exist");
        LocalPose output;
        std::map<ControlRigControlId, RigidTransform> models;
        if (!evaluate_control_rig(skeleton, document.rig, inputPose, controlLocals, output, &models, operationError))
            return false;
        RigidTransform local;
        if (!compute_local_for_space(skeleton, inputPose, models, controlId, models.at(controlId),
                newSpace, newSpaceBone, newParentControl, local, operationError)) return false;
        const ControlRigControl previous = *control;
        control->space = newSpace;
        control->spaceBone = newSpace == ControlRigSpace::Bone ? newSpaceBone : kInvalidBoneIndex;
        control->parentControl = newSpace == ControlRigSpace::Control ? newParentControl : kInvalidControlRigControlId;
        control->defaultLocal = local;
        const AnimationValidationResult validation = validate_control_rig(skeleton, document.rig);
        if (!validation) { *control = previous; return fail(operationError, validation.message); }
        document.rig.contentHash = 0U;
        return true;
    }, error);
}

bool ControlRigAuthoringSession::match_control_to_bone(
    const SkeletonAsset& skeleton, std::span<const RigidTransform> inputPose,
    const std::map<ControlRigControlId, RigidTransform>& controlLocals,
    ControlRigControlId controlId, BoneIndex bone, std::string* error) {
    return mutate("Match Control To Bone", [&](auto& document, std::string* operationError) {
        ControlRigControl* control = find_control(document.rig, controlId);
        const auto boneModels = compute_model_pose(skeleton, inputPose, operationError);
        if (!control || boneModels.empty() || bone >= boneModels.size()) return fail(operationError, "control or bone is invalid");
        LocalPose output;
        std::map<ControlRigControlId, RigidTransform> controls;
        if (!evaluate_control_rig(skeleton, document.rig, inputPose, controlLocals, output, &controls, operationError))
            return false;
        if (!compute_local_for_space(skeleton, inputPose, controls, controlId, boneModels[bone],
                control->space, control->spaceBone, control->parentControl,
                control->defaultLocal, operationError)) return false;
        document.rig.contentHash = 0U;
        return true;
    }, error);
}

bool ControlRigAuthoringSession::match_two_bone_ik(
    const SkeletonAsset& skeleton, std::span<const RigidTransform> inputPose,
    const std::map<ControlRigControlId, RigidTransform>& controlLocals,
    ControlRigNodeId nodeId, std::string* error) {
    return mutate("Match FK To IK", [&](auto& document, std::string* operationError) {
        const ControlRigNode* node = find_node(document.rig, nodeId);
        if (!node || node->kind != ControlRigNodeKind::TwoBoneIk)
            return fail(operationError, "two-bone IK node does not exist");
        const auto boneModels = compute_model_pose(skeleton, inputPose, operationError);
        if (boneModels.empty()) return false;
        LocalPose output;
        std::map<ControlRigControlId, RigidTransform> controls;
        if (!evaluate_control_rig(skeleton, document.rig, inputPose, controlLocals, output, &controls, operationError))
            return false;
        ControlRigControl* target = find_control(document.rig, node->targetControl);
        ControlRigControl* pole = find_control(document.rig, node->poleControl);
        if (!target || !pole) return fail(operationError, "IK controls are missing");
        if (!compute_local_for_space(skeleton, inputPose, controls, target->id, boneModels[node->endBone],
                target->space, target->spaceBone, target->parentControl, target->defaultLocal, operationError)) return false;
        RigidTransform poleModel = controls.at(pole->id);
        poleModel.position = boneModels[node->middleBone].position;
        if (!compute_local_for_space(skeleton, inputPose, controls, pole->id, poleModel,
                pole->space, pole->spaceBone, pole->parentControl, pole->defaultLocal, operationError)) return false;
        document.rig.contentHash = 0U;
        return true;
    }, error);
}

std::vector<ControlRigViewportLine> build_control_rig_viewport_lines(
    const ControlRigAuthoringDocument& document,
    const std::map<ControlRigControlId, RigidTransform>& controlModels) {
    std::vector<ControlRigViewportLine> lines;
    auto emit = [&](ControlRigControlId id, const RigidTransform& transform,
                    ControlRigEditorColor color, Float3 a, Float3 b) {
        lines.push_back({transform_point(transform, a), transform_point(transform, b), color, id});
    };
    constexpr std::size_t segments = 24U;
    for (const ControlRigControl& control : document.rig.controls) {
        const auto model = controlModels.find(control.id);
        if (model == controlModels.end()) continue;
        ControlRigControlVisual visual;
        if (const auto authored = document.controlVisuals.find(control.id); authored != document.controlVisuals.end())
            visual = authored->second;
        if (!visual.visible) continue;
        const float s = visual.sizeMeters;
        if (visual.shape == ControlRigControlShape::Cross) {
            emit(control.id, model->second, visual.color, {-s,0,0}, {s,0,0});
            emit(control.id, model->second, visual.color, {0,-s,0}, {0,s,0});
            emit(control.id, model->second, visual.color, {0,0,-s}, {0,0,s});
        } else if (visual.shape == ControlRigControlShape::Box) {
            const Float3 corners[8]{{-s,-s,-s},{s,-s,-s},{s,s,-s},{-s,s,-s},
                                    {-s,-s,s},{s,-s,s},{s,s,s},{-s,s,s}};
            constexpr unsigned edges[12][2]{{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
            for (const auto& edge : edges) emit(control.id, model->second, visual.color, corners[edge[0]], corners[edge[1]]);
        } else if (visual.shape == ControlRigControlShape::Arrow) {
            emit(control.id, model->second, visual.color, {}, {s,0,0});
            emit(control.id, model->second, visual.color, {s,0,0}, {s*0.7F,s*0.25F,0});
            emit(control.id, model->second, visual.color, {s,0,0}, {s*0.7F,-s*0.25F,0});
        } else {
            const unsigned rings = visual.shape == ControlRigControlShape::Sphere ? 3U : 1U;
            for (unsigned ring = 0U; ring < rings; ++ring) {
                for (std::size_t i = 0U; i < segments; ++i) {
                    const float a = 6.28318530718F * static_cast<float>(i) / static_cast<float>(segments);
                    const float b = 6.28318530718F * static_cast<float>(i + 1U) / static_cast<float>(segments);
                    Float3 from{}, to{};
                    if (ring == 0U) { from = {s*std::cos(a),s*std::sin(a),0}; to = {s*std::cos(b),s*std::sin(b),0}; }
                    else if (ring == 1U) { from = {s*std::cos(a),0,s*std::sin(a)}; to = {s*std::cos(b),0,s*std::sin(b)}; }
                    else { from = {0,s*std::cos(a),s*std::sin(a)}; to = {0,s*std::cos(b),s*std::sin(b)}; }
                    emit(control.id, model->second, visual.color, from, to);
                }
            }
        }
    }
    return lines;
}

std::optional<ControlRigControlId> pick_control_rig_viewport_control(
    const ControlRigAuthoringDocument& document,
    const std::map<ControlRigControlId, RigidTransform>& controlModels,
    Float3 rayOrigin, Float3 rayDirection, float toleranceMeters) {
    if (!finite(rayOrigin) || !finite(rayDirection) || !(length_squared(rayDirection) > 1.0e-8F) ||
        !std::isfinite(toleranceMeters) || toleranceMeters < 0.0F) return std::nullopt;
    rayDirection = normalize(rayDirection);
    std::optional<ControlRigControlId> best;
    float bestDistance = std::numeric_limits<float>::max();
    for (const ControlRigControl& control : document.rig.controls) {
        const auto model = controlModels.find(control.id);
        if (model == controlModels.end()) continue;
        ControlRigControlVisual visual;
        if (const auto authored = document.controlVisuals.find(control.id); authored != document.controlVisuals.end())
            visual = authored->second;
        if (!visual.visible) continue;
        const Float3 toCenter = subtract(model->second.position, rayOrigin);
        const float along = dot(toCenter, rayDirection);
        if (along < 0.0F) continue;
        const Float3 closest = add(rayOrigin, multiply(rayDirection, along));
        if (length(subtract(model->second.position, closest)) <= visual.sizeMeters + toleranceMeters && along < bestDistance) {
            bestDistance = along;
            best = control.id;
        }
    }
    return best;
}

bool write_control_rig_authoring_layout(
    const std::filesystem::path& path, const ControlRigAuthoringDocument& document,
    std::string* error) {
    if (!validate_authoring_metadata(document, error)) return false;
    std::ostringstream out;
    out << std::setprecision(std::numeric_limits<float>::max_digits10);
    out << "DVE_CONTROL_RIG_LAYOUT 1\n";
    out << "rig_hash " << control_rig_content_hash(document.rig) << '\n';
    out << "control_layouts " << document.controlLayouts.size() << '\n';
    for (const auto& [id, layout] : document.controlLayouts)
        out << id << ' ' << layout.position.x << ' ' << layout.position.y << ' ' << layout.size.width << ' '
            << layout.size.height << ' ' << layout.collapsed << '\n';
    out << "node_layouts " << document.nodeLayouts.size() << '\n';
    for (const auto& [id, layout] : document.nodeLayouts)
        out << id << ' ' << layout.position.x << ' ' << layout.position.y << ' ' << layout.size.width << ' '
            << layout.size.height << ' ' << layout.collapsed << '\n';
    out << "visuals " << document.controlVisuals.size() << '\n';
    for (const auto& [id, visual] : document.controlVisuals)
        out << id << ' ' << static_cast<unsigned>(visual.shape) << ' ' << visual.sizeMeters << ' '
            << visual.color.r << ' ' << visual.color.g << ' ' << visual.color.b << ' ' << visual.color.a << ' '
            << visual.visible << '\n';
    out << "links " << document.links.size() << '\n';
    for (const ControlRigGraphLink& link : document.links)
        out << link.id << ' ' << static_cast<unsigned>(link.from.kind) << ' ' << link.from.entity << ' '
            << std::quoted(link.from.pin) << ' ' << static_cast<unsigned>(link.to.kind) << ' ' << link.to.entity << ' '
            << std::quoted(link.to.pin) << '\n';
    out << "comments " << document.comments.size() << '\n';
    for (const ControlRigGraphComment& comment : document.comments)
        out << comment.id << ' ' << std::quoted(comment.text) << ' ' << comment.position.x << ' '
            << comment.position.y << ' ' << comment.size.width << ' ' << comment.size.height << ' '
            << comment.color.r << ' ' << comment.color.g << ' ' << comment.color.b << ' ' << comment.color.a << '\n';
    out << "counters " << document.nextLinkId << ' ' << document.nextCommentId << '\n';
    out << "end\n";
    return write_atomic(path, out.str(), error);
}

bool read_control_rig_authoring_layout(
    const std::filesystem::path& path, const ControlRigAsset& rig,
    ControlRigAuthoringDocument& document, std::string* error, std::uint64_t maximumBytes) {
    std::error_code ec;
    const std::uint64_t size = std::filesystem::file_size(path, ec);
    if (ec || size == 0U || size > maximumBytes ||
        size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
        return fail(error, "control rig layout size is invalid");
    std::string bytes(static_cast<std::size_t>(size), '\0');
    std::ifstream input(path, std::ios::binary);
    if (!input || !input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
        return fail(error, "could not read complete control rig layout");
    std::istringstream stream(bytes);
    std::string label;
    unsigned version{};
    std::uint64_t rigHash{};
    if (!(stream >> label >> version) || label != "DVE_CONTROL_RIG_LAYOUT" || version != 1U ||
        !(stream >> label >> rigHash) || label != "rig_hash" || rigHash != control_rig_content_hash(rig))
        return fail(error, "control rig layout header or rig hash is invalid");
    ControlRigAuthoringDocument loaded;
    loaded.rig = rig;
    auto readLayouts = [&](std::string_view expected, auto& layouts) {
        std::size_t count{};
        if (!(stream >> label >> count) || label != expected || count > 16384U) return false;
        for (std::size_t i = 0U; i < count; ++i) {
            std::uint64_t id{};
            ControlRigGraphNodeLayout layout;
            if (!(stream >> id >> layout.position.x >> layout.position.y >> layout.size.width >>
                  layout.size.height >> layout.collapsed)) return false;
            if (!layouts.emplace(id, layout).second) return false;
        }
        return true;
    };
    if (!readLayouts("control_layouts", loaded.controlLayouts) ||
        !readLayouts("node_layouts", loaded.nodeLayouts)) return fail(error, "control rig layout records are invalid");
    std::size_t count{};
    if (!(stream >> label >> count) || label != "visuals" || count > 4096U)
        return fail(error, "control rig visual count is invalid");
    for (std::size_t i = 0U; i < count; ++i) {
        ControlRigControlId id{};
        unsigned shape{};
        ControlRigControlVisual visual;
        if (!(stream >> id >> shape >> visual.sizeMeters >> visual.color.r >> visual.color.g >> visual.color.b >>
              visual.color.a >> visual.visible) || shape > static_cast<unsigned>(ControlRigControlShape::Arrow))
            return fail(error, "control rig visual record is invalid");
        visual.shape = static_cast<ControlRigControlShape>(shape);
        if (!loaded.controlVisuals.emplace(id, visual).second)
            return fail(error, "control rig visual ID is duplicated");
    }
    if (!(stream >> label >> count) || label != "links" || count > 32768U)
        return fail(error, "control rig graph link count is invalid");
    for (std::size_t i = 0U; i < count; ++i) {
        ControlRigGraphLink link;
        unsigned fromKind{}, toKind{};
        if (!(stream >> link.id >> fromKind >> link.from.entity >> std::quoted(link.from.pin) >>
              toKind >> link.to.entity >> std::quoted(link.to.pin)) ||
            fromKind > 1U || toKind > 1U) return fail(error, "control rig graph link record is invalid");
        link.from.kind = static_cast<ControlRigGraphEntityKind>(fromKind);
        link.to.kind = static_cast<ControlRigGraphEntityKind>(toKind);
        loaded.links.push_back(std::move(link));
    }
    if (!(stream >> label >> count) || label != "comments" || count > 4096U)
        return fail(error, "control rig graph comment count is invalid");
    for (std::size_t i = 0U; i < count; ++i) {
        ControlRigGraphComment comment;
        if (!(stream >> comment.id >> std::quoted(comment.text) >> comment.position.x >> comment.position.y >>
              comment.size.width >> comment.size.height >> comment.color.r >> comment.color.g >>
              comment.color.b >> comment.color.a)) return fail(error, "control rig graph comment is invalid");
        loaded.comments.push_back(std::move(comment));
    }
    if (!(stream >> label >> loaded.nextLinkId >> loaded.nextCommentId) || label != "counters" ||
        !(stream >> label) || label != "end") return fail(error, "control rig layout footer is invalid");
    stream >> std::ws;
    if (!stream.eof()) return fail(error, "control rig layout contains trailing data");
    if (!validate_authoring_metadata(loaded, error)) return false;
    document = std::move(loaded);
    return true;
}

} // namespace dve::editor
