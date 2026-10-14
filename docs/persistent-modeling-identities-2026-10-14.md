# Persistent identities for the supported modeling workflows

This implements the first portion of worklist ART-132 and ART-146 on top of merged #126. It establishes an editor-owned identity record and connects loop cut/slide, triangle-pair edge dissolve, supported boundary-vertex dissolve, protected merge by distance, unused-vertex cleanup, and commands that preserve render connectivity. It does not complete identity propagation for every modeling operation.

## Identity model

`EditorObject::modelingTopology` stores object-scoped 64-bit IDs independently of array offsets and positions:

- Each attribute-array entry has a separate attribute identity and refers to a geometric vertex identity. UV/normal seam copies can share geometry without sharing attribute identity.
- Each render-index corner has its own identity. Each render triangle also has an identity.
- Authored control faces have distinct identities and memberships expressed using triangle IDs.
- Geometric render edges have IDs, with a separate set marking selectable authored edges. A dissolved cage diagonal cannot be recalled as a selectable cage edge simply because the renderer still triangulates across it.

The cooked polygon file and rendering buffers are unchanged. The topology record is editor metadata in the scene manifest. `modelingNextId` is a separate allocation high-water mark. Undo restores the original identity record but does not reduce this mark. Save/reopen preserves it. Cancelled supported previews can reserve IDs, leaving harmless gaps rather than making those IDs available to subsequent edits.

`ModelingChange` records each old identity's explicit descendants (an empty set retires it), plus newly created identities. Unchanged elements preserve IDs. Many-to-one merges choose the smallest participating ID. One-to-many splits allocate child IDs. Each UV/normal attribute record remains distinct during geometric welding. New unrelated edges do not obtain crease values by proximity.

## Supported edit integration

Loop cut emits explicit output geometric groups, attribute survivors, render-triangle descendants, control-face descendants, geometric-edge descendants and corner descendants. Cleanup emits the same relevant mappings. Parent identity lookup uses those mappings, not a search for an old position matching a new position. Per-preview allocation starts from one captured floor, so slide adjustments and Apply retain consistent provisional IDs.

The existing explicit UV seam/pin and crease remapping is retained. Split crease edges produce child edge IDs and inherit the parent strength. Conflicting merged strengths and flags without supported descendants continue to refuse the edit. Seams and pins are still encoded using current arrays in the existing UV component; the persisted identity record and explicit edit mappings accompany those arrays. This is not a replacement of every authoring component's storage format.

`ReplacePolygonCommand` stores before/after identity records alongside geometry and subdivision settings. Unchanged render connectivity preserves identities through motion, UV edits and normal recalculation. Saved-face regrouping maps triangle membership explicitly, including logical face split/merge. Operations that change arrays without providing a prepared identity record refuse on an identity-bearing object, without silently erasing its references. Legacy objects without IDs retain their existing behavior until migrated.

Native selection history stores persistent vertex, authored-edge and logical-face IDs. Undo/Redo resolves them against the current restored mesh; missing IDs do not fall back to potentially reused array indices. Active selection is not newly persisted across editor restarts, and named selection sets remain a separate worklist item.

## Persistence, migration and prefab behavior

Scene format 11 appends bounded identity records and allocation floors. Formats 1–10 remain readable, and scenes without identity metadata retain the earlier format choice. IDs are initialized lazily before a supported identity-aware edit. Unambiguous legacy meshes use their current geometric grouping as the migration baseline. This initial compatibility migration cannot reconstruct missing authoring intent.

Duplicate coincident faces, coincident disconnected vertex fans and ambiguous/nonmanifold legacy grouping refuse automatic migration. The core API accepts an explicit geometric partition, so coincident independent vertices can be represented and persisted separately. Native picking and the current position-based geometry algorithms are not yet fully identity-aware; supported topology edits refuse such partitions rather than silently welding them. Explicit identity-aware picking and algorithm traversal are follow-up work.

Object/document clones preserve local IDs and allocation floors. New objects provide separate namespaces. Prefab template save/load, instantiation and unmodified geometry refresh carry the record; prefab content hashes include identity memberships while excluding allocator reservations. Geometry overrides retain the instance's own record.

This first batch does not introduce source-to-instance ID aliases for locally generated prefab topology. The native topology previews and saved-face changes therefore require editing the prefab source or detaching the instance. Existing identity-preserving motion and settings remain supported. General local prefab topology edits need a separate namespace/alias policy before enabling them.

## Validation and limits

Identity records are bounded to 100,000 attributes/geometric references, 150,000 corners/edges and 50,000 triangles/control faces. Validation checks IDs across element types, geometric-group coherence, render connectivity, authored-edge membership, logical-face membership and allocation counters. Malformed/oversized persisted records fail before unbounded allocation.

Executed locally:

- Identity record serialization, explicit coincident partitions, movement and reordered array mappings.
- Logical-face and authored-edge selection resolution; retired IDs do not select unrelated elements.
- Cut/slide IDs, inherited crease strengths, orphan cleanup, boundary dissolve and geometric merge lineage.
- Real `ReplacePolygonCommand` and `SetSubdivisionSettingsCommand` execute/Undo/Redo, monotonic allocation across a new history branch, and unsupported-edit refusal without mutation.
- Real scene format 10 compatibility, format 11 save/reopen with a higher retired-ID floor, object/document cloning, prefab capture/load/instantiate/refresh and allocator-independent prefab hashes.
- Existing loop/ring, cut/slide, dissolve and cleanup core regressions.

Changed native sources and native integration tests were syntax-checked. Full native controller runtime and visual interaction were not executed locally. Native tests registered as `dve_editor_modeling_topology_tests` cover preview/Apply ID equality, Cancel, selection/history and persistence after branching Undo. They also run the command and persistence checks above.

## Remaining work

Full extrusion, inset, bevel, bridge, Slice and Boolean lineage; identity-aware picking/traversal for coincident independent geometry; local prefab topology aliases; named component selection sets; generalized vertex dissolve and authored n-gons. These are not claimed complete by this PR.
