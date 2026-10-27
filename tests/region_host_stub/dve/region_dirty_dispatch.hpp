#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>
namespace dve {
struct BrickKey { int x{},y{},z{}; auto operator<=>(const BrickKey&) const = default; };
struct Bitset512 { static Bitset512 full(){return {};} };
struct AppliedBrickEdit { BrickKey key{}; Bitset512 changedMask{}; std::uint32_t generation{}; };
enum class RegionDirtyConsumer:std::uint8_t {Gpu,Navigation,Collision,Persistence};
using RegionDirtyChannels=std::uint8_t;
constexpr RegionDirtyChannels region_dirty_channel(RegionDirtyConsumer c) {
    return static_cast<RegionDirtyChannels>(1U << static_cast<unsigned>(c));
}
struct RegionKey { std::uint32_t worldId{}; std::uint8_t lod{}; int x{},y{},z{}; };
enum class PressureLevel {Normal};
struct WorkBudget {PressureLevel pressure{PressureLevel::Normal};std::uint32_t patchBrickLimit{192};};
struct RegionDirtyDispatchStats {std::size_t pending{};};
struct RegionDirtyTargets {};
using RegionDirtyTargetLookup=std::function<RegionDirtyTargets(std::uint64_t)>;
enum class RegionDirtyOfferResult:std::uint8_t {Accepted,Backpressure,Invalid};
class RegionDirtyDispatcher {
public:
    struct Ingress {std::uint64_t id{};std::size_t count{};};
    std::vector<Ingress> offered;std::size_t capacity{3};std::size_t buffered{};
    std::vector<std::uint64_t> forgotten;
    std::vector<std::uint64_t> replaced;
    std::size_t buffered_edit_count() const {return buffered;}
    std::size_t buffer_capacity() const {return capacity;}
    RegionDirtyOfferResult offer(std::uint32_t,std::uint8_t,std::uint64_t id,
        std::span<const AppliedBrickEdit> edits,RegionDirtyChannels) {
        if(edits.size()>capacity-buffered)return RegionDirtyOfferResult::Backpressure;
        offered.push_back({id,edits.size()});buffered+=edits.size();
        return RegionDirtyOfferResult::Accepted;
    }
    void forget_object(std::uint64_t id) {forgotten.push_back(id);}
    void mark_geometry_replaced(std::uint64_t id) {replaced.push_back(id);}
    RegionDirtyDispatchStats tick(RegionKey,const WorkBudget&,const RegionDirtyTargetLookup&) {
        buffered=0;return {};
    }
};
} // namespace dve
