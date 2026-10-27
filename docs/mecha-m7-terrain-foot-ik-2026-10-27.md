# Mecha M7: planted feet, terrain-adaptive leg IK and foot pads

**Dependency:** M7 branches from **M6 PR #248** (which is not merged as of implementation). The M7 pull request therefore targets the M6 feature branch. It should be rebased onto `main` only after M6 merges.

## Player-facing improvement

Iron Yard's M6 two-thigh/two-shin mechanical legs now use **analytic two-link inverse kinematics** (IK) instead of rotating the four segments by a simple periodic stride. Each shin connects a solved knee to a foot target, and each thigh connects that knee to the chassis hip. The new foot-pad meshes orient to the contact normal of the terrain underneath.

When walking across a slope, left and right foot probes return independent heights and surface normals. Each leg alternates between:
- **Stance:** hold the ankle at a persistent *world-space* contact point. The chassis may pass over the planted foot without dragging it along.
- **Swing:** lift and move that foot along an eased sinusoidal arc to a predicted forward landing location. Ground is queried at touchdown, not guessed from a fixed floor height.
- **No support:** if destructible voxels or polygon scenery no longer provide a valid foot contact, the foot immediately relinquishes its planted state and retracts near the chassis.
- **Jet flight:** airborne legs retract; upon landing, independent terrain contacts are reacquired without adding physical leg colliders.

The existing swept physics capsule remains authoritative for player motion. Feet are **non-physical visual targets**: their planted state does not invent traction or override rigid-body collision.

## M7 architecture

- `include/dve/mecha_leg_ik.hpp` and `src/mecha_leg_ik.cpp` implement a pure, deterministic, allocation-free two-bone solver. The law of cosines finds the knee relative to an authored pole direction (slightly forward in chassis-local space). Reach is clamped at the joint limits; zero-length vectors, unreachable targets and invalid dimensions are handled explicitly.
- `MechaGame` keeps **two FootState objects** (`-1` left, `+1` right) with world-space ankle position, planned swing endpoints, support normal, initialization, planted, and swing flags.
- Terrain is probed from above each target with `GameWorld::raycast` in **Z-up**; the same query covers polygon and voxel collision proxies. Surface normals shall have a significant positive Z component, rejecting ceilings and steep cliff faces.
- Step cadence reuses M6's distance-advanced gait phase and fixed-timestep update. Authored `foot_lift` and `step_reach` tune the swing arc and prediction distance; `thigh_length` and `shin_length` set exact rig proportions.
- Independent visual components `dve.prototype.mech_foot` support left/right foot pads. Each pad follows its IK ankle and aligns its local Z axis to the ground normal.
- Existing scenes with no `mech_leg` or `mech_foot` roles continue using their M6-compatible chassis movement without a new rig requirement. A partial four-link set, duplicated slots, orphan foot pads or invalid link lengths are rejected when authoring/exporting.

## Controller saves

The **application section name remains `game.mecha.v1`**. Its content is now `DVE_MECHA 3`, appending an `M7` extension with both feet's stance/plant states, swing endpoints and contact normals.

The parser continues accepting `DVE_MECHA 1` (M5) and `DVE_MECHA 2` (M6) sections. On those older saves, contacts are rebuilt from the **restored** world and the restored motor position. Version-3 foot state is validated completely before writing live state. This preserves the load-failure atomicity established in M5.

## Tests and checks

The dedicated `dve_mecha_m7_tests` target covers:
- Upper and lower link lengths, knee bending direction, unreachable feet, invalid link dimensions, near-singular solve and finite math.
- Stationary planted-feet invariance, world-space foot pad alignment, and contacts after movement.
- Slope elevation differences and normal alignment for each independent foot.
- Destroyed supporting geometry causing immediate unplant/retraction without NaNs.
- `DVE_MECHA 3` roundtrip, reject-malformed-state and read of legacy v2/v1 checkpoints.
- The Iron Yard prefab having exactly four IK leg segments and two foot-pad objects, with component validation.

```sh
cmake --build build --target dve_mecha_m7_tests dve_mecha_m6_tests dve_mecha_m5_tests
ctest --test-dir build -R "dve_mecha_(m7|m6|m5|m3_m4|game)_tests" --output-on-failure
```

## Known limits and intended M8 work

M7 is **visual IK**, not a powered knee/ankle physics simulation; the foot does not transmit constraint forces or friction to the chassis. The solver treats links as ideal rods and does not yet support arbitrary articulated rigs, nonuniform limb dimensions from mesh data, moving-platform foot anchoring, or animation retargeting. Ground probes may require caching for very dense battles. Camera, combat and network synchronization beyond M7 remain separate efforts.
