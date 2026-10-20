// CAD Detail (exact CAD, first batch): re-mesh CAD objects from their exact source at a chosen
// accuracy, and say how closely the triangles follow the true surface.
//
// Polygon > CAD Detail... opens a list of accuracies (world millimetres, the largest gap allowed
// between the triangles and the exact surface): 1, 0.1, 0.02 and 0.005 mm, brepkernel's default
// (1/500 of the part's size) or a typed value. The re-mesh runs as a background job and applies as
// one undo step per run; the exact source is unchanged. The inspector shows the result as
// "Exact CAD: mesh within ±0.1 mm".
#include "dve/editor_native.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "dve/editor_exact_cad.hpp"
#include "dve/editor_number_field.hpp"
#include "editor_native_long_job.hpp"

namespace dve::editor {
namespace {
std::string format_millimetres(double metres) {
    char buffer[48];
    const double mm = metres * 1000.0;
    std::snprintf(buffer, sizeof buffer, mm >= 0.1 ? "%.3g mm" : "%.2g mm", mm);
    return buffer;
}

// Fresh modeling identities for a re-meshed object, numbered above everything the object has ever
// used, so old selection-set members cannot be mistaken for new elements.
std::optional<ModelingTopology> renumbered_topology(const CookedPolygonAsset& mesh, ModelingId floor) {
    auto topology = make_modeling_topology(mesh);
    if (!topology || floor <= 1) return topology;
    const ModelingId offset = floor - 1;
    const auto shift = [&](ModelingId& id) { if (id) id += offset; };
    const auto shifted = [&](ModelingId id) { return id ? id + offset : id; };
    for (auto* list : {&topology->vertices, &topology->attributes, &topology->corners, &topology->triangles, &topology->faces})
        for (ModelingId& id : *list) shift(id);
    std::map<std::pair<ModelingId, ModelingId>, ModelingId> edges;
    for (const auto& [key, id] : topology->edges) edges.emplace(std::pair{shifted(key.first), shifted(key.second)}, shifted(id));
    topology->edges = std::move(edges);
    for (auto& face : topology->faceTriangles) for (ModelingId& id : face) shift(id);
    std::set<ModelingId> control;
    for (const ModelingId id : topology->controlEdges) control.insert(shifted(id));
    topology->controlEdges = std::move(control);
    topology->nextId += offset;
    return topology;
}
}  // namespace

std::vector<EditorObjectId> NativeEditorController::selected_exact_objects() const {
    std::vector<EditorObjectId> ids;
    for (const EditorObjectId id : workspace_.selected_objects())
        if (const EditorObject* object = workspace_.document().find_object(id); object && object->polygon && exact_brep_source(*object))
            ids.push_back(id);
    return ids;
}

// Why CAD Detail cannot run now (empty when it can); the menu shows it and the commands refuse with it.
std::string NativeEditorController::cad_detail_disabled_reason() const {
    if (workspace_.mode() != EditorMode::Edit || playSession_.active()) return "Stop Play or Simulate before re-meshing.";
    if (longJob_) return long_job_busy_reason();
    const auto ids = selected_exact_objects();
    if (ids.empty()) return "Select a CAD object (imported with Import Exact CAD) first.";
    for (const EditorObjectId id : ids) {
        const EditorObject* object = workspace_.document().find_object(id);
        if (object->flags.locked) return "Unlock " + object->name + " before changing its detail.";
        if (!object->flags.visible || is_isolated_out(id)) return object->name + " is hidden or isolated out of view.";
        if (object->prefabLink) return object->name + " is part of a prefab instance; unpack it or edit the prefab.";
    }
    return {};
}

std::string NativeEditorController::cad_detail_summary(EditorObjectId id) const {
    const EditorObject* object = workspace_.document().find_object(id);
    if (!object || !exact_brep_source(*object)) return {};
    if (const auto error = exact_mesh_world_error(*object))
        return "Exact CAD: mesh within \xC2\xB1" + format_millimetres(*error);
    return "Exact CAD: default mesh detail (\xE2\x89\x88 1/500 of its size)";
}

bool NativeEditorController::open_cad_detail_menu() {
    if (const std::string reason = cad_detail_disabled_reason(); !reason.empty()) { set_status(reason, true); return false; }
    contextMenu_ = {};
    contextMenu_.open = true;
    const int itemHeight = 24;
    const int width = 330;
    contextMenu_.width = width;
    const std::vector<std::pair<std::string, std::string>> items{
        {"cad.detail.1000", "Coarse: within \xC2\xB1" "1 mm"},
        {"cad.detail.100", "Medium: within \xC2\xB1" "0.1 mm"},
        {"cad.detail.20", "Fine: within \xC2\xB1" "0.02 mm"},
        {"cad.detail.5", "Very fine: within \xC2\xB1" "0.005 mm"},
        {"cad.detail.default", "Default: 1/500 of the part's size"},
        {"cad.detail.custom", "Custom accuracy..."}};
    const UiRect viewport = layout_.viewport;
    contextMenu_.x = std::clamp(viewport.x + viewport.width / 2 - width / 2, 0, std::max(0, width_ - width));
    contextMenu_.y = std::max(layout_.menuBar.height, viewport.y + 40);
    for (std::size_t i = 0; i < items.size(); ++i) {
        contextMenu_.items.push_back({items[i].first, items[i].second});
        contextMenu_.itemRects.push_back({contextMenu_.x, contextMenu_.y + static_cast<int>(i) * itemHeight, width, itemHeight});
    }
    const auto summary = cad_detail_summary(selected_exact_objects().front());
    set_status("CAD Detail: how closely the triangles follow the exact surface (now: " + summary.substr(summary.find(':') + 2) + ")");
    return true;
}

bool NativeEditorController::dispatch_cad_detail_action(std::string_view action) {
    if (action == "polygon.cad_detail") return open_cad_detail_menu();
    if (action == "cad.detail.custom") {
        if (const std::string reason = cad_detail_disabled_reason(); !reason.empty()) { set_status(reason, true); return false; }
        begin_text_edit(TextEditKind::CadDetail, 0, "0.05");
        set_status("Type the largest gap allowed between triangles and the exact surface, in mm (or with a unit: 20um, 0.1mm), then Enter");
        return true;
    }
    if (action == "cad.detail.default") return set_cad_detail(0.0);
    const std::string_view prefix = "cad.detail.";
    if (!action.starts_with(prefix)) return false;
    const std::string micrometres(action.substr(prefix.size()));
    char* end = nullptr;
    const double value = std::strtod(micrometres.c_str(), &end);
    if (!end || *end != '\0' || !(value > 0.0)) return false;
    return set_cad_detail(value / 1.0e6);
}

bool NativeEditorController::commit_cad_detail_text() {
    std::string text = textEdit_.buffer;
    textEdit_ = {};
    bool hasUnit = false;
    for (const char c : text) hasUnit = hasUnit || std::isalpha(static_cast<unsigned char>(c));
    // Plain numbers are millimetres here; "um" and "µm" mean micrometres.
    // The unit must end the text and the number must fill the rest ("20um", not "20dummy").
    for (const std::string_view micro : {"um", "\xC2\xB5m"})
        if (text.size() > micro.size() && text.ends_with(micro)) {
            std::string number = text.substr(0, text.size() - micro.size());
            while (!number.empty() && number.back() == ' ') number.pop_back();
            const NumberFieldParse value = parse_number_field(number, NumberFieldUnit::Count, 0.0);
            if (!value.ok) { set_status("CAD Detail: " + value.error, true); return false; }
            if (!(value.value > 0.0)) { set_status("Accuracy must be above 0", true); return false; }
            return set_cad_detail(value.value / 1.0e6);
        }
    const NumberFieldParse parsed = parse_number_field(hasUnit ? text : text + "mm", NumberFieldUnit::Meters, 0.0);
    if (!parsed.ok || !(parsed.value > 0.0)) { set_status("CAD Detail: " + (parsed.ok ? std::string("accuracy must be above 0") : parsed.error), true); return false; }
    return set_cad_detail(parsed.value);
}

bool NativeEditorController::set_cad_detail(double worldToleranceMeters) {
    if (const std::string reason = cad_detail_disabled_reason(); !reason.empty()) { set_status(reason, true); return false; }
    const auto ids = selected_exact_objects();
    if (worldToleranceMeters > 0.0 && (worldToleranceMeters < 1.0e-7 || worldToleranceMeters > 1.0)) {
        set_status("Choose an accuracy between 0.0001 mm and 1 m", true);
        return false;
    }
    struct Request {
        EditorObjectId id{};
        ExactBrepSource source;
        double deflection{};       // mesh frame
        std::uint64_t hash{};      // the mesh it replaces
    };
    std::vector<Request> requests;
    for (const EditorObjectId id : ids) {
        const EditorObject* object = workspace_.document().find_object(id);
        const double scale = std::max({std::abs(double(object->voxelScale.x)), std::abs(double(object->voxelScale.y)),
                                       std::abs(double(object->voxelScale.z)), 1.0e-12});
        requests.push_back({id, *exact_brep_source(*object), worldToleranceMeters / scale, object->polygon->contentHash});
    }
    auto results = std::make_shared<std::vector<ExactCadRemeshResult>>();
    const auto root = projectRoot_;
    const auto settings = ExactCadSettings::from_environment();
    const std::string accuracy = worldToleranceMeters > 0.0 ? "\xC2\xB1" + format_millimetres(worldToleranceMeters) : std::string("default detail");
    return start_long_job(LongJobKind::ImportModel, "CAD Detail (" + accuracy + ")", longJobThreshold_ + 1,
        [results, requests, root, settings](LongJob& job) {
            for (std::size_t i = 0; i < requests.size(); ++i) {
                if (job.cancelled()) return;
                job.report(static_cast<float>(i) / static_cast<float>(requests.size()), "brepkernel is re-meshing the exact surface");
                results->push_back(remesh_exact_cad(root, requests[i].source, requests[i].id, requests[i].deflection, settings));
            }
            job.report(1.0F, "Re-meshed");
        },
        [this, results, requests, accuracy](LongJob& job) {
            if (results->size() != requests.size()) { set_status(job.label + " was cancelled", true, 6.0F); return false; }
            auto& document = workspace_.document();
            auto compound = std::make_unique<CompoundCommand>("CAD Detail");
            std::uint64_t triangles = 0;
            std::size_t changedObjects = 0;
            for (std::size_t i = 0; i < requests.size(); ++i) {
                auto& result = (*results)[i];
                const Request& request = requests[i];
                if (!result) { set_status(result.error + " (nothing was changed)", true, 10.0F); return false; }
                const EditorObject* object = document.find_object(request.id);
                if (!object || !object->polygon || object->polygon->contentHash != request.hash) {
                    set_status(job.label + " was not applied: an object changed while it ran. Run it again", true, 8.0F);
                    return false;
                }
                CookedPolygonAsset mesh = std::move(result.mesh);
                mesh.objectId = request.id;
                // The object's materials carry over (one surface group, as for exact Booleans).
                if (!object->polygon->materials.empty()) {
                    mesh.materials = object->polygon->materials;
                    mesh.materialBindings = object->polygon->materialBindings;
                    mesh.images = object->polygon->images;
                    mesh.samplers = object->polygon->samplers;
                    mesh.textures = object->polygon->textures;
                    for (auto& submesh : mesh.submeshes) submesh.materialIndex = 0;
                }
                refresh_editor_polygon(mesh);
                // The same triangles at the same recorded accuracy as now: no step for it.
                if (mesh.contentHash == object->polygon->contentHash) {
                    bool sameAccuracy = false;
                    for (const Component& component : object->components) {
                        if (component.type != kExactBrepComponent) continue;
                        const auto recorded = [&](const char* key) {
                            const auto it = component.properties.find(key);
                            const double* value = it == component.properties.end() ? nullptr : std::get_if<double>(&it->second);
                            return value ? *value : -1.0;
                        };
                        sameAccuracy = recorded("deflection") == result.deflectionMeters && recorded("angular") == result.angularRadians;
                        break;
                    }
                    if (sameAccuracy) continue;
                }
                ++changedObjects;
                std::optional<ModelingTopology> topology;
                if (object->modelingTopology) {
                    topology = renumbered_topology(mesh, std::max(object->modelingNextId, object->modelingTopology->nextId));
                    if (!topology) { set_status("The re-meshed object could not get element identities", true); return false; }
                }
                triangles += mesh.indices.size() / 3;
                compound->add(std::make_unique<ReplacePolygonCommand>(request.id, *object->polygon, std::move(mesh), "CAD Detail", 0,
                                                                      std::nullopt, true, topology));
                // Remember the accuracy on the exact source component.
                for (const Component& component : object->components) {
                    if (component.type != kExactBrepComponent) continue;
                    const auto old = [&](const char* key) -> std::optional<ComponentValue> {
                        const auto it = component.properties.find(key);
                        return it == component.properties.end() ? std::nullopt : std::optional<ComponentValue>(it->second);
                    };
                    compound->add(std::make_unique<SetComponentPropertyCommand>(request.id, component.id, "deflection", old("deflection"),
                                                                                ComponentValue{result.deflectionMeters}, "CAD Detail"));
                    compound->add(std::make_unique<SetComponentPropertyCommand>(request.id, component.id, "angular", old("angular"),
                                                                                ComponentValue{result.angularRadians}, "CAD Detail"));
                    break;
                }
                // UV seams and pins belong to the old triangles.
                for (const Component& component : object->components)
                    if (component.type == kUvAuthoringComponent)
                        compound->add(std::make_unique<RemoveComponentCommand>(request.id, component.id, "CAD Detail"));
            }
            if (changedObjects == 0) {
                set_status("CAD Detail: the mesh is already at that accuracy; nothing changed", false, 6.0F);
                return true;
            }
            const auto before = capture_selection_snapshot();
            if (selectionDomain_ == SelectionDomain::Polygons) set_selection_domain(SelectionDomain::Objects);
            const CommandResult applied = workspace_.commands().execute(document, std::move(compound));
            if (!applied.success) { set_status(applied.message, true, 8.0F); return false; }
            workspace_.commands().set_last_selection_transition(before, capture_selection_snapshot());
            refresh_menu_state();
            const std::string summary = cad_detail_summary(requests.front().id);
            set_status("Re-meshed " + std::to_string(requests.size()) + " CAD object" + (requests.size() == 1 ? "" : "s") + " (" +
                       std::to_string(triangles) + " triangles). " + summary);
            return true;
        });
}

}  // namespace dve::editor
