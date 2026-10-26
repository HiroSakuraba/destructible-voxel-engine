#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#include "dve/gabor_volume.hpp"
#include "dve/asset_cooker.hpp"

namespace dve::render {
struct CpuFogStats;
// Owned, bounded immutable density acceleration. prepare() is called before
// workers start; query() is read-only. One cache belongs to one viewport.
class CpuFogDensityCache {
public:
    explicit CpuFogDensityCache(std::size_t maximumPrimitives=262144);
    ~CpuFogDensityCache();
    CpuFogDensityCache(const CpuFogDensityCache&)=delete;
    CpuFogDensityCache& operator=(const CpuFogDensityCache&)=delete;
    void clear();
    bool prepare(const GaborVolumeAsset& asset);
    [[nodiscard]] float density(const GaborVolumeAsset& asset, Float3 localPosition, CpuFogStats* stats=nullptr) const;
    // Conservative occupied ray intervals from BVH leaves. Empty-space rejection
    // never skips a primitive's support; overflow uses the original whole range.
    [[nodiscard]] std::vector<Float2> occupied(const GaborVolumeAsset& asset,Float3 localOrigin,
        Float3 localDirection,float lo,float hi) const;
    [[nodiscard]] std::size_t resident_primitives() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace dve::render
