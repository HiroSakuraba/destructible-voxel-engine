#include "dve/audio/midi.hpp"

#ifdef DVE_HAVE_RTMIDI
#include <RtMidi.h>

#include <memory>
#include <utility>
#include <vector>

namespace dve::audio {
namespace {
class RtMidiBackend final : public IMidiBackend {
public:
    RtMidiBackend() : input_(std::make_unique<RtMidiIn>()), output_(std::make_unique<RtMidiOut>()) {
        input_->ignoreTypes(false, false, false);
    }
    ~RtMidiBackend() override { close_input(); close_output(); }

    std::string_view backend_name() const noexcept override { return "RtMidi"; }
    std::vector<MidiPortDescriptor> input_ports() const override {
        std::vector<MidiPortDescriptor> result;
        const unsigned int count = input_->getPortCount();
        result.reserve(count);
        for (unsigned int i = 0; i < count; ++i) result.push_back({i, input_->getPortName(i), true, false, false});
        return result;
    }
    std::vector<MidiPortDescriptor> output_ports() const override {
        std::vector<MidiPortDescriptor> result;
        const unsigned int count = output_->getPortCount();
        result.reserve(count);
        for (unsigned int i = 0; i < count; ++i) result.push_back({i, output_->getPortName(i), false, true, false});
        return result;
    }
    bool open_input(std::size_t index, MidiInputCallback callback, std::string* error) override {
        try {
            if (index >= input_->getPortCount() || !callback) {
                if (error) *error = "invalid RtMidi input port or callback";
                return false;
            }
            close_input();
            callback_ = std::move(callback);
            input_->openPort(static_cast<unsigned int>(index), "DVE Synth Input");
            input_->setCallback([](double, std::vector<unsigned char>* bytes, void* user) {
                auto* self = static_cast<RtMidiBackend*>(user);
                if (self == nullptr || bytes == nullptr || self->callback_ == nullptr) return;
                const auto parsed = MidiMessage::parse(std::span<const std::uint8_t>(bytes->data(), bytes->size()));
                if (parsed) self->callback_(*parsed);
            }, this);
            inputOpen_ = true;
            return true;
        } catch (const RtMidiError& exception) {
            if (error) *error = exception.getMessage();
            return false;
        }
    }
    bool open_output(std::size_t index, std::string* error) override {
        try {
            if (index >= output_->getPortCount()) {
                if (error) *error = "invalid RtMidi output port";
                return false;
            }
            close_output();
            output_->openPort(static_cast<unsigned int>(index), "DVE Synth Output");
            outputOpen_ = true;
            return true;
        } catch (const RtMidiError& exception) {
            if (error) *error = exception.getMessage();
            return false;
        }
    }
    void close_input() noexcept override {
        try { if (input_ && input_->isPortOpen()) input_->closePort(); } catch (...) {}
        inputOpen_ = false;
        callback_ = {};
    }
    void close_output() noexcept override {
        try { if (output_ && output_->isPortOpen()) output_->closePort(); } catch (...) {}
        outputOpen_ = false;
    }
    bool input_open() const noexcept override { return inputOpen_; }
    bool output_open() const noexcept override { return outputOpen_; }
    bool send(const MidiMessage& message, std::string* error) override {
        if (!outputOpen_ || !message.valid()) {
            if (error) *error = !outputOpen_ ? "RtMidi output is not open" : "invalid MIDI message";
            return false;
        }
        try {
            const auto raw = message.raw_bytes();
            std::vector<unsigned char> bytes(raw.begin(), raw.end());
            output_->sendMessage(&bytes);
            return true;
        } catch (const RtMidiError& exception) {
            if (error) *error = exception.getMessage();
            return false;
        }
    }

private:
    std::unique_ptr<RtMidiIn> input_;
    std::unique_ptr<RtMidiOut> output_;
    MidiInputCallback callback_;
    bool inputOpen_{};
    bool outputOpen_{};
};
}

std::unique_ptr<IMidiBackend> make_native_midi_backend(std::string* error) {
    try {
        return std::make_unique<RtMidiBackend>();
    } catch (const RtMidiError& exception) {
        if (error) *error = exception.getMessage();
        return {};
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return {};
    }
}

} // namespace dve::audio
#endif
