# Phase 0 Smoothing Audit — DVE Synthesizer Engine

**Date:** 28 Sept 2026
**Scope:** Read-only audit of parameter smoothing coverage in `include/dve/audio/synthesizer.hpp` and `src/audio/synthesizer.cpp` (branch `merge-work`, remote main `ff065ff9`).
**Question:** When a user-facing parameter changes live (knob tweak, automation, preset edit), does the render path smooth the transition, or does the value jump instantly (click/zipper risk)?

## Architecture: why most parameters jump

`Synthesizer::set_preset()` (src/audio/synthesizer.cpp:5400) cooks a `RealtimePreset` and pushes it through a lock-free queue. At the next `render()` block boundary, `adopt_preset()` does a wholesale struct copy:

```cpp
// src/audio/synthesizer.cpp:2645-2647
void adopt_preset(const RealtimePreset& next) noexcept {
    ...
    parameters = next;
```

Every parameter in the preset swaps **atomically with zero per-parameter smoothing**. The render path then reads values directly from `parameters` each block/sample. There is no global smoothing layer between preset adoption and the DSP.

## SMOOTHED

| Parameter / path | Mechanism | Location |
|---|---|---|
| Modulation matrix slots | One-pole lowpass, configurable `smoothingMilliseconds` (default 8 ms, range 0–2000 ms): `v.modulationSmoothing[i] += (source - v.modulationSmoothing[i]) * coeff` | src/audio/synthesizer.cpp:2853–2860 |
| Breath pressure (physical models) | Fixed one-pole: `state.breathPressure * 0.90F + continuous * 0.10F` | src/audio/synthesizer.cpp:957 |
| Physical-model fractional delay length | Fixed one-pole: `delayLengthSamples += (target - delayLengthSamples) * 0.0025F` — explicitly so pitch bend/MPE don't jump | src/audio/synthesizer.cpp:946 |
| Arp step macro automation (Linear/Smooth curves) | Per-step ramp: `macroValues[i] += (target - macroValues[i]) * rate` (0.35 linear / 0.18 smooth). Note: `Step` curve is instant by design | src/audio/synthesizer.cpp:2516–2522 |
| Compressor detector | Inherent attack/release ballistics on `compressorEnvelope` (smooths the *signal follower*, not parameter changes) | src/audio/synthesizer.cpp:3548–3553 |
| Limiter envelope | Inherent release ballistics on `limiterEnvelope` | src/audio/synthesizer.cpp:3562–3564 |
| Filter output | Filter delay-element state inherently smooths output, but **not** parameter steps (see below) | — |

## NOT SMOOTHED (direct reads from `parameters` in the render path)

### Voice path — highest click risk

| Parameter | Location | Note |
|---|---|---|
| **Filter cutoff** | src/audio/synthesizer.cpp:3434–3436 | `parameters.filter.cutoffHertz` read directly per voice render; `AnalogFilter::process` applies it with no internal smoothing. Step changes zipper audibly, worse at high resonance |
| **Filter resonance** | src/audio/synthesizer.cpp:3438–3439 | Direct read. Resonance jumps change the filter's Q instantly — clicks/pings near self-oscillation |
| **Oscillator gain** | src/audio/synthesizer.cpp:3385 | `osc.gain * gainMod` multiplied directly per sample. Any gain tweak is an instant amplitude step |
| **Oscillator pan** | src/audio/synthesizer.cpp:3388–3390 | `sqrt` pan law applied directly; pan jumps image-shift instantly |
| **Oscillator semitones/cents, tuning** | src/audio/synthesizer.cpp:3267–3270 | Pitch computed directly; frequency jumps are by-design for pitch but will click if automated coarsely |
| Pitch bend / MPE bend | src/audio/synthesizer.cpp:3205–3210 | Direct. Low practical risk — MIDI bend arrives in small increments; physical-model path additionally smooths via fractional delay |
| Envelope times (A/D/S/R) | src/audio/synthesizer.cpp:304+ (`Envelope::advance`) | Read live per sample. Low click risk — they reshape a continuous trajectory rather than stepping a level |

### Master / channel output

| Parameter | Location | Note |
|---|---|---|
| **Master gain** | src/audio/synthesizer.cpp:5547–5548, 5560–5565 | Applied directly at the output bus. Instant level steps |
| Master pan | src/audio/synthesizer.cpp:5551–5552, 5564–5565 | Direct `sqrt` pan law |
| Channel volume (CC7) | src/audio/synthesizer.cpp:5557 | Direct read, no smoothing |
| Channel pan (CC10) | src/audio/synthesizer.cpp:5558 | Direct read, no smoothing |

### FX chain — all direct reads, no smoothing

| Parameter | Location | Note |
|---|---|---|
| **Delay time** | src/audio/synthesizer.cpp:3576 | `delaySamples` computed directly from `timeSeconds`; changing it jumps the delay-line read position → audible pitch jump/glitch |
| **Delay feedback / mix** | src/audio/synthesizer.cpp:3579–3582 | Direct. Feedback jumps with long tails are very audible |
| **EQ band gains** | src/audio/synthesizer.cpp:3506–3507 | `db_to_gain()` applied directly per sample. dB tweaks = instant multiplier steps |
| **Distortion drive / mix** | src/audio/synthesizer.cpp:3449, 3453, 3457 | Direct into `fast_tanh`; drive steps reshape the transfer curve instantly |
| **Flanger feedback** | src/audio/synthesizer.cpp:3519 | Direct. Feedback jumps in a resonant loop click |
| **Compressor threshold / ratio / makeup** | src/audio/synthesizer.cpp:3554–3557 | Threshold steps = instant gain-reduction steps |
| **Limiter ceiling** | src/audio/synthesizer.cpp:3560 | Direct |
| Chorus rate / depth / mix | src/audio/synthesizer.cpp:3486–3494 | Direct, but fractional delay-line reads soften the blow — low/medium risk |
| Flanger rate / depth / mix | src/audio/synthesizer.cpp:3497–3512 | Direct; fractional reads help, feedback does not |
| Phaser rate / depth / feedback / mix | src/audio/synthesizer.cpp:3516–3527 | Direct |
| Reverb roomSize / damping / width / mix | src/audio/synthesizer.cpp:715–728 | Direct into comb-filter feedback/damping coefficients; comb state softens but coefficient jumps can thump |
| Bitcrusher bits / downsample / mix | src/audio/synthesizer.cpp:3461–3471 | Direct; bit-depth changes are inherently steppy |
| Ensemble mix | src/audio/synthesizer.cpp:3532 | Direct; low risk (short modulated delays) |

### MIDI CC direct reads (bypass the smoothed mod matrix)

| CC | Use | Location |
|---|---|---|
| CC1 (mod wheel) | Pitch modulation depth, mod-matrix source | src/audio/synthesizer.cpp:3250, 2839 |
| CC71 | Resonance offset | src/audio/synthesizer.cpp:3450 |
| CC74 | Cutoff multiplier (timbre) | src/audio/synthesizer.cpp:3444–3445 |
| CC91 | Reverb mix offset | src/audio/synthesizer.cpp:3580 |
| CC93 | Chorus mix offset | src/audio/synthesizer.cpp:3510 |

These are only smoothed if routed through a modulation slot; the direct reads above are not.

## UNCLEAR / needs measurement

- **Filter topology/mode switches** — structural change, can't be smoothed; acceptable as-is (click expected, like hardware).
- **Delay-line `read_fractional`** (src/audio/synthesizer.cpp) — linear interpolation between taps softens *small* delay-time changes but does not smooth the parameter itself.
- **Preset morphing** (`SynthPreset::morph`, src/audio/synthesizer.cpp:5287) — interpolates cutoff in log domain, but it's a preset-*generation* utility, not a live render-path smoother.

## Highest-risk unsmoothed parameters (Phase 0 priority)

1. **Filter cutoff** — the classic knob-tweak click; highest audibility, most-tweaked live parameter.
2. **Oscillator gain / master gain** — instant amplitude steps click on any material.
3. **Delay time** — read-position jumps cause pitch glitches, not just clicks.
4. **EQ band gains** — instant `db_to_gain` multiplier steps.
5. **Filter resonance** — dangerous near self-oscillation.
6. **Distortion drive, flanger/delay feedback, compressor threshold** — all reshape gain or feedback structure instantly.

## Recommended Phase 0 approach

Add a per-parameter smoothing layer at preset adoption: for each smoothing-eligible float in `RealtimePreset`, keep a `current`/`target` pair in the impl and advance `current` toward `target` with a one-pole (or short linear ramp, ~5–20 ms) once per render block. The modulation-slot smoother (src/audio/synthesizer.cpp:2853–2860) is the existing pattern to follow. Exclude structural/enum parameters (waveform, topology, mode) and per-note state (envelope times are already trajectory-safe).
