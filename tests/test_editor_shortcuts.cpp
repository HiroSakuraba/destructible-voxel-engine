#include "dve/editor_workspace.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main() {
    using namespace dve::editor;

    EditorWorkspace workspace;
    EditorShortcutRegistry& shortcuts = workspace.shortcuts();
    require(shortcuts.has_profile("DVE Default"), "DVE default profile missing");
    require(shortcuts.has_profile("Unity Familiar"), "Unity profile missing");
    require(shortcuts.has_profile("Unreal Familiar"), "Unreal profile missing");
    require(shortcuts.has_profile("Accessibility One-Handed"), "accessibility profile missing");
    require(shortcuts.has_profile("Blank Custom"), "blank profile missing");
    require(shortcuts.commands().size() > 100U, "shortcut command inventory is unexpectedly small");

    const std::vector<ShortcutContext> viewport{ShortcutContext::Viewport};
    auto resolved = shortcuts.resolve(keyboard_shortcut("w"), viewport);
    require(resolved && resolved->actionId == "transform.translate", "W did not resolve to Move in viewport");

    const std::vector<ShortcutContext> fly{ShortcutContext::FlyNavigation, ShortcutContext::Viewport};
    resolved = shortcuts.resolve(keyboard_shortcut("w", false, false, false, ShortcutActivation::Hold), fly);
    require(resolved && resolved->actionId == "camera.fly_forward", "held W did not resolve to fly forward");

    const std::vector<ShortcutContext> sequencer{ShortcutContext::Sequencer};
    resolved = shortcuts.resolve(keyboard_shortcut("space"), sequencer);
    require(resolved && resolved->actionId == "sequencer.play_pause", "Space did not resolve in sequencer context");
    resolved = shortcuts.resolve(keyboard_shortcut("space"), viewport);
    require(resolved && resolved->actionId == "transform.space", "Space did not resolve to transform-space toggle");

    const std::vector<ShortcutContext> voxel{ShortcutContext::VoxelEditor, ShortcutContext::Viewport};
    resolved = shortcuts.resolve(mouse_shortcut("mouse4"), voxel);
    require(resolved && resolved->actionId == "voxel.sample_material", "Mouse4 did not prefer voxel sampling");
    resolved = shortcuts.resolve(mouse_shortcut("mouse4"), viewport);
    require(resolved && resolved->actionId == "view.previous_camera", "Mouse4 did not navigate camera history");
    resolved = shortcuts.resolve(mouse_shortcut("mouse1", false, false, false,
                                                   ShortcutActivation::DoubleClick), viewport);
    require(resolved && resolved->actionId == "view.frame", "double LMB did not resolve to Frame Selection");

    std::string error;
    require(shortcuts.duplicate_profile("DVE Default", "Test Custom", &error), "could not duplicate profile");
    require(shortcuts.set_binding("Test Custom", "transform.translate", ShortcutContext::Viewport,
                                  ShortcutSlot::Primary, keyboard_shortcut("u"), false, &error),
            "could not set custom binding");
    require(!shortcuts.set_binding("Test Custom", "transform.rotate", ShortcutContext::Viewport,
                                   ShortcutSlot::Primary, keyboard_shortcut("u"), false, &error),
            "conflicting shortcut was accepted without override");
    require(shortcuts.set_binding("Test Custom", "transform.rotate", ShortcutContext::Viewport,
                                  ShortcutSlot::Primary, keyboard_shortcut("u"), true, &error),
            "override did not replace conflicting shortcut");
    require(!shortcuts.bindings("Test Custom", "transform.translate", ShortcutContext::Viewport).primary,
            "override did not unbind the old action");

    const std::string serialized = shortcuts.serialize_profile("Test Custom");
    require(serialized.starts_with("DVE_SHORTCUT_PROFILE=1"), "profile serialization header missing");
    EditorWorkspace importedWorkspace;
    require(importedWorkspace.shortcuts().parse_profile(serialized, &error), "profile import failed");
    require(importedWorkspace.shortcuts().set_active_profile("Test Custom", &error), "imported profile unavailable");
    resolved = importedWorkspace.shortcuts().resolve(keyboard_shortcut("u"), viewport);
    require(resolved && resolved->actionId == "transform.rotate", "imported custom binding did not resolve");

    const std::string before = importedWorkspace.shortcuts().serialize_profile("Test Custom");
    std::string corrupt = serialized;
    corrupt += "bind \"missing.command\" \"Global\" \"F12\" \"\"\n";
    require(!importedWorkspace.shortcuts().parse_profile(corrupt, &error), "corrupt profile was accepted");
    require(importedWorkspace.shortcuts().serialize_profile("Test Custom") == before,
            "failed profile import mutated the previous profile");

    const auto matches = shortcuts.search("mouse4");
    require(!matches.empty(), "shortcut search by binding returned nothing");
    const auto keyboard = shortcuts.keyboard_map(ShortcutContext::Viewport);
    require(!keyboard.empty(), "visual keyboard map is empty");
    require(shortcuts.conflicts("DVE Default").empty(), "default shortcut profile contains conflicts");

    const MenuAction* move = workspace.menus().find("transform.translate");
    require(move && move->shortcut == "W", "menu shortcut label did not synchronize with active profile");

    std::cout << "editor shortcut tests passed\n";
    return 0;
}
