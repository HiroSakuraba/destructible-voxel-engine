#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "dve/audio/audio_asset.hpp"

namespace dve::audio {

struct AudioEditSourceId {
    std::uint32_t value{};
    [[nodiscard]] explicit operator bool() const noexcept { return value != 0U; }
    auto operator<=>(const AudioEditSourceId&) const = default;
};

struct AudioEditTrackId {
    std::uint32_t value{};
    [[nodiscard]] explicit operator bool() const noexcept { return value != 0U; }
    auto operator<=>(const AudioEditTrackId&) const = default;
};

struct AudioEditClipId {
    std::uint32_t value{};
    [[nodiscard]] explicit operator bool() const noexcept { return value != 0U; }
    auto operator<=>(const AudioEditClipId&) const = default;
};

struct AudioEditSource {
    AudioEditSourceId id{};
    std::filesystem::path path;
    std::string name;
    std::uint64_t contentHash{};
    std::uint32_t sampleRate{48000};
    std::uint8_t channels{2};
    std::uint64_t frameCount{};
};

enum class AudioEditFadeCurve : std::uint8_t { Linear, EqualPower, SmoothStep };
enum class AudioEditAutomationParameter : std::uint8_t { Gain, Pan };
enum class AudioEditAutomationCurve : std::uint8_t { Step, Linear, SmoothStep };
enum class AudioEditSendTarget : std::uint8_t {
    Music, Dialogue, Effects, Ambience, UserInterface, Reverb, PreviewMaster
};

struct AudioEditAutomationPoint {
    std::uint64_t frame{};
    float value{};
    AudioEditAutomationCurve curve{AudioEditAutomationCurve::Linear};
};

struct AudioEditAutomationLane {
    AudioEditAutomationParameter parameter{AudioEditAutomationParameter::Gain};
    bool enabled{true};
    std::vector<AudioEditAutomationPoint> points;
};

struct AudioEditSend {
    AudioEditSendTarget target{AudioEditSendTarget::PreviewMaster};
    float gain{};
    bool preFader{};
    bool enabled{};
};

struct AudioEditCompSegment {
    std::uint64_t timelineStartFrame{};
    std::uint64_t timelineFrameCount{};
    std::uint32_t takeLane{};
    std::uint64_t fadeInFrames{};
    std::uint64_t fadeOutFrames{};
};

struct AudioEditClip {
    AudioEditClipId id{};
    AudioEditSourceId source{};
    std::uint64_t timelineStartFrame{};
    std::uint64_t sourceStartFrame{};
    std::uint64_t sourceFrameCount{};
    std::uint64_t timelineFrameCount{};
    float gain{1.0F};
    float pan{};
    std::uint64_t fadeInFrames{};
    std::uint64_t fadeOutFrames{};
    AudioEditFadeCurve fadeInCurve{AudioEditFadeCurve::Linear};
    AudioEditFadeCurve fadeOutCurve{AudioEditFadeCurve::Linear};
    std::uint32_t takeLane{};
    bool reverse{};
    bool loopSource{};
    bool muted{};
};

struct AudioEditTrack {
    AudioEditTrackId id{};
    std::string name;
    float gain{1.0F};
    float pan{};
    bool mute{};
    bool solo{};
    bool armed{};
    std::uint32_t takeLaneCount{1};
    std::uint32_t activeTakeLane{};
    std::vector<AudioEditAutomationLane> automation;
    std::vector<AudioEditSend> sends;
    std::vector<AudioEditCompSegment> compSegments;
    std::vector<AudioEditClip> clips;
};

struct AudioEditLoopRegion {
    std::uint64_t beginFrame{};
    std::uint64_t endFrame{};
    bool enabled{};
};

struct AudioEditPunchRegion {
    std::uint64_t beginFrame{};
    std::uint64_t endFrame{};
    std::uint64_t preRollFrames{};
    std::uint64_t postRollFrames{};
    bool enabled{};
};

struct AudioEditMarker {
    std::uint64_t frame{};
    std::string name;
};

struct AudioEditTempoPoint {
    std::uint64_t frame{};
    double beatsPerMinute{120.0};
    std::uint16_t numerator{4};
    std::uint16_t denominator{4};
};

struct AudioEditSession {
    std::uint32_t version{3};
    std::string name{"Untitled audio edit"};
    std::uint32_t sampleRate{48000};
    std::vector<AudioEditSource> sources;
    std::vector<AudioEditTrack> tracks;
    std::vector<AudioEditMarker> markers;
    std::vector<AudioEditTempoPoint> tempoMap{{}};
    AudioEditLoopRegion loop{};
    AudioEditPunchRegion punch{};
    std::uint32_t nextSourceId{1};
    std::uint32_t nextTrackId{1};
    std::uint32_t nextClipId{1};
};

// Holds decoded, immutable source PCM outside the serialized edit session. The session stores only
// source identity/path/hash metadata; clips are non-destructive references into this library.
class AudioEditSourceLibrary {
public:
    AudioEditSourceId register_source(AudioEditSession& session,
                                      const std::filesystem::path& originalPath,
                                      std::shared_ptr<const DecodedAudioAsset> asset,
                                      std::string* error = nullptr);
    AudioEditSourceId import_source(AudioEditSession& session,
                                    const std::filesystem::path& path,
                                    const AudioImportOptions& options = {},
                                    std::string* error = nullptr);
    [[nodiscard]] std::shared_ptr<const DecodedAudioAsset> resolve(AudioEditSourceId id) const noexcept;
    bool remove(AudioEditSourceId id) noexcept;
    void clear() noexcept;
private:
    struct Entry { AudioEditSourceId id{}; std::shared_ptr<const DecodedAudioAsset> asset; };
    std::vector<Entry> entries_;
};

AudioEditTrackId add_audio_track(AudioEditSession& session, std::string name);
AudioEditClipId add_audio_clip(AudioEditSession& session, AudioEditTrackId track,
                               AudioEditSourceId source, std::uint64_t timelineStartFrame,
                               std::uint64_t sourceStartFrame = 0,
                               std::uint64_t sourceFrameCount = 0,
                               std::string* error = nullptr);
bool delete_audio_clip(AudioEditSession& session, AudioEditClipId clip) noexcept;
bool ripple_delete_audio_clip(AudioEditSession& session, AudioEditClipId clip) noexcept;
bool move_audio_clip(AudioEditSession& session, AudioEditClipId clip,
                     std::uint64_t timelineStartFrame) noexcept;
bool slip_audio_clip(AudioEditSession& session, AudioEditClipId clip,
                     std::int64_t sourceFrameDelta) noexcept;
bool trim_audio_clip(AudioEditSession& session, AudioEditClipId clip,
                     std::uint64_t sourceStartFrame, std::uint64_t sourceFrameCount) noexcept;
std::optional<AudioEditClipId> split_audio_clip(AudioEditSession& session, AudioEditClipId clip,
                                                std::uint64_t timelineFrame,
                                                std::string* error = nullptr);
bool roll_audio_clip_boundary(AudioEditSession& session, AudioEditClipId leftClip,
                              AudioEditClipId rightClip, std::uint64_t newBoundaryFrame,
                              std::string* error = nullptr);
bool create_audio_crossfade(AudioEditSession& session, AudioEditClipId leftClip,
                            AudioEditClipId rightClip, std::uint64_t crossfadeFrames,
                            AudioEditFadeCurve curve = AudioEditFadeCurve::EqualPower,
                            std::string* error = nullptr);
std::uint32_t add_audio_take_lane(AudioEditSession& session, AudioEditTrackId track,
                                  std::string* error = nullptr);
bool set_audio_active_take_lane(AudioEditSession& session, AudioEditTrackId track,
                                std::uint32_t lane) noexcept;
bool set_audio_comp_segment(AudioEditSession& session, AudioEditTrackId track,
                            std::uint64_t timelineStartFrame,
                            std::uint64_t timelineFrameCount,
                            std::uint32_t takeLane,
                            std::uint64_t fadeInFrames = 0U,
                            std::uint64_t fadeOutFrames = 0U,
                            std::string* error = nullptr);
bool delete_audio_comp_segment(AudioEditSession& session, AudioEditTrackId track,
                               std::uint64_t timelineFrame) noexcept;
void clear_audio_comp_segments(AudioEditSession& session, AudioEditTrackId track) noexcept;
[[nodiscard]] std::uint32_t audio_comp_take_lane_at_frame(const AudioEditTrack& track,
                                                           std::uint64_t frame) noexcept;
[[nodiscard]] float evaluate_audio_automation(const AudioEditTrack& track,
                                              AudioEditAutomationParameter parameter,
                                              std::uint64_t frame,
                                              float fallback) noexcept;
[[nodiscard]] std::uint64_t audio_edit_length_frames(const AudioEditSession& session) noexcept;
[[nodiscard]] double audio_edit_frame_to_beats(const AudioEditSession& session,
                                                std::uint64_t frame) noexcept;
[[nodiscard]] std::uint64_t audio_edit_beats_to_frame(const AudioEditSession& session,
                                                       double beats) noexcept;
[[nodiscard]] std::uint64_t audio_edit_snap_frame_to_beat(const AudioEditSession& session,
                                                           std::uint64_t frame,
                                                           std::uint32_t subdivisions = 1) noexcept;

// Renders the requested timeline range to interleaved stereo. Sources are imported at the session
// sample rate. Clip/track gain, pan, fades, reverse, looping, take lanes, mute/solo, and gain/pan
// automation are applied without modifying source media.
void render_audio_edit_session(const AudioEditSession& session,
                               const AudioEditSourceLibrary& sources,
                               std::uint64_t startFrame,
                               std::span<float> interleavedStereo) noexcept;

[[nodiscard]] DecodedAudioAsset bounce_audio_edit_session(const AudioEditSession& session,
                                                           const AudioEditSourceLibrary& sources,
                                                           std::string name = {});

[[nodiscard]] bool write_audio_edit_session(const std::filesystem::path& path,
                                             const AudioEditSession& session,
                                             std::string* error = nullptr);
[[nodiscard]] std::optional<AudioEditSession> read_audio_edit_session(
    const std::filesystem::path& path, std::string* error = nullptr);

class AudioEditHistory {
public:
    explicit AudioEditHistory(std::size_t maximumSnapshots = 64);
    void reset(const AudioEditSession& session);
    void commit(const AudioEditSession& session);
    [[nodiscard]] bool can_undo() const noexcept;
    [[nodiscard]] bool can_redo() const noexcept;
    bool undo(AudioEditSession& session);
    bool redo(AudioEditSession& session);
private:
    std::size_t maximumSnapshots_{};
    std::vector<AudioEditSession> snapshots_;
    std::size_t cursor_{};
};

enum class AudioTransportState : std::uint8_t { Stopped, Playing, Paused };

class AudioEditTransport {
public:
    void play() noexcept;
    void pause() noexcept;
    void stop() noexcept;
    void rewind() noexcept;
    void seek(std::uint64_t frame) noexcept;
    [[nodiscard]] std::uint64_t cursor() const noexcept { return cursor_; }
    [[nodiscard]] AudioTransportState state() const noexcept { return state_; }
    void render(const AudioEditSession& session, const AudioEditSourceLibrary& sources,
                std::span<float> interleavedStereo) noexcept;
private:
    AudioTransportState state_{AudioTransportState::Stopped};
    std::uint64_t cursor_{};
};

} // namespace dve::audio
