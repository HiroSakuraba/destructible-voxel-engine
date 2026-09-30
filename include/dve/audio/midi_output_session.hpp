#pragma once

// MIDI output session: picks, opens and supervises one MIDI output port for the synth's MIDI out
// (MIDI thru / arpeggiator output). It mirrors MidiInputSession:
//
// * Port choice is by name (persisted by the editor as `midi.output_port`): an empty request
//   means "Auto", kMidiPortNone disables output, anything else names a port. Names are matched
//   like input ports (exactly, then without the volatile ALSA " 24:0" suffix, then without a
//   trailing WinMM device index), so a replugged device is found again.
// * Auto keeps the editor's previous behaviour: the first output port (on Linux usually
//   "Midi Through", which other applications can listen to). Pick a device by name to send to
//   hardware.
// * Hotplug: a worker thread re-enumerates ports every pollInterval (about 1.5 s), reconnects
//   the requested port when it reappears and closes a port that vanished.
// * Before a port that is still present is closed (port change, None, shutdown), sustain-off,
//   all-notes-off and pitch-bend-center are sent on all 16 channels so the receiver does not
//   keep notes hanging.
//
// Threading: the backend is only used under backendMutex_. send() never blocks on the backend:
// it queues the message and flushes the queue when the backend is free (otherwise the worker
// or the next send() flushes it). Messages sent while no port is connected are dropped.

#include "dve/audio/midi_input_session.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace dve::audio {

// Index into `ports` of the output port to open for `requested` ("" = Auto = first port,
// "none" = nothing).
[[nodiscard]] std::optional<std::size_t> choose_midi_output_port(std::span<const MidiPortDescriptor> ports,
                                                                 std::string_view requested);

struct MidiOutputStatus {
    MidiConnectionState state{MidiConnectionState::Unavailable};
    std::string backendName;
    std::string requestedPort;     // as requested ("" = Auto)
    std::string connectedPort;     // full name of the open port
    std::string lastConnectedPort; // last port that was open
    std::string error;
    std::vector<std::string> ports; // output port names from the last enumeration
    std::uint64_t generation{};
    std::uint64_t connects{};
    std::uint64_t disconnects{};

    // "Midi Through: connected", "MPK mini 3: disconnected", "No MIDI output", "MIDI output off"
    [[nodiscard]] std::string summary() const;
};

class MidiOutputSession {
public:
    struct Options {
        std::chrono::milliseconds pollInterval{1500};
        bool startThread{true}; // false: nothing happens until poll_now() (deterministic tests)
        std::size_t maximumQueuedMessages{4096};
    };

    // `backend` may be null (no native MIDI): the session then reports Unavailable.
    MidiOutputSession(std::unique_ptr<IMidiBackend> backend, Options options);
    explicit MidiOutputSession(std::unique_ptr<IMidiBackend> backend)
        : MidiOutputSession(std::move(backend), Options{}) {}
    ~MidiOutputSession();
    MidiOutputSession(const MidiOutputSession&) = delete;
    MidiOutputSession& operator=(const MidiOutputSession&) = delete;

    // Any thread; never blocks on the backend. The worker applies it right away.
    void set_requested_port(std::string name);
    void request_rescan() noexcept;

    // Queues `message` for the connected port (false, and dropped, when nothing is connected or
    // the queue is full). Never blocks on the backend.
    bool send(const MidiMessage& message);

    [[nodiscard]] MidiOutputStatus status() const;
    [[nodiscard]] std::uint64_t status_generation() const noexcept { return generation_.load(std::memory_order_acquire); }
    [[nodiscard]] bool has_backend() const noexcept { return backend_ != nullptr; }
    [[nodiscard]] bool connected() const noexcept { return connectedFlag_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t messages_sent() const noexcept { return sent_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t messages_dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

    // One enumerate + reconcile step (and a queue flush) on the calling thread.
    void poll_now();

private:
    void worker_main();
    void flush_locked();           // backendMutex_ held
    void close_locked(bool release); // backendMutex_ held
    void publish(MidiOutputStatus next);

    std::unique_ptr<IMidiBackend> backend_;
    Options options_;

    std::atomic<std::uint64_t> sent_{};
    std::atomic<std::uint64_t> dropped_{};
    std::atomic<std::uint64_t> generation_{};
    std::atomic<bool> connectedFlag_{};

    std::mutex queueMutex_;
    std::deque<MidiMessage> queue_;

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
    MidiOutputStatus status_;

    std::thread worker_;
};

} // namespace dve::audio
