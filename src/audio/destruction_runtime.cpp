#include "dve/audio/destruction_runtime.hpp"

#include <algorithm>
#include <cmath>

namespace dve::audio {

bool AudioEventLibrary::register_event(std::string name, CompiledAudioEvent event, std::string* error) {
    if (name.empty()) { if (error) *error = "audio event name is empty"; return false; }
    if (event.nodes.empty() || event.root >= event.nodes.size()) {
        if (error) *error = "compiled audio event is invalid";
        return false;
    }
    event.name = name;
    events_.insert_or_assign(std::move(name), Entry{std::move(event), {}});
    return true;
}

bool AudioEventLibrary::register_asset(const AudioEventAsset& asset, std::string* error) {
    auto event = compile_audio_event(asset, error);
    return event && register_event(asset.graph.name, std::move(*event), error);
}

bool AudioEventLibrary::contains(std::string_view name) const noexcept { return find(name) != nullptr; }
const CompiledAudioEvent* AudioEventLibrary::find(std::string_view name) const noexcept {
    const auto found = events_.find(std::string(name));
    return found == events_.end() ? nullptr : &found->second.event;
}

AudioEventExecution AudioEventLibrary::execute(std::string_view name,
                                                const AudioEventParameters& parameters,
                                                std::uint32_t sampleRate,
                                                std::uint64_t absoluteFrame,
                                                bool* found) noexcept {
    const auto item = events_.find(std::string(name));
    if (item == events_.end()) {
        if (found) *found = false;
        return {};
    }
    if (found) *found = true;
    return execute_audio_event(item->second.event, parameters, item->second.state,
                               sampleRate, absoluteFrame);
}

DestructionAudioRuntime::DestructionAudioRuntime(AudioMixer& mixer, AudioEventLibrary& events,
                                                 float radius, float window) noexcept
    : mixer_(&mixer), events_(&events), compiler_(radius, window) {}

bool DestructionAudioRuntime::bind_material(MaterialDestructionAudioBinding binding, std::string* error) {
    if (binding.chipEvent.empty() && binding.crackEvent.empty() &&
        binding.breakEvent.empty() && binding.collapseEvent.empty()) {
        if (error) *error = "material audio binding contains no events";
        return false;
    }
    bindings_.insert_or_assign(binding.material, std::move(binding));
    return true;
}
void DestructionAudioRuntime::set_default_binding(MaterialDestructionAudioBinding binding) {
    defaultBinding_ = std::move(binding);
}

void DestructionAudioRuntime::submit_voxel_edits(std::span<const VoxelAudioEdit> edits,
                                                  double timeSeconds) noexcept {
    for (const auto& edit : edits) {
        compiler_.submit({DestructionStimulusType::VoxelRemoval, timeSeconds, edit.position,
                          edit.velocity, edit.material, std::max(0.0F, edit.removedVolume),
                          std::max(0.0F, edit.fractureArea), 0.0F, 0.0F});
        ++metrics_.submittedStimuli;
    }
}
void DestructionAudioRuntime::submit_contacts(std::span<const PhysicsContactAudio> contacts,
                                               double timeSeconds) noexcept {
    for (const auto& contact : contacts) {
        // Persistent contacts are intentionally weighted down: scrape/roll state machines can
        // submit sustained energy without creating a full impact at every physics step.
        const float impulse = std::max(0.0F, contact.normalImpulse) * (contact.persistent ? 0.20F : 1.0F);
        const std::uint16_t material = contact.materialA != 0U ? contact.materialA : contact.materialB;
        compiler_.submit({DestructionStimulusType::ContactImpulse, timeSeconds, contact.position,
                          contact.relativeVelocity, material, 0.0F, 0.0F, impulse,
                          std::max(0.0F, contact.effectiveMass)});
        ++metrics_.submittedStimuli;
    }
}
void DestructionAudioRuntime::submit_fragment_splits(std::span<const FragmentSplitAudio> splits,
                                                      double timeSeconds) noexcept {
    for (const auto& split : splits) {
        compiler_.submit({DestructionStimulusType::FragmentSeparation, timeSeconds, split.position,
                          split.velocity, split.material, 0.0F, std::max(0.0F, split.fractureArea),
                          0.0F, std::max(0.0F, split.separatedMass)});
        ++metrics_.submittedStimuli;
    }
}
void DestructionAudioRuntime::submit_structural_strain(std::span<const StructuralStrainAudio> values,
                                                        double timeSeconds) noexcept {
    for (const auto& strain : values) {
        compiler_.submit({DestructionStimulusType::StructuralStrain, timeSeconds, strain.position,
                          strain.velocity, strain.material, 0.0F, 0.0F,
                          std::sqrt(std::max(0.0F, strain.strainEnergy)),
                          std::max(0.0F, strain.affectedMass)});
        ++metrics_.submittedStimuli;
    }
}

const MaterialDestructionAudioBinding* DestructionAudioRuntime::binding(std::uint16_t material) const noexcept {
    const auto found = bindings_.find(material);
    if (found != bindings_.end()) return &found->second;
    if (!defaultBinding_.chipEvent.empty() || !defaultBinding_.crackEvent.empty() ||
        !defaultBinding_.breakEvent.empty() || !defaultBinding_.collapseEvent.empty()) return &defaultBinding_;
    return nullptr;
}

std::string_view DestructionAudioRuntime::event_for(const MaterialDestructionAudioBinding& item,
                                                     DestructionClass classification) noexcept {
    switch (classification) {
        case DestructionClass::Chip: return item.chipEvent;
        case DestructionClass::Crack: return item.crackEvent.empty() ? item.chipEvent : item.crackEvent;
        case DestructionClass::Break: return item.breakEvent.empty() ? item.crackEvent : item.breakEvent;
        case DestructionClass::Collapse: return item.collapseEvent.empty() ? item.breakEvent : item.collapseEvent;
    }
    return {};
}

std::size_t DestructionAudioRuntime::update(double nowSeconds, bool flushAll) noexcept {
    const auto result = compiler_.flush(nowSeconds, flushAll);
    metrics_.compiledEvents += result.count;
    metrics_.activeClusters = compiler_.active_clusters();
    std::size_t dispatched{};
    for (std::size_t i = 0; i < result.count; ++i) {
        const auto& event = result.events[i];
        const auto* material = binding(event.dominantMaterial);
        if (!material) { ++metrics_.missingMappings; continue; }
        const std::string_view name = event_for(*material, event.classification);
        if (name.empty()) { ++metrics_.missingMappings; continue; }
        AudioEventParameters parameters;
        parameters.values = {
            {"removed_volume", event.removedVolume},
            {"fracture_area", event.fractureArea},
            {"peak_impulse", event.peakImpulse},
            {"total_impulse", event.totalImpulse},
            {"fragment_mass", event.fragmentMass},
            {"duration", event.durationSeconds},
            {"stimulus_count", static_cast<float>(event.stimulusCount)},
        };
        bool found{};
        auto execution = events_->execute(name, parameters, mixer_->sample_rate(), mixer_->current_frame(), &found);
        if (!found) { ++metrics_.missingEvents; continue; }
        if (execution.truncated) ++metrics_.truncatedExecutions;
        AudioEmitterState emitter;
        emitter.position = event.centroid;
        emitter.velocity = event.averageVelocity;
        emitter.radiusMeters = std::clamp(std::cbrt(std::max(0.0F, event.removedVolume)), 0.05F, 8.0F);
        emitter.reverbSend = std::clamp(destruction_layer_recipe(event).environmentalTail, 0.0F, 1.0F);
        dispatch_audio_event(execution, *mixer_, emitter);
        dispatched += execution.count;
    }
    metrics_.dispatchedActions += dispatched;
    return dispatched;
}

DestructionAudioRuntimeMetrics DestructionAudioRuntime::metrics() const noexcept {
    auto result = metrics_;
    result.activeClusters = compiler_.active_clusters();
    return result;
}
void DestructionAudioRuntime::reset() noexcept {
    compiler_.reset();
    metrics_ = {};
}

} // namespace dve::audio
