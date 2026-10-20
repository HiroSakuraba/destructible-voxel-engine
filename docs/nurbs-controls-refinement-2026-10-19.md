# NURBS controls, shared gizmos and knot refinement

Builds on the native untrimmed source workflow from PR #148.

## Control selection

While NURBS control editing is active:
- Click a control to replace the selection. Standard Shift/Ctrl modifiers apply.
- Drag a box to select projected controls. Selection remains explicitly X-ray.
- NURBS menu: select all/none/invert, or the U row / V column through the active
  control. Edit menu Grow/Shrink/Connected works on control-net neighbors.
- Ctrl+A selects controls rather than all scene objects.
- Numeric coordinate/weight fields edit the active control only. Gizmos act on
  the full selected set. Opening a numeric field with no selection selects the
  active control.
- Finish Control Editing returns to whole-object behavior. Object clipboard,
  delete and saved-set commands are refused in control mode; this batch does
  not add a control clipboard or persistent named control sets.

## Shared transform workflow

Use existing Move, Rotate and Scale tools on selected controls, with existing
axis handles, numeric values, snapping and pivots. Held V snaps their pivot to
existing scene targets. Object pose/scale and unselected controls stay unchanged.
Transforms preserve rational weights and work through object rotation and
nonuniform scale.

Preview updates source and derived mesh together. Release/Enter commits one
source command. Escape or focus loss restores the captured source and mesh.
No-op gestures add no history. Invalid previews cancel instead of committing
the last valid intermediate result. If another source update replaces the
preview, cancellation preserves that external source rather than overwriting it.

Selection/history snapshots carry the selected controls, active control,
object and source stamp. Undo/redo restores the corresponding selection only
against its matching source. This is transient history state, not a new
persistent component-identity or named-set format.

## Shape-preserving refinement

Insert U Knot at Domain Midpoint and Insert V Knot at Domain Midpoint refine
the full tensor-product direction. Curves have no V operation. The core API
insert_nurbs_knot accepts an arbitrary interior parameter.

Refinement uses homogeneous rational knot insertion, retaining degree and the
underlying shape within floating-point tolerance. Existing multiplicity cannot
exceed the degree. Endpoints, invalid parameters and control/sample budget
overflow refuse atomically. Refinement can increase display mesh density; the
source shape is preserved, not the old tessellation connectivity.

The control net changes, so insertion explicitly clears selection. Undo restores
both the former net and selection; redo restores the refined net with an empty
selection. Controls do not silently inherit unrelated old array-index selections.

This does not add knot removal, arbitrary knot relocation, adaptive tessellation,
trimmed patches, stitching, periodic forms or general crease-normal splitting.

## Validation

dve_editor_nurbs_tools_tests compares positions and analytic derivatives before
and after rational curve and U/V surface insertion, including repeated knots,
multiplicity limits and codec round trips. Native-controller tests exercise
multi-selection, boxes, modifiers, Ctrl+A, row/column selection, shared
move/rotate/scale, numeric input, snapping, no-op, Escape/focus cancellation,
nonuniform/rotated placement, source-change refusal, insertion undo/redo and
protection against whole-object deletion in control mode.

The PR records the executed regression suites. Windowed GPU artist validation
remains separate from these headless controller tests.
