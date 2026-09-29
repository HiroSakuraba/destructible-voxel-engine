#include <SDL3/SDL_main.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "dve/audio/midi.hpp"
#include "dve/audio/sdl_synth_audio_device.hpp"
#include "dve/editor_accessibility.hpp"
#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"
#include "dve/editor_platform_bridge.hpp"
#include "dve/editor_sdl_canvas.hpp"
#include "dve/editor_ui_zoom.hpp"
#include "dve/platform/sdl_application_host.hpp"

int main(int argc, char** argv) {
    using namespace dve::editor;
    using namespace dve::platform;

    bool smoke = false;
    std::filesystem::path scenePath;
    std::filesystem::path accessibilityDump;
    std::filesystem::path projectRoot = std::filesystem::current_path();
    std::filesystem::path liveMcpRuntimeDirectory;
    bool liveMcp = false;
    bool liveMcpReadOnly = false;
    std::vector<std::string> dispatchActions;
    std::filesystem::path screenshot;
    std::optional<float> cliZoom;
    int initialWidth = 1280;
    int initialHeight = 800;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--smoke") smoke = true;
        else if (argument == "--scene" && index + 1 < argc) scenePath = argv[++index];
        else if (argument == "--project-root" && index + 1 < argc) projectRoot = argv[++index];
        else if (argument == "--live-mcp") liveMcp = true;
        else if (argument == "--live-mcp-read-only") { liveMcp = true; liveMcpReadOnly = true; }
        else if (argument == "--live-mcp-runtime-dir" && index + 1 < argc) liveMcpRuntimeDirectory = argv[++index];
        else if (argument == "--accessibility-dump" && index + 1 < argc) accessibilityDump = argv[++index];
        else if (argument == "--dispatch" && index + 1 < argc) dispatchActions.push_back(argv[++index]);
        // Headless verification helpers (same flags as the X11 host). --screenshot writes a BMP.
        else if (argument == "--screenshot" && index + 1 < argc) screenshot = argv[++index];
        else if (argument == "--ui-zoom" && index + 1 < argc) cliZoom = std::strtof(argv[++index], nullptr);
        else if (argument == "--window-size" && index + 1 < argc) {
            const std::string size = argv[++index];
            if (const auto x = size.find('x'); x != std::string::npos) {
                initialWidth = std::max(640, std::atoi(size.substr(0, x).c_str()));
                initialHeight = std::max(480, std::atoi(size.substr(x + 1).c_str()));
            }
        }
    }

    try {
        EditorDocument document = make_new_project_document();
        if (!scenePath.empty()) {
            std::string loadError;
            auto loaded = EditorDocument::load(scenePath, &loadError);
            if (!loaded) throw std::runtime_error(loadError);
            document = std::move(*loaded);
        }
        NativeEditorController controller{EditorWorkspace(std::move(document))};
        controller.configure_ai_assistant(projectRoot);
        // User-scope settings (UI zoom and everything else in Settings > User) persist here,
        // shared with the X11 host.
        controller.configure_user_settings(std::filesystem::current_path() / ".dve" / "user" / "editor_settings.txt");
        if (cliZoom) {
            // Session-scope override for this run; hotkeys / Settings > Apply persist to User.
            (void)controller.workspace().settings().set(SettingScope::Session, kUiZoomSettingId,
                                                        static_cast<double>(snap_ui_zoom(*cliZoom)));
            controller.workspace().synchronize_preferences_from_settings();
        }
        if (liveMcp) {
            dve::ai::LiveEditorMcpHostOptions liveOptions;
            liveOptions.projectRoot = projectRoot;
            liveOptions.runtimeDirectory = liveMcpRuntimeDirectory;
            liveOptions.approvalPolicy = liveMcpReadOnly
                ? dve::ai::AiApprovalPolicy::ReadOnlyOnly : dve::ai::AiApprovalPolicy::AskForChanges;
            std::string liveError;
            if (!controller.start_live_mcp_host(std::move(liveOptions), &liveError))
                throw std::runtime_error("could not start live-editor MCP host: " + liveError);
            std::cerr << "DVE live-editor MCP descriptor: "
                      << controller.live_mcp_status().descriptorPath << '\n';
        }
        for (const std::string& action : dispatchActions) (void)controller.dispatch_action(action);

        SdlApplicationHost host;
        std::string error;
        WindowDesc window;
        window.title = "DVE Desktop Editor v1.18";
        window.width = initialWidth;
        window.height = initialHeight;
        window.highDpi = true;
        window.hidden = smoke;
        if (!host.create_window(window, &error)) throw std::runtime_error(error);

        EditorPlatformBridge bridge(controller);
        SdlEditorCanvas canvas(host.native_window_handle(), &error);
        if (!canvas.valid()) throw std::runtime_error(error);
        // UI zoom: the controller runs at floor(window / zoom) logical pixels; the canvas maps
        // logical -> drawable pixels (zoom x HiDPI density) and the bridge maps pointers back.
        // Ctrl+= / Ctrl+- / Ctrl+0 are handled by the controller and update `editor.ui_scale`.
        float appliedZoom = 0.0F;
        float appliedDensity = 0.0F;
        int logicalWidth = 0;
        int logicalHeight = 0;
        const auto sync_zoom = [&](const WindowMetrics& metrics) {
            controller.set_ui_zoom_window_limit(max_ui_zoom_for_window(metrics.logicalWidth, metrics.logicalHeight));
            const float zoom = controller.effective_ui_zoom();
            const float density = metrics.logicalWidth > 0 && metrics.drawableWidth > 0
                ? static_cast<float>(metrics.drawableWidth) / static_cast<float>(metrics.logicalWidth) : 1.0F;
            if (zoom != appliedZoom || density != appliedDensity) {
                appliedZoom = zoom;
                appliedDensity = density;
                canvas.set_ui_zoom(zoom, density);
                bridge.set_ui_zoom(zoom);
            }
            const int nextWidth = ui_zoom_logical_extent(metrics.logicalWidth, zoom);
            const int nextHeight = ui_zoom_logical_extent(metrics.logicalHeight, zoom);
            if (nextWidth != logicalWidth || nextHeight != logicalHeight) {
                logicalWidth = nextWidth;
                logicalHeight = nextHeight;
                controller.resize(logicalWidth, logicalHeight);
            }
        };
        sync_zoom(host.window_metrics());

        dve::audio::SdlSynthAudioDevice audioDevice(controller.audio_mixer(), &error);
        if (!audioDevice.valid()) throw std::runtime_error("synth audio device: " + error);

        std::string midiStatus = "virtual-only";
        auto midiBackend = dve::audio::make_native_midi_backend(&error);
        if (midiBackend) {
            const auto inputs = midiBackend->input_ports();
            const auto outputs = midiBackend->output_ports();
            if (!inputs.empty()) {
                (void)midiBackend->open_input(inputs.front().index,
                    [&](const dve::audio::MidiMessage& message) { (void)controller.synthesizer().post_midi(message); }, &error);
            }
            if (!outputs.empty()) (void)midiBackend->open_output(outputs.front().index, &error);
            midiStatus = std::string(midiBackend->backend_name());
        }

        if (!accessibilityDump.empty()) {
            if (!save_accessibility_tree_json(accessibilityDump, build_editor_accessibility_tree(controller), &error))
                throw std::runtime_error(error);
        }

        int frames = 0;
        double previous = host.monotonic_seconds();
        while (!controller.quit_requested()) {
            PlatformEvent event;
            while (host.poll_event(event)) bridge.handle_event(event);
            if (midiBackend && midiBackend->output_open()) {
                dve::audio::MidiMessage message;
                while (controller.synthesizer().poll_midi_output(message)) (void)midiBackend->send(message);
            }

            const double now = host.monotonic_seconds();
            controller.update(static_cast<float>(now - previous));
            previous = now;
            sync_zoom(host.window_metrics());
            host.set_window_title(controller.workspace().document().name() +
                                  (controller.workspace().document().dirty() ? " *" : "") +
                                  " - DVE Desktop Editor");

            if (!canvas.begin_frame(0x07101DU, &error)) throw std::runtime_error(error);
            render_native_editor(canvas, controller, logicalWidth, logicalHeight);
            if (!screenshot.empty() && frames == 2) {
                if (!canvas.save_screenshot_bmp(screenshot, &error)) throw std::runtime_error("screenshot: " + error);
            }
            if (!canvas.end_frame(&error)) throw std::runtime_error(error);

            ++frames;
            if (smoke && frames == 2 && dispatchActions.empty()) {
                (void)controller.dispatch_action("physics.simulate");
                (void)controller.dispatch_action("physics.stop");
                (void)controller.dispatch_action("window.toggle_synth");
                (void)controller.dispatch_action("window.toggle_audio");
                (void)controller.synthesizer().note_on(60, 0.8F);
            }
            if (smoke && frames == 3) (void)controller.synthesizer().note_off(60);
            if (smoke && frames >= 4) break;
            host.sleep_for(std::chrono::milliseconds(8));
        }

        if (smoke) {
            std::cout << "dve_desktop_editor: PASS\n"
                      << "backend=" << host_backend_name(host.backend()) << '\n'
                      << "objects=" << controller.workspace().document().objects().size() << '\n'
                      << "draw_items=" << controller.draw_items().size() << '\n'
                      << "audio=" << audioDevice.backend_name() << '\n'
                      << "midi=" << midiStatus << '\n'
                      << "ui_zoom=" << format_ui_zoom_percent(controller.effective_ui_zoom()) << '\n'
                      << "text=" << canvas.text_backend() << '\n'
                      << "synth_frames=" << controller.synthesizer().meters().renderedFrames << '\n';
        }
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_desktop_editor: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
