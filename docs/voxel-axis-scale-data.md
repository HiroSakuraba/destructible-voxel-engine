# Voxel axis-scale data foundation

Voxel geometry gains positive object-local axis scale, independent of rigid pose
and base voxel size. Cell dimensions are `voxelSizeMeters * voxelScale`. Integer
occupancy, material IDs and anchors are unchanged. This represents rotated boxes
without adding shear or scale to rigid physics transforms.

`voxel_metric.hpp` provides component arithmetic, point conversion and the inverse
transpose normal conversion. `raycast_voxels_scaled` normalizes the transformed ray
and converts its travel distance back to world units. Its hit distance and position
remain in world units even under unequal axis dimensions.

Editor scene version 6 persists three scale components; scene versions 1–5 default
to unit scale. Cloning preserves scale. DVOX 1.3 appends the three scale components
to its header and covers them with the full-content hash. Unit-scale assets retain
the existing DVOX 1.2 encoding, and older DVOX files default to unit scale. Invalid
or nonfinite axis scale is rejected.

This PR contains the data/conversion foundation. Editor controls, runtime geometry,
collision and renderer integration are supplied by the following PR. It does not
enable a user-facing axis scale workflow by itself.
