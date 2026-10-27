#pragma once
#include "dve/voxel_object.hpp"
#include <span>
namespace dve {
struct PackedBrickmapUpdateStats {std::size_t staleDropped{},published{};};
class PackedBrickmapScene {public:int calls{};int rebuilds{};void rebuild(const VoxelObject&){++rebuilds;}PackedBrickmapUpdateStats update(const VoxelObject& a,std::span<const AppliedBrickEdit> es){
 ++calls;PackedBrickmapUpdateStats s;for(auto& e:es){auto b=a.find_brick(e.key);if(!b||b->generation()!=e.generation)++s.staleDropped;else ++s.published;}return s;}};
}
