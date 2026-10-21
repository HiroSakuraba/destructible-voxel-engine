# Persistent adaptive surface display

The NURBS **Surface Display** menu now offers three adaptive presets alongside
**Use Fixed Display Density and Crease Normals**. These edit the selected source's
display policy; they do not add a new object or change its rational geometry.

| Preset | Chord tolerance (source-local units) | Normal angle (radians) |
| --- | --- | --- |
| Coarse | 0.05 | 0.3 |
| Standard | 0.01 | 0.15 |
| Fine | 0.0025 | 0.075 |

Radio checkmarks reflect the current preset. Custom API settings can match none
of the presets. Fixed display uses the density selected in Construction Settings.
The density field is explicitly labeled **Fixed display samples per span**;
adaptive display does not use it, but the source retains its value. Curves keep
their fixed guide ribbons and cannot enable adaptive surface display.

## Source and cache behavior

`NurbsSource` now stores `adaptiveDisplay`, `chordTolerance` and `normalAngle`.
`tessellate_nurbs` dispatches according to that policy. Creation, CV edits,
weights, transformations of the source, split/crop, undo/redo and cache validation
therefore use the same selected policy. Surface operations that copy sources
retain the policy; extracted boundary/isocurves construct ordinary fixed-display
curve sources. New lofts retain their constructors' existing default policy.

The existing `EditNurbsCommand` prepares the new mesh before changing the source
or cache. An exhausted budget or failed bake returns no result, preserving the
previous source, polygon, materials, command history and control selection.
Preset edits preserve control selection, including selections parked by
Construction Settings. Reapplying an identical preset adds no history item.

Geometry cache comparison includes adaptive normals and tangents even when a
legacy source's `creaseNormals` flag is false. Material/texture resource edits
remain separate and are preserved across retessellation. Bake to New Polygon
copies the selected display cache. The separate adaptive polygon-bake tool from
PR #167 still uses its explicitly labeled 0.01-local-unit settings.

Adaptive source validation checks geometry and policy without applying the
unrelated fixed-density mesh budget. Adaptive baking enforces its own limits:
250,000 evaluations, at most ten refinement steps and at most 65,536 duplicated
cell-corner vertices. Switching back to an excessive fixed density is refused
without changing the current adaptive source/cache.

## Persistence and compatibility

New or edited source strings use **DVE_NURBS 3**. The v3 header adds the adaptive
flag, chord tolerance and normal angle after the v2 texture ranges. Readers
continue to accept v1 and v2; these default to fixed display and retain their
legacy crease/UV semantics. Unchanged legacy component strings need not be
rewritten merely because a scene is saved. Older engine versions cannot read
new v3 source strings.

The policy validates finite chord tolerances in [1e-8,1e4] and normal angles in
[1e-4,pi]. Invalid flags, values and adaptive-curve sources are rejected before
baking. Saving/reopening an edited scene retains source settings and its
matching cache. Source fingerprints include all v3 settings.

## Limits and next work

This is a persistent, source-local adaptive display policy with synchronous
rebuilds. It is not an asynchronous job scheduler or camera-distance LOD system.
Nonuniform object scale can magnify world-space error. Sampled midpoint/normal
tests are not certified bounds and can miss localized features. Separate patches
do not share sampling, even after C0/C1 attachment; edge synchronization and
sewing remain separate work. General curved intersections, arbitrary trim loops
and surface fillets are unchanged by this batch.

The dedicated regression suite covers v1/v2 migration, v3 roundtrip/invalid
metadata, dispatch equivalence, fixed/adaptive budget separation, metadata
retention through surface operations, extracted curves, stale normals,
native presets/radio state, parked controls, adaptive CV updates, budget refusal
atomicity, undo/redo, resource/placement retention, locks and scene save/reopen.
Existing NURBS, polygon, SubD and scene-export suites are also selected. Tests
are headless; OS-window/GPU artist acceptance remains outstanding.

Based on the surface-modeling work in PR #167. Earlier length PRs #165/#166 are
now merged; the surface-modeling dependency remains open at the time of writing.
