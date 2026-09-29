// Phase 5: compiled-patch round-trip for the preset-level spectral/resynthesis
// parameters (SpectralParameters), plus backward compatibility: a patch
// without the spectral keys must still load with spectral defaults, and
// unknown IDs must be skipped gracefully.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "dve/audio/synth_patch.hpp"
#include "dve/audio/synthesizer.hpp"

using namespace dve::audio;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

// Spectral key range, derived from the enum — never hardcoded. (Phase 4's
// strip helper hardcoded 0x0800-0x0810 and missed 0x0811 GranularQuality,
// which broke the backward-compat section; this cannot drift the same way.)
static constexpr std::uint16_t kSpectralKeyLo =
    static_cast<std::uint16_t>(SynthPatchParam::SpectralEnabled);
static constexpr std::uint16_t kSpectralKeyHi =
    static_cast<std::uint16_t>(SynthPatchParam::SpectralPhaseRandom);

static_assert(kSpectralKeyLo == 0x0900, "spectral block must start at 0x0900");
static_assert(kSpectralKeyHi > kSpectralKeyLo, "spectral block must be non-empty");

// A spectral probe with every field set to a non-default value.
static SynthPreset make_spectral_probe() {
    SynthPreset preset = SynthPreset::make_default();
    preset.name = "Spectral Serialization Probe";
    auto& s = preset.spectral;
    s.enabled = false;
    s.gain = 0.33F;
    s.timeStretch = 0.5F;
    s.freeze01 = 0.75F;
    s.formantShiftSemitones = 7.0F;
    s.harmonicStretch = 1.5F;
    s.spectralTiltDbPerOct = -6.0F;
    s.partialThreshold01 = 0.4375F;
    s.spectralBlur01 = 0.625F;
    s.frequencyQuantize01 = 0.5F;
    s.inharmonicity01 = 0.1875F;
    s.phaseRandom01 = 0.25F;
    s.stereoSpread01 = 0.8F;
    s.spectralQuality = FilterQuality::Eco;
    std::string error;
    if (!preset.validate(&error)) {
        std::printf("probe preset invalid: %s\n", error.c_str());
        ++g_failures;
    }
    return preset;
}

static void check_spectral_exact(const SpectralParameters& a, const SpectralParameters& b) {
    // Bit-exact: the binary format stores raw f32/u32 bits, so == is exact.
    CHECK(a.enabled == b.enabled);
    CHECK(a.gain == b.gain);
    CHECK(a.timeStretch == b.timeStretch);
    CHECK(a.freeze01 == b.freeze01);
    CHECK(a.formantShiftSemitones == b.formantShiftSemitones);
    CHECK(a.harmonicStretch == b.harmonicStretch);
    CHECK(a.spectralTiltDbPerOct == b.spectralTiltDbPerOct);
    CHECK(a.partialThreshold01 == b.partialThreshold01);
    CHECK(a.spectralBlur01 == b.spectralBlur01);
    CHECK(a.frequencyQuantize01 == b.frequencyQuantize01);
    CHECK(a.inharmonicity01 == b.inharmonicity01);
    CHECK(a.phaseRandom01 == b.phaseRandom01);
    CHECK(a.stereoSpread01 == b.stereoSpread01);
    CHECK(a.spectralQuality == b.spectralQuality);
}

static std::uint32_t crc32_local(const std::uint8_t* data, std::size_t size) noexcept {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) crc = (crc & 1U) ? (crc >> 1) ^ 0xEDB88320U : crc >> 1;
    }
    return crc ^ 0xFFFFFFFFU;
}

// Rebuild a patch program from a filtered entry list: keeps every entry whose
// ID is not in the spectral range, then rewrites the entry count and checksum.
static std::vector<std::uint8_t> strip_spectral_entries(const std::vector<std::uint8_t>& bytes) {
    std::vector<std::uint8_t> out;
    out.insert(out.end(), bytes.begin(), bytes.begin() + 12); // header
    std::uint32_t entryCount = static_cast<std::uint32_t>(bytes[8]) |
        (static_cast<std::uint32_t>(bytes[9]) << 8) |
        (static_cast<std::uint32_t>(bytes[10]) << 16) |
        (static_cast<std::uint32_t>(bytes[11]) << 24);
    const std::uint8_t* p = bytes.data() + 12;
    const std::uint8_t* end = bytes.data() + bytes.size() - 4;
    std::uint32_t kept = 0;
    auto entry_size = [](const std::uint8_t* q, std::uint8_t type) -> std::size_t {
        if (type == 0 || type == 1) return 2 + 1 + 4;
        if (type == 2) return 2 + 1 + 1;
        if (type == 3) {
            const std::uint16_t len = static_cast<std::uint16_t>(q[3]) | (static_cast<std::uint16_t>(q[4]) << 8);
            return 2 + 1 + 2 + len;
        }
        return 0;
    };
    for (std::uint32_t i = 0; i < entryCount && p < end; ++i) {
        const std::uint16_t id = static_cast<std::uint16_t>(p[0]) | (static_cast<std::uint16_t>(p[1]) << 8);
        const std::uint8_t type = p[2];
        const std::size_t size = entry_size(p, type);
        if (size == 0 || p + size > end) { ++g_failures; return {}; }
        const bool isSpectral = id >= kSpectralKeyLo && id <= kSpectralKeyHi;
        if (!isSpectral) {
            out.insert(out.end(), p, p + size);
            ++kept;
        }
        p += size;
    }
    out[8] = static_cast<std::uint8_t>(kept);
    out[9] = static_cast<std::uint8_t>(kept >> 8);
    out[10] = static_cast<std::uint8_t>(kept >> 16);
    out[11] = static_cast<std::uint8_t>(kept >> 24);
    const std::uint32_t crc = crc32_local(out.data(), out.size());
    out.push_back(static_cast<std::uint8_t>(crc));
    out.push_back(static_cast<std::uint8_t>(crc >> 8));
    out.push_back(static_cast<std::uint8_t>(crc >> 16));
    out.push_back(static_cast<std::uint8_t>(crc >> 24));
    return out;
}

// Insert one unknown entry (id 0xF00D, float) into a compiled program, then
// fix the entry count and checksum.
static std::vector<std::uint8_t> inject_unknown_entry(const std::vector<std::uint8_t>& bytes) {
    std::vector<std::uint8_t> out(bytes.begin(), bytes.end() - 4); // drop old crc
    const std::uint8_t entry[7] = {0x0D, 0xF0, 0x00, 0x00, 0x00, 0x80, 0x3F}; // id, float, 1.0f
    out.insert(out.end(), entry, entry + 7);
    std::uint32_t count = static_cast<std::uint32_t>(out[8]) |
        (static_cast<std::uint32_t>(out[9]) << 8) |
        (static_cast<std::uint32_t>(out[10]) << 16) |
        (static_cast<std::uint32_t>(out[11]) << 24);
    ++count;
    out[8] = static_cast<std::uint8_t>(count);
    out[9] = static_cast<std::uint8_t>(count >> 8);
    out[10] = static_cast<std::uint8_t>(count >> 16);
    out[11] = static_cast<std::uint8_t>(count >> 24);
    const std::uint32_t crc = crc32_local(out.data(), out.size());
    out.push_back(static_cast<std::uint8_t>(crc));
    out.push_back(static_cast<std::uint8_t>(crc >> 8));
    out.push_back(static_cast<std::uint8_t>(crc >> 16));
    out.push_back(static_cast<std::uint8_t>(crc >> 24));
    return out;
}

int main() {
    std::string error;

    // 1. Exact round-trip with every spectral field non-default.
    {
        const SynthPreset probe = make_spectral_probe();
        const SynthPatchProgram program = compile_patch(probe);
        CHECK(program.valid());
        auto loaded = load_patch_program(program, &error);
        CHECK(loaded.has_value());
        if (loaded) {
            CHECK(loaded->name == probe.name);
            check_spectral_exact(probe.spectral, loaded->spectral);
            std::string validation;
            CHECK(loaded->validate(&validation));
        } else {
            std::printf("load error: %s\n", error.c_str());
        }
        std::printf("exact round-trip: %zu bytes\n", program.bytes.size());
    }

    // 2. Backward compatibility: an old patch without spectral keys loads
    // with spectral defaults.
    {
        const SynthPreset probe = make_spectral_probe();
        const SynthPatchProgram program = compile_patch(probe);
        const std::vector<std::uint8_t> stripped = strip_spectral_entries(program.bytes);
        CHECK(!stripped.empty());
        auto loaded = load_patch_program(stripped.data(), stripped.size(), &error);
        CHECK(loaded.has_value());
        if (loaded) {
            const SpectralParameters defaults = SynthPreset::make_default().spectral;
            check_spectral_exact(defaults, loaded->spectral);
            // Non-spectral fields still survived the strip.
            CHECK(loaded->name == probe.name);
            std::string validation;
            CHECK(loaded->validate(&validation));
        } else {
            std::printf("stripped load error: %s\n", error.c_str());
        }
        std::printf("backward compatibility: OK\n");
    }

    // 3. Unknown IDs are skipped gracefully (newer patch on older build).
    {
        const SynthPreset probe = make_spectral_probe();
        const SynthPatchProgram program = compile_patch(probe);
        const std::vector<std::uint8_t> injected = inject_unknown_entry(program.bytes);
        auto loaded = load_patch_program(injected.data(), injected.size(), &error);
        CHECK(loaded.has_value());
        if (loaded) {
            check_spectral_exact(probe.spectral, loaded->spectral);
            CHECK(loaded->name == probe.name);
        } else {
            std::printf("injected load error: %s\n", error.c_str());
        }
        std::printf("unknown-id skip: OK\n");
    }

    // 4. Every builtin preset (incl. the spectral showcase) round-trips valid.
    {
        for (const auto& preset : SynthPreset::builtin_presets()) {
            auto prog = compile_patch(preset);
            auto back = load_patch_program(prog, &error);
            CHECK(back.has_value());
            if (!back) {
                std::printf("preset '%s' failed: %s\n", preset.name.c_str(), error.c_str());
                continue;
            }
            check_spectral_exact(preset.spectral, back->spectral);
            std::string validation;
            CHECK(back->validate(&validation));
        }
        std::printf("builtin preset round-trips: OK\n");
    }

    if (g_failures == 0) std::printf("spectral serialization: all tests passed\n");
    else std::printf("spectral serialization: %d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
