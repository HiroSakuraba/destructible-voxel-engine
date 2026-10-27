#pragma once
#include "dve/region_dirty_dispatch.hpp"
#include <cstdint>
#include <functional>
#include <map>
#include <vector>
namespace dve {
using GameObjectId=std::uint64_t;
constexpr GameObjectId kInvalidGameObjectId=0;
struct GameDamageEvent {
    GameObjectId objectId{};bool destroyed{};std::vector<GameObjectId> newFragmentIds;
};
struct Brick {std::uint32_t gen{};std::uint32_t generation() const {return gen;}};
struct FlatBrickMap {
    std::map<BrickKey,Brick> contents;
    std::vector<BrickKey> sorted_keys() const {
        std::vector<BrickKey> keys;for(auto& [key,value]:contents)keys.push_back(key);
        return keys;
    }
};
struct VoxelObject {
    FlatBrickMap map;
    const FlatBrickMap& bricks()const{return map;}
    const Brick* find_brick(BrickKey key)const {
        auto it=map.contents.find(key);return it==map.contents.end()?nullptr:&it->second;
    }
};
struct GameRenderObject {GameObjectId id{};const VoxelObject* voxels{};};
class GameWorld {
public:
    std::map<GameObjectId,VoxelObject> vox;
    std::vector<std::function<void(const GameDamageEvent&)>> listeners;
    void on_damage(std::function<void(const GameDamageEvent&)> fn) {listeners.push_back(std::move(fn));}
    void damage(GameDamageEvent e) {for(auto& listener:listeners)listener(e);}
    std::vector<GameRenderObject> render_objects() const {
        std::vector<GameRenderObject> views;
        for(auto& [id,v]:vox)views.push_back({id,&v});
        return views;
    }
};
} // namespace dve
