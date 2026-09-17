#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

#include "dve/audio/acoustics.hpp"
#include "dve/audio/destruction_audio.hpp"
#include "dve/audio/event_graph.hpp"
#include "dve/audio/mixer.hpp"

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

dve::audio::ResidentSampleDesc make_tone(float frequency, float seconds, std::uint32_t rate = 48000U) {
    dve::audio::ResidentSampleDesc result;
    result.name = "test tone";
    result.sampleRate = rate;
    result.channels = 1;
    const std::size_t frames = static_cast<std::size_t>(seconds * static_cast<float>(rate));
    result.samples.resize(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(rate);
        result.samples[i] = std::sin(2.0F * std::numbers::pi_v<float> * frequency * t) * 0.25F;
    }
    return result;
}
}

int main() {
    using namespace dve::audio;
    try {
        AudioMixer mixer(48000U);
        std::string error;
        const SampleId tone = mixer.register_resident_sample(make_tone(220.0F, 0.4F), &error);
        require(static_cast<bool>(tone), error.c_str());
        require(mixer.sample_name(tone) == "test tone", "sample registry lost name");

        AudioListenerState listener;
        listener.position = {0.0F, 0.0F, 0.0F};
        mixer.set_listener(listener);
        PlaySampleDesc play;
        play.sample = tone;
        play.emitter.position = {2.0F, 0.0F, -2.0F};
        play.priority = AudioPriority::Hero;
        const AudioSourceHandle source = mixer.play_sample(play);
        require(static_cast<bool>(source), "could not queue resident sample");
        require(mixer.synthesizer().note_on(60, 0.65F), "could not queue synth note through mixer");

        std::vector<float> audio(4096U * 2U);
        mixer.render(audio);
        require(std::all_of(audio.begin(), audio.end(), [](float value) { return std::isfinite(value); }),
                "mixer emitted non-finite audio");
        const float peak = *std::max_element(audio.begin(), audio.end());
        require(peak > 0.005F, "mixer emitted silence");
        require(mixer.meters().synthVoices > 0U, "synth did not become a mixer source");

        // Exceed the physical voice budget. Logical voices must continue their timelines while
        // only the highest-scoring subset consumes sample-mixing work.
        for (int i = 0; i < 80; ++i) {
            play.priority = i < 4 ? AudioPriority::Critical : AudioPriority::Background;
            play.emitter.position = {static_cast<float>(i % 10), 0.0F, -3.0F - static_cast<float>(i / 10)};
            play.loop = true;
            require(static_cast<bool>(mixer.play_sample(play)), "voice command queue unexpectedly full");
        }
        mixer.render(audio);
        const AudioMixerMeters overloaded = mixer.meters();
        require(overloaded.logicalSampleVoices >= 80U, "logical voice count missing");
        require(overloaded.physicalSampleVoices == kMaxPhysicalSampleVoices, "physical voice budget not enforced");
        require(overloaded.virtualSampleVoices > 0U, "voice virtualization did not activate");

        // Compile and execute an immutable graph with layering, delay, random/no-repeat and a
        // parameter switch. Execution produces bounded actions rather than allocating graph
        // nodes in the callback.
        AudioEventGraph graph;
        graph.name = "test/destruction";
        graph.root = 8U;
        graph.nodes.resize(9U);
        graph.nodes[0].type = AudioEventNodeType::Sample; graph.nodes[0].sample = tone;
        graph.nodes[1].type = AudioEventNodeType::Sample; graph.nodes[1].sample = tone; graph.nodes[1].value = 0.8F;
        graph.nodes[2].type = AudioEventNodeType::RandomNoRepeat; graph.nodes[2].children = {0U, 1U};
        graph.nodes[3].type = AudioEventNodeType::SynthNote; graph.nodes[3].note = 48U; graph.nodes[3].durationSeconds = 0.1F;
        graph.nodes[4].type = AudioEventNodeType::Delay; graph.nodes[4].value = 0.02F; graph.nodes[4].children = {3U};
        graph.nodes[5].type = AudioEventNodeType::Layer; graph.nodes[5].children = {2U, 4U};
        graph.nodes[6].type = AudioEventNodeType::Gain; graph.nodes[6].value = 0.5F; graph.nodes[6].children = {0U};
        graph.nodes[7].type = AudioEventNodeType::Switch; graph.nodes[7].parameter = "large"; graph.nodes[7].threshold = 0.5F; graph.nodes[7].children = {6U, 5U};
        graph.nodes[8].type = AudioEventNodeType::Bus; graph.nodes[8].bus = AudioBusId::Effects; graph.nodes[8].children = {7U};
        auto compiled = compile_audio_event(graph, &error);
        require(compiled.has_value(), error.c_str());
        AudioEventParameters parameters; parameters.values["large"] = 1.0F;
        AudioEventInstanceState eventState;
        const AudioEventExecution execution = execute_audio_event(*compiled, parameters, eventState, 48000U);
        require(!execution.truncated && execution.count == 3U, "compiled event produced wrong action count");
        require(execution.actions[1].frameOffset <= execution.actions[2].frameOffset, "event actions are not scheduled");
        dispatch_audio_event(execution, mixer, play.emitter);
        mixer.render(audio);

        DestructionAudioCompiler destruction(1.5F, 0.12F);
        for (int i = 0; i < 1000; ++i) {
            DestructionStimulus stimulus;
            stimulus.timeSeconds = static_cast<double>(i % 8) * 0.005;
            stimulus.position = {static_cast<float>(i % 5) * 0.05F, 0.0F, 0.0F};
            stimulus.material = 3U;
            stimulus.removedVolume = 0.003F;
            stimulus.fractureArea = 0.02F;
            stimulus.impulse = 0.4F;
            stimulus.mass = 0.02F;
            destruction.submit(stimulus);
        }
        const DestructionCompileResult collapsed = destruction.flush(1.0, true);
        require(collapsed.count == 1U, "destruction stimuli were not spatial-temporally aggregated");
        require(collapsed.events[0].stimulusCount == 1000U, "destruction compiler lost clustered stimuli");
        require(destruction_layer_recipe(collapsed.events[0]).debris > 0.5F, "destruction layer recipe ignored density");

        CoarseAcousticGrid grid(16U, 8U, 16U, 0.5F, {-4.0F, -2.0F, -4.0F});
        for (std::uint32_t y = 0; y < 8U; ++y)
            for (std::uint32_t z = 0; z < 16U; ++z)
                require(grid.set_cell(8U, y, z, {255U, 1U, 0U, 0U}), "could not populate acoustic wall");
        const AcousticTraceResult trace = grid.trace({-2.0F, 0.0F, 0.0F}, {2.0F, 0.0F, 0.0F});
        require(trace.occlusion > 0.1F && trace.transmission < 0.5F && trace.wallThicknessMeters > 0.0F,
                "coarse acoustic wall was not detected");
        GridAwareSpatializer gridSpatializer(&grid);
        const SpatializationResult blocked = gridSpatializer.spatialize(listener, AudioEmitterState{{2.0F, 0.0F, 0.0F}});
        require(blocked.lowPassHertz < 19000.0F, "grid-aware spatializer did not filter obstruction");

        RoomPortalGraph rooms;
        require(rooms.add_room({1U, "Interior", 100.0F, 0.3F, false}, &error), error.c_str());
        require(rooms.add_room({2U, "Exterior", 10000.0F, 0.0F, true}, &error), error.c_str());
        require(rooms.add_portal({1U, 1U, 2U, 2.0F, 0.0F, 0.9F}, &error), error.c_str());
        require(rooms.path_transmission(1U, 2U) == 0.0F, "closed portal transmitted sound");
        require(rooms.set_portal_openness(1U, 1.0F), "portal openness update failed");
        require(rooms.path_transmission(1U, 2U) > 0.8F, "open portal did not transmit sound");

        mixer.all_sounds_off();
        mixer.render(audio);
        std::cout << "dve_audio_runtime_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_audio_runtime_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
