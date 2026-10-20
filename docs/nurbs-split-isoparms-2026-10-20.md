# Native NURBS splitting, isocurves and parameter directions

Builds on [loft and sweep](nurbs-loft-sweep-2026-10-20.md). This batch adds
source-preserving rational operations to the native NURBS menu.

## Workflow

Open **NURBS > Construction and Split/Isocurve Settings**. The fourth numeric
field sets the operation parameter in the normalized range 0..1. Click, type,
and press Enter; Escape cancels. The value maps linearly to the selected source
axis's existing knot domain. It is a parameter fraction, not an arc-length or
physical-distance fraction. Settings are transient controller state.

Select one visible unlocked native source, then use:

| Tool | Result |
| --- | --- |
| Split U / Split V at Parameter | Two independent editable pieces, retaining the original. Both pieces enter the scene in one undo command. |
| Extract Isocurve at Fixed U | A rational curve running in V on a surface at the chosen U. |
| Extract Isocurve at Fixed V | A rational curve running in U on a surface at the chosen V. |
| Reverse U / Reverse V Direction | Changes the selected source's parameter direction and control ordering. |
| Swap Surface U/V | Transposes the control net, degrees and knot axes. |
| Insert U / V Knot at Parameter | Shape-preserving refinement at the chosen parameter. Existing midpoint commands remain available. |

Curves support U split, U reversal and U insertion. V tools, swapping and
isocurve extraction require surfaces. Splitting and insertion require a strict
interior parameter; extraction permits 0 and 1 for boundary curves. Prefab
instances must be detached before editing.

Split pieces retain their original parameter intervals; their knot domains are
not renormalized. The common boundary controls appear in both independent
pieces. There is no live stitching relationship after creation. New objects
are unparented, using the original world pose, nonuniform scale, materials,
texture resources, tags, groups and layer. They do not duplicate gameplay
components, attachments or children. Extracted curve ribbons disable collision.
The original object, source and hierarchy remain intact.

Reverse/swap/insertion edits are one source undo command. They clear the current
control selection because control indices change meaning; undo restores the
prior matching source and control selection. Splits and extraction select the
first result in object mode. All results use the existing scene save/reopen,
control editing, polygon bake and Play/export paths.

## Geometry and orientation

Splitting inserts the requested knot in homogeneous coordinates until its
multiplicity equals the degree, then partitions the refined controls and full
knot vectors into two clamped sources. Isocurve extraction performs the same
refinement and takes the corresponding rational control strip (or an existing
boundary strip). Neither operation samples or fits a replacement curve.
Geometry is preserved within floating-point arithmetic, not interval-certified.

Reversal reflects the knot axis and reverses the corresponding controls.
Swapping U/V transposes both axes and the net. The geometric locus is unchanged,
but reversing one axis or swapping U/V reverses the oriented surface normal.
UV coordinates follow the new parameterization, so texture orientation changes.
Each split piece regenerates display UVs over its own normalized domain; texture
mapping therefore restarts on each piece rather than preserving cross-split UV
continuity. Material and texture resources are retained.
These are parameterization tools, not normal-only overrides.

Both split bakes are prepared and validated before either object is published.
Invalid parameters, degenerate/float-collapsed display meshes, unsupported axes
and resource-budget violations add no objects or scene-history command. The
existing insertion implementation bounds the **intermediate** refined net to
4096 controls; some valid large sources may therefore refuse a split or interior
isocurve even if its final outputs would be smaller. Existing sample-grid,
weight-conditioning, degree and coordinate limits still apply.

This batch does not add trimming, surface-surface intersection, arbitrary
cutting curves, patch joining/stitching, degree elevation, knot removal,
periodic forms, adaptive tessellation or general C0 crease-normal splitting.

## Validation

`dve_editor_nurbs_topology_tests` compares rational split positions and analytic
first derivatives against the original source for degrees 1..7, U/V surface
splits on nonuniform domains, and already repeated knots. It compares extracted
rational isocurves and their derivatives at boundaries, existing knots and
arbitrary parameters. It also checks transpose/reversal derivatives and
involutions, source codec, valid tessellation, parameter/axis/budget rejection,
native parameter input, retained source/materials/placement, undo/redo,
selection restoration, locked sources and all-or-nothing bake failure.

The PR records the executed local regression results. Native interaction tests
are headless; windowed GPU and artist validation remain separate.
