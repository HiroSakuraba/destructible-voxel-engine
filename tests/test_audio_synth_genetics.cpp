// Tests for seeded patch mutation and breeding (SYN-013).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "dve/audio/patch_genetics.hpp"

using namespace dve::audio;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

int main() {
    // Test 1: seed reproducibility — same seed gives byte-identical results.
    {
        const SynthPreset p = SynthPreset::make_default();
        const SynthPreset m1 = mutate_preset(p, 0.7F, 12345ULL, 0U);
        const SynthPreset m2 = mutate_preset(p, 0.7F, 12345ULL, 0U);
        CHECK(m1.serialize() == m2.serialize());
        // Different seeds give different mutations (overwhelmingly likely).
        const SynthPreset m3 = mutate_preset(p, 0.7F, 99999ULL, 0U);
        CHECK(m1.serialize() != m3.serialize());
        std::printf("reproducibility: same-seed identical, different-seed differs\n");
    }

    // Test 2: intensity 0 returns the patch unchanged.
    {
        const SynthPreset p = SynthPreset::make_default();
        const SynthPreset m = mutate_preset(p, 0.0F, 4242ULL, 0U);
        CHECK(m.serialize() == p.serialize());
        // And locking every group at positive intensity also leaves it unchanged.
        const SynthPreset locked = mutate_preset(p, 1.0F, 4242ULL, kAllGeneGroups);
        CHECK(locked.serialize() == p.serialize());
        std::printf("intensity 0 / all-locked: patch unchanged\n");
    }

    // Test 3: oscillator tuning snaps to integer semitone offsets.
    {
        bool anyChanged = false;
        for (std::uint64_t seed = 0; seed < 50; ++seed) {
            SynthPreset p = SynthPreset::make_default();
            p.oscillators[0].semitones = 3.0F;  // start on an integer
            const SynthPreset m = mutate_preset(p, 1.0F, seed, 0U);
            for (const auto& osc : m.oscillators) {
                const float v = osc.semitones;
                CHECK(std::isfinite(v));
                CHECK(v >= -96.0F && v <= 96.0F);
                // Must be an integer semitone offset (snapped, not fractional drift).
                CHECK(std::abs(v - std::round(v)) < 1e-4F);
                if (v != p.oscillators[0].semitones && &osc == &m.oscillators[0]) anyChanged = true;
            }
        }
        CHECK(anyChanged);  // sanity: mutation actually fires at intensity 1
        std::printf("tuning: semitone snap verified over 50 seeds\n");
    }

    // Test 4: envelope times stay positive and move multiplicatively (ratio-based).
    {
        SynthPreset p = SynthPreset::make_default();
        p.ampEnvelope.attackSeconds = 0.5F;
        p.ampEnvelope.decaySeconds = 0.5F;
        p.ampEnvelope.releaseSeconds = 0.5F;
        const float maxOctaves = 0.5F + 2.5F * 0.5F;  // 1.75 at intensity 0.5
        for (std::uint64_t seed = 0; seed < 30; ++seed) {
            const SynthPreset m = mutate_preset(p, 0.5F, seed, 0U);
            const float times[3] = {m.ampEnvelope.attackSeconds, m.ampEnvelope.decaySeconds,
                                    m.ampEnvelope.releaseSeconds};
            for (float t : times) {
                CHECK(std::isfinite(t));
                CHECK(t >= 0.0F && t <= 60.0F);
                if (t > 0.0F) {
                    const float logRatio = std::log2(t / 0.5F);
                    CHECK(std::abs(logRatio) <= maxOctaves + 0.01F);
                }
            }
            CHECK(m.ampEnvelope.sustainLevel >= 0.0F && m.ampEnvelope.sustainLevel <= 1.0F);
        }
        std::printf("envelopes: positive, multiplicative, within ratio bounds\n");
    }

    // Test 5: filter cutoff moves in the log domain (ratio-based).
    {
        SynthPreset p = SynthPreset::make_default();
        p.filter.cutoffHertz = 1000.0F;
        const float maxOctaves = 0.5F + 2.5F * 1.0F;  // 3.0 at intensity 1
        for (std::uint64_t seed = 0; seed < 30; ++seed) {
            const SynthPreset m = mutate_preset(p, 1.0F, seed, 0U);
            const float c = m.filter.cutoffHertz;
            CHECK(std::isfinite(c));
            CHECK(c >= 18.0F && c <= 24000.0F);
            const float logRatio = std::log2(c / 1000.0F);
            CHECK(std::abs(logRatio) <= maxOctaves + 0.01F);
        }
        std::printf("cutoff: log-domain drift within ratio bounds\n");
    }

    // Test 6: locks are respected — locked groups are copied verbatim.
    {
        const SynthPreset p = SynthPreset::make_default();
        const SynthPreset m = mutate_preset(p, 1.0F, 777ULL, gene_group_bit(GeneGroup::Filters));
        // The Filters group owns the whole filter struct except its envelope
        // (which belongs to Envelopes). Compare with the envelope masked out.
        FilterParameters masked = m.filter;
        masked.envelope = p.filter.envelope;
        CHECK(std::memcmp(&masked, &p.filter, sizeof(FilterParameters)) == 0);
        // Unlocked groups should have changed somewhere (intensity 1 hits hard).
        CHECK(m.serialize() != p.serialize());
        // Envelopes-only lock: amp envelope untouched.
        const SynthPreset m2 = mutate_preset(p, 1.0F, 777ULL, gene_group_bit(GeneGroup::Envelopes));
        CHECK(std::memcmp(&m2.ampEnvelope, &p.ampEnvelope, sizeof(AdsrParameters)) == 0);
        CHECK(std::memcmp(&m2.filter.envelope, &p.filter.envelope, sizeof(AdsrParameters)) == 0);
        std::printf("locks: locked groups byte-identical, unlocked groups mutate\n");
    }

    // Test 7: breeding takes groups from the right parent.
    {
        SynthPreset a = SynthPreset::make_default();
        SynthPreset b = SynthPreset::make_default();
        a.name = "Parent A";
        b.name = "Parent B";
        a.filter.cutoffHertz = 200.0F;
        b.filter.cutoffHertz = 8000.0F;
        a.oscillators[0].gain = 0.1F;
        b.oscillators[0].gain = 0.9F;
        a.oscillators[1].waveform = OscillatorWaveform::Saw;
        b.oscillators[1].waveform = OscillatorWaveform::Square;

        const SynthPreset child =
            breed_presets(a, b, gene_group_bit(GeneGroup::Filters), 31337ULL);
        // Filters group came from B (post-breed mutation is small, so it stays
        // much closer to B than to A).
        CHECK(std::abs(child.filter.cutoffHertz - 8000.0F) < std::abs(child.filter.cutoffHertz - 200.0F));
        // Oscillators group came from A.
        CHECK(std::abs(child.oscillators[0].gain - 0.1F) < std::abs(child.oscillators[0].gain - 0.9F));
        // Reproducible.
        const SynthPreset child2 =
            breed_presets(a, b, gene_group_bit(GeneGroup::Filters), 31337ULL);
        CHECK(child.serialize() == child2.serialize());
        // Breeding everything from B (except the name) lands near B.
        const SynthPreset childAll = breed_presets(a, b, kAllGeneGroups, 31337ULL);
        CHECK(std::abs(childAll.filter.cutoffHertz - 8000.0F) <
              std::abs(childAll.filter.cutoffHertz - 200.0F));
        std::printf("breed: filters from B (%.0f Hz), osc gain from A (%.3f)\n",
                    child.filter.cutoffHertz, child.oscillators[0].gain);
    }

    // Test 8: no NaNs and valid presets across 100 random mutations and breeds.
    {
        const SynthPreset base = SynthPreset::make_default();
        std::string error;
        for (std::uint64_t i = 0; i < 100; ++i) {
            const float intensity = 0.1F + 0.9F * static_cast<float>(i % 11) / 10.0F;
            const SynthPreset m = mutate_preset(base, intensity, 1000ULL + i * 7919ULL, 0U);
            error.clear();
            CHECK(m.validate(&error));
            if (!error.empty()) std::printf("mutation %lu invalid: %s\n", i, error.c_str());
            CHECK(std::isfinite(m.masterGain) && std::isfinite(m.masterPan));
        }
        // Breeds with a spread of group masks.
        const SynthPreset other = SynthPreset::builtin_presets().front();
        for (std::uint64_t i = 0; i < 20; ++i) {
            const std::uint8_t mask = static_cast<std::uint8_t>(i * 37U + 11U);
            const SynthPreset child = breed_presets(base, other, mask, 555ULL + i);
            error.clear();
            CHECK(child.validate(&error));
            if (!error.empty()) std::printf("breed %lu invalid: %s\n", i, error.c_str());
        }
        std::printf("no NaNs / validate() green over 100 mutations + 20 breeds\n");
    }

    if (g_failures == 0) std::printf("ALL GENETICS TESTS PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
