#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

#include "dve/editor_control_rig.hpp"
#if defined(DVE_V185_NATIVE_INTEGRATION_TEST)
#include "dve/editor_native.hpp"
#endif

namespace {
using namespace dve;
using namespace dve::editor;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

ControlRigAssetPaths paths_for(const std::filesystem::path& root, std::string_view stem) {
    return {root / (std::string(stem) + ".dverig"),
            root / (std::string(stem) + ".dverigui"),
            root / (std::string(stem) + ".dveskeleton")};
}

void write_fixture(const ControlRigAssetPaths& paths) {
    const SkeletonAsset skeleton = make_control_rig_editor_demo_skeleton();
    std::string error;
    require(write_dveskeleton(paths.skeleton, skeleton, &error), error);
    const auto saved = save_control_rig_pair_transactional(
        paths, skeleton, make_control_rig_editor_demo_document());
    require(saved.success, saved.error);
}

void test_paired_transaction_and_rollback(const std::filesystem::path& root) {
    const auto paths = paths_for(root, "transaction_arm");
    write_fixture(paths);
    const std::string rigBefore = read_bytes(paths.rig);
    const std::string layoutBefore = read_bytes(paths.layout);

    ControlRigAuthoringSession edited(make_control_rig_editor_demo_document());
    ControlRigControl extra;
    extra.name = "Rollback Probe";
    std::string error;
    require(edited.add_control(extra, {700.0F, 300.0F}, &error) != 0U, error);
    ControlRigPairSaveOptions injected;
    injected.failAfterRuntimeCommit = true;
    const auto failed = save_control_rig_pair_transactional(
        paths, make_control_rig_editor_demo_skeleton(), edited.document(), injected);
    require(!failed.success && failed.error.find("injected") != std::string::npos,
            "paired save failure injection did not fail at the commit boundary");
    require(read_bytes(paths.rig) == rigBefore, "failed paired save changed the runtime rig");
    require(read_bytes(paths.layout) == layoutBefore, "failed paired save changed the authoring layout");

    const auto saved = save_control_rig_pair_transactional(
        paths, make_control_rig_editor_demo_skeleton(), edited.document());
    require(saved.success, saved.error);
    ControlRigAuthoringDocument loaded;
    require(load_control_rig_pair(paths, make_control_rig_editor_demo_skeleton(), loaded, &error, false), error);
    require(loaded.rig.controls.size() == 3U, "successful paired save did not advance both files");
}

void test_tabs_dirty_save_and_recent(const std::filesystem::path& root) {
    const auto arm = paths_for(root, "arm");
    const auto leg = paths_for(root, "leg");
    write_fixture(arm);
    write_fixture(leg);
    EditorControlRigPanel panel;
    std::string error;
    require(panel.open_asset(arm, false, &error), error);
    require(panel.open_asset(leg, false, &error), error);
    require(panel.tab_count() == 2U && panel.active_tab() == 1U, "asset opens did not create ordered tabs");
    require(panel.recent_documents().size() == 2U && panel.recent_documents().front() == leg.rig,
            "recent Control Rig ordering is incorrect");
    require(panel.open_asset(arm, false, &error), error);
    require(panel.tab_count() == 2U && panel.active_tab() == 0U,
            "opening an existing rig duplicated its tab instead of activating it");

    const auto control = panel.session().document().rig.controls.front().id;
    const auto before = panel.session().document().controlLayouts.at(control).position;
    require(panel.session().move_control(control, {before.x + 25.0F, before.y}), "tab mutation failed");
    require(panel.dirty(panel.active_tab()), "mutated rig tab did not become dirty");
    require(!panel.close_tab(panel.active_tab(), false, &error) && error.find("unsaved") != std::string::npos,
            "dirty tab closed without an explicit discard");
    require(panel.save_active(&error), error);
    require(!panel.dirty(panel.active_tab()), "saved rig tab remained dirty");
    require(panel.close_tab(0U, false, &error), error);
    require(panel.tab_count() == 1U, "clean tab did not close");
}

void test_autosave_recovery(const std::filesystem::path& root) {
    const auto paths = paths_for(root, "recovery");
    write_fixture(paths);
    EditorControlRigPanel editing;
    std::string error;
    require(editing.open_asset(paths, false, &error), error);
    ControlRigControl control;
    control.name = "Recovered Control";
    require(editing.session().add_control(control, {820.0F, 140.0F}, &error) != 0U, error);
    editing.update(31.0F);
    const auto autosaveRig = paths.rig.parent_path() / (paths.rig.filename().string() + ".autosave");
    const auto autosaveLayout = paths.layout.parent_path() / (paths.layout.filename().string() + ".autosave");
    require(std::filesystem::exists(autosaveRig) && std::filesystem::exists(autosaveLayout),
            "autosave did not publish a complete pair");
    std::error_code timeError;
    const auto savedTime = std::filesystem::last_write_time(paths.rig, timeError);
    std::filesystem::last_write_time(autosaveRig, savedTime + std::chrono::seconds(5), timeError);
    require(!timeError, "could not establish deterministic autosave ordering");

    EditorControlRigPanel recovered;
    require(recovered.open_asset(paths, true, &error), error);
    require(recovered.dirty(0U), "recovered autosave did not mark the tab dirty");
    require(recovered.session().document().rig.controls.size() == 3U,
            "newer autosave content was not recovered");
    require(recovered.tab_views().front().recovered, "recovery state is absent from the tab model");
    require(recovered.save_active(&error), error);
    require(!std::filesystem::exists(autosaveRig) && !std::filesystem::exists(autosaveLayout),
            "successful recovery save did not remove obsolete autosave files");
}

void test_native_asset_open_routing(const std::filesystem::path& root) {
#if defined(DVE_V185_NATIVE_INTEGRATION_TEST)
    const auto paths = paths_for(root / "assets" / "rigs", "native_arm");
    write_fixture(paths);
    NativeEditorController controller;
    controller.configure_ai_assistant(root);
    require(controller.dispatch_action("window.toggle_assets"), "assets panel did not open");
    const auto relative = std::filesystem::relative(paths.rig, root);
    const auto* record = controller.asset_database().find_path(relative);
    require(record, "Control Rig was not indexed as an asset");
    controller.asset_browser_state().selectedId = record->id;
    require(controller.dispatch_action("asset.open"), "asset action did not open the Control Rig");
    require(controller.control_rig_panel().open() && controller.control_rig_panel().tab_count() == 1U,
            "native asset routing did not activate the rig workspace");
#else
    (void)root;
#endif
}

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "dve_control_rig_asset_workflow_v185";
    try {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        test_paired_transaction_and_rollback(root);
        test_tabs_dirty_save_and_recent(root);
        test_autosave_recovery(root);
        test_native_asset_open_routing(root);
        std::filesystem::remove_all(root);
        std::cout << "dve_v185_control_rig_asset_workflow_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::filesystem::remove_all(root);
        std::cerr << "dve_v185_control_rig_asset_workflow_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
