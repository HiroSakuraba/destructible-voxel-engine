#pragma once

// MIDI input session: picks, opens and supervises one MIDI input port for the synth.
//
// * Port choice is by name (persisted by the editor as `midi.input_port`): an empty request means
//   "Auto" (first port that is not a Through / loopback / virtual port), kMidiPortNone disables
//   input, anything else names a port. Names are matched exactly first, then with the volatile
//   ALSA "client:port" suffix (" 24:0") removed, so a replugged device is found again.
// * Hotplug: a worker thread re-enumerates ports every pollInterval (about 1.5 s) and reconciles:
//   it reconnects the requested port when it reappears and closes a port that vanished. The UI
//   thread never touches the backend; it only reads a status snapshot and posts requests.
// * Disconnects (unplug, port change, shutdown) close the input first (RtMidi joins its callback
//   thread) and then send sustain-off, all-notes-off and pitch-bend-center on all 16 channels to
//   the sink, so no note is left hanging.
// * The channel filter (Omni or 1-16, plus a separate choice for channel 10 drum pads) is applied
//   on the MIDI callback thread using atomics.
//
// Threading: the backend is only used under backendMutex_ by the worker thread (or by poll_now()
// in threadless mode). The sink is called from the MIDI callback thread and from whichever thread
// runs the reconcile step; Synthesizer::post_midi is a lock-free MPMC queue push, so it is safe.

#include "dve/audio/midi.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace dve::audio {

inline constexpr std::string_view kMidiPortAuto{};
inline constexpr std::string_view kMidiPortNone{"none"};
inline constexpr std::uint8_t kMidiDrumChannel = 9U; // channel 10, zero-based

enum class MidiDrumChannelMode : std::uint8_t {
    Play,   // channel 10 always reaches the synth, whatever the channel filter says
    Ignore, // channel 10 never reaches the synth (also in Omni)
    Follow, // channel 10 is treated like any other channel
};

struct MidiChannelFilter {
    std::uint8_t channel{}; // 0 = Omni, 1..16 = only that channel
    MidiDrumChannelMode drums{MidiDrumChannelMode::Play};

    // Channel-less messages (clock, start/stop, SysEx) always pass.
    [[nodiscard]] bool accepts(const MidiMessage& message) const noexcept;
};

enum class MidiConnectionState : std::uint8_t {
    Unavailable,  // no native backend compiled/created
    Disabled,     // user chose "None"
    NoDevice,     // Auto and no suitable port present
    Connected,
    Disconnected, // the requested port is not present (unplugged)
    Error,        // opening failed
};

[[nodiscard]] std::string_view midi_connection_state_name(MidiConnectionState state) noexcept;

// "MPK mini 3:MPK mini 3 MIDI 1 24:0" -> "MPK mini 3:MPK mini 3 MIDI 1" (ALSA client:port ids
// change when a device is replugged).
[[nodiscard]] std::string normalize_midi_port_name(std::string_view name);
// Short user-facing name: the ALSA client name ("MPK mini 3") or the normalized name.
[[nodiscard]] std::string midi_port_display_name(std::string_view name);
// Through / loopback / virtual ports (ALSA "Midi Through", other RtMidi clients including our own
// output, macOS IAC, loopMIDI, ...). Auto never picks them; the user still can.
[[nodiscard]] bool is_midi_through_or_virtual_port(const MidiPortDescriptor& port) noexcept;
// Index into `ports` of the port to open for `requested` ("" = Auto, "none" = nothing).
[[nodiscard]] std::optional<std::size_t> choose_midi_input_port(std::span<const MidiPortDescriptor> ports,
                                                                std::string_view requested);

struct MidiInputStatus {
    MidiConnectionState state{MidiConnectionState::Unavailable};
    std::string backendName;
    std::string requestedPort;  // as requested ("" = Auto)
    std::string connectedPort;  // full name of the open port
    std::string lastConnectedPort; // last port that was open (for "X: disconnected")
    std::string error;
    std::vector<std::string> ports; // input port names from the last enumeration
    std::uint64_t generation{};     // bumps whenever anything above changes
    std::uint64_t connects{};
    std::uint64_t disconnects{};

    // "MPK mini 3: connected", "MPK mini 3: disconnected", "No MIDI device", "MIDI off", ...
    [[nodiscard]] std::string summary() const;
};

// In-memory backend with input and output port lists that can change at runtime (plug /
// unplug) and messages that can be injected from any thread, like RtMidi's callback thread.
// Output ports record what was sent. Used by the session tests and by the editors'
// --midi-fake-ports option (screenshots / demos without MIDI hardware).
class FakeMidiPortBackend final : public IMidiBackend {
public:
    explicit FakeMidiPortBackend(std::vector<std::string> inputPorts = {}, std::vector<std::string> outputPorts = {});
    [[nodiscard]] std::string_view backend_name() const noexcept override { return "Fake MIDI"; }
    [[nodiscard]] std::vector<MidiPortDescriptor> input_ports() const override;
    [[nodiscard]] std::vector<MidiPortDescriptor> output_ports() const override;
    bool open_input(std::size_t portIndex, MidiInputCallback callback, std::string* error = nullptr) override;
    bool open_output(std::size_t portIndex, std::string* error = nullptr) override;
    void close_input() noexcept override;
    void close_output() noexcept override;
    [[nodiscard]] bool input_open() const noexcept override;
    [[nodiscard]] bool output_open() const noexcept override;
    // Records the message on the open output (false when nothing is open or it was unplugged).
    bool send(const MidiMessage& message, std::string* error = nullptr) override;

    void set_ports(std::vector<std::string> inputPorts);
    void plug(std::string port);
    void unplug(std::string_view port);
    // Delivers to the open input's callback (false when nothing is open).
    bool inject(const MidiMessage& message);
    [[nodiscard]] std::string open_port_name() const;
    [[nodiscard]] std::size_t open_count() const;
    // When set, open_input fails with this message.
    void fail_next_open(std::string message);

    void set_output_ports(std::vector<std::string> outputPorts);
    void plug_output(std::string port);
    void unplug_output(std::string_view port);
    [[nodiscard]] std::string open_output_port_name() const;
    [[nodiscard]] std::size_t output_open_count() const;
    // Every message delivered to an output, with the port it went to.
    [[nodiscard]] std::vector<std::pair<std::string, MidiMessage>> sent_messages() const;
    void clear_sent();
    // When set, open_output fails with this message.
    void fail_next_output_open(std::string message);

private:
    mutable std::mutex mutex_;
    std::vector<std::string> ports_;
    MidiInputCallback callback_;
    std::string openPort_;
    std::string failOpen_;
    std::size_t openCount_{};
    bool open_{};
    std::vector<std::string> outputPorts_;
    std::string openOutputPort_;
    std::string failOutputOpen_;
    std::size_t outputOpenCount_{};
    bool outputOpen_{};
    std::vector<std::pair<std::string, MidiMessage>> sent_;
};

class MidiInputSession {
public:
    using Sink = std::function<void(const MidiMessage&)>;
    struct Options {
        std::chrono::milliseconds pollInterval{1500};
        bool startThread{true}; // false: nothing happens until poll_now() (deterministic tests)
    };

    // `backend` may be null (no native MIDI): the session then reports Unavailable.
    MidiInputSession(std::unique_ptr<IMidiBackend> backend, Sink sink, Options options);
    MidiInputSession(std::unique_ptr<IMidiBackend> backend, Sink sink)
        : MidiInputSession(std::move(backend), std::move(sink), Options{}) {}
    ~MidiInputSession();
    MidiInputSession(const MidiInputSession&) = delete;
    MidiInputSession& operator=(const MidiInputSession&) = delete;

    // Any thread; never blocks on the backend. The worker applies it right away.
    void set_requested_port(std::string name);
    void set_channel_filter(MidiChannelFilter filter) noexcept;
    [[nodiscard]] MidiChannelFilter channel_filter() const noexcept;
    void request_rescan() noexcept;

    [[nodiscard]] MidiInputStatus status() const;
    [[nodiscard]] std::uint64_t status_generation() const noexcept { return generation_.load(std::memory_order_acquire); }
    [[nodiscard]] bool has_backend() const noexcept { return backend_ != nullptr; }
    [[nodiscard]] std::uint64_t messages_received() const noexcept { return received_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t messages_filtered() const noexcept { return filtered_.load(std::memory_order_relaxed); }

    // One enumerate + reconcile step on the calling thread (used by the worker; call it directly
    // when Options::startThread is false).
    void poll_now();

private:
    void worker_main();
    void on_input(const MidiMessage& message) noexcept;
    void close_and_release(); // backendMutex_ held
    void publish(MidiInputStatus next);

    std::unique_ptr<IMidiBackend> backend_;
    Sink sink_;
    Options options_;

    std::atomic<std::uint8_t> filterChannel_{0};
    std::atomic<std::uint8_t> filterDrums_{static_cast<std::uint8_t>(MidiDrumChannelMode::Play)};
    std::atomic<std::uint64_t> received_{};
    std::atomic<std::uint64_t> filtered_{};
    std::atomic<std::uint64_t> generation_{};

    std::mutex backendMutex_; // backend + connection fields below
    bool connected_{};
    std::string connectedPort_;
    std::string connectedRequest_;
    std::uint64_t connects_{};
    std::uint64_t disconnects_{};

    mutable std::mutex stateMutex_; // requested_, status_, wake/stop flags
    std::condition_variable wake_;
    std::string requested_;
    bool wakeRequested_{};
    bool stop_{};
    MidiInputStatus status_;

    std::thread worker_;
};

} // namespace dve::audio
