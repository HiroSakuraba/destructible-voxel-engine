# meshoptimizer subset (v1.3)

Pinned upstream release: https://github.com/zeux/meshoptimizer/releases/tag/v1.3
Commit: `9e1f07b159d3cb777f1c67ed31fc11fd117986f4`

DVE vendors only the files needed for the optional voxel remesh prototype:
`meshoptimizer.h`, `allocator.cpp`, `indexgenerator.cpp`, `remesher.cpp`, and
`tangentspace.cpp`.
The remeshing and normal-generation APIs are marked experimental upstream; keep
this dependency isolated behind `dve::remesh_triangle_mesh` so it can be updated
or replaced without exposing meshoptimizer types to DVE callers.

See `LICENSE.md` for the upstream MIT license.
