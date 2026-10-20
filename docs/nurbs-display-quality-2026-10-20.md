# Native NURBS display quality and continuous split textures

Builds on [split/isocurve tools](nurbs-split-isoparms-2026-10-20.md). This batch
fixes display shading at potential creases, exposes display density and retains
surface texture coordinates when splitting.

## Artist workflow

1. Select a native NURBS source. Open **Construction and Split/Isocurve Settings**.
2. The fifth numeric field is **Display samples per span (1..64)**. It starts at
   the selected source's current density. Type an integer and press Enter;
   Escape cancels. This edits a pending setting, not the scene object.
3. Choose **Apply Display Density and Crease Normals**. The source and display
   cache update together in one undo command. This also upgrades older sources
   to crease-aware shading. Controls, weights, materials, placement and control
   selection remain unchanged. Applying identical settings adds no history.
4. Split a surface using the existing tools. Both pieces now retain their
   respective texture subranges, so the texture continues across the split.
   Further splits, knot refinement and display-density changes retain them.
5. Use **Reset Surface Texture Ranges to 0..1** to deliberately remap a piece
   over the full texture again. Reset is undoable and preserves materials.

New sources enable crease-aware shading by default. Loaded version-1 sources
retain the old mesh behavior until explicitly upgraded or a new construction
chooses crease-aware output. Higher density remains fixed sampling per nonzero
knot span, not adaptive sampling or a guaranteed error tolerance.

## Crease behavior

At an interior knot whose multiplicity equals the degree, the parameterization
can be only C0 continuous: positions meet but tangent directions may differ.
The old grid shared one vertex and its right-hand normal across that boundary.
The new grid duplicates the boundary vertices and evaluates derivatives from
both sides at the **same parameter**, rather than nudging the sample positions.
Faces use the normal and tangent belonging to their own side. The mesh does not
connect the duplicate boundary rows/columns with zero-area bridge triangles.
Intersecting U/V crease lines receive separate corner normals in all quadrants.

This is conservative parameter-space seam handling, not an automatic geometric
crease detector. A rational circle's repeated construction knots can still have
matching geometric tangents; its duplicate normals therefore agree and the
surface remains visually smooth. Single-span patches keep their existing grid.
Sharp curve guide ribbons break into independently shaded span pieces; they do
not gain a miter, bevel, cap or physical thickness at their corners.

The actual crease-aware vertex count, including duplicated seams and both guide
ribbon rows, is limited to 65,536. Validation counts samples before allocating a
mesh. Upgrading a dense legacy source can therefore refuse until density is
reduced. Failure leaves the previous source, cache and scene history intact.

## Texture source and compatibility

Version 2 of the NURBS source codec stores the crease flag and affine texture
ranges for U and V. Ranges are finite, increasing and bounded to +/-1,000,000.
Split computes the corresponding subrange from the original parameter interval;
repeated splitting preserves that original mapping instead of restarting 0..1.
Both display UV channels use the same mapping, as before. Extracted isocurve
ribbons carry the varying axis's range along their length; they do not reproduce
the original surface's complete two-dimensional texture mapping.

Swapping U/V swaps the stored ranges with their axes. Reversal still reverses
texture orientation with the parameter direction. Loft/sweep use the first
profile's U range and a fresh 0..1 V range; this is not arbitrary per-profile UV
interpolation. Materials and texture resources remain separately authored.

Version-1 sources decode with normalized texture ranges and legacy shared crease
vertices. A captured parent-commit fixture verifies its complete display asset
hash is unchanged. This lets existing saved source/cache pairs validate without
an automatic destructive rebake. Version-2 data round-trips both legacy and new
display settings; older DVE builds do not understand the new source version.
Source-derived UVs are validated against the cache, and crease-aware normals and
tangents are also checked. Bake an independent polygon before custom mesh UV or
normal editing. Vertex colors and material resources remain separate.

## Validation

`dve_editor_nurbs_display_tests` checks a parent-generated legacy mesh hash,
version-1 decoding and version-2 round trips; U/V and intersecting creases;
per-face normals and absent bridge triangles; smooth rational conic seams;
density/source invariance; split and repeated-split UV coordinates;
texture-axis transposition; invalid metadata and duplicate-vertex budgets;
native legacy upgrade, density input, selection preservation, no-op history,
undo/redo, explicit texture reset, cache authority and scene save/reopen.

The PR records the executed regression results. OS-window/GPU artist validation
remains separate from headless controller and geometry checks.
