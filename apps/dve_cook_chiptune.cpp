// dve_cook_chiptune: validate/render .dvechip songs, generate built-in retro SFX presets, and
// export either editable WAV or the engine's cooked .dvesample format.

#include "dve/audio/chiptune.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

using dve::audio::ChipSfxPreset;

struct PresetName {
    std::string_view name;
    ChipSfxPreset preset;
};

constexpr std::array<PresetName, 8> kPresets{{
    {"coin", ChipSfxPreset::Coin},
    {"jump", ChipSfxPreset::Jump},
    {"laser", ChipSfxPreset::Laser},
    {"explosion", ChipSfxPreset::Explosion},
    {"hit", ChipSfxPreset::Hit},
    {"power_up", ChipSfxPreset::PowerUp},
    {"ui_confirm", ChipSfxPreset::UiConfirm},
    {"ui_cancel", ChipSfxPreset::UiCancel},
}};

void print_usage() {
    std::cerr
        << "usage: dve_cook_chiptune [song.dvechip] [options]\n"
        << "  --wav PATH          write 16-bit stereo WAV\n"
        << "  --sample PATH       write engine .dvesample asset\n"
        << "  --emit-chip PATH    write the canonical selected/generated .dvechip\n"
        << "  --emit-demo PATH    compatibility alias for --emit-chip\n"
        << "  --seconds N         maximum song render duration (default 8)\n"
        << "  --sfx PRESET        generate coin, jump, laser, explosion, hit, power_up,\n"
        << "                      ui_confirm, or ui_cancel instead of loading a song\n"
        << "  --sfx-bank DIR      emit all presets as .dvechip, .wav, and .dvesample\n"
        << "  --base-midi N       SFX base MIDI note (12..127, default 72)\n"
        << "  --duration N        SFX duration in seconds (0.05..2, default 0.35)\n"
        << "  --gain N            SFX master gain (0..1.5, default 0.8)\n"
        << "  --pan N             SFX stereo pan (-1..1, default 0)\n"
        << "  --sample-rate N     SFX sample rate (8000..192000, default 48000)\n";
}

std::optional<ChipSfxPreset> parse_preset(std::string_view name) {
    for (const auto& item : kPresets) {
        if (item.name == name) return item.preset;
    }
    return std::nullopt;
}

std::string_view preset_name(ChipSfxPreset preset) {
    for (const auto& item : kPresets) {
        if (item.preset == preset) return item.name;
    }
    return "unknown";
}

bool parse_float(std::string_view text, float& value) {
    std::string owned{text};
    char* end = nullptr;
    value = std::strtof(owned.c_str(), &end);
    return end == owned.c_str() + owned.size() && std::isfinite(value);
}

bool parse_int(std::string_view text, int& value) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool parse_u32(std::string_view text, std::uint32_t& value) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool write_text(const std::filesystem::path& path, std::string_view text, std::string& error) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        error = "cannot create output directory: " + ec.message();
        return false;
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error = "cannot open " + path.string();
        return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) {
        error = "write failed for " + path.string();
        return false;
    }
    return true;
}

bool export_audio(const dve::audio::ChipSong& song, float seconds,
                  const std::filesystem::path& wavPath,
                  const std::filesystem::path& samplePath, std::string& error) {
    using namespace dve::audio;
    DecodedAudioAsset asset = render_chiptune_audio_asset(song, seconds, &error);
    if (asset.samples.empty()) {
        if (error.empty()) error = "render produced no samples";
        return false;
    }
    if (!wavPath.empty() && !write_wav_file(wavPath, asset, WavSampleEncoding::Pcm16, &error)) {
        return false;
    }
    if (!samplePath.empty() && !write_cooked_audio_asset(samplePath, asset, &error)) {
        return false;
    }
    return true;
}

bool emit_sfx_bank(const std::filesystem::path& directory, const dve::audio::ChipSfxRequest& base,
                   std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        error = "cannot create SFX bank directory: " + ec.message();
        return false;
    }

    for (const auto& item : kPresets) {
        auto request = base;
        request.preset = item.preset;
        const auto song = dve::audio::make_chiptune_sfx_song(request);
        const std::string stem{item.name};
        if (!write_text(directory / (stem + ".dvechip"), song.serialize(), error)) return false;
        if (!export_audio(song, request.durationSeconds + 0.1F,
                          directory / (stem + ".wav"),
                          directory / (stem + ".dvesample"), error)) return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    using namespace dve::audio;

    std::filesystem::path inputPath;
    std::filesystem::path wavPath;
    std::filesystem::path samplePath;
    std::filesystem::path emitChipPath;
    std::filesystem::path sfxBankPath;
    float seconds = 8.0F;
    ChipSfxRequest sfxRequest;
    std::optional<ChipSfxPreset> selectedPreset;

    auto need_value = [&](int& index, std::string_view option) -> const char* {
        if (index + 1 >= argc) {
            std::cerr << option << " requires a value\n";
            print_usage();
            return nullptr;
        }
        return argv[++index];
    };

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        }
        if (arg == "--wav") {
            const char* value = need_value(i, arg); if (!value) return 2; wavPath = value;
        } else if (arg == "--sample") {
            const char* value = need_value(i, arg); if (!value) return 2; samplePath = value;
        } else if (arg == "--emit-chip" || arg == "--emit-demo") {
            const char* value = need_value(i, arg); if (!value) return 2; emitChipPath = value;
        } else if (arg == "--sfx-bank") {
            const char* value = need_value(i, arg); if (!value) return 2; sfxBankPath = value;
        } else if (arg == "--sfx") {
            const char* value = need_value(i, arg); if (!value) return 2;
            selectedPreset = parse_preset(value);
            if (!selectedPreset) {
                std::cerr << "unknown SFX preset: " << value << '\n';
                print_usage();
                return 2;
            }
        } else if (arg == "--seconds") {
            const char* value = need_value(i, arg); if (!value || !parse_float(value, seconds) || seconds <= 0.0F) {
                std::cerr << "--seconds must be a finite positive number\n"; return 2;
            }
        } else if (arg == "--duration") {
            const char* value = need_value(i, arg); if (!value || !parse_float(value, sfxRequest.durationSeconds)) {
                std::cerr << "--duration must be a finite number\n"; return 2;
            }
        } else if (arg == "--gain") {
            const char* value = need_value(i, arg); if (!value || !parse_float(value, sfxRequest.gain)) {
                std::cerr << "--gain must be a finite number\n"; return 2;
            }
        } else if (arg == "--pan") {
            const char* value = need_value(i, arg); if (!value || !parse_float(value, sfxRequest.pan)) {
                std::cerr << "--pan must be a finite number\n"; return 2;
            }
        } else if (arg == "--base-midi") {
            const char* value = need_value(i, arg); if (!value || !parse_int(value, sfxRequest.baseMidi)) {
                std::cerr << "--base-midi must be an integer\n"; return 2;
            }
        } else if (arg == "--sample-rate") {
            const char* value = need_value(i, arg); if (!value || !parse_u32(value, sfxRequest.sampleRate)) {
                std::cerr << "--sample-rate must be an unsigned integer\n"; return 2;
            }
        } else if (!arg.empty() && arg.front() != '-' && inputPath.empty()) {
            inputPath = std::string{arg};
        } else {
            std::cerr << "unknown or duplicate argument: " << arg << '\n';
            print_usage();
            return 2;
        }
    }

    if (!inputPath.empty() && selectedPreset) {
        std::cerr << "a song path and --sfx are mutually exclusive\n";
        return 2;
    }
    if (sfxRequest.durationSeconds < 0.05F || sfxRequest.durationSeconds > 2.0F ||
        sfxRequest.gain < 0.0F || sfxRequest.gain > 1.5F ||
        sfxRequest.pan < -1.0F || sfxRequest.pan > 1.0F ||
        sfxRequest.baseMidi < 12 || sfxRequest.baseMidi > 127 ||
        sfxRequest.sampleRate < 8000U || sfxRequest.sampleRate > 192000U) {
        std::cerr << "SFX parameters are outside their documented ranges\n";
        return 2;
    }

    std::string error;
    if (!sfxBankPath.empty()) {
        if (!emit_sfx_bank(sfxBankPath, sfxRequest, error)) {
            std::cerr << "SFX bank export failed: " << error << '\n';
            return 1;
        }
        std::cout << "wrote 8-preset SFX bank to " << sfxBankPath << '\n';
        if (inputPath.empty() && !selectedPreset && emitChipPath.empty() &&
            wavPath.empty() && samplePath.empty()) return 0;
    }

    ChipSong song;
    if (!inputPath.empty()) {
        const auto text = read_file(inputPath);
        if (!text) {
            std::cerr << "cannot read " << inputPath << '\n';
            return 1;
        }
        if (!ChipSong::parse(*text, song, &error)) {
            std::cerr << "parse failed: " << error << '\n';
            return 1;
        }
        std::cout << "loaded " << song.name << " (" << song.channelCount << " channels, "
                  << song.instruments.size() << " instruments, " << song.patterns.size()
                  << " patterns)\n";
    } else if (selectedPreset) {
        sfxRequest.preset = *selectedPreset;
        song = make_chiptune_sfx_song(sfxRequest);
        seconds = sfxRequest.durationSeconds + 0.1F;
        std::cout << "generated SFX preset " << preset_name(*selectedPreset) << '\n';
    } else {
        song = make_demo_chiptune_song();
        std::cout << "using built-in demo song\n";
    }

    if (!emitChipPath.empty()) {
        if (!write_text(emitChipPath, song.serialize(), error)) {
            std::cerr << error << '\n';
            return 1;
        }
        std::cout << "wrote " << emitChipPath << " (hash 0x" << std::hex << song.content_hash()
                  << std::dec << ")\n";
    }

    if (!wavPath.empty() || !samplePath.empty()) {
        if (!export_audio(song, seconds, wavPath, samplePath, error)) {
            std::cerr << "audio export failed: " << error << '\n';
            return 1;
        }
        if (!wavPath.empty()) std::cout << "wrote " << wavPath << '\n';
        if (!samplePath.empty()) std::cout << "wrote " << samplePath << '\n';
    }

    if (sfxBankPath.empty() && emitChipPath.empty() && wavPath.empty() && samplePath.empty()) {
        std::cout << "validated input; no output requested\n";
    }
    return 0;
}
