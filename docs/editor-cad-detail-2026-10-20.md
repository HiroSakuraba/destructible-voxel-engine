# CAD Detail and mesh accuracy (20 Oct 2026)

Exact CAD, first batch, part 1. Builds on the pivot editing branch.

CAD objects (imported with Polygon > Import Exact CAD, or made by an exact Boolean) keep their
exact B-rep next to the triangles. Until now the triangles were always cut at brepkernel's
default accuracy, 1/500 of the part's size: up to 0.2 mm off on a 100 mm part, 2 mm on a 1 m
part. Nothing showed the number and nothing could change it.

## Polygon > CAD Detail...

A list of accuracies, as the largest gap allowed between the triangles and the true surface, in
world millimetres:

| Entry      | Accuracy |
|------------|----------|
| Coarse     | ±1 mm |
| Medium     | ±0.1 mm |
| Fine       | ±0.02 mm |
| Very fine  | ±0.005 mm |
| Default    | 1/500 of the part's size (brepkernel's default) |
| Custom...  | type it: plain numbers are mm; `20um`, `0.1mm`, `0.002in` also work |

The selected CAD objects are re-meshed from their exact source by brepkernel in the background
(`brepkernel mesh --deflection`), then applied as one undo step. The exact source does not
change. The object's materials are kept; UV seams and pins (which belong to the old triangles)
are removed; the mesh gets new element identities numbered above its old ones, so selection sets
on it show its old members as missing rather than picking wrong elements. A stretched object is
handled: the accuracy is divided by its largest axis scale so the world gap is what you asked for.

## Readout

The inspector line under the object details now reads, for CAD objects,
`Exact CAD: mesh within ±0.1 mm` (world, including the object's scale), or
`Exact CAD: default mesh detail (≈ 1/500 of its size)` for objects meshed before the accuracy
was recorded. Import and exact Booleans now record the accuracy they used.

## Stored data

The `dve.exact_brep` component gains two optional properties: `deflection` (metres, in the
mesh-local frame) and `angular` (radians). Older scenes load unchanged.

## Code

- `remesh_exact_cad` and `exact_mesh_world_error` in `editor_exact_cad.hpp`.
- `src/editor_native_cad_detail.cpp`: the command, list, field and job.
- `src/editor_native_long_job.hpp`: the background job record, moved out of `editor_native.cpp`
  so feature files can start jobs.

## Tests

`tests/test_editor_cad_detail.cpp` with a stand-in brepkernel (`tests/exact_cad_test_support.hpp`):
argv and report, refusals of missing and outside sources, the stored and world accuracy, the
native list, presets, typed mm and µm, stretched objects, one undo step, materials, locked and
plain objects. With the real brepkernel: a 12 mm cylinder at the default (±0.05 mm, 140
triangles) and at 0.01 mm (304 triangles), side vertices on the exact radius.
