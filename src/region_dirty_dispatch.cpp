#include "dve/region_dirty_dispatch.hpp"
#include "dve/gpu_brick_mirror.hpp"
#include "dve/packed_brickmap.hpp"
#include "dve/v235_foundations.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace dve {
namespace {
using Clock = std::chrono::steady_clock;
[[nodiscard]] std::int32_t clamp_coord(std::int64_t v) noexcept {
    return static_cast<std::int32_t>(std::clamp(v,
        static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()),
        static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())));
}
[[nodiscard]] BrickKey region_corner(RegionKey camera, std::int32_t radius, bool high) noexcept {
    const auto d = high ? static_cast<std::int64_t>(radius) : -static_cast<std::int64_t>(radius);
    return {clamp_coord(static_cast<std::int64_t>(camera.x)+d),
            clamp_coord(static_cast<std::int64_t>(camera.y)+d),
            clamp_coord(static_cast<std::int64_t>(camera.z)+d)};
}
[[nodiscard]] bool valid_channels(RegionDirtyChannels ch) noexcept {
    return ch != 0 && (ch & static_cast<RegionDirtyChannels>(~kAllRegionDirtyChannels)) == 0;
}
} // namespace

RegionDirtyDispatcher::RegionDirtyDispatcher(RegionDirtyIndex& queue, RegionDirtyDispatchConfig config)
    : queue_(queue), config_(config) {
    if (config.maximumBufferedEdits == 0 || config.maximumTicketsPerTick == 0 ||
        config.maximumDispatcherMicroseconds == 0 || config.interestRadius < 0)
        throw std::invalid_argument("invalid R3.1 dispatch budget");
}

RegionDirtyOfferResult RegionDirtyDispatcher::offer(std::uint32_t world, std::uint8_t lod,
    std::uint64_t id, std::span<const AppliedBrickEdit> edits, RegionDirtyChannels channels) {
    if (id == 0 || !valid_channels(channels)) return RegionDirtyOfferResult::Invalid;
    if (static_cast<std::size_t>(std::popcount(static_cast<unsigned>(channels))) > queue_.task_capacity())
        return RegionDirtyOfferResult::Invalid;
    if (edits.size() > config_.maximumBufferedEdits - bufferedEdits_) return RegionDirtyOfferResult::Backpressure;
    // Allocation and copy finish before mutating observable queue state.
    Buffered buffer{world, lod, id, channels, {edits.begin(),edits.end()}, 0};
    if (buffer.edits.empty()) return RegionDirtyOfferResult::Accepted;
    ingress_.push_back(std::move(buffer));
    bufferedEdits_ += edits.size();
    totalOffered_ += edits.size();
    return RegionDirtyOfferResult::Accepted;
}

void RegionDirtyDispatcher::forget_object(std::uint64_t id) {
    for (auto it=ingress_.begin(); it!=ingress_.end();) {
        if (it->objectId == id) {
            bufferedEdits_ -= it->edits.size()-it->cursor;
            it=ingress_.erase(it);
        } else ++it;
    }
    rebuild_.erase(id);
    (void)queue_.forget_object(id);
}

void RegionDirtyDispatcher::mark_geometry_replaced(std::uint64_t id) {
    if (id != 0) rebuild_.insert(id);
}

void RegionDirtyDispatcher::forward_ingress(RegionDirtyDispatchStats& stats, Clock::time_point deadline) {
    // Forwarding is bounded by `deadline` (checked every 64 edits) so a huge accepted
    // batch cannot consume the whole tick; the batch cursor keeps the remainder for the
    // next tick.
    constexpr std::size_t kClockStride = 64U;
    std::size_t sinceClock = 0;
    while (!ingress_.empty()) {
        auto& batch=ingress_.front();
        while (batch.cursor < batch.edits.size()) {
            if (++sinceClock >= kClockStride) {
                sinceClock = 0;
                if (Clock::now() >= deadline) { stats.hitTimeBudget = true; return; }
            }
            const auto outcome = queue_.submit(batch.worldId,batch.lod,batch.objectId,
                batch.edits[batch.cursor],batch.channels);
            if (outcome == RegionDirtySubmitResult::CapacityExceeded) return;
            if (outcome == RegionDirtySubmitResult::Invalid) {
                // Retain malformed events: never silently consume them.
                ++stats.failed;
                return;
            }
            if (outcome == RegionDirtySubmitResult::Coalesced) ++stats.coalesced;
            ++stats.forwarded;
            ++batch.cursor;
            --bufferedEdits_;
        }
        ingress_.pop_front();
    }
}

RegionDirtyDispatchStats RegionDirtyDispatcher::tick(RegionKey camera, const WorkBudget& budget,
    const RegionDirtyTargetLookup& lookup) {
    RegionDirtyDispatchStats stats{};
    stats.pressure=budget.pressure;
    if (!lookup) { stats.failed=1; stats.buffered=bufferedEdits_; stats.pending=queue_.task_count(); return stats; }
    const auto start=Clock::now();
    // Ingress forwarding gets at most half of the dispatcher budget so publication is
    // never starved by sustained ingress.
    forward_ingress(stats, start + std::chrono::microseconds(std::max<std::uint32_t>(1U, config_.maximumDispatcherMicroseconds / 2U)));
    stats.buffered=bufferedEdits_;
    stats.offered=totalOffered_;
    const auto time_exceeded=[&] {
        return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-start).count() >=
            static_cast<std::int64_t>(config_.maximumDispatcherMicroseconds);
    };
    const std::size_t cap=std::min<std::size_t>(config_.maximumTicketsPerTick, budget.patchBrickLimit);
    if (cap==0 || time_exceeded()) { stats.hitTicketBudget=(cap==0); stats.hitTimeBudget=stats.hitTimeBudget||time_exceeded(); stats.pending=queue_.task_count(); return stats; }
    const BrickKey lo=region_corner(camera,config_.interestRadius,false);
    const BrickKey hi=region_corner(camera,config_.interestRadius,true);

    // Every frame reserves some service for navigation; with a one-ticket
    // budget, alternate which consumer gets first refusal. Empty navigation
    // queues surrender their reserved slot to GPU rather than waste a frame.
    const auto gpuChannel=region_dirty_channel(RegionDirtyConsumer::Gpu);
    const auto navChannel=region_dirty_channel(RegionDirtyConsumer::Navigation);
    auto publish_nav=[&](std::size_t maximumTickets) -> std::size_t {
        if (maximumTickets==0 || time_exceeded()) return 0;
        auto nav=queue_.lease_box(camera.worldId,camera.lod,lo,hi,maximumTickets,navChannel);
        if (!nav) { stats.queryRejected=true; return 0; }
        for (const auto& t:*nav) {
            if (time_exceeded()) { stats.hitTimeBudget=true; (void)queue_.complete(t,false); continue; }
            const auto target=lookup(t.key.objectId);
            const auto bounds=RegionDirtyIndex::voxel_bounds(t.key.brick);
            bool ok=false;
            if (target.navigation && bounds && std::isfinite(target.metresPerVoxel) && target.metresPerVoxel>0.0F) {
                try { target.navigation->mark_voxel_edit(bounds->minimum,bounds->maximum,target.metresPerVoxel); ok=true; }
                catch (...) { ++stats.failed; }
            }
            const auto done=queue_.complete(t,ok);
            if (done==RegionDirtyCompletion::Retired) ++stats.navigationInvalidations;
            else if (done==RegionDirtyCompletion::Superseded) ++stats.superseded;
            else ++stats.retried;
        }
        return nav->size();
    };
    // Rebuilds the derived GPU state of one object from its authority snapshot. Used for
    // explicit geometry replacement and for tickets whose brick vanished from, or has an
    // older generation than, the authority (incremental updates cannot remove slots).
    const auto rebuild_object=[&](const RegionDirtyTargets& target) -> bool {
        if (!target.authority || (!target.mirror && !target.packed)) return false;
        try {
            if (target.mirror) target.mirror->rebuild(*target.authority);
            if (target.packed) target.packed->rebuild(*target.authority);
        } catch (...) { ++stats.failed; return false; }
        ++stats.gpuRebuilds;
        return true;
    };
    // At most one explicit rebuild per tick: it is not preemptible.
    for (auto it=rebuild_.begin(); it!=rebuild_.end() && !time_exceeded(); ++it) {
        if (rebuild_object(lookup(*it))) { rebuild_.erase(it); break; }
    }
    if (time_exceeded()) { stats.hitTimeBudget=true; stats.pending=queue_.task_count(); return stats; }
    const bool favorNav=favorNavNext_;
    favorNavNext_=!favorNavNext_;
    const std::size_t preNav=(cap==1 && favorNav)?publish_nav(1):0;
    const std::size_t gpuQuota=(cap==1)?cap-preNav:cap-1;
    auto gpu=gpuQuota ? queue_.lease_box(camera.worldId,camera.lod,lo,hi,gpuQuota,gpuChannel)
                      : std::optional<std::vector<RegionDirtyTicket>>(std::vector<RegionDirtyTicket>{});
    if (!gpu) { stats.queryRejected=true; stats.pending=queue_.task_count(); return stats; }
    std::map<std::uint64_t,std::vector<RegionDirtyTicket>> groups;
    for (const auto& t:*gpu) groups[t.key.objectId].push_back(t);
    std::size_t used=0;
    for (const auto& [id,tickets]:groups) {
        if (time_exceeded()) { stats.hitTimeBudget=true; for (const auto& t:tickets) (void)queue_.complete(t,false); continue; }
        const RegionDirtyTargets target=lookup(id);
        if (!target.authority || (!target.mirror && !target.packed)) {
            for (const auto& t:tickets) { (void)queue_.complete(t,false); ++stats.retried; }
            continue;
        }
        // A brick that no longer exists in the authority, or whose generation went backwards
        // (object storage replaced), cannot be expressed as an incremental update and would
        // otherwise be retried forever at the head of the queue. Reconcile by rebuilding.
        const bool diverged=std::any_of(tickets.begin(),tickets.end(),[&](const RegionDirtyTicket& t){
            const Brick* b=target.authority->find_brick(t.key.brick);
            return b==nullptr || b->generation() < t.generation;
        });
        if (diverged) {
            const bool rebuilt=rebuild_object(target);
            for (const auto& t:tickets) {
                const auto done=queue_.complete(t,rebuilt);
                if (done==RegionDirtyCompletion::Retired) ++stats.gpuTickets;
                else if (done==RegionDirtyCompletion::Superseded) ++stats.superseded;
                else ++stats.retried;
            }
            used += tickets.size();
            continue;
        }
        std::vector<AppliedBrickEdit> edits;
        edits.reserve(tickets.size());
        for (const auto& t:tickets) {
            const Brick* b=target.authority->find_brick(t.key.brick);
            if (b && b->generation() == t.generation) edits.push_back(RegionDirtyIndex::gpu_edit(t));
            else {
                // Do not retire work against a changed authority snapshot. Reissue
                // with current generation when possible; the old ticket is superseded.
                if (b && b->generation() > t.generation) {
                    (void)queue_.submit_full(t.key.region.worldId,t.key.region.lod,id,
                        t.key.brick,b->generation(),gpuChannel);
                }
            }
        }
        bool ok=false;
        if (!edits.empty()) {
            try {
                if (target.mirror) {
                    const auto s=target.mirror->update(*target.authority,edits);
                    if (s.staleDropped != 0) throw std::runtime_error("GPU mirror stale generation");
                }
                if (target.packed) {
                    const auto s=target.packed->update(*target.authority,edits);
                    if (s.staleDropped != 0) throw std::runtime_error("packed GPU stale generation");
                }
                ok=true;
                ++stats.gpuBatches;
            } catch (...) { ++stats.failed; }
        }
        for (const auto& t:tickets) {
            const bool current=std::any_of(edits.begin(),edits.end(),[&](const AppliedBrickEdit& e){return e.key==t.key.brick && e.generation==t.generation;});
            const auto done=queue_.complete(t,ok && current);
            if (done==RegionDirtyCompletion::Retired) ++stats.gpuTickets;
            else if (done==RegionDirtyCompletion::Superseded) ++stats.superseded;
            else ++stats.retried;
        }
        used += tickets.size();
    }
    if (preNav + used >= cap) stats.hitTicketBudget=true;

    if (!time_exceeded() && preNav + used < cap)
        (void)publish_nav(cap-preNav-used);
    stats.pending=queue_.task_count();
    stats.hitTimeBudget=stats.hitTimeBudget||time_exceeded();
    return stats;
}
} // namespace dve
