// Background autosave (editor.autosave_minutes): recovery copies of the open scene that
// never touch the scene file, its path, revision or unsaved-changes flag.
#include "dve/editor_native.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using namespace dve::editor;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

// Drives update() until the in-flight autosave finishes (or fails the test).
void finish_autosave(NativeEditorController& controller, std::uint64_t completedBefore) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (controller.autosave_status().inFlight || controller.autosave_status().completed == completedBefore) {
        require(controller.autosave_status().failed == 0U, "autosave failed: " + controller.autosave_status().lastError);
        require(std::chrono::steady_clock::now() < deadline, "autosave did not finish");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        controller.update(0.0F);
    }
}

bool log_contains(const NativeEditorController& controller, std::string_view text) {
    for (const auto& entry : controller.workspace().log().entries())
        if (entry.text.find(text) != std::string::npos) return true;
    return false;
}

void test_autosave(const std::filesystem::path& root) {
    NativeEditorController controller{EditorWorkspace(make_native_editor_demo_document())};
    controller.configure_ai_assistant(root);
    require(controller.workspace().settings().set(SettingScope::Session, "editor.autosave_minutes", std::int64_t{1}),
            "could not set the autosave interval");

    // A clean document is never autosaved.
    controller.workspace().document().mark_clean();
    controller.update(0.0F);
    controller.update(120.0F);
    require(!controller.autosave_status().inFlight && controller.autosave_status().completed == 0U,
            "a clean document was autosaved");

    // Dirty: nothing before the interval, a background save after it.
    controller.workspace().document().mark_dirty();
    const auto revision = controller.workspace().document().revision();
    controller.update(30.0F);
    require(!controller.autosave_status().inFlight, "autosave started before the interval");
    controller.update(31.0F);
    require(controller.autosave_status().inFlight, "autosave did not start after the interval");
    finish_autosave(controller, 0U);

    const auto manifest = controller.autosave_manifest_path();
    require(controller.autosave_status().lastManifest == manifest, "autosave reported a different path");
    require(manifest.parent_path().parent_path() == root / ".dve" / "recovery", "recovery copy is not under .dve/recovery");
    require(std::filesystem::exists(manifest), "recovery manifest was not written");
    require(!std::filesystem::exists(manifest.parent_path().string() + ".writing"), "staging folder was left behind");
    std::string error;
    const auto recovered = EditorDocument::load(manifest, &error);
    require(recovered.has_value(), "recovery copy does not load: " + error);
    require(recovered->objects().size() == controller.workspace().document().objects().size(),
            "recovery copy lost objects");
    require(controller.workspace().document().dirty(), "autosave cleared the unsaved-changes flag");
    require(controller.workspace().document().path().empty(), "autosave changed the document path");
    require(controller.workspace().document().revision() == revision, "autosave changed the document revision");

    // The next autosave replaces the copy in place.
    controller.update(61.0F);
    finish_autosave(controller, 1U);
    std::size_t entries = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root / ".dve" / "recovery")) {
        (void)entry;
        ++entries;
    }
    require(entries == 1U, "autosaves accumulated instead of replacing the copy");

    // File > Save removes the now-stale copy.
    const auto scenePath = root / "scenes" / "autosave_probe.dvescene";
    std::filesystem::create_directories(scenePath.parent_path());
    require(controller.workspace().document().save_transactional(scenePath).success, "initial save failed");
    controller.workspace().document().mark_dirty();
    controller.update(61.0F);
    finish_autosave(controller, 2U);
    const auto sceneRecovery = controller.autosave_manifest_path();
    require(std::filesystem::exists(sceneRecovery), "autosave of a saved scene was not written");
    require(sceneRecovery.filename() == scenePath.filename(), "recovery copy does not mirror the scene file name");
    require(controller.dispatch_action("file.save"), "File > Save failed");
    require(!std::filesystem::exists(sceneRecovery.parent_path()), "File > Save left a stale recovery copy");

    // An autosave newer than the scene file is reported when the scene is reopened.
    controller.workspace().document().mark_dirty();
    controller.update(61.0F);
    finish_autosave(controller, 3U);
    std::filesystem::last_write_time(scenePath, std::filesystem::last_write_time(sceneRecovery) - std::chrono::minutes(1));
    auto reopened = EditorDocument::load(scenePath, &error);
    require(reopened.has_value(), "scene does not reload: " + error);
    NativeEditorController next{EditorWorkspace(std::move(*reopened))};
    next.configure_ai_assistant(root);
    next.update(0.0F);
    require(log_contains(next, "were autosaved to"), "a newer recovery copy was not reported on reopen");

    // ...but not when the scene file is newer than the copy.
    std::filesystem::last_write_time(scenePath, std::filesystem::last_write_time(sceneRecovery) + std::chrono::minutes(1));
    reopened = EditorDocument::load(scenePath, &error);
    NativeEditorController current{EditorWorkspace(std::move(*reopened))};
    current.configure_ai_assistant(root);
    current.update(0.0F);
    require(!log_contains(current, "were autosaved to"), "an older recovery copy was reported");
    std::cout << "autosave: OK\n";
}

} // namespace

int main() {
    try {
        const auto root = std::filesystem::temp_directory_path() / "dve_editor_autosave_tests";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        // Match NativeEditorController::configure_ai_assistant so recovery-path
        // comparisons stay valid on Windows, where temp_directory_path() may be
        // an 8.3 short path and weakly_canonical expands it to the long form.
        std::error_code ec;
        const auto canonicalRoot = std::filesystem::weakly_canonical(root, ec);
        const auto projectRoot = ec ? root.lexically_normal() : canonicalRoot;
        test_autosave(projectRoot);
        std::filesystem::remove_all(projectRoot);
        std::cout << "dve_editor_autosave_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_editor_autosave_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
