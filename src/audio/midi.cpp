#include "dve/audio/midi.hpp"

#include <algorithm>
#include <exception>
#include <utility>

namespace dve::audio {
namespace {
constexpr std::uint8_t clamp_channel(std::uint8_t channel) noexcept { return static_cast<std::uint8_t>(channel & 0x0FU); }
constexpr std::uint8_t clamp7(std::uint8_t value) noexcept { return static_cast<std::uint8_t>(value & 0x7FU); }

MidiMessage channel_message(MidiMessageType type, std::uint8_t statusBase, std::uint8_t channel,
                            std::uint8_t data1, std::uint8_t data2, std::uint8_t size,
                            std::uint64_t sampleFrame) noexcept {
    MidiMessage result;
    result.type = type;
    result.channel = clamp_channel(channel);
    result.status = static_cast<std::uint8_t>(statusBase | result.channel);
    result.data1 = clamp7(data1);
    result.data2 = clamp7(data2);
    result.size = size;
    result.sampleFrame = sampleFrame;
    result.bytes[0] = result.status;
    if (size > 1) result.bytes[1] = result.data1;
    if (size > 2) result.bytes[2] = result.data2;
    return result;
}
}

MidiMessage MidiMessage::note_on(std::uint8_t channel, std::uint8_t note, std::uint8_t velocity,
                                 std::uint64_t sampleFrame) noexcept {
    return channel_message(MidiMessageType::NoteOn, 0x90U, channel, note, velocity, 3, sampleFrame);
}
MidiMessage MidiMessage::note_off(std::uint8_t channel, std::uint8_t note, std::uint8_t velocity,
                                  std::uint64_t sampleFrame) noexcept {
    return channel_message(MidiMessageType::NoteOff, 0x80U, channel, note, velocity, 3, sampleFrame);
}
MidiMessage MidiMessage::control_change(std::uint8_t channel, std::uint8_t controller, std::uint8_t value,
                                        std::uint64_t sampleFrame) noexcept {
    return channel_message(MidiMessageType::ControlChange, 0xB0U, channel, controller, value, 3, sampleFrame);
}
MidiMessage MidiMessage::pitch_bend(std::uint8_t channel, std::int16_t centeredValue,
                                    std::uint64_t sampleFrame) noexcept {
    const int bounded = std::clamp<int>(centeredValue, -8192, 8191) + 8192;
    return channel_message(MidiMessageType::PitchBend, 0xE0U, channel,
                           static_cast<std::uint8_t>(bounded & 0x7F),
                           static_cast<std::uint8_t>((bounded >> 7) & 0x7F), 3, sampleFrame);
}
MidiMessage MidiMessage::program_change(std::uint8_t channel, std::uint8_t program,
                                        std::uint64_t sampleFrame) noexcept {
    return channel_message(MidiMessageType::ProgramChange, 0xC0U, channel, program, 0, 2, sampleFrame);
}
MidiMessage MidiMessage::realtime(std::uint8_t statusByte, std::uint64_t sampleFrame) noexcept {
    MidiMessage result;
    result.type = MidiMessageType::SystemRealtime;
    result.status = statusByte;
    result.bytes[0] = statusByte;
    result.size = 1;
    result.sampleFrame = sampleFrame;
    return result;
}

std::optional<MidiMessage> MidiMessage::parse(std::span<const std::uint8_t> input,
                                              std::uint64_t sampleFrame) noexcept {
    if (input.empty()) return std::nullopt;
    const std::uint8_t statusByte = input[0];
    if ((statusByte & 0x80U) == 0) return std::nullopt;
    MidiMessage result;
    result.status = statusByte;
    result.sampleFrame = sampleFrame;
    result.channel = static_cast<std::uint8_t>(statusByte & 0x0FU);
    const std::uint8_t high = static_cast<std::uint8_t>(statusByte & 0xF0U);
    std::size_t required = 0;
    switch (high) {
        case 0x80U: result.type = MidiMessageType::NoteOff; required = 3; break;
        case 0x90U: result.type = MidiMessageType::NoteOn; required = 3; break;
        case 0xA0U: result.type = MidiMessageType::PolyPressure; required = 3; break;
        case 0xB0U: result.type = MidiMessageType::ControlChange; required = 3; break;
        case 0xC0U: result.type = MidiMessageType::ProgramChange; required = 2; break;
        case 0xD0U: result.type = MidiMessageType::ChannelPressure; required = 2; break;
        case 0xE0U: result.type = MidiMessageType::PitchBend; required = 3; break;
        case 0xF0U:
            if (statusByte >= 0xF8U) { result.type = MidiMessageType::SystemRealtime; required = 1; }
            else if (statusByte == 0xF0U) {
                result.type = MidiMessageType::SysEx;
                const auto end = std::find(input.begin(), input.end(), static_cast<std::uint8_t>(0xF7U));
                required = end == input.end() ? input.size() : static_cast<std::size_t>(end - input.begin()) + 1U;
            } else {
                result.type = MidiMessageType::SystemCommon;
                required = statusByte == 0xF1U || statusByte == 0xF3U ? 2U : (statusByte == 0xF2U ? 3U : 1U);
            }
            break;
        default: return std::nullopt;
    }
    if (input.size() < required || required > result.bytes.size()) return std::nullopt;
    result.size = static_cast<std::uint8_t>(required);
    for (std::size_t i = 0; i < required; ++i) result.bytes[i] = input[i];
    if (required > 1) result.data1 = static_cast<std::uint8_t>(input[1] & 0x7FU);
    if (required > 2) result.data2 = static_cast<std::uint8_t>(input[2] & 0x7FU);
    return result;
}

std::vector<MidiPortDescriptor> VirtualMidiBackend::input_ports() const {
    return {{0, "DVE Synth Virtual In", true, false, true}};
}
std::vector<MidiPortDescriptor> VirtualMidiBackend::output_ports() const {
    return {{0, "DVE Synth Virtual Out", false, true, true}};
}
bool VirtualMidiBackend::open_input(std::size_t portIndex, MidiInputCallback callback, std::string* error) {
    if (portIndex != 0 || !callback) {
        if (error) *error = "invalid virtual MIDI input port or callback";
        return false;
    }
    callback_ = std::move(callback);
    inputOpen_ = true;
    return true;
}
bool VirtualMidiBackend::open_output(std::size_t portIndex, std::string* error) {
    if (portIndex != 0) {
        if (error) *error = "invalid virtual MIDI output port";
        return false;
    }
    outputOpen_ = true;
    return true;
}
void VirtualMidiBackend::close_input() noexcept { inputOpen_ = false; callback_ = {}; }
void VirtualMidiBackend::close_output() noexcept { outputOpen_ = false; }
bool VirtualMidiBackend::send(const MidiMessage& message, std::string* error) {
    if (!outputOpen_) {
        if (error) *error = "virtual MIDI output is not open";
        return false;
    }
    if (!message.valid()) {
        if (error) *error = "invalid MIDI message";
        return false;
    }
    sent_.push_back(message);
    return true;
}
bool VirtualMidiBackend::inject(const MidiMessage& message, std::string* error) {
    if (!inputOpen_ || !callback_) {
        if (error) *error = "virtual MIDI input is not open";
        return false;
    }
    if (!message.valid()) {
        if (error) *error = "invalid MIDI message";
        return false;
    }
    callback_(message);
    return true;
}

#ifndef DVE_HAVE_RTMIDI
std::unique_ptr<IMidiBackend> make_native_midi_backend(std::string* error) {
    if (error) *error = "RtMidi was not compiled; use DVE Virtual MIDI or provide RtMidi";
    return {};
}
#endif

} // namespace dve::audio
