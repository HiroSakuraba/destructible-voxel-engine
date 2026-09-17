#include "dve/audio/audio_edit.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace dve::audio {
namespace {

constexpr std::array<char, 8> kMagic{'D','V','E','A','E','D','T','1'};
constexpr std::uint32_t kCurrentVersion = 3;
constexpr std::size_t kMaximumStringBytes = 1U << 20U;
constexpr std::size_t kMaximumSources = 65536U;
constexpr std::size_t kMaximumTracks = 4096U;
constexpr std::size_t kMaximumClipsPerTrack = 1U << 20U;
constexpr std::size_t kMaximumMarkers = 1U << 20U;
constexpr std::size_t kMaximumTempoPoints = 65536U;
constexpr std::size_t kMaximumAutomationLanes = 32U;
constexpr std::size_t kMaximumAutomationPoints = 1U << 20U;
constexpr std::size_t kMaximumSends = 32U;
constexpr std::size_t kMaximumCompSegments = 1U << 20U;
constexpr double kPi = 3.1415926535897932384626433832795;

float clamp_gain(float value) noexcept { return std::clamp(value, 0.0F, 8.0F); }
float clamp_pan(float value) noexcept { return std::clamp(value, -1.0F, 1.0F); }

bool valid_fade_curve(AudioEditFadeCurve curve) noexcept {
    return curve == AudioEditFadeCurve::Linear || curve == AudioEditFadeCurve::EqualPower ||
           curve == AudioEditFadeCurve::SmoothStep;
}

bool valid_automation_curve(AudioEditAutomationCurve curve) noexcept {
    return curve == AudioEditAutomationCurve::Step || curve == AudioEditAutomationCurve::Linear ||
           curve == AudioEditAutomationCurve::SmoothStep;
}

bool valid_automation_parameter(AudioEditAutomationParameter parameter) noexcept {
    return parameter == AudioEditAutomationParameter::Gain || parameter == AudioEditAutomationParameter::Pan;
}

bool valid_send_target(AudioEditSendTarget target) noexcept {
    return static_cast<unsigned>(target) <= static_cast<unsigned>(AudioEditSendTarget::PreviewMaster);
}

bool valid_time_signature(const AudioEditTempoPoint& point) noexcept {
    if (!std::isfinite(point.beatsPerMinute) || point.beatsPerMinute < 20.0 || point.beatsPerMinute > 400.0 ||
        point.numerator == 0U || point.numerator > 32U || point.denominator == 0U || point.denominator > 32U) return false;
    return (point.denominator & static_cast<std::uint16_t>(point.denominator - 1U)) == 0U;
}

bool validate_loaded_session(AudioEditSession& session, std::string* error) {
    const auto fail = [error](const char* message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (session.sampleRate < 8000U || session.sampleRate > 384000U) {
        return fail("invalid audio edit sample rate");
    }
    if (session.loop.enabled && session.loop.endFrame <= session.loop.beginFrame) {
        return fail("invalid audio edit loop region");
    }
    if (session.punch.enabled && session.punch.endFrame <= session.punch.beginFrame) {
        return fail("invalid audio edit punch region");
    }
    if (session.tempoMap.empty()) session.tempoMap.push_back({});
    if (session.tempoMap.front().frame != 0U) return fail("tempo map must begin at frame zero");
    for (std::size_t i = 0; i < session.tempoMap.size(); ++i) {
        if (!valid_time_signature(session.tempoMap[i]) ||
            (i > 0U && session.tempoMap[i].frame <= session.tempoMap[i - 1U].frame)) {
            return fail("invalid or unsorted tempo map");
        }
    }
    for (std::size_t i = 1; i < session.markers.size(); ++i) {
        if (session.markers[i].frame < session.markers[i - 1U].frame) return fail("audio edit markers are not sorted");
    }

    std::unordered_map<std::uint32_t, const AudioEditSource*> sources;
    sources.reserve(session.sources.size());
    std::uint32_t maximumSourceId{};
    for (const auto& source : session.sources) {
        if (!source.id || source.sampleRate != session.sampleRate || source.channels == 0U ||
            source.channels > 2U || source.frameCount == 0U ||
            !sources.emplace(source.id.value, &source).second) {
            return fail("invalid or duplicate audio edit source");
        }
        maximumSourceId = std::max(maximumSourceId, source.id.value);
    }

    std::unordered_set<std::uint32_t> trackIds;
    std::unordered_set<std::uint32_t> clipIds;
    trackIds.reserve(session.tracks.size());
    std::uint32_t maximumTrackId{};
    std::uint32_t maximumClipId{};
    for (auto& track : session.tracks) {
        if (!track.id || !std::isfinite(track.gain) || !std::isfinite(track.pan) ||
            track.takeLaneCount == 0U || track.takeLaneCount > 1024U ||
            track.activeTakeLane >= track.takeLaneCount ||
            !trackIds.insert(track.id.value).second) {
            return fail("invalid or duplicate audio edit track");
        }
        track.gain = clamp_gain(track.gain);
        track.pan = clamp_pan(track.pan);
        maximumTrackId = std::max(maximumTrackId, track.id.value);

        std::array<bool, 2> seenAutomation{};
        if (track.automation.size() > kMaximumAutomationLanes || track.sends.size() > kMaximumSends ||
            track.compSegments.size() > kMaximumCompSegments) {
            return fail("audio edit track metadata exceeds limits");
        }
        for (auto& lane : track.automation) {
            const auto parameterIndex = static_cast<std::size_t>(lane.parameter);
            if (!valid_automation_parameter(lane.parameter) || parameterIndex >= seenAutomation.size() ||
                seenAutomation[parameterIndex] || lane.points.size() > kMaximumAutomationPoints) {
                return fail("invalid or duplicate audio automation lane");
            }
            seenAutomation[parameterIndex] = true;
            for (std::size_t i = 0; i < lane.points.size(); ++i) {
                auto& point = lane.points[i];
                if (!std::isfinite(point.value) || !valid_automation_curve(point.curve) ||
                    (i > 0U && point.frame <= lane.points[i - 1U].frame)) {
                    return fail("invalid or unsorted audio automation points");
                }
                point.value = lane.parameter == AudioEditAutomationParameter::Gain
                    ? clamp_gain(point.value) : clamp_pan(point.value);
            }
        }
        for (auto& send : track.sends) {
            if (!valid_send_target(send.target) || !std::isfinite(send.gain)) return fail("invalid audio edit send");
            send.gain = clamp_gain(send.gain);
        }
        std::sort(track.compSegments.begin(), track.compSegments.end(),
                  [](const AudioEditCompSegment& a, const AudioEditCompSegment& b) {
                      return a.timelineStartFrame < b.timelineStartFrame;
                  });
        std::uint64_t previousCompEnd{};
        bool firstComp = true;
        for (auto& segment : track.compSegments) {
            if (segment.timelineFrameCount == 0U || segment.takeLane >= track.takeLaneCount ||
                segment.timelineStartFrame > std::numeric_limits<std::uint64_t>::max() - segment.timelineFrameCount) {
                return fail("invalid audio comp segment");
            }
            const std::uint64_t end = segment.timelineStartFrame + segment.timelineFrameCount;
            if (!firstComp && segment.timelineStartFrame < previousCompEnd) {
                return fail("overlapping audio comp segments");
            }
            segment.fadeInFrames = std::min(segment.fadeInFrames, segment.timelineFrameCount);
            segment.fadeOutFrames = std::min(segment.fadeOutFrames, segment.timelineFrameCount);
            previousCompEnd = end;
            firstComp = false;
        }

        for (auto& clip : track.clips) {
            const auto sourceIt = sources.find(clip.source.value);
            if (!clip.id || !clip.source || !clipIds.insert(clip.id.value).second ||
                sourceIt == sources.end() || !std::isfinite(clip.gain) || !std::isfinite(clip.pan) ||
                clip.sourceFrameCount == 0U || clip.timelineFrameCount == 0U ||
                clip.takeLane >= track.takeLaneCount || !valid_fade_curve(clip.fadeInCurve) ||
                !valid_fade_curve(clip.fadeOutCurve)) {
                return fail("invalid or duplicate audio edit clip");
            }
            const AudioEditSource& source = *sourceIt->second;
            if (clip.sourceStartFrame >= source.frameCount ||
                clip.sourceFrameCount > source.frameCount - clip.sourceStartFrame ||
                (!clip.loopSource && clip.timelineFrameCount > clip.sourceFrameCount) ||
                clip.timelineStartFrame > std::numeric_limits<std::uint64_t>::max() - clip.timelineFrameCount) {
                return fail("audio edit clip range is invalid");
            }
            clip.gain = clamp_gain(clip.gain);
            clip.pan = clamp_pan(clip.pan);
            clip.fadeInFrames = std::min(clip.fadeInFrames, clip.timelineFrameCount);
            clip.fadeOutFrames = std::min(clip.fadeOutFrames, clip.timelineFrameCount);
            maximumClipId = std::max(maximumClipId, clip.id.value);
        }
    }
    if (maximumSourceId == std::numeric_limits<std::uint32_t>::max() ||
        maximumTrackId == std::numeric_limits<std::uint32_t>::max() ||
        maximumClipId == std::numeric_limits<std::uint32_t>::max()) {
        return fail("audio edit identifier space is exhausted");
    }
    session.nextSourceId = std::max(session.nextSourceId, maximumSourceId + 1U);
    session.nextTrackId = std::max(session.nextTrackId, maximumTrackId + 1U);
    session.nextClipId = std::max(session.nextClipId, maximumClipId + 1U);
    session.version = kCurrentVersion;
    return true;
}

template<class T>
bool write_value(std::ostream& stream, const T& value) {
    stream.write(reinterpret_cast<const char*>(&value), static_cast<std::streamsize>(sizeof(T)));
    return static_cast<bool>(stream);
}

template<class T>
bool read_value(std::istream& stream, T& value) {
    stream.read(reinterpret_cast<char*>(&value), static_cast<std::streamsize>(sizeof(T)));
    return static_cast<bool>(stream);
}

bool write_string(std::ostream& stream, const std::string& value) {
    if (value.size() > kMaximumStringBytes) return false;
    const auto size = static_cast<std::uint32_t>(value.size());
    return write_value(stream, size) &&
        (size == 0U || static_cast<bool>(stream.write(value.data(), static_cast<std::streamsize>(size))));
}

bool read_string(std::istream& stream, std::string& value) {
    std::uint32_t size{};
    if (!read_value(stream, size) || size > kMaximumStringBytes) return false;
    value.resize(size);
    if (size != 0U) stream.read(value.data(), static_cast<std::streamsize>(size));
    return static_cast<bool>(stream);
}

AudioEditTrack* find_track(AudioEditSession& session, AudioEditTrackId id) noexcept {
    const auto it = std::find_if(session.tracks.begin(), session.tracks.end(),
                                 [id](const AudioEditTrack& track) { return track.id == id; });
    return it == session.tracks.end() ? nullptr : &*it;
}


const AudioEditSource* find_source(const AudioEditSession& session, AudioEditSourceId id) noexcept {
    const auto it = std::find_if(session.sources.begin(), session.sources.end(),
                                 [id](const AudioEditSource& source) { return source.id == id; });
    return it == session.sources.end() ? nullptr : &*it;
}

AudioEditClip* find_clip(AudioEditSession& session, AudioEditClipId id,
                         AudioEditTrack** owner = nullptr) noexcept {
    for (auto& track : session.tracks) {
        const auto it = std::find_if(track.clips.begin(), track.clips.end(),
                                     [id](const AudioEditClip& clip) { return clip.id == id; });
        if (it != track.clips.end()) {
            if (owner != nullptr) *owner = &track;
            return &*it;
        }
    }
    return nullptr;
}

float curve_value(float t, AudioEditFadeCurve curve) noexcept {
    t = std::clamp(t, 0.0F, 1.0F);
    switch (curve) {
        case AudioEditFadeCurve::EqualPower:
            return std::sin(t * static_cast<float>(kPi * 0.5));
        case AudioEditFadeCurve::SmoothStep:
            return t * t * (3.0F - 2.0F * t);
        case AudioEditFadeCurve::Linear:
        default:
            return t;
    }
}

float clip_envelope(const AudioEditClip& clip, std::uint64_t localFrame) noexcept {
    float envelope = 1.0F;
    if (clip.fadeInFrames > 0U && localFrame < clip.fadeInFrames) {
        const float t = static_cast<float>(localFrame) / static_cast<float>(clip.fadeInFrames);
        envelope *= curve_value(t, clip.fadeInCurve);
    }
    if (clip.fadeOutFrames > 0U && localFrame < clip.timelineFrameCount) {
        const std::uint64_t remaining = clip.timelineFrameCount - localFrame;
        if (remaining < clip.fadeOutFrames) {
            const float t = static_cast<float>(remaining) / static_cast<float>(clip.fadeOutFrames);
            envelope *= curve_value(t, clip.fadeOutCurve);
        }
    }
    return std::clamp(envelope, 0.0F, 1.0F);
}

float comp_segment_envelope(const AudioEditTrack& track, std::uint32_t takeLane,
                            std::uint64_t frame) noexcept {
    if (track.compSegments.empty()) return takeLane == track.activeTakeLane ? 1.0F : 0.0F;
    const auto upper = std::upper_bound(track.compSegments.begin(), track.compSegments.end(), frame,
        [](std::uint64_t value, const AudioEditCompSegment& segment) {
            return value < segment.timelineStartFrame;
        });
    if (upper == track.compSegments.begin()) return takeLane == track.activeTakeLane ? 1.0F : 0.0F;
    const auto& segment = *(upper - 1);
    if (frame < segment.timelineStartFrame || frame - segment.timelineStartFrame >= segment.timelineFrameCount)
        return takeLane == track.activeTakeLane ? 1.0F : 0.0F;
    if (takeLane != segment.takeLane) return 0.0F;
    const std::uint64_t local = frame - segment.timelineStartFrame;
    float envelope = 1.0F;
    if (segment.fadeInFrames > 0U && local < segment.fadeInFrames) {
        envelope *= curve_value(static_cast<float>(local) / static_cast<float>(segment.fadeInFrames),
                                AudioEditFadeCurve::EqualPower);
    }
    if (segment.fadeOutFrames > 0U) {
        const std::uint64_t remaining = segment.timelineFrameCount - local;
        if (remaining < segment.fadeOutFrames) {
            envelope *= curve_value(static_cast<float>(remaining) / static_cast<float>(segment.fadeOutFrames),
                                    AudioEditFadeCurve::EqualPower);
        }
    }
    return std::clamp(envelope, 0.0F, 1.0F);
}

void stereo_gains(float gain, float pan, float& left, float& right) noexcept {
    const float resolvedGain = clamp_gain(gain);
    const float resolvedPan = clamp_pan(pan);
    left = resolvedGain * (resolvedPan > 0.0F ? 1.0F - resolvedPan : 1.0F);
    right = resolvedGain * (resolvedPan < 0.0F ? 1.0F + resolvedPan : 1.0F);
}

float automation_curve_value(float a, float b, float t, AudioEditAutomationCurve curve) noexcept {
    if (curve == AudioEditAutomationCurve::Step) return a;
    if (curve == AudioEditAutomationCurve::SmoothStep) t = t * t * (3.0F - 2.0F * t);
    return a + (b - a) * t;
}

} // namespace

AudioEditSourceId AudioEditSourceLibrary::register_source(AudioEditSession& session,
                                                           const std::filesystem::path& originalPath,
                                                           std::shared_ptr<const DecodedAudioAsset> asset,
                                                           std::string* error) {
    if (!asset || asset->metadata.channels == 0U || asset->metadata.channels > 2U ||
        asset->metadata.sampleRate < 8000U || asset->samples.empty()) {
        if (error) *error = "audio edit source is invalid";
        return {};
    }
    if (asset->metadata.sampleRate != session.sampleRate) {
        if (error) *error = "audio edit source must be imported at the session sample rate";
        return {};
    }
    for (const auto& source : session.sources) {
        if (source.contentHash == asset->metadata.contentHash && source.frameCount == asset->metadata.frameCount) {
            const auto existing = resolve(source.id);
            if (!existing) entries_.push_back({source.id, std::move(asset)});
            return source.id;
        }
    }
    AudioEditSource source;
    source.id = {session.nextSourceId++};
    source.path = originalPath;
    source.name = asset->metadata.name.empty() ? originalPath.stem().string() : asset->metadata.name;
    source.contentHash = asset->metadata.contentHash;
    source.sampleRate = asset->metadata.sampleRate;
    source.channels = asset->metadata.channels;
    source.frameCount = asset->metadata.frameCount;
    session.sources.push_back(source);
    entries_.push_back({source.id, std::move(asset)});
    return source.id;
}

AudioEditSourceId AudioEditSourceLibrary::import_source(AudioEditSession& session,
                                                         const std::filesystem::path& path,
                                                         const AudioImportOptions& options,
                                                         std::string* error) {
    AudioImportOptions resolved = options;
    resolved.targetSampleRate = session.sampleRate;
    auto decoded = import_audio_file(path, resolved, error);
    if (!decoded) return {};
    return register_source(session, path, std::make_shared<DecodedAudioAsset>(std::move(*decoded)), error);
}

std::shared_ptr<const DecodedAudioAsset> AudioEditSourceLibrary::resolve(AudioEditSourceId id) const noexcept {
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [id](const Entry& entry) { return entry.id == id; });
    return it == entries_.end() ? std::shared_ptr<const DecodedAudioAsset>{} : it->asset;
}

bool AudioEditSourceLibrary::remove(AudioEditSourceId id) noexcept {
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [id](const Entry& entry) { return entry.id == id; });
    if (it == entries_.end()) return false;
    entries_.erase(it);
    return true;
}

void AudioEditSourceLibrary::clear() noexcept { entries_.clear(); }

AudioEditTrackId add_audio_track(AudioEditSession& session, std::string name) {
    AudioEditTrack track;
    track.id = {session.nextTrackId++};
    track.name = name.empty() ? "Track " + std::to_string(track.id.value) : std::move(name);
    session.tracks.push_back(std::move(track));
    return session.tracks.back().id;
}

AudioEditClipId add_audio_clip(AudioEditSession& session, AudioEditTrackId trackId,
                                AudioEditSourceId sourceId, std::uint64_t timelineStartFrame,
                                std::uint64_t sourceStartFrame, std::uint64_t sourceFrameCount,
                                std::string* error) {
    AudioEditTrack* track = find_track(session, trackId);
    const AudioEditSource* source = find_source(session, sourceId);
    if (track == nullptr || source == nullptr || sourceStartFrame >= source->frameCount) {
        if (error) *error = "audio edit track/source/range is invalid";
        return {};
    }
    const std::uint64_t available = source->frameCount - sourceStartFrame;
    if (sourceFrameCount == 0U) sourceFrameCount = available;
    if (sourceFrameCount > available) {
        if (error) *error = "audio edit clip exceeds its source";
        return {};
    }
    AudioEditClip clip;
    clip.id = {session.nextClipId++};
    clip.source = sourceId;
    clip.timelineStartFrame = timelineStartFrame;
    clip.sourceStartFrame = sourceStartFrame;
    clip.sourceFrameCount = sourceFrameCount;
    clip.timelineFrameCount = sourceFrameCount;
    clip.takeLane = track->activeTakeLane;
    track->clips.push_back(clip);
    return clip.id;
}

bool delete_audio_clip(AudioEditSession& session, AudioEditClipId id) noexcept {
    for (auto& track : session.tracks) {
        const auto oldSize = track.clips.size();
        std::erase_if(track.clips, [id](const AudioEditClip& clip) { return clip.id == id; });
        if (track.clips.size() != oldSize) return true;
    }
    return false;
}

bool ripple_delete_audio_clip(AudioEditSession& session, AudioEditClipId id) noexcept {
    AudioEditTrack* owner{};
    AudioEditClip* clip = find_clip(session, id, &owner);
    if (clip == nullptr || owner == nullptr) return false;
    const std::uint64_t begin = clip->timelineStartFrame;
    const std::uint64_t duration = clip->timelineFrameCount;
    const std::uint64_t end = begin + duration;
    if (!delete_audio_clip(session, id)) return false;
    for (auto& track : session.tracks) {
        for (auto& candidate : track.clips) {
            if (candidate.timelineStartFrame >= end) candidate.timelineStartFrame -= duration;
        }
    }
    for (auto& marker : session.markers) {
        if (marker.frame >= end) marker.frame -= duration;
        else if (marker.frame > begin) marker.frame = begin;
    }
    return true;
}

bool move_audio_clip(AudioEditSession& session, AudioEditClipId id,
                     std::uint64_t timelineStartFrame) noexcept {
    AudioEditClip* clip = find_clip(session, id);
    if (clip == nullptr || timelineStartFrame > std::numeric_limits<std::uint64_t>::max() - clip->timelineFrameCount)
        return false;
    clip->timelineStartFrame = timelineStartFrame;
    return true;
}

bool slip_audio_clip(AudioEditSession& session, AudioEditClipId id,
                     std::int64_t sourceFrameDelta) noexcept {
    AudioEditClip* clip = find_clip(session, id);
    if (clip == nullptr) return false;
    const AudioEditSource* source = find_source(session, clip->source);
    if (source == nullptr || clip->sourceFrameCount > source->frameCount) return false;
    const std::uint64_t maximumStart = source->frameCount - clip->sourceFrameCount;
    std::uint64_t resolved{};
    if (sourceFrameDelta < 0) {
        const std::uint64_t magnitude = static_cast<std::uint64_t>(-(sourceFrameDelta + 1)) + 1U;
        resolved = magnitude > clip->sourceStartFrame ? 0U : clip->sourceStartFrame - magnitude;
    } else {
        const std::uint64_t delta = static_cast<std::uint64_t>(sourceFrameDelta);
        resolved = delta > maximumStart - std::min(maximumStart, clip->sourceStartFrame)
            ? maximumStart : clip->sourceStartFrame + delta;
    }
    clip->sourceStartFrame = std::min(resolved, maximumStart);
    return true;
}

bool trim_audio_clip(AudioEditSession& session, AudioEditClipId id,
                     std::uint64_t sourceStartFrame, std::uint64_t sourceFrameCount) noexcept {
    AudioEditClip* clip = find_clip(session, id);
    if (clip == nullptr || sourceFrameCount == 0U) return false;
    const AudioEditSource* source = find_source(session, clip->source);
    if (source == nullptr || sourceStartFrame >= source->frameCount ||
        sourceFrameCount > source->frameCount - sourceStartFrame) return false;
    clip->sourceStartFrame = sourceStartFrame;
    clip->sourceFrameCount = sourceFrameCount;
    if (!clip->loopSource) clip->timelineFrameCount = sourceFrameCount;
    clip->fadeInFrames = std::min(clip->fadeInFrames, clip->timelineFrameCount);
    clip->fadeOutFrames = std::min(clip->fadeOutFrames, clip->timelineFrameCount);
    return true;
}

std::optional<AudioEditClipId> split_audio_clip(AudioEditSession& session, AudioEditClipId id,
                                                 std::uint64_t timelineFrame,
                                                 std::string* error) {
    AudioEditTrack* track{};
    AudioEditClip* clip = find_clip(session, id, &track);
    if (clip == nullptr || track == nullptr || timelineFrame <= clip->timelineStartFrame ||
        timelineFrame >= clip->timelineStartFrame + clip->timelineFrameCount) {
        if (error) *error = "split point is outside the clip interior";
        return std::nullopt;
    }
    if (clip->loopSource) {
        if (error) *error = "split a looped clip after consolidating it or disabling source looping";
        return std::nullopt;
    }
    const std::uint64_t leftFrames = timelineFrame - clip->timelineStartFrame;
    const std::uint64_t rightFrames = clip->timelineFrameCount - leftFrames;
    if (leftFrames > clip->sourceFrameCount || rightFrames > clip->sourceFrameCount) {
        if (error) *error = "clip/source duration mismatch";
        return std::nullopt;
    }
    AudioEditClip original = *clip;
    AudioEditClip right = original;
    right.id = {session.nextClipId++};
    right.timelineStartFrame = timelineFrame;
    right.timelineFrameCount = rightFrames;
    right.sourceFrameCount = rightFrames;
    right.fadeInFrames = 0U;
    if (original.reverse) {
        clip->sourceStartFrame = original.sourceStartFrame + rightFrames;
        right.sourceStartFrame = original.sourceStartFrame;
    } else {
        right.sourceStartFrame = original.sourceStartFrame + leftFrames;
    }
    clip->sourceFrameCount = leftFrames;
    clip->timelineFrameCount = leftFrames;
    clip->fadeOutFrames = 0U;
    track->clips.push_back(right);
    return right.id;
}

bool roll_audio_clip_boundary(AudioEditSession& session, AudioEditClipId leftId,
                              AudioEditClipId rightId, std::uint64_t newBoundaryFrame,
                              std::string* error) {
    AudioEditTrack* leftTrack{};
    AudioEditTrack* rightTrack{};
    AudioEditClip* left = find_clip(session, leftId, &leftTrack);
    AudioEditClip* right = find_clip(session, rightId, &rightTrack);
    auto fail = [&](const char* message) { if (error) *error = message; return false; };
    if (left == nullptr || right == nullptr || leftTrack != rightTrack) return fail("roll edit requires clips on one track");
    if (left->loopSource || right->loopSource || left->reverse || right->reverse) return fail("roll edit requires forward non-looped clips");
    const std::uint64_t leftBegin = left->timelineStartFrame;
    const std::uint64_t oldBoundary = right->timelineStartFrame;
    const std::uint64_t rightEnd = right->timelineStartFrame + right->timelineFrameCount;
    if (leftBegin >= oldBoundary || left->timelineStartFrame + left->timelineFrameCount != oldBoundary ||
        newBoundaryFrame <= leftBegin || newBoundaryFrame >= rightEnd) {
        return fail("roll edit requires adjacent clips and an interior boundary");
    }
    const std::int64_t delta = newBoundaryFrame >= oldBoundary
        ? static_cast<std::int64_t>(newBoundaryFrame - oldBoundary)
        : -static_cast<std::int64_t>(oldBoundary - newBoundaryFrame);
    const AudioEditSource* leftSource = find_source(session, left->source);
    const AudioEditSource* rightSource = find_source(session, right->source);
    if (leftSource == nullptr || rightSource == nullptr) return fail("roll edit source is missing");
    const std::uint64_t newLeftFrames = newBoundaryFrame - leftBegin;
    const std::uint64_t newRightFrames = rightEnd - newBoundaryFrame;
    if (newLeftFrames > leftSource->frameCount - left->sourceStartFrame) return fail("left source has no roll handle");
    if (delta < 0) {
        const std::uint64_t amount = static_cast<std::uint64_t>(-delta);
        if (amount > right->sourceStartFrame) return fail("right source has no roll handle");
        right->sourceStartFrame -= amount;
    } else {
        const std::uint64_t amount = static_cast<std::uint64_t>(delta);
        if (right->sourceStartFrame + amount > rightSource->frameCount - newRightFrames)
            return fail("right source has no roll handle");
        right->sourceStartFrame += amount;
    }
    left->sourceFrameCount = newLeftFrames;
    left->timelineFrameCount = newLeftFrames;
    right->timelineStartFrame = newBoundaryFrame;
    right->sourceFrameCount = newRightFrames;
    right->timelineFrameCount = newRightFrames;
    left->fadeOutFrames = std::min(left->fadeOutFrames, newLeftFrames);
    right->fadeInFrames = std::min(right->fadeInFrames, newRightFrames);
    return true;
}

bool create_audio_crossfade(AudioEditSession& session, AudioEditClipId leftId,
                            AudioEditClipId rightId, std::uint64_t crossfadeFrames,
                            AudioEditFadeCurve curve, std::string* error) {
    AudioEditTrack* leftTrack{};
    AudioEditTrack* rightTrack{};
    AudioEditClip* left = find_clip(session, leftId, &leftTrack);
    AudioEditClip* right = find_clip(session, rightId, &rightTrack);
    auto fail = [&](const char* message) { if (error) *error = message; return false; };
    if (left == nullptr || right == nullptr || leftTrack != rightTrack || crossfadeFrames == 0U ||
        !valid_fade_curve(curve)) return fail("crossfade clips or curve are invalid");
    if (crossfadeFrames > left->timelineFrameCount || crossfadeFrames > right->timelineFrameCount)
        return fail("crossfade exceeds clip duration");
    const std::uint64_t leftEnd = left->timelineStartFrame + left->timelineFrameCount;
    if (leftEnd < crossfadeFrames) return fail("crossfade timeline underflow");
    right->timelineStartFrame = leftEnd - crossfadeFrames;
    left->fadeOutFrames = crossfadeFrames;
    right->fadeInFrames = crossfadeFrames;
    left->fadeOutCurve = curve;
    right->fadeInCurve = curve;
    return true;
}

std::uint32_t add_audio_take_lane(AudioEditSession& session, AudioEditTrackId trackId,
                                  std::string* error) {
    AudioEditTrack* track = find_track(session, trackId);
    if (track == nullptr || track->takeLaneCount >= 1024U) {
        if (error) *error = "audio take lane limit reached or track is invalid";
        return std::numeric_limits<std::uint32_t>::max();
    }
    return track->takeLaneCount++;
}

bool set_audio_active_take_lane(AudioEditSession& session, AudioEditTrackId trackId,
                                std::uint32_t lane) noexcept {
    AudioEditTrack* track = find_track(session, trackId);
    if (track == nullptr || lane >= track->takeLaneCount) return false;
    track->activeTakeLane = lane;
    return true;
}

bool set_audio_comp_segment(AudioEditSession& session, AudioEditTrackId trackId,
                            std::uint64_t timelineStartFrame,
                            std::uint64_t timelineFrameCount,
                            std::uint32_t takeLane,
                            std::uint64_t fadeInFrames,
                            std::uint64_t fadeOutFrames,
                            std::string* error) {
    AudioEditTrack* track = find_track(session, trackId);
    auto fail = [&](const char* message) { if (error != nullptr) *error = message; return false; };
    if (track == nullptr || timelineFrameCount == 0U || takeLane >= track->takeLaneCount ||
        timelineStartFrame > std::numeric_limits<std::uint64_t>::max() - timelineFrameCount) {
        return fail("invalid audio comp segment");
    }
    if (track->compSegments.size() >= kMaximumCompSegments) return fail("audio comp segment limit reached");
    const std::uint64_t newEnd = timelineStartFrame + timelineFrameCount;
    std::vector<AudioEditCompSegment> rebuilt;
    rebuilt.reserve(track->compSegments.size() + 2U);
    for (const auto& existing : track->compSegments) {
        const std::uint64_t existingEnd = existing.timelineStartFrame + existing.timelineFrameCount;
        if (existingEnd <= timelineStartFrame || existing.timelineStartFrame >= newEnd) {
            rebuilt.push_back(existing);
            continue;
        }
        if (existing.timelineStartFrame < timelineStartFrame) {
            auto left = existing;
            left.timelineFrameCount = timelineStartFrame - existing.timelineStartFrame;
            left.fadeOutFrames = std::min(left.fadeOutFrames, left.timelineFrameCount);
            left.fadeInFrames = std::min(left.fadeInFrames, left.timelineFrameCount);
            rebuilt.push_back(left);
        }
        if (existingEnd > newEnd) {
            auto right = existing;
            right.timelineStartFrame = newEnd;
            right.timelineFrameCount = existingEnd - newEnd;
            right.fadeOutFrames = std::min(right.fadeOutFrames, right.timelineFrameCount);
            right.fadeInFrames = std::min(right.fadeInFrames, right.timelineFrameCount);
            rebuilt.push_back(right);
        }
    }
    rebuilt.push_back({timelineStartFrame, timelineFrameCount, takeLane,
                       std::min(fadeInFrames, timelineFrameCount),
                       std::min(fadeOutFrames, timelineFrameCount)});
    std::sort(rebuilt.begin(), rebuilt.end(),
              [](const AudioEditCompSegment& a, const AudioEditCompSegment& b) {
                  return a.timelineStartFrame < b.timelineStartFrame;
              });
    track->compSegments = std::move(rebuilt);
    return true;
}

bool delete_audio_comp_segment(AudioEditSession& session, AudioEditTrackId trackId,
                               std::uint64_t timelineFrame) noexcept {
    AudioEditTrack* track = find_track(session, trackId);
    if (track == nullptr) return false;
    const auto it = std::find_if(track->compSegments.begin(), track->compSegments.end(),
        [timelineFrame](const AudioEditCompSegment& segment) {
            return timelineFrame >= segment.timelineStartFrame &&
                   timelineFrame - segment.timelineStartFrame < segment.timelineFrameCount;
        });
    if (it == track->compSegments.end()) return false;
    track->compSegments.erase(it);
    return true;
}

void clear_audio_comp_segments(AudioEditSession& session, AudioEditTrackId trackId) noexcept {
    if (AudioEditTrack* track = find_track(session, trackId)) track->compSegments.clear();
}

std::uint32_t audio_comp_take_lane_at_frame(const AudioEditTrack& track,
                                             std::uint64_t frame) noexcept {
    const auto upper = std::upper_bound(track.compSegments.begin(), track.compSegments.end(), frame,
        [](std::uint64_t value, const AudioEditCompSegment& segment) {
            return value < segment.timelineStartFrame;
        });
    if (upper != track.compSegments.begin()) {
        const auto& segment = *(upper - 1);
        if (frame >= segment.timelineStartFrame &&
            frame - segment.timelineStartFrame < segment.timelineFrameCount) return segment.takeLane;
    }
    return track.activeTakeLane;
}

float evaluate_audio_automation(const AudioEditTrack& track,
                                AudioEditAutomationParameter parameter,
                                std::uint64_t frame,
                                float fallback) noexcept {
    const auto laneIt = std::find_if(track.automation.begin(), track.automation.end(),
        [parameter](const AudioEditAutomationLane& lane) { return lane.parameter == parameter && lane.enabled; });
    if (laneIt == track.automation.end() || laneIt->points.empty()) return fallback;
    const auto& points = laneIt->points;
    if (frame <= points.front().frame) return points.front().value;
    if (frame >= points.back().frame) return points.back().value;
    const auto upper = std::upper_bound(points.begin(), points.end(), frame,
        [](std::uint64_t value, const AudioEditAutomationPoint& point) { return value < point.frame; });
    const auto& right = *upper;
    const auto& left = *(upper - 1);
    const double span = static_cast<double>(right.frame - left.frame);
    const float t = span <= 0.0 ? 0.0F : static_cast<float>(static_cast<double>(frame - left.frame) / span);
    return automation_curve_value(left.value, right.value, t, left.curve);
}

std::uint64_t audio_edit_length_frames(const AudioEditSession& session) noexcept {
    std::uint64_t length{};
    for (const auto& track : session.tracks) {
        for (const auto& clip : track.clips) {
            if (clip.timelineFrameCount <= std::numeric_limits<std::uint64_t>::max() - clip.timelineStartFrame)
                length = std::max(length, clip.timelineStartFrame + clip.timelineFrameCount);
        }
    }
    return length;
}

double audio_edit_frame_to_beats(const AudioEditSession& session, std::uint64_t frame) noexcept {
    if (session.sampleRate == 0U || session.tempoMap.empty()) return 0.0;
    double beats{};
    for (std::size_t i = 0; i < session.tempoMap.size(); ++i) {
        const auto& point = session.tempoMap[i];
        const std::uint64_t nextFrame = i + 1U < session.tempoMap.size() ? session.tempoMap[i + 1U].frame : frame;
        if (frame <= point.frame) break;
        const std::uint64_t segmentEnd = std::min(frame, nextFrame);
        if (segmentEnd > point.frame) {
            const double seconds = static_cast<double>(segmentEnd - point.frame) / static_cast<double>(session.sampleRate);
            beats += seconds * point.beatsPerMinute / 60.0;
        }
        if (frame < nextFrame) break;
    }
    return beats;
}

std::uint64_t audio_edit_beats_to_frame(const AudioEditSession& session, double beats) noexcept {
    if (session.sampleRate == 0U || session.tempoMap.empty() || !std::isfinite(beats) || beats <= 0.0) return 0U;
    double remaining = beats;
    for (std::size_t i = 0; i < session.tempoMap.size(); ++i) {
        const auto& point = session.tempoMap[i];
        if (i + 1U < session.tempoMap.size()) {
            const double seconds = static_cast<double>(session.tempoMap[i + 1U].frame - point.frame) /
                                   static_cast<double>(session.sampleRate);
            const double segmentBeats = seconds * point.beatsPerMinute / 60.0;
            if (remaining > segmentBeats) { remaining -= segmentBeats; continue; }
        }
        const double frames = remaining * 60.0 / point.beatsPerMinute * static_cast<double>(session.sampleRate);
        if (frames >= static_cast<double>(std::numeric_limits<std::uint64_t>::max() - point.frame))
            return std::numeric_limits<std::uint64_t>::max();
        return point.frame + static_cast<std::uint64_t>(std::llround(frames));
    }
    return 0U;
}

std::uint64_t audio_edit_snap_frame_to_beat(const AudioEditSession& session,
                                             std::uint64_t frame,
                                             std::uint32_t subdivisions) noexcept {
    subdivisions = std::clamp<std::uint32_t>(subdivisions, 1U, 64U);
    const double beats = audio_edit_frame_to_beats(session, frame);
    const double snapped = std::round(beats * static_cast<double>(subdivisions)) /
                           static_cast<double>(subdivisions);
    return audio_edit_beats_to_frame(session, snapped);
}

void render_audio_edit_session(const AudioEditSession& session,
                               const AudioEditSourceLibrary& sources,
                               std::uint64_t startFrame,
                               std::span<float> output) noexcept {
    const std::size_t outputFrames = output.size() / 2U;
    std::fill(output.begin(), output.end(), 0.0F);
    const bool anySolo = std::any_of(session.tracks.begin(), session.tracks.end(),
                                     [](const AudioEditTrack& track) { return track.solo; });
    for (const auto& track : session.tracks) {
        if (track.mute || (anySolo && !track.solo)) continue;
        for (const auto& clip : track.clips) {
            if (clip.muted || clip.sourceFrameCount == 0U || clip.timelineFrameCount == 0U) continue;
            const auto source = sources.resolve(clip.source);
            if (!source || source->metadata.channels == 0U || source->metadata.frameCount == 0U) continue;
            if (startFrame > std::numeric_limits<std::uint64_t>::max() - static_cast<std::uint64_t>(outputFrames)) continue;
            const std::uint64_t requestEnd = startFrame + static_cast<std::uint64_t>(outputFrames);
            const std::uint64_t clipEnd = clip.timelineStartFrame + clip.timelineFrameCount;
            if (clipEnd <= startFrame || clip.timelineStartFrame >= requestEnd) continue;
            const std::uint64_t first = std::max(startFrame, clip.timelineStartFrame);
            const std::uint64_t last = std::min(requestEnd, clipEnd);
            float clipLeft{};
            float clipRight{};
            stereo_gains(clip.gain, clip.pan, clipLeft, clipRight);
            for (std::uint64_t timelineFrame = first; timelineFrame < last; ++timelineFrame) {
                const std::uint64_t local = timelineFrame - clip.timelineStartFrame;
                if (!clip.loopSource && local >= clip.sourceFrameCount) continue;
                std::uint64_t sourceLocal = clip.loopSource ? local % clip.sourceFrameCount : local;
                if (clip.reverse) sourceLocal = clip.sourceFrameCount - 1U - sourceLocal;
                const std::uint64_t sourceFrame = clip.sourceStartFrame + sourceLocal;
                if (sourceFrame >= source->metadata.frameCount) continue;
                const std::size_t sourceIndex = static_cast<std::size_t>(sourceFrame) * source->metadata.channels;
                const float sourceLeft = source->samples[sourceIndex];
                const float sourceRight = source->metadata.channels == 2U ? source->samples[sourceIndex + 1U] : sourceLeft;
                const float trackGain = evaluate_audio_automation(track, AudioEditAutomationParameter::Gain,
                                                                   timelineFrame, track.gain);
                const float trackPan = evaluate_audio_automation(track, AudioEditAutomationParameter::Pan,
                                                                  timelineFrame, track.pan);
                float trackLeft{};
                float trackRight{};
                stereo_gains(trackGain, trackPan, trackLeft, trackRight);
                const float envelope = clip_envelope(clip, local) *
                                       comp_segment_envelope(track, clip.takeLane, timelineFrame);
                if (envelope <= 0.0F) continue;
                const std::size_t outIndex = static_cast<std::size_t>(timelineFrame - startFrame) * 2U;
                output[outIndex] += sourceLeft * clipLeft * trackLeft * envelope;
                output[outIndex + 1U] += sourceRight * clipRight * trackRight * envelope;
            }
        }
    }
    for (float& value : output) value = std::tanh(value);
}

DecodedAudioAsset bounce_audio_edit_session(const AudioEditSession& session,
                                             const AudioEditSourceLibrary& sources,
                                             std::string name) {
    DecodedAudioAsset result;
    result.metadata.name = name.empty() ? session.name : std::move(name);
    result.metadata.sampleRate = session.sampleRate;
    result.metadata.channels = 2;
    result.metadata.storagePolicy = AudioStoragePolicy::Resident;
    const std::uint64_t length = audio_edit_length_frames(session);
    if (length > std::numeric_limits<std::size_t>::max() / 2U) return result;
    result.samples.resize(static_cast<std::size_t>(length) * 2U);
    render_audio_edit_session(session, sources, 0U, result.samples);
    result.metadata.frameCount = length;
    result.metadata.durationSeconds = session.sampleRate == 0U ? 0.0 :
        static_cast<double>(length) / static_cast<double>(session.sampleRate);
    double energy{};
    for (const float sample : result.samples) {
        result.metadata.peakLinear = std::max(result.metadata.peakLinear, std::abs(sample));
        energy += static_cast<double>(sample) * sample;
    }
    if (!result.samples.empty()) {
        result.metadata.rmsLinear = static_cast<float>(std::sqrt(energy / static_cast<double>(result.samples.size())));
        result.metadata.approximateLoudnessDbfs = result.metadata.rmsLinear > 1.0e-9F
            ? 20.0F * std::log10(result.metadata.rmsLinear) : -120.0F;
    }
    result.metadata.contentHash = audio_content_hash(result.samples, result.metadata);
    return result;
}

bool write_audio_edit_session(const std::filesystem::path& path, const AudioEditSession& input,
                              std::string* error) {
    AudioEditSession session = input;
    if (!validate_loaded_session(session, error)) return false;
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) { if (error) *error = "unable to create audio edit session"; return false; }
    stream.write(kMagic.data(), static_cast<std::streamsize>(kMagic.size()));
    if (!write_value(stream, kCurrentVersion) || !write_string(stream, session.name) ||
        !write_value(stream, session.sampleRate) || !write_value(stream, session.loop.beginFrame) ||
        !write_value(stream, session.loop.endFrame) || !write_value(stream, session.loop.enabled) ||
        !write_value(stream, session.nextSourceId) || !write_value(stream, session.nextTrackId) ||
        !write_value(stream, session.nextClipId) ||
        !write_value(stream, session.punch.beginFrame) || !write_value(stream, session.punch.endFrame) ||
        !write_value(stream, session.punch.preRollFrames) || !write_value(stream, session.punch.postRollFrames) ||
        !write_value(stream, session.punch.enabled)) {
        if (error) *error = "failed to write audio edit header";
        return false;
    }
    const auto markerCount = static_cast<std::uint32_t>(session.markers.size());
    const auto tempoCount = static_cast<std::uint32_t>(session.tempoMap.size());
    const auto sourceCount = static_cast<std::uint32_t>(session.sources.size());
    const auto trackCount = static_cast<std::uint32_t>(session.tracks.size());
    if (!write_value(stream, markerCount) || !write_value(stream, tempoCount) ||
        !write_value(stream, sourceCount) || !write_value(stream, trackCount)) return false;
    for (const auto& marker : session.markers) {
        if (!write_value(stream, marker.frame) || !write_string(stream, marker.name)) return false;
    }
    for (const auto& tempo : session.tempoMap) {
        if (!write_value(stream, tempo.frame) || !write_value(stream, tempo.beatsPerMinute) ||
            !write_value(stream, tempo.numerator) || !write_value(stream, tempo.denominator)) return false;
    }
    for (const auto& source : session.sources) {
        const std::string pathText = source.path.generic_string();
        if (!write_value(stream, source.id.value) || !write_string(stream, pathText) ||
            !write_string(stream, source.name) || !write_value(stream, source.contentHash) ||
            !write_value(stream, source.sampleRate) || !write_value(stream, source.channels) ||
            !write_value(stream, source.frameCount)) return false;
    }
    for (const auto& track : session.tracks) {
        const auto automationCount = static_cast<std::uint32_t>(track.automation.size());
        const auto sendCount = static_cast<std::uint32_t>(track.sends.size());
        const auto compCount = static_cast<std::uint32_t>(track.compSegments.size());
        const auto clipCount = static_cast<std::uint32_t>(track.clips.size());
        if (!write_value(stream, track.id.value) || !write_string(stream, track.name) ||
            !write_value(stream, track.gain) || !write_value(stream, track.pan) ||
            !write_value(stream, track.mute) || !write_value(stream, track.solo) ||
            !write_value(stream, track.armed) || !write_value(stream, track.takeLaneCount) ||
            !write_value(stream, track.activeTakeLane) || !write_value(stream, automationCount) ||
            !write_value(stream, sendCount) || !write_value(stream, compCount) ||
            !write_value(stream, clipCount)) return false;
        for (const auto& lane : track.automation) {
            const auto parameter = static_cast<std::uint8_t>(lane.parameter);
            const auto pointCount = static_cast<std::uint32_t>(lane.points.size());
            if (!write_value(stream, parameter) || !write_value(stream, lane.enabled) ||
                !write_value(stream, pointCount)) return false;
            for (const auto& point : lane.points) {
                const auto curve = static_cast<std::uint8_t>(point.curve);
                if (!write_value(stream, point.frame) || !write_value(stream, point.value) ||
                    !write_value(stream, curve)) return false;
            }
        }
        for (const auto& send : track.sends) {
            const auto target = static_cast<std::uint8_t>(send.target);
            if (!write_value(stream, target) || !write_value(stream, send.gain) ||
                !write_value(stream, send.preFader) || !write_value(stream, send.enabled)) return false;
        }
        for (const auto& segment : track.compSegments) {
            if (!write_value(stream, segment.timelineStartFrame) ||
                !write_value(stream, segment.timelineFrameCount) ||
                !write_value(stream, segment.takeLane) ||
                !write_value(stream, segment.fadeInFrames) ||
                !write_value(stream, segment.fadeOutFrames)) return false;
        }
        for (const auto& clip : track.clips) {
            const auto fadeInCurve = static_cast<std::uint8_t>(clip.fadeInCurve);
            const auto fadeOutCurve = static_cast<std::uint8_t>(clip.fadeOutCurve);
            if (!write_value(stream, clip.id.value) || !write_value(stream, clip.source.value) ||
                !write_value(stream, clip.timelineStartFrame) || !write_value(stream, clip.sourceStartFrame) ||
                !write_value(stream, clip.sourceFrameCount) || !write_value(stream, clip.timelineFrameCount) ||
                !write_value(stream, clip.gain) || !write_value(stream, clip.pan) ||
                !write_value(stream, clip.fadeInFrames) || !write_value(stream, clip.fadeOutFrames) ||
                !write_value(stream, fadeInCurve) || !write_value(stream, fadeOutCurve) ||
                !write_value(stream, clip.takeLane) || !write_value(stream, clip.reverse) ||
                !write_value(stream, clip.loopSource) || !write_value(stream, clip.muted)) return false;
        }
    }
    if (!stream) { if (error) *error = "failed to write audio edit session"; return false; }
    return true;
}

std::optional<AudioEditSession> read_audio_edit_session(const std::filesystem::path& path,
                                                         std::string* error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) { if (error) *error = "unable to open audio edit session"; return std::nullopt; }
    std::array<char, 8> magic{};
    std::uint32_t version{};
    stream.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!stream || magic != kMagic || !read_value(stream, version) || (version < 1U || version > kCurrentVersion)) {
        if (error) *error = "unsupported audio edit session format";
        return std::nullopt;
    }
    AudioEditSession session;
    session.version = version;
    if (!read_string(stream, session.name) || !read_value(stream, session.sampleRate) ||
        !read_value(stream, session.loop.beginFrame) || !read_value(stream, session.loop.endFrame) ||
        !read_value(stream, session.loop.enabled) || !read_value(stream, session.nextSourceId) ||
        !read_value(stream, session.nextTrackId) || !read_value(stream, session.nextClipId)) {
        if (error) *error = "truncated audio edit header";
        return std::nullopt;
    }
    std::uint32_t markerCount{};
    std::uint32_t tempoCount{};
    std::uint32_t sourceCount{};
    std::uint32_t trackCount{};
    if (version >= 2U) {
        if (!read_value(stream, session.punch.beginFrame) || !read_value(stream, session.punch.endFrame) ||
            !read_value(stream, session.punch.preRollFrames) || !read_value(stream, session.punch.postRollFrames) ||
            !read_value(stream, session.punch.enabled) || !read_value(stream, markerCount) ||
            !read_value(stream, tempoCount) || markerCount > kMaximumMarkers || tempoCount > kMaximumTempoPoints) {
            if (error) *error = "invalid audio edit v2 header";
            return std::nullopt;
        }
    }
    if (!read_value(stream, sourceCount) || !read_value(stream, trackCount) ||
        sourceCount > kMaximumSources || trackCount > kMaximumTracks) {
        if (error) *error = "invalid audio edit counts";
        return std::nullopt;
    }
    if (version >= 2U) {
        session.markers.resize(markerCount);
        for (auto& marker : session.markers) {
            if (!read_value(stream, marker.frame) || !read_string(stream, marker.name)) {
                if (error) *error = "truncated audio edit marker";
                return std::nullopt;
            }
        }
        session.tempoMap.resize(tempoCount);
        for (auto& tempo : session.tempoMap) {
            if (!read_value(stream, tempo.frame) || !read_value(stream, tempo.beatsPerMinute) ||
                !read_value(stream, tempo.numerator) || !read_value(stream, tempo.denominator)) {
                if (error) *error = "truncated audio edit tempo map";
                return std::nullopt;
            }
        }
    }
    session.sources.resize(sourceCount);
    for (auto& source : session.sources) {
        std::string pathText;
        if (!read_value(stream, source.id.value) || !read_string(stream, pathText) ||
            !read_string(stream, source.name) || !read_value(stream, source.contentHash) ||
            !read_value(stream, source.sampleRate) || !read_value(stream, source.channels) ||
            !read_value(stream, source.frameCount)) {
            if (error) *error = "truncated audio edit source";
            return std::nullopt;
        }
        source.path = std::filesystem::path(pathText);
    }
    session.tracks.resize(trackCount);
    for (auto& track : session.tracks) {
        std::uint32_t clipCount{};
        if (!read_value(stream, track.id.value) || !read_string(stream, track.name) ||
            !read_value(stream, track.gain) || !read_value(stream, track.pan) ||
            !read_value(stream, track.mute) || !read_value(stream, track.solo)) {
            if (error) *error = "invalid audio edit track";
            return std::nullopt;
        }
        if (version >= 2U) {
            std::uint32_t automationCount{};
            std::uint32_t sendCount{};
            std::uint32_t compCount{};
            if (!read_value(stream, track.armed) || !read_value(stream, track.takeLaneCount) ||
                !read_value(stream, track.activeTakeLane) || !read_value(stream, automationCount) ||
                !read_value(stream, sendCount) ||
                (version >= 3U && !read_value(stream, compCount)) ||
                !read_value(stream, clipCount) ||
                automationCount > kMaximumAutomationLanes || sendCount > kMaximumSends ||
                compCount > kMaximumCompSegments || clipCount > kMaximumClipsPerTrack) {
                if (error) *error = "invalid audio edit v2 track";
                return std::nullopt;
            }
            track.automation.resize(automationCount);
            for (auto& lane : track.automation) {
                std::uint8_t parameter{};
                std::uint32_t pointCount{};
                if (!read_value(stream, parameter) || !read_value(stream, lane.enabled) ||
                    !read_value(stream, pointCount) || pointCount > kMaximumAutomationPoints) {
                    if (error) *error = "invalid audio automation lane";
                    return std::nullopt;
                }
                lane.parameter = static_cast<AudioEditAutomationParameter>(parameter);
                lane.points.resize(pointCount);
                for (auto& point : lane.points) {
                    std::uint8_t curve{};
                    if (!read_value(stream, point.frame) || !read_value(stream, point.value) ||
                        !read_value(stream, curve)) {
                        if (error) *error = "truncated audio automation point";
                        return std::nullopt;
                    }
                    point.curve = static_cast<AudioEditAutomationCurve>(curve);
                }
            }
            track.sends.resize(sendCount);
            for (auto& send : track.sends) {
                std::uint8_t target{};
                if (!read_value(stream, target) || !read_value(stream, send.gain) ||
                    !read_value(stream, send.preFader) || !read_value(stream, send.enabled)) {
                    if (error) *error = "truncated audio edit send";
                    return std::nullopt;
                }
                send.target = static_cast<AudioEditSendTarget>(target);
            }
            if (version >= 3U) {
                track.compSegments.resize(compCount);
                for (auto& segment : track.compSegments) {
                    if (!read_value(stream, segment.timelineStartFrame) ||
                        !read_value(stream, segment.timelineFrameCount) ||
                        !read_value(stream, segment.takeLane) ||
                        !read_value(stream, segment.fadeInFrames) ||
                        !read_value(stream, segment.fadeOutFrames)) {
                        if (error) *error = "truncated audio comp segment";
                        return std::nullopt;
                    }
                }
            }
        } else if (!read_value(stream, clipCount) || clipCount > kMaximumClipsPerTrack) {
            if (error) *error = "invalid audio edit track clip count";
            return std::nullopt;
        }
        track.clips.resize(clipCount);
        for (auto& clip : track.clips) {
            if (!read_value(stream, clip.id.value) || !read_value(stream, clip.source.value) ||
                !read_value(stream, clip.timelineStartFrame) || !read_value(stream, clip.sourceStartFrame) ||
                !read_value(stream, clip.sourceFrameCount) || !read_value(stream, clip.timelineFrameCount) ||
                !read_value(stream, clip.gain) || !read_value(stream, clip.pan) ||
                !read_value(stream, clip.fadeInFrames) || !read_value(stream, clip.fadeOutFrames)) {
                if (error) *error = "truncated audio edit clip";
                return std::nullopt;
            }
            if (version >= 2U) {
                std::uint8_t fadeInCurve{};
                std::uint8_t fadeOutCurve{};
                if (!read_value(stream, fadeInCurve) || !read_value(stream, fadeOutCurve) ||
                    !read_value(stream, clip.takeLane)) {
                    if (error) *error = "truncated audio edit v2 clip";
                    return std::nullopt;
                }
                clip.fadeInCurve = static_cast<AudioEditFadeCurve>(fadeInCurve);
                clip.fadeOutCurve = static_cast<AudioEditFadeCurve>(fadeOutCurve);
            }
            if (!read_value(stream, clip.reverse) || !read_value(stream, clip.loopSource) ||
                !read_value(stream, clip.muted)) {
                if (error) *error = "truncated audio edit clip flags";
                return std::nullopt;
            }
        }
    }
    if (!validate_loaded_session(session, error)) return std::nullopt;
    if (stream.peek() != std::char_traits<char>::eof()) {
        if (error != nullptr) *error = "trailing data in audio edit session";
        return std::nullopt;
    }
    return session;
}

AudioEditHistory::AudioEditHistory(std::size_t maximumSnapshots)
    : maximumSnapshots_(std::max<std::size_t>(2U, maximumSnapshots)) {}

void AudioEditHistory::reset(const AudioEditSession& session) {
    snapshots_.assign(1U, session);
    cursor_ = 0U;
}

void AudioEditHistory::commit(const AudioEditSession& session) {
    if (snapshots_.empty()) { reset(session); return; }
    snapshots_.erase(snapshots_.begin() + static_cast<std::ptrdiff_t>(cursor_ + 1U), snapshots_.end());
    snapshots_.push_back(session);
    if (snapshots_.size() > maximumSnapshots_) snapshots_.erase(snapshots_.begin());
    cursor_ = snapshots_.size() - 1U;
}

bool AudioEditHistory::can_undo() const noexcept { return !snapshots_.empty() && cursor_ > 0U; }
bool AudioEditHistory::can_redo() const noexcept { return cursor_ + 1U < snapshots_.size(); }
bool AudioEditHistory::undo(AudioEditSession& session) {
    if (!can_undo()) return false;
    session = snapshots_[--cursor_];
    return true;
}
bool AudioEditHistory::redo(AudioEditSession& session) {
    if (!can_redo()) return false;
    session = snapshots_[++cursor_];
    return true;
}

void AudioEditTransport::play() noexcept { state_ = AudioTransportState::Playing; }
void AudioEditTransport::pause() noexcept {
    if (state_ == AudioTransportState::Playing) state_ = AudioTransportState::Paused;
}
void AudioEditTransport::stop() noexcept { state_ = AudioTransportState::Stopped; cursor_ = 0U; }
void AudioEditTransport::rewind() noexcept { cursor_ = 0U; }
void AudioEditTransport::seek(std::uint64_t frame) noexcept { cursor_ = frame; }

void AudioEditTransport::render(const AudioEditSession& session,
                                const AudioEditSourceLibrary& sources,
                                std::span<float> output) noexcept {
    std::fill(output.begin(), output.end(), 0.0F);
    if (state_ != AudioTransportState::Playing) return;
    const std::size_t frames = output.size() / 2U;
    if (frames == 0U) return;
    std::size_t rendered{};
    while (rendered < frames) {
        if (session.loop.enabled && session.loop.endFrame > session.loop.beginFrame &&
            cursor_ >= session.loop.endFrame) cursor_ = session.loop.beginFrame;
        const std::uint64_t sessionLength = audio_edit_length_frames(session);
        if (!session.loop.enabled && cursor_ >= sessionLength) { state_ = AudioTransportState::Stopped; break; }
        std::size_t chunk = frames - rendered;
        if (session.loop.enabled && session.loop.endFrame > cursor_) {
            chunk = std::min<std::size_t>(chunk, static_cast<std::size_t>(session.loop.endFrame - cursor_));
        } else if (!session.loop.enabled && sessionLength > cursor_) {
            chunk = std::min<std::size_t>(chunk, static_cast<std::size_t>(sessionLength - cursor_));
        }
        if (chunk == 0U) break;
        render_audio_edit_session(session, sources, cursor_, output.subspan(rendered * 2U, chunk * 2U));
        cursor_ += chunk;
        rendered += chunk;
    }
}

} // namespace dve::audio
