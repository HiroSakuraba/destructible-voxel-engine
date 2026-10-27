#pragma once
#include "dve/voxel_object.hpp"
namespace dve {class DynamicNavigationWorld {public:int calls{};void mark_voxel_edit(Int3,Int3,float){++calls;}};}
