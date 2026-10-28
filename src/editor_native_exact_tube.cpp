// Exact tubes, coping and plane (miter) cuts in the native editor (editor_exact_tube.hpp).
//
//   Polygon > Create Exact Tube...      type "round 50.8 3 600 y"; a hollow tube as a certified exact B-rep
//   Polygon > Cope Tube A to B...       select A (active) and B; type the fit-up gap in mm (0 for none)
//   Polygon > Plane Cut / Miter Tube... select a tube; type "47.5" (angle from a square cut; high|low,
//                                       u|v and "at <mm>" are optional)
//
// Each runs brepkernel as a background job and applies one undo step: a new object (the operands are
// not touched) carrying the re-meshed exact result, its exact source and its tube record. A refusal
// or failure changes nothing and says why.
#include "dve/editor_native.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>

#include "dve/editor_exact_tube.hpp"
#include "dve/editor_number_field.hpp"
#include "editor_native_long_job.hpp"

namespace dve::editor {
namespace {

std::string mm_label(double metres) {
    char buffer[48];
    std::snprintf(buffer, sizeof buffer, "%.4g", metres * 1000.0);
    return buffer;
}

bool tube_target_ok(const EditorObject& object) {
    return object.polygon && object.flags.visible && !object.flags.locked && !object.prefabLink;
}

} // namespace

std::string NativeEditorController::exact_tube_summary(EditorObjectId id) const {
    const EditorObject* object = workspace_.document().find_object(id);
    if (!object || !object->polygon || !exact_brep_source(*object)) return {};
    const auto record = exact_tube_record(*object);
    if (!record) return {};
    std::string text = describe_tube_spec(record->spec);
    if (!record->cuts.empty()) {
        const auto count = 1 + std::count(record->cuts.begin(), record->cuts.end(), ';');
        text += "; " + std::to_string(count) + (count == 1 ? " cut" : " cuts");
    }
    return text;
}

std::string NativeEditorController::exact_tube_disabled_reason(std::string_view action) const {
    if (workspace_.mode() != EditorMode::Edit || playSession_.active()) return "Stop Play or Simulate before cutting tubes.";
    if (longJob_) return long_job_busy_reason();
    if (action == "polygon.create_exact_tube") return {};
    const auto selected = workspace_.selected_objects();
    const EditorObject* primary = workspace_.selected_object() ? workspace_.document().find_object(*workspace_.selected_object()) : nullptr;
    if (action == "mesh.exact_miter") {
        if (selected.size() != 1 || !primary) return "Select one tube first.";
        if (!tube_target_ok(*primary)) return primary->name + " must be a visible, unlocked polygon object (not a prefab instance).";
        if (is_isolated_out(primary->id)) return primary->name + " is isolated out of view.";
        if (!exact_brep_source(*primary)) return primary->name + " has no exact CAD source (make it with Polygon > Create Exact Tube).";
        if (!exact_tube_record(*primary)) return primary->name + " has no tube record (make it with Polygon > Create Exact Tube).";
        return {};
    }
    if (selected.size() != 2 || !primary) return "Select two tubes: A (the active one) is coped to B.";
    const EditorObject* other = nullptr;
    for (const EditorObjectId id : selected)
        if (id != primary->id) other = workspace_.document().find_object(id);
    if (!other) return "Select two different tubes.";
    for (const EditorObject* object : {primary, other}) {
        if (!tube_target_ok(*object)) return object->name + " must be a visible, unlocked polygon object (not a prefab instance).";
        if (is_isolated_out(object->id)) return object->name + " is isolated out of view.";
        if (!exact_brep_source(*object)) return object->name + " has no exact CAD source (make it with Polygon > Create Exact Tube).";
    }
    if (!exact_tube_record(*other)) return other->name + " (B) has no tube record: its outer body is what is cut away. Make it with Polygon > Create Exact Tube.";
    return {};
}

bool NativeEditorController::dispatch_exact_tube_action(std::string_view action) {
    if (const std::string reason = exact_tube_disabled_reason(action); !reason.empty()) { set_status(reason, true); return false; }
    if (action == "polygon.create_exact_tube") {
        begin_text_edit(TextEditKind::ExactTube, 0, "round 50.8 3 600 y");
        set_status("Type the tube: round OD wall length | square W wall length | rect W x H wall length, in mm (units mm cm m in), then the axis x, y or z. Enter creates it");
        return true;
    }
    if (action == "mesh.exact_cope") {
        begin_text_edit(TextEditKind::ExactCopeGap, 0, "0");
        set_status("Cope A to B: type the fit-up gap in mm between A's cut and B's outer surface (0 for none; e.g. 0.5 or 1.2mm), then Enter");
        return true;
    }
    if (action == "mesh.exact_miter") {
        begin_text_edit(TextEditKind::ExactMiter, 0, "45");
        set_status("Plane cut: type the angle from a square cut in degrees (47.5); optional high|low (side kept), u|v (lean direction), at <mm> (position on the axis), then Enter");
        return true;
    }
    return false;
}

bool NativeEditorController::commit_exact_tube_text() {
    const TextEditKind kind = textEdit_.kind;
    const std::string text = textEdit_.buffer;
    textEdit_ = {};
    if (kind == TextEditKind::ExactTube) {
        const auto parsed = parse_tube_spec(text);
        if (!parsed.spec) { set_status("Create Exact Tube: " + parsed.error, true, 8.0F); return false; }
        return start_exact_tube(*parsed.spec);
    }
    if (kind == TextEditKind::ExactCopeGap) {
        bool hasUnit = false;
        for (const char c : text) hasUnit = hasUnit || std::isalpha(static_cast<unsigned char>(c));
        const NumberFieldParse gap = parse_number_field(hasUnit ? text : text + "mm", NumberFieldUnit::Meters, 0.0);
        if (!gap.ok) { set_status("Cope: " + gap.error, true, 8.0F); return false; }
        if (!(gap.value >= 0.0)) { set_status("Cope: the fit-up gap cannot be negative", true, 8.0F); return false; }
        const auto selected = workspace_.selected_objects();
        const auto primary = workspace_.selected_object();
        if (!primary || selected.size() != 2) { set_status("Cope: select two tubes (A active)", true); return false; }
        EditorObjectId other = 0;
        for (const EditorObjectId id : selected) if (id != *primary) other = id;
        ExactCopeOptions options;
        options.gapWorldMeters = gap.value;
        return start_exact_cope(*primary, other, options);
    }
    if (kind == TextEditKind::ExactMiter) {
        const auto parsed = parse_miter_spec(text);
        if (!parsed.spec) { set_status("Plane cut: " + parsed.error, true, 8.0F); return false; }
        const auto primary = workspace_.selected_object();
        if (!primary || workspace_.selection_count() != 1) { set_status("Plane cut: select one tube", true); return false; }
        return start_exact_miter(*primary, *parsed.spec);
    }
    return false;
}

bool NativeEditorController::start_exact_tube(const TubeSpec& spec) {
    if (const std::string reason = exact_tube_disabled_reason("polygon.create_exact_tube"); !reason.empty()) { set_status(reason, true); return false; }
    auto created = std::make_shared<std::optional<ExactTubeResult>>();
    const auto id = workspace_.document().allocate_object_id();
    const auto root = projectRoot_;
    const auto settings = ExactCadSettings::from_environment();
    return start_long_job(LongJobKind::ImportModel, "Create exact tube (" + format_tube_spec(spec) + ")", longJobThreshold_ + 1,
        [created, spec, id, root, settings](LongJob& job) {
            if (job.cancelled()) return;
            job.report(0.1F, "brepkernel is building and certifying the tube");
            auto result = create_exact_tube(root, spec, id, settings);
            if (job.cancelled()) return;
            if (!result) {
                std::lock_guard lock(job.mutex);
                job.error = result.error;
                return;
            }
            created->emplace(std::move(result));
            job.report(1.0F, "Tube ready");
        },
        [this, created, spec, id](LongJob& job) {
            if (!*created) { set_status(job.label + " produced no geometry", true, 8.0F); return false; }
            auto& result = **created;
            const auto before = capture_selection_snapshot();
            const std::string profile = spec.profile == TubeProfile::Round ? "Round" : spec.profile == TubeProfile::Square ? "Square" : "Rect";
            EditorObject object(id, profile + " tube " + mm_label(spec.width) + (spec.profile == TubeProfile::Rectangular ? "x" + mm_label(spec.height) : std::string()) +
                                        "x" + mm_label(spec.wall));
            object.voxelSizeMeters = 1.0F;
            object.polygon = std::move(result.mesh);
            object.polygon->objectId = id;
            refresh_editor_polygon(*object.polygon);
            object.transform.position = camera_.target;
            Component exact = make_exact_brep_component(result.source);
            exact.id = 1;
            object.components.push_back(std::move(exact));
            ComponentId next = 2;
            if (result.collision.component) {
                Component pieces = *result.collision.component;
                pieces.id = next++;
                object.components.push_back(std::move(pieces));
            }
            Component record = make_exact_tube_component(spec);
            record.id = next;
            object.components.push_back(std::move(record));
            const auto added = workspace_.commands().execute(workspace_.document(), std::make_unique<AddObjectCommand>(std::move(object), "Create exact tube"));
            if (!added.success) { set_status(added.message, true); return false; }
            if (!viewportSettings_.isolatedObjects.empty()) viewportSettings_.isolatedObjects.insert(id);
            workspace_.select_object(id);
            selectionDomain_ = SelectionDomain::Objects;
            workspace_.commands().set_last_selection_transition(before, capture_selection_snapshot());
            frame_selection();
            refresh_menu_state();
            set_status(result.summary + "; exact source kept at " + result.source.relativePath.generic_string());
            return true;
        });
}

bool NativeEditorController::start_exact_cope(EditorObjectId a, EditorObjectId b, const ExactCopeOptions& options) {
    if (const std::string reason = exact_tube_disabled_reason("mesh.exact_cope"); !reason.empty()) { set_status(reason, true); return false; }
    auto request = prepare_exact_cope(projectRoot_, workspace_.document(), a, b, options);
    if (!request.error.empty()) { set_status(request.error, true, 8.0F); return false; }
    return start_exact_tube_cut(std::move(request), " (Coped to " + workspace_.document().find_object(b)->name + ")");
}

bool NativeEditorController::start_exact_miter(EditorObjectId a, const ExactMiterSpec& spec) {
    if (const std::string reason = exact_tube_disabled_reason("mesh.exact_miter"); !reason.empty()) { set_status(reason, true); return false; }
    auto request = prepare_exact_miter(projectRoot_, workspace_.document(), a, spec);
    if (!request.error.empty()) { set_status(request.error, true, 8.0F); return false; }
    char angle[32];
    std::snprintf(angle, sizeof angle, "%g", spec.angleDegrees);
    return start_exact_tube_cut(std::move(request), std::string(" (Plane cut ") + angle + ")");
}

bool NativeEditorController::start_exact_tube_cut(ExactTubeCutRequest request, std::string suffix) {
    auto output = std::make_shared<std::optional<ExactTubeCutResult>>();
    const auto root = projectRoot_;
    const auto settings = ExactCadSettings::from_environment();
    const std::string label = std::string("Exact ") + (request.kind == "cope" ? "cope" : "plane cut");
    return start_long_job(LongJobKind::ImportModel, label, longJobThreshold_ + 1,
        [output, request, root, settings](LongJob& job) {
            if (job.cancelled()) return;
            job.report(0.1F, "brepkernel is computing and certifying the cut");
            auto result = run_exact_tube_cut(root, request, settings);
            if (job.cancelled()) return;
            output->emplace(std::move(result));
            job.report(1.0F, "Cut finished");
        },
        [this, output, request, suffix](LongJob& job) {
            if (!*output) { set_status(job.label + " produced no result", true, 8.0F); return false; }
            auto& r = **output;
            auto& document = workspace_.document();
            if (exact_operand_stamp(document, request.boolean.a, request.boolean.b) != request.boolean.operandStamp) {
                set_status(job.label + " was not applied: an operand changed while it ran. Run it again", true, 8.0F);
                return false;
            }
            const auto* objectA = document.find_object(request.boolean.a);
            if (!objectA) { set_status(job.label + " was not applied: the tube is gone", true, 8.0F); return false; }
            if (!r) {
                std::string message = r.boolean.edit.error;
                if (!r.boolean.advisory.empty()) message += ". " + r.boolean.advisory;
                message += ". Nothing was changed";
                set_status(message, true, 14.0F);
                return false;
            }
            const auto before = capture_selection_snapshot();
            const auto id = document.allocate_object_id();
            EditorObject result(id, objectA->name + suffix);
            result.polygon = std::move(r.boolean.edit.mesh);
            result.polygon->objectId = id;
            refresh_editor_polygon(*result.polygon);
            result.transform = objectA->transform;
            result.parent = objectA->parent;
            result.voxelScale = objectA->voxelScale;
            result.voxelSizeMeters = objectA->voxelSizeMeters;
            result.flags = objectA->flags;
            Component exact = make_exact_brep_component(r.boolean.source);
            exact.id = 1;
            result.components.push_back(std::move(exact));
            ComponentId next = 2;
            if (r.boolean.collision.component) {
                Component pieces = *r.boolean.collision.component;
                pieces.id = next++;
                result.components.push_back(std::move(pieces));
            }
            if (request.tubeA) {
                Component record = make_exact_tube_component(request.tubeA->spec, append_tube_cut(request.tubeA->cuts, r.record));
                record.id = next++;
                result.components.push_back(std::move(record));
            }
            const auto added = workspace_.commands().execute(document, std::make_unique<AddObjectCommand>(std::move(result), "Create exact " + request.kind + " result"));
            if (!added.success) { set_status(added.message, true); return false; }
            if (!viewportSettings_.isolatedObjects.empty()) viewportSettings_.isolatedObjects.insert(id);
            workspace_.select_object(id);
            selectionDomain_ = SelectionDomain::Objects;
            workspace_.commands().set_last_selection_transition(before, capture_selection_snapshot());
            recompute_layout();
            refresh_menu_state();
            set_status(r.boolean.summary + "; the operands are unchanged", !r.warning.empty() || r.clearanceApplied, 14.0F);
            return true;
        });
}

} // namespace dve::editor
