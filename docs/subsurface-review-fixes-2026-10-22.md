# Subsurface: fixes from review (22 Oct 2026)

Codex's notes on #184 (diffusion), plus one problem found while testing them.

- **Any sample count keeps Burley's profile.** Sample radii were taken at fixed stratum midpoints.
  With 1 or 2 samples per pixel, no sample ever picked the short exponential, so the profile
  itself was wrong, not just noisy. Each pixel now offsets all its strata by one hashed jitter, so
  every sample is uniform in its stratum and the ¼ : ¾ mixture holds for any count. Voxel and
  polygon references both.
- **The hybrid reference's diffusion setting reaches the voxels.** `render_hybrid_reference` built
  its voxel renderer with the default 16 samples whatever the polygon options said. Now
  `polygonOptions.subsurfaceDiffusionSamples` applies to both.
- **Sample angles.** These were the bit-reversed pixel seed. Down an image column the seed steps by
  a multiple of 16, so whole columns shared one narrow slice of angles: correlated, not noise.
  Angles are now a golden-ratio sequence turned by a hashed per-pixel rotation.

(The other note on #182, on triangle lookups, is fixed in the caustics review PR, with the BVH's
new `firstIndex`.)

## Tests (`tests/test_subsurface_diffusion.cpp`)

- **1, 2 and 3 samples per pixel:** a shadow edge on marble, averaged down 960-pixel columns,
  matches the exact Burley edge within 0.04, 0.02 and 0.01. That is plain noise; before, it was
  off by 0.27 to 0.51.
- **Hybrid with 0 samples:** identical to the voxel renderer with diffusion off.
- **512 samples:** the edge test still matches within 0.003.
