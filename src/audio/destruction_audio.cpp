#include "dve/audio/destruction_audio.hpp"

#include <algorithm>
#include <cmath>
#include <new>

namespace dve::audio {
namespace {

float distance_squared(AudioVec3 a, AudioVec3 b) noexcept {
    const float x = a.x - b.x; const float y = a.y - b.y; const float z = a.z - b.z;
    return x * x + y * y + z * z;
}

float clamp01(float value) noexcept { return std::clamp(value, 0.0F, 1.0F); }

} // namespace

struct DestructionAudioCompiler::Cluster {
    bool active{};
    double firstTime{};
    double lastTime{};
    AudioVec3 weightedPosition{};
    AudioVec3 weightedVelocity{};
    float weight{};
    float removedVolume{};
    float fractureArea{};
    float peakImpulse{};
    float totalImpulse{};
    float mass{};
    std::array<std::pair<std::uint16_t, std::uint16_t>, 4> materials{};
    std::uint16_t count{};
};

DestructionAudioCompiler::DestructionAudioCompiler(float clusterRadiusMeters,
                                                   float clusterWindowSeconds) noexcept
    : clusters_(new std::array<Cluster, kMaxDestructionClusters>{}),
      radius_(std::clamp(clusterRadiusMeters, 0.05F, 20.0F)),
      window_(std::clamp(clusterWindowSeconds, 0.01F, 2.0F)) {}

DestructionAudioCompiler::~DestructionAudioCompiler() { delete clusters_; }

void DestructionAudioCompiler::submit(const DestructionStimulus& stimulus) noexcept {
    Cluster* best = nullptr;
    float bestDistance = radius_ * radius_;
    for (auto& cluster : *clusters_) {
        if (!cluster.active || stimulus.timeSeconds - cluster.lastTime > static_cast<double>(window_)) continue;
        const AudioVec3 centroid = cluster.weight > 0.0F
            ? AudioVec3{cluster.weightedPosition.x / cluster.weight,
                        cluster.weightedPosition.y / cluster.weight,
                        cluster.weightedPosition.z / cluster.weight}
            : stimulus.position;
        const float d2 = distance_squared(centroid, stimulus.position);
        if (d2 <= bestDistance) { bestDistance = d2; best = &cluster; }
    }
    if (best == nullptr) {
        for (auto& cluster : *clusters_) if (!cluster.active) { best = &cluster; break; }
    }
    if (best == nullptr) {
        ++dropped_;
        return;
    }
    if (!best->active) {
        *best = {};
        best->active = true;
        best->firstTime = stimulus.timeSeconds;
    }
    best->lastTime = stimulus.timeSeconds;
    const float contribution = std::max(0.001F, stimulus.impulse + stimulus.removedVolume * 4.0F + stimulus.mass * 0.2F);
    best->weightedPosition.x += stimulus.position.x * contribution;
    best->weightedPosition.y += stimulus.position.y * contribution;
    best->weightedPosition.z += stimulus.position.z * contribution;
    best->weightedVelocity.x += stimulus.velocity.x * contribution;
    best->weightedVelocity.y += stimulus.velocity.y * contribution;
    best->weightedVelocity.z += stimulus.velocity.z * contribution;
    best->weight += contribution;
    best->removedVolume += std::max(0.0F, stimulus.removedVolume);
    best->fractureArea += std::max(0.0F, stimulus.fractureArea);
    best->peakImpulse = std::max(best->peakImpulse, std::max(0.0F, stimulus.impulse));
    best->totalImpulse += std::max(0.0F, stimulus.impulse);
    best->mass += std::max(0.0F, stimulus.mass);
    best->count = static_cast<std::uint16_t>(std::min<unsigned>(65535U, static_cast<unsigned>(best->count) + 1U));
    auto* materialSlot = &best->materials[0];
    for (auto& entry : best->materials) {
        if (entry.second == 0U || entry.first == stimulus.material) { materialSlot = &entry; break; }
        if (entry.second < materialSlot->second) materialSlot = &entry;
    }
    if (materialSlot->second == 0U || materialSlot->first == stimulus.material) {
        materialSlot->first = stimulus.material;
        materialSlot->second = static_cast<std::uint16_t>(std::min<unsigned>(65535U, static_cast<unsigned>(materialSlot->second) + 1U));
    }
}

DestructionCompileResult DestructionAudioCompiler::flush(double nowSeconds, bool flushAll) noexcept {
    DestructionCompileResult result;
    result.droppedStimuli = dropped_;
    dropped_ = 0U;
    for (auto& cluster : *clusters_) {
        if (!cluster.active) continue;
        if (!flushAll && nowSeconds - cluster.lastTime < static_cast<double>(window_)) continue;
        if (result.count >= result.events.size()) {
            result.droppedStimuli += cluster.count;
            cluster = {};
            continue;
        }
        DestructionAudioEvent event;
        const float inverseWeight = 1.0F / std::max(0.001F, cluster.weight);
        event.centroid = {cluster.weightedPosition.x * inverseWeight,
                          cluster.weightedPosition.y * inverseWeight,
                          cluster.weightedPosition.z * inverseWeight};
        event.averageVelocity = {cluster.weightedVelocity.x * inverseWeight,
                                 cluster.weightedVelocity.y * inverseWeight,
                                 cluster.weightedVelocity.z * inverseWeight};
        event.removedVolume = cluster.removedVolume;
        event.fractureArea = cluster.fractureArea;
        event.peakImpulse = cluster.peakImpulse;
        event.totalImpulse = cluster.totalImpulse;
        event.fragmentMass = cluster.mass;
        event.durationSeconds = static_cast<float>(std::max(0.0, cluster.lastTime - cluster.firstTime));
        event.stimulusCount = cluster.count;
        const auto dominant = std::max_element(cluster.materials.begin(), cluster.materials.end(),
                                               [](const auto& a, const auto& b) { return a.second < b.second; });
        event.dominantMaterial = dominant->first;
        const float scale = event.removedVolume * 2.0F + event.totalImpulse * 0.025F + event.fragmentMass * 0.02F;
        event.classification = scale > 12.0F ? DestructionClass::Collapse
                             : scale > 3.0F ? DestructionClass::Break
                             : scale > 0.5F ? DestructionClass::Crack : DestructionClass::Chip;
        result.mergedStimuli += cluster.count;
        result.events[result.count++] = event;
        cluster = {};
    }
    return result;
}

std::size_t DestructionAudioCompiler::active_clusters() const noexcept {
    return static_cast<std::size_t>(std::count_if(clusters_->begin(), clusters_->end(), [](const Cluster& c) { return c.active; }));
}

void DestructionAudioCompiler::reset() noexcept {
    for (auto& cluster : *clusters_) cluster = {};
    dropped_ = 0U;
}

DestructionLayerRecipe destruction_layer_recipe(const DestructionAudioEvent& event) noexcept {
    const float impulse = clamp01(event.peakImpulse / 80.0F);
    const float volume = clamp01(std::sqrt(std::max(0.0F, event.removedVolume)) / 4.0F);
    const float mass = clamp01(std::sqrt(std::max(0.0F, event.fragmentMass)) / 12.0F);
    const float density = clamp01(static_cast<float>(event.stimulusCount) / 64.0F);
    return {
        clamp01(0.35F + impulse * 0.65F),
        clamp01(volume * 0.75F + density * 0.25F),
        clamp01(mass * 0.85F + volume * 0.15F),
        clamp01(density * 0.8F + volume * 0.2F),
        clamp01(mass * volume),
        clamp01(0.12F + volume * 0.45F),
    };
}

} // namespace dve::audio
