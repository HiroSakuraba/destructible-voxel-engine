// MidiInputSession: port preference (skip Midi Through / virtual ports), matching a saved port by
// name across ALSA renumbering, hotplug reconnect, all-notes-off on disconnect (no stuck notes,
// also with the sustain pedal down), the channel filter (Omni / 1-16 and channel 10 pads), and a
// threaded run (injector thread = RtMidi callback thread, worker polling, UI thread changing the
// port and filter, render thread) that TSan checks.
#include "dve/audio/midi_input_session.hpp"
#include "dve/audio/synthesizer.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace dve::audio;

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::printf("FAIL: %s\n", message.c_str()); ++g_failures; }
}

const std::string kThrough = "Midi Through:Midi Through Port-0 14:0";
const std::string kOwnOutput = "RtMidi Output Client:DVE Synth Output 130:0";
const std::string kMpk = "MPK mini 3:MPK mini 3 MIDI 1 24:0";
const std::string kMpkReplugged = "MPK mini 3:MPK mini 3 MIDI 1 28:0";
const std::string kKeystep = "Arturia KeyStep 37:Arturia KeyStep 37 MIDI 1 32:0";

std::vector<MidiPortDescriptor> ports(std::vector<std::string> names) {
    std::vector<MidiPortDescriptor> result;
    for (std::size_t i = 0; i < names.size(); ++i) result.push_back({i, names[i], true, false, false});
    return result;
}

void test_names_and_preference() {
    check(normalize_midi_port_name(kMpk) == "MPK mini 3:MPK mini 3 MIDI 1", "ALSA client:port suffix not stripped");
    check(normalize_midi_port_name("MPK mini 3") == "MPK mini 3", "plain name changed by normalization");
    check(normalize_midi_port_name("Synth:Port1:2") == "Synth:Port1:2", "only a space-separated ' <digits>:<digits>' suffix is stripped");
    check(midi_port_display_name(kMpk) == "MPK mini 3", "display name is not the ALSA client name");
    check(midi_port_display_name("MPK mini 3 0") == "MPK mini 3 0", "display name for a WinMM-style port");

    check(is_midi_through_or_virtual_port({0, kThrough, true, false, false}), "Midi Through not detected");
    check(is_midi_through_or_virtual_port({0, kOwnOutput, true, false, false}), "own RtMidi output not detected");
    check(is_midi_through_or_virtual_port({0, "IAC Driver Bus 1", true, false, false}), "macOS IAC bus not detected");
    check(is_midi_through_or_virtual_port({0, "loopMIDI Port 1", true, false, false}), "loopMIDI not detected");
    check(is_midi_through_or_virtual_port({0, "Anything", true, false, true}), "virtualPort flag ignored");
    check(!is_midi_through_or_virtual_port({0, kMpk, true, false, false}), "MPK mini treated as virtual");

    const auto list = ports({kThrough, kOwnOutput, kMpk, kKeystep});
    check(choose_midi_input_port(list, "") == std::optional<std::size_t>{2}, "Auto did not skip Through/virtual ports");
    check(!choose_midi_input_port(ports({kThrough, kOwnOutput}), "").has_value(), "Auto picked a Through port");
    check(!choose_midi_input_port(list, kMidiPortNone).has_value(), "None opened a port");
    check(choose_midi_input_port(list, kKeystep) == std::optional<std::size_t>{3}, "exact name not matched");
    check(choose_midi_input_port(list, kThrough) == std::optional<std::size_t>{0}, "explicit Through choice not honoured");
    check(choose_midi_input_port(ports({kThrough, kMpkReplugged}), "MPK mini 3:MPK mini 3 MIDI 1") ==
          std::optional<std::size_t>{1}, "saved (normalized) name not found after renumbering");
    check(choose_midi_input_port(ports({kThrough, kMpkReplugged}), kMpk) == std::optional<std::size_t>{1},
          "saved full name not found after renumbering");
    check(choose_midi_input_port(ports({"Microsoft GS Wavetable Synth 0", "MPK mini 3 1"}), "MPK mini 3 0") ==
          std::optional<std::size_t>{1}, "WinMM index suffix not tolerated");
    check(!choose_midi_input_port(list, "Launchkey Mini").has_value(), "missing port matched something");
}

void test_channel_filter() {
    const auto on = [](std::uint8_t channel) { return MidiMessage::note_on(channel, 60, 100); };
    MidiChannelFilter omni{0, MidiDrumChannelMode::Play};
    for (std::uint8_t ch = 0; ch < 16U; ++ch) check(omni.accepts(on(ch)), "Omni dropped a channel");
    MidiChannelFilter omniIgnore{0, MidiDrumChannelMode::Ignore};
    check(!omniIgnore.accepts(on(9)), "Omni + ignore pads let channel 10 through");
    check(omniIgnore.accepts(on(0)) && omniIgnore.accepts(on(10)), "Omni + ignore pads dropped another channel");
    MidiChannelFilter one{1, MidiDrumChannelMode::Play};
    check(one.accepts(on(0)), "channel 1 filter dropped channel 1");
    check(!one.accepts(on(1)) && !one.accepts(on(15)), "channel 1 filter let another channel through");
    check(one.accepts(on(9)), "channel 1 + play pads dropped channel 10");
    MidiChannelFilter oneFollow{1, MidiDrumChannelMode::Follow};
    check(!oneFollow.accepts(on(9)), "channel 1 + follow let channel 10 through");
    MidiChannelFilter oneIgnore{1, MidiDrumChannelMode::Ignore};
    check(!oneIgnore.accepts(on(9)), "channel 1 + ignore let channel 10 through");
    MidiChannelFilter ten{10, MidiDrumChannelMode::Follow};
    check(ten.accepts(on(9)) && !ten.accepts(on(0)), "channel 10 filter wrong");
    MidiChannelFilter sixteen{16, MidiDrumChannelMode::Play};
    check(sixteen.accepts(on(15)) && !sixteen.accepts(on(14)), "channel 16 filter wrong");
    check(one.accepts(MidiMessage::control_change(0, 1, 64)) && !one.accepts(MidiMessage::control_change(3, 1, 64)),
          "filter does not apply to control changes");
    check(one.accepts(MidiMessage::pitch_bend(0, 100)) && !one.accepts(MidiMessage::pitch_bend(4, 100)),
          "filter does not apply to pitch bend");
    check(oneIgnore.accepts(MidiMessage::realtime(0xF8)), "clock was filtered");
}

struct Recorder {
    std::mutex mutex;
    std::vector<MidiMessage> messages;
    void operator()(const MidiMessage& m) { std::lock_guard lock(mutex); messages.push_back(m); }
    std::size_t count() { std::lock_guard lock(mutex); return messages.size(); }
    void clear() { std::lock_guard lock(mutex); messages.clear(); }
    bool released_all() {
        std::lock_guard lock(mutex);
        std::array<bool, 16> sustainOff{}, notesOff{};
        for (const MidiMessage& m : messages) {
            if (m.type != MidiMessageType::ControlChange) continue;
            if (m.data1 == 64U && m.data2 == 0U) sustainOff[m.channel] = true;
            if (m.data1 == 123U && sustainOff[m.channel]) notesOff[m.channel] = true;  // 123 after sustain off
        }
        for (bool ok : notesOff) if (!ok) return false;
        return true;
    }
};

void test_session_hotplug() {
    auto backendOwner = std::make_unique<FakeMidiPortBackend>(std::vector<std::string>{kThrough, kOwnOutput, kMpk});
    FakeMidiPortBackend* backend = backendOwner.get();
    auto recorder = std::make_shared<Recorder>();
    MidiInputSession session(std::move(backendOwner), [recorder](const MidiMessage& m) { (*recorder)(m); },
                             {std::chrono::milliseconds(10), false});
    session.poll_now();
    MidiInputStatus status = session.status();
    check(status.state == MidiConnectionState::Connected, "Auto did not connect");
    check(status.connectedPort == kMpk, "Auto connected to " + status.connectedPort + " instead of the MPK mini");
    check(status.summary() == "MPK mini 3: connected", "summary: " + status.summary());
    check(status.ports.size() == 3U, "status does not list the ports");
    const std::uint64_t generation = session.status_generation();
    session.poll_now();
    check(session.status_generation() == generation, "an unchanged poll bumped the status generation");

    check(backend->inject(MidiMessage::note_on(0, 60, 100)), "inject failed while connected");
    check(recorder->count() == 1U, "note did not reach the sink");

    // Unplug: the session closes the port and releases everything on all 16 channels.
    recorder->clear();
    backend->unplug(kMpk);
    session.poll_now();
    status = session.status();
    check(status.state == MidiConnectionState::NoDevice, "unplug did not disconnect");
    check(status.summary() == "MPK mini 3: disconnected", "summary after unplug: " + status.summary());
    check(status.disconnects == 1U, "disconnect not counted");
    check(!backend->input_open(), "port left open after unplug");
    check(recorder->released_all(), "no sustain-off + all-notes-off on every channel after unplug");
    check(recorder->count() == 48U, "expected 16 x (sustain off, all notes off, pitch-bend center)");

    // Replug (ALSA gives it a new client:port id): Auto reconnects.
    backend->plug(kMpkReplugged);
    session.poll_now();
    status = session.status();
    check(status.state == MidiConnectionState::Connected && status.connectedPort == kMpkReplugged,
          "Auto did not reconnect after replug");
    check(status.connects == 2U, "reconnect not counted");

    // Saved port by name: stays disconnected while another device is present, reconnects later.
    session.set_requested_port("Arturia KeyStep 37:Arturia KeyStep 37 MIDI 1");
    session.poll_now();
    status = session.status();
    check(status.state == MidiConnectionState::Disconnected, "saved-but-absent port did not report disconnected");
    check(status.summary() == "Arturia KeyStep 37: disconnected", "summary for absent saved port: " + status.summary());
    check(!backend->input_open(), "switching to an absent saved port kept the old port open");
    backend->plug(kKeystep);
    session.poll_now();
    check(session.status().state == MidiConnectionState::Connected && backend->open_port_name() == kKeystep,
          "saved port did not reconnect when it appeared");

    // None disables input and releases notes.
    recorder->clear();
    session.set_requested_port(std::string(kMidiPortNone));
    session.poll_now();
    check(session.status().state == MidiConnectionState::Disabled, "None did not disable input");
    check(!backend->input_open(), "None kept the port open");
    check(recorder->released_all(), "None did not release notes");

    // Open failure is reported and retried.
    backend->fail_next_open("device busy");
    session.set_requested_port(kKeystep);
    session.poll_now();
    status = session.status();
    check(status.state == MidiConnectionState::Error && status.error == "device busy", "open error not reported");
    session.poll_now();
    check(session.status().state == MidiConnectionState::Connected, "open was not retried on the next poll");

    // Channel filter on the session (callback thread path).
    recorder->clear();
    session.set_channel_filter({2, MidiDrumChannelMode::Ignore});
    (void)backend->inject(MidiMessage::note_on(1, 60, 100));  // channel 2
    (void)backend->inject(MidiMessage::note_on(0, 60, 100));  // channel 1: filtered
    (void)backend->inject(MidiMessage::note_on(9, 36, 100));  // pads: ignored
    check(recorder->count() == 1U, "session channel filter wrong");
    check(session.messages_filtered() == 2U, "filtered messages not counted");
}

void test_no_backend() {
    MidiInputSession session(nullptr, [](const MidiMessage&) {});
    session.poll_now();
    check(session.status().state == MidiConnectionState::Unavailable, "no backend is not Unavailable");
    check(!session.has_backend(), "has_backend without a backend");
}

// Real synth: a held note and a sustained note are released when the device vanishes.
void test_synth_no_stuck_notes() {
    Synthesizer synth;
    auto backendOwner = std::make_unique<FakeMidiPortBackend>(std::vector<std::string>{kMpk});
    FakeMidiPortBackend* backend = backendOwner.get();
    MidiInputSession session(std::move(backendOwner), [&synth](const MidiMessage& m) { (void)synth.post_midi(m); },
                             {std::chrono::milliseconds(10), false});
    session.poll_now();
    std::vector<float> buffer(256U * 2U);
    (void)backend->inject(MidiMessage::control_change(0, 64, 127));  // sustain pedal down
    (void)backend->inject(MidiMessage::note_on(0, 60, 110));
    (void)backend->inject(MidiMessage::note_on(0, 64, 110));
    (void)backend->inject(MidiMessage::note_off(0, 64));  // sustained by the pedal
    (void)backend->inject(MidiMessage::note_on(9, 36, 110)); // pad
    for (int i = 0; i < 8; ++i) synth.render(buffer);
    auto held = [&] {
        int count = 0;
        for (const SynthVoiceInfo& voice : synth.voices())
            if (voice.active && voice.stage != VoiceStage::Release && voice.stage != VoiceStage::Idle) ++count;
        return count;
    };
    check(held() >= 3, "notes did not start (held voices: " + std::to_string(held()) + ")");
    backend->unplug(kMpk);  // device unplugged with keys and the pedal down
    session.poll_now();
    for (int i = 0; i < 4; ++i) synth.render(buffer);
    check(held() == 0, "voices still held after the device was unplugged: " + std::to_string(held()));
    // The pedal is up again: a fresh note released later must not ring on.
    backend->plug(kMpk);
    session.poll_now();
    (void)backend->inject(MidiMessage::note_on(0, 67, 100));
    (void)backend->inject(MidiMessage::note_off(0, 67));
    for (int i = 0; i < 4; ++i) synth.render(buffer);
    check(held() == 0, "sustain stayed on after the disconnect");
}

// Threaded: RtMidi-like callback thread, worker polling every 2 ms, UI thread changing port and
// filter, and an audio thread rendering. Run under TSan (out/build/tsan).
void test_threaded() {
    Synthesizer synth;
    auto backendOwner = std::make_unique<FakeMidiPortBackend>(std::vector<std::string>{kThrough, kMpk});
    FakeMidiPortBackend* backend = backendOwner.get();
    std::atomic<std::uint64_t> sunk{0};
    auto session = std::make_unique<MidiInputSession>(std::move(backendOwner),
        [&](const MidiMessage& m) { sunk.fetch_add(1U, std::memory_order_relaxed); (void)synth.post_midi(m); },
        MidiInputSession::Options{std::chrono::milliseconds(2), true});
    std::atomic<bool> stop{false};
    std::thread midiThread([&] {
        std::uint8_t note = 40;
        while (!stop.load(std::memory_order_relaxed)) {
            (void)backend->inject(MidiMessage::note_on(static_cast<std::uint8_t>(note % 16U), note, 100));
            (void)backend->inject(MidiMessage::note_off(static_cast<std::uint8_t>(note % 16U), note));
            note = static_cast<std::uint8_t>(40U + (note + 1U) % 40U);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });
    std::thread audioThread([&] {
        std::vector<float> buffer(128U * 2U);
        while (!stop.load(std::memory_order_relaxed)) synth.render(buffer);
    });
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
    int step = 0;
    while (std::chrono::steady_clock::now() < until) {
        switch (step++ % 6) {
            case 0: backend->unplug(kMpk); break;
            case 1: backend->plug(kMpk); break;
            case 2: session->set_requested_port(kMpk); break;
            case 3: session->set_channel_filter({static_cast<std::uint8_t>(step % 17), MidiDrumChannelMode::Ignore}); break;
            case 4: session->set_requested_port(""); break;
            case 5: (void)session->status(); session->request_rescan(); break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    session->set_channel_filter({0, MidiDrumChannelMode::Play});
    // The churn loop can stop right after an unplug or a request for no port; end in a known
    // state (plugged exactly once, requested) so the settle check does not depend on timing.
    backend->unplug(kMpk);
    backend->plug(kMpk);
    session->set_requested_port(kMpk);
    // Reaches a stable connected state once the port stays plugged.
    const auto settle = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (session->status().state != MidiConnectionState::Connected && std::chrono::steady_clock::now() < settle)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    check(session->status().state == MidiConnectionState::Connected, "threaded session did not settle connected");
    const std::uint64_t before = sunk.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    check(sunk.load() > before, "no messages delivered once connected");
    stop.store(true);
    midiThread.join();  // the session owns the backend the injector uses
    session.reset();    // joins the worker, closes the port, releases notes while audio still renders
    audioThread.join();
    check(session == nullptr, "session not destroyed");
    std::printf("threaded: %llu messages delivered\n", static_cast<unsigned long long>(sunk.load()));
}

} // namespace

int main() {
    test_names_and_preference();
    test_channel_filter();
    test_session_hotplug();
    test_no_backend();
    test_synth_no_stuck_notes();
    test_threaded();
    if (g_failures == 0) std::printf("dve_midi_input_session_tests: PASS\n");
    return g_failures == 0 ? 0 : 1;
}
