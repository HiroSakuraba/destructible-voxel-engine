#include <SDL3/SDL_main.h>

#include <filesystem>
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
        window.width = 1280;
        window.height = 800;
        window.highDpi = true;
        window.hidden = smoke;
        if (!host.create_window(window, &error)) throw std::runtime_error(error);

        const WindowMetrics initial = host.window_metrics();
        controller.resize(initial.logicalWidth, initial.logicalHeight);
        EditorPlatformBridge bridge(controller);
        SdlEditorCanvas canvas(host.native_window_handle(), &error);
        if (!canvas.valid()) throw std::runtime_error(error);

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
            const WindowMetrics metrics = host.window_metrics();
            controller.resize(metrics.logicalWidth, metrics.logicalHeight);
            host.set_window_title(controller.workspace().document().name() +
                                  (controller.workspace().document().dirty() ? " *" : "") +
                                  " - DVE Desktop Editor");

            if (!canvas.begin_frame(0x07101DU, &error)) throw std::runtime_error(error);
            render_native_editor(canvas, controller, metrics.logicalWidth, metrics.logicalHeight);
            if (!canvas.end_frame(&error)) throw std::runtime_error(error);

            ++frames;
            if (smoke && frames == 2) {
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
                      << "synth_frames=" << controller.synthesizer().meters().renderedFrames << '\n';
        }
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_desktop_editor: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
