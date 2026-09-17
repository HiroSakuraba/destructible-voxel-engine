#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

#include "dve/audio/destruction_audio.hpp"
#include "dve/audio/event_graph.hpp"

namespace dve::audio {

// Engine-neutral ingestion records. Jolt and the voxel runtime translate their native events
// into these compact records on the game/physics thread; no middleware type crosses this API.
struct VoxelAudioEdit {
    AudioVec3 position{};
    AudioVec3 velocity{};
    std::uint16_t material{};
    float removedVolume{};
    float fractureArea{};
};
struct PhysicsContactAudio {
    AudioVec3 position{};
    AudioVec3 relativeVelocity{};
    std::uint16_t materialA{};
    std::uint16_t materialB{};
    float normalImpulse{};
    float effectiveMass{};
    bool persistent{};
};
struct FragmentSplitAudio {
    AudioVec3 position{};
    AudioVec3 velocity{};
    std::uint16_t material{};
    float separatedMass{};
    float fractureArea{};
};
struct StructuralStrainAudio {
    AudioVec3 position{};
    AudioVec3 velocity{};
    std::uint16_t material{};
    float strainEnergy{};
    float affectedMass{};
};

class AudioEventLibrary {
public:
    bool register_event(std::string name, CompiledAudioEvent event, std::string* error = nullptr);
    bool register_asset(const AudioEventAsset& asset, std::string* error = nullptr);
    [[nodiscard]] bool contains(std::string_view name) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return events_.size(); }
    [[nodiscard]] const CompiledAudioEvent* find(std::string_view name) const noexcept;

    AudioEventExecution execute(std::string_view name, const AudioEventParameters& parameters,
                                std::uint32_t sampleRate, std::uint64_t absoluteFrame,
                                bool* found = nullptr) noexcept;
private:
    struct Entry {
        CompiledAudioEvent event;
        AudioEventInstanceState state;
    };
    std::unordered_map<std::string, Entry> events_;
};

struct MaterialDestructionAudioBinding {
    std::uint16_t material{};
    std::string chipEvent;
    std::string crackEvent;
    std::string breakEvent;
    std::string collapseEvent;
};

struct DestructionAudioRuntimeMetrics {
    std::uint64_t submittedStimuli{};
    std::uint64_t compiledEvents{};
    std::uint64_t dispatchedActions{};
    std::uint64_t missingMappings{};
    std::uint64_t missingEvents{};
    std::uint64_t truncatedExecutions{};
    std::size_t activeClusters{};
};

// End-to-end control-thread bridge: voxel/Jolt records -> bounded clusters -> material event ->
// sample/synth actions -> mixer. It never runs in the audio callback.
class DestructionAudioRuntime {
public:
    DestructionAudioRuntime(AudioMixer& mixer, AudioEventLibrary& events,
                            float clusterRadiusMeters = 2.0F,
                            float clusterWindowSeconds = 0.18F) noexcept;

    bool bind_material(MaterialDestructionAudioBinding binding, std::string* error = nullptr);
    void set_default_binding(MaterialDestructionAudioBinding binding);

    void submit_voxel_edits(std::span<const VoxelAudioEdit> edits, double timeSeconds) noexcept;
    void submit_contacts(std::span<const PhysicsContactAudio> contacts, double timeSeconds) noexcept;
    void submit_fragment_splits(std::span<const FragmentSplitAudio> splits, double timeSeconds) noexcept;
    void submit_structural_strain(std::span<const StructuralStrainAudio> strain, double timeSeconds) noexcept;

    std::size_t update(double nowSeconds, bool flushAll = false) noexcept;
    [[nodiscard]] DestructionAudioRuntimeMetrics metrics() const noexcept;
    void reset() noexcept;
private:
    [[nodiscard]] const MaterialDestructionAudioBinding* binding(std::uint16_t material) const noexcept;
    [[nodiscard]] static std::string_view event_for(const MaterialDestructionAudioBinding& binding,
                                                    DestructionClass classification) noexcept;

    AudioMixer* mixer_{};
    AudioEventLibrary* events_{};
    DestructionAudioCompiler compiler_;
    std::unordered_map<std::uint16_t, MaterialDestructionAudioBinding> bindings_;
    MaterialDestructionAudioBinding defaultBinding_{};
    DestructionAudioRuntimeMetrics metrics_{};
};

} // namespace dve::audio
