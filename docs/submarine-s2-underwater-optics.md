# S2 — Deep Explorer physically informed underwater optics

**Dependency:** S1 mini-submarine PR #224. S2 does not depend on W3/W4 draft water PRs. The implementation reuses S1's `OceanField` and `WaterVolumeSettings`, and runs in the CPU player **before HDR tonemapping**.

## Physically informed transport rather than a depth-based blue overlay

Light transport through seawater depends on the actual **length of the submerged segment of a camera ray**, the wavelengths involved, and the object's own illumination.

- Direct view transmittance: `T_c(L)=exp(-(a_c+b_c)*L)` with absorption `a` and total scattering `b` (m^-1). The background of a submerged pixel does *not* imply a geometry hit 500 m away; the bounded view path is integrated into a finite water column.
- Sunlight reaching the object: `D_c(d)=exp(-Kd_c*d)`, where `d` is actual object depth below the moving ocean surface and `Kd` is the separately authorable *diffuse downwelling irradiance attenuation*. **Kd is not the same as a+b**.
- Water-column daylight in-scattering: approximate single-scattering source using the analytic integral along the viewing path of `b_c exp(-Kdc*d(t)) exp(-(a_c+b_c)*t)`. It attenuates the incoming sunlight on the surface-to-scatterer path and again along scatterer-to-camera. A fixed isotropic phase proxy is used: this is not a full multiple-scattering radiative-transfer solver.
- Lights: two hull-mounted forward-aimed warm-white spotlights illuminate local world-space scene points with range attenuation, smooth cone cutoff and Beer–Lambert extinction on the lamp-to-object path. The illuminated object then suffers extinction on the object-to-camera path. Direct + four-step stratified volumetric lamp in-scattering makes light cones visible even when the ray misses hard geometry.
- Fresnel: a water-to-air dielectric approximation computes normal-incidence reflectance and total internal reflection; the sky seen through a submerged water interface is attenuated along the underwater portion only. **The renderer does not yet retrace refracted geometry**, so surface refraction geometry is an S2.1 quality follow-up, not a completed physical claim.

The water ray segment is found by fixed-step bisection of the exact same Gerstner `OceanField::sample()` that drives buoyancy; a camera above the surface looking toward a submerged wreck attenuates only the underwater segment. A camera below water looking toward the sky attenuates its underwater route and applies Fresnel at the crossing. Air-to-air rays receive no extra underwater tint.

## Authorable optical profiles and physical units

The existing `water_clarity` property offers clear ocean, coastal and turbid water. The S1 RGB coefficients are *visually plausible starting approximations*, not certified measurements. S2 adds:
- `custom_optics` (boolean)
- `optical_absorption_rgb` — per-channel absorption, 1/metre
- `optical_scattering_rgb` — per-channel total scattering, 1/metre
- `optical_kd_rgb` — per-channel downwelling irradiance diffuse attenuation, 1/metre
- `sub_lamp_intensity` — per-sub headlamp intensity in engine HDR scene-linear units

Overrides are independently validated for nonnegative finite coefficients ≤10/m. Never treat these three vectors as interchangeable. They approximate wavelengths via the RGB basis, **not** a calibrated spectral-to-XYZ integration. Measured Jerlov-water or site-specific coefficients can be substituted when known. If future quality demands 12–16 spectral samples, the same transport equations can be generalized before the display RGB conversion.

The red/orange channel generally suffers much stronger attenuation than blue in clear ocean. Red targets far from a camera lose contrast, while the same targets nearby under a white lamp can become red again. Coastal/turbid profiles increase scatter and visible whitewater haze. At sufficiently great depth, natural sunlight is effectively absent instead of turning the scene into an implausibly luminous blue.

## Render pipeline details

1. Trace/rasterize opaque voxels and polygons into the existing scene-linear HDR `PolygonRenderTarget` and linear depth buffer.
2. Run the **S2 underwater medium compositor**, reconstructing actual view ray distances in **world metres** (independent of DVE voxelSize), sampling the ocean surface, and evaluating lighting on submerged geometry.
3. Suppress legacy uniform distance fog in ocean scenes to avoid double extinction. Existing explicit authored fog volumes still use their original path; combining them with seawater optics is an intentional material stacking choice.
4. Apply bloom and the existing scene tone map once.
5. Apply the existing water-surface/ship/racing UI overlays. The water surface mesh is still an approximation; this PR improves volume imaging.

The physics does not depend on the renderer: all optical math and lamps have headless pure unit tests. The volume pass is deterministic across frames; no temporal noise required.

## Acceptance

- `dve_underwater_optics_tests`: Beer–Lambert wavelength/distance ordering, separate daylight decay, near-lamp red recovery, cone/range limits, water crossing both directions, no-color-in-air and water-to-air critical-angle total internal reflection.
- `dve_underwater_render_tests`: export S1 starter, boot actual player, verify volume/lamp bindings, compare real HDR-framebuffer output with/without water optics and with/without lamps, change turbidity and camera immersion, and verify image differences.

### Known limitations / responsible claims

This is a *physically informed real-time approximation*, not a spectral radiative-transfer renderer. The four-sample headlamp shafts assume homogeneous optical media; single-scattering uses an isotropic phase proxy and does not implement caustic shadows, volumetric occlusion, backscatter from particles, natural-light shafts, or articulated research-sub lamps. The renderer currently does not retrace Snell-bent refracted geometry through the moving interface. Lighting contribution uses traced scene radiance as an approximate albedo for screen-space lamps rather than performing additional shaded material BRDF sampling. These are high-value S2.1 enhancements once the native player is validated.

References:
- NOAA Ocean Exploration, "Why are so many deep-sea animals red?" https://oceanexplorer.noaa.gov/ocean-fact/red-color/
- IOCCG, *Remote Sensing of Inherent Optical Properties*, report 5 (separate a, b, and Kd): https://www.ioccg.org/reports/report5.pdf
- *Inherent optical properties of Jerlov water types*, Applied Optics 54 (2015): https://opg.optica.org/ao/abstract.cfm?uri=ao-54-17-5392
