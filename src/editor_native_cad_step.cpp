// File > Export Exact CAD (STEP) (exact CAD, first batch).
//
// The selected CAD objects (and CAD pieces of selected groups) go into one STEP file in the
// project's exports/ folder, each as a named solid placed where it sits in the scene, from its
// exact source (not its triangles). A list picks the units and up axis: millimetres and Z up suit
// most CAD programs. brepkernel checks each placed part and reads the file back before replacing
// any existing file, so a failed export writes nothing. Stretched objects are exported converted
// (a general affine map: still solid, but cylinders become freeform surfaces) and the report names
// them; selected objects without an exact source are skipped and named.
#include "dve/editor_native.hpp"

#include <algorithm>
#include <memory>

#include "dve/editor_exact_cad.hpp"
#include "editor_native_long_job.hpp"

namespace dve::editor {
namespace {
std::string file_stem(std::string name) {
    if (name.empty()) name = "cad";
    for (char& c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') c = '_';
    return name;
}
std::string names(const std::vector<std::string>& list) {
    std::string out;
    for (std::size_t i = 0; i < list.size() && i < 4; ++i) out += (i ? ", " : "") + list[i];
    if (list.size() > 4) out += " and " + std::to_string(list.size() - 4) + " more";
    return out;
}
}  // namespace

std::vector<EditorObjectId> NativeEditorController::step_export_objects(std::vector<std::string>* skipped) const {
    const EditorDocument& document = workspace_.document();
    const std::vector<EditorObjectId> roots(workspace_.selected_objects().begin(), workspace_.selected_objects().end());
    std::vector<EditorObjectId> cad;
    for (const EditorObjectId id : collect_editor_object_subtree_ids(document, roots)) {
        const EditorObject* object = document.find_object(id);
        if (!object) continue;
        if (object->polygon && exact_brep_source(*object)) cad.push_back(id);
        else if (skipped && (object->polygon || object->voxels)) skipped->push_back(object->name);
    }
    return cad;
}

bool NativeEditorController::dispatch_step_export_action(std::string_view action) {
    if (action == "file.export_exact_step") {
        if (step_export_objects().empty()) {
            set_status("Select CAD objects (Import Exact CAD or exact Booleans) to export as STEP", true);
            return false;
        }
        contextMenu_ = {};
        contextMenu_.open = true;
        const int itemHeight = 24;
        const int width = 360;
        contextMenu_.width = width;
        const std::vector<std::pair<std::string, std::string>> items{
            {"cad.step.mm_z", "Millimetres, Z up (most CAD programs)"},
            {"cad.step.mm_y", "Millimetres, Y up"},
            {"cad.step.m_z", "Metres, Z up"},
            {"cad.step.m_y", "Metres, Y up (as in this editor)"}};
        const UiRect viewport = layout_.viewport;
        contextMenu_.x = std::clamp(viewport.x + viewport.width / 2 - width / 2, 0, std::max(0, width_ - width));
        contextMenu_.y = std::max(layout_.menuBar.height, viewport.y + 40);
        for (std::size_t i = 0; i < items.size(); ++i) {
            contextMenu_.items.push_back({items[i].first, items[i].second});
            contextMenu_.itemRects.push_back({contextMenu_.x, contextMenu_.y + static_cast<int>(i) * itemHeight, width, itemHeight});
        }
        set_status("Export Exact CAD (STEP): choose units and up axis");
        return true;
    }
    if (action == "cad.step.mm_z") return export_exact_step(true, true);
    if (action == "cad.step.mm_y") return export_exact_step(true, false);
    if (action == "cad.step.m_z") return export_exact_step(false, true);
    if (action == "cad.step.m_y") return export_exact_step(false, false);
    return false;
}

bool NativeEditorController::export_exact_step(bool millimetres, bool zUp) {
    std::vector<std::string> skipped;
    const std::vector<EditorObjectId> ids = step_export_objects(&skipped);
    if (ids.empty()) { set_status("Select CAD objects (Import Exact CAD or exact Booleans) to export as STEP", true); return false; }
    const EditorDocument& document = workspace_.document();
    std::vector<ExactStepPart> parts;
    for (const EditorObjectId id : ids) {
        const EditorObject* object = document.find_object(id);
        const auto placement = exact_world_placement(document, id);
        if (!placement) { set_status(object->name + " has a zero or invalid scale and cannot be exported", true); return false; }
        parts.push_back({*exact_brep_source(*object), object->name, *placement});
    }
    // One part: named after it; several: after the (first) selected object or group.
    const EditorObject* first = workspace_.selected_object() ? document.find_object(*workspace_.selected_object()) : nullptr;
    const std::string stem = file_stem(ids.size() == 1 ? parts.front().name : first ? first->name : "cad-parts");
    const std::filesystem::path relative = std::filesystem::path("exports") / (stem + ".step");
    const std::filesystem::path output = projectRoot_ / relative;
    const auto settings = ExactCadSettings::from_environment();
    const auto root = projectRoot_;
    auto result = std::make_shared<ExactStepExportResult>();
    const std::string format = std::string(millimetres ? "mm" : "m") + ", " + (zUp ? "Z" : "Y") + " up";
    return start_long_job(LongJobKind::ImportModel, "Export Exact CAD (STEP)", longJobThreshold_ + 1,
        [result, parts, output, millimetres, zUp, settings, root](LongJob& job) {
            job.report(0.1F, "brepkernel is writing and checking the STEP file");
            *result = export_exact_cad_step(root, parts, output, millimetres, zUp, settings);
            job.report(1.0F, "Done");
        },
        [this, result, relative, skipped, format](LongJob&) {
            if (!*result) { set_status("STEP export failed, no file written: " + result->error, true, 12.0F); return false; }
            std::vector<std::string> converted;
            for (const ExactStepPartReport& part : result->parts) if (!part.exact) converted.push_back(part.name);
            std::string text = "Exported " + std::to_string(result->parts.size()) + (result->parts.size() == 1 ? " CAD part" : " CAD parts") +
                               " to " + relative.generic_string() + " (" + format + "), read back and checked";
            if (!converted.empty()) text += "; stretched, exported converted (not exact): " + names(converted);
            if (!skipped.empty()) text += "; skipped, no exact source: " + names(skipped);
            lastStepExport_ = relative;
            set_status(text, false, 12.0F);
            return true;
        });
}

}  // namespace dve::editor
