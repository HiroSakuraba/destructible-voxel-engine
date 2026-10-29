# Selection Core Closeout — ART-010–013 / ART-015

Date: 2026-10-28. This tranche consolidates existing selection functionality and closes
specific correctness gaps. It does **not** claim that the entire selection/navigation
worklist is complete or that native windowed acceptance has been performed.

## Already present on main before this tranche

- Select tool begins a box over geometry or empty viewport without a transform drag.
- Left-to-right containment and right-to-left crossing, X-Ray selection, object masking,
  visible-only ray filtering, and closed-group aggregation.
- Shift to add/toggle clicks, Ctrl to subtract, Ctrl+Shift to intersect, and
  `EditorWorkspace::apply_selection` set operations.
- Repeated-click depth cycling and Alt-click depth-sorted list with object name,
  type and hierarchy path; object/voxel/polygon selection domains.
- Lasso and paint shapes, saved selection sets, select-by-property, object pick-walk.

## New work in this PR

1. **Projected polygon geometry (ART-011).** Triangle/selection-rectangle crossing
   now uses screen-space separating-axis testing, not the triangle's projected AABB.
   Near/far clipped triangles are processed as convex projected polygons. A face
   spanning the camera near plane no longer relies on projecting hidden vertices.
   Existing voxel-brick/occupied-voxel logic is unchanged. The projected object
   AABB remains only a conservative fast rejection when fully projectable.
2. **Consistent hierarchy operations (ART-012).** Ctrl+Shift on an object row
   intersects, rather than subtracting; Shift-click toggles; Ctrl subtracts. The
   shared operation precedence applies to viewport, polygon, voxel and hierarchy.
   The menu is refreshed immediately after hierarchy changes.
3. **Explicit operation mode (ART-012).** Edit > Selection Operation offers
   Replace, Add, Subtract, Toggle, Intersect. The chosen mode persists for this
   editor session and applies to click, box, lasso, hierarchy and depth-list choice.
   A held Shift/Ctrl modifier overrides the default. Paint remains an additive/
   subtractive stroke, because an intersection for every dab would be misleading.
   Explicit Add no longer accidentally removes already selected click targets;
   only a genuine Shift-click toggles.
4. **Grouped object selection and depth picking (ART-010/013).** Closed-group box
   aggregation keeps containment strict for all visible pieces. A depth-list item
   respects the explicit selection operation instead of unconditionally replacing.

## Automated tests added

`tests/test_editor_selection_filters.cpp`:
- Small right-to-left box in the empty corner of a diagonal triangle's AABB:
  must **not** hit; crossing inside the real triangle must hit.
- Small containment box refuses a partial triangle; whole viewport accepts it.
- Toggle box twice removes then restores an object set.
- Intersect box against disjoint geometry clears the intersection.
- Edit-menu radio state and marquee operation indicator.
- Explicit Add click never deselects a target already in the set.
- Ctrl+Shift hierarchy click intersects; Shift toggles the same row off/on.

Existing `dve_editor_selection_filters_tests`, `dve_editor_selection_shapes_tests`,
`dve_editor_hierarchy_navigation_tests`, `dve_editor_isolate_groups_tests`,
`dve_editor_polygon_tests` and `dve_editor_tests` are regression dependencies.

## Manual/native acceptance still needed

1. In a windowed editor, select boxes that start on an object and on empty space,
   then confirm a transform gizmo is never inadvertently invoked by Select.
2. With a diagonal mesh, draw a crossing box in an empty triangle-bounding-box
   corner; rotate the mesh and repeat near the camera near plane.
3. Test closed/open groups, isolating objects, hidden objects, locked/prefab/helper
   masks, X-Ray versus front-only, and both orthographic and perspective views.
4. Exercise all five operations with click, box, lasso, hierarchy and Alt pick list,
   including empty-space gestures. Paint should remain stroke-add/remove.
5. Confirm viewport picking and hierarchy presentation with DPI scaling, very
   large meshes and heavy occlusion. Geometry-correct selection still has a
   potentially expensive per-triangle slow path for near-plane/partial-bound
   cases; profile before claiming unbounded performance.

Selection-only changes do not dirty the authored document or enter the content
Undo stack. The source was edited through a repository connector; no local
full C++ compile, renderer inspection, or native GUI session was executed here.
GitHub checks and artist acceptance must be reported separately.
