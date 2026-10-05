# Phase 0 — Modulation System Map

**Repo:** `~/workspace/dve-engine` @ `2df476e` (branch `merge-work`, remote main `ff065ff9`)
**Date:** 28 Sept 2026
**Status:** READ-ONLY survey. No files modified.

## TL;DR

The synth **already has a real modulation matrix** — this is not hardwired. It is per-voice,
compiled at preset load (only enabled slots are evaluated), with per-slot smoothing, curves,
and polarity. It also has live telemetry for UI visualization and a dedicated editor page.
Phase 0's "modulation matrix" requirement is **~85% done**.

What is missing is almost entirely **Phase 1 scope**: physics-model sources, full MPE
per-note sources (timbre/slide, per-note pitch bend, release velocity), broader destinations
(wavetable position, FX params), per-route bias, and the plan's "click a destination to see
all sources" UX.

---

## 1. Architecture

```
SynthPreset.modulation[16]          // authoring-time slots (serialized)
        │  set_preset()
        ▼
RealtimePreset                       // COMPILED runtime form
  .activeModulation[]                // only enabled slots (source≠Off, dest≠Off)
  .activeModulationIndices[]         // original slot index per active slot
  .activeModulationCount
        │  per voice, per block
        ▼
evaluate_modulation(voice)           // src/audio/synthesizer.cpp:2842
  → ModulationValues                 // accumulated per-destination offsets
```

- **Compilation** happens in `realtime_preset()` (`src/audio/synthesizer.cpp:215`). This is
  the plan's "compiled runtime patch" — at least for the modulation subsystem.
- **Evaluation is per-voice**: `evaluate_modulation(v, amp, filterEnv, lfoValues)` is called
  for every active voice every block (`:3229`).
- **Smoothing is per-voice per-slot**: `v.modulationSmoothing[originalIndex]` with a
  one-pole lowpass; coefficient derived from `slot.smoothingMilliseconds` (0–2000 ms,
  default 8 ms). Bypasses to direct assignment when smoothing ≈ 0.
- **Polarity handling**: sources have a native polarity (`native_bipolar()` — LFO1/2,
  KeyTrack, Random are bipolar; the rest unipolar). The slot's `polarity` setting converts
  (unipolar→bipolar: `*2-1`; bipolar→unipolar: `*0.5+0.5`) *before* the curve is applied.
- **Destination accumulation is additive** across slots, then clamped per destination.

### ModulationSlot (include/dve/audio/synthesizer.hpp:281)

| Field | Type | Range |
|---|---|---|
| `enabled` | bool | — |
| `source` | `ModulationSource` | 14 values (see §2) |
| `destination` | `ModulationDestination` | 39 values (see §3) |
| `amount` | float | −1.0 … +1.0 |
| `curve` | `ModulationCurve` | Linear, Quadratic, Cubic |
| `polarity` | `ModulationPolarity` | Bipolar, Unipolar |
| `smoothingMilliseconds` | float | 0 … 2000 (default 8.0) |

Slot count: `kSynthModulationSlotCount = 16`.

---

## 2. Sources (14) — `ModulationSource` (synthesizer.hpp:120)

| Source | Value produced | Notes |
|---|---|---|
| `Off` | 0 | — |
| `Lfo1`, `Lfo2` | per-voice LFO output (−1…+1) | see §4 |
| `AmpEnvelope` | current amp env value | 0…1 |
| `FilterEnvelope` | current filter env value | 0…1 |
| `Velocity` | `v.velocity` | 0…1 (MIDI velocity / 127) |
| `KeyTrack` | `(note − 60) / 60` | −1…+1, bipolar |
| `ModWheel` | `controller[1]` | MIDI CC 1, 0…1 |
| `Aftertouch` | `max(v.pressure, channelPressure[ch])` | per-note pressure preferred, channel fallback |
| `Random` | `v.noteRandom` | per-note random, bipolar |
| `Macro1`…`Macro4` | `macroValues[i]` | 0…1, see §5 |

**Notably present in `Voice` but NOT exposed as sources:**
- `v.timbre` — per-note MPE timbre/slide exists on the voice (`:3330` uses it for
  filtering) but there is no `ModulationSource::Timbre`/`Slide`.
- `v.pitchBendSemitones` — per-note pitch bend exists but is not a mod source.
- Release velocity — no field, no source.

---

## 3. Destinations (39) — `ModulationDestination` (synthesizer.hpp:125)

| Destination | Scale applied (amount × …) | Clamp |
|---|---|---|
| `GlobalPitch` | × 24 semitones | — |
| `FilterCutoff` | × 8 (octaves) | ±12 |
| `FilterResonance` | × 0.75 | ±1 |
| `FilterDrive` | × 8 | ±16 |
| `VoiceGain` | × 1 | −1…+2 |
| `VoicePan` | × 1 | ±1 |
| `Osc1–8Pitch` | × 24 semitones each | — |
| `Osc1–8Shape` | × 1 each | — |
| `Osc1–8PulseWidth` | × 0.5 each | — |
| `Osc1–8Gain` | × 1 each | — |

Count: 1 (Off) + 6 (global/voice/filter) + 32 (8 oscs × 4 params) = 39.

**Not modulatable (plan wants "nearly every meaningful continuous parameter"):**
wavetable position, filter envelope amount, LFO rate/depth, FX parameters (all 12
effects), arp/chord parameters, unison, granular params, reverb/delay sends.

---

## 4. LFOs

- Count: `kSynthLfoCount = 2`.
- **Per-voice** (`v.lfoPhases[]`, `v.lfoRandomValues[]`) — each voice gets its own LFO
  phase, so polyphonic LFO modulation is independent per note.
- Waveforms (`LfoWaveform`, 6): Sine, Triangle, Saw, Square, SampleAndHold, SmoothRandom.
- Parameters per LFO (`LfoParameters`, synthesizer.hpp:269): enabled, waveform, rateHertz
  (0.01–100), depth (0–1), phase, fadeInSeconds (0–60), keySync (default true),
  tempoSync, beatsPerCycle (0.03125–32, default 1).
- Tempo sync uses the effective arp tempo (internal / game clock / MIDI clock).
- `advance_lfo()` (`src/audio/synthesizer.cpp:2751`): returns 0 when disabled or
  depth = 0. SampleAndHold / SmoothRandom reseed per cycle via xorshift on
  `v.lfoNoiseState[]`.

---

## 5. Macros

- Count: `kSynthMacroCount = 4`. Values 0…1, user-renamable (max 32 chars).
- **Three write paths** into `macroValues[]` (the compiled `RealtimePreset` snapshots
  `source.macros.values` at preset load):
  1. **Preset edit** — the editor modifies `preset.macros.values` then calls `set_preset()`
     (`src/editor_synth.cpp:451`). There is no live `set_macro()` API; UI moves go
     through a full preset re-compilation.
  2. **MIDI learn** — `MidiLearnMapping{controller, macroIndex, minimum, maximum,
     inverted}` (`:2693`); any CC can drive any macro with range remap at runtime.
  3. **Arp step automation** — each of the 16 arp steps has `macro1…macro4` targets
     (≥ 0 = automate); applied per step with Step / Linear / Smooth curves
     (`:2499–2508`). This is the "sequencer lanes → modulation" path, but it only
     exists *through* macros, not as direct matrix sources.
- Macros are unipolar sources; the slot polarity setting can make them bipolar.

---

## 6. "Modulation matrix slot" in validate()

`SynthPreset::validate()` (`src/audio/synthesizer.cpp:4317–4322`) checks every one of
the 16 slots and fails with `"invalid modulation matrix slot"` if:

- `amount` ∉ [−1, 1]
- `smoothingMilliseconds` ∉ [0, 2000]
- `source` > `ModulationSource::Macro4` (i.e. not a known source)
- `destination` > `ModulationDestination::Osc8Gain` (i.e. not a known destination)

Adjacent validation: LFO params (rate/phase/fade/beats ranges), macro values ∈ [0,1],
macro names non-empty ≤ 32 chars, MIDI-learn mappings (controller ≤ 127, macroIndex
valid, min/max ∈ [0,1]).

---

## 7. Serialization

- Slots: `mod{i}.enabled`, `mod{i}.source`, `mod{i}.destination`, `mod{i}.amount`,
  `mod{i}.curve`, `mod{i}.polarity`, `mod{i}.smoothingMs` for i = 0…15.
- Sources/destinations/curves serialize as **string tokens**
  (`modulation_source_token()` etc.); parse rejects unknown tokens
  (`parse_modulation_source()` → `fail("invalid modulation preset key")` on bad key).
- LFOs: `lfo{i}.enabled/waveform/rate/depth/phase/fade/keySync/tempoSync/beats`.
- Macros: `macro{i}.name`, `macro{i}.value`; MIDI learn: `midiLearn{i}.*`.
- The format is **not versioned** — there is no format-version key (separate Phase 0 item).

---

## 8. Editor & telemetry

- **Editor** has a dedicated `SynthPanelPage::Modulation` page: 9 rows per LFO
  (enable, waveform, rate, depth, phase, fade, keySync, tempoSync, beats) + slot editor
  (slot select 0–15, source cycle [14], destination cycle [39], amount, curve).
  (`src/editor_synth.cpp:486`, `src/editor_native_renderer.cpp` Modulation branch.)
- **Live telemetry**: `Synthesizer::modulation_activity()` returns per-slot
  `{enabled, source, destination, polarity, amount, currentValue}` where `currentValue`
  is an atomic snapshot of the smoothed modulation output — enough to drive animated
  slot meters in the UI.
- **Patch morphing** (`:5296`) lerps `modulation[i].amount` between presets — the matrix
  participates in morphing already.

---

## 9. Gap analysis vs the plan

### Phase 0 "modulation matrix" — mostly DONE

| Plan item | Status |
|---|---|
| Routing matrix (source → dest with depth) | ✅ 16 slots, full matrix |
| Compiled runtime patch | ✅ `RealtimePreset` pre-filters active slots |
| Parameter smoothing | ✅ per-slot, per-voice, configurable 0–2000 ms |
| Curves / polarity | ✅ Linear/Quadratic/Cubic; Bipolar/Unipolar with auto-conversion |
| Headless render tests | ✅ audio suites render headlessly (modulation covered in synth tests) |

### Missing — Phase 1 scope (physics modulation, MPE sources)

1. **No physics models.** Plan §14 wants `PhysicsModel` (Ball2D, SpringMass, Pendulum,
   DoublePendulum, Orbit, StrangeAttractor, CoupledOscillators) with `PhysicsOutputs`
   {x, y, speed, energy, angle, collision} routable through the matrix. Nothing of this
   exists — no enum, no simulator, no sources.
2. **MPE sources incomplete.** Plan §20 wants per-note velocity, release velocity,
   pressure, pitch bend, slide as matrix sources. Today: Velocity ✅, Aftertouch
   (pressure) ✅ — but per-note **timbre/slide** ❌, per-note **pitch bend** ❌,
   **release velocity** ❌, despite `v.timbre` and `v.pitchBendSemitones` already
   existing on the voice.
3. **Sequencer lanes are not direct sources.** Arp step automation can only write to
   macros (side-channel); there is no `SeqLane1…N` source.
4. **No game-parameter sources.** Plan lists "optional game parameters" — nothing exists.
5. **Destinations are narrow.** 39 destinations cover oscs + filter + voice gain/pan
   only. No wavetable position, no FX parameters, no LFO rate/depth, no envelope
   times — the plan wants "nearly every meaningful continuous synth parameter."
6. **No per-route bias.** Plan's `ModRoute` has `{source, destination, amount, bias,
   curve, bipolar}`. Current slot has amount but no bias/offset term.
7. **Only 2 LFOs.** The plan explicitly wants physics modulation to *replace* "add more
   LFOs," but 2 LFOs is still the ceiling for conventional cyclic modulation.
8. **Missing UX feature.** Plan §16: "Clicking a destination in the editor should show
   every source currently affecting it and the resulting modulation range." The editor
   edits slots one at a time; there is no reverse lookup (destination → sources).

### Suggested Phase 0 → Phase 1 sequencing

- **Phase 0 remainder (no new DSP):** add per-route `bias`; add missing MPE sources
  (`Timbre`/`Slide`, `NotePitchBend`, `ReleaseVelocity`) — the voice fields already
  exist, so this is enum + `modulation_source_value()` + validation + tokens; widen
  validation accordingly.
- **Phase 1:** physics simulator + `PhysicsX/Y/Speed/Energy/Angle/Collision` sources;
  destination expansion (wavetable position first, then FX); reverse-lookup UX in the
  editor; consider raising slot count 16 → 24/32 once destinations grow.
