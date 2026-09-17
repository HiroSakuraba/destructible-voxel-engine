#include "dve/audio/audio_analysis.hpp"
#include "dve/audio/audio_edit.hpp"
#include "dve/audio/audio_metering.hpp"
#include "dve/audio/audio_recovery.hpp"
#include "dve/audio/interactive_music.hpp"
#include "dve/editor_audio_timeline.hpp"
#include "dve/editor_audio_workspace.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

class PpmCanvas final : public dve::editor::IEditorCanvas {
public:
    PpmCanvas(int width, int height) : width_(width), height_(height), pixels_(static_cast<std::size_t>(width * height), 0x0A1018U) {}
    void fill(dve::editor::UiRect rect, dve::editor::EditorColor color) const override {
        rect.x = std::max(0, rect.x); rect.y = std::max(0, rect.y);
        const int right = std::min(width_, rect.x + std::max(0, rect.width));
        const int bottom = std::min(height_, rect.y + std::max(0, rect.height));
        for (int y = rect.y; y < bottom; ++y)
            for (int x = rect.x; x < right; ++x) pixel(x, y, color);
    }
    void outline(dve::editor::UiRect rect, dve::editor::EditorColor color) const override {
        line(rect.x, rect.y, rect.x + rect.width - 1, rect.y, color);
        line(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, color);
        line(rect.x, rect.y, rect.x, rect.y + rect.height - 1, color);
        line(rect.x + rect.width - 1, rect.y, rect.x + rect.width - 1, rect.y + rect.height - 1, color);
    }
    void line(int x1, int y1, int x2, int y2, dve::editor::EditorColor color, int width = 1) const override {
        const int dx = std::abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
        const int dy = -std::abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
        int error = dx + dy;
        for (;;) {
            for (int oy = -width / 2; oy <= width / 2; ++oy)
                for (int ox = -width / 2; ox <= width / 2; ++ox) pixel(x1 + ox, y1 + oy, color);
            if (x1 == x2 && y1 == y2) break;
            const int twice = 2 * error;
            if (twice >= dy) { error += dy; x1 += sx; }
            if (twice <= dx) { error += dx; y1 += sy; }
        }
    }
    void text(int x, int y, std::string_view value, dve::editor::EditorColor color) const override {
        int cursor = x;
        for (unsigned char character : value) {
            if (character == ' ') { cursor += 6; continue; }
            // Compact deterministic pseudo-glyph. It is intentionally dependency-free but still
            // makes labels visually distinct in headless release screenshots.
            for (int row = 0; row < 7; ++row) {
                const unsigned bits = static_cast<unsigned>(character) * 0x45D9F3BU + static_cast<unsigned>(row * 17);
                for (int column = 0; column < 5; ++column)
                    if (((bits >> (column + row)) & 1U) != 0U) pixel(cursor + column, y - 7 + row, color);
            }
            cursor += 6;
        }
    }
    [[nodiscard]] int text_width(std::string_view value) const override { return static_cast<int>(value.size()) * 6; }
    bool write(const std::filesystem::path& path) const {
        std::ofstream stream(path, std::ios::binary);
        if (!stream) return false;
        stream << "P6\n" << width_ << ' ' << height_ << "\n255\n";
        for (const auto color : pixels_) {
            const std::array<char, 3> rgb{static_cast<char>((color >> 16U) & 0xFFU),
                                          static_cast<char>((color >> 8U) & 0xFFU),
                                          static_cast<char>(color & 0xFFU)};
            stream.write(rgb.data(), 3);
        }
        return static_cast<bool>(stream);
    }
private:
    void pixel(int x, int y, dve::editor::EditorColor color) const {
        if (x < 0 || y < 0 || x >= width_ || y >= height_) return;
        pixels_[static_cast<std::size_t>(y * width_ + x)] = color;
    }
    int width_{};
    int height_{};
    mutable std::vector<dve::editor::EditorColor> pixels_;
};

dve::audio::DecodedAudioAsset make_source(std::string name, float frequency, float phase, float pulseSpacing) {
    dve::audio::DecodedAudioAsset asset;
    asset.metadata.name = std::move(name);
    asset.metadata.sampleRate = 48000U;
    asset.metadata.channels = 2U;
    asset.metadata.frameCount = 192000U;
    asset.metadata.durationSeconds = 4.0;
    asset.samples.resize(static_cast<std::size_t>(asset.metadata.frameCount) * 2U);
    for (std::uint64_t frame = 0U; frame < asset.metadata.frameCount; ++frame) {
        const float time = static_cast<float>(frame) / 48000.0F;
        const float pulsePhase = std::fmod(time, pulseSpacing);
        const float envelope = pulsePhase < 0.11F ? std::exp(-pulsePhase * 27.0F) : 0.08F;
        const float left = envelope * 0.62F * std::sin(6.283185307179586F * frequency * time + phase);
        const float right = envelope * 0.58F * std::sin(6.283185307179586F * (frequency * 1.003F) * time + phase);
        asset.samples[static_cast<std::size_t>(frame) * 2U] = left;
        asset.samples[static_cast<std::size_t>(frame) * 2U + 1U] = right;
    }
    asset.metadata.contentHash = dve::audio::audio_content_hash(asset.samples, asset.metadata);
    return asset;
}

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

} // namespace

int main(int argc, char** argv) {
    try {
        using namespace dve;
        const std::filesystem::path output = argc > 1 ? argv[1] : "dve_audio_workstation_v1_30_demo";
        std::filesystem::create_directories(output);
        audio::AudioEditSession session;
        session.name = "DVE Destruction Remix — v1.30";
        session.markers = {{0U, "Intro"}, {48000U, "Impact"}, {96000U, "Combat"}, {144000U, "Release"}};
        session.tempoMap = {{0U, 120.0, 4U, 4U}, {96000U, 132.0, 4U, 4U}};
        session.loop = {48000U, 144000U, true};
        session.punch = {72000U, 120000U, 4800U, 4800U, true};

        audio::AudioEditSourceLibrary sources;
        auto pulse = std::make_shared<audio::DecodedAudioAsset>(make_source("Pulse stem", 110.0F, 0.0F, 0.5F));
        auto shards = std::make_shared<audio::DecodedAudioAsset>(make_source("Shard stem", 330.0F, 0.7F, 0.25F));
        std::string error;
        const auto pulseId = sources.register_source(session, "pulse-source.flac", pulse, &error);
        const auto shardsId = sources.register_source(session, "shards-source.opus", shards, &error);
        require(pulseId && shardsId, error);

        const auto music = audio::add_audio_track(session, "Music / Synth");
        const auto destruction = audio::add_audio_track(session, "Destruction FX");
        const auto bed = audio::add_audio_clip(session, music, pulseId, 0U, 0U, 168000U, &error);
        require(static_cast<bool>(bed), error);
        session.tracks[0].clips.back().fadeInFrames = 4800U;
        session.tracks[0].clips.back().fadeOutFrames = 9600U;
        session.tracks[0].automation.push_back({audio::AudioEditAutomationParameter::Gain, true,
            {{0U, 0.45F, audio::AudioEditAutomationCurve::Linear},
             {48000U, 0.8F, audio::AudioEditAutomationCurve::SmoothStep},
             {144000U, 0.55F, audio::AudioEditAutomationCurve::Linear}}});
        session.tracks[0].sends.push_back({audio::AudioEditSendTarget::Reverb, 0.22F, false, true});

        require(audio::add_audio_take_lane(session, destruction, &error) == 1U, error);
        require(audio::set_audio_active_take_lane(session, destruction, 0U), "take lane zero");
        const auto impactA = audio::add_audio_clip(session, destruction, shardsId, 36000U, 0U, 84000U, &error);
        require(static_cast<bool>(impactA), error);
        require(audio::set_audio_active_take_lane(session, destruction, 1U), "take lane one");
        const auto impactB = audio::add_audio_clip(session, destruction, shardsId, 36000U, 48000U, 84000U, &error);
        require(static_cast<bool>(impactB), error);
        session.tracks[1].clips.back().reverse = true;
        require(audio::set_audio_comp_segment(session, destruction, 36000U, 36000U, 0U, 1200U, 1200U, &error), error);
        require(audio::set_audio_comp_segment(session, destruction, 72000U, 48000U, 1U, 1200U, 2400U, &error), error);
        session.tracks[1].pan = 0.18F;

        const auto bounce = audio::bounce_audio_edit_session(session, sources, "v1.30 workstation remix");
        require(audio::write_wav_file(output / "workstation_remix_v3.wav", bounce,
                                      audio::WavSampleEncoding::Float32, &error), error);
        require(audio::write_audio_edit_session(output / "workstation_remix_v3.dveaudioedit", session, &error), error);
        audio::AudioEditRecoveryJournal journal(output / "workstation_recovery_v3.dveaudiojournal");
        require(journal.append_snapshot(session, "v1.30 demo", &error), error);

        audio::InteractiveMusicGraph graph;
        graph.name = "Voxel combat score";
        graph.states = {
            {"Exploration", {{"Pulse", "pulse-source.flac", "danger", 0.0F, 1.0F, 0.7F, true}},
             {48000U, 96000U}, "", "", {{"Debris", audio::InteractiveMusicContainerMode::Shuffle, true,
                 {{"Small", "debris-small.wav", 2.0F, 0.8F, 12000U},
                  {"Large", "debris-large.wav", 1.0F, 1.0F, 24000U}}}}},
            {"Combat", {{"Pulse", "pulse-source.flac", "intensity", 0.0F, 1.0F, 1.0F, true},
                        {"Shards", "shards-source.opus", "intensity", 0.3F, 1.0F, 0.9F, true}},
             {48000U}, "combat-enter.wav", "", {}}
        };
        graph.transitions = {{"Exploration", "Combat", audio::InteractiveMusicQuantization::Bar, 2U,
                              "combat-stinger.wav", {{"danger", audio::InteractiveMusicComparison::GreaterEqual, 0.6F}}}};
        require(audio::write_interactive_music_graph(output / "voxel_combat.dveinteractiveaudio", graph, &error), error);

        std::vector<editor::AudioTimelinePeakCacheEntry> caches;
        caches.push_back({pulseId, audio::build_audio_waveform_peak_cache(*pulse, 64U, 12U)});
        caches.push_back({shardsId, audio::build_audio_waveform_peak_cache(*shards, 64U, 12U)});
        editor::AudioTimelineView view;
        view.framesPerPixel = 150.0;
        view.trackHeight = 230;
        view.ruler = editor::AudioTimelineRuler::Beats;
        view.display = editor::AudioTimelineDisplay::Combined;
        std::vector<editor::AudioTimelineSpectrogramCacheEntry> spectrograms;
        spectrograms.push_back({pulseId, audio::build_audio_spectrogram(*pulse, 256U, 96U, 64U, 1200U)});
        spectrograms.push_back({shardsId, audio::build_audio_spectrogram(*shards, 256U, 96U, 64U, 1200U)});
        const editor::UiRect content{24, 32, 1392, 690};
        const auto model = editor::build_audio_timeline_draw_model(session, view, content, 103000U, caches, impactB, spectrograms);
        const auto analysis = audio::analyze_audio_asset(bounce);
        PpmCanvas canvas(1440, 760);
        editor::render_audio_timeline_workspace(canvas, session, model, view, editor::AudioEditorTool::Crossfade,
                                                {72000U, 120000U, true}, &analysis);
        require(canvas.write(output / "audio_workstation_v1_30.ppm"), "could not write editor screenshot");

        const auto loudness = audio::measure_audio_loudness_ebu_r128(bounce, true);
        std::ofstream json(output / "workstation_analysis_v1_30.json");
        json << "{\n"
             << "  \"session_version\": " << session.version << ",\n"
             << "  \"tracks\": " << session.tracks.size() << ",\n"
             << "  \"comp_segments\": " << session.tracks[1].compSegments.size() << ",\n"
             << "  \"integrated_lufs\": " << loudness.integratedLufs << ",\n"
             << "  \"loudness_range_lu\": " << loudness.loudnessRangeLu << ",\n"
             << "  \"true_peak_dbtp\": " << loudness.truePeakDbtp << ",\n"
             << "  \"tempo_bpm\": " << analysis.estimatedTempoBpm << ",\n"
             << "  \"transients\": " << analysis.transientFrames.size() << "\n"
             << "}\n";
        require(static_cast<bool>(json), "could not write analysis JSON");

        std::cout << "DVE v1.30 audio workstation demo written to " << output << '\n';
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "DVE v1.30 demo failed: " << exception.what() << '\n';
        return 1;
    }
}
