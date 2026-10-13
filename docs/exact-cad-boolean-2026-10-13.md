# Exact CAD import and exact Booleans (brepkernel)

DVE edits polygons. CAD parts are B-reps: exact surfaces (planes, cylinders,
NURBS) with exact edges. This adds a bridge to the
[brepkernel](https://github.com/HiroSakuraba/robust-brep-booleans) command line tool so a polygon object can keep
its exact B-rep next to its triangles, and so two such objects can be combined
with a certified exact Boolean and re-meshed from the exact result.

brepkernel either certifies a result or refuses it. A refusal is reported with
its reason and the uncertified second opinions; nothing in the scene changes.

## What you need

brepkernel with the `brepkernel` command line (the `feature/dve-exact-boolean`
branch or a release that includes it; `pip install .` from a checkout). The editor finds it in this order:

1. `DVE_BREPKERNEL`, split on spaces, for example
   `DVE_BREPKERNEL="/opt/venv/bin/python -m brepkernel"`
2. `brepkernel` on `PATH`
3. `python3 -m brepkernel` (`python -m brepkernel` on Windows)

It runs as a separate process (argv, no shell) with a 300 second timeout. The
editor reads only its JSON report (schema `brepkernel.cli/1`).

## Import Exact CAD

Select a `.step`, `.stp`, `.brep` or `.usda` (OpenUSD B-rep from usd-brep) file
in the Assets panel, then Polygon > Import Exact CAD (STEP, BREP, USD). It runs
as a background job.

- brepkernel converts the part to meters, +Y up. STEP is read as millimeters,
  +Z up; BREP and USD as meters, +Y up.
- The exact B-rep is stored in the project as
  `sources/exact/<name>-<first 12 hex of sha256>.brep`. The original file is
  not changed.
- The mesh comes from brepkernel's glTF exporter: hard edges between CAD faces,
  exact surface normals, watertight, cooked by DVE's own GLB reader with normal
  generation off.
- The new object gets a `dve.exact_brep` component ("Exact CAD Source") with
  `source` (project path), `sha256` and `origin`. The component is editor-only:
  scene export and Play strip it.

## Exact Booleans

Select exactly two polygon objects that both have an exact CAD source. The
active object is A. Polygon > Exact Boolean Union / A minus B / B minus A /
Intersection (CAD), grouped as "Exact CAD Booleans" next to the Manifold
"Mesh Booleans".

- B is placed into A's mesh frame with both world poses and both per-axis
  voxel scales, computed in double precision and passed to brepkernel as a 3x4
  matrix. Rigid, mirrored, uniform-scale and general affine placements work.
- The job runs in the background. The result is applied only if neither
  operand changed while it ran.
- On success, one undo step adds a new object "A (Exact Boolean)" with A's
  parent, pose, voxel scale and flags, the re-meshed exact result, and its own
  `dve.exact_brep` source. Both operands stay as they are. The status line
  gives the certified exact volume.
- A certified empty result (for example, disjoint parts intersected) is
  reported and nothing is added.
- On a refusal the status line shows brepkernel's reason, for example
  `PatchClassificationInconsistent`, plus one line of uncertified second
  opinions (OCCT and SMLib volumes and their triage, such as `contested` or
  `suspected_over_refusal`).
- Fallback: when the build has Manifold mesh authoring, a refusal also opens
  the normal Manifold Boolean preview for the same operands. Enter applies it
  as a mesh-only result (no exact source), Escape closes it. Nothing is applied
  automatically.

## Limits

- The result uses A's material palette and the whole result is in A's first
  material. Per-face material provenance from the operands is not carried over.
- Detail level is set by brepkernel's default deflection (bounding box
  diagonal / 500) and 0.35 rad angular deflection. There is no LOD chain yet.
- Edits to the triangles (topology tools, UV changes) do not touch the exact
  source. A later exact Boolean starts from the stored B-rep, not from the
  edited mesh.
- The Windows process runner (CreateProcessW with a log file) compiles but has
  not been run on Windows yet. Linux and macOS use fork/execvp with a process
  group, SIGTERM then SIGKILL on timeout.

## Tests

`dve_editor_exact_cad_tests` always checks the placement against
`voxel_to_world`/`world_to_voxel` (rotations, voxel scales, a parent), the
component's save/load round trip and its removal from runtime export, and the
import and Boolean flows against a fake brepkernel (argv, reports, refusal with
advisory, errors, timeouts, empty results). When brepkernel can run it also
does a real import, a certified difference whose mesh volume matches the exact
one, a real refusal, and the native editor commands end to end (import, exact
Boolean, one-step undo, refusal with the Manifold preview). Set
`DVE_REQUIRE_BREPKERNEL=1` to fail instead of skipping when brepkernel is
missing.
