# Edge bevel, chamfer and rounded fillet options

Source review only. No builds, tests or CI were run. Manual acceptance steps below have not
been executed. The publication commit includes `[skip ci]`.

## Workflow

Select crease edges on a closed convex Polygon or Subdivision cage. Choose Bevel Edges:
Options, Chamfer Edges, or Rounded Fillet Edges from its geometry menu. All three open the
same options panel and a wire preview. The source stays unchanged until Apply.

| Control | DVE behavior |
| --- | --- |
| Width | Positive numeric value; fractional values are limited to 1. |
| Fractional / Absolute | Fraction of a conservative safe width, or an absolute cut-plane offset. |
| World / Local space | Absolute world width accounts for nonuniform object scale. Fractional width uses the local cage. |
| Segments | 1..16 facets around each selected crease; higher values resolve rounded profiles. |
| Depth / Profile | 0 is flat; 1 is a circular convex cross-section; intermediate values blend the two. |
| Auto Fit | Caps width using the shortest crease-edge feature and selected dihedral angles. |
| Smoothing Angle | 0..180 degrees; joins output face-normal fans below the selected angular threshold. |
| Chamfer preset | One flat segment, Depth 0 and Smoothing Angle 30. |
| Rounded preset | Four segments, Depth 1 and Smoothing Angle 60. |
| Reset | Width 0.2 fractional, one segment, Depth 1, Auto Fit on and smoothing 30. |

Click a field and type its number. Tab/Shift+Tab cycles fields; arrows adjust the selected
field. F changes offset type, W changes absolute offset space, A toggles Auto Fit, C selects
Chamfer, and R selects Rounded. Click Apply/Cancel or press Enter/Escape. Invalid numeric
input disables Apply through the invalid preview result. Selecting another field or preset
restores a preview from the last valid numeric settings. Narrow panels clip and elide labels.

The selected edges are captured when the preview begins. Source geometry/placement/scale
changes refresh the preview and require another Apply, preventing stale geometry commits.
An invalid preview displays the unchanged source instead of hiding it. Primary viewport input
is owned by the preview; Alt/non-primary camera navigation remains available.

## Maya comparison and boundaries

The control names and options workflow follow Autodesk's documented
[Bevel Options](https://help.autodesk.com/cloudhelp/2026/ENU/Maya-Modeling/files/GUID-A1C5EC72-AD48-4A7D-8577-1823B3832E14.htm)
and [polyBevel attributes](https://help.autodesk.com/cloudhelp/ENU/MayaCRE-Modeling/files/GUID-40E32F44-1EB9-4DC6-8EE4-6A013EEC626F.htm).
This is a Maya-style subset, not algorithm or feature parity.

DVE's absolute Width retains its previous convention: distance from the original edge to the
flat chamfer's bisector plane. The circular profile shares the flat chamfer's face endpoints;
its radius is derived from Width and the edge angle. It is not a directly specified CAD radius.
For an exterior normal angle theta, the derived circle radius is
`Width * cos(theta/2) / sin(theta/2)^2`. Segments sample this cross-section into chords, and
intersections of inward trim half-spaces produce the mesh. One segment gives a flat chamfer
at any Depth. Depth zero uses one coplanar cut even if a larger Segments value is selected;
it does not pretend to add invisible coplanar strip subdivisions.

Fractional width uses `0.49 * shortest noncoplanar cage-edge length * minimum selected
sin(theta/2)` as a conservative cap. Auto Fit uses the same cap for absolute offsets. It is
not Maya's maximum-fit/overlap solver and does not certify preservation of every nearby
feature. Disable Auto Fit for unrestricted absolute offsets; empty/invalid output is refused.

The existing Manifold backend still requires a closed, consistently wound convex solid.
Concave/open edges, negative Depth, Maya's separate miter modes, miter-direction controls,
Chamfer-off semantics, UV assignment modes and persistent editable construction-history
nodes are not implemented. Intersecting bevels use the convex half-space intersection rather
than Maya's Uniform/Patch/Radial corner reconstruction. Results are polygonal mesh fillets,
not exact CAD fillets. Generated UVs remain planar projections; original interpolated
UV0/UV1 and colors are retained on surviving surfaces. Smoothing Angle recomputes output
geometric normal fans while retaining UV/color splits.

## Integration and limits

The new settings overload keeps the float-width API as a flat local-offset compatibility
wrapper. Boolean operations and hull fitting are unchanged. World offset stretches the cage
by its metric scale for clipping and returns positions to the original local cage; rotation and
translation do not change Euclidean width. World mode requires finite positive scale.

Budgets: 100,000 source triangles, 200,000 source attribute vertices, 256 total requested
profile planes, 16 segments per edge and one million output triangle corners. Large selections
are refused before clipping. Material/face provenance and output topology are checked before
returning an applicable result. Geometry history uses the shared ReplacePolygonCommand,
including subdivision face/crease remapping and Undo/Redo selection restoration.

## Manual acceptance handoff (not executed)

- Chamfer one cube edge, then several meeting edges. Compare bevel options and both presets.
- Compare 1/2/4/8/16 segments and Depth 0/0.5/1, including a wedge with a non-right dihedral.
- Enter values through mouse fields and keyboard. Exercise invalid/empty/out-of-range input,
  reset, preset switching, Tab, arrows, Apply and Cancel, including a narrow viewport.
- Compare Fractional/Absolute and Auto Fit on/off. Try a thin convex solid and excessive widths;
  confirm refusal leaves the source unchanged and the source remains visible.
- Use nonuniform object scale to compare world/local width. Inspect returned local normals,
  UV0/UV1, colors, material boundaries and smoothing at 0/30/60/180 degrees.
- Exercise subdivision cage remapping, original sources after Cancel, Undo/Redo and save/reopen.
- Change source geometry or scale during the preview; inspect refresh before the second Apply.
- Confirm open/concave/non-manifold meshes, triangle diagonals and excessive budgets are refused.
