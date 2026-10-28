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
#include <charconv>
#include <sstream>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dve/editor_exact_tube.hpp"
#include "editor_native_long_job.hpp"

namespace dve::editor {
namespace {

std::string mm_label(double metres) {
    char buffer[48];
    std::snprintf(buffer, sizeof buffer, "%.4g", metres * 1000.0);
    return buffer;
}

// `base`, or `base #2`, `base #3` ... when an object of that name already exists, so repeated cuts of
// the same tubes are told apart in the Hierarchy.
std::string unique_object_name(const EditorDocument& document, const std::string& base) {
    const auto taken = [&](const std::string& name) {
        for (const auto& [id, object] : document.objects()) {
            (void)id;
            if (object.name == name) return true;
        }
        return false;
    };
    if (!taken(base)) return base;
    for (int n = 2;; ++n) {
        std::string candidate = base + " #" + std::to_string(n);
        if (!taken(candidate)) return candidate;
    }
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
    if (action == "mesh.exact_edge_fillet" || action == "mesh.exact_variable_fillet" || action == "mesh.exact_face_fillet") {
        if (selected.size() != 1 || !primary) return "Select one exact CAD object.";
        if (!tube_target_ok(*primary) || is_isolated_out(primary->id)) return "The exact CAD object must be visible, unlocked and outside a prefab.";
        if (!exact_brep_source(*primary)) return "The selected object has no exact CAD B-rep source.";
        return {};
    }
    if (action == "mesh.exact_miter" || action == "mesh.exact_resize_tube") {
        if (selected.size() != 1 || !primary) return "Select one tube first.";
        if (!tube_target_ok(*primary)) return primary->name + " must be a visible, unlocked polygon object (not a prefab instance).";
        if (is_isolated_out(primary->id)) return primary->name + " is isolated out of view.";
        if (!exact_brep_source(*primary)) return primary->name + " has no exact CAD source (make it with Polygon > Create Exact Tube).";
        const auto tube = exact_tube_record(*primary);
        if (!tube) return primary->name + " has no tube record (make it with Polygon > Create Exact Tube).";
        if (action == "mesh.exact_resize_tube" && !tube->cuts.empty())
            return "This tube has cuts that cannot be replayed yet; resize the uncut stock and repeat its cuts.";
        return {};
    }
    if (selected.size() != 2 || !primary) return action == "mesh.exact_cope_solid"
        ? "Select two exact CAD objects: A (the active one) is cut by the supplied solid B."
        : "Select two tubes: A (the active one) is coped to B.";
    const EditorObject* other = nullptr;
    for (const EditorObjectId id : selected)
        if (id != primary->id) other = workspace_.document().find_object(id);
    if (!other) return "Select two different tubes.";
    for (const EditorObject* object : {primary, other}) {
        if (!tube_target_ok(*object)) return object->name + " must be a visible, unlocked polygon object (not a prefab instance).";
        if (is_isolated_out(object->id)) return object->name + " is isolated out of view.";
        if (!exact_brep_source(*object)) return object->name + " has no exact CAD source (make it with Polygon > Create Exact Tube).";
    }
    if (action == "mesh.exact_cope_solid") return {};
    if (!exact_tube_record(*other)) return other->name + " (B) has no tube record: its outer body is what is cut away. Make it with Polygon > Create Exact Tube.";
    return {};
}

bool NativeEditorController::dispatch_exact_tube_action(std::string_view action) {
    if (const std::string reason = exact_tube_disabled_reason(action); !reason.empty()) { set_status(reason, true); return false; }
    if (action == "polygon.create_exact_tube") {
        begin_text_edit(TextEditKind::ExactTube, 0, "round 50.8 3 600 y");
        set_status("Type round OD wall length | square W wall length | rect W x H wall length; optional r <corner radius> for square/rect, then axis x y z. Dimensions are mm. Enter creates it");
        return true;
    }
    if (action == "mesh.exact_edge_fillet" || action == "mesh.exact_variable_fillet" || action == "mesh.exact_face_fillet") {
        const auto id = *workspace_.selected_object();
        const bool variable = action == "mesh.exact_variable_fillet";
        const bool face = action == "mesh.exact_face_fillet";
        begin_text_edit(face ? TextEditKind::ExactFaceFillet : variable ? TextEditKind::ExactVariableFillet : TextEditKind::ExactEdgeFillet, id, "");
        set_status(face ? "Type radius in local mm and two zero-based exact face numbers (Measure shows them); only adjacent faces with one shared edge." : variable ? "Type start radius, end radius, then edge endpoints x y z x y z, all mesh-local mm; Enter creates a new OCCT-validated object."
                            : "Type radius, then edge endpoints x y z x y z, all mesh-local mm; Enter creates a new OCCT-validated object.");
        return true;
    }
    if (action == "mesh.exact_resize_tube") {
        const auto id = *workspace_.selected_object();
        const auto tube = exact_tube_record(*workspace_.document().find_object(id));
        begin_text_edit(TextEditKind::ExactTubeResize, id, format_tube_spec(tube->spec));
        set_status("Edit mesh-local tube sizes in mm (before object scale); Enter creates an exact stock copy at the original pose. Original unchanged.");
        return true;
    }
    if (action == "mesh.exact_cope_solid") {
        const auto primary = *workspace_.selected_object();
        EditorObjectId other = 0;
        for (auto id : workspace_.selected_objects()) if (id != primary) other = id;
        return start_exact_cope_solid(primary, other);
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

// Enter in one of the typed prompts. A value the prompt cannot use (unreadable, out of range, a gap
// the tubes cannot take, a plane that misses the tube) keeps the prompt open with the reason under
// it, so the value can be corrected; Esc cancels. Nothing typed afterwards can reach the viewport
// shortcuts, because the open prompt owns the keyboard. Every message starts with the command name.
bool NativeEditorController::commit_exact_tube_text() {
    const TextEditKind kind = textEdit_.kind;
    const std::string text = textEdit_.buffer;
    const EditorObjectId resizeSource = textEdit_.objectId;
    const auto retry = [this](const std::string& message) {
        textEdit_.error = message;
        textEdit_.replaceOnNextInput = false;  // the typed text stays; editing continues from it
        set_status(message, true, 8.0F);
        return false;
    };
    if (kind == TextEditKind::ExactTube) {
        const auto parsed = parse_tube_spec(text);
        if (!parsed.spec) return retry("Create Exact Tube: " + parsed.error);
        textEdit_ = {};
        return start_exact_tube(*parsed.spec);
    }
    if (kind == TextEditKind::ExactFaceFillet) {
        std::istringstream input(text);std::string rtext,atext,btext,extra;
        if (!(input>>rtext>>atext>>btext) || input>>extra) return retry("Face fillet needs radius in local mm and two zero-based face numbers");
        double radius{};int a{},b{};
        const auto [re,rec]=std::from_chars(rtext.data(),rtext.data()+rtext.size(),radius);
        const auto [ae,aec]=std::from_chars(atext.data(),atext.data()+atext.size(),a);
        const auto [be,bec]=std::from_chars(btext.data(),btext.data()+btext.size(),b);
        if (rec!=std::errc{} || re!=rtext.data()+rtext.size() || !std::isfinite(radius) ||
            aec!=std::errc{} || ae!=atext.data()+atext.size() || bec!=std::errc{} || be!=btext.data()+btext.size()) {
            return retry("Face fillet needs a finite radius and integer face numbers");
        }
        if (workspace_.selected_object()!=resizeSource || workspace_.selection_count()!=1) {textEdit_={};set_status("Face fillet: select the original exact object again",true);return false;}
        const auto* object=workspace_.document().find_object(resizeSource);
        if (!object || !tube_target_ok(*object) || is_isolated_out(resizeSource)) {textEdit_={};set_status("Face fillet: source is unavailable",true);return false;}
        auto request=prepare_exact_face_fillet(projectRoot_,workspace_.document(),resizeSource,a,b,radius*0.001);
        if (!request.error.empty()) return retry(request.error);
        textEdit_={};
        return start_exact_edge_fillet(request);
    }
    if (kind == TextEditKind::ExactEdgeFillet || kind == TextEditKind::ExactVariableFillet) {
        const bool variable = kind == TextEditKind::ExactVariableFillet;
        const std::size_t expected = variable ? 8U : 7U;
        std::istringstream input(text); std::vector<double> numbers; std::string token;
        while (input >> token) {
            double value{}; const auto [end, ec] = std::from_chars(token.data(), token.data()+token.size(), value);
            if (ec != std::errc{} || end != token.data()+token.size() || !std::isfinite(value)) {
                return retry("Edge fillet: enter plain finite numbers in mesh-local millimetres");
            }
            numbers.push_back(value * 0.001);
        }
        if (numbers.size()!=expected) {
            return retry(variable ? "Variable fillet needs two radii and six endpoint coordinates in mm" : "Edge fillet needs one radius and six endpoint coordinates in mm");
        }
        if (workspace_.selected_object()!=resizeSource || workspace_.selection_count()!=1) {
            textEdit_={};set_status("Edge fillet: select the original exact object again", true);return false;
        }
        const auto* object=workspace_.document().find_object(resizeSource);
        if (!object || !tube_target_ok(*object) || is_isolated_out(resizeSource)) {textEdit_={};set_status("Edge fillet: source is unavailable",true);return false;}
        const std::size_t base=variable?2U:1U;
        auto request=prepare_exact_edge_fillet(projectRoot_,workspace_.document(),resizeSource,
            {numbers[base],numbers[base+1],numbers[base+2]}, {numbers[base+3],numbers[base+4],numbers[base+5]},
            numbers[0],variable?numbers[1]:numbers[0]);
        if (!request.error.empty()) return retry(request.error);
        textEdit_={};
        return start_exact_edge_fillet(request);
    }
    if (kind == TextEditKind::ExactTubeResize) {
        const auto parsed = parse_tube_spec(text);
        if (!parsed.spec) return retry("Resize Exact Tube: " + parsed.error);
        if (workspace_.selected_object() != resizeSource || workspace_.selection_count() != 1) {
            textEdit_ = {}; set_status("Resize Exact Tube: select the original stock tube again", true); return false;
        }
        if (const auto reason = exact_tube_disabled_reason("mesh.exact_resize_tube"); !reason.empty()) { textEdit_ = {}; set_status(reason, true); return false; }
        textEdit_ = {};
        return start_exact_tube(*parsed.spec, resizeSource);
    }
    if (kind == TextEditKind::ExactCopeGap) {
        const auto gap = parse_fit_up_gap(text);
        if (!gap.meters) return retry("Cope: " + gap.error);
        const auto selected = workspace_.selected_objects();
        const auto primary = workspace_.selected_object();
        if (!primary || selected.size() != 2) { textEdit_ = {}; set_status("Cope: select two tubes (A active)", true); return false; }
        EditorObjectId other = 0;
        for (const EditorObjectId id : selected) if (id != *primary) other = id;
        ExactCopeOptions options;
        options.gapWorldMeters = *gap.meters;
        const TextEditState prompt = textEdit_;
        textEdit_ = {};
        std::string refusal;
        if (start_exact_cope(*primary, other, options, &refusal)) return true;
        if (refusal.empty()) return false;  // already reported (a job is running, Play is active, ...)
        textEdit_ = prompt;
        return retry("Cope: " + refusal);
    }
    if (kind == TextEditKind::ExactMiter) {
        const auto parsed = parse_miter_spec(text);
        if (!parsed.spec) return retry("Plane cut: " + parsed.error);
        const auto primary = workspace_.selected_object();
        if (!primary || workspace_.selection_count() != 1) { textEdit_ = {}; set_status("Plane cut: select one tube", true); return false; }
        const TextEditState prompt = textEdit_;
        textEdit_ = {};
        std::string refusal;
        if (start_exact_miter(*primary, *parsed.spec, &refusal)) return true;
        if (refusal.empty()) return false;
        textEdit_ = prompt;
        return retry("Plane cut: " + refusal);
    }
    textEdit_ = {};
    return false;
}

bool NativeEditorController::start_exact_tube(const TubeSpec& spec, EditorObjectId resizeSource) {
    if (const std::string reason = exact_tube_disabled_reason("polygon.create_exact_tube"); !reason.empty()) { set_status(reason, true); return false; }
    auto created = std::make_shared<std::optional<ExactTubeResult>>();
    const auto id = workspace_.document().allocate_object_id();
    const auto root = projectRoot_;
    const auto settings = ExactCadSettings::from_environment();
    const auto sourceStamp = resizeSource ? exact_operand_stamp(workspace_.document(), resizeSource, resizeSource) : 0;
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
        [this, created, spec, id, resizeSource, sourceStamp](LongJob& job) {
            if (resizeSource && exact_operand_stamp(workspace_.document(), resizeSource, resizeSource) != sourceStamp) {
                set_status("Resize was not applied: the source changed while the tube was built", true, 8.0F); return false;
            }
            const auto* source = resizeSource ? workspace_.document().find_object(resizeSource) : nullptr;
            if (resizeSource && (!source || !source->polygon)) { set_status("Resize source no longer exists", true); return false; }
            if (source) {
                const auto record = exact_tube_record(*source);
                if (!record || !record->cuts.empty()) {
                    set_status("Resize was not applied: the source is no longer an uncut tube", true); return false;
                }
            }
            if (!*created) { set_status(job.label + " produced no geometry", true, 8.0F); return false; }
            auto& result = **created;
            const auto before = capture_selection_snapshot();
            const std::string profile = spec.profile == TubeProfile::Round ? "Round" : spec.profile == TubeProfile::Square ? "Square" : "Rect";
            // Two tubes of one size would be indistinguishable in the Hierarchy: the second is "... #2".
            EditorObject object(id, unique_object_name(workspace_.document(), profile + " tube " + mm_label(spec.width) +
                                        (spec.profile == TubeProfile::Rectangular ? "x" + mm_label(spec.height) : std::string()) +
                                        "x" + mm_label(spec.wall)));
            object.voxelSizeMeters = 1.0F;
            object.polygon = std::move(result.mesh);
            object.polygon->objectId = id;
            refresh_editor_polygon(*object.polygon);
            if (source) {
                object.transform = source->transform; object.parent = source->parent;
                object.voxelScale = source->voxelScale; object.voxelSizeMeters = source->voxelSizeMeters;
                object.flags = source->flags;
                // A new stock mesh cannot safely inherit per-face assignments from a different topology.
            } else object.transform.position = camera_.target;
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
            recompute_layout();
            refresh_menu_state();
            set_status(result.summary + "; exact source kept at " + result.source.relativePath.generic_string() +
                       (resizeSource ? "; resized copy created; original unchanged; reassign materials if needed" : ""));
            return true;
        });
}

bool NativeEditorController::start_exact_cope(EditorObjectId a, EditorObjectId b, const ExactCopeOptions& options,
                                              std::string* refusal) {
    if (const std::string reason = exact_tube_disabled_reason("mesh.exact_cope"); !reason.empty()) { set_status(reason, true); return false; }
    auto request = prepare_exact_cope(projectRoot_, workspace_.document(), a, b, options);
    if (!request.error.empty()) {
        if (refusal != nullptr) *refusal = request.error;
        else set_status("Cope: " + request.error, true, 8.0F);
        return false;
    }
    return start_exact_tube_cut(std::move(request), " (Coped to " + workspace_.document().find_object(b)->name + ")");
}

bool NativeEditorController::start_exact_edge_fillet(const ExactEdgeFilletRequest& request) {
    auto output=std::make_shared<std::optional<ExactEdgeFilletResult>>();
    const auto root=projectRoot_;const auto settings=ExactCadSettings::from_environment();
    return start_long_job(LongJobKind::ImportModel,"Exact CAD edge fillet",longJobThreshold_+1,
        [output,request,root,settings](LongJob& job) {
            if(job.cancelled()) return;
            job.report(0.1F,"OpenCASCADE is building the edge fillet");
            auto result=run_exact_edge_fillet(root,request,settings);
            if(job.cancelled()) return;
            if(!result) {std::lock_guard lock(job.mutex);job.error=result.error;return;}
            output->emplace(std::move(result));job.report(1.0F,"Fillet ready");
        },
        [this,output,request](LongJob& job) {
            auto& document=workspace_.document();
            if(exact_operand_stamp(document,request.object,request.object)!=request.operandStamp) {
                set_status("Fillet not applied: source changed during the operation",true,8.0F);return false;
            }
            const auto* original=document.find_object(request.object);
            if(!original || !*output) {set_status(job.label+" produced no geometry",true,8.0F);return false;}
            auto& result=(**output).imported;
            const auto before=capture_selection_snapshot(); const auto id=document.allocate_object_id();
            EditorObject object(id,original->name+" (CAD fillet)");
            object.polygon=std::move(result.mesh);object.polygon->objectId=id;refresh_editor_polygon(*object.polygon);
            object.transform=original->transform;object.parent=original->parent;object.voxelScale=original->voxelScale;
            object.voxelSizeMeters=original->voxelSizeMeters;object.flags=original->flags;
            auto exact=make_exact_brep_component(result.source);exact.id=1;object.components.push_back(std::move(exact));
            if(result.collision.component) {auto collision=*result.collision.component;collision.id=2;object.components.push_back(std::move(collision));}
            const auto added=workspace_.commands().execute(document,std::make_unique<AddObjectCommand>(std::move(object),"Create CAD edge fillet"));
            if(!added.success) {set_status(added.message,true);return false;}
            if(!viewportSettings_.isolatedObjects.empty())viewportSettings_.isolatedObjects.insert(id);
            workspace_.select_object(id);selectionDomain_=SelectionDomain::Objects;
            workspace_.commands().set_last_selection_transition(before,capture_selection_snapshot());
            frame_selection();refresh_menu_state();
            set_status("OCCT-validated B-rep fillet created; not Boolean-certified. Original unchanged; reassign materials if needed.",false,14.0F);return true;
        });
}

bool NativeEditorController::start_exact_cope_solid(EditorObjectId a, EditorObjectId b) {
    if (const std::string reason = exact_tube_disabled_reason("mesh.exact_cope_solid"); !reason.empty()) { set_status(reason, true); return false; }
    auto request = prepare_exact_cope_to_solid(projectRoot_, workspace_.document(), a, b);
    if (!request.error.empty()) { set_status(request.error, true, 8.0F); return false; }
    return start_exact_tube_cut(std::move(request), " (Coped to supplied solid " + workspace_.document().find_object(b)->name + ")");
}

bool NativeEditorController::start_exact_miter(EditorObjectId a, const ExactMiterSpec& spec, std::string* refusal) {
    if (const std::string reason = exact_tube_disabled_reason("mesh.exact_miter"); !reason.empty()) { set_status(reason, true); return false; }
    auto request = prepare_exact_miter(projectRoot_, workspace_.document(), a, spec);
    if (!request.error.empty()) {
        if (refusal != nullptr) *refusal = request.error;
        else set_status("Plane cut: " + request.error, true, 8.0F);
        return false;
    }
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
            EditorObject result(id, unique_object_name(document, objectA->name + suffix));
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
            // A success with a caveat (clearance, several pieces) is a warning: amber, not the red of a failure.
            const std::string summary = r.boolean.summary + "; the operands are unchanged and stay visible where they are, "
                "overlapping the result (untick Visible on an operand to compare)";
            if (!r.warning.empty() || r.clearanceApplied) set_status_warning(summary, 14.0F);
            else set_status(summary, false, 14.0F);
            return true;
        });
}

} // namespace dve::editor
