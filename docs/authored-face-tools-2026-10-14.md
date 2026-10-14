# Face tools after persistent topology identities

This batch extends merged #127's identity-aware workflows to polygon/subdivision extrusion, inset, face bevel, planar UV projection and face-normal reversal. It does not add concave/open-edge bevel, bridge, Slice or Boolean lineage.

## Supported operations

Extrude retains the selected cap triangles and appends a strip of side quads. Inset and face bevel retain their existing connected, coplanar convex-patch restriction. Subdivision selections must include whole saved control faces. Each side quad is recorded explicitly; output does not depend on a later triangle-pairing guess. Input/output must be consistently wound manifold triangle surfaces, and degenerate or inverted cap output refuses Apply. Zero extrusion height or zero inset produces no valid side strip and is refused. Global self-intersection testing is not added.

The face cap keeps its logical face, triangle and render-corner identities. Original base geometric/attribute entries retain their IDs. Copied cap entries have new geometric and attribute IDs, with explicit parent-copy provenance. UV0, UV1 and colors are copied independently; normals/tangents follow the existing face-tool recomputation policy. Normal seam copies remain separate attribute entries.

A surviving base edge keeps its existing ID even when the operation also derives a cap edge from it. Both base and cap inherit source crease strengths and saved UV seams where those edges exist. New connecting edges default smooth. UV pins follow the original face corner onto the cap, rather than being duplicated onto new wall corners. Different UV channels and disabled authoring components retain their state. Side walls now inherit the material of their own adjacent selected face, rather than the first selected face in the whole patch.

Planar UV projection reports attribute copies when selected face corners share entries with unselected faces. The copy gets a fresh attribute ID but retains the geometric vertex ID. Render-corner, triangle and logical-face identities remain attached. The untouched UV channel and saved flags survive. Legacy UV-only edits do not force an ID-less mesh to migrate; projection on an already identified mesh preserves its IDs.

Normal reversal emits a corner permutation so pinned corners follow their positions in the reversed triangle. Whole-shell reversal is supported. Partial reversal that would leave inconsistent winding is refused. The native command applies immediately as one history entry, matching its existing interaction.

## Native preview and history

Extrude, Inset, face Bevel and Planar UV use the captured-source preview with numeric controls, Apply/Cancel and one Undo/Redo transaction for geometry, subdivision records, persistent IDs and saved UV flags. Apply recomputes mappings and rechecks flags changed during the preview. Initial invalid geometry refuses before leaving a dead preview open. Arrow adjustments now start from the last valid typed value.

The source/detach restriction for local prefab topology changes remains. Native position-based tools still refuse explicit coincident-independent partitions. General array-changing operations without lineage remain protected by #127's command refusal. Attribute-only projection/reversal preserve legacy workflow behavior without activating identity metadata unnecessarily.

## Reserved-ID persistence fix

After Undo removes a legacy mesh's last live topology record, the allocation high-water mark must still survive saving. Otherwise reopening could reuse an ID previously issued by a committed edit. Format 12 stores a bounded `modeling_floors` section for ID-less retired namespaces, without recreating a live topology record. Formats 1–11 remain readable. Existing live identity records continue to use format 11 when no retired-only namespace exists; untouched legacy scenes retain their earlier format choice.

Cancel and Undo still remove preview-time migration from originally ID-less meshes. Re-migration uses the persisted allocation floor and allocates fresh IDs above the retired range.

## Validation

Executed locally in the focused regression executable:

- Extrude, inset and face-bevel cap/side output, stable cap/base IDs, explicit geometry/attribute copies, inherited creases and independent seam channels/pins.
- Preview-value-independent ID allocation, whole-control-face restrictions and degenerate-value refusal.
- Per-boundary material inheritance for a multi-material patch.
- Partial-face UV projection with detached attribute entries and retained geometric/corner IDs.
- Whole-shell normal reversal with permuted stable corners and pinned-corner remapping.
- Real compound command execute/Undo/Redo restoring prepared IDs and UV flags.
- Scene format 12 save/reopen of an ID-less allocation floor, followed by fresh migration.
- Previous identity/command/scene/prefab tests and loop/ring/cut/dissolve/cleanup core regressions.

Native sources and integration tests were syntax-checked. Controller tests cover face previews, Cancel, Apply ID equality, Undo/Redo and saved flags; full native runtime and visual acceptance were not executed locally.

Manual acceptance: insert a loop, select one saved quad and try Extrude, Inset and face Bevel. Mark an edge as creased and as a seam in both channels; inspect base/cap inheritance, smooth vertical edges and a checker texture. Type a value, then use an arrow to adjust from that value. Test Cancel and Undo/Redo. Project only one triangle's UVs while leaving its neighbour's attributes untouched. Reverse a closed shell and inspect pins. Finally Undo every edit on an originally ID-less mesh, save/reopen and make a fresh edit; retired IDs must not be reused.
