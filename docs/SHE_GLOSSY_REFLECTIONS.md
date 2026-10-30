# SHE-1: Spherical Harmonic Exponential (SHE) glossy reflections

## State: CPU reference implementation and shader decoder, not yet live GPU integration

Paper: A. Silvennoinen, P.-P. Sloan, M. Iwanicki, D. Nowrouzezahrai,
*Spherical Harmonic Exponentials for Efficient Glossy Reflections*,
Computer Graphics Forum 44(8), HPG 2025, [DOI:10.1111/cgf.70219](https://doi.org/10.1111/cgf.70219).
Slides: https://highperformancegraphics.org/untracked/2025/presentations/Pa5_1_hpg_2025_slides.pdf

DVE currently renders environment specular via a prefiltered cubemap plus a BRDF LUT
(`shaders/common/environment_ibl.hlsli`). SHE-1 **does not change the Vulkan/Live
Environment GPU render path**. It supplies the groundwork to implement a runtime
choice, and a CPU opt-in to compare the two.

### Implemented

- Factorization in reflection direction `r=reflect(-v,n)` and half-reflection
  `h_r=normalize(n+r)`: `E0 = exp(P(r,alpha)+Q(h_r,alpha))`.
- Orthonormal real spherical harmonics through degree 4 for P (25 terms)
  and degree 2 without redundant DC for Q (8 terms), 33 coefficients **per channel**.
- Directional roughness encoding `vMF_l(1/alpha)=exp(-l(l+1)*alpha/2)`
  where `alpha=material.roughness^2` is the GGX distribution width.
- Offline CPU GGX reference sampling and regularized log-linear least-squares
  fitting of E0. Targets use height-correlated Smith masking-shadowing.
- Nonnegative reconstruction via the exponential, with bounded log output and
  input validation; explicit fallback for alpha below 0.25.
- Packed FP16 format: RGB interleaved 99 half coefficients plus five padding
  halfs = **208 bytes per probe**. The HLSL helper uses the same basis, order,
  and packed layout; not yet bound to the live GPU pipeline.
- `IblMaterialInput::sheProbe`: optional CPU/reference opt-in; existing
  diffuse, AO, and split-sum behavior remain unchanged when null. With a fitted
  probe and supported alpha, specular uses SHE E0 weighted by the approximate
  BRDF LUT Fresnel ratio `(F0*A + B)/(A+B)`.
- Dedicated correctness tests: synthetic fitting, packing, unsupported-alpha
  fallback, and constant-color cubemap baking.

### Why not always SHE?

The paper's 33-coefficient variant was demonstrated principally for
GGX alpha in **[0.25, 1.0]**. That corresponds to perceptual roughness
approximately **[0.5, 1.0]** for DVE's roughness-squared convention.
Mirrors, glass with sharp reflections, and very glossy clear coat need
another path. SHE is an approximation to reflected *environment light*;
it is not ray-traced reflection, refraction, SSR, or a replacement for GI.

The paper reports around 200x less memory in a particular production
comparison (around 8,000 probes), not a general DVE speedup.
A 208-byte probe costs 1.59 MiB for 8,000 probes before any buffers,
diffuse resources or alignment overhead. The current prototype retains all
traditional IBL GPU textures, so **it saves no GPU memory yet**.

### Next: SHE-2 (make the Live Environment renderer use it)

1. Introduce a versioned optional SHE payload in `.dveibl` with an identity
   keyed to the source environment's content hash, plus cooker flags and cache.
   Retain legacy assets and split-sum fallback; never silently reinterpret bytes.
2. Add RHI storage-buffer upload for 208-byte probes and a new, optional
   shader binding; avoid colliding with existing frame/environment/material
   bind group slots. Update `live_environment_renderer.cpp` and the material
   pipeline inventory/PSO warmup accordingly.
3. Change `live_ibl_material_ps.hlsl` to select SHE for supported alpha and
   available probes. Sample diffuse irradiance as before, compute environment
   rotation once, and use the Fresnel factor and specular AO consistently.
   Coat and ordinary lobes choose their fallback **independently**.
4. Add hard fallback for no fitted probe, invalid or stale coefficients,
   alpha below 0.25, unsupported shader/buffer capability, and budget/quality
   limits. Do not allocate the split-sum specular cube for SHE-only probes
   until a complete safe fallback policy exists.
5. On dynamic sky/sun HDR change, fit off-thread / asynchronously or use
   a bounded GPU reduction; **never** execute the CPU fitting loop every frame.
   Double-buffer coefficients and synchronize uploads with in-flight frames.
6. Extend tests through NullDevice and Vulkan, including storage buffer barriers,
   binding reflection, descriptor retirement and refresh, plus coefficient parity
   between CPU and HLSL. Test (i) camera-angle dependence, (ii) alpha sweep,
   (iii) high-frequency HDR maps, (iv) roughness fallback, (v) sky rotations.
7. Benchmark GTX 1080 separately. Report 1080p median/p95/p99 GPU time,
   shader registers/instructions, occupancy, upload costs, CPU fitting time,
   measured VRAM, and MSE/SSIM or HDR relative errors vs path-traced
   or high-sample Monte Carlo reference. Do not assume GPU FP16 arithmetic:
   Pascal supports FP16 storage but fast native FP16 operations are not
   guaranteed. Decode into FP32 in the shader initially.

### Developer usage

```cpp
const auto fit = dve::bake_she_probe(environmentCube);
if (fit) {
    dve::IblMaterialInput m = /* ... */;
    m.sheProbe = &fit->coefficients;
    auto lit = dve::evaluate_image_based_lighting(legacyIblBake, m);
}
```

Shader-only reference decoder: `shaders/common/spherical_harmonic_exponentials.hlsli`.

### References / external ports

- Original HPG slides and paper (links above).
- MIT D3D12 reference port: https://github.com/Avelina9X/SphericalHarmonicExponentials
- MIT experimental port: https://github.com/breeze028/Spherical-Harmonic-Exponentials-for-Efficient-Glossy-Reflections

The code here is an independent implementation of the published math, not
copied from external ports. No claim is made that the shader has been compiled
or that production-quality parity with the paper has yet been demonstrated.
