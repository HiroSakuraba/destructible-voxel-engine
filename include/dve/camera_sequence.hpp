#pragma once

#include <cstdint>
#include <optional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "dve/camera_runtime.hpp"

namespace dve::camera {

using CameraDollyId = std::uint64_t;
using CameraShotId = std::uint64_t;

struct CameraDollyKey {
    float timeSeconds{};
    CameraPose pose{};
    CameraPostProcessProfile postProcess{};
    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

// How a dolly passes through its keys' positions and targets.
//   CatmullRom: each segment shaped by its neighbours, in a per-segment parameter. Curvature
//               jumps at every key, and so does speed when keys are unevenly spaced in time;
//               on a fast move both read as a small jolt.
//   Smooth:     natural cubic spline in time (C2): velocity and acceleration are continuous
//               through every key, so the path's curvature is continuous too. Each key
//               influences the whole path a little (moving one key nudges its neighbours'
//               segments), and the path starts and ends with zero acceleration.
enum class CameraDollyInterpolation : std::uint8_t { CatmullRom, Smooth };

struct CameraDollySpline {
    CameraDollyId id{};
    std::string name;
    bool closed{};
    std::uint32_t arcLengthSamplesPerSegment{24U};
    CameraDollyInterpolation interpolation{CameraDollyInterpolation::CatmullRom};
    std::vector<CameraDollyKey> keys;
    [[nodiscard]] bool validate(std::string* error = nullptr) const;
    [[nodiscard]] CameraDollyKey evaluate(float timeSeconds) const noexcept;
    [[nodiscard]] CameraDollyKey evaluate_progress(float normalizedProgress,
                                                    bool constantSpeed) const noexcept;
    [[nodiscard]] float approximate_length() const noexcept;
};

struct CameraShot {
    CameraShotId id{};
    std::string name;
    float startSeconds{};
    float durationSeconds{1.0F};
    std::optional<CameraRigId> rigId;
    std::optional<CameraDollyId> dollyId;
    CameraBlend blendIn{};
    bool constantSpeedDolly{};
    std::string marker;
    std::string stateTrigger;
    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};


struct CameraSequenceEvent {
    std::uint64_t id{};
    float timeSeconds{};
    std::string name;
    std::string payload;
    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct CameraSequenceSample {
    float timeSeconds{};
    std::optional<CameraShotId> shotId;
    std::optional<CameraRigId> rigId;
    std::optional<CameraPose> pose;
    CameraPostProcessProfile postProcess{};
    std::string marker;
    std::string stateTrigger;
    std::vector<CameraSequenceEvent> triggeredEvents;
    bool cut{};
};

struct CameraSequence {
    std::string name;
    float durationSeconds{};
    std::vector<CameraDollySpline> dollies;
    std::vector<CameraShot> shots;
    std::vector<CameraSequenceEvent> events;
    std::map<std::string, std::string, std::less<>> metadata;

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
    [[nodiscard]] CameraSequenceSample evaluate(float timeSeconds) const noexcept;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static std::optional<CameraSequence> parse(std::string_view text,
                                                              std::string* error = nullptr);
};

class CameraSequencePlayer {
public:
    [[nodiscard]] std::uint64_t state_hash() const noexcept;
    void set_sequence(const CameraSequence* sequence) noexcept;
    void play(bool loop = false) noexcept;
    void pause() noexcept { playing_ = false; }
    void stop() noexcept;
    void seek(float timeSeconds) noexcept;
    [[nodiscard]] CameraSequenceSample update(float elapsedSeconds) noexcept;
    [[nodiscard]] bool playing() const noexcept { return playing_; }
    [[nodiscard]] float time_seconds() const noexcept { return timeSeconds_; }
private:
    const CameraSequence* sequence_{};
    float timeSeconds_{};
    bool playing_{};
    bool loop_{};
    float previousTimeSeconds_{};
};

} // namespace dve::camera
