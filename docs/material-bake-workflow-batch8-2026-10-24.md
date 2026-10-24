# Batch 8: saved bake recipes and material inspection

Batch 8 builds on Batch 7 (#209). Batch 7's updated branch includes the parallax-normal review correction: root UV
normal sampling retains the parallax displacement while sharing the derivative
frame decoder. Batch 8 adds color/normal alignment regressions on UV0 and UV1.

## Artist workflow

1. Select one static polygon object and open Materials, then **Layers**. Choose
   the source material slot, target UV, resolution, samples and padding. A complete
   bake saves a recipe; **Bake / export / apply copy** also assigns its result.
2. Save the scene. The scene owns the editable source and baked mesh snapshots,
   their modeling identities, both UV streams, settings, last successful settings,
   dependency hashes and output location. Reopening does not require the exported
   PNG/bundle directory to remain available.
3. **Restore / edit source** restores the original editable polygon and topology.
   Edit its materials, maps, geometry or UVs using normal editor tools. The saved
   recipe stays attached to the object. **Show baked result** retains those source
   edits and displays the last baked snapshot. Each switch has Undo/Redo.
4. The status control reports source changes, settings/world-pose changes, edited
   assigned baked geometry and unsupported live modifiers. Changes to embedded
   maps, samplers and bindings participate in the source content hash. The open
   material panel checks status at most twice per second. This tracks owned data;
   external source-file changes become dependencies when the normal import or
   reimport workflow updates the owned mesh. Restore the source before reimporting
   its geometry or maps; replacing an assigned baked result is treated as a manual
   result edit. No new filesystem watcher is added.
5. **Rebake saved recipe** uses the editable source, even while the baked result
   is assigned. It exports a fresh no-overwrite bundle and replaces the derived
   material/snapshots in one undo transaction. Undo restores the prior result,
   saved recipe and source edits; Redo restores the new result. Cancellation or
   a changed scene leaves the current association and geometry intact. An already
   published export remains independent of scene application.
6. Changes to saved bake controls are undoable recipe edits. The original slot
   can be changed with **Bake source slot**; choosing a different output UV never
   silently repacks or swaps the source UV streams. World pose is captured anew
   on a successful rebake.

Export-only baking records the initial complete recipe while retaining the
source assignment. Export-only runs on an already associated object leave its
last applied result/recipe baseline intact. Map-only subsets cannot create a
complete material association. Initial Batch 7 scenes have no embedded recipe;
import their bundle's source.dmesh once and bake it to establish the association.

An assigned baked mesh that was manually edited is marked stale. Switching
snapshots asks the artist to Undo those edits or rebake first, so a switch cannot
silently lose them. Rebaking retains the current edited mesh in its undo command.
Live subdivision, NURBS, mirror and active Exact CAD Source guards continue to
protect geometry owned by a different authoring path.

## Inspection

**Inspect source / baked** enables an asynchronous preview of the actual owned
geometry. **Inspect** cycles beauty, base color, world-space normals, roughness,
metallic, emission, opacity and UV coverage/padding. **Compare** cycles source,
baked and a split view: source on the left, baked on the right, white divider.
Both halves use exactly the same camera, current rigid pose, scale and fixed
reference lighting. The camera frames the source geometry; editing its shape can
make the comparison intentionally show a different silhouette.

Base color and LDR emission are displayed as sRGB, normal values as packed world
normals, and roughness/metallic/opacity as linear scalar displays. Beauty uses the
CPU reference renderer and a fixed display curve, providing a consistent material
comparison rather than a lighting bake. Checkerboard marks background and clipped
surfaces. UV coverage is schematic at preview resolution: cyan covered triangles,
gray scaled padding and red overlapping coverage. The baker's continuous overlap,
resolution and gutter validation remains authoritative; subpixel overlaps can be
absent from the schematic.

The preview worker uses immutable snapshots and generation keys covering both
assets, channel/view, pose, scale and UV/padding settings. Superseded results never
replace the requested generation. Inspection checks cancellation between render
passes and during UV/pixel processing, with a 100,000-triangle inspection limit.
The studio preset preview remains available when inspection is disabled.

## Persistence and compatibility

Scenes containing bake recipes use format 14. Scenes without recipes retain their
existing format choice. The transactional scene revision includes fixed-name
source.dmesh, baked.dmesh and a versioned recipe.txt under each associated object's
`.material-bake` directory. Snapshot pairs are limited to 512 MiB on save/load;
recipe metadata is bounded and rejects unsupported versions, malformed/trailing
data, invalid meshes, broken hashes and invalid modeling topology. Snapshot paths
are generated from object IDs rather than supplied by recipe text. Output paths
are provenance labels and are project-relative when the output is inside the open
project.

Cloning a document preserves recipes. Prefab integrity hashes include bake
authoring metadata, and every path that gives an object a new id (duplicate, paste, prefab
instantiation and prefab instance update) remaps both snapshots and their baseline hashes.
Prefab instance updates copy or clear the recipe together with the geometry unless
`polygon.geometry` is overridden. Runtime scene export continues to publish
only the assigned polygon representation; editor recipe snapshots are not runtime
components. Renderer-free editor builds retain recipe persistence/status and
snapshot switching, with explicit inspection/baking availability messages.

Batch 7's static PNG8 limits remain: opaque/masked polygon materials, DVE +Y
mapped derivative normal convention, no HDR/16-bit, projection cage, UDIM,
lighting/AO bake or automatic atlas packing.

## Validation

The workflow regression exercises bake/apply, clone, save/reopen after deleting
the export bundle, source restore and identity preservation, source edits across
restart, source/baked switching, deterministic rebake and undo/redo, recipe edits,
pose and image invalidation, protection of manually edited results, final scene
reload, cancellation and malformed recipe rejection. Inspection fixtures exercise
all eight channels, both individual views, exact split halves, cancellation and
latest-request generation handling. Polygon-rendering tests cover the Batch 7
parallax-normal regression on both UV streams.
