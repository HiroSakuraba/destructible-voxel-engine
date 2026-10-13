# Saved UV seams and pins

This batch adds scene-backed UV authoring flags to the native editor.
It was reviewed as source only. No builds, tests or CI were run, at the user's
request. The publication commit includes `[skip ci]`.

## Artist controls

Polygon > Mark UV Seam and Clear UV Seam operate on selected geometric edges.
Inside UV Workspace, use Edge mode (2), select edges and press M to mark or
Shift+M to clear. Red edges show seams in the UV pane and on the 3D mesh.
When the UV pane is active, these commands use its channel; outside the pane
they use UV0. UV0 and UV1 have independent seam and pin sets.

P toggles saved pins on selected UV corners. Polygon > Toggle Saved UV Pins
also supports the face selection displayed from the 3D view. Pins are purple
in UV Vertex mode and constrain the corners' current UV coordinates at unwrap.
Move a pinned UV to change its constraint location; its coordinates are saved
as part of the mesh's existing UV data. Pins do not prevent direct UV transforms.
Pin/seam edits are normal undoable commands, not transient previews. Disabling
UV Seams and Pins in the component inspector temporarily disables its constraints
and markers while retaining the record. Editing flags re-enables the record in
the same undo step.

Close and reopen the workspace, or save/reload the editor scene, to retain flags.
Use U to unwrap with the channel's saved seams and pins. Marking or clearing a
seam alone does not move UVs or visible mesh geometry. Packing and island selection
use the saved seam boundaries together with existing UV discontinuities.

Polygon > Reset Saved UV Seams and Pins removes both channels' authoring records
in one undoable command. It is also the recovery path for invalid, duplicate or
stale metadata. Apply/cancel previews before changing or resetting flags.

## Representation and compatibility

A versioned `dve.uv_authoring` component stores the two channels' geometric edge
keys and triangle-corner pin IDs. Existing editor component serialization and
clone/prefab paths carry the metadata. Editor scene export and Play publication
strip this editor-only component; ordinary UV coordinates remain in exported
polygon geometry. There is no DMESH format change.

A noncryptographic layout fingerprint follows the triangle order and geometric
vertex representatives across normal/UV attribute copies. UV attribute splits
retain the original representatives, so these edits keep the metadata valid.
Topology/reindexing changes that alter the fingerprint block authoring flag use;
they do not silently reassign saved IDs. Undo can restore the matching layout,
or Reset can discard the old flags. Automatic topology remapping is not included.

An empty state field is a valid uninitialized inspector component with no flags.
The parser rejects unknown versions, invalid numeric tokens, duplicates, trailing
data, missing edges/corners and mismatched layouts. Records are limited to 4 MiB,
40,000 seams and 60,000 pins per channel. The generic inspector can still store
malformed raw metadata; the UV tools refuse to use it and offer Reset.

## Seam-aware unwrap

The least squares conformal map solver joins corner sectors only across unmarked
edges whose UV endpoints match. It duplicates UV attributes where the cut requires
different coordinates on the two sides, preserving positions, normals, materials
and the other UV channel. This supports seam cuts within a connected face chart,
not only cuts that split the mesh into disconnected face groups.

Charts must still satisfy the existing disk, size, finite-coordinate, convergence
and orientation checks. With explicitly marked seams, a failed disk chart is
reported rather than silently falling back to independent triangles. Without
explicit seams, the existing closed-chart triangle fallback remains.
Marking an arbitrary single edge does not guarantee a valid unwrap chart.
The solver does not certify global absence of overlap.

## Manual acceptance handoff

The following checks have not been executed here:

- Mark/clear a geometric edge, inspect the red UV/3D markers, and Undo/Redo.
- Select UV vertices/corners, toggle pins, move a pinned UV, and unwrap. Confirm
  the pin holds the updated coordinate and remains after Undo/Redo/reopening.
- Save/reload a scene and duplicate or round-trip a prefab containing seams/pins.
  Inspect both UV channels independently.
- Mark a continuous cut suitable for a disk chart, including a cut that does not
  disconnect face adjacency. Unwrap and inspect UV seam separation, visible
  geometry, normals/materials and the other UV channel.
- Try an insufficient seam cut and confirm the tool reports failure while
  retaining the original geometry and metadata.
- Delete/subdivide/reindex faces, confirm stale flags are refused, then Undo the
  topology edit or Reset flags and author a fresh set.
- Confirm Play and scene export omit UV authoring metadata while retaining the
  UV coordinates produced by the edit.
- Confirm pin/seam actions are unavailable during pending UV/hull/polygon previews.

UDIM, UV layout export, saved pane/navigation state, overlap/stretch diagnostics,
texel-density tools and optimal atlas packing remain separate work.
