# Selection masks, front-only boxes, grow / shrink / connected / invert (16 Oct 2026)

Artist worklist ART-015, ART-016 and ART-017.

## Selection masks (ART-015)

Edit > Selection Mask has three checkboxes, all on by default:

- **Selectable: Locked Objects**
- **Selectable: Helpers (cameras, lights, emitters)**: objects with no geometry and no children
- **Selectable: Prefab Instances**

plus **Make Everything Selectable**. A masked-out object is transparent to viewport clicks (the
click reaches whatever is behind it), boxes skip it, and Invert / Select Connected leave it out.
It still hides what is behind it for front-only boxes, and the hierarchy panel still selects it.
The status line says what is being skipped, and so does the label under a selection box.

## Front-only versus through (ART-016)

Object boxes now follow X-Ray Selection like mesh and voxel boxes already did:

- **X-Ray off**: a box takes only objects with a visible part inside it. Window boxes still need
  the whole object inside; crossing boxes need any visible part.
- **X-Ray on** (View > X-Ray Selection): boxes select through, as before.

While dragging, the box is labelled with its rule, for example `Crossing · visible only` or
`Window · X-Ray (through)`, plus any masks in effect.

Visibility is sampled with rays: one at each candidate's centre, then a grid of at most about
2,500 rays over the box. A full-screen box over 400 voxel objects takes about 150 ms on the test
machine (6 ms with X-Ray, which needs no rays).

## Grow, shrink, connected, invert in every domain (ART-017)

Edit > Invert Selection (Ctrl+Shift+I), Grow Selection (`.`), Shrink Selection (`,`) and Select
Connected work in whichever domain is active:

| Domain  | Grow                 | Shrink                               | Connected                    | Invert |
|---------|----------------------|--------------------------------------|------------------------------|--------|
| Objects | add children         | drop children of selected parents    | whole hierarchy              | everything visible and selectable, swapped |
| Voxels  | Voxel > Grow         | Voxel > Shrink                       | every piece touched          | Voxel > Invert |
| Mesh    | Polygon > Grow       | Polygon > Shrink                     | shells (Polygon > Connected) | Polygon > Invert |

The domain-specific menu items are unchanged; the Edit items route to them.

## Tests

`tests/test_editor_selection_filters.cpp`: front-only crossing and window boxes, X-Ray boxes,
box labels, the three masks (click-through, boxes, Invert), menu checks, object grow / shrink /
connected (skipping hidden), voxel and mesh routing, and the new keys.
