#pragma once
#include "dve/voxel_object.hpp"
#include <span>
#include <stdexcept>
namespace dve {
struct GpuMirrorUpdateStats {std::size_t submitted{},published{},staleDropped{};};
class GpuBrickMirror {public:int calls{};int rebuilds{};bool throwOnUpdate{};bool throwOnRebuild{};void rebuild(const VoxelObject&){if(throwOnRebuild)throw std::runtime_error("fake GPU rebuild failure");++rebuilds;}GpuMirrorUpdateStats update(const VoxelObject& a,std::span<const AppliedBrickEdit> es){
 if(throwOnUpdate)throw std::runtime_error("fake GPU failure");
 ++calls;GpuMirrorUpdateStats s;for(auto& e:es){++s.submitted;auto b=a.find_brick(e.key);if(!b||b->generation()!=e.generation)++s.staleDropped;else ++s.published;}return s;}};
}
