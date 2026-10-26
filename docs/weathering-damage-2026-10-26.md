# Weathering, damage marks and voxel decals (26 Oct 2026)

Stacked on `feature/rule-masks`. This is the last of the texturing branches. Surfaces can now
show age (snow, moss, rust, dust, wet patches, worn edges) and the aftermath of destruction
(soot, cracks, freshly broken stone). Decals now work on voxels as well as meshes.

## Weathering recipes

`dve/weathering.hpp` packages the previous branch's rule-driven layers as ready-made recipes.
`make_weathering_layer(kind, amount, up, seed)` returns a source material and a layer.
`add_weathering(materials, target, kind, amount, …)` appends both to a material table, and
refuses (changing nothing) when the target already has four layers.

`amount` is the stage, from 0 (barely started) to 1 (fully weathered). It widens the rule bands,
so the same material can age in place.

| kind | material | where | what `amount` does |
| --- | --- | --- | --- |
| Snow | white, rough | what faces up; hollows first; ragged edges | slope band opens from cos 0.95 to cos 0.35 |
| Moss | dark green, soft | occluded places, not undersides; low parts first | occlusion band opens from 0.35 to 0.95 |
| Rust | orange-brown, rough, not metallic | along exposed edges and in pits | edge width grows from 0.5 cm to 8 cm |
| Dust | pale, matte | near-flat surfaces, collecting low | slope band opens and weight grows |
| Wetness | ×0.55 colour, ×0.15 roughness (a multiply layer) | low parts of flat surfaces (puddles) | weight grows from 0.3 to 1 |
| EdgeWear | lighter, rough | exposed edges, broken up | edge width grows from 2 mm to 3 cm |

## Damage marks

`dve/surface_marks.hpp` adds the marks. A `SurfaceMark` is a sphere in its object's own space,
in metres, so it moves with the object. It shades three things:

- **Exposure.** The freshly broken surface inside the crater is lighter, duller and less
  metallic: the material's untouched interior.
- **Scorch.** Soot covers the crater and spreads past the rim to 1.8 × r. At full strength the
  colour drops to 7 %; the surface also roughens, loses metal and gets a ragged edge.
- **Cracks.** Thin dark fracture lines run through the ring from r to 2.2 × r. They are the edges
  of 3D Voronoi cells about 0.35 r apart and fade outward. When a crack is thinner than a pixel it
  fades instead of aliasing.

The marks only change shading; the crater's shape is the voxels'. Overlapping marks keep the
strongest of each effect.

`SurfaceMarkList` keeps the newest marks up to a limit (256 by default) and makes craters with
the settings' strengths.

**In the player.** `PlayerApp` keeps a list per session and listens to the world's damage events.
Every carve leaves a mark at the crater, converted into the object's space. The view passes the
marks to the CPU renderer, and `PlayerApp::surface_marks()` exposes the list for settings and
clearing.

Marks are cosmetic: they are not saved. Fragments that break off don't take the marks with them.

## Decals on voxels

`ReferenceVoxelRenderer::decals` takes main's `MaterialDecal`s (box projections with feathered
edges, normal fade, priorities and channels) and composites them on voxel surfaces, after
layers and marks.

`PlayerRenderView::decals` is in world metres. The CPU player converts it into the renderers'
units and passes it to both the voxel and the polygon renderer.

**Order on a voxel hit:** maps and parallax → surface pattern → layers (with rules) → damage
marks → decals.

## Checks

`dve_weathering_damage_tests`:

**Marks**
- **Crater wall.** It is exposed (0.6) and scorched (0.85), with no cracks inside.
- **Soot.** Its mean falls ring by ring from the rim to the reach, and nothing is marked beyond
  it.
- **Ownership.** Another object's surfaces are untouched.
- **Cracks.** They cover 7.8 % of the ring and vanish at a footprint of 0.5 m.
- **Overlap.** Overlapping marks keep the strongest soot.
- **Shading.** Full soot gives exactly 7 % colour and 0.95 roughness; exposure lightens and
  dulls.
- **The list.** It keeps the newest 3 of 5 and ignores invalid or disabled additions.

**World events.** On a rotated `GameWorld` wall, `damage_sphere` leaves exactly one mark, at the
crater's object-space centre, within 1e-4.

**Rendered**
- **Darkening.** The crater's soot cuts the floor's sun light to 0.251 at its centre and leaves
  the far corner unchanged.
- **Moving with the object.** Moving and turning the floor, camera and sun together changes no
  pixel by more than 0.00001.
- **Voxel decals.** A box decal covers voxel pixels exactly (196 inside, 2108 outside, every
  value 0 or 1).

**Weathering**
- Snow is on tops and not on walls, and spreads to 45° slopes as `amount` grows.
- Moss grows in hollows, and rust and edge wear stay on edges.
- Wetness multiplies.
- Every recipe is valid.
- `add_weathering` appends the source material and the layer, and refuses a full stack or an
  unknown target.
- A snowed floor renders covered.

## Not in this change

- **GPU.** Weathering rules, marks and voxel decals are CPU reference only.
- **Saving.** Marks are not saved, and they don't follow fragments that break away.
- **Lua and editor.** There is no Lua or editor UI for weathering or marks yet.
