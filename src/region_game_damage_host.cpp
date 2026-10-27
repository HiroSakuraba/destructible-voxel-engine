#include "dve/region_game_damage_host.hpp"
#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <utility>

namespace dve {
struct RegionGameDamageHost::Inbox {
    // Event ids (bounded by `limit`). `sweep` holds ids queued by a full rescan; it is
    // intentionally not bounded by `limit` (it is O(live objects), documented) so that a
    // world with more objects than `limit` can still converge under continuing damage.
    std::set<GameObjectId> changed;
    std::set<GameObjectId> destroyed;
    std::set<GameObjectId> sweep;
    std::size_t limit{};
    bool rescan{};
    std::size_t events{};
    std::size_t overflowEvents{};
    std::thread::id owner{};
    std::atomic<std::size_t> foreignEvents{0};

    // GameWorld is single-threaded (authority thread). If a listener is ever invoked from
    // another thread we must not touch the unlocked sets; record it and force a rescan.
    [[nodiscard]] bool foreign() noexcept {
        if (std::this_thread::get_id() == owner) return false;
        foreignEvents.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    void bound() {
        if (changed.size() > limit || destroyed.size() > limit ||
            changed.size() > limit-destroyed.size()) {
            // Never discard events silently on a bounded inbox overflow.
            rescan = true;
            changed.clear();
            destroyed.clear();
            ++overflowEvents;
        }
    }
    void note_damage(const GameDamageEvent& e) {
        if (foreign()) return;
        ++events;
        if (e.objectId != kInvalidGameObjectId) {
            if (!e.destroyed) { changed.insert(e.objectId); sweep.erase(e.objectId); }
            else destroyed.insert(e.objectId);
        }
        for (const auto id:e.newFragmentIds)
            if (id != kInvalidGameObjectId) { changed.insert(id); sweep.erase(id); }
        bound();
    }
    // Fires for every GameWorld::destroy_object, including retire_oldest_debris(), pool
    // release and explicit destruction, none of which necessarily emit a damage event.
    void note_destroyed(GameObjectId id) {
        if (foreign()) return;
        ++events;
        if (id == kInvalidGameObjectId) return;
        destroyed.insert(id);
        bound();
    }
};

RegionGameDamageHost::RegionGameDamageHost(GameWorld& world, RegionDirtyDispatcher& dispatcher,
    RegionGameDamageHostConfig config)
    : world_(world), dispatcher_(dispatcher), config_(config),
      ownerThread_(std::this_thread::get_id()), inbox_(std::make_shared<Inbox>()) {
    const RegionDirtyChannels derived = static_cast<RegionDirtyChannels>(
        region_dirty_channel(RegionDirtyConsumer::Gpu) |
        region_dirty_channel(RegionDirtyConsumer::Navigation));
    if (config.maximumPendingIds == 0 || config.maximumSnapshotsPerTick == 0 ||
        config.maximumOffersPerTick == 0 || config.maximumSnapshotBricks == 0 ||
        config.channels == 0 || (config.channels & static_cast<RegionDirtyChannels>(~derived)) != 0)
        throw std::invalid_argument("invalid derived-only region damage host configuration");
    inbox_->limit = config.maximumPendingIds;
    inbox_->owner = ownerThread_;
    const std::weak_ptr<Inbox> weak = inbox_;
    // GameWorld has no unsubscribe token: both callbacks stay registered (inert once the
    // inbox expires) for the GameWorld's lifetime, so avoid churning hosts per frame.
    world_.on_damage([weak](const GameDamageEvent& event) {
        if (auto inbox = weak.lock()) inbox->note_damage(event);
    });
    world_.on_destroyed([weak](GameObjectId id) {
        if (auto inbox = weak.lock()) inbox->note_destroyed(id);
    });
}
RegionGameDamageHost::~RegionGameDamageHost() { inbox_.reset(); }
void RegionGameDamageHost::request_full_reconcile() {
    if (std::this_thread::get_id()!=ownerThread_) throw std::logic_error("wrong region host thread");
    inbox_->rescan=true;
}
void RegionGameDamageHost::reconcile(RegionGameDamageHostStats& stats) {
    if (!inbox_->rescan) return;
    inbox_->rescan=false;
    if (current_) {
        dispatcher_.forget_object(current_->id);
        current_.reset();
    }
    inbox_->changed.clear();
    inbox_->destroyed.clear();
    oversized_.clear(); // an explicit rescan retries every oversized object once
    const auto views=world_.render_objects();
    std::set<GameObjectId> live;
    for (const auto& v:views) {
        if (!v.voxels) continue;
        live.insert(v.id);
        inbox_->sweep.insert(v.id);
    }
    for (auto it=inbox_->sweep.begin();it!=inbox_->sweep.end();) {
        if (live.contains(*it)) ++it; else it=inbox_->sweep.erase(it);
    }
    for (auto it=indexedObjects_.begin();it!=indexedObjects_.end();) {
        if (live.contains(*it)) {++it;continue;}
        dispatcher_.forget_object(*it);
        it=indexedObjects_.erase(it);
        ++stats.forgotten;
    }
    ++stats.reconciliations;
}
RegionGameDamageHost::SnapshotResult RegionGameDamageHost::begin_snapshot(GameObjectId id,
                                                                          RegionGameDamageHostStats& stats) {
    current_.reset();
    const auto views=world_.render_objects();
    const auto it=std::find_if(views.begin(),views.end(),[&](const GameRenderObject& v){return v.id==id;});
    if (it==views.end() || !it->voxels) {
        dispatcher_.forget_object(id);
        indexedObjects_.erase(id);
        oversized_.erase(id);
        ++stats.forgotten;
        return SnapshotResult::Forgotten;
    }
    Snapshot fresh;
    fresh.id=id;
    const auto keys=it->voxels->bricks().sorted_keys();
    if (keys.size() > config_.maximumSnapshotBricks) {
        // Explicit failure with retained dirty state (oversized_): the caller skips this
        // object so later ids still progress. Increase the limit or add a streaming brick
        // enumerator; do not silently drop a changed object.
        return SnapshotResult::TooLarge;
    }
    fresh.edits.reserve(keys.size());
    for (const auto key:keys) {
        const auto* brick=it->voxels->find_brick(key);
        if (brick) fresh.edits.push_back({key,Bitset512::full(),brick->generation()});
    }
    // This queue MUST be derived-only. Generation counters may have reset after
    // a GameWorld split, so invalidate earlier derived tickets before re-offering.
    dispatcher_.forget_object(id);
    // fresh.edits only name bricks that exist now, and GPU mirrors never drop slots on an
    // incremental update (R3.1 follow-up). Bricks removed by damage, a queued destruction
    // commit or a fragment split would stay visible, so always request a rebuild from the
    // authority (retained by the dispatcher until it succeeds or forget_object).
    dispatcher_.mark_geometry_replaced(id);
    current_.emplace(std::move(fresh));
    indexedObjects_.insert(id);
    oversized_.erase(id);
    ++stats.objectsSnapshotted;
    return SnapshotResult::Started;
}
RegionGameDamageHostStats RegionGameDamageHost::tick(RegionKey camera, const WorkBudget& budget,
                                                     const RegionDirtyTargetLookup& lookup) {
    RegionGameDamageHostStats stats{};
    if (std::this_thread::get_id()!=ownerThread_) {stats.wrongThread=true;return stats;}
    stats.foreignThreadEvents=inbox_->foreignEvents.load(std::memory_order_relaxed);
    if (stats.foreignThreadEvents != foreignSeen_) {
        foreignSeen_=stats.foreignThreadEvents;
        inbox_->rescan=true; // events from another thread were not recorded
    }
    stats.eventsObserved=inbox_->events;
    stats.overflowEvents=inbox_->overflowEvents;
    reconcile(stats);
    for (const auto id:inbox_->destroyed) {
        // Only ids this host ever indexed can have dispatcher state; unrelated
        // (non-voxel) object churn must not cost an ingress scan.
        if (indexedObjects_.erase(id)>0 || (current_ && current_->id==id)) {
            dispatcher_.forget_object(id);
            ++stats.forgotten;
        }
        inbox_->changed.erase(id);
        inbox_->sweep.erase(id);
        oversized_.erase(id);
        if (current_ && current_->id==id) current_.reset();
    }
    inbox_->destroyed.clear();
    if (current_ && inbox_->changed.contains(current_->id)) {
        dispatcher_.forget_object(current_->id);
        current_.reset();
    }
    std::size_t snapshots=0;
    std::size_t offered=0;
    while (offered < config_.maximumOffersPerTick) {
        if (!current_) {
            if (snapshots >= config_.maximumSnapshotsPerTick) break;
            // Fresh damage events first, then the rescan sweep.
            GameObjectId id=kInvalidGameObjectId;
            if (!inbox_->changed.empty()) id=*inbox_->changed.begin();
            else if (!inbox_->sweep.empty()) {
                // Round-robin: a rescan that keeps being re-triggered by overflow must
                // not restart at the lowest id every time and starve the high ids.
                auto it=inbox_->sweep.upper_bound(sweepCursor_);
                if (it==inbox_->sweep.end()) it=inbox_->sweep.begin();
                id=*it;
                sweepCursor_=id;
            } else break;
            const auto result=begin_snapshot(id,stats);
            inbox_->changed.erase(id);
            inbox_->sweep.erase(id);
            ++snapshots;
            if (result==SnapshotResult::TooLarge) {oversized_.insert(id);continue;}
            if (!current_) continue;
        }
        auto& item=*current_;
        if (item.offset==item.edits.size()) {current_.reset();continue;}
        const std::size_t buffered=dispatcher_.buffered_edit_count();
        const std::size_t capacity=dispatcher_.buffer_capacity();
        const std::size_t room=buffered<=capacity?capacity-buffered:0;
        if (room==0) {stats.backpressure=true;break;}
        const std::size_t size=std::min({room,config_.maximumOffersPerTick-offered,
                                        item.edits.size()-item.offset});
        const auto result=dispatcher_.offer(config_.worldId,config_.lod,item.id,
            std::span<const AppliedBrickEdit>(item.edits.data()+item.offset,size),config_.channels);
        if (result!=RegionDirtyOfferResult::Accepted) {
            if (result==RegionDirtyOfferResult::Backpressure) stats.backpressure=true;
            else stats.snapshotLimit=true;
            break;
        }
        item.offset+=size;
        offered+=size;
        stats.editsOffered+=size;
        if (item.offset==item.edits.size()) current_.reset();
    }
    stats.oversizedObjects=oversized_.size();
    if (!oversized_.empty()) stats.snapshotLimit=true;
    stats.pendingObjects=inbox_->changed.size()+inbox_->sweep.size()+(current_?1U:0U);
    stats.pendingBricks=current_?current_->edits.size()-current_->offset:0;
    stats.delivery=dispatcher_.tick(camera,budget,lookup);
    return stats;
}
} // namespace dve
