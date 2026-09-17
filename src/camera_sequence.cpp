#include "dve/camera_sequence.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace dve::camera {
namespace {
float lerp(float a, float b, float t) noexcept { return a + (b - a) * t; }
Float3 lerp(Float3 a, Float3 b, float t) noexcept { return add(a, multiply(subtract(b, a), t)); }

Float3 catmull(Float3 p0, Float3 p1, Float3 p2, Float3 p3, float t) noexcept {
    const float t2 = t * t;
    const float t3 = t2 * t;
    return multiply(add(add(multiply(p1, 2.0F), multiply(subtract(p2, p0), t)),
                        add(multiply(add(add(multiply(p0, 2.0F), multiply(p1, -5.0F)),
                                         add(multiply(p2, 4.0F), multiply(p3, -1.0F))), t2),
                            multiply(add(add(multiply(p0, -1.0F), multiply(p1, 3.0F)),
                                         add(multiply(p2, -3.0F), p3)), t3))), 0.5F);
}

CameraLens blend_lens(CameraLens a, const CameraLens& b, float t) noexcept {
    a.verticalFieldOfViewRadians = blend_camera_vertical_fov(a.verticalFieldOfViewRadians, b.verticalFieldOfViewRadians, t);
    a.orthographicHeightMeters = lerp(a.orthographicHeightMeters, b.orthographicHeightMeters, t);
    a.nearPlaneMeters = lerp(a.nearPlaneMeters, b.nearPlaneMeters, t);
    a.farPlaneMeters = lerp(a.farPlaneMeters, b.farPlaneMeters, t);
    a.aspectRatio = lerp(a.aspectRatio, b.aspectRatio, t);
    a.physical.enabled = t < 0.5F ? a.physical.enabled : b.physical.enabled;
    // Same reasoning as camera_system.cpp: angular field is roughly logarithmic in focal length,
    // focus is linear in diopters, aperture is logarithmic in stops. A dolly key pair authored as
    // a 24 mm to 200 mm zoom or a 1 m to 100 m focus pull is unusable interpolated in raw units.
    a.physical.focalLengthMillimeters = blend_camera_focal_length(a.physical.focalLengthMillimeters,
                                                                  b.physical.focalLengthMillimeters, t);
    a.physical.sensorWidthMillimeters = lerp(a.physical.sensorWidthMillimeters,
                                             b.physical.sensorWidthMillimeters, t);
    a.physical.sensorHeightMillimeters = lerp(a.physical.sensorHeightMillimeters,
                                              b.physical.sensorHeightMillimeters, t);
    a.physical.lensShiftX = lerp(a.physical.lensShiftX, b.physical.lensShiftX, t);
    a.physical.lensShiftY = lerp(a.physical.lensShiftY, b.physical.lensShiftY, t);
    a.physical.focusDistanceMeters = blend_camera_focus_distance(a.physical.focusDistanceMeters,
                                                                 b.physical.focusDistanceMeters, t);
    a.physical.apertureFStop = blend_camera_f_stop(a.physical.apertureFStop, b.physical.apertureFStop, t);
    return a;
}

CameraPostProcessProfile blend_post(CameraPostProcessProfile a,
                                    const CameraPostProcessProfile& b,
                                    float t) noexcept {
    return blend_camera_post_process(a, b, t);
}

float curve_weight(CameraBlendCurve curve, float t) noexcept {
    t = std::clamp(t, 0.0F, 1.0F);
    switch (curve) {
        case CameraBlendCurve::Cut: return 1.0F;
        case CameraBlendCurve::Linear: return t;
        case CameraBlendCurve::EaseIn: return t * t;
        case CameraBlendCurve::EaseOut: return 1.0F - (1.0F - t) * (1.0F - t);
        case CameraBlendCurve::EaseInOut:
            return t < 0.5F ? 2.0F * t * t : 1.0F - 2.0F * (1.0F - t) * (1.0F - t);
        case CameraBlendCurve::SmoothStep: return t * t * (3.0F - 2.0F * t);
    }
    return t;
}

CameraDollyKey blend_keys(const CameraDollyKey& a, const CameraDollyKey& b, float t) noexcept {
    CameraDollyKey result = b;
    result.pose.position = lerp(a.pose.position, b.pose.position, t);
    result.pose.target = lerp(a.pose.target, b.pose.target, t);
    result.pose.worldUp = normalize(lerp(a.pose.worldUp, b.pose.worldUp, t));
    result.pose.lens = blend_lens(a.pose.lens, b.pose.lens, t);
    result.pose.postProcessWeight = lerp(a.pose.postProcessWeight, b.pose.postProcessWeight, t);
    result.postProcess = blend_post(a.postProcess, b.postProcess, t);
    return result;
}

std::string curve_name(CameraBlendCurve curve) {
    switch (curve) {
        case CameraBlendCurve::Cut: return "Cut";
        case CameraBlendCurve::Linear: return "Linear";
        case CameraBlendCurve::EaseIn: return "EaseIn";
        case CameraBlendCurve::EaseOut: return "EaseOut";
        case CameraBlendCurve::EaseInOut: return "EaseInOut";
        case CameraBlendCurve::SmoothStep: return "SmoothStep";
    }
    return "Cut";
}

std::optional<CameraBlendCurve> parse_curve(std::string_view value) {
    if (value == "Cut") return CameraBlendCurve::Cut;
    if (value == "Linear") return CameraBlendCurve::Linear;
    if (value == "EaseIn") return CameraBlendCurve::EaseIn;
    if (value == "EaseOut") return CameraBlendCurve::EaseOut;
    if (value == "EaseInOut") return CameraBlendCurve::EaseInOut;
    if (value == "SmoothStep") return CameraBlendCurve::SmoothStep;
    return std::nullopt;
}


void write_post_process(std::ostream& out,const CameraPostProcessProfile& post) {
    const auto& c=post.cinematic;
    out << post.exposure << ' ' << post.bloomIntensity << ' ' << post.saturation << ' '
        << post.contrast << ' ' << post.vignette << ' ' << post.depthOfFieldWeight << ' '
        << post.focusDistanceMeters << ' ' << post.apertureFStop << ' '
        << post.colorTint.x << ' ' << post.colorTint.y << ' ' << post.colorTint.z << ' '
        << static_cast<std::uint32_t>(c.colorGrade.toneMap) << ' '
        << c.colorGrade.temperatureKelvin << ' ' << c.colorGrade.tint << ' '
        << c.colorGrade.lift.x << ' ' << c.colorGrade.lift.y << ' ' << c.colorGrade.lift.z << ' '
        << c.colorGrade.gamma.x << ' ' << c.colorGrade.gamma.y << ' ' << c.colorGrade.gamma.z << ' '
        << c.colorGrade.gain.x << ' ' << c.colorGrade.gain.y << ' ' << c.colorGrade.gain.z << ' '
        << c.colorGrade.lutBlend << ' '
        << static_cast<std::uint32_t>(c.lens.distortionModel) << ' '
        << c.lens.radialK1 << ' ' << c.lens.radialK2 << ' ' << c.lens.radialK3 << ' '
        << c.lens.fisheyeStrength << ' ' << c.lens.chromaticAberrationPixels << ' '
        << c.lens.anamorphicSqueeze << ' ' << c.lens.lensBreathing << ' '
        << c.lens.anamorphicFlareIntensity << ' ' << c.lens.anamorphicFlareThreshold << ' '
        << c.lens.gateWeavePixels << ' '
        << c.bokeh.bladeCount << ' ' << c.bokeh.bladeRotationRadians << ' ' << c.bokeh.roundness << ' '
        << c.bokeh.anamorphicRatio << ' ' << c.bokeh.catEye << ' '
        << (c.splitDiopter.enabled?1:0) << ' ' << c.splitDiopter.nearFocusDistanceMeters << ' '
        << c.splitDiopter.farFocusDistanceMeters << ' ' << c.splitDiopter.centerX << ' '
        << c.splitDiopter.centerY << ' ' << c.splitDiopter.angleRadians << ' '
        << c.splitDiopter.featherFraction << ' '
        << c.film.grainIntensity << ' ' << c.film.halationIntensity << ' '
        << c.film.sharpenIntensity << ' ' << c.film.motionBlurWeight << ' '
        << c.film.shutterAngleDegrees << ' ' << c.film.grainSeed << ' '
        << static_cast<std::uint32_t>(c.film.framing) << ' ' << c.film.customAspectRatio << ' '
        << c.film.matteOpacity << ' ' << c.film.matteColor.x << ' ' << c.film.matteColor.y << ' '
        << c.film.matteColor.z;
}

bool read_post_process(std::istream& in,CameraPostProcessProfile& post,int version) {
    if(version<=2){
        return static_cast<bool>(in >> post.exposure >> post.bloomIntensity >> post.saturation >>
            post.contrast >> post.vignette >> post.depthOfFieldWeight >> post.colorTint.x >>
            post.colorTint.y >> post.colorTint.z);
    }
    std::uint32_t toneMap{},distortion{},framing{};
    int splitEnabled{};
    auto& c=post.cinematic;
    if(!(in >> post.exposure >> post.bloomIntensity >> post.saturation >> post.contrast >>
         post.vignette >> post.depthOfFieldWeight >> post.focusDistanceMeters >>
         post.apertureFStop >> post.colorTint.x >> post.colorTint.y >> post.colorTint.z >>
         toneMap >> c.colorGrade.temperatureKelvin >> c.colorGrade.tint >>
         c.colorGrade.lift.x >> c.colorGrade.lift.y >> c.colorGrade.lift.z >>
         c.colorGrade.gamma.x >> c.colorGrade.gamma.y >> c.colorGrade.gamma.z >>
         c.colorGrade.gain.x >> c.colorGrade.gain.y >> c.colorGrade.gain.z >>
         c.colorGrade.lutBlend >> distortion >> c.lens.radialK1 >> c.lens.radialK2 >>
         c.lens.radialK3 >> c.lens.fisheyeStrength >> c.lens.chromaticAberrationPixels >>
         c.lens.anamorphicSqueeze >> c.lens.lensBreathing >> c.lens.anamorphicFlareIntensity >>
         c.lens.anamorphicFlareThreshold >> c.lens.gateWeavePixels >> c.bokeh.bladeCount >>
         c.bokeh.bladeRotationRadians >> c.bokeh.roundness >> c.bokeh.anamorphicRatio >>
         c.bokeh.catEye >> splitEnabled >> c.splitDiopter.nearFocusDistanceMeters >>
         c.splitDiopter.farFocusDistanceMeters >> c.splitDiopter.centerX >>
         c.splitDiopter.centerY >> c.splitDiopter.angleRadians >> c.splitDiopter.featherFraction >>
         c.film.grainIntensity >> c.film.halationIntensity >> c.film.sharpenIntensity >>
         c.film.motionBlurWeight >> c.film.shutterAngleDegrees >> c.film.grainSeed >> framing >>
         c.film.customAspectRatio >> c.film.matteOpacity >> c.film.matteColor.x >>
         c.film.matteColor.y >> c.film.matteColor.z))return false;
    if(toneMap>static_cast<std::uint32_t>(CameraToneMapCurve::AcesFilmic)||
       distortion>static_cast<std::uint32_t>(CameraLensDistortionModel::FisheyeEquidistant)||
       framing>static_cast<std::uint32_t>(CameraFramingPreset::Custom)||
       (splitEnabled!=0&&splitEnabled!=1))return false;
    c.colorGrade.toneMap=static_cast<CameraToneMapCurve>(toneMap);
    c.lens.distortionModel=static_cast<CameraLensDistortionModel>(distortion);
    c.film.framing=static_cast<CameraFramingPreset>(framing);
    c.splitDiopter.enabled=splitEnabled!=0;
    return true;
}

} // namespace

bool CameraDollyKey::validate(std::string* error) const {
    if (!std::isfinite(timeSeconds) || timeSeconds < 0.0F) {
        if (error) *error = "dolly key time is invalid";
        return false;
    }
    std::string local;
    if (!pose.lens.validate(&local) || !postProcess.validate(&local)) {
        if (error) *error = local;
        return false;
    }
    const float values[]{pose.position.x, pose.position.y, pose.position.z,
                         pose.target.x, pose.target.y, pose.target.z,
                         pose.worldUp.x, pose.worldUp.y, pose.worldUp.z};
    for (float value : values) if (!std::isfinite(value)) {
        if (error) *error = "dolly key pose is not finite";
        return false;
    }
    return true;
}

bool CameraDollySpline::validate(std::string* error) const {
    auto fail = [&](std::string message) {
        if (error) *error = std::move(message);
        return false;
    };
    if (id == 0U || name.empty() || keys.size() < 2U || keys.size() > 100000U)
        return fail("dolly requires id, name, and at least two keys");
    if (arcLengthSamplesPerSegment < 2U || arcLengthSamplesPerSegment > 4096U)
        return fail("dolly arc-length sample count is out of range");
    float previous = -1.0F;
    for (const auto& key : keys) {
        if (!key.validate(error)) return false;
        if (key.timeSeconds <= previous) return fail("dolly key times must be strictly increasing");
        previous = key.timeSeconds;
    }
    return true;
}

CameraDollyKey CameraDollySpline::evaluate(float time) const noexcept {
    if (keys.empty()) return {};
    if (keys.size() == 1U || time <= keys.front().timeSeconds) return keys.front();
    if (time >= keys.back().timeSeconds) return keys.back();
    const auto upper = std::upper_bound(keys.begin(), keys.end(), time,
        [](float value, const CameraDollyKey& key) { return value < key.timeSeconds; });
    const std::size_t i2 = static_cast<std::size_t>(upper - keys.begin());
    const std::size_t i1 = i2 - 1U;
    const std::size_t i0 = i1 > 0U ? i1 - 1U : i1;
    const std::size_t i3 = std::min(i2 + 1U, keys.size() - 1U);
    const float t = (time - keys[i1].timeSeconds) /
                    (keys[i2].timeSeconds - keys[i1].timeSeconds);
    CameraDollyKey result = keys[i1];
    result.timeSeconds = time;
    result.pose.position = catmull(keys[i0].pose.position, keys[i1].pose.position,
                                   keys[i2].pose.position, keys[i3].pose.position, t);
    result.pose.target = catmull(keys[i0].pose.target, keys[i1].pose.target,
                                 keys[i2].pose.target, keys[i3].pose.target, t);
    result.pose.worldUp = normalize(lerp(keys[i1].pose.worldUp, keys[i2].pose.worldUp, t));
    result.pose.lens = blend_lens(keys[i1].pose.lens, keys[i2].pose.lens, t);
    result.pose.postProcessWeight = lerp(keys[i1].pose.postProcessWeight,
                                         keys[i2].pose.postProcessWeight, t);
    result.postProcess = blend_post(keys[i1].postProcess, keys[i2].postProcess, t);
    return result;
}

CameraDollyKey CameraDollySpline::evaluate_progress(float normalizedProgress,
                                                     bool constantSpeed) const noexcept {
    if (keys.empty()) return {};
    normalizedProgress = std::clamp(normalizedProgress, 0.0F, 1.0F);
    const float begin = keys.front().timeSeconds;
    const float end = keys.back().timeSeconds;
    if (!constantSpeed || keys.size() < 3U) return evaluate(lerp(begin, end, normalizedProgress));

    struct Sample { float time{}; float distance{}; };
    std::vector<Sample> samples;
    samples.reserve((keys.size() - 1U) * arcLengthSamplesPerSegment + 1U);
    CameraDollyKey previous = evaluate(begin);
    float distance = 0.0F;
    samples.push_back({begin, 0.0F});
    for (std::size_t segment = 0; segment + 1U < keys.size(); ++segment) {
        for (std::uint32_t sample = 1U; sample <= arcLengthSamplesPerSegment; ++sample) {
            const float local = static_cast<float>(sample) /
                                static_cast<float>(arcLengthSamplesPerSegment);
            const float time = lerp(keys[segment].timeSeconds, keys[segment + 1U].timeSeconds, local);
            CameraDollyKey current = evaluate(time);
            distance += length(subtract(current.pose.position, previous.pose.position));
            samples.push_back({time, distance});
            previous = current;
        }
    }
    if (!(distance > 1.0e-6F)) return evaluate(lerp(begin, end, normalizedProgress));
    const float wanted = normalizedProgress * distance;
    const auto upper = std::lower_bound(samples.begin(), samples.end(), wanted,
        [](const Sample& sample, float value) { return sample.distance < value; });
    if (upper == samples.begin()) return evaluate(upper->time);
    if (upper == samples.end()) return evaluate(end);
    const auto lower = upper - 1;
    const float span = std::max(1.0e-6F, upper->distance - lower->distance);
    return evaluate(lerp(lower->time, upper->time, (wanted - lower->distance) / span));
}

float CameraDollySpline::approximate_length() const noexcept {
    if (keys.size() < 2U) return 0.0F;
    float result = 0.0F;
    CameraDollyKey previous = evaluate(keys.front().timeSeconds);
    for (std::size_t segment = 0; segment + 1U < keys.size(); ++segment) {
        for (std::uint32_t sample = 1U; sample <= arcLengthSamplesPerSegment; ++sample) {
            const float local = static_cast<float>(sample) /
                                static_cast<float>(arcLengthSamplesPerSegment);
            const CameraDollyKey current = evaluate(lerp(keys[segment].timeSeconds,
                                                         keys[segment + 1U].timeSeconds, local));
            result += length(subtract(current.pose.position, previous.pose.position));
            previous = current;
        }
    }
    return result;
}

bool CameraShot::validate(std::string* error) const {
    if (id == 0U || name.empty() || !std::isfinite(startSeconds) || startSeconds < 0.0F ||
        !std::isfinite(durationSeconds) || durationSeconds <= 0.0F) {
        if (error) *error = "shot identity or timing is invalid";
        return false;
    }
    if (rigId.has_value() == dollyId.has_value()) {
        if (error) *error = "shot must reference exactly one rig or dolly";
        return false;
    }
    if (constantSpeedDolly && !dollyId) {
        if (error) *error = "constant-speed playback requires a dolly shot";
        return false;
    }
    return blendIn.validate(error);
}

bool CameraSequenceEvent::validate(std::string* error) const {
    if (id == 0U || name.empty() || !std::isfinite(timeSeconds) || timeSeconds < 0.0F) {
        if (error) *error = "camera sequence event is invalid";
        return false;
    }
    if (name.size() > 1024U || payload.size() > 65536U) {
        if (error) *error = "camera sequence event text is too large";
        return false;
    }
    return true;
}

bool CameraSequence::validate(std::string* error) const {
    auto fail = [&](std::string message) {
        if (error) *error = std::move(message);
        return false;
    };
    if (name.empty() || !std::isfinite(durationSeconds) || durationSeconds <= 0.0F)
        return fail("sequence name or duration is invalid");
    std::unordered_set<std::uint64_t> dollyIds;
    std::unordered_set<std::uint64_t> shotIds;
    std::unordered_set<std::uint64_t> eventIds;
    for (const auto& dolly : dollies) {
        if (!dolly.validate(error) || !dollyIds.insert(dolly.id).second)
            return fail("invalid or duplicate dolly");
    }
    float priorEnd = 0.0F;
    for (const auto& shot : shots) {
        if (!shot.validate(error) || !shotIds.insert(shot.id).second)
            return fail("invalid or duplicate shot");
        if (shot.startSeconds < priorEnd - 1.0e-5F) return fail("shots overlap");
        if (shot.startSeconds + shot.durationSeconds > durationSeconds + 1.0e-5F)
            return fail("shot exceeds sequence duration");
        if (shot.dollyId && !dollyIds.contains(*shot.dollyId))
            return fail("shot references missing dolly");
        priorEnd = shot.startSeconds + shot.durationSeconds;
    }
    float previousEvent = -1.0F;
    for (const auto& event : events) {
        if (!event.validate(error) || !eventIds.insert(event.id).second)
            return fail("invalid or duplicate camera event");
        if (event.timeSeconds > durationSeconds + 1.0e-5F)
            return fail("camera event exceeds sequence duration");
        if (event.timeSeconds < previousEvent) return fail("camera events must be time ordered");
        previousEvent = event.timeSeconds;
    }
    if (metadata.size() > 4096U) return fail("too much camera sequence metadata");
    for (const auto& [key, value] : metadata)
        if (key.empty() || key.size() > 1024U || value.size() > 65536U)
            return fail("camera sequence metadata is invalid");
    return true;
}

CameraSequenceSample CameraSequence::evaluate(float time) const noexcept {
    CameraSequenceSample result{};
    result.timeSeconds = std::clamp(time, 0.0F, std::max(0.0F, durationSeconds));
    const CameraShot* shot = nullptr;
    std::size_t shotIndex{};
    for (std::size_t index = 0; index < shots.size(); ++index) {
        if (result.timeSeconds >= shots[index].startSeconds &&
            result.timeSeconds < shots[index].startSeconds + shots[index].durationSeconds) {
            shot = &shots[index];
            shotIndex = index;
            break;
        }
    }
    if (!shot && !shots.empty() &&
        result.timeSeconds >= shots.back().startSeconds + shots.back().durationSeconds) {
        shot = &shots.back();
        shotIndex = shots.size() - 1U;
    }
    if (!shot) return result;
    result.shotId = shot->id;
    result.rigId = shot->rigId;
    result.marker = shot->marker;
    result.stateTrigger = shot->stateTrigger;
    result.cut = shot->blendIn.curve == CameraBlendCurve::Cut ||
                 shot->blendIn.durationSeconds <= 0.0F;
    if (shot->dollyId) {
        const auto it = std::find_if(dollies.begin(), dollies.end(), [&](const auto& dolly) {
            return dolly.id == *shot->dollyId;
        });
        if (it != dollies.end()) {
            const float local = std::clamp(result.timeSeconds - shot->startSeconds,
                                           0.0F, shot->durationSeconds);
            const float progress = shot->durationSeconds > 0.0F ? local / shot->durationSeconds : 0.0F;
            auto key = it->evaluate_progress(progress, shot->constantSpeedDolly);
            if (shotIndex > 0U && shot->blendIn.durationSeconds > 0.0F &&
                local < shot->blendIn.durationSeconds && shots[shotIndex - 1U].dollyId) {
                const auto previous = std::find_if(dollies.begin(), dollies.end(), [&](const auto& dolly) {
                    return dolly.id == *shots[shotIndex - 1U].dollyId;
                });
                if (previous != dollies.end()) {
                    const float weight = curve_weight(shot->blendIn.curve,
                                                       local / shot->blendIn.durationSeconds);
                    key = blend_keys(previous->keys.back(), key, weight);
                }
            }
            result.pose = key.pose;
            result.postProcess = key.postProcess;
        }
    }
    return result;
}

std::string CameraSequence::serialize() const {
    std::ostringstream out;
    out << "DVE_CAMERA_SEQUENCE 3\n" << std::quoted(name) << ' '
        << std::setprecision(9) << durationSeconds << '\n';
    out << "dollies " << dollies.size() << '\n';
    for (const auto& dolly : dollies) {
        out << "dolly " << dolly.id << ' ' << std::quoted(dolly.name) << ' '
            << (dolly.closed ? 1 : 0) << ' ' << dolly.arcLengthSamplesPerSegment << ' '
            << dolly.keys.size() << '\n';
        for (const auto& key : dolly.keys) {
            const auto& pose = key.pose;
            const auto& post = key.postProcess;
            out << "key " << key.timeSeconds << ' '
                << pose.position.x << ' ' << pose.position.y << ' ' << pose.position.z << ' '
                << pose.target.x << ' ' << pose.target.y << ' ' << pose.target.z << ' '
                << pose.worldUp.x << ' ' << pose.worldUp.y << ' ' << pose.worldUp.z << ' '
                << pose.lens.verticalFieldOfViewRadians << ' ' << pose.lens.nearPlaneMeters << ' '
                << pose.lens.farPlaneMeters << ' ' << (pose.lens.physical.enabled ? 1 : 0) << ' '
                << pose.lens.physical.focalLengthMillimeters << ' '
                << pose.lens.physical.focusDistanceMeters << ' '
                << pose.lens.physical.apertureFStop << ' ' << pose.postProcessWeight << ' ';
            write_post_process(out,post);
            out << '\n';
        }
    }
    out << "shots " << shots.size() << '\n';
    for (const auto& shot : shots) {
        out << "shot " << shot.id << ' ' << std::quoted(shot.name) << ' '
            << shot.startSeconds << ' ' << shot.durationSeconds << ' '
            << shot.rigId.value_or(0U) << ' ' << shot.dollyId.value_or(0U) << ' '
            << curve_name(shot.blendIn.curve) << ' ' << shot.blendIn.durationSeconds << ' '
            << (shot.constantSpeedDolly ? 1 : 0) << ' '
            << std::quoted(shot.marker) << ' ' << std::quoted(shot.stateTrigger) << '\n';
    }
    out << "events " << events.size() << '\n';
    for (const auto& event : events)
        out << "event " << event.id << ' ' << event.timeSeconds << ' '
            << std::quoted(event.name) << ' ' << std::quoted(event.payload) << '\n';
    out << "metadata " << metadata.size() << '\n';
    for (const auto& [key, value] : metadata)
        out << "meta " << std::quoted(key) << ' ' << std::quoted(value) << '\n';
    return out.str();
}

std::optional<CameraSequence> CameraSequence::parse(std::string_view text, std::string* error) {
    auto fail = [&](std::string message) -> std::optional<CameraSequence> {
        if (error) *error = std::move(message);
        return std::nullopt;
    };
    std::istringstream in{std::string(text)};
    std::string magic;
    std::string key;
    int version{};
    CameraSequence result;
    if (!(in >> magic >> version) || magic != "DVE_CAMERA_SEQUENCE" ||
        (version != 1 && version != 2 && version != 3)) return fail("unsupported camera sequence format");
    if (!(in >> std::quoted(result.name) >> result.durationSeconds))
        return fail("malformed sequence header");
    std::size_t dollyCount{};
    if (!(in >> key >> dollyCount) || key != "dollies" || dollyCount > 10000U)
        return fail("invalid dolly count");
    for (std::size_t di = 0; di < dollyCount; ++di) {
        CameraDollySpline dolly;
        int closed{};
        std::size_t keyCount{};
        if (version == 1) {
            if (!(in >> key >> dolly.id >> std::quoted(dolly.name) >> closed >> keyCount) ||
                key != "dolly") return fail("malformed dolly");
        } else {
            if (!(in >> key >> dolly.id >> std::quoted(dolly.name) >> closed >>
                  dolly.arcLengthSamplesPerSegment >> keyCount) || key != "dolly")
                return fail("malformed dolly");
        }
        if ((closed != 0 && closed != 1) || keyCount > 100000U)
            return fail("malformed dolly");
        dolly.closed = closed != 0;
        for (std::size_t ki = 0; ki < keyCount; ++ki) {
            CameraDollyKey dollyKey;
            int physical{};
            auto& pose = dollyKey.pose;
            auto& post = dollyKey.postProcess;
            if (!(in >> key >> dollyKey.timeSeconds >>
                  pose.position.x >> pose.position.y >> pose.position.z >>
                  pose.target.x >> pose.target.y >> pose.target.z >>
                  pose.worldUp.x >> pose.worldUp.y >> pose.worldUp.z >>
                  pose.lens.verticalFieldOfViewRadians >> pose.lens.nearPlaneMeters >>
                  pose.lens.farPlaneMeters >> physical >>
                  pose.lens.physical.focalLengthMillimeters >>
                  pose.lens.physical.focusDistanceMeters >>
                  pose.lens.physical.apertureFStop >> pose.postProcessWeight) || key != "key" ||
                (physical != 0 && physical != 1) || !read_post_process(in,post,version))
                return fail("malformed dolly key");
            pose.lens.physical.enabled = physical != 0;
            if(version<=2){
                post.focusDistanceMeters = pose.lens.physical.focusDistanceMeters;
                post.apertureFStop = pose.lens.physical.apertureFStop;
            }
            dolly.keys.push_back(dollyKey);
        }
        result.dollies.push_back(std::move(dolly));
    }
    std::size_t shotCount{};
    if (!(in >> key >> shotCount) || key != "shots" || shotCount > 100000U)
        return fail("invalid shot count");
    for (std::size_t si = 0; si < shotCount; ++si) {
        CameraShot shot;
        std::uint64_t rig{};
        std::uint64_t dolly{};
        std::string curve;
        int constantSpeed{};
        if (version == 1) {
            if (!(in >> key >> shot.id >> std::quoted(shot.name) >> shot.startSeconds >>
                  shot.durationSeconds >> rig >> dolly >> curve >> shot.blendIn.durationSeconds >>
                  std::quoted(shot.marker) >> std::quoted(shot.stateTrigger)) || key != "shot")
                return fail("malformed shot");
        } else {
            if (!(in >> key >> shot.id >> std::quoted(shot.name) >> shot.startSeconds >>
                  shot.durationSeconds >> rig >> dolly >> curve >> shot.blendIn.durationSeconds >>
                  constantSpeed >> std::quoted(shot.marker) >> std::quoted(shot.stateTrigger)) ||
                key != "shot" || (constantSpeed != 0 && constantSpeed != 1))
                return fail("malformed shot");
            shot.constantSpeedDolly = constantSpeed != 0;
        }
        if (rig) shot.rigId = rig;
        if (dolly) shot.dollyId = dolly;
        const auto parsedCurve = parse_curve(curve);
        if (!parsedCurve) return fail("unknown shot blend curve");
        shot.blendIn.curve = *parsedCurve;
        result.shots.push_back(std::move(shot));
    }
    if (version >= 2) {
        std::size_t eventCount{};
        if (!(in >> key >> eventCount) || key != "events" || eventCount > 100000U)
            return fail("invalid camera event count");
        for (std::size_t index = 0; index < eventCount; ++index) {
            CameraSequenceEvent event;
            if (!(in >> key >> event.id >> event.timeSeconds >> std::quoted(event.name) >>
                  std::quoted(event.payload)) || key != "event")
                return fail("malformed camera event");
            result.events.push_back(std::move(event));
        }
        std::size_t metadataCount{};
        if (!(in >> key >> metadataCount) || key != "metadata" || metadataCount > 4096U)
            return fail("invalid camera metadata count");
        for (std::size_t index = 0; index < metadataCount; ++index) {
            std::string name;
            std::string value;
            if (!(in >> key >> std::quoted(name) >> std::quoted(value)) || key != "meta")
                return fail("malformed camera metadata");
            if (!result.metadata.emplace(std::move(name), std::move(value)).second)
                return fail("duplicate camera metadata key");
        }
    }
    in >> std::ws;
    if (!in.eof()) return fail("trailing camera sequence data");
    if (!result.validate(error)) return std::nullopt;
    return result;
}

void CameraSequencePlayer::set_sequence(const CameraSequence* sequence) noexcept {
    sequence_ = sequence;
    timeSeconds_ = 0.0F;
    previousTimeSeconds_ = 0.0F;
    playing_ = false;
}

void CameraSequencePlayer::play(bool loop) noexcept {
    if (sequence_) {
        playing_ = true;
        loop_ = loop;
        previousTimeSeconds_ = timeSeconds_;
    }
}

void CameraSequencePlayer::stop() noexcept {
    playing_ = false;
    timeSeconds_ = 0.0F;
    previousTimeSeconds_ = 0.0F;
}

void CameraSequencePlayer::seek(float time) noexcept {
    timeSeconds_ = sequence_ ? std::clamp(time, 0.0F, sequence_->durationSeconds) : 0.0F;
    previousTimeSeconds_ = timeSeconds_;
}

CameraSequenceSample CameraSequencePlayer::update(float elapsed) noexcept {
    if (!sequence_) return {};
    const float oldTime = timeSeconds_;
    bool wrapped = false;
    if (playing_ && elapsed > 0.0F && std::isfinite(elapsed)) {
        timeSeconds_ += elapsed;
        if (timeSeconds_ >= sequence_->durationSeconds) {
            if (loop_ && sequence_->durationSeconds > 0.0F) {
                timeSeconds_ = std::fmod(timeSeconds_, sequence_->durationSeconds);
                wrapped = true;
            } else {
                timeSeconds_ = sequence_->durationSeconds;
                playing_ = false;
            }
        }
    }
    CameraSequenceSample sample = sequence_->evaluate(timeSeconds_);
    if (elapsed > 0.0F) {
        for (const auto& event : sequence_->events) {
            const bool crossed = wrapped
                ? (event.timeSeconds > oldTime || event.timeSeconds <= timeSeconds_)
                : (event.timeSeconds > oldTime && event.timeSeconds <= timeSeconds_);
            if (crossed) sample.triggeredEvents.push_back(event);
        }
    }
    previousTimeSeconds_ = timeSeconds_;
    return sample;
}

} // namespace dve::camera
