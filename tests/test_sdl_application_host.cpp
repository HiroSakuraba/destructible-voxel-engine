#include <SDL3/SDL.h>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

#include "dve/platform/sdl_application_host.hpp"

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
}

int main() {
    using namespace dve::platform;
    try {
        SDLTest_Reset();
        SDLTest_SetWindowMetrics(1280, 800, 2560, 1600, 2.0F);
        SdlApplicationHost host;
        std::string error;
        require(host.create_window({"SDL host test", 1280, 800, true, true, false}, &error), error.c_str());
        require(host.backend() == HostBackend::SDL3 && host.has_window(), "SDL host did not create a window");
        const WindowMetrics metrics = host.window_metrics();
        require(metrics.logicalWidth == 1280 && metrics.drawableWidth == 2560, "logical/pixel sizes were conflated");
        require(std::abs(metrics.contentScale - 2.0F) < 0.001F, "display scale was lost");
        require(host.native_window_handle().window != nullptr, "native SDL window was not transported");

        host.set_clipboard_text("hierarchy-copy");
        require(host.clipboard_text() == "hierarchy-copy", "clipboard round trip failed");

        // Editors set the title every frame; only a changed title reaches SDL.
        const int titleCallsBefore = SDLTest_SetWindowTitleCalls();
        host.set_window_title("SDL host test");  // the title the window was created with
        for (int frame = 0; frame < 10; ++frame) host.set_window_title("Scene - DVE Desktop Editor");
        require(SDLTest_SetWindowTitleCalls() - titleCallsBefore == 1, "an unchanged window title was re-sent");
        require(std::string(SDL_GetWindowTitle(static_cast<SDL_Window*>(host.native_window_handle().window))) ==
                    "Scene - DVE Desktop Editor", "window title was not applied");
        host.set_window_title("Scene * - DVE Desktop Editor");
        require(SDLTest_SetWindowTitleCalls() - titleCallsBefore == 2, "a changed window title was not sent");

        SDL_Event key{};
        key.type = SDL_EVENT_KEY_DOWN;
        key.key.type = SDL_EVENT_KEY_DOWN;
        key.key.timestamp = 44;
        key.key.key = 'S';
        key.key.mod = SDL_KMOD_CTRL | SDL_KMOD_SHIFT;
        SDLTest_PushEvent(&key);
        SDL_Event editing{};
        editing.type = SDL_EVENT_TEXT_EDITING;
        editing.edit.type = SDL_EVENT_TEXT_EDITING;
        editing.edit.timestamp = 45;
        editing.edit.text = "\xE3\x81\x8B";
        editing.edit.start = 1;
        editing.edit.length = 2;
        SDLTest_PushEvent(&editing);
        SDL_Event text{};
        text.type = SDL_EVENT_TEXT_INPUT;
        text.text.type = SDL_EVENT_TEXT_INPUT;
        text.text.timestamp = 46;
        text.text.text = "Voxel";
        SDLTest_PushEvent(&text);
        SDL_Event added{};
        added.type = SDL_EVENT_GAMEPAD_ADDED;
        added.gdevice.type = SDL_EVENT_GAMEPAD_ADDED;
        added.gdevice.which = 17;
        SDLTest_PushEvent(&added);
        SDL_Event button{};
        button.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        button.gbutton.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        button.gbutton.which = 17;
        button.gbutton.button = SDL_GAMEPAD_BUTTON_SOUTH;
        SDLTest_PushEvent(&button);
        SDL_Event axis{};
        axis.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
        axis.gaxis.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
        axis.gaxis.which = 17;
        axis.gaxis.axis = SDL_GAMEPAD_AXIS_LEFTX;
        axis.gaxis.value = -16384;
        SDLTest_PushEvent(&axis);
        SDL_Event drop{};
        drop.type = SDL_EVENT_DROP_FILE;
        drop.drop.type = SDL_EVENT_DROP_FILE;
        drop.drop.data = "/tmp/model.gltf";
        SDLTest_PushEvent(&drop);

        require(host.wait_for_events(std::chrono::milliseconds(1000)), "queued input did not wake the event wait");
        PlatformEvent converted;
        require(host.poll_event(converted) && converted.type == EventType::KeyDown && converted.key == "s", "key normalization failed");
        require(has_modifier(converted.modifiers, Modifier::Control) && has_modifier(converted.modifiers, Modifier::Shift), "key modifiers failed");
        require(host.poll_event(converted) && converted.type == EventType::TextEditing &&
                converted.text == "\xE3\x81\x8B" && converted.compositionStart == 1 &&
                converted.compositionLength == 2, "UTF-8 composition event failed");
        require(host.poll_event(converted) && converted.type == EventType::TextInput && converted.text == "Voxel", "UTF-8 text event failed");
        require(host.poll_event(converted) && converted.type == EventType::GamepadAdded &&
                converted.gamepadId == 17, "gamepad addition failed");
        require(host.poll_event(converted) && converted.type == EventType::GamepadButtonDown &&
                converted.gamepadButton == GamepadButton::South, "gamepad button normalization failed");
        require(host.poll_event(converted) && converted.type == EventType::GamepadAxisMotion &&
                converted.gamepadAxis == GamepadAxis::LeftX &&
                std::abs(converted.gamepadValue + 0.5F) < 0.001F, "gamepad axis normalization failed");
        require(host.poll_event(converted) && converted.type == EventType::FileDropped && converted.path == "/tmp/model.gltf", "file drop failed");
        require(!host.poll_event(converted), "SDL event queue did not drain");
        require(!host.wait_for_events(std::chrono::milliseconds(0)), "empty queue reported an event");
        require(!host.wait_for_events(std::chrono::milliseconds(-1)), "negative wait was not clamped to a poll");

        // Deterministic clock: rendering already uses 1 ms. Continuous queued
        // motion may wake the second half of the budget, never the 4 ms floor.
        SDLTest_SetTicksNS(0);
        const auto target = std::chrono::milliseconds(8), minimum = std::chrono::milliseconds(4);
        for (int frame = 0; frame < 250; ++frame) {
            const auto startTicks = SDL_GetTicksNS();
            const double start = host.monotonic_seconds();
            SDL_Delay(1);
            SDL_Event motion{}; motion.type = SDL_EVENT_MOUSE_MOTION;
            SDLTest_PushEvent(&motion);
            require(host.wait_for_frame(start, target, minimum), "motion did not wake paced wait");
            const auto elapsed = SDL_GetTicksNS() - startTicks;
            require(elapsed >= 4'000'000 && elapsed <= 5'000'000, "motion bypassed minimum frame interval");
            require(host.poll_event(converted) && converted.type == EventType::PointerMove,
                    "paced wait consumed queued motion");
            require(!host.poll_event(converted), "paced wait duplicated motion");
        }
        auto startTicks = SDL_GetTicksNS();
        double start = host.monotonic_seconds();
        SDL_Delay(1);
        require(!host.wait_for_frame(start, target, minimum), "idle paced wait reported input");
        require(SDL_GetTicksNS() - startTicks >= 8'000'000 && SDL_GetTicksNS() - startTicks <= 9'000'000,
                "idle budget excludes render work or waits twice");
        start = host.monotonic_seconds();
        SDL_Delay(12);
        startTicks = SDL_GetTicksNS();
        require(!host.wait_for_frame(start, target, minimum) && SDL_GetTicksNS() == startTicks,
                "over-budget frame incurred another delay");
        start = host.monotonic_seconds();
        SDLTest_PushEvent(&key);
        require(host.wait_for_frame(start, target, minimum), "key did not wake paced wait");
        require(host.poll_event(converted) && converted.type == EventType::KeyDown,
                "paced wait lost keyboard input");

        SDLTest_SetDialogResult("/tmp/scene.dvescene", nullptr);
        FileDialogRequest request;
        request.kind = FileDialogKind::OpenFile;
        request.filters.push_back({"DVE scene", {"dvescene"}});
        const FileDialogToken token = host.request_file_dialog(request);
        const auto result = host.take_file_dialog_result(token);
        require(result && result->accepted && result->paths.size() == 1U, "asynchronous dialog result failed");

        host.destroy_window();
        require(!host.has_window(), "destroy_window failed");
        require(!host.wait_for_events(std::chrono::milliseconds(1000)), "destroyed host attempted an event wait");
        std::cout << "SDL application-host contract passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "SDL application-host contract failed: " << exception.what() << '\n';
        return 1;
    }
}
