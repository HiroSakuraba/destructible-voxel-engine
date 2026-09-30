#include "dve/audio/midi_input_session.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <utility>

namespace dve::audio {
namespace {

std::string lower_copy(std::string_view text) {
    std::string result(text);
    for (char& c : result) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}

bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

// Drops one trailing " <digits>" token (Windows/WinMM appends the device index to port names).
std::string strip_trailing_index(std::string_view name) {
    std::size_t end = name.size();
    while (end > 0 && is_digit(name[end - 1])) --end;
    if (end == name.size() || end == 0 || name[end - 1] != ' ') return std::string(name);
    return std::string(name.substr(0, end - 1));
}

bool same_port(std::string_view a, std::string_view b) {
    return a == b || normalize_midi_port_name(a) == normalize_midi_port_name(b);
}

void send_release_all(const MidiInputSession::Sink& sink) {
    if (!sink) return;
    for (std::uint8_t channel = 0; channel < 16U; ++channel) {
        sink(MidiMessage::control_change(channel, 64U, 0U));  // sustain off first, or 123 notes stay held
        sink(MidiMessage::control_change(channel, 123U, 0U)); // all notes off
        sink(MidiMessage::pitch_bend(channel, 0));
    }
}

} // namespace

bool MidiChannelFilter::accepts(const MidiMessage& message) const noexcept {
    switch (message.type) {
        case MidiMessageType::NoteOff: case MidiMessageType::NoteOn: case MidiMessageType::PolyPressure:
        case MidiMessageType::ControlChange: case MidiMessageType::ProgramChange:
        case MidiMessageType::ChannelPressure: case MidiMessageType::PitchBend:
            break;
        default:
            return true;
    }
    const std::uint8_t messageChannel = static_cast<std::uint8_t>(message.channel & 0x0FU);
    if (messageChannel == kMidiDrumChannel) {
        if (drums == MidiDrumChannelMode::Play) return true;
        if (drums == MidiDrumChannelMode::Ignore) return false;
    }
    return channel == 0U || messageChannel + 1U == channel;
}

std::string_view midi_connection_state_name(MidiConnectionState state) noexcept {
    switch (state) {
        case MidiConnectionState::Unavailable: return "unavailable";
        case MidiConnectionState::Disabled: return "off";
        case MidiConnectionState::NoDevice: return "no device";
        case MidiConnectionState::Connected: return "connected";
        case MidiConnectionState::Disconnected: return "disconnected";
        case MidiConnectionState::Error: return "error";
    }
    return "unknown";
}

std::string normalize_midi_port_name(std::string_view name) {
    // Trailing " <client>:<port>" as produced by RtMidi's ALSA backend.
    std::size_t end = name.size();
    std::size_t i = end;
    while (i > 0 && is_digit(name[i - 1])) --i;
    if (i == end || i == 0 || name[i - 1] != ':') return std::string(name);
    std::size_t j = i - 1;
    std::size_t k = j;
    while (k > 0 && is_digit(name[k - 1])) --k;
    if (k == j || k == 0 || name[k - 1] != ' ') return std::string(name);
    return std::string(name.substr(0, k - 1));
}

std::string midi_port_display_name(std::string_view name) {
    const std::string normalized = normalize_midi_port_name(name);
    const std::size_t colon = normalized.find(':');
    if (colon != std::string::npos && colon > 0) return normalized.substr(0, colon);
    return normalized;
}

bool is_midi_through_or_virtual_port(const MidiPortDescriptor& port) noexcept {
    if (port.virtualPort) return true;
    try {
        const std::string name = lower_copy(port.name);
        for (std::string_view token : {"through", "rtmidi", "dve synth", "loopmidi", "loopbe", "iac driver",
                                       "iac bus", "virtual", "midi mapper"})
            if (name.find(token) != std::string::npos) return true;
    } catch (...) {
        return false;
    }
    return false;
}

std::optional<std::size_t> choose_midi_input_port(std::span<const MidiPortDescriptor> ports,
                                                  std::string_view requested) {
    if (requested == kMidiPortNone) return std::nullopt;
    if (requested.empty()) {
        for (std::size_t i = 0; i < ports.size(); ++i)
            if (!is_midi_through_or_virtual_port(ports[i])) return i;
        return std::nullopt;
    }
    for (std::size_t i = 0; i < ports.size(); ++i)
        if (ports[i].name == requested) return i;
    const std::string normalized = normalize_midi_port_name(requested);
    for (std::size_t i = 0; i < ports.size(); ++i)
        if (normalize_midi_port_name(ports[i].name) == normalized) return i;
    const std::string loose = strip_trailing_index(normalized);
    for (std::size_t i = 0; i < ports.size(); ++i)
        if (strip_trailing_index(normalize_midi_port_name(ports[i].name)) == loose) return i;
    return std::nullopt;
}

std::string MidiInputStatus::summary() const {
    const auto named = [&](std::string_view port, std::string_view what) {
        return midi_port_display_name(port) + ": " + std::string(what);
    };
    switch (state) {
        case MidiConnectionState::Unavailable: return "MIDI unavailable (no RtMidi / MIDI system)";
        case MidiConnectionState::Disabled: return "MIDI input off";
        case MidiConnectionState::NoDevice:
            return lastConnectedPort.empty() ? std::string("No MIDI device") : named(lastConnectedPort, "disconnected");
        case MidiConnectionState::Connected: return named(connectedPort, "connected");
        case MidiConnectionState::Disconnected: return named(requestedPort, "disconnected");
        case MidiConnectionState::Error:
            return named(requestedPort.empty() ? lastConnectedPort : requestedPort, "error") +
                   (error.empty() ? std::string() : " (" + error + ")");
    }
    return {};
}

FakeMidiPortBackend::FakeMidiPortBackend(std::vector<std::string> inputPorts) : ports_(std::move(inputPorts)) {}

std::vector<MidiPortDescriptor> FakeMidiPortBackend::input_ports() const {
    std::lock_guard lock(mutex_);
    std::vector<MidiPortDescriptor> result;
    result.reserve(ports_.size());
    for (std::size_t i = 0; i < ports_.size(); ++i) result.push_back({i, ports_[i], true, false, false});
    return result;
}

bool FakeMidiPortBackend::open_input(std::size_t portIndex, MidiInputCallback callback, std::string* error) {
    std::lock_guard lock(mutex_);
    if (!failOpen_.empty()) {
        if (error) *error = failOpen_;
        failOpen_.clear();
        return false;
    }
    if (portIndex >= ports_.size() || !callback) {
        if (error) *error = "invalid fake MIDI input port or callback";
        return false;
    }
    callback_ = std::move(callback);
    openPort_ = ports_[portIndex];
    open_ = true;
    ++openCount_;
    return true;
}

void FakeMidiPortBackend::close_input() noexcept {
    std::lock_guard lock(mutex_);  // waits for an inject() in flight, like RtMidi joining its thread
    open_ = false;
    openPort_.clear();
    callback_ = {};
}

bool FakeMidiPortBackend::input_open() const noexcept {
    std::lock_guard lock(mutex_);
    return open_;
}

void FakeMidiPortBackend::set_ports(std::vector<std::string> inputPorts) {
    std::lock_guard lock(mutex_);
    ports_ = std::move(inputPorts);
}

void FakeMidiPortBackend::plug(std::string port) {
    std::lock_guard lock(mutex_);
    ports_.push_back(std::move(port));
}

void FakeMidiPortBackend::unplug(std::string_view port) {
    std::lock_guard lock(mutex_);
    std::erase_if(ports_, [&](const std::string& name) { return name == port; });
    // A vanished device stops delivering, but the port stays "open" until the session closes it
    // (RtMidi behaves the same way: nothing tells it the device went away).
}

bool FakeMidiPortBackend::inject(const MidiMessage& message) {
    std::lock_guard lock(mutex_);
    if (!open_ || !callback_) return false;
    if (std::none_of(ports_.begin(), ports_.end(), [&](const std::string& name) { return name == openPort_; }))
        return false;
    callback_(message);
    return true;
}

std::string FakeMidiPortBackend::open_port_name() const {
    std::lock_guard lock(mutex_);
    return openPort_;
}

std::size_t FakeMidiPortBackend::open_count() const {
    std::lock_guard lock(mutex_);
    return openCount_;
}

void FakeMidiPortBackend::fail_next_open(std::string message) {
    std::lock_guard lock(mutex_);
    failOpen_ = std::move(message);
}

MidiInputSession::MidiInputSession(std::unique_ptr<IMidiBackend> backend, Sink sink, Options options)
    : backend_(std::move(backend)), sink_(std::move(sink)), options_(options) {
    status_.state = backend_ ? MidiConnectionState::NoDevice : MidiConnectionState::Unavailable;
    if (backend_) status_.backendName = std::string(backend_->backend_name());
    if (backend_ && options_.startThread) worker_ = std::thread([this] { worker_main(); });
}

MidiInputSession::~MidiInputSession() {
    {
        std::lock_guard lock(stateMutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard lock(backendMutex_);
    if (connected_) close_and_release();
}

void MidiInputSession::set_requested_port(std::string name) {
    {
        std::lock_guard lock(stateMutex_);
        requested_ = std::move(name);
        wakeRequested_ = true;
    }
    wake_.notify_all();
}

void MidiInputSession::set_channel_filter(MidiChannelFilter filter) noexcept {
    filterChannel_.store(std::min<std::uint8_t>(filter.channel, 16U), std::memory_order_relaxed);
    filterDrums_.store(static_cast<std::uint8_t>(filter.drums), std::memory_order_relaxed);
}

MidiChannelFilter MidiInputSession::channel_filter() const noexcept {
    return {filterChannel_.load(std::memory_order_relaxed),
            static_cast<MidiDrumChannelMode>(filterDrums_.load(std::memory_order_relaxed))};
}

void MidiInputSession::request_rescan() noexcept {
    {
        std::lock_guard lock(stateMutex_);
        wakeRequested_ = true;
    }
    wake_.notify_all();
}

MidiInputStatus MidiInputSession::status() const {
    std::lock_guard lock(stateMutex_);
    return status_;
}

void MidiInputSession::on_input(const MidiMessage& message) noexcept {
    received_.fetch_add(1U, std::memory_order_relaxed);
    if (!channel_filter().accepts(message)) {
        filtered_.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    try { if (sink_) sink_(message); } catch (...) {}
}

void MidiInputSession::close_and_release() {
    // close_input() stops RtMidi's callback thread, so nothing can arrive after the release.
    backend_->close_input();
    connected_ = false;
    connectedPort_.clear();
    connectedRequest_.clear();
    try { send_release_all(sink_); } catch (...) {}
}

void MidiInputSession::publish(MidiInputStatus next) {
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

void MidiInputSession::poll_now() {
    std::lock_guard backendLock(backendMutex_);
    MidiInputStatus next;
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
        ports = backend_->input_ports();
    } catch (const std::exception& exception) {
        next.error = exception.what();
    } catch (...) {
        next.error = "could not list MIDI ports";
    }
    next.ports.reserve(ports.size());
    for (const MidiPortDescriptor& port : ports) next.ports.push_back(port.name);

    if (connected_) {
        const bool present = std::any_of(ports.begin(), ports.end(),
            [&](const MidiPortDescriptor& port) { return same_port(port.name, connectedPort_); });
        if (connectedRequest_ != next.requestedPort) {
            close_and_release();
        } else if (!present || !backend_->input_open()) {
            next.lastConnectedPort = connectedPort_;
            close_and_release();
            ++disconnects_;
        }
    }
    MidiConnectionState state = MidiConnectionState::NoDevice;
    if (!connected_) {
        if (next.requestedPort == kMidiPortNone) {
            state = MidiConnectionState::Disabled;
        } else if (const auto index = choose_midi_input_port(ports, next.requestedPort)) {
            std::string error;
            if (backend_->open_input(ports[*index].index,
                                     [this](const MidiMessage& message) { on_input(message); }, &error)) {
                connected_ = true;
                connectedPort_ = ports[*index].name;
                connectedRequest_ = next.requestedPort;
                ++connects_;
            } else {
                state = MidiConnectionState::Error;
                next.error = error.empty() ? std::string("could not open MIDI input") : error;
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
    }
    next.state = state;
    next.connects = connects_;
    next.disconnects = disconnects_;
    publish(std::move(next));
}

void MidiInputSession::worker_main() {
    for (;;) {
        try { poll_now(); } catch (...) {}
        std::unique_lock lock(stateMutex_);
        wake_.wait_for(lock, options_.pollInterval, [&] { return stop_ || wakeRequested_; });
        if (stop_) return;
        wakeRequested_ = false;
    }
}

} // namespace dve::audio
