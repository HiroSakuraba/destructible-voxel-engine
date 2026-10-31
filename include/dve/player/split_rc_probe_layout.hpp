#pragma once
// Deterministic CPU counterpart of split_rc_probe_assign_cs.hlsl.
// Removes probe-assignment GPU readbacks from the experimental Vulkan player.
// Geometry and material lighting are NOT cached: only stable world-space probe keys.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace dve::player {

struct SplitRcProbeCandidate {
    std::array<float,4> positionSpacing{};
    std::array<std::int32_t,4> levelSheet{};
    bool operator==(const SplitRcProbeCandidate&) const = default;
};

inline std::uint32_t split_rc_hash(std::uint32_t k) noexcept {
    k ^= k >> 16; k *= 0x7feb352dU; k ^= k >> 15;
    k *= 0x846ca68bU; return k ^ (k >> 16);
}

// Identical input keys reuse the same occupied table and slot assignments.
// The cache does not persist radiance; scene edits still require fresh tracing.
class SplitRcProbeLayoutCache {
public:
    bool assign(std::span<const SplitRcProbeCandidate> samples,
                std::vector<std::uint32_t>& keys,
                std::vector<std::uint32_t>& assignments,
                std::string* error = nullptr) {
        reused_ = false;
        if(samples.size() > 65536U) {
            if(error)*error = "split RC exceeds 65536 probe candidates";
            invalidate(); return false;
        }
        // Do not store or reuse malformed samples, even if their bit patterns compare equal.
        for(const auto& s : samples) {
            const float base = s.positionSpacing[3];
            const int level = s.levelSheet[0], sheet = s.levelSheet[1];
            if(!(std::isfinite(base) && base > 0.0F) || level < 0 || level > 3 ||
               sheet < 0 || sheet > 5 ||
               !std::all_of(s.positionSpacing.begin(),s.positionSpacing.end(),
                            [](float v){return std::isfinite(v);})) {
                if(error)*error = "split RC invalid probe candidate";
                invalidate(); return false;
            }
            const float spacing = base * static_cast<float>(1U << level);
            if(!(std::isfinite(spacing) && spacing > 0.0F)) {
                if(error)*error = "split RC invalid probe spacing";
                invalidate(); return false;
            }
            for(int axis=0;axis<3;++axis) {
                const float cell = std::floor(s.positionSpacing[axis] / spacing);
                if(!std::isfinite(cell) || cell < -128.0F || cell >= 128.0F) {
                    if(error)*error = "split RC probe outside keyed region";
                    invalidate(); return false;
                }
            }
        }
        if(valid_ && samples.size() == samples_.size() &&
           std::equal(samples.begin(),samples.end(),samples_.begin())) {
            keys = keys_; assignments = assignments_; reused_ = true;
            return true;
        }
        std::size_t capacity = 1024U;
        while(capacity < samples.size()*2U) capacity *= 2U;
        std::vector<std::uint32_t> newKeys(capacity,0U);
        std::vector<std::uint32_t> newAssignments(samples.size(),0xffffffffU);
        for(std::size_t i=0;i<samples.size();++i) {
            const auto& s=samples[i];
            const auto level=static_cast<std::uint32_t>(s.levelSheet[0]);
            const auto sheet=static_cast<std::uint32_t>(s.levelSheet[1]);
            const float spacing=s.positionSpacing[3]*static_cast<float>(1U<<level);
            std::uint32_t packed=level<<24U | sheet<<26U;
            for(int axis=0;axis<3;++axis) {
                const auto cell=static_cast<std::uint32_t>(
                    static_cast<std::int32_t>(std::floor(s.positionSpacing[axis]/spacing))+128);
                packed |= cell << (8U*axis);
            }
            const std::uint32_t key=packed+1U;
            const std::size_t start=split_rc_hash(key)%capacity;
            bool inserted=false;
            for(std::size_t offset=0;offset<capacity;++offset) {
                const std::size_t slot=(start+offset)%capacity;
                if(newKeys[slot]==0U || newKeys[slot]==key) {
                    newKeys[slot]=key;
                    newAssignments[i]=static_cast<std::uint32_t>(slot);
                    inserted=true;break;
                }
            }
            if(!inserted) {
                if(error)*error="split RC probe hash table overflow";
                invalidate();return false;
            }
        }
        samples_.assign(samples.begin(),samples.end());
        keys_=std::move(newKeys);assignments_=std::move(newAssignments);
        valid_=true;keys=keys_;assignments=assignments_;
        return true;
    }
    [[nodiscard]] bool reused_last_call() const noexcept {return reused_;}
    void invalidate() noexcept {
        valid_=false;reused_=false;
        samples_.clear();keys_.clear();assignments_.clear();
    }
private:
    bool valid_{}, reused_{};
    std::vector<SplitRcProbeCandidate> samples_;
    std::vector<std::uint32_t> keys_,assignments_;
};
} // namespace dve::player
