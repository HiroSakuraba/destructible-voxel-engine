// Tests for the compiled synth patch format (Phase 0).
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>

#include "dve/audio/synth_patch.hpp"
#include "dve/audio/synthesizer.hpp"

using namespace dve::audio;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static bool nearly(float a, float b, float tol = 1e-5F) {
    return std::fabs(a - b) <= tol * (1.0F + std::fabs(b));
}

int main() {
    // Round-trip: compile a preset and load it back.
    SynthPreset original = SynthPreset::make_default();
    original.name = "Patch Round Trip";
    original.masterGain = 0.63F;
    original.filter.cutoffHertz = 3200.0F;
    original.arpeggiator.phraseVelocityStart = 0.5F;
    original.arpeggiator.phraseVelocityEnd = 1.0F;
    original.delay.enabled = true;
    original.delay.timeSeconds = 0.375F;
    original.reverb.mix = 0.42F;

    SynthPatchProgram program = compile_patch(original);
    CHECK(program.valid());
    CHECK(program.bytes.size() > 16);

    std::string error;
    auto loaded = load_patch_program(program, &error);
    CHECK(loaded.has_value());
    if (!loaded) { std::printf("load error: %s\n", error.c_str()); return 1; }

    CHECK(loaded->name == "Patch Round Trip");
    CHECK(nearly(loaded->masterGain, 0.63F));
    CHECK(nearly(loaded->filter.cutoffHertz, 3200.0F));
    CHECK(nearly(loaded->arpeggiator.phraseVelocityStart, 0.5F));
    CHECK(nearly(loaded->arpeggiator.phraseVelocityEnd, 1.0F));
    CHECK(loaded->delay.enabled);
    CHECK(nearly(loaded->delay.timeSeconds, 0.375F));
    CHECK(nearly(loaded->reverb.mix, 0.42F));

    // Every builtin preset must round-trip and stay valid.
    for (const auto& preset : SynthPreset::builtin_presets()) {
        auto prog = compile_patch(preset);
        auto back = load_patch_program(prog, &error);
        CHECK(back.has_value());
        if (!back) { std::printf("preset '%s' failed: %s\n", preset.name.c_str(), error.c_str()); continue; }
        CHECK(back->name == preset.name);
        CHECK(nearly(back->masterGain, preset.masterGain));
        std::string validation;
        CHECK(back->validate(&validation));
    }

    // Corruption is detected.
    {
        auto bad = program;
        bad.bytes[20] ^= 0xFF;
        auto result = load_patch_program(bad, &error);
        CHECK(!result.has_value());
        CHECK(!error.empty());
    }

    // Wrong magic is rejected.
    {
        auto bad = program;
        bad.bytes[0] = 'X';
        // Fix the checksum so we reach the magic check.
        std::uint32_t crc = 0xFFFFFFFFU;
        for (std::size_t i = 0; i + 4 < bad.bytes.size(); ++i) {
            crc ^= bad.bytes[i];
            for (int b = 0; b < 8; ++b) crc = (crc & 1U) ? (crc >> 1) ^ 0xEDB88320U : crc >> 1;
        }
        crc ^= 0xFFFFFFFFU;
        const std::size_t n = bad.bytes.size();
        bad.bytes[n - 4] = static_cast<std::uint8_t>(crc);
        bad.bytes[n - 3] = static_cast<std::uint8_t>(crc >> 8);
        bad.bytes[n - 2] = static_cast<std::uint8_t>(crc >> 16);
        bad.bytes[n - 1] = static_cast<std::uint8_t>(crc >> 24);
        auto result = load_patch_program(bad, &error);
        CHECK(!result.has_value());
    }

    // Truncated input is rejected.
    {
        auto result = load_patch_program(program.bytes.data(), 8, &error);
        CHECK(!result.has_value());
    }

    if (g_failures == 0) std::printf("patch program tests: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
