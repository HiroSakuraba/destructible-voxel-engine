# Caustics: fixes from review (22 Oct 2026)

Codex reviewed #180 (photon reference) and #181 (caustic map). All seven findings were real; each
is fixed and tested in `tests/test_caustics_review.cpp`.

## Light paths (`optical_scene.cpp`, `polygon_bvh`)

- **A glass mesh with several glass materials.** Leaving a polygon medium required the exit
  triangle to have the same material as the entry one. A closed mesh with another glass material
  on its exit face was re-entered instead: the extra medium bent the light with the wrong index
  and tinted it in air. Now any refractive face of the same mesh is its exit; an opaque face of
  it is still a receiver.
  - Test: a box with a second glass material on its bottom face transmits exactly
    (1 − F)² = 0.9216, with two refractions.
- **Normals from the right triangle.** `PolygonBvhHit::triangleIndex` counts triangles in submesh
  order, but normals were looked up at `triangleIndex × 3` in the index buffer. That is wrong when
  submeshes start past 0 or are out of order. Hits now carry `firstIndex`, the triangle's
  index-buffer offset; `triangleIndex` keeps its meaning for picking and physics.
  - Test: a ball whose lower half comes first in the buffer has exact sphere normals.

## Photons (`caustic_photons.cpp`)

- **The sun's direction.** `trace_caustic_photons` took its direction from the region and ignored
  `SunLight::towardSun`. Photons now follow the sun. A region built for another direction is
  refused: `regionMismatch` is set and nothing is traced, rather than silently tracing the old
  direction.
- **Sparse objects.** Finding the glass in a voxel object walked its whole bounding box. Two glass
  cells a million cells apart meant on the order of 10^17 lookups. It now walks the occupied bricks.

## In the renderer (`voxel_reference_renderer.cpp`, `caustic_map.cpp`)

- **Caustic light follows the shadow.** Caustic light replaces what the glass's shadow takes
  away, so it now scales with `shadowStrength`. With shadows off it is not added, because all the
  direct sun is already there. Before, a glass slab at the default strength 0.85 received
  15 % direct sun plus 100 % of the transmitted light.
  - Test: under a glass slab the floor gets exactly (1 − s) + s × 0.9216 for s = 1, 0.85 and
    0.5, and plain sun with shadows off.
- **Masked polygons cast shadows only where they are drawn.** Sun rays through Masked materials
  (foliage, fences) now evaluate the surface's opacity at the hit, the same way the rasterizer's
  alpha test does: material × vertex colour × base-colour texture × opacity texture × tint. They
  pass through where the opacity is under the cutoff.
  - New public helper: `polygon_hit_opacity`.
  - Test: a quad with a hole on one half shades only the other half.
- **Caustic light stays where it landed.** `CausticMap::irradiance` takes the receiving object.
  Light that landed on another object counts only across a seam in the same plane, where the
  sun-size blur really spreads it. A surface just behind or below the one that caught the light
  gets none; it used to get the full 0.92.
  - The map also no longer drops grid triangles that straddle such a seam (two floors side by
    side). That used to leave a dark line along the join.
  - The renderer passes each voxel instance's `objectId`.

Not changed: translucent polygons that are not glass (alpha-blended, no transmission) still block
the sun fully on voxels.
