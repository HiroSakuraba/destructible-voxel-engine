#include "dve/audio/chiptune.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <sstream>

namespace dve::audio {

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

[[nodiscard]] float clamp01(float v) noexcept { return std::clamp(v, 0.0F, 1.0F); }

[[nodiscard]] float frequency_from_midi(float midi) noexcept {
    return 440.0F * std::pow(2.0F, (midi - 69.0F) / 12.0F);
}

// PolyBLEP residual for suppressing alias energy at an oscillator discontinuity. t is the phase
// in [0,1); dt is the per-sample phase increment.
[[nodiscard]] float poly_blep(float t, float dt) noexcept {
    if (dt <= 0.0F) return 0.0F;
    if (t < dt) {
        const float x = t / dt;
        return x + x - x * x - 1.0F;
    }
    if (t > 1.0F - dt) {
        const float x = (t - 1.0F) / dt;
        return x * x + x + x + 1.0F;
    }
    return 0.0F;
}

void append_uint(std::string& out, std::uint64_t v) {
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), v);
    out.append(buffer, result.ptr);
}

void append_int(std::string& out, std::int64_t v) {
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), v);
    out.append(buffer, result.ptr);
}

void append_float(std::string& out, float v) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.6f", static_cast<double>(v));
    out.append(buffer);
}

[[nodiscard]] bool parse_uint(std::string_view token, std::uint64_t& out) {
    const auto* begin = token.data();
    const auto* end = token.data() + token.size();
    const auto result = std::from_chars(begin, end, out);
    return result.ec == std::errc{} && result.ptr == end;
}

[[nodiscard]] bool parse_int(std::string_view token, std::int64_t& out) {
    const auto* begin = token.data();
    const auto* end = token.data() + token.size();
    const auto result = std::from_chars(begin, end, out);
    return result.ec == std::errc{} && result.ptr == end;
}

[[nodiscard]] bool parse_float(std::string_view token, float& out) {
    std::string copy(token);
    char* endPtr = nullptr;
    const float value = std::strtof(copy.c_str(), &endPtr);
    if (endPtr == copy.c_str() || *endPtr != '\0') return false;
    out = value;
    return true;
}

void hash_bytes(std::uint64_t& state, std::string_view bytes) noexcept {
    for (const char c : bytes) {
        state ^= static_cast<std::uint8_t>(c);
        state *= 0x100000001b3ULL;
    }
}

std::vector<std::string_view> tokenize(std::string_view line) {
    std::vector<std::string_view> tokens;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        const std::size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
        if (i > start) tokens.push_back(line.substr(start, i - start));
    }
    return tokens;
}

bool parse_cell(std::string_view token, ChipCell& cell) {
    std::array<std::string_view, 5> parts{};
    std::size_t count = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= token.size() && count < 5; ++i) {
        if (i == token.size() || token[i] == ':') {
            parts[count++] = token.substr(start, i - start);
            start = i + 1;
        }
    }
    if (count != 5) return false;
    std::uint64_t note = 0, instrument = 0, volume = 0, effect = 0, param = 0;
    if (!parse_uint(parts[0], note) || !parse_uint(parts[1], instrument) ||
        !parse_uint(parts[2], volume) || !parse_uint(parts[3], effect) ||
        !parse_uint(parts[4], param)) {
        return false;
    }
    if (note > 255 || instrument > 255 || volume > 255 ||
        effect > static_cast<std::uint64_t>(ChipEffect::SetWave) || param > 255) {
        return false;
    }
    cell.note = static_cast<std::uint8_t>(note);
    cell.instrument = static_cast<std::uint8_t>(instrument);
    cell.volume = static_cast<std::uint8_t>(volume);
    cell.effect = static_cast<ChipEffect>(effect);
    cell.effectParam = static_cast<std::uint8_t>(param);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Note and name helpers
// ---------------------------------------------------------------------------------------------

int chip_note_to_midi(std::uint8_t note) noexcept {
    if (note == kChipNoteNone || note == kChipNoteOff) return -1;
    return 11 + static_cast<int>(note);
}

float midi_note_to_frequency(int midiNote) noexcept {
    return frequency_from_midi(static_cast<float>(midiNote));
}

std::string_view chip_wave_name(ChipWave wave) noexcept {
    switch (wave) {
        case ChipWave::Pulse: return "pulse";
        case ChipWave::Triangle: return "triangle";
        case ChipWave::Sawtooth: return "sawtooth";
        case ChipWave::Noise: return "noise";
        case ChipWave::Sine: return "sine";
        case ChipWave::Wavetable: return "wavetable";
    }
    return "pulse";
}

std::string_view chip_effect_name(ChipEffect effect) noexcept {
    switch (effect) {
        case ChipEffect::NoEffect: return "none";
        case ChipEffect::Arpeggio: return "arpeggio";
        case ChipEffect::PortamentoUp: return "porta_up";
        case ChipEffect::PortamentoDown: return "porta_down";
        case ChipEffect::TonePortamento: return "tone_porta";
        case ChipEffect::Vibrato: return "vibrato";
        case ChipEffect::VolumeSlide: return "vol_slide";
        case ChipEffect::SetSpeed: return "set_speed";
        case ChipEffect::PatternBreak: return "pattern_break";
        case ChipEffect::PositionJump: return "position_jump";
        case ChipEffect::SetPan: return "set_pan";
        case ChipEffect::SetDuty: return "set_duty";
        case ChipEffect::NoteCut: return "note_cut";
        case ChipEffect::Retrigger: return "retrigger";
        case ChipEffect::SetWave: return "set_wave";
    }
    return "none";
}

// ---------------------------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------------------------

bool ChipEnvelope::validate(std::string* error) const {
    if (values.size() > kChiptuneEnvelopeMax || releaseValues.size() > kChiptuneEnvelopeMax) {
        if (error) *error = "envelope exceeds capacity";
        return false;
    }
    if (loopIndex > values.size()) {
        if (error) *error = "envelope loop index out of range";
        return false;
    }
    for (const float v : values) {
        if (!(v >= 0.0F && v <= 1.0F)) { if (error) *error = "envelope value out of [0,1]"; return false; }
    }
    for (const float v : releaseValues) {
        if (!(v >= 0.0F && v <= 1.0F)) { if (error) *error = "release value out of [0,1]"; return false; }
    }
    return true;
}

bool ChipPitchEnvelope::validate(std::string* error) const {
    if (semitones.size() > kChiptuneEnvelopeMax) {
        if (error) *error = "pitch envelope exceeds capacity";
        return false;
    }
    if (loopIndex > semitones.size()) {
        if (error) *error = "pitch envelope loop index out of range";
        return false;
    }
    for (const float value : semitones) {
        if (!std::isfinite(value) || value < -48.0F || value > 48.0F) {
            if (error) *error = "pitch envelope value out of [-48,48]";
            return false;
        }
    }
    return true;
}

bool ChipArpeggio::validate(std::string* error) const {
    if (semitones.size() > kChiptuneArpeggioMax) {
        if (error) *error = "arpeggio exceeds capacity";
        return false;
    }
    if (loopIndex > semitones.size()) {
        if (error) *error = "arpeggio loop index out of range";
        return false;
    }
    return true;
}

bool ChipInstrument::validate(std::string* error) const {
    if (!(duty > 0.0F && duty < 1.0F)) { if (error) *error = "duty must be in (0,1)"; return false; }
    if (!std::isfinite(gain) || !(gain >= 0.0F && gain <= 4.0F)) { if (error) *error = "gain out of [0,4]"; return false; }
    if (!std::isfinite(pan) || pan < -1.0F || pan > 1.0F) { if (error) *error = "pan out of [-1,1]"; return false; }
    if (!std::isfinite(fineTuneCents) || fineTuneCents < -100.0F || fineTuneCents > 100.0F) { if (error) *error = "fine tune out of [-100,100] cents"; return false; }
    if (!std::isfinite(vibratoDepthSemitones) || vibratoDepthSemitones < 0.0F || vibratoDepthSemitones > 24.0F ||
        !std::isfinite(vibratoRateHz) || vibratoRateHz < 0.0F || vibratoRateHz > 100.0F) {
        if (error) *error = "vibrato out of range";
        return false;
    }
    if (!std::isfinite(lowPassHz) || lowPassHz < 0.0F || lowPassHz > 96000.0F ||
        !std::isfinite(highPassHz) || highPassHz < 0.0F || highPassHz > 96000.0F) {
        if (error) *error = "filter cutoff out of range";
        return false;
    }
    if (bitDepth == 0 || bitDepth > 16) { if (error) *error = "bit depth out of [1,16]"; return false; }
    if (sampleHold == 0 || sampleHold > 64) { if (error) *error = "sample hold out of [1,64]"; return false; }
    if (!volume.validate(error) || !pitch.validate(error) || !dutyEnvelope.validate(error) || !arpeggio.validate(error)) return false;
    if (wave == ChipWave::Wavetable && (wavetable.size() < 2 || wavetable.size() > kChiptuneWavetableMax)) {
        if (error) *error = "wavetable wave requires 2..64 samples";
        return false;
    }
    if (wavetable.size() > kChiptuneWavetableMax) { if (error) *error = "wavetable exceeds capacity"; return false; }
    for (const float value : wavetable) {
        if (!std::isfinite(value) || value < -1.0F || value > 1.0F) { if (error) *error = "wavetable value out of [-1,1]"; return false; }
    }
    return true;
}

const ChipCell& ChipPattern::at(std::uint32_t row, std::uint32_t channel,
                                std::uint32_t channelCount) const {
    return cells[static_cast<std::size_t>(row) * channelCount + channel];
}

ChipCell& ChipPattern::at(std::uint32_t row, std::uint32_t channel, std::uint32_t channelCount) {
    return cells[static_cast<std::size_t>(row) * channelCount + channel];
}

bool ChipSong::validate(std::string* error) const {
    if (channelCount == 0 || channelCount > kChiptuneMaxChannels) {
        if (error) *error = "channelCount out of range";
        return false;
    }
    if (sampleRate < 8000 || sampleRate > 192000) { if (error) *error = "sampleRate out of range"; return false; }
    if (ticksPerRow == 0 || ticksPerRow > 255) { if (error) *error = "ticksPerRow out of [1,255]"; return false; }
    if (ticksPerSecond < 20 || ticksPerSecond > 240) { if (error) *error = "ticksPerSecond out of range"; return false; }
    if (!std::isfinite(masterGain) || !(masterGain >= 0.0F && masterGain <= 4.0F)) { if (error) *error = "masterGain out of [0,4]"; return false; }
    if (instruments.empty()) { if (error) *error = "song has no instruments"; return false; }
    for (const auto& instrument : instruments) {
        if (!instrument.validate(error)) return false;
    }
    if (patterns.empty()) { if (error) *error = "song has no patterns"; return false; }
    for (const auto& pattern : patterns) {
        if (pattern.rowCount == 0 || pattern.rowCount > 256) { if (error) *error = "pattern rowCount out of range"; return false; }
        if (pattern.cells.size() != static_cast<std::size_t>(pattern.rowCount) * channelCount) {
            if (error) *error = "pattern cell count mismatch";
            return false;
        }
        for (const auto& cell : pattern.cells) {
            if (cell.instrument > instruments.size()) { if (error) *error = "cell instrument out of range"; return false; }
            if (cell.note != kChipNoteNone && cell.note != kChipNoteOff && chip_note_to_midi(cell.note) > 127) { if (error) *error = "cell note out of MIDI range"; return false; }
            if (cell.volume != kChipVolumeNone && cell.volume > 15) { if (error) *error = "cell volume out of range"; return false; }
            if (static_cast<std::uint8_t>(cell.effect) > static_cast<std::uint8_t>(ChipEffect::SetWave)) { if (error) *error = "cell effect out of range"; return false; }
            if (cell.effect == ChipEffect::SetWave && cell.effectParam > static_cast<std::uint8_t>(ChipWave::Wavetable)) { if (error) *error = "SetWave parameter out of range"; return false; }
        }
    }
    if (order.empty()) { if (error) *error = "song order is empty"; return false; }
    for (const std::uint32_t index : order) {
        if (index >= patterns.size()) { if (error) *error = "order references missing pattern"; return false; }
    }
    for (const auto& pattern : patterns) {
        for (const auto& cell : pattern.cells) {
            if (cell.effect == ChipEffect::PositionJump && cell.effectParam >= order.size()) {
                if (error) *error = "position jump references missing order";
                return false;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------------------------

std::string ChipSong::serialize() const {
    std::string out;
    out.reserve(4096);
    out += "dvechip 1\n";
    out += "name "; out += name; out += '\n';
    out += "sample_rate "; append_uint(out, sampleRate); out += '\n';
    out += "channels "; append_uint(out, channelCount); out += '\n';
    out += "ticks_per_row "; append_uint(out, ticksPerRow); out += '\n';
    out += "ticks_per_second "; append_uint(out, ticksPerSecond); out += '\n';
    out += "master_gain "; append_float(out, masterGain); out += '\n';
    out += "loop "; out += (loop ? "1" : "0"); out += '\n';

    out += "instruments "; append_uint(out, instruments.size()); out += '\n';
    for (const auto& instrument : instruments) {
        out += "instrument\n";
        out += "name "; out += instrument.name; out += '\n';
        out += "wave "; out += chip_wave_name(instrument.wave); out += '\n';
        out += "noise "; out += (instrument.noiseMode == ChipNoiseMode::Short ? "short" : "long"); out += '\n';
        out += "duty "; append_float(out, instrument.duty); out += '\n';
        out += "band_limited "; out += (instrument.bandLimited ? "1" : "0"); out += '\n';
        out += "gain "; append_float(out, instrument.gain); out += '\n';
        out += "pan "; append_float(out, instrument.pan); out += '\n';
        out += "transpose "; append_int(out, instrument.transposeSemitones); out += '\n';
        out += "fine_tune "; append_float(out, instrument.fineTuneCents); out += '\n';
        out += "pitch_slide "; append_int(out, instrument.pitchSlidePerTick); out += '\n';
        out += "vibrato "; append_float(out, instrument.vibratoDepthSemitones); out += ' ';
        append_float(out, instrument.vibratoRateHz); out += '\n';
        out += "filter "; append_float(out, instrument.lowPassHz); out += ' ';
        append_float(out, instrument.highPassHz); out += '\n';
        out += "crush "; append_uint(out, instrument.bitDepth); out += ' ';
        append_uint(out, instrument.sampleHold); out += '\n';
        out += "vol_loop "; append_uint(out, instrument.volume.loopIndex); out += '\n';
        out += "vol";
        for (const float value : instrument.volume.values) { out += ' '; append_float(out, value); }
        out += '\n';
        out += "vol_release";
        for (const float value : instrument.volume.releaseValues) { out += ' '; append_float(out, value); }
        out += '\n';
        out += "pitch_loop "; append_uint(out, instrument.pitch.loopIndex); out += '\n';
        out += "pitch";
        for (const float value : instrument.pitch.semitones) { out += ' '; append_float(out, value); }
        out += '\n';
        out += "duty_loop "; append_uint(out, instrument.dutyEnvelope.loopIndex); out += '\n';
        out += "duty_env";
        for (const float value : instrument.dutyEnvelope.values) { out += ' '; append_float(out, value); }
        out += '\n';
        out += "arp_loop "; append_uint(out, instrument.arpeggio.loopIndex); out += '\n';
        out += "arp";
        for (const std::int8_t semitone : instrument.arpeggio.semitones) { out += ' '; append_int(out, semitone); }
        out += '\n';
        out += "wavetable";
        for (const float value : instrument.wavetable) { out += ' '; append_float(out, value); }
        out += '\n';
    }

    out += "patterns "; append_uint(out, patterns.size()); out += '\n';
    for (const auto& pattern : patterns) {
        out += "pattern "; append_uint(out, pattern.rowCount); out += '\n';
        for (std::uint32_t row = 0; row < pattern.rowCount; ++row) {
            out += "row";
            for (std::uint32_t channel = 0; channel < channelCount; ++channel) {
                const ChipCell& cell = pattern.at(row, channel, channelCount);
                out += ' ';
                append_uint(out, cell.note); out += ':';
                append_uint(out, cell.instrument); out += ':';
                append_uint(out, cell.volume); out += ':';
                append_uint(out, static_cast<std::uint64_t>(cell.effect)); out += ':';
                append_uint(out, cell.effectParam);
            }
            out += '\n';
        }
    }

    out += "order";
    for (const std::uint32_t index : order) { out += ' '; append_uint(out, index); }
    out += '\n';
    return out;
}

bool ChipSong::parse(std::string_view text, ChipSong& out, std::string* error) {
    out = ChipSong{};
    out.instruments.clear();
    out.patterns.clear();
    out.order.clear();

    std::istringstream stream{std::string(text)};
    std::string rawLine;
    bool sawMagic = false;

    auto fail = [&](const char* message) {
        if (error) *error = message;
        return false;
    };

    while (std::getline(stream, rawLine)) {
        std::string_view line = rawLine;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) continue;
        auto tokens = tokenize(line);
        if (tokens.empty()) continue;
        const std::string_view key = tokens[0];

        if (!sawMagic) {
            if (key != "dvechip" || tokens.size() != 2 || tokens[1] != "1") return fail("missing or unsupported dvechip header");
            sawMagic = true;
            continue;
        }

        if (key == "name") {
            const std::size_t pos = line.find("name");
            std::string value = (line.size() > pos + 5) ? std::string(line.substr(pos + 5)) : std::string{};
            // A song-level name only appears before any instrument; every later "name" belongs to
            // the instrument currently being read (instruments precede patterns in the format).
            if (!out.instruments.empty() && out.patterns.empty()) {
                out.instruments.back().name = std::move(value);
            } else {
                out.name = std::move(value);
            }
        } else if (key == "sample_rate") {
            std::uint64_t v{}; if (tokens.size() < 2 || !parse_uint(tokens[1], v)) return fail("bad sample_rate");
            if (v > std::numeric_limits<std::uint32_t>::max()) return fail("sample_rate overflow");
            out.sampleRate = static_cast<std::uint32_t>(v);
        } else if (key == "channels") {
            std::uint64_t v{}; if (tokens.size() < 2 || !parse_uint(tokens[1], v)) return fail("bad channels");
            if (v > std::numeric_limits<std::uint32_t>::max()) return fail("channels overflow");
            out.channelCount = static_cast<std::uint32_t>(v);
        } else if (key == "ticks_per_row") {
            std::uint64_t v{}; if (tokens.size() < 2 || !parse_uint(tokens[1], v)) return fail("bad ticks_per_row");
            if (v > std::numeric_limits<std::uint32_t>::max()) return fail("ticks_per_row overflow");
            out.ticksPerRow = static_cast<std::uint32_t>(v);
        } else if (key == "ticks_per_second") {
            std::uint64_t v{}; if (tokens.size() < 2 || !parse_uint(tokens[1], v)) return fail("bad ticks_per_second");
            if (v > std::numeric_limits<std::uint32_t>::max()) return fail("ticks_per_second overflow");
            out.ticksPerSecond = static_cast<std::uint32_t>(v);
        } else if (key == "master_gain") {
            float v{}; if (tokens.size() < 2 || !parse_float(tokens[1], v)) return fail("bad master_gain");
            out.masterGain = v;
        } else if (key == "loop") {
            out.loop = tokens.size() > 1 && tokens[1] == "1";
        } else if (key == "instruments" || key == "patterns") {
            // advisory counts, entries follow
        } else if (key == "instrument") {
            out.instruments.emplace_back();
        } else if (key == "wave") {
            if (out.instruments.empty()) return fail("wave outside instrument");
            auto& inst = out.instruments.back();
            const std::string_view w = tokens.size() > 1 ? tokens[1] : std::string_view{};
            if (w == "pulse") inst.wave = ChipWave::Pulse;
            else if (w == "triangle") inst.wave = ChipWave::Triangle;
            else if (w == "sawtooth") inst.wave = ChipWave::Sawtooth;
            else if (w == "noise") inst.wave = ChipWave::Noise;
            else if (w == "sine") inst.wave = ChipWave::Sine;
            else if (w == "wavetable") inst.wave = ChipWave::Wavetable;
            else return fail("unknown wave");
        } else if (key == "noise") {
            if (out.instruments.empty()) return fail("noise outside instrument");
            out.instruments.back().noiseMode = (tokens.size() > 1 && tokens[1] == "short") ? ChipNoiseMode::Short : ChipNoiseMode::Long;
        } else if (key == "duty") {
            if (out.instruments.empty()) return fail("duty outside instrument");
            float v{}; if (tokens.size() < 2 || !parse_float(tokens[1], v)) return fail("bad duty");
            out.instruments.back().duty = v;
        } else if (key == "band_limited") {
            if (out.instruments.empty()) return fail("band_limited outside instrument");
            out.instruments.back().bandLimited = tokens.size() > 1 && tokens[1] == "1";
        } else if (key == "gain") {
            if (out.instruments.empty()) return fail("gain outside instrument");
            float v{}; if (tokens.size() < 2 || !parse_float(tokens[1], v)) return fail("bad gain");
            out.instruments.back().gain = v;
        } else if (key == "pan") {
            if (out.instruments.empty()) return fail("pan outside instrument");
            float v{}; if (tokens.size() < 2 || !parse_float(tokens[1], v)) return fail("bad pan");
            out.instruments.back().pan = v;
        } else if (key == "transpose") {
            if (out.instruments.empty()) return fail("transpose outside instrument");
            std::int64_t v{}; if (tokens.size() < 2 || !parse_int(tokens[1], v) || v < -128 || v > 127) return fail("bad transpose");
            out.instruments.back().transposeSemitones = static_cast<std::int8_t>(v);
        } else if (key == "fine_tune") {
            if (out.instruments.empty()) return fail("fine_tune outside instrument");
            float v{}; if (tokens.size() < 2 || !parse_float(tokens[1], v)) return fail("bad fine_tune");
            out.instruments.back().fineTuneCents = v;
        } else if (key == "pitch_slide") {
            if (out.instruments.empty()) return fail("pitch_slide outside instrument");
            std::int64_t v{}; if (tokens.size() < 2 || !parse_int(tokens[1], v)) return fail("bad pitch_slide");
            out.instruments.back().pitchSlidePerTick = static_cast<std::int8_t>(v);
        } else if (key == "vibrato") {
            if (out.instruments.empty()) return fail("vibrato outside instrument");
            float depth{}, rate{};
            if (tokens.size() < 3 || !parse_float(tokens[1], depth) || !parse_float(tokens[2], rate)) return fail("bad vibrato");
            out.instruments.back().vibratoDepthSemitones = depth;
            out.instruments.back().vibratoRateHz = rate;
        } else if (key == "filter") {
            if (out.instruments.empty()) return fail("filter outside instrument");
            float low{}, high{}; if (tokens.size() < 3 || !parse_float(tokens[1], low) || !parse_float(tokens[2], high)) return fail("bad filter");
            out.instruments.back().lowPassHz = low;
            out.instruments.back().highPassHz = high;
        } else if (key == "crush") {
            if (out.instruments.empty()) return fail("crush outside instrument");
            std::uint64_t bits{}, hold{}; if (tokens.size() < 3 || !parse_uint(tokens[1], bits) || !parse_uint(tokens[2], hold) || bits > 255 || hold > 255) return fail("bad crush");
            out.instruments.back().bitDepth = static_cast<std::uint8_t>(bits);
            out.instruments.back().sampleHold = static_cast<std::uint8_t>(hold);
        } else if (key == "vol_loop") {
            if (out.instruments.empty()) return fail("vol_loop outside instrument");
            std::uint64_t v{}; if (tokens.size() < 2 || !parse_uint(tokens[1], v)) return fail("bad vol_loop");
            out.instruments.back().volume.loopIndex = static_cast<std::size_t>(v);
        } else if (key == "vol") {
            if (out.instruments.empty()) return fail("vol outside instrument");
            auto& values = out.instruments.back().volume.values;
            values.clear();
            for (std::size_t i = 1; i < tokens.size(); ++i) { float v{}; if (!parse_float(tokens[i], v)) return fail("bad vol value"); values.push_back(v); }
        } else if (key == "vol_release") {
            if (out.instruments.empty()) return fail("vol_release outside instrument");
            auto& values = out.instruments.back().volume.releaseValues;
            values.clear();
            for (std::size_t i = 1; i < tokens.size(); ++i) { float v{}; if (!parse_float(tokens[i], v)) return fail("bad vol_release value"); values.push_back(v); }
        } else if (key == "pitch_loop") {
            if (out.instruments.empty()) return fail("pitch_loop outside instrument");
            std::uint64_t v{}; if (tokens.size() < 2 || !parse_uint(tokens[1], v)) return fail("bad pitch_loop");
            out.instruments.back().pitch.loopIndex = static_cast<std::size_t>(v);
        } else if (key == "pitch") {
            if (out.instruments.empty()) return fail("pitch outside instrument");
            auto& values = out.instruments.back().pitch.semitones;
            values.clear();
            for (std::size_t i = 1; i < tokens.size(); ++i) { float v{}; if (!parse_float(tokens[i], v)) return fail("bad pitch value"); values.push_back(v); }
        } else if (key == "duty_loop") {
            if (out.instruments.empty()) return fail("duty_loop outside instrument");
            std::uint64_t v{}; if (tokens.size() < 2 || !parse_uint(tokens[1], v)) return fail("bad duty_loop");
            out.instruments.back().dutyEnvelope.loopIndex = static_cast<std::size_t>(v);
        } else if (key == "duty_env") {
            if (out.instruments.empty()) return fail("duty_env outside instrument");
            auto& values = out.instruments.back().dutyEnvelope.values;
            values.clear();
            for (std::size_t i = 1; i < tokens.size(); ++i) { float v{}; if (!parse_float(tokens[i], v)) return fail("bad duty_env value"); values.push_back(v); }
        } else if (key == "arp_loop") {
            if (out.instruments.empty()) return fail("arp_loop outside instrument");
            std::uint64_t v{}; if (tokens.size() < 2 || !parse_uint(tokens[1], v)) return fail("bad arp_loop");
            out.instruments.back().arpeggio.loopIndex = static_cast<std::size_t>(v);
        } else if (key == "arp") {
            if (out.instruments.empty()) return fail("arp outside instrument");
            auto& values = out.instruments.back().arpeggio.semitones;
            values.clear();
            for (std::size_t i = 1; i < tokens.size(); ++i) { std::int64_t v{}; if (!parse_int(tokens[i], v)) return fail("bad arp value"); values.push_back(static_cast<std::int8_t>(v)); }
        } else if (key == "wavetable") {
            if (out.instruments.empty()) return fail("wavetable outside instrument");
            auto& values = out.instruments.back().wavetable;
            values.clear();
            for (std::size_t i = 1; i < tokens.size(); ++i) { float v{}; if (!parse_float(tokens[i], v)) return fail("bad wavetable value"); values.push_back(v); }
        } else if (key == "pattern") {
            std::uint64_t rows{}; if (tokens.size() < 2 || !parse_uint(tokens[1], rows)) return fail("bad pattern rows");
            ChipPattern pattern;
            pattern.rowCount = static_cast<std::uint32_t>(rows);
            pattern.cells.reserve(static_cast<std::size_t>(rows) * out.channelCount);
            out.patterns.push_back(std::move(pattern));
        } else if (key == "row") {
            if (out.patterns.empty()) return fail("row outside pattern");
            auto& pattern = out.patterns.back();
            if (tokens.size() - 1 != out.channelCount) return fail("row channel count mismatch");
            for (std::size_t i = 1; i < tokens.size(); ++i) {
                ChipCell cell;
                if (!parse_cell(tokens[i], cell)) return fail("bad cell");
                pattern.cells.push_back(cell);
            }
        } else if (key == "order") {
            for (std::size_t i = 1; i < tokens.size(); ++i) { std::uint64_t v{}; if (!parse_uint(tokens[i], v)) return fail("bad order index"); out.order.push_back(static_cast<std::uint32_t>(v)); }
        } else {
            return fail("unknown key");
        }
    }

    if (!sawMagic) return fail("empty or non-dvechip input");
    if (!out.validate(error)) return false;
    return true;
}

std::uint64_t ChipSong::content_hash() const {
    std::uint64_t state = 0xcbf29ce484222325ULL;
    hash_bytes(state, serialize());
    return state;
}

// ---------------------------------------------------------------------------------------------
// Voice
// ---------------------------------------------------------------------------------------------

ChiptuneVoice::ChiptuneVoice(std::uint32_t sampleRate) noexcept : sampleRate_(sampleRate) {
    update_filter_coefficients();
}

void ChiptuneVoice::set_sample_rate(std::uint32_t sampleRate) noexcept {
    sampleRate_ = sampleRate;
    update_filter_coefficients();
}

void ChiptuneVoice::set_instrument(const ChipInstrument& source) noexcept {
    RuntimeInstrument compiled{};
    compiled.wave = source.wave;
    compiled.noiseMode = source.noiseMode;
    compiled.duty = source.duty;
    compiled.bandLimited = source.bandLimited;
    compiled.gain = source.gain;
    compiled.pan = source.pan;
    compiled.transposeSemitones = source.transposeSemitones;
    compiled.fineTuneCents = source.fineTuneCents;
    compiled.pitchSlidePerTick = source.pitchSlidePerTick;
    compiled.vibratoDepthSemitones = source.vibratoDepthSemitones;
    compiled.vibratoRateHz = source.vibratoRateHz;
    compiled.lowPassHz = source.lowPassHz;
    compiled.highPassHz = source.highPassHz;
    compiled.bitDepth = source.bitDepth;
    compiled.sampleHold = source.sampleHold;

    const auto copy_float = [](const std::vector<float>& values, auto& destination, std::uint8_t& count) noexcept {
        const std::size_t size = std::min(values.size(), destination.size());
        for (std::size_t i = 0; i < size; ++i) destination[i] = values[i];
        count = static_cast<std::uint8_t>(size);
    };
    copy_float(source.volume.values, compiled.volume, compiled.volumeCount);
    copy_float(source.volume.releaseValues, compiled.release, compiled.releaseCount);
    copy_float(source.pitch.semitones, compiled.pitch, compiled.pitchCount);
    copy_float(source.dutyEnvelope.values, compiled.dutyEnvelope, compiled.dutyCount);
    copy_float(source.wavetable, compiled.wavetable, compiled.wavetableCount);
    const std::size_t arpCount = std::min(source.arpeggio.semitones.size(), compiled.arpeggio.size());
    for (std::size_t i = 0; i < arpCount; ++i) compiled.arpeggio[i] = source.arpeggio.semitones[i];
    compiled.arpeggioCount = static_cast<std::uint8_t>(arpCount);

    compiled.volumeLoop = static_cast<std::uint8_t>(std::min(source.volume.loopIndex, static_cast<std::size_t>(compiled.volumeCount)));
    compiled.pitchLoop = static_cast<std::uint8_t>(std::min(source.pitch.loopIndex, static_cast<std::size_t>(compiled.pitchCount)));
    compiled.dutyLoop = static_cast<std::uint8_t>(std::min(source.dutyEnvelope.loopIndex, static_cast<std::size_t>(compiled.dutyCount)));
    compiled.arpeggioLoop = static_cast<std::uint8_t>(std::min(source.arpeggio.loopIndex, static_cast<std::size_t>(compiled.arpeggioCount)));
    instrument_ = compiled;
    pan_ = std::clamp(compiled.pan, -1.0F, 1.0F);
    duty_ = std::clamp(compiled.duty, 0.01F, 0.99F);
    update_filter_coefficients();
}

void ChiptuneVoice::update_filter_coefficients() noexcept {
    const float sampleRate = static_cast<float>(std::max<std::uint32_t>(sampleRate_, 1U));
    const float nyquist = sampleRate * 0.5F;
    const float lowCutoff = std::clamp(instrument_.lowPassHz, 0.0F, nyquist);
    lowPassCoefficient_ = (lowCutoff <= 0.0F) ? 0.0F :
        ((lowCutoff >= nyquist * 0.999F) ? 1.0F : 1.0F - std::exp(-2.0F * kPi * lowCutoff / sampleRate));
    const float highCutoff = std::clamp(instrument_.highPassHz, 0.0F, nyquist);
    if (highCutoff <= 0.0F) {
        highPassCoefficient_ = 0.0F;
    } else {
        const float rc = 1.0F / (2.0F * kPi * highCutoff);
        const float dt = 1.0F / sampleRate;
        highPassCoefficient_ = rc / (rc + dt);
    }
}

void ChiptuneVoice::retrigger_lfsr() noexcept {
    lfsr_ = 1;
    noiseAccum_ = 0.0F;
    noiseValue_ = -1.0F;
}

void ChiptuneVoice::note_on(int midiNote, float velocity) noexcept {
    baseMidi_ = midiNote;
    glideMidi_ = static_cast<float>(midiNote + instrument_.transposeSemitones) + instrument_.fineTuneCents / 100.0F;
    portaTargetMidi_ = static_cast<int>(std::lround(glideMidi_));
    portaRate_ = 0.0F;
    pitchSlidePerTick_ = static_cast<float>(instrument_.pitchSlidePerTick) / 16.0F;
    velocity_ = std::clamp(velocity, 0.0F, 1.0F);
    columnVolume_ = 1.0F;
    arpCursor_ = volCursor_ = relCursor_ = pitchCursor_ = dutyCursor_ = 0;
    tickCounter_ = 0;
    arpOverlayA_ = arpOverlayB_ = 0;
    vibratoDepth_ = instrument_.vibratoDepthSemitones;
    vibratoRate_ = instrument_.vibratoRateHz;
    vibratoPhase_ = 0.0F;
    phase_ = 0.0F;
    envAmplitude_ = instrument_.volumeCount == 0 ? 1.0F : clamp01(instrument_.volume[0]);
    pitchEnvelopeSemitones_ = instrument_.pitchCount == 0 ? 0.0F : instrument_.pitch[0];
    duty_ = instrument_.dutyCount == 0 ? instrument_.duty : std::clamp(instrument_.dutyEnvelope[0], 0.01F, 0.99F);
    pan_ = std::clamp(instrument_.pan, -1.0F, 1.0F);
    lowPassState_ = highPassState_ = highPassPreviousInput_ = heldSample_ = 0.0F;
    holdCounter_ = 0;
    active_ = true;
    released_ = false;
    retrigger_lfsr();
}

void ChiptuneVoice::note_off() noexcept {
    if (!active_) return;
    released_ = true;
    relCursor_ = 0;
    if (instrument_.releaseCount == 0) active_ = false;
}

void ChiptuneVoice::hard_stop() noexcept {
    active_ = false;
    released_ = false;
}

void ChiptuneVoice::retrigger() noexcept {
    if (baseMidi_ < 0) return;
    const float slide = pitchSlidePerTick_;
    const int arpA = arpOverlayA_;
    const int arpB = arpOverlayB_;
    const float vibratoDepth = vibratoDepth_;
    const float vibratoRate = vibratoRate_;
    const float volume = columnVolume_;
    const float currentPan = pan_;
    const float currentDuty = duty_;
    note_on(baseMidi_, velocity_);
    pitchSlidePerTick_ = slide;
    arpOverlayA_ = arpA;
    arpOverlayB_ = arpB;
    vibratoDepth_ = vibratoDepth;
    vibratoRate_ = vibratoRate;
    columnVolume_ = volume;
    pan_ = currentPan;
    duty_ = currentDuty;
}

void ChiptuneVoice::set_tone_portamento(int targetMidi, float rate) noexcept {
    portaTargetMidi_ = targetMidi + instrument_.transposeSemitones;
    portaRate_ = std::max(0.0F, rate);
    active_ = true;
    released_ = false;
}

void ChiptuneVoice::set_vibrato(float depthSemitones, float rateHz) noexcept {
    vibratoDepth_ = std::max(0.0F, depthSemitones);
    vibratoRate_ = std::max(0.0F, rateHz);
}

void ChiptuneVoice::set_column_volume(float volume01) noexcept { columnVolume_ = clamp01(volume01); }
void ChiptuneVoice::slide_column_volume(float delta) noexcept { columnVolume_ = clamp01(columnVolume_ + delta); }
void ChiptuneVoice::set_arpeggio_overlay(int semitoneA, int semitoneB) noexcept { arpOverlayA_ = semitoneA; arpOverlayB_ = semitoneB; }
void ChiptuneVoice::set_pan(float pan) noexcept { pan_ = std::clamp(pan, -1.0F, 1.0F); }
void ChiptuneVoice::set_duty(float duty) noexcept { duty_ = std::clamp(duty, 0.01F, 0.99F); }
void ChiptuneVoice::set_wave(ChipWave wave) noexcept {
    if (wave == ChipWave::Wavetable && instrument_.wavetableCount < 2) return;
    instrument_.wave = wave;
}

namespace {

template <typename T, std::size_t N>
std::size_t advance_cursor(std::size_t cursor, std::uint8_t count, std::uint8_t loop,
                           const std::array<T, N>&) noexcept {
    if (count == 0) return 0;
    ++cursor;
    if (cursor >= count) cursor = (loop < count) ? loop : static_cast<std::size_t>(count - 1);
    return cursor;
}

} // namespace

void ChiptuneVoice::tick() noexcept {
    if (!active_) return;

    if (released_ && instrument_.releaseCount > 0) {
        if (relCursor_ < instrument_.releaseCount) {
            envAmplitude_ = clamp01(instrument_.release[relCursor_++]);
        } else {
            active_ = false;
            return;
        }
    } else if (instrument_.volumeCount > 0) {
        envAmplitude_ = clamp01(instrument_.volume[std::min(volCursor_, static_cast<std::size_t>(instrument_.volumeCount - 1))]);
        volCursor_ = advance_cursor(volCursor_, instrument_.volumeCount, instrument_.volumeLoop, instrument_.volume);
    }

    if (instrument_.pitchCount > 0) {
        pitchEnvelopeSemitones_ = instrument_.pitch[std::min(pitchCursor_, static_cast<std::size_t>(instrument_.pitchCount - 1))];
        pitchCursor_ = advance_cursor(pitchCursor_, instrument_.pitchCount, instrument_.pitchLoop, instrument_.pitch);
    }
    if (instrument_.dutyCount > 0) {
        duty_ = std::clamp(instrument_.dutyEnvelope[std::min(dutyCursor_, static_cast<std::size_t>(instrument_.dutyCount - 1))], 0.01F, 0.99F);
        dutyCursor_ = advance_cursor(dutyCursor_, instrument_.dutyCount, instrument_.dutyLoop, instrument_.dutyEnvelope);
    }
    if (instrument_.arpeggioCount > 0 && tickCounter_ > 0) {
        arpCursor_ = advance_cursor(arpCursor_, instrument_.arpeggioCount, instrument_.arpeggioLoop, instrument_.arpeggio);
    }

    if (portaRate_ > 0.0F && portaTargetMidi_ >= 0) {
        const float target = static_cast<float>(portaTargetMidi_) + instrument_.fineTuneCents / 100.0F;
        if (glideMidi_ < target) glideMidi_ = std::min(target, glideMidi_ + portaRate_);
        else if (glideMidi_ > target) glideMidi_ = std::max(target, glideMidi_ - portaRate_);
    }
    glideMidi_ = std::clamp(glideMidi_ + pitchSlidePerTick_, 0.0F, 127.0F);
    ++tickCounter_;
}

float ChiptuneVoice::current_frequency() const noexcept {
    float midi = glideMidi_ + pitchEnvelopeSemitones_;
    if (instrument_.arpeggioCount > 0) {
        const std::size_t cursor = std::min(arpCursor_, static_cast<std::size_t>(instrument_.arpeggioCount - 1));
        midi += static_cast<float>(instrument_.arpeggio[cursor]);
    }
    if (arpOverlayA_ != 0 || arpOverlayB_ != 0) {
        const std::uint32_t phase = tickCounter_ == 0 ? 0U : (tickCounter_ - 1U) % 3U;
        if (phase == 1U) midi += static_cast<float>(arpOverlayA_);
        else if (phase == 2U) midi += static_cast<float>(arpOverlayB_);
    }
    if (vibratoDepth_ > 0.0F) midi += vibratoDepth_ * std::sin(2.0F * kPi * vibratoPhase_);
    return frequency_from_midi(std::clamp(midi, 0.0F, 127.0F));
}

float ChiptuneVoice::oscillator_sample(float phase, float phaseIncrement) noexcept {
    switch (instrument_.wave) {
        case ChipWave::Pulse: {
            float value = phase < duty_ ? 1.0F : -1.0F;
            if (instrument_.bandLimited) {
                value += poly_blep(phase, phaseIncrement);
                float shifted = phase - duty_;
                if (shifted < 0.0F) shifted += 1.0F;
                value -= poly_blep(shifted, phaseIncrement);
            }
            return value;
        }
        case ChipWave::Sawtooth: {
            float value = 2.0F * phase - 1.0F;
            if (instrument_.bandLimited) value -= poly_blep(phase, phaseIncrement);
            return value;
        }
        case ChipWave::Triangle: return 4.0F * std::fabs(phase - 0.5F) - 1.0F;
        case ChipWave::Sine: return std::sin(2.0F * kPi * phase);
        case ChipWave::Wavetable: {
            if (instrument_.wavetableCount < 2) return 0.0F;
            const float position = phase * static_cast<float>(instrument_.wavetableCount);
            const std::size_t first = static_cast<std::size_t>(position) % instrument_.wavetableCount;
            const std::size_t second = (first + 1U) % instrument_.wavetableCount;
            const float fraction = position - std::floor(position);
            return instrument_.wavetable[first] + (instrument_.wavetable[second] - instrument_.wavetable[first]) * fraction;
        }
        case ChipWave::Noise: {
            noiseAccum_ += phaseIncrement;
            while (noiseAccum_ >= 1.0F) {
                noiseAccum_ -= 1.0F;
                const std::uint16_t feedbackBit = instrument_.noiseMode == ChipNoiseMode::Short
                    ? static_cast<std::uint16_t>((lfsr_ ^ (lfsr_ >> 6)) & 1U)
                    : static_cast<std::uint16_t>((lfsr_ ^ (lfsr_ >> 1)) & 1U);
                lfsr_ = static_cast<std::uint16_t>((lfsr_ >> 1) | (feedbackBit << 14));
                noiseValue_ = (lfsr_ & 1U) ? -1.0F : 1.0F;
            }
            return noiseValue_;
        }
    }
    return 0.0F;
}

float ChiptuneVoice::apply_tone_shaping(float sample) noexcept {
    lowPassState_ += lowPassCoefficient_ * (sample - lowPassState_);
    float shaped = lowPassCoefficient_ >= 1.0F ? sample : lowPassState_;
    if (highPassCoefficient_ > 0.0F) {
        highPassState_ = highPassCoefficient_ * (highPassState_ + shaped - highPassPreviousInput_);
        highPassPreviousInput_ = shaped;
        shaped = highPassState_;
    }

    if (holdCounter_ == 0) {
        const int bits = std::clamp<int>(instrument_.bitDepth, 1, 16);
        if (bits < 16) {
            const float levels = static_cast<float>((1U << (bits - 1)) - 1U);
            heldSample_ = levels > 0.0F ? std::round(std::clamp(shaped, -1.0F, 1.0F) * levels) / levels
                                        : (shaped >= 0.0F ? 1.0F : -1.0F);
        } else {
            heldSample_ = shaped;
        }
        holdCounter_ = static_cast<std::uint8_t>(std::max<int>(1, instrument_.sampleHold));
    }
    --holdCounter_;
    return heldSample_;
}

float ChiptuneVoice::next_sample() noexcept {
    if (!active_) return 0.0F;
    const float sampleRate = static_cast<float>(std::max<std::uint32_t>(sampleRate_, 1U));
    const float frequency = current_frequency();
    const float phaseIncrement = std::min(frequency / sampleRate, 0.49F);
    float value = oscillator_sample(phase_, phaseIncrement);
    phase_ += phaseIncrement;
    while (phase_ >= 1.0F) phase_ -= 1.0F;
    if (vibratoRate_ > 0.0F) {
        vibratoPhase_ += vibratoRate_ / sampleRate;
        while (vibratoPhase_ >= 1.0F) vibratoPhase_ -= 1.0F;
    }
    value *= envAmplitude_ * velocity_ * columnVolume_ * instrument_.gain;
    return apply_tone_shaping(value);
}

// ---------------------------------------------------------------------------------------------
// Player
// ---------------------------------------------------------------------------------------------

bool ChiptunePlayer::load(const ChipSong& song, std::string* error) {
    if (!song.validate(error)) {
        loaded_ = false;
        voices_.clear();
        channelEffects_.clear();
        return false;
    }
    song_ = song;
    voices_.clear();
    voices_.reserve(song_.channelCount);
    for (std::uint32_t i = 0; i < song_.channelCount; ++i) voices_.emplace_back(song_.sampleRate);
    channelEffects_.assign(song_.channelCount, ChannelEffectState{});
    ticksPerRow_ = song_.ticksPerRow;
    loaded_ = true;
    restart();
    return true;
}

void ChiptunePlayer::restart() noexcept {
    orderIndex_ = 0;
    row_ = 0;
    tickInRow_ = 0;
    tickAccumulator_ = 0;
    ticksPerRow_ = song_.ticksPerRow;
    finished_ = false;
    playing_ = loaded_;
    firstTickPending_ = true;
    pendingBreak_ = pendingJump_ = false;
    for (auto& effect : channelEffects_) effect = ChannelEffectState{};
    for (auto& voice : voices_) voice.hard_stop();
}

void ChiptunePlayer::process_row() noexcept {
    if (orderIndex_ >= song_.order.size()) return;
    const std::uint32_t patternIndex = song_.order[orderIndex_];
    if (patternIndex >= song_.patterns.size()) return;
    const ChipPattern& pattern = song_.patterns[patternIndex];
    if (row_ >= pattern.rowCount) return;

    for (std::uint32_t channel = 0; channel < song_.channelCount; ++channel) {
        ChiptuneVoice& voice = voices_[channel];
        ChannelEffectState& state = channelEffects_[channel];
        const ChipCell& cell = pattern.at(row_, channel, song_.channelCount);
        state = ChannelEffectState{};
        voice.set_arpeggio_overlay(0, 0);
        voice.set_pitch_slide(0.0F);

        const bool tonePortamento = cell.effect == ChipEffect::TonePortamento;
        if (cell.note == kChipNoteOff) {
            voice.note_off();
        } else if (cell.note != kChipNoteNone) {
            if (cell.instrument >= 1 && cell.instrument <= song_.instruments.size()) {
                voice.set_instrument(song_.instruments[cell.instrument - 1]);
            }
            const int midi = chip_note_to_midi(cell.note);
            if (tonePortamento && voice.active()) {
                const float rate = cell.effectParam > 0 ? static_cast<float>(cell.effectParam) / 16.0F : 1.0F;
                voice.set_tone_portamento(midi, rate);
            } else {
                voice.note_on(midi);
            }
        } else if (cell.instrument >= 1 && cell.instrument <= song_.instruments.size()) {
            voice.set_instrument(song_.instruments[cell.instrument - 1]);
        }

        if (cell.volume != kChipVolumeNone) voice.set_column_volume(static_cast<float>(cell.volume) / 15.0F);

        switch (cell.effect) {
            case ChipEffect::Arpeggio:
                voice.set_arpeggio_overlay((cell.effectParam >> 4) & 0x0F, cell.effectParam & 0x0F);
                break;
            case ChipEffect::PortamentoUp:
                voice.set_pitch_slide(static_cast<float>(cell.effectParam) / 16.0F);
                break;
            case ChipEffect::PortamentoDown:
                voice.set_pitch_slide(-static_cast<float>(cell.effectParam) / 16.0F);
                break;
            case ChipEffect::Vibrato:
                voice.set_vibrato(static_cast<float>(cell.effectParam & 0x0F) * 0.25F,
                                  static_cast<float>((cell.effectParam >> 4) & 0x0F) + 1.0F);
                break;
            case ChipEffect::VolumeSlide: {
                const float up = static_cast<float>((cell.effectParam >> 4) & 0x0F);
                const float down = static_cast<float>(cell.effectParam & 0x0F);
                state.volumeSlidePerTick = (up - down) / 15.0F;
                break;
            }
            case ChipEffect::SetSpeed:
                if (cell.effectParam > 0) ticksPerRow_ = cell.effectParam;
                break;
            case ChipEffect::PatternBreak:
                pendingBreak_ = true;
                pendingBreakRow_ = cell.effectParam;
                break;
            case ChipEffect::PositionJump:
                pendingJump_ = true;
                pendingJumpOrder_ = cell.effectParam;
                break;
            case ChipEffect::SetPan:
                voice.set_pan(static_cast<float>(cell.effectParam) / 127.5F - 1.0F);
                break;
            case ChipEffect::SetDuty:
                voice.set_duty(std::clamp(static_cast<float>(cell.effectParam) / 255.0F, 0.01F, 0.99F));
                break;
            case ChipEffect::NoteCut:
                state.noteCutTick = cell.effectParam;
                break;
            case ChipEffect::Retrigger:
                state.retriggerTicks = cell.effectParam;
                break;
            case ChipEffect::SetWave:
                voice.set_wave(static_cast<ChipWave>(cell.effectParam));
                break;
            case ChipEffect::TonePortamento:
            case ChipEffect::NoEffect:
                break;
        }
    }
}

void ChiptunePlayer::apply_tick_effects() noexcept {
    for (std::size_t channel = 0; channel < voices_.size(); ++channel) {
        auto& voice = voices_[channel];
        const auto& state = channelEffects_[channel];
        if (tickInRow_ > 0 && state.volumeSlidePerTick != 0.0F) voice.slide_column_volume(state.volumeSlidePerTick);
        if (state.noteCutTick != 0xFFU && tickInRow_ == state.noteCutTick) voice.hard_stop();
        if (state.retriggerTicks > 0 && tickInRow_ > 0 && tickInRow_ % state.retriggerTicks == 0) voice.retrigger();
    }
}

void ChiptunePlayer::begin_tick() noexcept {
    if (tickInRow_ == 0) process_row();
    apply_tick_effects();
    for (auto& voice : voices_) voice.tick();
}

void ChiptunePlayer::finish_tick() noexcept {
    ++tickInRow_;
    if (tickInRow_ < ticksPerRow_) return;
    tickInRow_ = 0;

    const std::uint32_t patternIndex = song_.order[orderIndex_];
    const std::uint32_t rowCount = song_.patterns[patternIndex].rowCount;
    std::uint32_t nextOrder = orderIndex_;
    std::uint32_t nextRow = row_ + 1;
    if (pendingJump_) {
        nextOrder = pendingJumpOrder_;
        nextRow = 0;
    } else if (pendingBreak_) {
        nextOrder = orderIndex_ + 1;
        nextRow = pendingBreakRow_;
    } else if (nextRow >= rowCount) {
        nextOrder = orderIndex_ + 1;
        nextRow = 0;
    }
    pendingBreak_ = pendingJump_ = false;

    if (nextOrder >= song_.order.size()) {
        if (song_.loop) {
            nextOrder = 0;
        } else {
            finished_ = true;
            playing_ = false;
            return;
        }
    }
    const std::uint32_t nextPattern = song_.order[nextOrder];
    if (nextRow >= song_.patterns[nextPattern].rowCount) nextRow = 0;
    orderIndex_ = nextOrder;
    row_ = nextRow;
}

void ChiptunePlayer::render(std::span<float> interleavedStereo) noexcept {
    render(interleavedStereo.data(), interleavedStereo.size() / 2);
}

void ChiptunePlayer::render(float* interleavedStereo, std::size_t frameCount) noexcept {
    if (interleavedStereo == nullptr) return;
    if (!loaded_ || !playing_ || finished_) {
        std::fill_n(interleavedStereo, frameCount * 2, 0.0F);
        return;
    }

    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        if (firstTickPending_) {
            firstTickPending_ = false;
            begin_tick();
        }

        float left = 0.0F;
        float right = 0.0F;
        for (auto& voice : voices_) {
            const float sample = voice.next_sample();
            const float pan = voice.pan();
            const float leftGain = pan <= 0.0F ? 1.0F : 1.0F - pan;
            const float rightGain = pan >= 0.0F ? 1.0F : 1.0F + pan;
            left += sample * leftGain;
            right += sample * rightGain;
        }
        interleavedStereo[frame * 2] = std::clamp(left * song_.masterGain, -1.0F, 1.0F);
        interleavedStereo[frame * 2 + 1] = std::clamp(right * song_.masterGain, -1.0F, 1.0F);

        tickAccumulator_ += song_.ticksPerSecond;
        while (tickAccumulator_ >= song_.sampleRate && playing_) {
            tickAccumulator_ -= song_.sampleRate;
            finish_tick();
            if (playing_) begin_tick();
        }
        if (!playing_) {
            for (std::size_t remaining = frame + 1; remaining < frameCount; ++remaining) {
                interleavedStereo[remaining * 2] = 0.0F;
                interleavedStereo[remaining * 2 + 1] = 0.0F;
            }
            return;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Offline bake and demo song
// ---------------------------------------------------------------------------------------------

std::vector<float> render_chiptune_song(const ChipSong& song, float maxSeconds, std::string* error) {
    ChipSong offline = song;
    offline.loop = false;
    ChiptunePlayer player;
    if (!player.load(offline, error)) return {};

    const std::uint32_t sr = offline.sampleRate;
    const std::size_t maxFrames = static_cast<std::size_t>(std::max(0.0F, maxSeconds) * static_cast<float>(sr));
    std::vector<float> pcm;
    pcm.reserve(maxFrames * 2);

    const std::size_t block = 1024;
    std::vector<float> scratch(block * 2, 0.0F);
    std::size_t produced = 0;
    while (produced < maxFrames && player.playing() && !player.finished()) {
        const std::size_t frames = std::min(block, maxFrames - produced);
        player.render(scratch.data(), frames);
        pcm.insert(pcm.end(), scratch.begin(), scratch.begin() + frames * 2);
        produced += frames;
    }
    return pcm;
}

DecodedAudioAsset render_chiptune_audio_asset(const ChipSong& song, float maxSeconds,
                                               std::string* error) {
    DecodedAudioAsset asset;
    asset.metadata.name = song.name.empty() ? "chiptune" : song.name;
    asset.metadata.sampleRate = song.sampleRate;
    asset.metadata.channels = 2;
    asset.metadata.storagePolicy = AudioStoragePolicy::Resident;
    asset.samples = render_chiptune_song(song, maxSeconds, error);
    if (asset.samples.empty()) return asset;

    asset.metadata.frameCount = asset.samples.size() / 2;
    asset.metadata.durationSeconds = static_cast<double>(asset.metadata.frameCount) /
                                     static_cast<double>(asset.metadata.sampleRate);
    double squareSum = 0.0;
    float peak = 0.0F;
    for (const float sample : asset.samples) {
        peak = std::max(peak, std::fabs(sample));
        squareSum += static_cast<double>(sample) * static_cast<double>(sample);
    }
    asset.metadata.peakLinear = peak;
    asset.metadata.rmsLinear = asset.samples.empty() ? 0.0F
        : static_cast<float>(std::sqrt(squareSum / static_cast<double>(asset.samples.size())));
    asset.metadata.approximateLoudnessDbfs = asset.metadata.rmsLinear > 0.0F
        ? 20.0F * std::log10(asset.metadata.rmsLinear) : -120.0F;
    asset.metadata.contentHash = audio_content_hash(asset.samples, asset.metadata);
    return asset;
}

namespace {

ChipCell note_cell(std::uint8_t note, std::uint8_t instrument, std::uint8_t volume = kChipVolumeNone,
                   ChipEffect effect = ChipEffect::NoEffect, std::uint8_t param = 0) {
    ChipCell cell;
    cell.note = note;
    cell.instrument = instrument;
    cell.volume = volume;
    cell.effect = effect;
    cell.effectParam = param;
    return cell;
}

} // namespace

ChipSong make_chiptune_sfx_song(const ChipSfxRequest& request) {
    ChipSong song;
    song.name = "chip_sfx";
    song.sampleRate = std::clamp<std::uint32_t>(request.sampleRate, 8000U, 192000U);
    song.channelCount = 1;
    song.ticksPerRow = 1;
    song.ticksPerSecond = 120;
    song.masterGain = std::clamp(request.gain, 0.0F, 1.5F);
    song.loop = false;

    ChipInstrument instrument;
    instrument.name = "sfx";
    instrument.pan = std::clamp(request.pan, -1.0F, 1.0F);
    instrument.gain = 1.0F;
    instrument.volume.loopIndex = 0;
    instrument.pitch.loopIndex = 0;

    switch (request.preset) {
        case ChipSfxPreset::Coin:
            song.name = "coin";
            instrument.wave = ChipWave::Pulse;
            instrument.duty = 0.25F;
            instrument.volume.values = {1.0F, 0.95F, 0.85F, 0.72F, 0.58F, 0.42F, 0.28F, 0.16F, 0.08F, 0.0F};
            instrument.pitch.semitones = {0.0F, 0.0F, 7.0F, 7.0F, 12.0F, 12.0F, 19.0F, 19.0F};
            instrument.bitDepth = 10;
            break;
        case ChipSfxPreset::Jump:
            song.name = "jump";
            instrument.wave = ChipWave::Pulse;
            instrument.duty = 0.5F;
            instrument.volume.values = {0.9F, 1.0F, 0.95F, 0.85F, 0.72F, 0.58F, 0.4F, 0.25F, 0.1F, 0.0F};
            instrument.pitch.semitones = {-7.0F, -5.0F, -3.0F, 0.0F, 3.0F, 5.0F, 7.0F, 9.0F, 10.0F};
            instrument.dutyEnvelope.values = {0.20F, 0.25F, 0.35F, 0.45F, 0.50F};
            instrument.dutyEnvelope.loopIndex = 4;
            break;
        case ChipSfxPreset::Laser:
            song.name = "laser";
            instrument.wave = ChipWave::Sawtooth;
            instrument.volume.values = {1.0F, 0.95F, 0.88F, 0.78F, 0.65F, 0.50F, 0.35F, 0.20F, 0.08F, 0.0F};
            instrument.pitch.semitones = {18.0F, 15.0F, 12.0F, 9.0F, 6.0F, 3.0F, 0.0F, -4.0F, -8.0F, -12.0F};
            instrument.lowPassHz = 12000.0F;
            instrument.bitDepth = 9;
            break;
        case ChipSfxPreset::Explosion:
            song.name = "explosion";
            instrument.wave = ChipWave::Noise;
            instrument.noiseMode = ChipNoiseMode::Long;
            instrument.volume.values = {1.0F, 1.0F, 0.92F, 0.82F, 0.70F, 0.58F, 0.46F, 0.35F, 0.25F, 0.17F, 0.10F, 0.05F, 0.0F};
            instrument.pitch.semitones = {12.0F, 8.0F, 4.0F, 0.0F, -4.0F, -8.0F, -12.0F, -16.0F};
            instrument.lowPassHz = 5500.0F;
            instrument.highPassHz = 80.0F;
            instrument.bitDepth = 7;
            instrument.sampleHold = 2;
            break;
        case ChipSfxPreset::Hit:
            song.name = "hit";
            instrument.wave = ChipWave::Noise;
            instrument.noiseMode = ChipNoiseMode::Short;
            instrument.volume.values = {1.0F, 0.72F, 0.45F, 0.22F, 0.08F, 0.0F};
            instrument.pitch.semitones = {5.0F, 0.0F, -5.0F, -10.0F};
            instrument.lowPassHz = 8500.0F;
            instrument.bitDepth = 8;
            break;
        case ChipSfxPreset::PowerUp:
            song.name = "power_up";
            instrument.wave = ChipWave::Wavetable;
            instrument.wavetable = {0.0F, 0.8F, 1.0F, 0.4F, 0.0F, -0.4F, -1.0F, -0.8F};
            instrument.volume.values = {0.5F, 0.7F, 0.85F, 1.0F, 0.95F, 0.85F, 0.72F, 0.58F, 0.42F, 0.28F, 0.14F, 0.0F};
            instrument.pitch.semitones = {-12.0F, -7.0F, -5.0F, 0.0F, 4.0F, 7.0F, 12.0F, 16.0F, 19.0F, 24.0F};
            instrument.arpeggio.semitones = {0, 4, 7};
            instrument.arpeggio.loopIndex = 0;
            break;
        case ChipSfxPreset::UiConfirm:
            song.name = "ui_confirm";
            instrument.wave = ChipWave::Sine;
            instrument.volume.values = {0.85F, 1.0F, 0.82F, 0.55F, 0.28F, 0.08F, 0.0F};
            instrument.pitch.semitones = {0.0F, 4.0F, 7.0F, 12.0F};
            break;
        case ChipSfxPreset::UiCancel:
            song.name = "ui_cancel";
            instrument.wave = ChipWave::Triangle;
            instrument.volume.values = {0.9F, 0.85F, 0.72F, 0.55F, 0.35F, 0.16F, 0.0F};
            instrument.pitch.semitones = {5.0F, 2.0F, 0.0F, -3.0F, -7.0F};
            break;
    }

    song.instruments = {instrument};
    const float duration = std::clamp(request.durationSeconds, 0.05F, 2.0F);
    const std::uint32_t rows = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(std::ceil(duration * static_cast<float>(song.ticksPerSecond))), 1U, 240U);
    ChipPattern pattern;
    pattern.rowCount = rows;
    pattern.cells.resize(rows);
    const int midi = std::clamp(request.baseMidi, 12, 127);
    pattern.cells[0] = note_cell(static_cast<std::uint8_t>(midi - 11), 1, 15);
    if (rows > 1) pattern.cells[rows - 1].note = kChipNoteOff;
    song.patterns = {std::move(pattern)};
    song.order = {0};
    return song;
}

DecodedAudioAsset render_chiptune_sfx(const ChipSfxRequest& request, std::string* error) {
    const ChipSong song = make_chiptune_sfx_song(request);
    return render_chiptune_audio_asset(song, std::clamp(request.durationSeconds, 0.05F, 2.0F) + 0.1F, error);
}

ChipSong make_demo_chiptune_song() {
    ChipSong song;
    song.name = "dve_demo_chiptune";
    song.sampleRate = kChiptuneDefaultSampleRate;
    song.channelCount = 4;
    song.ticksPerRow = 6;
    song.ticksPerSecond = 60;
    song.masterGain = 0.38F;
    song.loop = true;

    // Instrument 1: lead pulse (25% duty) with a short attack and sustain, gentle vibrato.
    ChipInstrument lead;
    lead.name = "lead";
    lead.wave = ChipWave::Pulse;
    lead.duty = 0.25F;
    lead.gain = 0.9F;
    lead.pan = -0.35F;
    lead.dutyEnvelope.values = {0.18F, 0.22F, 0.25F, 0.30F, 0.25F};
    lead.dutyEnvelope.loopIndex = 2;
    lead.volume.values = {1.0F, 0.95F, 0.9F, 0.85F, 0.8F};
    lead.volume.loopIndex = 4;
    lead.volume.releaseValues = {0.5F, 0.25F, 0.1F, 0.0F};
    lead.vibratoDepthSemitones = 0.25F;
    lead.vibratoRateHz = 6.0F;

    // Instrument 2: harmony pulse (50% duty).
    ChipInstrument harmony;
    harmony.name = "harmony";
    harmony.wave = ChipWave::Pulse;
    harmony.duty = 0.5F;
    harmony.gain = 0.6F;
    harmony.pan = 0.35F;
    harmony.fineTuneCents = -4.0F;
    harmony.volume.values = {0.7F, 0.65F, 0.6F};
    harmony.volume.loopIndex = 2;
    harmony.volume.releaseValues = {0.3F, 0.0F};

    // Instrument 3: triangle bass.
    ChipInstrument bass;
    bass.name = "bass";
    bass.wave = ChipWave::Triangle;
    bass.gain = 1.0F;
    bass.lowPassHz = 7000.0F;
    bass.volume.values = {1.0F};
    bass.volume.loopIndex = 0;
    bass.volume.releaseValues = {0.6F, 0.3F, 0.0F};

    // Instrument 4: noise percussion (long mode), fast decay.
    ChipInstrument perc;
    perc.name = "perc";
    perc.wave = ChipWave::Noise;
    perc.noiseMode = ChipNoiseMode::Long;
    perc.gain = 0.8F;
    perc.bitDepth = 8;
    perc.sampleHold = 2;
    perc.lowPassHz = 9000.0F;
    perc.volume.values = {1.0F, 0.6F, 0.3F, 0.15F, 0.0F};
    perc.volume.loopIndex = 4;

    song.instruments = {lead, harmony, bass, perc};

    // One 16-row pattern. Notes use tracker byte where 49 == C-4 (MIDI 60).
    // C-4=49, D-4=51, E-4=53, F-4=54, G-4=56, A-4=58, B-4=60, C-5=61.
    ChipPattern pattern;
    pattern.rowCount = 16;
    pattern.cells.resize(static_cast<std::size_t>(pattern.rowCount) * song.channelCount);

    auto set = [&](std::uint32_t row, std::uint32_t channel, ChipCell cell) {
        pattern.at(row, channel, song.channelCount) = cell;
    };

    const std::uint8_t none = kChipNoteNone;
    const std::uint8_t off = kChipNoteOff;

    // Channel 0: lead melody.
    const std::uint8_t lead_notes[16] = {49, none, 53, none, 56, none, 61, off,
                                         58, none, 56, none, 53, none, 49, off};
    // Channel 1: harmony a third below-ish.
    const std::uint8_t harm_notes[16] = {45, none, 49, none, 52, none, 56, off,
                                         54, none, 52, none, 49, none, 45, off};
    // Channel 2: bass on downbeats (C-2=25, G-2=32).
    const std::uint8_t bass_notes[16] = {25, none, none, none, 32, none, none, none,
                                         25, none, none, none, 32, none, none, none};
    // Channel 3: noise percussion pattern.
    const std::uint8_t perc_notes[16] = {40, none, 40, none, 40, none, 40, 40,
                                         40, none, 40, none, 40, none, 40, 40};

    for (std::uint32_t r = 0; r < 16; ++r) {
        set(r, 0, lead_notes[r] == none ? note_cell(none, 0) : note_cell(lead_notes[r], lead_notes[r] == off ? 0 : 1));
        set(r, 1, harm_notes[r] == none ? note_cell(none, 0) : note_cell(harm_notes[r], harm_notes[r] == off ? 0 : 2));
        set(r, 2, bass_notes[r] == none ? note_cell(none, 0) : note_cell(bass_notes[r], 3));
        set(r, 3, perc_notes[r] == none ? note_cell(none, 0) : note_cell(perc_notes[r], 4));
    }

    song.patterns = {pattern};
    song.order = {0};
    return song;
}

} // namespace dve::audio
