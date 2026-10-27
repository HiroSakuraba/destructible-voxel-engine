#include "dve/region_game_damage_host.hpp"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace dve {
struct RegionGameDamageHost::Inbox {
    std::set<GameObjectId> changed;
    std::set<GameObjectId> destroyed;
    std::size_t limit{};
    bool rescan{};
    std::size_t events{};
    std::size_t overflowEvents{};
    void note(const GameDamageEvent& e) {
        ++events;
        if (!e.destroyed) changed.insert(e.objectId);
        else destroyed.insert(e.objectId);
        for (const auto id:e.newFragmentIds)
            if (id != kInvalidGameObjectId) changed.insert(id);
        if (changed.size() > limit || destroyed.size() > limit ||
            changed.size() > limit-destroyed.size()) {
            // Never discard events silently on a bounded inbox overflow.
            rescan = true;
            changed.clear();
            destroyed.clear();
            ++overflowEvents;
        }
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
    const std::weak_ptr<Inbox> weak = inbox_;
    world_.on_damage([weak](const GameDamageEvent& event) {
        if (auto inbox = weak.lock()) inbox->note(event);
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
    const auto views=world_.render_objects();
    std::set<GameObjectId> live;
    for (const auto& v:views) {
        if (!v.voxels) continue;
        live.insert(v.id);
        inbox_->changed.insert(v.id);
    }
    for (auto it=indexedObjects_.begin();it!=indexedObjects_.end();) {
        if (live.contains(*it)) {++it;continue;}
        dispatcher_.forget_object(*it);
        it=indexedObjects_.erase(it);
        ++stats.forgotten;
    }
    ++stats.reconciliations;
}
bool RegionGameDamageHost::begin_snapshot(GameObjectId id, RegionGameDamageHostStats& stats) {
    current_.reset();
    const auto views=world_.render_objects();
    const auto it=std::find_if(views.begin(),views.end(),[&](const GameRenderObject& v){return v.id==id;});
    if (it==views.end() || !it->voxels) {
        dispatcher_.forget_object(id);
        indexedObjects_.erase(id);
        ++stats.forgotten;
        return true;
    }
    Snapshot fresh;
    fresh.id=id;
    const auto keys=it->voxels->bricks().sorted_keys();
    if (keys.size() > config_.maximumSnapshotBricks) {
        // Explicit failure, retained dirty ID. Increase limit or add a streaming
        // brick enumerator; do not silently drop a changed object.
        stats.snapshotLimit=true;
        return false;
    }
    fresh.edits.reserve(keys.size());
    for (const auto key:keys) {
        const auto* brick=it->voxels->find_brick(key);
        if (brick) fresh.edits.push_back({key,Bitset512::full(),brick->generation()});
    }
    // This queue MUST be derived-only. Generation counters may have reset after
    // a GameWorld split, so invalidate earlier derived tickets before re-offering.
    dispatcher_.forget_object(id);
    current_.emplace(std::move(fresh));
    indexedObjects_.insert(id);
    ++stats.objectsSnapshotted;
    return true;
}
RegionGameDamageHostStats RegionGameDamageHost::tick(RegionKey camera, const WorkBudget& budget,
                                                     const RegionDirtyTargetLookup& lookup) {
    RegionGameDamageHostStats stats{};
    if (std::this_thread::get_id()!=ownerThread_) {stats.wrongThread=true;return stats;}
    stats.eventsObserved=inbox_->events;
    stats.overflowEvents=inbox_->overflowEvents;
    reconcile(stats);
    for (const auto id:inbox_->destroyed) {
        dispatcher_.forget_object(id);
        indexedObjects_.erase(id);
        inbox_->changed.erase(id);
        if (current_ && current_->id==id) current_.reset();
        ++stats.forgotten;
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
            if (inbox_->changed.empty() || snapshots >= config_.maximumSnapshotsPerTick) break;
            const auto id=*inbox_->changed.begin();
            if (!begin_snapshot(id,stats)) break;
            inbox_->changed.erase(id);
            ++snapshots;
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
            stats.backpressure=(result==RegionDirtyOfferResult::Backpressure);
            stats.snapshotLimit=(result==RegionDirtyOfferResult::Invalid);
            break;
        }
        item.offset+=size;
        offered+=size;
        stats.editsOffered+=size;
        if (item.offset==item.edits.size()) current_.reset();
    }
    stats.pendingObjects=inbox_->changed.size()+(current_?1U:0U);
    stats.pendingBricks=current_?current_->edits.size()-current_->offset:0;
    stats.delivery=dispatcher_.tick(camera,budget,lookup);
    return stats;
}
} // namespace dve
