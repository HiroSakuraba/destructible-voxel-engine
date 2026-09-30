#include "dve/audio/midi_output_session.hpp"

#include <algorithm>
#include <exception>
#include <utility>

namespace dve::audio {
namespace {

bool same_port(std::string_view a, std::string_view b) {
    return a == b || normalize_midi_port_name(a) == normalize_midi_port_name(b);
}

} // namespace

std::optional<std::size_t> choose_midi_output_port(std::span<const MidiPortDescriptor> ports,
                                                   std::string_view requested) {
    if (requested == kMidiPortNone) return std::nullopt;
    if (requested.empty()) {
        if (ports.empty()) return std::nullopt;
        return std::size_t{0};
    }
    // Named requests resolve exactly like input ports.
    return choose_midi_input_port(ports, requested);
}

std::string MidiOutputStatus::summary() const {
    const auto named = [&](std::string_view port, std::string_view what) {
        return midi_port_display_name(port) + ": " + std::string(what);
    };
    switch (state) {
        case MidiConnectionState::Unavailable: return "MIDI unavailable (no RtMidi / MIDI system)";
        case MidiConnectionState::Disabled: return "MIDI output off";
        case MidiConnectionState::NoDevice:
            return lastConnectedPort.empty() ? std::string("No MIDI output") : named(lastConnectedPort, "disconnected");
        case MidiConnectionState::Connected: return named(connectedPort, "connected");
        case MidiConnectionState::Disconnected: return named(requestedPort, "disconnected");
        case MidiConnectionState::Error:
            return named(requestedPort.empty() ? lastConnectedPort : requestedPort, "error") +
                   (error.empty() ? std::string() : " (" + error + ")");
    }
    return {};
}

MidiOutputSession::MidiOutputSession(std::unique_ptr<IMidiBackend> backend, Options options)
    : backend_(std::move(backend)), options_(options) {
    status_.state = backend_ ? MidiConnectionState::NoDevice : MidiConnectionState::Unavailable;
    if (backend_) status_.backendName = std::string(backend_->backend_name());
    if (backend_ && options_.startThread) worker_ = std::thread([this] { worker_main(); });
}

MidiOutputSession::~MidiOutputSession() {
    {
        std::lock_guard lock(stateMutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard lock(backendMutex_);
    if (connected_) {
        flush_locked();
        close_locked(true);
    }
}

void MidiOutputSession::set_requested_port(std::string name) {
    {
        std::lock_guard lock(stateMutex_);
        requested_ = std::move(name);
        wakeRequested_ = true;
    }
    wake_.notify_all();
}

void MidiOutputSession::request_rescan() noexcept {
    {
        std::lock_guard lock(stateMutex_);
        wakeRequested_ = true;
    }
    wake_.notify_all();
}

MidiOutputStatus MidiOutputSession::status() const {
    std::lock_guard lock(stateMutex_);
    return status_;
}

bool MidiOutputSession::send(const MidiMessage& message) {
    if (!connectedFlag_.load(std::memory_order_acquire)) {
        dropped_.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    {
        std::lock_guard lock(queueMutex_);
        if (queue_.size() >= options_.maximumQueuedMessages) {
            dropped_.fetch_add(1U, std::memory_order_relaxed);
            return false;
        }
        queue_.push_back(message);
    }
    std::unique_lock backendLock(backendMutex_, std::try_to_lock);
    if (backendLock.owns_lock()) flush_locked();
    return true;
}

void MidiOutputSession::flush_locked() {
    std::deque<MidiMessage> pending;
    {
        std::lock_guard lock(queueMutex_);
        pending.swap(queue_);
    }
    for (const MidiMessage& message : pending) {
        bool ok = false;
        if (connected_) {
            try { ok = backend_->send(message); } catch (...) { ok = false; }
        }
        (ok ? sent_ : dropped_).fetch_add(1U, std::memory_order_relaxed);
    }
}

void MidiOutputSession::close_locked(bool release) {
    if (release) {
        // The receiver would otherwise keep sounding whatever was held when the port closed.
        for (std::uint8_t channel = 0; channel < 16U; ++channel) {
            try {
                (void)backend_->send(MidiMessage::control_change(channel, 64U, 0U));
                (void)backend_->send(MidiMessage::control_change(channel, 123U, 0U));
                (void)backend_->send(MidiMessage::pitch_bend(channel, 0));
            } catch (...) {
            }
        }
    }
    backend_->close_output();
    connected_ = false;
    connectedFlag_.store(false, std::memory_order_release);
    connectedPort_.clear();
    connectedRequest_.clear();
}

void MidiOutputSession::publish(MidiOutputStatus next) {
    std::lock_guard lock(stateMutex_);
    const bool changed = next.state != status_.state || next.backendName != status_.backendName ||
        next.requestedPort != status_.requestedPort || next.connectedPort != status_.connectedPort ||
        next.lastConnectedPort != status_.lastConnectedPort || next.error != status_.error ||
        next.ports != status_.ports || next.connects != status_.connects ||
        next.disconnects != status_.disconnects;
    if (!changed) return;
    next.generation = status_.generation + 1U;
    status_ = std::move(next);
    generation_.store(status_.generation, std::memory_order_release);
}

void MidiOutputSession::poll_now() {
    std::lock_guard backendLock(backendMutex_);
    MidiOutputStatus next;
    if (!backend_) {
        next.state = MidiConnectionState::Unavailable;
        publish(std::move(next));
        return;
    }
    next.backendName = std::string(backend_->backend_name());
    {
        std::lock_guard lock(stateMutex_);
        next.requestedPort = requested_;
        next.lastConnectedPort = status_.lastConnectedPort;
    }
    std::vector<MidiPortDescriptor> ports;
    try {
        ports = backend_->output_ports();
    } catch (const std::exception& exception) {
        next.error = exception.what();
    } catch (...) {
        next.error = "could not list MIDI ports";
    }
    next.ports.reserve(ports.size());
    for (const MidiPortDescriptor& port : ports) next.ports.push_back(port.name);

    if (connected_) {
        flush_locked(); // deliver what was queued for the old port first
        const bool present = std::any_of(ports.begin(), ports.end(),
            [&](const MidiPortDescriptor& port) { return same_port(port.name, connectedPort_); });
        if (!present || !backend_->output_open()) {
            next.lastConnectedPort = connectedPort_;
            close_locked(false); // the device is gone: nothing to release on it
            ++disconnects_;
        } else if (connectedRequest_ != next.requestedPort) {
            close_locked(true);
        }
    }
    MidiConnectionState state = MidiConnectionState::NoDevice;
    if (!connected_) {
        if (next.requestedPort == kMidiPortNone) {
            state = MidiConnectionState::Disabled;
        } else if (const auto index = choose_midi_output_port(ports, next.requestedPort)) {
            std::string error;
            bool opened = false;
            try { opened = backend_->open_output(ports[*index].index, &error); } catch (...) { opened = false; }
            if (opened) {
                connected_ = true;
                connectedFlag_.store(true, std::memory_order_release);
                connectedPort_ = ports[*index].name;
                connectedRequest_ = next.requestedPort;
                ++connects_;
            } else {
                state = MidiConnectionState::Error;
                next.error = error.empty() ? std::string("could not open MIDI output") : error;
                if (next.lastConnectedPort.empty()) next.lastConnectedPort = ports[*index].name;
            }
        } else {
            state = next.requestedPort.empty() ? MidiConnectionState::NoDevice : MidiConnectionState::Disconnected;
        }
    }
    if (connected_) {
        state = MidiConnectionState::Connected;
        next.connectedPort = connectedPort_;
        next.lastConnectedPort = connectedPort_;
        next.error.clear();
        flush_locked();
    }
    next.state = state;
    next.connects = connects_;
    next.disconnects = disconnects_;
    publish(std::move(next));
}

void MidiOutputSession::worker_main() {
    for (;;) {
        try { poll_now(); } catch (...) {}
        std::unique_lock lock(stateMutex_);
        wake_.wait_for(lock, options_.pollInterval, [&] { return stop_ || wakeRequested_; });
        if (stop_) return;
        wakeRequested_ = false;
    }
}

} // namespace dve::audio
