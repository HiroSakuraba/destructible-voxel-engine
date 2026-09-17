#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "dve/audio/spatializer.hpp"

namespace dve::audio {

inline constexpr std::size_t kMaxDestructionClusters = 64;
inline constexpr std::size_t kMaxPublishedDestructionEvents = 32;

enum class DestructionStimulusType : std::uint8_t { VoxelRemoval, ContactImpulse, FragmentSeparation, StructuralStrain };
enum class DestructionClass : std::uint8_t { Chip, Crack, Break, Collapse };

struct DestructionStimulus {
    DestructionStimulusType type{DestructionStimulusType::VoxelRemoval};
    double timeSeconds{};
    AudioVec3 position{};
    AudioVec3 velocity{};
    std::uint16_t material{};
    float removedVolume{};
    float fractureArea{};
    float impulse{};
    float mass{};
};

struct DestructionAudioEvent {
    AudioVec3 centroid{};
    AudioVec3 averageVelocity{};
    float removedVolume{};
    float fractureArea{};
    float peakImpulse{};
    float totalImpulse{};
    float fragmentMass{};
    float durationSeconds{};
    std::uint16_t dominantMaterial{};
    std::uint16_t stimulusCount{};
    DestructionClass classification{DestructionClass::Chip};
};

struct DestructionLayerRecipe {
    float onset{};
    float fracture{};
    float resonance{};
    float debris{};
    float structuralMovement{};
    float environmentalTail{};
};

struct DestructionCompileResult {
    std::array<DestructionAudioEvent, kMaxPublishedDestructionEvents> events{};
    std::size_t count{};
    std::size_t mergedStimuli{};
    std::size_t droppedStimuli{};
};

class DestructionAudioCompiler {
public:
    explicit DestructionAudioCompiler(float clusterRadiusMeters = 2.0F,
                                      float clusterWindowSeconds = 0.18F) noexcept;
    ~DestructionAudioCompiler();
    DestructionAudioCompiler(const DestructionAudioCompiler&) = delete;
    DestructionAudioCompiler& operator=(const DestructionAudioCompiler&) = delete;
    void submit(const DestructionStimulus& stimulus) noexcept;
    DestructionCompileResult flush(double nowSeconds, bool flushAll = false) noexcept;
    [[nodiscard]] std::size_t active_clusters() const noexcept;
    void reset() noexcept;

private:
    struct Cluster;
    std::array<Cluster, kMaxDestructionClusters>* clusters_{};
    float radius_{};
    float window_{};
    std::size_t dropped_{};
};

[[nodiscard]] DestructionLayerRecipe destruction_layer_recipe(const DestructionAudioEvent& event) noexcept;

} // namespace dve::audio
