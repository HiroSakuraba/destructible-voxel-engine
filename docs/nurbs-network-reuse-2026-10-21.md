# NURBS network placement and reuse

Select the complete connected seam network, or its containing hierarchy group, to move, rotate or scale it with the ordinary object tools. Bulk transform/scale/reparent commands preflight every linked member and all proposed placements before mutation. Networks retain a common parent, pose and axis scale; partial selections and divergent per-patch targets refuse atomically. Grouping/ungrouping and normal group placement retain unattached child relationships. Socket attachment and independent patch placement remain unsupported.

Normal Duplicate, object Copy/Cut/Paste and prefab capture require complete seam components. Every copied endpoint gets a fresh mapped ID. Source geometry and approximation policies are retained; display object IDs and canonical corner anchors are updated for the new identity ordering. Copies are added/undone as one validated transaction, preserving original objects and relationships. Copies of linked prefab geometry become owned objects; asset instantiation retains prefab identity. Duplicate Special and mirror-copy retain their existing single-object paths and remain unavailable for linked patches.

Prefab templates and instances store reciprocal seams in their own ID domains. Capture, instantiation and source updates remap every relationship, canonicalize junctions and validate the complete result. A network may consist of owned objects or members of one prefab instance; relationships cannot cross ownership/instance boundaries. Source editing still requires detaching the instance. Instance placement is allowed only when the complete network remains in a common frame. Existing prefab overrides that conflict with network consistency cause the staged source update to refuse.

The geometry source, C0/C1 driving policy, graph limits and acyclic constraint requirement are unchanged. Placement edits preserve local mesh caches; remapping regenerates constrained display caches without refitting source geometry. Group/object transforms use the editor's existing local-axis scaling semantics.

## Validation

All 21 selected headless CTest suites passed. Dedicated cases cover whole-network move/rotate/nonuniform scale and grouping, partial/divergent target refusal, arbitrary endpoint ID mappings, atomic copy undo/redo, native object gizmos and group scale, clipboard/duplicate, separate prefab instances, source-update remapping, read-only prefab geometry and scene round trips. Existing geometry/network/history, generic editor, scene export and menu suites remain selected checks. Windowed/GPU artist acceptance remains outstanding.

A focused AddressSanitizer run passed with the reuse test and reuse, seam, prefab and generic editor-command sources instrumented. Other engine archives, including the native UI translation unit, were uninstrumented; leak detection was disabled.
