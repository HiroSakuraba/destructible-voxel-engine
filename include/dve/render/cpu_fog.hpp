#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <span>
#include <string>
#include <vector>
#include "dve/gabor_volume.hpp"
#include "dve/render/cpu_fog_cache.hpp"
#include "dve/render/polygon_renderer.hpp"

namespace dve::render {

// Borrowed immutable assets; positions and dimensions use the target's world units.
// Density and integration lengths are always per metre/metres.
struct CpuFogVolume {
    const GaborVolumeAsset* asset{};
    RigidTransform transform{};
    Float3 scale{1,1,1};
    bool visible{true};
    float sampleStepMeters{}; // optional feature-size hint; apply_cpu_fog derives it when zero
};

struct CpuFogSample {
    Float3 radiance{};
    float transmittance{1.0F};
};

struct CpuFogHistory {
    std::uint64_t key{};
    std::vector<CpuFogSample> samples;
    std::vector<float> depth;
    void clear(){key=0;samples.clear();depth.clear();}
};

// Optional quantized transmittance cache. Explicitly approximate, bounded, and
// reset with the full lighting/media key before each frame's workers start.
class CpuFogLightingCache {
public:
    void reset(std::uint64_t key){std::lock_guard lock(mutex_);if(key_!=key){key_=key;values_.clear();}}
    bool find(std::uint64_t cell,float& value){std::lock_guard lock(mutex_);const auto it=values_.find(cell);if(it==values_.end())return false;value=it->second;return true;}
    void store(std::uint64_t cell,float value){std::lock_guard lock(mutex_);if(values_.size()<65536)values_[cell]=value;}
private:
    std::mutex mutex_;
    std::uint64_t key_{};
    std::unordered_map<std::uint64_t,float> values_;
};

struct CpuFogLight {
    Float3 position{}; // world units
    Float3 color{1,1,1};
    float intensity{1};
    float rangeMeters{10};
};

struct CpuFogSettings {
    // Step target plus mandatory support-boundary samples. At most
    // maximumSteps + 129 primary segments for the <=32 volume API.
    std::uint32_t maximumSteps{96};
    float stepMeters{0.1F};
    float minimumTransmittance{0.005F};
    // Distance fog keeps its existing surface-only policy. Local volumes also
    // appear against the sky, clipped to the camera far plane.
    bool enabled{true};
    std::uint32_t resolutionDivisor{1}; // 1, 2 or 4; edge pixels retrace at full resolution
    float depthRejectionMeters{0.05F};
    bool jitter{};
    bool adaptiveSampling{true};
    std::uint32_t frameIndex{};
    float historyWeight{}; // 0..0.95, stationary-camera accumulation only
    std::uint64_t sceneRevision{}; // caller changes this when shadow casters/lights move
    bool scattering{};
    bool volumeShadows{};
    std::uint32_t shadowSteps{8};
    float shadowDistanceMeters{32};
    std::span<const CpuFogLight> lights{}; // at most 32
    // Optional scene transmittance [0,1]; thread-safe when threadCount > 1.
    // The caller owns geometry semantics (opaque or transmissive shadows).
    std::function<float(Float3,Float3,float)> sceneVisibility{};
    std::uint32_t threadCount{1}; // 0 = automatic, capped at 16
    float lightCacheCellMeters{}; // 0 disables spatial approximation
    CpuFogLightingCache* lightingCache{};
    CpuFogDensityCache* densityCache{}; // prepared by apply_cpu_fog before workers
    CpuFogHistory* history{}; // one viewport, one owner; never shared across renders
};

struct CpuFogStats {
    std::uint64_t rays{}, steps{}, densityEvaluations{}, volumeTests{};
    std::uint64_t terminatedRays{}, budgetLimitedRays{};
    std::uint64_t lightingCacheHits{};
    std::uint64_t shadowRays{}, shadowDensityEvaluations{};
    std::uint64_t edgeRetraces{}, historyAccepted{}, historyRejected{};
    std::uint64_t primitiveEvaluations{}, densityNodes{};
    double milliseconds{};
};

[[nodiscard]] CpuFogSettings cpu_fog_quality_settings(GaborVolumeQuality quality);

// Integrates all overlapping media together, independent of volume submission order.
// Uses midpoint segments with exact exponential attenuation for each segment.
[[nodiscard]] CpuFogSample trace_cpu_fog(
    Float3 origin, Float3 unitDirection, float nearDistance, float farDistance,
    float metersPerUnit, std::span<const CpuFogVolume> volumes,
    const RenderEnvironment& environment, const CpuFogSettings& settings,
    CpuFogStats* stats = nullptr, bool includeDistanceFog = true);

// Operates on linear HDR before tonemapping, never changes depth or picking IDs.
// Scales every light AOV and records integrated radiance in the fog AOV.
[[nodiscard]] bool apply_cpu_fog(
    PolygonRenderTarget& target, const PolygonCamera& camera,
    const RenderEnvironment& environment, float metersPerUnit,
    std::span<const CpuFogVolume> volumes, const CpuFogSettings& settings = {},
    CpuFogStats* stats = nullptr, std::string* error = nullptr);

} // namespace dve::render
