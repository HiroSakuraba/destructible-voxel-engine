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

struct CameraDollySpline {
    CameraDollyId id{};
    std::string name;
    bool closed{};
    std::uint32_t arcLengthSamplesPerSegment{24U};
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
