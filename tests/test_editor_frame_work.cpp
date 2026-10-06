// Per-frame work in NativeEditorController::update() and the idle-redraw signal.
//
// update() used to refresh a camera target for every scene object on every frame (about
// 1.4 ms at 10,000 objects) although rigs only read the targets they follow or look at.
// It now refreshes just those, and drops a target whose object was deleted so a rig stops
// tracking it. animating() tells hosts when they may redraw less often.
#include "dve/editor_native.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace dve;
using namespace dve::editor;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool near(Float3 a, Float3 b) {
    return std::fabs(a.x - b.x) < 1.0e-3F && std::fabs(a.y - b.y) < 1.0e-3F && std::fabs(a.z - b.z) < 1.0e-3F;
}

std::unique_ptr<NativeEditorController> make_controller() {
    auto controller = std::make_unique<NativeEditorController>(EditorWorkspace(make_native_editor_demo_document()));
    controller->resize(1280, 800);
    return controller;
}

// An orbit rig that follows an object keeps aiming at it as it moves; once the object is
// deleted the rig falls back to its authored target instead of the last position.
void test_rig_tracks_only_named_targets() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    require(!controller.workspace().document().objects().empty(), "demo document has no objects");
    const EditorObjectId objectId = controller.workspace().document().objects().begin()->first;

    const camera::CameraRigId rigId = controller.create_camera_rig_from_view("Tracker");
    require(rigId != 0U, "could not create a camera rig");
    camera::CameraRig* rig = controller.camera_director().find_rig(rigId);
    require(rig != nullptr, "rig not found");
    rig->mode = camera::CameraRigMode::Orbit;
    rig->followTarget = objectId;
    rig->framing.localOffset = {};
    rig->framing.lookAheadSeconds = 0.0F;
    rig->framing.deadZoneFraction = 0.0F;
    rig->framing.softZoneFraction = 0.0F;
    rig->framing.aimDampingSeconds = 0.0F;
    rig->framing.positionDampingSeconds = 0.0F;
    const Float3 authoredTarget = rig->authoredPose.target;

    controller.update(0.016F);
    const Float3 start = controller.workspace().document().find_object(objectId)->transform.position;
    require(near(controller.camera_director().current_pose().target, start), "rig does not look at its target");

    const Float3 moved{start.x + 5.0F, start.y + 1.0F, start.z - 2.0F};
    controller.workspace().document().find_object(objectId)->transform.position = moved;
    controller.update(0.016F);
    require(near(controller.camera_director().current_pose().target, moved), "rig did not follow its target's move");

    require(controller.workspace().document().remove_object(objectId), "could not delete the target object");
    controller.update(0.016F);
    require(near(controller.camera_director().current_pose().target, authoredTarget),
            "rig kept tracking a deleted object");
    std::printf("camera targets: OK\n");
}

// update() cost no longer grows with objects no rig names.
void test_update_cost_with_many_objects() {
    EditorDocument document = make_native_editor_demo_document();
    for (int i = 0; i < 10000; ++i) {
        EditorObject object;
        object.name = "Empty " + std::to_string(i);
        object.transform.position = {static_cast<float>(i % 100), 0.0F, static_cast<float>(i / 100)};
        (void)document.add_object(std::move(object));
    }
    NativeEditorController controller{EditorWorkspace(std::move(document))};
    controller.resize(1280, 800);
    for (int i = 0; i < 5; ++i) controller.update(0.008F);
    std::vector<double> samples;
    for (int i = 0; i < 51; ++i) {
        const auto start = std::chrono::steady_clock::now();
        controller.update(0.008F);
        samples.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
    }
    std::sort(samples.begin(), samples.end());
    std::printf("update() with 10,004 objects: median %.1f us\n", samples[samples.size() / 2]);
    // Was ~1,450 us (Release); generous bound so slow or instrumented builds still pass.
    require(samples[samples.size() / 2] < 400.0, "update() still scales with the object count");
}

void test_animating_signal() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    controller.update(0.016F);
    require(!controller.animating(), "an idle editor reports that it is animating");

    // A sounding synth voice keeps the editor at full rate (the meters and piano move).
    require(controller.dispatch_action("window.toggle_synth"), "could not open the synth");
    controller.key_down("z", false, false, false);
    std::vector<float> audio(4096U * 2U);
    controller.synthesizer().render(audio);
    require(controller.animating(), "a sounding synth voice is not reported as animating");
    controller.key_up("z", false, false, false);
    for (int i = 0; i < 64 && controller.animating(); ++i) controller.synthesizer().render(audio);
    require(!controller.animating(), "the editor stays animating after the voice released");

    require(controller.dispatch_action("physics.simulate"), "could not start Simulate");
    require(controller.animating(), "a Simulate session is not reported as animating");
    require(controller.dispatch_action("physics.stop"), "could not stop Simulate");
    require(!controller.animating(), "the editor stays animating after Simulate stopped");
    std::printf("animating signal: OK\n");
}

} // namespace

int main() {
    try {
        test_rig_tracks_only_named_targets();
        test_update_cost_with_many_objects();
        test_animating_signal();
        std::printf("editor frame work tests passed\n");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "dve_editor_frame_work_tests: FAIL: %s\n", error.what());
        return 1;
    }
}
