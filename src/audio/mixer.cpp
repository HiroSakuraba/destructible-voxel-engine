#include "dve/audio/mixer.hpp"
#include "dve/audio/chiptune.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <chrono>
#include <limits>
#include <type_traits>

namespace dve::audio {
namespace {

constexpr std::size_t kCommandCapacity = 4096;
constexpr std::size_t kRenderQuantum = 1024;
constexpr float kPi = 3.14159265358979323846F;

float clampf(float value, float low, float high) noexcept { return std::clamp(value, low, high); }

std::uint32_t float_bits(float value) noexcept { return std::bit_cast<std::uint32_t>(value); }
float bits_float(std::uint32_t value) noexcept { return std::bit_cast<float>(value); }

template <class T, std::size_t Capacity>
class BoundedQueue {
    static_assert(std::is_trivially_copyable_v<T>);
    struct Cell { std::atomic<std::size_t> sequence{}; T value{}; };
public:
    BoundedQueue() noexcept {
        for (std::size_t i = 0; i < Capacity; ++i) cells_[i].sequence.store(i, std::memory_order_relaxed);
    }
    bool push(const T& value) noexcept {
        Cell* cell = nullptr;
        std::size_t position = enqueue_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[position % Capacity];
            const std::size_t sequence = cell->sequence.load(std::memory_order_acquire);
            const std::intptr_t difference = static_cast<std::intptr_t>(sequence) - static_cast<std::intptr_t>(position);
            if (difference == 0) {
                if (enqueue_.compare_exchange_weak(position, position + 1U, std::memory_order_relaxed)) break;
            } else if (difference < 0) return false;
            else position = enqueue_.load(std::memory_order_relaxed);
        }
        cell->value = value;
        cell->sequence.store(position + 1U, std::memory_order_release);
        return true;
    }
    bool pop(T& value) noexcept {
        Cell* cell = nullptr;
        std::size_t position = dequeue_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[position % Capacity];
            const std::size_t sequence = cell->sequence.load(std::memory_order_acquire);
            const std::intptr_t difference = static_cast<std::intptr_t>(sequence) - static_cast<std::intptr_t>(position + 1U);
            if (difference == 0) {
                if (dequeue_.compare_exchange_weak(position, position + 1U, std::memory_order_relaxed)) break;
            } else if (difference < 0) return false;
            else position = dequeue_.load(std::memory_order_relaxed);
        }
        value = cell->value;
        cell->sequence.store(position + Capacity, std::memory_order_release);
        return true;
    }
private:
    std::array<Cell, Capacity> cells_{};
    alignas(64) std::atomic<std::size_t> enqueue_{};
    alignas(64) std::atomic<std::size_t> dequeue_{};
};

enum class CommandType : std::uint8_t { Play, PlayStream, Stop, Emitter, Gain, Bus, Snapshot, Listener, AllOff };
struct Command {
    CommandType type{};
    AudioSourceHandle handle{};
    PlaySampleDesc play{};
    PlayStreamDesc playStream{};
    AudioEmitterState emitter{};
    AudioBusId bus{};
    AudioBusParameters busParameters{};
    AudioBusSnapshot snapshot{};
    AudioListenerState listener{};
    float value{};
};
static_assert(std::is_trivially_copyable_v<Command>);

struct SampleAsset {
    std::string name;
    std::uint32_t sampleRate{};
    std::uint8_t channels{};
    std::vector<float> samples;
    [[nodiscard]] std::size_t frame_count() const noexcept {
        return channels == 0 ? 0U : samples.size() / channels;
    }
};

struct StreamAsset {
    std::string name;
    std::unique_ptr<CookedAudioStream> stream;
    bool hasPlayed{}; // audio-thread owned after registration
};

struct StreamVoice {
    bool active{};
    bool stopping{};
    AudioSourceHandle handle{};
    StreamSampleId sample{};
    AudioBusId bus{AudioBusId::Music};
    float gain{1.0F};
    float fadeGain{1.0F};
    float fadeStep{};
    bool loop{};
    std::uint64_t startFrame{};
};

struct SampleVoice {
    bool active{};
    bool physical{};
    bool stopping{};
    AudioSourceHandle handle{};
    SampleId sample{};
    AudioBusId bus{AudioBusId::Effects};
    AudioPriority priority{AudioPriority::Normal};
    AudioEmitterState emitter{};
    double cursor{};
    float gain{1.0F};
    float pitch{1.0F};
    float fadeGain{1.0F};
    float fadeStep{};
    bool loop{};
    bool spatialized{true};
    std::uint64_t startFrame{};
    std::uint64_t age{};
};

struct OnePole {
    float left{};
    float right{};
    void process(float& l, float& r, float cutoff, float sampleRate) noexcept {
        const float normalized = clampf(cutoff / std::max(1.0F, sampleRate), 0.0001F, 0.45F);
        const float coefficient = 1.0F - std::exp(-2.0F * kPi * normalized);
        left += coefficient * (l - left);
        right += coefficient * (r - right);
        l = left; r = right;
    }
};

struct ReverbNetwork {
    static constexpr std::array<std::size_t, 4> lengths{1499, 1601, 1867, 1999};
    std::array<std::array<float, 2048>, 4> left{};
    std::array<std::array<float, 2048>, 4> right{};
    std::array<std::size_t, 4> positions{};
    void process(float inputL, float inputR, float& outL, float& outR) noexcept {
        float sumL{};
        float sumR{};
        for (std::size_t i = 0; i < lengths.size(); ++i) {
            const std::size_t p = positions[i];
            const float delayedL = left[i][p];
            const float delayedR = right[i][p];
            left[i][p] = inputL + delayedL * (0.68F - static_cast<float>(i) * 0.035F);
            right[i][p] = inputR + delayedR * (0.66F - static_cast<float>(i) * 0.03F);
            positions[i] = (p + 1U) % lengths[i];
            sumL += delayedL;
            sumR += delayedR;
        }
        outL = sumL * 0.25F;
        outR = sumR * 0.25F;
    }
};

} // namespace

struct AudioMixer::Impl {
    explicit Impl(std::uint32_t rate)
        : sampleRate(std::max<std::uint32_t>(8000U, rate)), synth(sampleRate),
          spatializer(std::make_shared<AnalyticSpatializer>()) {
        for (auto& bus : controlBuses) bus = {};
        controlBuses[audio_bus_index(AudioBusId::Master)].gain = 0.90F;
        controlBuses[audio_bus_index(AudioBusId::Music)].gain = 0.85F;
        controlBuses[audio_bus_index(AudioBusId::Dialogue)].gain = 1.0F;
        controlBuses[audio_bus_index(AudioBusId::Effects)].gain = 0.92F;
        controlBuses[audio_bus_index(AudioBusId::Ambience)].gain = 0.75F;
        controlBuses[audio_bus_index(AudioBusId::UserInterface)].gain = 0.90F;
        controlBuses[audio_bus_index(AudioBusId::Reverb)].gain = 0.35F;
        controlBuses[audio_bus_index(AudioBusId::Effects)].reverbSend = 0.08F;
        controlBuses[audio_bus_index(AudioBusId::Ambience)].reverbSend = 0.25F;
        renderBuses = controlBuses;
    }

    std::uint32_t sampleRate{};
    Synthesizer synth;
    std::atomic<std::shared_ptr<ChiptunePlayer>> chiptunePreview{};
    std::atomic<AudioBusId> chiptunePreviewBus{AudioBusId::Music};
    std::array<std::unique_ptr<SampleAsset>, kMaxResidentSamples> samples{};
    std::size_t sampleCount{};
    std::array<std::unique_ptr<StreamAsset>, kMaxStreamedSamples> streams{};
    std::size_t streamCount{};
    std::array<SampleVoice, kMaxLogicalSampleVoices> voices{};
    std::array<StreamVoice, kMaxStreamVoices> streamVoices{};
    BoundedQueue<Command, kCommandCapacity> commands;
    std::atomic<std::uint32_t> nextHandle{1U};
    std::atomic<std::uint64_t> droppedCommands{};
    std::uint64_t frame{};
    std::uint64_t ageCounter{};
    AudioListenerState listener{};
    std::array<AudioBusParameters, kAudioBusCount> controlBuses{};
    std::array<AudioBusParameters, kAudioBusCount> renderBuses{};
    std::atomic<std::shared_ptr<const IAudioSpatializer>> spatializer;
    std::array<std::atomic<std::shared_ptr<IAudioCaptureSink>>, kMaxAudioCaptureSinks> captureSinks{};
    std::array<std::atomic<AudioCaptureTap>, kMaxAudioCaptureSinks> captureTaps{};
    std::array<AudioBusParameters, kAudioBusCount> snapshotStart{};
    std::array<AudioBusParameters, kAudioBusCount> snapshotTarget{};
    std::uint64_t snapshotFramesTotal{};
    std::uint64_t snapshotFramesRemaining{};

    std::array<std::array<float, kRenderQuantum * 2U>, kAudioBusCount> busScratch{};
    std::array<float, kRenderQuantum * 2U> synthScratch{};
    std::array<float, kRenderQuantum * 2U> chiptuneScratch{};
    std::array<float, kRenderQuantum * 2U> streamScratch{};
    std::array<OnePole, kAudioBusCount> busFilters{};
    ReverbNetwork reverb{};

    std::array<std::atomic<std::uint32_t>, kAudioBusCount * 4U> meterBits{};
    std::atomic<std::uint32_t> logicalVoices{};
    std::atomic<std::uint32_t> physicalVoices{};
    std::atomic<std::uint32_t> virtualVoices{};
    std::atomic<std::uint32_t> activeStreamVoices{};
    std::atomic<std::uint64_t> streamUnderrunFrames{};
    std::atomic<std::uint64_t> meterFrames{};
    std::atomic<std::uint64_t> stolenVoices{};
    std::atomic<std::uint32_t> lastStolenHandle{};
    std::atomic<std::uint32_t> lastStolenPriority{};
    std::atomic<std::uint64_t> renderCalls{};
    std::atomic<std::uint64_t> renderNanoseconds{};
    std::atomic<std::uint64_t> renderLastNanoseconds{};
    std::atomic<std::uint64_t> renderMaxNanoseconds{};
    std::array<std::atomic<std::uint64_t>, 64> renderHistogram{};

    bool post(const Command& command) noexcept {
        if (commands.push(command)) return true;
        droppedCommands.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }

    void capture(AudioCaptureTap tap, const float* capturedSamples, std::size_t frames) noexcept {
        if (capturedSamples == nullptr || frames == 0U) return;
        for (std::size_t slot = 0; slot < captureSinks.size(); ++slot) {
            if (captureTaps[slot].load(std::memory_order_relaxed) != tap) continue;
            const auto sink = captureSinks[slot].load(std::memory_order_acquire);
            if (sink) sink->capture_interleaved(
                std::span<const float>(capturedSamples, frames * 2U), frame);
        }
    }

    SampleVoice* find_voice(AudioSourceHandle handle) noexcept {
        for (auto& voice : voices) if (voice.active && voice.handle == handle) return &voice;
        return nullptr;
    }

    StreamVoice* find_stream_voice(AudioSourceHandle handle) noexcept {
        for (auto& voice : streamVoices) if (voice.active && voice.handle == handle) return &voice;
        return nullptr;
    }

    const SampleAsset* asset(SampleId id) const noexcept {
        if (!id || id.value > sampleCount) return nullptr;
        return samples[id.value - 1U].get();
    }

    StreamAsset* stream_asset(StreamSampleId id) noexcept {
        if (!id || id.value > streamCount) return nullptr;
        return streams[id.value - 1U].get();
    }
    const StreamAsset* stream_asset(StreamSampleId id) const noexcept {
        if (!id || id.value > streamCount) return nullptr;
        return streams[id.value - 1U].get();
    }

    void start_voice(const PlaySampleDesc& desc, AudioSourceHandle handle) noexcept {
        const SampleAsset* sample = asset(desc.sample);
        if (sample == nullptr || sample->frame_count() == 0U) return;
        SampleVoice* slot = nullptr;
        for (auto& voice : voices) if (!voice.active) { slot = &voice; break; }
        if (slot == nullptr) {
            slot = &*std::min_element(voices.begin(), voices.end(), [](const SampleVoice& a, const SampleVoice& b) {
                if (a.priority != b.priority) return static_cast<unsigned>(a.priority) < static_cast<unsigned>(b.priority);
                return a.age < b.age;
            });
            stolenVoices.fetch_add(1U, std::memory_order_relaxed);
            lastStolenHandle.store(slot->handle.value, std::memory_order_relaxed);
            lastStolenPriority.store(static_cast<std::uint32_t>(slot->priority), std::memory_order_relaxed);
        }
        *slot = {};
        slot->active = true;
        slot->handle = handle;
        slot->sample = desc.sample;
        slot->bus = desc.bus;
        slot->priority = desc.priority;
        slot->emitter = desc.emitter;
        slot->gain = clampf(desc.gain, 0.0F, 8.0F);
        slot->pitch = clampf(desc.pitch, 0.125F, 8.0F);
        slot->loop = desc.loop;
        slot->spatialized = desc.spatialized;
        slot->startFrame = desc.sampleFrame == 0U ? frame : desc.sampleFrame;
        slot->age = ++ageCounter;
    }

    void start_stream_voice(const PlayStreamDesc& desc, AudioSourceHandle handle) noexcept {
        StreamAsset* asset = stream_asset(desc.sample);
        if (asset == nullptr || !asset->stream || !asset->stream->valid()) return;
        // A decoder/ring has a single cursor. Restarting the same stream replaces its prior voice.
        for (auto& voice : streamVoices) {
            if (voice.active && voice.sample == desc.sample) voice.active = false;
        }
        StreamVoice* slot = nullptr;
        for (auto& voice : streamVoices) if (!voice.active) { slot = &voice; break; }
        if (slot == nullptr) slot = &streamVoices.front();
        *slot = {};
        slot->active = true;
        slot->handle = handle;
        slot->sample = desc.sample;
        slot->bus = desc.bus;
        slot->gain = clampf(desc.gain, 0.0F, 8.0F);
        slot->loop = desc.loop;
        slot->startFrame = desc.sampleFrame == 0U ? frame : desc.sampleFrame;
        asset->stream->set_looping(desc.loop);
        if (asset->hasPlayed) asset->stream->request_rewind();
        asset->hasPlayed = true;
        asset->stream->set_running(true);
    }

    void process_commands() noexcept {
        Command command;
        while (commands.pop(command)) {
            switch (command.type) {
                case CommandType::Play: start_voice(command.play, command.handle); break;
                case CommandType::PlayStream: start_stream_voice(command.playStream, command.handle); break;
                case CommandType::Stop: {
                    if (SampleVoice* voice = find_voice(command.handle)) {
                        if (command.value <= 0.0F) voice->active = false;
                        else {
                            voice->stopping = true;
                            voice->fadeStep = 1.0F / std::max(1.0F, command.value * static_cast<float>(sampleRate));
                        }
                    } else if (StreamVoice* streamVoice = find_stream_voice(command.handle)) {
                        if (command.value <= 0.0F) streamVoice->active = false;
                        else {
                            streamVoice->stopping = true;
                            streamVoice->fadeStep = 1.0F / std::max(1.0F, command.value * static_cast<float>(sampleRate));
                        }
                    }
                    break;
                }
                case CommandType::Emitter:
                    if (SampleVoice* voice = find_voice(command.handle)) voice->emitter = command.emitter;
                    break;
                case CommandType::Gain:
                    if (SampleVoice* voice = find_voice(command.handle)) voice->gain = clampf(command.value, 0.0F, 8.0F);
                    else if (StreamVoice* streamVoice = find_stream_voice(command.handle)) streamVoice->gain = clampf(command.value, 0.0F, 8.0F);
                    break;
                case CommandType::Bus:
                    renderBuses[audio_bus_index(command.bus)] = command.busParameters;
                    snapshotFramesRemaining = 0U;
                    break;
                case CommandType::Snapshot:
                    snapshotStart = renderBuses;
                    snapshotTarget = command.snapshot.buses;
                    snapshotFramesTotal = std::max<std::uint64_t>(1U, static_cast<std::uint64_t>(
                        std::max(0.0F, command.value) * static_cast<float>(sampleRate)));
                    snapshotFramesRemaining = snapshotFramesTotal;
                    break;
                case CommandType::Listener: listener = command.listener; break;
                case CommandType::AllOff:
                    for (auto& voice : voices) voice.active = false;
                    for (auto& voice : streamVoices) voice.active = false;
                    for (auto& asset : streams) if (asset && asset->stream) asset->stream->set_running(false);
                    synth.all_notes_off(true);
                    break;
            }
        }
    }

    void advance_snapshot(std::size_t frames) noexcept {
        if (snapshotFramesRemaining == 0U) return;
        const std::uint64_t progressed = snapshotFramesTotal - snapshotFramesRemaining;
        const std::uint64_t nextProgressed = std::min(snapshotFramesTotal, progressed + frames);
        const float t = static_cast<float>(nextProgressed) / static_cast<float>(snapshotFramesTotal);
        for (std::size_t i = 0; i < kAudioBusCount; ++i) {
            renderBuses[i].gain = snapshotStart[i].gain + (snapshotTarget[i].gain - snapshotStart[i].gain) * t;
            renderBuses[i].lowPassHertz = snapshotStart[i].lowPassHertz +
                (snapshotTarget[i].lowPassHertz - snapshotStart[i].lowPassHertz) * t;
            renderBuses[i].reverbSend = snapshotStart[i].reverbSend +
                (snapshotTarget[i].reverbSend - snapshotStart[i].reverbSend) * t;
            renderBuses[i].mute = t >= 1.0F ? snapshotTarget[i].mute : snapshotStart[i].mute;
        }
        snapshotFramesRemaining = snapshotFramesTotal - nextProgressed;
    }

    void classify_voices() noexcept {
        struct Candidate { std::size_t index{}; float score{}; };
        std::array<Candidate, kMaxLogicalSampleVoices> candidates{};
        std::size_t count{};
        for (std::size_t i = 0; i < voices.size(); ++i) {
            auto& voice = voices[i];
            if (!voice.active || frame < voice.startFrame) { voice.physical = false; continue; }
            const float dx = voice.emitter.position.x - listener.position.x;
            const float dy = voice.emitter.position.y - listener.position.y;
            const float dz = voice.emitter.position.z - listener.position.z;
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            const float audibility = voice.spatialized ? 1.0F / (1.0F + distance) : 1.0F;
            candidates[count++] = {i, static_cast<float>(static_cast<unsigned>(voice.priority)) * voice.gain * audibility};
        }
        std::sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(count),
                  [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
        for (auto& voice : voices) voice.physical = false;
        const std::size_t physical = std::min<std::size_t>(count, kMaxPhysicalSampleVoices);
        for (std::size_t i = 0; i < physical; ++i) voices[candidates[i].index].physical = true;
        logicalVoices.store(static_cast<std::uint32_t>(count), std::memory_order_relaxed);
        physicalVoices.store(static_cast<std::uint32_t>(physical), std::memory_order_relaxed);
        virtualVoices.store(static_cast<std::uint32_t>(count - physical), std::memory_order_relaxed);
    }

    void render_chunk(float* output, std::size_t frames) noexcept {
        process_commands();
        advance_snapshot(frames);
        classify_voices();
        for (auto& bus : busScratch) std::fill_n(bus.data(), frames * 2U, 0.0F);

        std::fill_n(synthScratch.data(), frames * 2U, 0.0F);
        synth.render(synthScratch.data(), frames);
        capture(AudioCaptureTap::SynthDry, synthScratch.data(), frames);
        auto& music = busScratch[audio_bus_index(AudioBusId::Music)];
        for (std::size_t i = 0; i < frames * 2U; ++i) music[i] += synthScratch[i];

        const auto tracker = chiptunePreview.load(std::memory_order_acquire);
        if (tracker && tracker->playing() && !tracker->finished()) {
            std::fill_n(chiptuneScratch.data(), frames * 2U, 0.0F);
            tracker->render(chiptuneScratch.data(), frames);
            auto& trackerBus = busScratch[audio_bus_index(
                chiptunePreviewBus.load(std::memory_order_relaxed))];
            for (std::size_t i = 0; i < frames * 2U; ++i) trackerBus[i] += chiptuneScratch[i];
        }

        const auto activeSpatializer = spatializer.load(std::memory_order_acquire);
        for (auto& voice : voices) {
            if (!voice.active || frame + frames <= voice.startFrame) continue;
            const SampleAsset* sample = asset(voice.sample);
            if (sample == nullptr || sample->frame_count() == 0U) { voice.active = false; continue; }
            const SpatializationResult spatial = voice.spatialized && activeSpatializer
                ? activeSpatializer->spatialize(listener, voice.emitter)
                : SpatializationResult{};
            const double increment = static_cast<double>(voice.pitch * spatial.dopplerRatio) *
                                     static_cast<double>(sample->sampleRate) / static_cast<double>(sampleRate);
            auto& bus = busScratch[audio_bus_index(voice.bus)];
            auto& reverbBus = busScratch[audio_bus_index(AudioBusId::Reverb)];
            for (std::size_t f = 0; f < frames; ++f) {
                const std::uint64_t absoluteFrame = frame + f;
                if (absoluteFrame < voice.startFrame) continue;
                const std::size_t frameCount = sample->frame_count();
                if (voice.cursor >= static_cast<double>(frameCount)) {
                    if (voice.loop) voice.cursor = std::fmod(voice.cursor, static_cast<double>(frameCount));
                    else { voice.active = false; break; }
                }
                const std::size_t i0 = std::min<std::size_t>(static_cast<std::size_t>(voice.cursor), frameCount - 1U);
                const std::size_t i1 = voice.loop ? (i0 + 1U) % frameCount : std::min(i0 + 1U, frameCount - 1U);
                const float fraction = static_cast<float>(voice.cursor - static_cast<double>(i0));
                auto read = [&](std::size_t frameIndex, std::size_t channel) {
                    if (sample->channels == 1U) return sample->samples[frameIndex];
                    return sample->samples[frameIndex * 2U + channel];
                };
                float left = read(i0, 0U) + (read(i1, 0U) - read(i0, 0U)) * fraction;
                float right = read(i0, sample->channels == 1U ? 0U : 1U) +
                              (read(i1, sample->channels == 1U ? 0U : 1U) - read(i0, sample->channels == 1U ? 0U : 1U)) * fraction;
                if (voice.stopping) {
                    voice.fadeGain = std::max(0.0F, voice.fadeGain - voice.fadeStep);
                    if (voice.fadeGain <= 0.0F) { voice.active = false; break; }
                }
                const float gain = voice.gain * voice.fadeGain * spatial.distanceGain;
                if (voice.physical) {
                    if (voice.spatialized) {
                        const float mono = 0.5F * (left + right);
                        left = mono * spatial.leftGain;
                        right = mono * spatial.rightGain;
                    }
                    left *= gain; right *= gain;
                    bus[f * 2U] += left;
                    bus[f * 2U + 1U] += right;
                    const float send = clampf(spatial.reverbSend + renderBuses[audio_bus_index(voice.bus)].reverbSend, 0.0F, 1.0F);
                    reverbBus[f * 2U] += left * send;
                    reverbBus[f * 2U + 1U] += right * send;
                }
                voice.cursor += increment;
            }
        }

        std::uint32_t streamsActive{};
        std::uint64_t streamUnderruns{};
        for (auto& voice : streamVoices) {
            if (!voice.active || frame + frames <= voice.startFrame) continue;
            StreamAsset* asset = stream_asset(voice.sample);
            if (asset == nullptr || !asset->stream || !asset->stream->valid()) { voice.active = false; continue; }
            const auto& metadata = asset->stream->metadata();
            const std::size_t begin = voice.startFrame > frame
                ? static_cast<std::size_t>(voice.startFrame - frame) : 0U;
            const std::size_t requested = frames - std::min(begin, frames);
            std::fill_n(streamScratch.data(), requested * metadata.channels, 0.0F);
            const std::size_t read = asset->stream->read(streamScratch.data(), requested);
            auto& bus = busScratch[audio_bus_index(voice.bus)];
            for (std::size_t f = 0; f < requested; ++f) {
                if (voice.stopping) {
                    voice.fadeGain = std::max(0.0F, voice.fadeGain - voice.fadeStep);
                    if (voice.fadeGain <= 0.0F) { voice.active = false; break; }
                }
                const float left = streamScratch[f * metadata.channels];
                const float right = metadata.channels == 2U ? streamScratch[f * 2U + 1U] : left;
                const float gain = voice.gain * voice.fadeGain;
                bus[(begin + f) * 2U] += left * gain;
                bus[(begin + f) * 2U + 1U] += right * gain;
            }
            const auto telemetry = asset->stream->telemetry();
            streamUnderruns += telemetry.underrunFrames;
            if (!voice.loop && telemetry.endOfStream && telemetry.bufferedFrames == 0U && read == 0U) {
                voice.active = false;
                asset->stream->set_running(false);
            }
            if (voice.active) ++streamsActive;
        }
        activeStreamVoices.store(streamsActive, std::memory_order_relaxed);
        streamUnderrunFrames.store(streamUnderruns, std::memory_order_relaxed);

        // Dialogue-driven ducking protects speech without requiring a sidechain graph.
        float dialogueEnergy{};
        const auto& dialogue = busScratch[audio_bus_index(AudioBusId::Dialogue)];
        for (std::size_t i = 0; i < frames * 2U; ++i) dialogueEnergy += dialogue[i] * dialogue[i];
        const float dialogueRms = std::sqrt(dialogueEnergy / std::max(1.0F, static_cast<float>(frames * 2U)));
        const float duck = 1.0F - std::clamp((dialogueRms - 0.03F) * 8.0F, 0.0F, 0.65F);

        auto& master = busScratch[audio_bus_index(AudioBusId::Master)];
        for (std::size_t busIndex = 1U; busIndex < kAudioBusCount - 1U; ++busIndex) {
            const AudioBusParameters params = renderBuses[busIndex];
            const float busGain = params.mute ? 0.0F : params.gain *
                (busIndex == audio_bus_index(AudioBusId::Music) || busIndex == audio_bus_index(AudioBusId::Ambience) ? duck : 1.0F);
            auto& source = busScratch[busIndex];
            for (std::size_t f = 0; f < frames; ++f) {
                float l = source[f * 2U] * busGain;
                float r = source[f * 2U + 1U] * busGain;
                busFilters[busIndex].process(l, r, params.lowPassHertz, static_cast<float>(sampleRate));
                source[f * 2U] = l; source[f * 2U + 1U] = r;
                master[f * 2U] += l; master[f * 2U + 1U] += r;
            }
        }

        auto& reverbInput = busScratch[audio_bus_index(AudioBusId::Reverb)];
        const AudioBusParameters reverbParams = renderBuses[audio_bus_index(AudioBusId::Reverb)];
        for (std::size_t f = 0; f < frames; ++f) {
            float wetL{}; float wetR{};
            reverb.process(reverbInput[f * 2U], reverbInput[f * 2U + 1U], wetL, wetR);
            master[f * 2U] += wetL * reverbParams.gain;
            master[f * 2U + 1U] += wetR * reverbParams.gain;
        }

        const float masterGain = renderBuses[audio_bus_index(AudioBusId::Master)].mute ? 0.0F :
                                 renderBuses[audio_bus_index(AudioBusId::Master)].gain;
        for (std::size_t f = 0; f < frames; ++f) {
            const float l = std::tanh(master[f * 2U] * masterGain);
            const float r = std::tanh(master[f * 2U + 1U] * masterGain);
            output[f * 2U] = l;
            output[f * 2U + 1U] = r;
            master[f * 2U] = l;
            master[f * 2U + 1U] = r;
        }

        capture(AudioCaptureTap::MasterPost, master.data(), frames);
        capture(AudioCaptureTap::MusicPost,
                busScratch[audio_bus_index(AudioBusId::Music)].data(), frames);
        capture(AudioCaptureTap::DialoguePost,
                busScratch[audio_bus_index(AudioBusId::Dialogue)].data(), frames);
        capture(AudioCaptureTap::EffectsPost,
                busScratch[audio_bus_index(AudioBusId::Effects)].data(), frames);
        capture(AudioCaptureTap::AmbiencePost,
                busScratch[audio_bus_index(AudioBusId::Ambience)].data(), frames);
        capture(AudioCaptureTap::UserInterfacePost,
                busScratch[audio_bus_index(AudioBusId::UserInterface)].data(), frames);
        capture(AudioCaptureTap::ReverbInput, reverbInput.data(), frames);

        for (std::size_t busIndex = 0; busIndex < kAudioBusCount; ++busIndex) {
            float peakL{}; float peakR{}; float energyL{}; float energyR{};
            const auto& data = busScratch[busIndex];
            for (std::size_t f = 0; f < frames; ++f) {
                const float l = data[f * 2U]; const float r = data[f * 2U + 1U];
                peakL = std::max(peakL, std::abs(l)); peakR = std::max(peakR, std::abs(r));
                energyL += l * l; energyR += r * r;
            }
            meterBits[busIndex * 4U].store(float_bits(peakL), std::memory_order_relaxed);
            meterBits[busIndex * 4U + 1U].store(float_bits(peakR), std::memory_order_relaxed);
            meterBits[busIndex * 4U + 2U].store(float_bits(std::sqrt(energyL / std::max(1.0F, static_cast<float>(frames)))), std::memory_order_relaxed);
            meterBits[busIndex * 4U + 3U].store(float_bits(std::sqrt(energyR / std::max(1.0F, static_cast<float>(frames)))), std::memory_order_relaxed);
        }
        frame += frames;
        meterFrames.store(frame, std::memory_order_relaxed);
    }
};

AudioMixer::AudioMixer(std::uint32_t sampleRate) : impl_(std::make_unique<Impl>(sampleRate)) {}
AudioMixer::~AudioMixer() = default;
std::uint32_t AudioMixer::sample_rate() const noexcept { return impl_->sampleRate; }
std::uint64_t AudioMixer::current_frame() const noexcept { return impl_->meterFrames.load(std::memory_order_relaxed); }
Synthesizer& AudioMixer::synthesizer() noexcept { return impl_->synth; }
const Synthesizer& AudioMixer::synthesizer() const noexcept { return impl_->synth; }

SampleId AudioMixer::register_resident_sample(ResidentSampleDesc sample, std::string* error) {
    if (impl_->sampleCount >= kMaxResidentSamples) {
        if (error) *error = "resident sample capacity exhausted";
        return {};
    }
    if (sample.channels != 1U && sample.channels != 2U) {
        if (error) *error = "resident sample must be mono or stereo";
        return {};
    }
    if (sample.sampleRate < 8000U || sample.sampleRate > 384000U || sample.samples.empty() ||
        sample.samples.size() % sample.channels != 0U) {
        if (error) *error = "resident sample has invalid rate or frame data";
        return {};
    }
    if (!std::all_of(sample.samples.begin(), sample.samples.end(), [](float value) { return std::isfinite(value); })) {
        if (error) *error = "resident sample contains non-finite values";
        return {};
    }
    auto asset = std::make_unique<SampleAsset>();
    asset->name = std::move(sample.name);
    asset->sampleRate = sample.sampleRate;
    asset->channels = sample.channels;
    asset->samples = std::move(sample.samples);
    impl_->samples[impl_->sampleCount] = std::move(asset);
    ++impl_->sampleCount;
    return {static_cast<std::uint32_t>(impl_->sampleCount)};
}

std::string_view AudioMixer::sample_name(SampleId sample) const noexcept {
    const SampleAsset* asset = impl_->asset(sample);
    return asset ? std::string_view(asset->name) : std::string_view{};
}

bool AudioMixer::start_chiptune_preview(const ChipSong& song, AudioBusId bus, bool loop,
                                         std::string* error) {
    if (song.sampleRate != impl_->sampleRate) {
        if (error) *error = "chiptune preview sample rate must match the mixer";
        return false;
    }
    ChipSong previewSong = song;
    previewSong.loop = loop;
    auto preview = std::make_shared<ChiptunePlayer>();
    if (!preview->load(previewSong, error)) return false;
    preview->set_playing(true);
    impl_->chiptunePreviewBus.store(bus, std::memory_order_relaxed);
    (void)impl_->chiptunePreview.exchange(std::move(preview), std::memory_order_acq_rel);
    return true;
}

void AudioMixer::stop_chiptune_preview() noexcept {
    (void)impl_->chiptunePreview.exchange({}, std::memory_order_acq_rel);
}

bool AudioMixer::chiptune_preview_active() const noexcept {
    return static_cast<bool>(impl_->chiptunePreview.load(std::memory_order_acquire));
}

StreamSampleId AudioMixer::register_streamed_sample(const std::filesystem::path& path,
                                                    std::size_t ringCapacityFrames,
                                                    std::string* error) {
    if (impl_->streamCount >= kMaxStreamedSamples) {
        if (error) *error = "streamed sample capacity exhausted";
        return {};
    }
    auto stream = std::make_unique<CookedAudioStream>(path, ringCapacityFrames, error);
    if (!stream->valid()) return {};
    const auto& metadata = stream->metadata();
    if (metadata.sampleRate != impl_->sampleRate) {
        if (error) *error = "streamed sample rate must match the mixer rate; recook the asset";
        return {};
    }
    if (metadata.channels != 1U && metadata.channels != 2U) {
        if (error) *error = "streamed sample must be mono or stereo";
        return {};
    }
    auto asset = std::make_unique<StreamAsset>();
    asset->name = metadata.name.empty() ? path.filename().string() : metadata.name;
    asset->stream = std::move(stream);
    impl_->streams[impl_->streamCount] = std::move(asset);
    ++impl_->streamCount;
    return {static_cast<std::uint32_t>(impl_->streamCount)};
}

std::string_view AudioMixer::stream_name(StreamSampleId sample) const noexcept {
    const StreamAsset* asset = impl_->stream_asset(sample);
    return asset ? std::string_view(asset->name) : std::string_view{};
}

AudioSourceHandle AudioMixer::play_sample(const PlaySampleDesc& desc) noexcept {
    if (!impl_->asset(desc.sample)) return {};
    AudioSourceHandle handle{impl_->nextHandle.fetch_add(1U, std::memory_order_relaxed)};
    if (handle.value == 0U) handle.value = impl_->nextHandle.fetch_add(1U, std::memory_order_relaxed);
    Command command{}; command.type = CommandType::Play; command.handle = handle; command.play = desc;
    return impl_->post(command) ? handle : AudioSourceHandle{};
}

AudioSourceHandle AudioMixer::play_stream(const PlayStreamDesc& desc) noexcept {
    if (!impl_->stream_asset(desc.sample)) return {};
    AudioSourceHandle handle{impl_->nextHandle.fetch_add(1U, std::memory_order_relaxed)};
    if (handle.value == 0U) handle.value = impl_->nextHandle.fetch_add(1U, std::memory_order_relaxed);
    Command command{};
    command.type = CommandType::PlayStream;
    command.handle = handle;
    command.playStream = desc;
    return impl_->post(command) ? handle : AudioSourceHandle{};
}

bool AudioMixer::stop(AudioSourceHandle source, float fadeSeconds) noexcept {
    Command command{}; command.type = CommandType::Stop; command.handle = source; command.value = std::max(0.0F, fadeSeconds);
    return impl_->post(command);
}
bool AudioMixer::set_source_emitter(AudioSourceHandle source, const AudioEmitterState& emitter) noexcept {
    Command command{}; command.type = CommandType::Emitter; command.handle = source; command.emitter = emitter;
    return impl_->post(command);
}
bool AudioMixer::set_source_gain(AudioSourceHandle source, float gain) noexcept {
    Command command{}; command.type = CommandType::Gain; command.handle = source; command.value = gain;
    return impl_->post(command);
}
bool AudioMixer::set_bus_parameters(AudioBusId bus, const AudioBusParameters& parameters) noexcept {
    AudioBusParameters sanitized = parameters;
    sanitized.gain = clampf(sanitized.gain, 0.0F, 4.0F);
    sanitized.lowPassHertz = clampf(sanitized.lowPassHertz, 20.0F, 24000.0F);
    sanitized.reverbSend = clampf(sanitized.reverbSend, 0.0F, 1.0F);
    impl_->controlBuses[audio_bus_index(bus)] = sanitized;
    Command command{}; command.type = CommandType::Bus; command.bus = bus; command.busParameters = sanitized;
    return impl_->post(command);
}
bool AudioMixer::apply_bus_snapshot(const AudioBusSnapshot& snapshot, float transitionSeconds) noexcept {
    AudioBusSnapshot sanitized = snapshot;
    for (auto& parameters : sanitized.buses) {
        parameters.gain = clampf(parameters.gain, 0.0F, 4.0F);
        parameters.lowPassHertz = clampf(parameters.lowPassHertz, 20.0F, 24000.0F);
        parameters.reverbSend = clampf(parameters.reverbSend, 0.0F, 1.0F);
    }
    impl_->controlBuses = sanitized.buses;
    Command command{};
    command.type = CommandType::Snapshot;
    command.snapshot = sanitized;
    command.value = std::clamp(transitionSeconds, 0.0F, 30.0F);
    return impl_->post(command);
}
AudioBusSnapshot AudioMixer::bus_snapshot() const noexcept { return {impl_->controlBuses}; }
AudioBusParameters AudioMixer::bus_parameters(AudioBusId bus) const noexcept { return impl_->controlBuses[audio_bus_index(bus)]; }
void AudioMixer::set_listener(const AudioListenerState& listener) noexcept {
    Command command{}; command.type = CommandType::Listener; command.listener = listener; (void)impl_->post(command);
}
void AudioMixer::set_spatializer(std::shared_ptr<const IAudioSpatializer> spatializer) {
    impl_->spatializer.store(spatializer ? std::move(spatializer) : std::make_shared<AnalyticSpatializer>(),
                             std::memory_order_release);
}
void AudioMixer::set_capture_sink(std::shared_ptr<IAudioCaptureSink> sink, AudioCaptureTap tap) {
    (void)set_capture_sink(0U, std::move(sink), tap);
}
bool AudioMixer::set_capture_sink(std::size_t slot, std::shared_ptr<IAudioCaptureSink> sink,
                                  AudioCaptureTap tap) noexcept {
    if (slot >= kMaxAudioCaptureSinks) return false;
    impl_->captureTaps[slot].store(tap, std::memory_order_release);
    impl_->captureSinks[slot].store(std::move(sink), std::memory_order_release);
    return true;
}
void AudioMixer::clear_capture_sinks() noexcept {
    for (auto& sink : impl_->captureSinks) sink.store({}, std::memory_order_release);
}

void AudioMixer::render(std::span<float> interleavedStereo) noexcept {
    render(interleavedStereo.data(), interleavedStereo.size() / 2U);
}
void AudioMixer::render(float* output, std::size_t frameCount) noexcept {
    if (output == nullptr) return;
    const auto profileStart = std::chrono::steady_clock::now();
    std::size_t rendered{};
    while (rendered < frameCount) {
        const std::size_t chunk = std::min<std::size_t>(kRenderQuantum, frameCount - rendered);
        impl_->render_chunk(output + rendered * 2U, chunk);
        rendered += chunk;
    }
    const auto elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - profileStart).count());
    impl_->renderCalls.fetch_add(1U, std::memory_order_relaxed);
    impl_->renderNanoseconds.fetch_add(elapsed, std::memory_order_relaxed);
    impl_->renderLastNanoseconds.store(elapsed, std::memory_order_relaxed);
    std::uint64_t previous = impl_->renderMaxNanoseconds.load(std::memory_order_relaxed);
    while (elapsed > previous && !impl_->renderMaxNanoseconds.compare_exchange_weak(
        previous, elapsed, std::memory_order_relaxed)) {}
    const std::size_t bin = std::min<std::size_t>(impl_->renderHistogram.size() - 1U,
                                                  static_cast<std::size_t>(elapsed / 50000U));
    impl_->renderHistogram[bin].fetch_add(1U, std::memory_order_relaxed);
}

AudioMixerMeters AudioMixer::meters() const noexcept {
    AudioMixerMeters result;
    for (std::size_t bus = 0; bus < kAudioBusCount; ++bus) {
        result.buses[bus].peakLeft = bits_float(impl_->meterBits[bus * 4U].load(std::memory_order_relaxed));
        result.buses[bus].peakRight = bits_float(impl_->meterBits[bus * 4U + 1U].load(std::memory_order_relaxed));
        result.buses[bus].rmsLeft = bits_float(impl_->meterBits[bus * 4U + 2U].load(std::memory_order_relaxed));
        result.buses[bus].rmsRight = bits_float(impl_->meterBits[bus * 4U + 3U].load(std::memory_order_relaxed));
    }
    result.logicalSampleVoices = impl_->logicalVoices.load(std::memory_order_relaxed);
    result.physicalSampleVoices = impl_->physicalVoices.load(std::memory_order_relaxed);
    result.virtualSampleVoices = impl_->virtualVoices.load(std::memory_order_relaxed);
    result.streamVoices = impl_->activeStreamVoices.load(std::memory_order_relaxed);
    result.streamUnderrunFrames = impl_->streamUnderrunFrames.load(std::memory_order_relaxed);
    result.synthVoices = impl_->synth.meters().activeVoices;
    result.renderedFrames = impl_->meterFrames.load(std::memory_order_relaxed);
    result.droppedCommands = impl_->droppedCommands.load(std::memory_order_relaxed);
    result.stolenVoices = impl_->stolenVoices.load(std::memory_order_relaxed);
    result.lastStolenSource = {impl_->lastStolenHandle.load(std::memory_order_relaxed)};
    result.lastStolenPriority = static_cast<AudioPriority>(
        impl_->lastStolenPriority.load(std::memory_order_relaxed));
    const std::uint64_t calls = impl_->renderCalls.load(std::memory_order_relaxed);
    result.callbackProfile.renderCalls = calls;
    result.callbackProfile.lastMilliseconds = static_cast<double>(impl_->renderLastNanoseconds.load(std::memory_order_relaxed)) / 1.0e6;
    result.callbackProfile.maximumMilliseconds = static_cast<double>(impl_->renderMaxNanoseconds.load(std::memory_order_relaxed)) / 1.0e6;
    result.callbackProfile.meanMilliseconds = calls == 0U ? 0.0 :
        static_cast<double>(impl_->renderNanoseconds.load(std::memory_order_relaxed)) / static_cast<double>(calls) / 1.0e6;
    auto percentile = [&](double fraction) {
        if (calls == 0U) return 0.0;
        const std::uint64_t target = static_cast<std::uint64_t>(std::ceil(static_cast<double>(calls) * fraction));
        std::uint64_t cumulative{};
        for (std::size_t i = 0; i < impl_->renderHistogram.size(); ++i) {
            cumulative += impl_->renderHistogram[i].load(std::memory_order_relaxed);
            if (cumulative >= target) return static_cast<double>((i + 1U) * 50000U) / 1.0e6;
        }
        return result.callbackProfile.maximumMilliseconds;
    };
    result.callbackProfile.p95Milliseconds = percentile(0.95);
    result.callbackProfile.p99Milliseconds = percentile(0.99);
    return result;
}
void AudioMixer::all_sounds_off() noexcept { Command command{}; command.type = CommandType::AllOff; (void)impl_->post(command); }

std::string_view audio_bus_name(AudioBusId bus) noexcept {
    static constexpr std::array<std::string_view, kAudioBusCount> names{
        "Master", "Music", "Dialogue", "Effects", "Ambience", "UI", "Reverb"};
    return names[audio_bus_index(bus)];
}

} // namespace dve::audio
