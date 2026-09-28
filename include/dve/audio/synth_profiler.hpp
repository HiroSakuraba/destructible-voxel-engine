// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace dve::audio {

// Snapshot of synth-wide performance counters, returned by Synthesizer::profiler().
// All cumulative values are monotonic within a profiler epoch (see reset_profiler());
// activeVoices is a point-in-time reading taken at the last render call.
struct SynthProfiler {
    std::uint64_t renderCalls{};            // render() invocations
    std::uint64_t samplesRendered{};        // total audio frames rendered
    std::uint64_t voicesStarted{};          // note-ons that allocated a voice
    std::uint64_t voicesStolen{};           // allocations that stole an active voice
    std::uint64_t voicesRetired{};          // voices that went idle after release
    std::uint64_t oscillatorVoiceFrames{};  // active-voice frames through the oscillator stage
    std::uint64_t filterFrames{};           // frames with the filter stage processing audio
    std::uint64_t fxFrames{};               // frames with any FX stage processing audio
    std::uint64_t arpSteps{};               // arpeggiator steps triggered
    std::uint64_t droppedEvents{};          // dropped MIDI / scheduled voice events
    std::uint32_t activeVoices{};            // active voices at the last render call
    std::uint32_t maximumActiveVoices{};    // high-water mark of active voices
};

}  // namespace dve::audio
