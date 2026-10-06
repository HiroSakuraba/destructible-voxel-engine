// MidiOutputSession: Auto (first output port, the editor's previous behaviour), a port chosen by
// name across ALSA renumbering, None, hotplug reconnect, releasing held notes on the old port
// before switching, sends dropped while disconnected, a failed open, and a threaded run (worker
// polling, UI thread sending and changing the port) that TSan checks. Uses the fake backend's
// output ports, like the input session tests.
#include "dve/audio/midi_output_session.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
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
const std::string kSynth = "Roland JD-Xi:JD-Xi MIDI 1 24:0";
const std::string kSynthReplugged = "Roland JD-Xi:JD-Xi MIDI 1 28:0";
const std::string kSynthSaved = "Roland JD-Xi:JD-Xi MIDI 1";

std::vector<MidiPortDescriptor> ports(std::vector<std::string> names) {
    std::vector<MidiPortDescriptor> result;
    for (std::size_t i = 0; i < names.size(); ++i) result.push_back({i, names[i], false, true, false});
    return result;
}

struct Harness {
    FakeMidiPortBackend* backend{};
    std::unique_ptr<MidiOutputSession> session;
};

Harness make(std::vector<std::string> outputs, std::string requested = {}) {
    auto backend = std::make_unique<FakeMidiPortBackend>(std::vector<std::string>{}, std::move(outputs));
    Harness harness;
    harness.backend = backend.get();
    harness.session = std::make_unique<MidiOutputSession>(std::move(backend),
                                                          MidiOutputSession::Options{std::chrono::milliseconds(10), false});
    harness.session->set_requested_port(std::move(requested));
    return harness;
}

std::size_t count_all_notes_off(const std::vector<std::pair<std::string, MidiMessage>>& sent, const std::string& port) {
    std::size_t count = 0;
    for (const auto& [name, message] : sent)
        if (name == port && message.type == MidiMessageType::ControlChange && message.data1 == 123U) ++count;
    return count;
}

void test_choice() {
    check(choose_midi_output_port(ports({kThrough, kSynth}), "") == std::optional<std::size_t>(0U),
          "Auto must keep the previous behaviour (first output port)");
    check(!choose_midi_output_port(ports({}), "").has_value(), "Auto with no ports");
    check(!choose_midi_output_port(ports({kThrough, kSynth}), std::string(kMidiPortNone)).has_value(), "None opened a port");
    check(choose_midi_output_port(ports({kThrough, kSynthReplugged}), kSynthSaved) == std::optional<std::size_t>(1U),
          "saved name did not match the renumbered port");
    check(!choose_midi_output_port(ports({kThrough}), kSynthSaved).has_value(), "missing named port matched something");
}

void test_session_lifecycle() {
    Harness h = make({kThrough, kSynth});
    check(!h.session->send(MidiMessage::note_on(0, 60, 100)), "send before the first poll should be dropped");
    h.session->poll_now();
    MidiOutputStatus status = h.session->status();
    check(status.state == MidiConnectionState::Connected && status.connectedPort == kThrough, "Auto did not open the first port");
    check(status.summary() == "Midi Through: connected", "summary: " + status.summary());
    check(status.ports.size() == 2U, "port list not published");
    check(h.session->send(MidiMessage::note_on(0, 60, 100)), "send while connected failed");
    auto sent = h.backend->sent_messages();
    check(sent.size() == 1U && sent[0].first == kThrough && sent[0].second.is_note_on(), "note did not reach Midi Through");

    // Choose the synth by name: the held note is released on the old port first.
    h.backend->clear_sent();
    h.session->set_requested_port(kSynthSaved);
    h.session->poll_now();
    status = h.session->status();
    check(status.state == MidiConnectionState::Connected && h.backend->open_output_port_name() == kSynth,
          "named port not opened");
    sent = h.backend->sent_messages();
    check(count_all_notes_off(sent, kThrough) == 16U, "all-notes-off not sent on every channel of the old port");
    check(h.session->send(MidiMessage::note_on(1, 64, 90)), "send to the synth failed");
    sent = h.backend->sent_messages();
    check(!sent.empty() && sent.back().first == kSynth && sent.back().second.data1 == 64U, "note did not reach the synth");

    // Unplug: disconnected, sends dropped; replug with a new ALSA id: reconnects by name.
    h.backend->unplug_output(kSynth);
    h.session->poll_now();
    status = h.session->status();
    check(status.state == MidiConnectionState::Disconnected, "unplug not detected");
    check(status.summary() == "Roland JD-Xi: disconnected", "summary after unplug: " + status.summary());
    const std::uint64_t dropped = h.session->messages_dropped();
    check(!h.session->send(MidiMessage::note_on(0, 60, 100)) && h.session->messages_dropped() == dropped + 1U,
          "send while unplugged not dropped");
    h.backend->plug_output(kSynthReplugged);
    h.session->poll_now();
    check(h.session->status().state == MidiConnectionState::Connected && h.backend->open_output_port_name() == kSynthReplugged,
          "saved port did not reconnect after replug");
    check(h.session->status().disconnects == 1U && h.session->status().connects == 3U, "connect/disconnect counters");

    // None closes the output (releasing notes) and reports Disabled.
    h.backend->clear_sent();
    h.session->set_requested_port(std::string(kMidiPortNone));
    h.session->poll_now();
    check(h.session->status().state == MidiConnectionState::Disabled && !h.backend->output_open(), "None did not close the output");
    check(count_all_notes_off(h.backend->sent_messages(), kSynthReplugged) == 16U, "None did not release notes");
    check(h.session->status().summary() == "MIDI output off", "summary for None");

    // A failing open reports Error and is retried on the next poll.
    h.backend->fail_next_output_open("busy");
    h.session->set_requested_port({});
    h.session->poll_now();
    check(h.session->status().state == MidiConnectionState::Error && h.session->status().error == "busy", "open error not reported");
    h.session->poll_now();
    check(h.session->status().state == MidiConnectionState::Connected, "open not retried");

    // Auto with no ports at all.
    Harness empty = make({});
    empty.session->poll_now();
    check(empty.session->status().state == MidiConnectionState::NoDevice &&
          empty.session->status().summary() == "No MIDI output", "no-port Auto state");
    // Without a backend.
    MidiOutputSession none(nullptr, {std::chrono::milliseconds(10), false});
    none.poll_now();
    check(none.status().state == MidiConnectionState::Unavailable && !none.send(MidiMessage::note_on(0, 60, 1)), "null backend");
}

void test_close_releases_notes() {
    auto backend = std::make_unique<FakeMidiPortBackend>(std::vector<std::string>{}, std::vector<std::string>{kSynth});
    FakeMidiPortBackend* raw = backend.get();
    std::vector<std::pair<std::string, MidiMessage>> sent;
    {
        MidiOutputSession session(std::move(backend), {std::chrono::milliseconds(10), false});
        session.poll_now();
        (void)session.send(MidiMessage::note_on(0, 60, 100));
        sent = raw->sent_messages();
        check(sent.size() == 1U, "note not sent");
        raw->clear_sent();
        session.set_requested_port(std::string(kMidiPortNone));
        session.poll_now();
        sent = raw->sent_messages();
    }
    check(count_all_notes_off(sent, kSynth) == 16U, "closing did not release notes");
}

void test_threaded() {
    auto backend = std::make_unique<FakeMidiPortBackend>(std::vector<std::string>{}, std::vector<std::string>{kThrough, kSynth});
    FakeMidiPortBackend* raw = backend.get();
    MidiOutputSession session(std::move(backend), {std::chrono::milliseconds(1), true});
    std::atomic<bool> stop{false};
    std::thread plugger([&] {
        for (int i = 0; i < 200 && !stop.load(); ++i) {
            if (i % 2 == 0) raw->unplug_output(kSynth); else raw->plug_output(kSynth);
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
    });
    for (int i = 0; i < 2000; ++i) {
        (void)session.send(MidiMessage::note_on(0, static_cast<std::uint8_t>(i % 128), 100));
        if (i % 250 == 0) session.set_requested_port(i % 500 == 0 ? kSynthSaved : std::string());
        if (i % 50 == 0) (void)session.status();
    }
    stop.store(true);
    plugger.join();
    check(session.messages_sent() + session.messages_dropped() >= 1U, "threaded run sent nothing");
}

} // namespace

int main() {
    test_choice();
    test_session_lifecycle();
    test_close_releases_notes();
    test_threaded();
    if (g_failures == 0) std::printf("dve_midi_output_session_tests: PASS\n");
    return g_failures == 0 ? 0 : 1;
}
