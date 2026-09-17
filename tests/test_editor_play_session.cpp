#include "dve/editor_play_session.hpp"
#include "dve/editor_native.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace dve;
using namespace dve::editor;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool near(Float3 a, Float3 b, float epsilon = 1.0e-6F) {
    return std::abs(a.x - b.x) <= epsilon && std::abs(a.y - b.y) <= epsilon &&
           std::abs(a.z - b.z) <= epsilon;
}

EditorDocument make_session_document() {
    EditorDocument document("Play Session Fixture");

    EditorObject floor(1U, "Floor");
    floor.flags.anchored = true;
    floor.voxelSizeMeters = 1.0F;
    floor.transform.position = {0.0F, -2.0F, 0.0F};
    floor.voxels->fill_brick({0, 0, 0}, kDefaultSurfaceMaterial);
    document.add_object(std::move(floor));

    EditorObject dynamic(2U, "Dynamic");
    dynamic.flags.anchored = false;
    dynamic.voxelSizeMeters = 0.5F;
    dynamic.transform.position = {1.0F, 8.0F, 3.0F};
    dynamic.voxels->set_voxel({0, 0, 0}, kDefaultSurfaceMaterial);
    document.add_object(std::move(dynamic));
    document.mark_clean();
    return document;
}

void test_fixed_step_pause_and_restore() {
    EditorWorkspace workspace(make_session_document());
    workspace.select_object(2U);
    EditorMaterialLibrary materials = EditorMaterialLibrary::make_default();
    EditorPlaySession session;
    EditorPlaySessionConfig config;
    config.fixedDeltaSeconds = 1.0F / 60.0F;
    config.maximumSubstepsPerUpdate = 4U;

    const Float3 original = workspace.document().find_object(2U)->transform.position;
    std::string error;
    require(session.start(workspace, materials, EditorMode::Simulate, config, {}, &error), error.c_str());
    require(workspace.mode() == EditorMode::Simulate && workspace.has_pre_simulation_snapshot(),
            "session did not enter Simulate with a snapshot");
    require(session.update(workspace, 1.0F / 30.0F, &error), error.c_str());
    require(session.telemetry().fixedTickCount == 2U && session.telemetry().stepsLastUpdate == 2U,
            "fixed-step accumulator did not execute two ticks");
    const Float3 moved = workspace.document().find_object(2U)->transform.position;
    require(moved.y < original.y, "reference physics did not update the dynamic object");

    require(session.pause(&error), error.c_str());
    const Float3 paused = workspace.document().find_object(2U)->transform.position;
    require(session.update(workspace, 1.0F, &error), error.c_str());
    require(near(workspace.document().find_object(2U)->transform.position, paused),
            "paused session advanced during update");
    require(session.single_step(workspace, &error), error.c_str());
    require(workspace.document().find_object(2U)->transform.position.y < paused.y,
            "single-step did not advance exactly one fixed tick");

    workspace.select_object(1U);
    require(session.stop(workspace, &error), error.c_str());
    require(workspace.mode() == EditorMode::Edit && !workspace.has_pre_simulation_snapshot(),
            "stop did not return to Edit and consume the snapshot");
    require(near(workspace.document().find_object(2U)->transform.position, original),
            "stop did not restore the entry document");
    require(workspace.selected_object() && *workspace.selected_object() == 2U,
            "stop did not restore the entry selection");
    require(!workspace.document().dirty(), "restored document changed its dirty state");
}

void test_accept_runtime_changes() {
    EditorWorkspace workspace(make_session_document());
    EditorMaterialLibrary materials = EditorMaterialLibrary::make_default();
    EditorPlaySession session;
    std::string error;
    require(session.start(workspace, materials, EditorMode::Simulate, {}, {}, &error), error.c_str());
    require(session.update(workspace, 1.0F / 30.0F, &error), error.c_str());
    const Float3 runtimePosition = workspace.document().find_object(2U)->transform.position;
    require(session.accept_runtime_changes(workspace, &error), error.c_str());
    require(workspace.mode() == EditorMode::Edit && workspace.document().dirty(),
            "accepted runtime state was not promoted to a dirty edit document");
    require(near(workspace.document().find_object(2U)->transform.position, runtimePosition),
            "accept_runtime_changes restored instead of retaining runtime state");
}

void test_bounded_catch_up() {
    EditorWorkspace workspace(make_session_document());
    EditorMaterialLibrary materials = EditorMaterialLibrary::make_default();
    EditorPlaySession session;
    EditorPlaySessionConfig config;
    config.fixedDeltaSeconds = 0.01F;
    config.maximumAccumulatedSeconds = 0.05F;
    config.maximumSubstepsPerUpdate = 2U;
    std::string error;
    require(session.start(workspace, materials, EditorMode::Simulate, config, {}, &error), error.c_str());
    require(session.update(workspace, 2.0F, &error), error.c_str());
    require(session.telemetry().stepsLastUpdate == 2U,
            "catch-up limit did not bound the number of fixed steps");
    require(session.telemetry().droppedSeconds > 1.9,
            "discarded wall-clock debt was not recorded");
    require(session.stop(workspace, &error), error.c_str());
}

#ifdef DVE_HAVE_LUA
void test_lua_input_hud_and_log(const std::filesystem::path& root) {
    std::filesystem::create_directories(root / "scripts");
    const std::filesystem::path script = root / "scripts" / "main.lua";
    std::ofstream out(script);
    out << R"LUA(
world.log("startup reached")
world.hud_set_tools({{id="hammer", label="Hammer", enabled=true}})
world.on_tick(function(dt)
    if world.is_action_pressed("jump") then
        local id = world.find_by_name("Dynamic")
        world.set_position(id, 4, 12, 6)
        world.hud_set_interaction_prompt("jump accepted")
    end
end)
)LUA";
    out.close();

    EditorWorkspace workspace(make_session_document());
    EditorMaterialLibrary materials = EditorMaterialLibrary::make_default();
    EditorPlaySession session;
    EditorPlaySessionConfig config;
    config.projectRoot = root;
    config.startupScript = script;
    bool sawStartupLog = false;
    std::string error;
    require(session.start(
        workspace, materials, EditorMode::Play, config,
        [&](EditorLogLevel level, std::string text) {
            require(level != EditorLogLevel::Error, "Lua startup emitted an error");
            if (text == "startup reached") sawStartupLog = true;
        }, &error), error.c_str());
    require(session.telemetry().scriptHostAvailable && session.telemetry().startupScriptLoaded,
            "Lua host or startup script did not become active");
    require(sawStartupLog, "world.log was not routed to the editor log sink");
    session.set_action_pressed("jump", true);
    require(session.update(workspace, config.fixedDeltaSeconds, &error), error.c_str());
    const Float3 scripted = workspace.document().find_object(2U)->transform.position;
    require(std::abs(scripted.x - 4.0F) < 1.0e-5F && std::abs(scripted.y - 12.0F) < 1.0e-5F,
            "Lua did not consume the editor input snapshot");
    require(session.hud().interactionPrompt == "jump accepted", "Lua HUD prompt was not surfaced");
    require(session.hud().selectedTool && session.hud().selectedTool->id == "hammer",
            "Lua HUD tool state was not surfaced");
    require(session.stop(workspace, &error), error.c_str());
}

void test_failed_lua_start_restores_edit_state(const std::filesystem::path& root) {
    std::filesystem::create_directories(root / "scripts");
    const std::filesystem::path script = root / "scripts" / "broken.lua";
    std::ofstream out(script);
    out << "this is not valid lua !!!\n";
    out.close();

    EditorWorkspace workspace(make_session_document());
    workspace.select_object(2U);
    EditorMaterialLibrary materials = EditorMaterialLibrary::make_default();
    EditorPlaySession session;
    EditorPlaySessionConfig config;
    config.projectRoot = root;
    config.startupScript = script;
    const Float3 original = workspace.document().find_object(2U)->transform.position;
    std::string error;
    require(!session.start(workspace, materials, EditorMode::Play, config, {}, &error),
            "invalid startup script unexpectedly started Play");
    require(!error.empty(), "failed startup did not report an error");
    require(workspace.mode() == EditorMode::Edit && !workspace.has_pre_simulation_snapshot(),
            "failed startup left the workspace outside Edit mode");
    require(near(workspace.document().find_object(2U)->transform.position, original),
            "failed startup did not restore the entry document");
    require(workspace.selected_object() && *workspace.selected_object() == 2U,
            "failed startup did not restore the entry selection");
}
#endif

void test_native_controller_actions_and_camera() {
    NativeEditorController controller{EditorWorkspace(make_session_document())};
    const EditorCamera entryCamera = controller.camera();
    require(controller.dispatch_action("physics.play"), "native Play action failed");
    require(controller.play_session().active() && controller.play_camera_possessed(),
            "native controller did not start or possess the play session");
    const std::size_t objectCount = controller.workspace().document().objects().size();
    require(!controller.dispatch_action("create.empty"),
            "active Play allowed an authored-content mutation");
    require(controller.workspace().document().objects().size() == objectCount,
            "blocked authored-content action changed the play document");
    controller.key_down("w", false, false, false);
    require(controller.play_session().input().axes.at("move_y") == 1.0F,
            "native key routing did not produce a movement axis");
    controller.key_up("w", false, false, false);
    require(controller.play_session().input().axes.at("move_y") == 0.0F,
            "native key release left stale movement input");
    require(controller.dispatch_action("play.pause"), "native Pause action failed");
    require(controller.play_session().paused(), "native Pause did not pause the session");
    require(controller.dispatch_action("play.step"), "native Step action failed");
    require(controller.play_session().telemetry().fixedTickCount == 1U,
            "native Step did not advance one fixed tick");
    require(controller.dispatch_action("play.eject"), "native Eject action failed");
    require(!controller.play_camera_possessed(), "native Eject left the play camera possessed");
    require(controller.dispatch_action("play.eject"), "native Possess action failed");
    require(controller.play_camera_possessed(), "native Possess did not restore the play camera");
    require(controller.dispatch_action("physics.stop"), "native Stop action failed");
    require(controller.workspace().mode() == EditorMode::Edit && !controller.play_session().active(),
            "native Stop did not restore edit mode");
    require(near(controller.camera().position, entryCamera.position) && near(controller.camera().target, entryCamera.target),
            "native Stop did not restore the entry editor camera");
}

} // namespace

int main() {
    try {
        test_fixed_step_pause_and_restore();
        test_accept_runtime_changes();
        test_bounded_catch_up();
#ifdef DVE_HAVE_LUA
        const auto root = std::filesystem::temp_directory_path() / "dve_editor_play_session_lua";
        std::filesystem::remove_all(root);
        test_lua_input_hud_and_log(root);
        test_failed_lua_start_restores_edit_state(root);
        std::filesystem::remove_all(root);
#endif
        test_native_controller_actions_and_camera();
        std::cout << "dve_editor_play_session_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_editor_play_session_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
