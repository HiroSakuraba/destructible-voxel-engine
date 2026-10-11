# Muse handoff: full-program validation and streaming GPU metrics

Merge PR #109 before its editor/runtime follow-up (or merge both in order). Run the
native program in Muse's environment. The CPU player and GameWorld are implemented;
the streaming GPU scene loader deliberately refuses nonunit DVOX until the contract
below is implemented and verified. Do not remove that guard alone.

## Native validation

1. Scale a rotated two-material voxel object by 1.3 on X, then separately on Y/Z.
   Check framing, picking, exposed faces, collision overlay, voxel selection and
   placement corners. Grid count, materials, anchors and voxel revision must remain
   unchanged. Check the single Undo step, repeated Undo/Redo, Escape and focus loss.
2. Hold Alt after starting a drag. Confirm all axes share the factor and existing
   aspect ratio survives. Ctrl at drag start must still resample occupancy. Check
   Shift snap override, typed values and the effective 1 mm/10 m cell limits.
3. Repeat with a mixed rotation group and a parent/child selection. Confirm the
   documented local-axis/group-pivot behavior; inspect for unexpected child jumps.
4. Slice, Extract, Separate, Boolean/Join, copy/paste and print/export the scaled
   result. Reopen its scene/DVOX. Confirm selection, materials, anchors and dimensions.
5. Play with reference and production physics. Drop an elongated object, collide
   a capsule with its compressed axis, and damage it at a known world-space radius.
   Compare immediate and queued fragments, their velocity/COM/inertia, save/load,
   and CPU screenshots containing multiple voxel sizes. Check attachments too.
6. Ask artists to approve these workflows before declaring interaction criteria
   complete. Record the build/backend, commands, screenshots and observed failures.

## Remaining GPU implementation

Extend `RuntimeBrickmapCreateDesc`, its update alias, persistent object views and stored objects
in `include/dve/runtime_brickmap_world.hpp` with positive cell dimensions. Carry
`asset.voxelSizeMeters * asset.voxelScale` through staging, reload, dirty publication,
resource identity/hash and previous/current frame records. Follow it through
`primary_brickmap_trace.hlsl` and `common/brick_trace_common.hlsli`, including shadow,
GI and translucent rays. Update CPU/HLSL reflection and buffer layout together.
Do not put geometry scale into `RigidTransform`.

The following is sample shader math, not a drop-in patch. `R` is the normalized
object-to-world rotation. DDA currently normalizes its local direction, so both
its maximum travel and returned distance need the reciprocal metric conversion:

```hlsl
float3 spacing = baseVoxelMeters * axisScale;
float3 localOrigin = mul(transpose(R), worldOrigin - objectOrigin) / spacing;
float3 localDirection = mul(transpose(R), normalize(worldDirection)) / spacing;
float metric = length(localDirection);
// Validate spacing and metric before publishing an instance/ray.
float3 localUnitDirection = localDirection / metric;
float localMaximum = worldMaximum * metric;
// DDATrace(localOrigin, localUnitDirection, localMaximum) returns localDistance.
float worldDistance = localDistance / metric;
float3 voxelPoint = localOrigin + localUnitDirection * localDistance;
float3 worldPoint = objectOrigin + mul(R, voxelPoint * spacing);
float3 worldNormal = normalize(mul(R, voxelNormal / spacing));
```

Broadphase boxes use all eight transformed scaled corners. Capsule radii stay in
meters: scale voxel boxes, not the capsule into an ellipsoid. Port the exact volume
and inertia conversion in `voxel_axis_physics.hpp` to every streaming physics build
and fragment/reload path; apply the scaled COM offset when recovering object origins.
All hit distances and depth comparisons remain in world units. Keep the existing
unit-metric ray path unchanged for deterministic lighting baselines.

Add GPU/reference agreement tests for `(2, 0.5, 3)` dimensions under rotation,
nonuniform normals, closest-hit ordering across mixed sizes, shadow/GI distance,
fragments and reloads. Only then remove the rejection in `RuntimeSceneWorld::load_object_asset`.
