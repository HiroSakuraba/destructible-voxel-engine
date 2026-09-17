#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dve::audio {

enum class MidiMessageType : std::uint8_t {
    Invalid,
    NoteOff,
    NoteOn,
    PolyPressure,
    ControlChange,
    ProgramChange,
    ChannelPressure,
    PitchBend,
    SystemRealtime,
    SystemCommon,
    SysEx,
};

struct MidiMessage {
    MidiMessageType type{MidiMessageType::Invalid};
    std::uint8_t status{};
    std::uint8_t channel{};
    std::uint8_t data1{};
    std::uint8_t data2{};
    std::array<std::uint8_t, 16> bytes{};
    std::uint8_t size{};
    std::uint64_t sampleFrame{};

    [[nodiscard]] bool valid() const noexcept { return type != MidiMessageType::Invalid && size != 0; }
    [[nodiscard]] bool is_note_on() const noexcept { return type == MidiMessageType::NoteOn && data2 != 0; }
    [[nodiscard]] bool is_note_off() const noexcept {
        return type == MidiMessageType::NoteOff || (type == MidiMessageType::NoteOn && data2 == 0);
    }
    [[nodiscard]] std::span<const std::uint8_t> raw_bytes() const noexcept { return {bytes.data(), size}; }

    static MidiMessage note_on(std::uint8_t channel, std::uint8_t note, std::uint8_t velocity,
                               std::uint64_t sampleFrame = 0) noexcept;
    static MidiMessage note_off(std::uint8_t channel, std::uint8_t note, std::uint8_t velocity = 0,
                                std::uint64_t sampleFrame = 0) noexcept;
    static MidiMessage control_change(std::uint8_t channel, std::uint8_t controller, std::uint8_t value,
                                      std::uint64_t sampleFrame = 0) noexcept;
    static MidiMessage pitch_bend(std::uint8_t channel, std::int16_t centeredValue,
                                  std::uint64_t sampleFrame = 0) noexcept;
    static MidiMessage program_change(std::uint8_t channel, std::uint8_t program,
                                      std::uint64_t sampleFrame = 0) noexcept;
    static MidiMessage realtime(std::uint8_t status, std::uint64_t sampleFrame = 0) noexcept;
    static std::optional<MidiMessage> parse(std::span<const std::uint8_t> bytes,
                                            std::uint64_t sampleFrame = 0) noexcept;
};

struct MidiPortDescriptor {
    std::size_t index{};
    std::string name;
    bool input{};
    bool output{};
    bool virtualPort{};
};

using MidiInputCallback = std::function<void(const MidiMessage&)>;

class IMidiBackend {
public:
    virtual ~IMidiBackend() = default;
    [[nodiscard]] virtual std::string_view backend_name() const noexcept = 0;
    [[nodiscard]] virtual std::vector<MidiPortDescriptor> input_ports() const = 0;
    [[nodiscard]] virtual std::vector<MidiPortDescriptor> output_ports() const = 0;
    virtual bool open_input(std::size_t portIndex, MidiInputCallback callback, std::string* error = nullptr) = 0;
    virtual bool open_output(std::size_t portIndex, std::string* error = nullptr) = 0;
    virtual void close_input() noexcept = 0;
    virtual void close_output() noexcept = 0;
    [[nodiscard]] virtual bool input_open() const noexcept = 0;
    [[nodiscard]] virtual bool output_open() const noexcept = 0;
    virtual bool send(const MidiMessage& message, std::string* error = nullptr) = 0;
};

// Deterministic full-duplex endpoint used by tests, offline rendering, automation, and as a
// virtual MIDI patch-bay inside the editor. It exercises both input and output contracts without
// claiming that an operating-system hardware port was opened.
class VirtualMidiBackend final : public IMidiBackend {
public:
    [[nodiscard]] std::string_view backend_name() const noexcept override { return "DVE Virtual MIDI"; }
    [[nodiscard]] std::vector<MidiPortDescriptor> input_ports() const override;
    [[nodiscard]] std::vector<MidiPortDescriptor> output_ports() const override;
    bool open_input(std::size_t portIndex, MidiInputCallback callback, std::string* error = nullptr) override;
    bool open_output(std::size_t portIndex, std::string* error = nullptr) override;
    void close_input() noexcept override;
    void close_output() noexcept override;
    [[nodiscard]] bool input_open() const noexcept override { return inputOpen_; }
    [[nodiscard]] bool output_open() const noexcept override { return outputOpen_; }
    bool send(const MidiMessage& message, std::string* error = nullptr) override;

    bool inject(const MidiMessage& message, std::string* error = nullptr);
    [[nodiscard]] const std::vector<MidiMessage>& sent_messages() const noexcept { return sent_; }
    void clear_sent() { sent_.clear(); }

private:
    MidiInputCallback callback_;
    std::vector<MidiMessage> sent_;
    bool inputOpen_{};
    bool outputOpen_{};
};

// Available only when RtMidi is found by CMake. The declaration remains stable so editor code
// never includes RtMidi headers. A null result means that no native backend was compiled.
[[nodiscard]] std::unique_ptr<IMidiBackend> make_native_midi_backend(std::string* error = nullptr);

} // namespace dve::audio
