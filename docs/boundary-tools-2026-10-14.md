# Identity-aware Bridge and Cap

This batch builds on #128's operation-local lineage. It enables Bridge Boundary Loops and Cap Boundary Loop on both polygon meshes and subdivision cages carrying persistent modeling identities. It follows the artist HTML worklist's stable vertex/edge/face identity requirement; authored n-gons remain separate work.

## Artist workflow

Select complete open boundary edges in polygon or subdivision edge mode. Bridge requires two loops with equal vertex counts. It reverses their correspondence for consistent winding and chooses the cyclic offset with the least total squared connector length. Each side must be a convex planar quad. There are no twist, offset or segment controls in this batch.

Cap fills one simple planar contour, including a concave contour, using ear clipping. Its new saved control faces are triangles. Multiple contours, nested holes, nonplanar contours, self-intersections and self-touching contours are refused. Each loop is limited to 512 edges; geometry remains within the identity system's 100,000 attribute / 50,000 render-triangle budget.

Both tools open the shared Apply/Cancel preview. Enter applies one compound history entry; Escape restores the exact source mesh, subdivision settings and component selection without adding history. Numeric keys have no effect. Apply revalidates saved UV flags; a corrected flag record can be retried with Enter without reopening the preview. The UV workspace closes when a boundary preview opens, and mesh workspace keyboard handling yields to an active polygon preview.

## Provenance and preservation

Existing vertices, attribute entries, render triangles and corners keep their indices and identities. Original attributes, including normals, tangents, color and both UV channels, remain unchanged. Existing logical faces and boundary edges retain their identities. Bridge adds explicit saved quads; Cap adds explicit saved triangles.

New wall/cap attributes copy source boundary attributes and receive fresh attribute IDs while retaining their source geometric vertex ID. New triangles, corners, logical faces and connecting edges receive fresh IDs. Operation-local mappings drive allocation, selection and metadata preservation; repeated recomputation from the same snapshot uses the same provisional IDs.

Existing crease strengths and both channels of saved UV seams and pins remain attached to their original elements. New connecting edges start smooth and new corners start unpinned. Original material assignments stay intact. Bridge sides inherit the first loop's corresponding boundary material; corresponding materials on the other rim must match. Cap requires one compatible material around the contour. Identical definitions and bindings in distinct joined palette slots count as compatible; genuine conflicts are refused. New UV values are boundary copies, not an unwrap or texel-density solution.

Cancel and Undo remove lazy migration from originally ID-less objects. Allocation floors remain reserved so future edits cannot reuse retired IDs. Local prefab topology still requires editing the source or detaching the instance, and coincident independent geometric partitions remain refused by native position-based tools. The tools validate manifold connectivity and local face geometry; they do not perform a global surface self-intersection test.

## Validation

Executed the focused persistent-modeling regression executable against the real polygon, subdivision, identity, UV, command, document and prefab sources. It covers Bridge/Cap with and without cage metadata; preserved source attributes and all identity domains; fresh attribute copies; explicit quad/triangle control faces; subdivision evaluation; both seam/pin channels; crease retention; provisional-ID stability; simple concave caps; duplicate palette compatibility; and incomplete, unequal, nonplanar, intersecting, folded, conflicting-material and oversized contour refusals. Real compound Apply/Undo/Redo tests cover identified and ID-less source objects and restore exact mesh hashes, records and allocation floors. Previous identity/command/scene/prefab and cage cut/dissolve/cleanup regressions also passed.

Native sources and integration tests were syntax-checked. Registered controller coverage exercises polygon/cage menus, legacy/identified Cancel, Enter Apply, ignored numeric keys, selection restoration, exact Undo/Redo and stale-UV Apply/retry. The full native controller suite and visual acceptance were not executed locally.

Manual acceptance: bridge two open rims, then cap one end of an open tube. Repeat on subdivision cages. Mark rim creases and seams in both UV channels, pin existing corners and use a checker texture. Confirm original shading/UVs, smooth new connecting edges, new-face selection, Cancel and Undo/Redo. Try a concave cap, incomplete contour and conflicting material assignment; inspect the refusal reason. Save/reopen after Undo on an originally ID-less mesh and verify another edit allocates fresh identities.
