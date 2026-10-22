# Caustics, part 3: the caustic map (21 Oct 2026)

Builds on part 2 (light through media, photon ground truth).

## The method

A noise-free caustic method, the one a real-time version will rasterize
(`include/dve/render/caustic_map.hpp`):

1. **Sunlight as a mesh.** A grid of rays (default 256 × 256 cells) covers the glass and water as
   the sun sees them.
2. **Follow each vertex.** Each grid vertex is followed through the media
   (`follow_light_transmitted`: Snell, exact Fresnel, Beer–Lambert, total internal reflection) to
   the surface where it lands.
3. **Irradiance from area change.** Each grid triangle carries a fixed share of sunlight; where it
   lands, that share is spread over the landed triangle. So irradiance = flux ÷ landed area: a
   triangle that shrank has focused its light.
4. **Drop triangles that don't fit.** A triangle is skipped if its corners land on different
   surfaces, or on different branches (different numbers of refractions or reflections). Silhouette
   rims are where this happens.
5. **Filter.** A point's irradiance is averaged over a small regular 16-gon around it, clipped
   exactly against each landed triangle:
   - its radius is tan(sun radius) × the distance from the last surface the light left (a real
     sun blurs caustics), and at least a quarter of a grid cell;
   - this keeps energy exact;
   - folds, where triangles collapse to nothing, stay finite: they are bounded by flux ÷ filter
     area, the physical limit for a sun of that size.
6. **Lookups.** A multi-level grid index keeps them fast even when edge triangles land stretched
   across the floor.

Sources:
- Evan Wallace, *Rendering Realtime Caustics in WebGL*;
- Shah, Konttinen and Pattanaik, *Caustics Mapping: An Image-Space Technique for Real-Time
  Caustics* (2007).

## In the renderer

`ReferenceVoxelRenderer` gains two optional inputs:
- `caustics`: added to the sun term of every voxel it reaches;
- `sunOccluders`: polygons that now block the sun on voxels. Glass and water block it too; the
  light they let through comes back as caustics, so nothing is counted twice.

The CPU player builds the map automatically at Reference quality when any material has
transmission > 0 (`CpuPlayerRendererOptions::causticResolution`, default 256, 0 = off). Without
glass or water, every image is unchanged.

Not yet: seeing refraction from the camera (glass and water still draw as alpha-blended surfaces),
caustics landing on polygons, reflected caustics, and the GPU version (see the plan below).

## Measured (`tests/test_caustic_map.cpp`)

- **Glass slab: 0.92160** (exact 0.9216). **Pool under a 40° sun: 0.74754** (exact 0.74754). Both
  noise-free and exact to five digits.
- **Glass ball, sun radius 0.03:**
  - the energy through it is within 0.75 % of the Fresnel integral (the rim triangles across the
    silhouette are dropped);
  - the floor pattern matches the photon ground truth (2 million photons) within 8 % wherever it is
    lit, from the centre out to beyond the ball's edge.
- **Point-sun focus:** bright (×2400 the sun at 0.012 rad) but finite.
- **Renderer:**
  - without occluders, the floor under the ball is plain sunlit;
  - with them it is fully shadowed;
  - with the caustic map it is far brighter than plain sun at the focus, and dark around it.

A 64 × 64-voxel pool with a wavy water surface and a glass ball renders at 800 × 500 in about 7 s on
the CPU, 2.3 s of it building the map at 768 × 768.

## Toward caustic mapping on the GPU

The same triangles map onto the GPU directly:
1. A compute pass traces the grid vertices with the brick tracer, for voxel media and height-field
   water.
2. A raster pass draws the landed triangles additively into a light-space caustic map per cascade,
   with flux ÷ area as the colour. The receiver's depth comes from the cascade's depth map, so
   only first-visible receivers count.
3. Shading samples that map like a shadow map, filtered by the sun-size blur.

The photon reference and these tests are the yardstick for each step.
