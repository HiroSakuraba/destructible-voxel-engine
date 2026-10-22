// View > Measure and exact CAD descriptions (exact CAD, first batch).
//
// Measure: click two points; the status bar and the viewport show the distance between them and
// its X/Y/Z parts. Points snap like a held-V move (vertices, edge midpoints, circle centres,
// corners, the surface). On CAD objects the points come from the exact shape: corners, edge
// midpoints and circle centres from brepkernel `describe`, surface clicks projected onto the exact
// surface by brepkernel `query`, which also names the face ("Hole Ø 12.000 mm"). CAD readouts are
// in millimetres; everything else stays in metres. Esc (or View > Measure again) ends.
//
// Descriptions are read once per exact source (by content hash) on a background thread, so exact
// snapping is ready without blocking the editor.
#include "dve/editor_native.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string_view>
#include <cstdio>
#include <future>
#include <memory>
#include <thread>

#include "dve/editor_exact_cad.hpp"
#include "dve/voxel_metric.hpp"
#include "editor_native_long_job.hpp"

namespace dve::editor {

struct NativeEditorController::CadDescribeTask {
    std::string sha;
    std::future<ExactCadDescription> result;
};

namespace {
std::string millimetres(double metres) {
    char buffer[48];
    std::snprintf(buffer, sizeof buffer, "%.3f mm", metres * 1000.0);
    return buffer;
}
std::string metres_text(double metres) {
    char buffer[48];
    std::snprintf(buffer, sizeof buffer, "%.4g m", metres);
    return buffer;
}
// The object's world scale when it is the same on all axes (lengths on the exact shape scale by
// it); nullopt for stretched objects.
std::optional<double> uniform_scale(const EditorObject& object) {
    const double x = std::abs(double(object.voxelScale.x)), y = std::abs(double(object.voxelScale.y)),
                 z = std::abs(double(object.voxelScale.z));
    if (std::abs(x - y) > 1.0e-6 * x || std::abs(x - z) > 1.0e-6 * x) return std::nullopt;
    return x;
}
}  // namespace

std::shared_ptr<const ExactCadDescription> NativeEditorController::exact_cad_description(EditorObjectId id) const {
    const EditorObject* object = workspace_.document().find_object(id);
    if (!object || !object->polygon) return nullptr;
    const auto source = exact_brep_source(*object);
    if (!source) return nullptr;
    const auto found = cadDescriptions_.find(source->sha256);
    return found == cadDescriptions_.end() ? nullptr : found->second;
}

void NativeEditorController::poll_cad_descriptions() {
    if (cadDescribeTask_) {
        if (cadDescribeTask_->result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        ExactCadDescription description = cadDescribeTask_->result.get();
        const std::string sha = cadDescribeTask_->sha;
        cadDescribeTask_.reset();
        if (description) cadDescriptions_[sha] = std::make_shared<const ExactCadDescription>(std::move(description));
        else cadDescriptionErrors_[sha] = description.error;
        cadDescribeScanDone_ = false;   // more sources may be waiting
    }
    if (projectRoot_.empty()) return;
    // Scanning every object each frame cost ~100 us with 10,000 objects: scan only when the scene
    // may have changed (object count, undo history, document) since the last scan found nothing.
    const EditorDocument& document = workspace_.document();
    const auto& commands = workspace_.commands();
    std::uint64_t key = 1469598103934665603ULL;
    for (const std::uint64_t part : {static_cast<std::uint64_t>(document.objects().size()), static_cast<std::uint64_t>(commands.size()),
                                     static_cast<std::uint64_t>(commands.can_undo()) | (static_cast<std::uint64_t>(commands.can_redo()) << 1U),
                                     static_cast<std::uint64_t>(std::hash<std::string_view>{}(commands.undo_label())),
                                     static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&document))}) {
        key ^= part;
        key *= 1099511628211ULL;
    }
    if (cadDescribeScanDone_ && key == cadDescribeScanKey_) return;
    cadDescribeScanKey_ = key;
    cadDescribeScanDone_ = true;
    // One source at a time, visible CAD objects only.
    for (const auto& [id, object] : document.objects()) {
        if (!object.polygon || !object.flags.visible) continue;
        const auto source = exact_brep_source(object);
        if (!source || source->sha256.empty() || cadDescriptions_.contains(source->sha256) || cadDescriptionErrors_.contains(source->sha256))
            continue;
        auto task = std::make_shared<CadDescribeTask>();
        task->sha = source->sha256;
        task->result = std::async(std::launch::async, [root = projectRoot_, source = *source, settings = ExactCadSettings::from_environment()] {
            return describe_exact_cad(root, source, settings);
        });
        cadDescribeTask_ = std::move(task);
        cadDescribeScanDone_ = false;
        return;
    }
}

bool NativeEditorController::wait_for_cad_descriptions(double seconds) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    cadDescribeScanDone_ = false;   // objects may have been added or changed directly
    do {
        poll_cad_descriptions();
        if (!cadDescribeTask_) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < until);
    return false;
}

void NativeEditorController::set_measuring(bool on) {
    measureActive_ = on;
    measurePoints_.clear();
    measureHover_.reset();
    if (on) {
        if (placementMode_ != PlacementTarget::NoTarget) finish_placement(true);
        floodFillArmed_ = false;
        std::string note;
        for (const auto& [sha, error] : cadDescriptionErrors_)
            if (error.find("update brepkernel") != std::string::npos) note = " (" + error + ")";
        set_status("Measure: click the first point (Esc ends)" + note, !note.empty(), 0.0F);
    } else {
        set_status("Measure ended", false, 2.0F);
    }
    refresh_menu_state();
}

// A different document: points from the old scene must not be measured against the new one.
void NativeEditorController::end_measure_for_new_document() {
    if (!measureActive_ && measurePoints_.empty()) return;
    measureActive_ = false;
    measurePoints_.clear();
    measureHover_.reset();
    refresh_menu_state();
}

void NativeEditorController::update_measure_hover(int x, int y) {
    measureHover_ = find_snap_target(x, y, std::nullopt, true);
}

bool NativeEditorController::measure_at(int x, int y) {
    if (!measureActive_) return false;
    auto target = find_snap_target(x, y, std::nullopt, true);
    if (!target) { set_status("Measure: click on an object", true, 3.0F); return false; }
    NativeMeasurePoint point;
    point.world = target->point;
    point.object = target->object;
    point.kind = target->kind;
    if (target->kind == SnapTargetKind::Surface)  // the placement offset is for placing, not measuring
        point.world = subtract(point.world, multiply(target->normal, placementOffsetMeters_));
    point.what = snap_target_name(target->kind);
    const EditorObject* object = workspace_.document().find_object(target->object);
    const auto source = object && object->polygon ? exact_brep_source(*object) : std::nullopt;
    point.cad = source.has_value();
    const auto description = point.cad ? exact_cad_description(target->object) : nullptr;
    if (description && target->kind != SnapTargetKind::Surface) {
        point.exact = true;   // an exact corner, edge midpoint or circle centre
        if (target->kind == SnapTargetKind::CircleCentre) {
            const ExactCadCircle* nearest = nullptr;
            float best = 0.0F;
            for (const ExactCadCircle& c : description->circles) {
                const Float3 w = voxel_to_world(object->transform, object->voxelScale, Float3{float(c.centre[0]), float(c.centre[1]), float(c.centre[2])});
                const float d = length(subtract(w, point.world));
                if (!nearest || d < best) { nearest = &c; best = d; }
            }
            if (nearest) {
                if (const auto scale = uniform_scale(*object))
                    point.what = "circle centre \xC3\x98 " + millimetres(2.0 * nearest->radius * *scale);
            }
        }
    }
    if (measurePoints_.size() >= 2) measurePoints_.clear();
    measurePoints_.push_back(point);
    const std::size_t index = measurePoints_.size() - 1;
    set_status(measure_readout(), false, 0.0F);
    if (!point.cad || target->kind != SnapTargetKind::Surface) return true;

    // A CAD surface click: project onto the exact surface and name the face (background, ~1 s).
    const Float3 local = world_to_voxel(object->transform, object->voxelScale, point.world);
    const ExactPoint query{double(local.x), double(local.y), double(local.z)};
    const auto settings = ExactCadSettings::from_environment();
    const auto root = projectRoot_;
    struct Answer { ExactCadQueryResult query; std::shared_ptr<const ExactCadDescription> description; std::string describeError; };
    auto answer = std::make_shared<Answer>();
    answer->description = description;
    const ExactBrepSource exact = *source;
    const EditorObjectId objectId = target->object;
    const Float3 clicked = point.world;
    return start_long_job(LongJobKind::ReadModel, "Measuring the exact surface", longJobThreshold_ + 1,
        [answer, exact, query, settings, root](LongJob& job) {
            job.report(0.1F, "brepkernel is finding the exact surface point");
            if (!answer->description) {
                auto d = describe_exact_cad(root, exact, settings);
                if (d) answer->description = std::make_shared<const ExactCadDescription>(std::move(d));
                else answer->describeError = d.error;
            }
            if (job.cancelled()) return;
            answer->query = query_exact_cad(root, exact, {query}, settings);
            job.report(1.0F, "Done");
        },
        [this, answer, exact, index, objectId, clicked](LongJob&) {
            if (answer->description && !cadDescriptions_.contains(exact.sha256)) cadDescriptions_[exact.sha256] = answer->description;
            // The click may have been superseded (Esc, a new measurement): only update that point.
            if (!measureActive_ || index >= measurePoints_.size() || measurePoints_[index].object != objectId ||
                length(subtract(measurePoints_[index].world, clicked)) > 1.0e-9F) return true;
            NativeMeasurePoint& p = measurePoints_[index];
            if (!answer->query || answer->query.hits.empty() || !answer->query.hits.front().found) {
                const std::string why = answer->query ? std::string("no exact surface point") : answer->query.error;
                p.what = "surface (mesh: " + why + ")";
                set_status(measure_readout(), answer->query.unsupported, 0.0F);
                return true;
            }
            const EditorObject* object = workspace_.document().find_object(objectId);
            if (!object) return true;
            const ExactCadQueryHit& hit = answer->query.hits.front();
            p.world = voxel_to_world(object->transform, object->voxelScale, Float3{float(hit.closest[0]), float(hit.closest[1]), float(hit.closest[2])});
            p.exact = true;
            p.what = "surface";
            if (answer->description && hit.face >= 0 && std::size_t(hit.face) < answer->description->faces.size()) {
                const auto scale = uniform_scale(*object);
                p.what = describe_exact_face(answer->description->faces[std::size_t(hit.face)], scale.value_or(1.0));
                if (!scale && answer->description->faces[std::size_t(hit.face)].type != "plane") p.what += " before stretching";
            }
            set_status(measure_readout(), false, 0.0F);
            return true;
        });
}

std::string NativeEditorController::measure_readout() const {
    if (!measureActive_) return {};
    if (measurePoints_.empty()) return "Measure: click the first point (Esc ends)";
    const NativeMeasurePoint& a = measurePoints_.front();
    if (measurePoints_.size() == 1) return "Measure: " + a.what + (a.exact ? " (exact)" : "") + "; click the second point";
    const NativeMeasurePoint& b = measurePoints_[1];
    const Float3 d = subtract(b.world, a.world);
    const bool cad = a.cad || b.cad;
    const auto text = [&](double metres) { return cad ? millimetres(metres) : metres_text(metres); };
    std::string out = "Distance " + text(length(d)) + "  (X " + text(std::abs(d.x)) + ", Y " + text(std::abs(d.y)) + ", Z " +
                      text(std::abs(d.z)) + ")";
    out += "  from " + a.what + " to " + b.what;
    if (cad && a.exact && b.exact) out += "; exact";
    else if (cad) out += "; a point is on the triangles, not the exact shape";
    return out;
}

}  // namespace dve::editor
