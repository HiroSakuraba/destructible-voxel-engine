#include "dve/audio/steam_audio.hpp"

#include <algorithm>
#include <cmath>

namespace dve::audio {
namespace {

bool finite_direct(const SteamAudioDirectPath& value) noexcept {
    return std::isfinite(value.distanceAttenuation) && std::isfinite(value.airAbsorptionLow) &&
           std::isfinite(value.airAbsorptionMid) && std::isfinite(value.airAbsorptionHigh) &&
           std::isfinite(value.directivity) && std::isfinite(value.occlusion) &&
           std::isfinite(value.transmissionLow) && std::isfinite(value.transmissionMid) &&
           std::isfinite(value.transmissionHigh) && std::isfinite(value.reverbSend) &&
           std::isfinite(value.propagationDelaySeconds);
}

} // namespace

SteamAudioSpatializer::SteamAudioSpatializer(
    std::shared_ptr<const ISteamAudioSimulationBackend> backend,
    SteamAudioQuality quality) noexcept
    : backend_(std::move(backend)), quality_(quality) {}
void SteamAudioSpatializer::set_quality(SteamAudioQuality quality) noexcept {
    quality_.store(quality, std::memory_order_release);
}
SteamAudioQuality SteamAudioSpatializer::quality() const noexcept {
    return quality_.load(std::memory_order_acquire);
}

SpatializationResult SteamAudioSpatializer::spatialize(const AudioListenerState& listener,
                                                        const AudioEmitterState& emitter) const noexcept {
    SpatializationResult result = fallback_.spatialize(listener, emitter);
    if (!backend_ || !backend_->valid()) {
        fallbackQueries_.fetch_add(1U, std::memory_order_relaxed);
        return result;
    }
    const auto direct = backend_->query_direct(listener, emitter, quality());
    if (!finite_direct(direct)) {
        invalidResults_.fetch_add(1U, std::memory_order_relaxed);
        fallbackQueries_.fetch_add(1U, std::memory_order_relaxed);
        return result;
    }
    backendQueries_.fetch_add(1U, std::memory_order_relaxed);
    const float low = std::clamp(direct.airAbsorptionLow * direct.transmissionLow, 0.0F, 1.0F);
    const float mid = std::clamp(direct.airAbsorptionMid * direct.transmissionMid, 0.0F, 1.0F);
    const float high = std::clamp(direct.airAbsorptionHigh * direct.transmissionHigh, 0.0F, 1.0F);
    const float spectralGain = 0.20F * low + 0.45F * mid + 0.35F * high;
    const float occlusion = std::clamp(direct.occlusion, 0.0F, 1.0F);
    result.distanceGain *= std::clamp(direct.distanceAttenuation, 0.0F, 4.0F) *
                           std::clamp(direct.directivity, 0.0F, 2.0F) * spectralGain;
    result.lowPassHertz = std::clamp(400.0F + 19600.0F * high * (1.0F - 0.75F * occlusion),
                                     100.0F, 20000.0F);
    result.reverbSend = std::clamp(std::max(result.reverbSend, direct.reverbSend), 0.0F, 1.0F);
    result.propagationDelaySeconds = std::max(0.0F, direct.propagationDelaySeconds);
    return result;
}

SteamAudioSpatializerMetrics SteamAudioSpatializer::metrics() const noexcept {
    return {backendQueries_.load(std::memory_order_relaxed),
            fallbackQueries_.load(std::memory_order_relaxed),
            invalidResults_.load(std::memory_order_relaxed)};
}

} // namespace dve::audio
