#include "dve/editor_runtime_settings.hpp"
#include "dve/editor_native_renderer.hpp"
#include "dve/editor_platform_bridge.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace dve;
using namespace dve::editor;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void set(EditorSettingsRegistry& settings, std::string_view id, SettingValue value) {
    std::string error;
    if (!settings.set(SettingScope::Session, id, std::move(value), &error)) throw std::runtime_error(error);
}
EditorDocument fixture() {
    EditorDocument document("Settings fixture");
    EditorObject floor(1, "Floor");
    floor.flags.anchored = true;
    floor.voxelSizeMeters = 1.0F;
    floor.transform.position = {0, -2, 0};
    floor.voxels->fill_brick({0,0,0}, kDefaultSurfaceMaterial);
    document.add_object(std::move(floor));
    EditorObject body(2, "Body");
    body.flags.anchored = false;
    body.voxelSizeMeters = 0.5F;
    body.transform.position = {1, 8, 3};
    body.voxels->set_voxel({0,0,0}, kDefaultSurfaceMaterial);
    document.add_object(std::move(body));
    document.mark_clean();
    return document;
}
struct Canvas final : IEditorCanvas {
    mutable std::vector<EditorColor> fills;
    mutable std::vector<std::string> texts;
    mutable std::size_t outlines{};
    void fill(UiRect, EditorColor color) const override { fills.push_back(color); }
    void outline(UiRect, EditorColor) const override { ++outlines; }
    void line(int,int,int,int,EditorColor,int) const override {}
    void text(int,int,std::string_view value,EditorColor) const override { texts.emplace_back(value); }
    int text_width(std::string_view value) const override { return static_cast<int>(value.size()) * 7; }
};

void layers_and_transactions() {
    auto registry = EditorSettingsRegistry::make_default();
    const auto initial = registry.revision();
    set(registry, "audio.master_gain_db", -12.0);
    const auto changed = registry.revision();
    require(changed > initial, "setting did not wake consumers");
    set(registry, "audio.master_gain_db", -12.0);
    require(registry.revision() == changed, "no-op setter woke consumers");
    const auto profile = registry.serialize_profile("Audio", SettingScope::Session);
    registry.clear_scope(SettingScope::Session);
    const auto cleared = registry.revision();
    std::string error;
    require(registry.parse_profile(SettingScope::Session, profile, &error), "profile did not load");
    require(registry.revision() > cleared, "profile load did not wake consumers");
    EditorSettingsPanelState panel;
    panel.open_for(SettingScope::Session, "Camera");
    require(panel.stage(registry, "camera.dead_zone", 0.8, &error), "valid scalar did not stage");
    const auto before = registry.serialize_scope(SettingScope::Session);
    require(!panel.apply(registry, &error), "invalid paired zones were applied");
    require(registry.serialize_scope(SettingScope::Session) == before, "failed batch changed saved settings");
    require(panel.stage(registry, "camera.soft_zone", 0.9, &error), "second zone did not stage");
    require(panel.apply(registry, &error), "valid paired batch failed");
    auto invalid = registry;
    set(invalid, "camera.dead_zone", 1.0);
    require(!registry.parse_scope(SettingScope::Session, invalid.serialize_scope(SettingScope::Session), &error),
            "file load accepted invalid zone pair");
    require(std::get<double>(registry.value("camera.dead_zone")) == 0.8, "failed load changed active layer");
    EditorSettingsPanelState backendPanel;
    backendPanel.open_for(SettingScope::Session, "Physics");
    require(backendPanel.cycle(registry, "physics.backend", 1, &error), "no compiled backend choice could be selected");
    const auto selected = std::get<std::string>(backendPanel.displayed_value(registry, "physics.backend"));
    require(registry.choice_availability("physics.backend", selected).available, "UI selected unavailable backend");
    set(registry, "physics.backend", std::string("reference"));
    require(!registry.availability("physics.worker_threads").available, "reference world advertised worker tuning");
    require(registry.availability("physics.gravity").available, "reference world hid usable gravity control");
}

void simulation_observables() {
    NativeEditorController controller{EditorWorkspace(fixture())};
    auto& registry = controller.workspace().settings();
    set(registry, "physics.backend", std::string("reference"));
    set(registry, "physics.fixed_timestep", 0.01);
    set(registry, "physics.max_substeps", std::int64_t{2});
    set(registry, "physics.gravity", 0.0);
    require(controller.dispatch_action("physics.simulate"), "simulation did not start");
    controller.update(0.2F);
    require(controller.play_session().telemetry().fixedTickCount == 2, "catch-up limit was ignored");
    require(controller.play_session().telemetry().droppedSeconds > 0.1, "excess simulation time was not bounded");
    require(std::fabs(controller.workspace().document().find_object(2)->transform.position.y - 8.0F) < 1e-5F,
            "zero gravity did not preserve body height");
    camera::CameraCollisionHit hit;
    require(controller.play_session().camera_collision_world()->sweep_sphere({1,10,1},{1,-3,1},0.2F,hit),
            "camera collision adapter did not see scene geometry");
    set(registry, "physics.gravity", -20.0);
    controller.update(0.02F);
    require(controller.workspace().document().find_object(2)->transform.position.y < 7.999F,
            "live gravity edit did not affect the running world");
    require(controller.dispatch_action("physics.stop"), "simulation did not stop");
    require(controller.play_session().camera_collision_world() == nullptr, "stopped collision adapter survived world");
    set(registry, "physics.gravity", -20.0);
    require(controller.dispatch_action("physics.simulate"), "second simulation did not start");
    controller.update(0.02F);
    require(controller.workspace().document().find_object(2)->transform.position.y < 7.999F,
            "configured gravity did not move the body");
    require(controller.dispatch_action("physics.stop"), "second simulation did not stop");
    require(std::fabs(controller.workspace().document().find_object(2)->transform.position.y - 8.0F) < 1e-5F,
            "stop did not restore authored position");
}

void camera_observables() {
    {
        NativeEditorController authored;
        const auto rigId = authored.create_camera_rig_from_view("Authored damping");
        authored.camera_director().find_rig(rigId)->framing.positionDampingSeconds = 0.7F;
        auto& settings = authored.workspace().settings();
        set(settings, "audio.music_gain_db", -3.0);
        authored.update(0.01F);
        require(authored.camera_director().find_rig(rigId)->framing.positionDampingSeconds == 0.7F,
                "unrelated setting change overwrote authored camera framing");
        auto* authoredRig = authored.camera_director().find_rig(rigId);
        authoredRig->framing.softZoneFraction = 0.3F;
        const float previousDeadZone = authoredRig->framing.deadZoneFraction;
        set(settings, "camera.dead_zone", 0.4);
        authored.update(0.01F);
        require(authoredRig->framing.validate(), "global override published an invalid per-camera zone pair");
        require(authoredRig->framing.deadZoneFraction == previousDeadZone,
                "conflicting global zone override replaced the previous working camera");
        require(settings.clear(SettingScope::Session, "camera.dead_zone"), "conflicting override did not clear");
        set(settings, "camera.position_damping", 0.2);
        authored.update(0.01F);
        require(authored.camera_director().find_rig(rigId)->framing.positionDampingSeconds == 0.2F,
                "explicit camera override did not take effect");
        require(settings.clear(SettingScope::Session, "camera.position_damping"), "override did not clear");
        authored.update(0.01F);
        require(authored.camera_director().find_rig(rigId)->framing.positionDampingSeconds == 0.7F,
                "clearing camera override did not restore authored framing");
    }
    NativeEditorController controller{EditorWorkspace(fixture())};
    auto& registry = controller.workspace().settings();
    set(registry, "camera.position_damping", 0.0);
    set(registry, "camera.aim_damping", 0.0);
    set(registry, "camera.dead_zone", 0.0);
    set(registry, "camera.soft_zone", 0.0);
    set(registry, "camera.look_ahead", 0.5);
    set(registry, "camera.lock_viewport_to_selected", true);
    const auto id = controller.create_camera_rig_from_view("Follow");
    auto* rig = controller.camera_director().find_rig(id);
    rig->mode = camera::CameraRigMode::Follow;
    rig->followTarget = 2;
    controller.update(0.1F);
    require(std::fabs(controller.camera().target.x - 1.0F) < 1e-4F, "first target observation invented velocity");
    controller.workspace().document().find_object(2)->transform.position.x = 2.0F;
    controller.update(0.1F);
    require(controller.camera().target.x > 6.0F, "look-ahead did not use target velocity");
    require(controller.camera().target.x == controller.camera_director().current_pose().target.x,
            "editor discarded computed camera pose");
    set(registry, "camera.look_ahead", 0.0);
    controller.update(0.1F);
    require(std::fabs(controller.camera().target.x - 2.0F) < 1e-4F, "zero look-ahead did not follow current target");
    set(registry, "camera.shake_scale", 2.0);
    set(registry, "accessibility.camera_shake", 0.25);
    set(registry, "accessibility.reduced_motion", true);
    controller.update(0.1F);
    require(controller.camera_director().shake_scale() == 0.5F, "shake scales were not combined once");
    require(controller.camera_director().reduced_motion(), "reduced motion did not reach camera runtime");
}

audio::AudioSourceHandle constant_sample(audio::AudioMixer& mixer) {
    audio::ResidentSampleDesc sample;
    sample.sampleRate = mixer.sample_rate();
    sample.channels = 2;
    sample.samples.resize(8192);
    for (std::size_t i = 0; i < sample.samples.size(); i += 2) sample.samples[i] = 0.2F;
    const auto sampleId = mixer.register_resident_sample(std::move(sample));
    audio::PlaySampleDesc play;
    play.sample = sampleId;
    play.spatialized = false;
    play.loop = true;
    return mixer.play_sample(play);
}
void audio_observables() {
    NativeEditorController controller;
    auto& mixer = controller.audio_mixer();
    auto effects = mixer.bus_parameters(audio::AudioBusId::Effects);
    effects.reverbSend = 0;
    require(mixer.set_bus_parameters(audio::AudioBusId::Effects, effects), "bus setup failed");
    require(static_cast<bool>(constant_sample(mixer)), "sample did not start");
    std::vector<float> samples(4096);
    mixer.render(samples);
    const float baseline = samples[samples.size() - 2];
    require(baseline > 0.1F && samples.back() == 0.0F, "stereo fixture did not render");
    set(controller.workspace().settings(), "audio.effects_gain_db", -20.0);
    controller.update(0.01F);
    require(mixer.bus_parameters(audio::AudioBusId::Effects).gain == effects.gain,
            "user volume changed authored bus gain");
    mixer.render(samples);
    require(samples[samples.size() - 2] < baseline * 0.12F && samples[samples.size() - 2] > baseline * 0.09F,
            "bus dB setting did not change actual waveform");
    set(controller.workspace().settings(), "accessibility.mono_audio", true);
    controller.update(0.01F);
    mixer.render(samples);
    require(samples[samples.size() - 2] == samples.back() && samples[samples.size() - 2] > 0, "mono setting did not downmix both channels");
    const float full = samples[samples.size() - 2];
    set(controller.workspace().settings(), "accessibility.dynamic_range", std::string("night"));
    controller.update(0.01F);
    mixer.render(samples);
    require(samples[samples.size() - 2] < full, "night processing did not reduce output peaks");

    // A volume transition must follow sample time, regardless of callback block size.
    audio::AudioMixer whole, split;
    for (auto* engine : {&whole, &split}) {
        auto bus = engine->bus_parameters(audio::AudioBusId::Effects);
        bus.reverbSend = 0.0F;
        require(engine->set_bus_parameters(audio::AudioBusId::Effects, bus), "ramp bus setup failed");
        require(static_cast<bool>(constant_sample(*engine)), "ramp source did not start");
        engine->render(samples);
        auto gains = engine->user_bus_gains();
        gains[audio::audio_bus_index(audio::AudioBusId::Effects)] = 0.1F;
        require(engine->set_user_bus_gains(gains), "ramp command did not post");
    }
    std::vector<float> wholeRamp(2048), splitRamp(2048);
    whole.render(wholeRamp);
    std::size_t offset = 0;
    for (const auto frames : {17U, 203U, 311U, 493U}) {
        split.render(std::span<float>(splitRamp).subspan(offset * 2U, frames * 2U));
        offset += frames;
    }
    for (std::size_t i = 0; i < wholeRamp.size(); ++i)
        require(std::fabs(wholeRamp[i] - splitRamp[i]) < 1e-6F, "gain ramp depends on callback block size");
    require(wholeRamp.front() > baseline * 0.99F, "volume preference jumped at the block boundary");

    audio::AudioMixer queued;
    audio::AudioBusParameters parameters;
    parameters.gain = 0.5F;
    bool fullQueue = false;
    for (int i = 0; i < 8192; ++i) if (!queued.set_bus_parameters(audio::AudioBusId::Music, parameters)) {
        fullQueue = true; break;
    }
    require(fullQueue, "command queue did not remain bounded");
    parameters.gain = 0.1F;
    require(!queued.set_bus_parameters(audio::AudioBusId::Music, parameters), "full queue accepted a command");
    require(queued.bus_parameters(audio::AudioBusId::Music).gain == 0.5F, "failed command falsely changed bus state");
    NativeEditorController pending;
    auto& pendingMixer = pending.audio_mixer();
    parameters.gain = 0.5F;
    for (int i = 0; i < 8192; ++i)
        if (!pendingMixer.set_bus_parameters(audio::AudioBusId::Music, parameters)) break;
    set(pending.workspace().settings(), "audio.master_gain_db", -6.0);
    pending.update(0.01F);
    require(pending.audio_settings_pending(), "rejected audio preferences were falsely reported active");
    require(pendingMixer.user_bus_gains()[0] == 1.0F, "failed gain-group command changed control state");
    pendingMixer.render(samples);
    pending.update(0.01F);
    require(!pending.audio_settings_pending(), "pending audio preferences did not retry after queue drain");
    require(pendingMixer.user_bus_gains()[0] < 0.51F, "retry did not publish latest requested gain");
}

void ui_and_input_observables() {
    NativeEditorController controller;
    auto& registry = controller.workspace().settings();
    set(registry, "editor.theme", std::string("light"));
    set(registry, "accessibility.focus_indicators", false);
    controller.update(0.01F);
    Canvas light;
    render_native_editor(light, controller, 1280, 800);
    require((light.fills.front() & 255U) > 200, "light theme did not alter drawn pixels");
    set(registry, "accessibility.focus_indicators", true);
    controller.update(0.01F);
    Canvas focused;
    render_native_editor(focused, controller, 1280, 800);
    require(focused.outlines == light.outlines + 1, "focus indicator did not draw its outline");
    set(registry, "editor.theme", std::string("system"));
    controller.set_system_theme_light(false);
    Canvas dark;
    render_native_editor(dark, controller, 1280, 800);
    require((dark.fills.front() & 255U) < 50, "system dark theme was ignored");
    controller.set_system_theme_light(true);
    Canvas systemLight;
    render_native_editor(systemLight, controller, 1280, 800);
    require(systemLight.fills.front() == light.fills.front(), "system theme change did not take effect");

    set(registry, "input.ui_repeat_delay", 0.2);
    set(registry, "input.ui_repeat_rate", 0.1);
    controller.open_settings(SettingScope::User, "Camera");
    EditorPlatformBridge bridge(controller);
    platform::PlatformEvent down;
    down.type = platform::EventType::KeyDown;
    down.key = "Down";
    bridge.handle_event(down);
    const auto first = controller.settings_panel().selectedRow;
    down.repeat = true;
    bridge.handle_event(down);
    require(controller.settings_panel().selectedRow == first, "OS repeat bypassed configured delay");
    bridge.update(0.1F);
    require(controller.settings_panel().selectedRow == first, "navigation repeated too soon");
    bridge.update(0.11F);
    require(controller.settings_panel().selectedRow == first + 1, "navigation did not repeat at configured delay");
    down.type = platform::EventType::WindowFocusLost;
    bridge.handle_event(down);
    bridge.update(1.0F);
    require(controller.settings_panel().selectedRow == first + 1, "focus loss left a repeating key held");
    controller.close_settings(false);
    require(controller.dispatch_action("physics.simulate"), "input simulation did not start");
    set(registry, "input.controller_dead_zone", 0.5);
    platform::PlatformEvent axis;
    axis.type = platform::EventType::GamepadAxisMotion;
    axis.gamepadAxis = platform::GamepadAxis::LeftX;
    axis.gamepadValue = 0.25F;
    bridge.handle_event(axis);
    require(controller.play_session().input().axes.at("move_x") == 0, "stick inside dead zone moved player");
    axis.gamepadValue = 0.75F;
    bridge.handle_event(axis);
    require(controller.play_session().input().axes.at("move_x") == 0.5F, "stick range was not rescaled");
    axis.gamepadValue = 0.4F;
    bridge.handle_event(axis);
    axis.gamepadAxis = platform::GamepadAxis::LeftY;
    bridge.handle_event(axis);
    require(controller.play_session().input().axes.at("move_x") > 0.0F,
            "diagonal stick outside radial dead zone was discarded");
    require(controller.play_session().input().axes.at("move_y") < 0.0F,
            "stick direction did not match keyboard forward convention");
    set(registry, "input.controller_dead_zone", 0.0);
    bridge.update(0.01F);
    require(std::fabs(controller.play_session().input().axes.at("move_x") - 0.4F) < 1e-6F,
            "dead-zone edit did not reprocess an unmoving held stick");
    controller.key_down("D", false, false, false);
    require(controller.play_session().input().axes.at("move_x") == 1.0F,
            "keyboard movement did not combine with gamepad input");
    controller.key_up("D", false, false, false);
    require(std::fabs(controller.play_session().input().axes.at("move_x") - 0.4F) < 1e-6F,
            "keyboard release discarded held analog input");
    axis.type = platform::EventType::GamepadRemoved;
    bridge.handle_event(axis);
    require(controller.play_session().input().axes.at("move_x") == 0.0F,
            "unplugged gamepad left movement active");
    require(controller_axis_with_dead_zone(-1, 0.95F) == -1, "maximum dead zone lost full negative input");
    require(editor_frame_floor(60).count() == 17 && editor_frame_floor(1).count() == 1000,
            "frame cap was rounded toward an early present");
    require(editor_frame_floor(0).count() == 4, "unlimited user cap removed internal pacing floor");
}
} // namespace

int main() {
    try {
        layers_and_transactions(); simulation_observables(); camera_observables();
        audio_observables(); ui_and_input_observables();
        std::cout << "dve_settings_runtime_tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "dve_settings_runtime_tests: FAIL: " << error.what() << '\n';
        return 1;
    }
}
