# GameWorld collision after voxel damage

`damage_sphere` rebuilds the surviving object's collision body after every successful carve,
including a carve that leaves the voxels in one connected piece. A dynamic object keeps its
current world pose and inherits the old body's linear and angular motion at the rebuilt
center of mass. Static collision is rebuilt from the remaining voxel boxes.

When damage leaves several components, the largest stays with the original object. At most
32 other components become dynamic fragments per call. If more than 32 break loose, the 32
largest are selected, breaking equal-size ties by component index. The unselected components
stay in the original object's voxel and collision data. This cap bounds fragment body creation;
it does not suppress the primary collision rebuild.
