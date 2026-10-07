#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#include <stdexcept>

#include "dve/audio/sdl_synth_audio_device.hpp"
#include "dve/audio/audio_device_capture.hpp"
#include "dve/audio/audio_recorder.hpp"

int main() {
    try {
        SDLTest_Reset();
        dve::audio::AudioMixer mixer;
        std::string error;
        // A driver can ignore the request. Report the physical format and preserve
        // the SDL hint that belonged to the host before this device was opened.
        (void)SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "2048");
        SDLTest_SetAudioDevice(44100, 512, false, false);
        {
            dve::audio::SdlSynthAudioDevice requested(mixer, {128}, &error);
            const auto status = requested.status();
            if (!requested.valid() || !status.deviceFormatKnown || status.engineSampleRate != 48000 ||
                status.deviceSampleRate != 44100 || status.requestedBufferFrames != 128 ||
                status.deviceBufferFrames != 512 || !status.bufferHintAccepted ||
                std::string(SDLTest_AudioBufferHintAtOpen()) != "128" ||
                std::string(SDL_GetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES)) != "2048")
                throw std::runtime_error("device request/actual format or hint restoration failed");
            SDLTest_RequestAudio(17); // partial final stereo frame must not loop or overrun
            SDLTest_RequestAudio(65560); // exceeds the fixed callback scratch capacity
        }
        SDLTest_SetAudioDevice(44100, 512, true, false);
        {
            dve::audio::SdlSynthAudioDevice unavailable(mixer, {64}, &error);
            if (unavailable.valid() || error.empty() ||
                std::string(SDL_GetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES)) != "2048")
                throw std::runtime_error("failed device open lost previous buffer hint");
        }
        (void)SDL_ResetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES);
        SDLTest_SetAudioDevice(44100, 512, false, true);
        {
            dve::audio::SdlSynthAudioDevice unknown(mixer, {64}, &error);
            if (!unknown.valid() || unknown.status().deviceFormatKnown ||
                unknown.status().deviceSampleRate != 0 || unknown.status().deviceBufferFrames != 0 ||
                SDL_GetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES) != nullptr)
                throw std::runtime_error("unavailable format query reported invented hardware values");
        }
        SDLTest_SetAudioDevice(48000, 256, false, false);
        if (!mixer.synthesizer().note_on(60, 0.9F)) throw std::runtime_error("could not queue test note");
        dve::audio::SdlSynthAudioDevice device(mixer, &error);
        if (!device.valid()) throw std::runtime_error(error);
        SDLTest_RequestAudio(16384);
        std::size_t sampleCount = 0;
        const float* samples = SDLTest_AudioData(&sampleCount);
        if (samples == nullptr || sampleCount < 2048U) throw std::runtime_error("SDL callback produced no audio");
        if (!std::all_of(samples, samples + sampleCount, [](float sample) { return std::isfinite(sample); }))
            throw std::runtime_error("SDL callback produced non-finite samples");
        const float peak = *std::max_element(samples, samples + sampleCount);
        if (!(peak > 0.001F)) throw std::runtime_error("SDL callback produced silence");

        const auto inputs = dve::audio::SdlAudioInputDevice::enumerate(&error);
        if (inputs.size() < 3U || !inputs.front().systemDefault)
            throw std::runtime_error("SDL recording-device enumeration failed");
        auto recorder = std::make_shared<dve::audio::AudioTakeRecorder>(48000U, 2U, 32768U);
        if (!recorder->start("microphone")) throw std::runtime_error("recording sink did not start");
        dve::audio::SdlAudioInputDevice input(recorder, 48000U, 2U, 101U, 48000U, &error);
        if (!input.valid()) throw std::runtime_error(error);
        std::vector<float> microphone(4096U);
        for (std::size_t i = 0U; i < microphone.size(); ++i)
            microphone[i] = 0.25F * std::sin(6.283185307179586F * 330.0F *
                                             static_cast<float>(i / 2U) / 48000.0F);
        SDLTest_PushRecordingData(microphone.data(), microphone.size());
        for (int attempt = 0; attempt < 100 && input.telemetry().deliveredFrames < 1000U; ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const auto inputTelemetry = input.telemetry();
        auto take = recorder->stop(&error);
        if (!take || take->metadata.frameCount < 1000U || take->metadata.peakLinear < 0.1F ||
            inputTelemetry.callbackFrames < 1000U || inputTelemetry.droppedFrames != 0U)
            throw std::runtime_error("SDL recording device did not deliver the microphone take");
        std::cout << "dve_sdl_synth_audio_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_sdl_synth_audio_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
