# Subsurface, part 2: a random-walk ground truth (22 Oct 2026)

Builds on part 1 (translucency). This is the yardstick for every subsurface approximation in DVE,
the same role the photon tracer plays for caustics.

## The walk (`include/dve/render/subsurface_random_walk.hpp`)

Light is followed through the volume of the material, step by step:
1. It flies an exponential free path.
2. At each event it survives with the single-scattering albedo and scatters (isotropic, or
   Henyey–Greenstein).
3. It ends when it crosses the surface again, or is absorbed.
4. A smooth boundary with index η > 1 sends light back inside with the exact Fresnel probability,
   total internal reflection included. Light arriving from outside first loses its specular
   share.

**Where it runs**, through a `SubsurfaceDomain` (the same march contract as part 1):
- an analytic slab or half-space, for exact checks;
- the cells of one material in a voxel instance (leaving into another solid absorbs);
- a closed polygon mesh.

**The medium for a material** uses Chiang, Kutz and Burley's fits from the artist's albedo A (per
channel) and Burley scatter distance d, the two numbers DVE materials already carry:
- α = 1 − exp(−5.09406 A + 2.61188 A² − 4.31805 A³)
- s(A) = 1.9 − A + 3.5 (A − 0.8)²
- σt = 1 / (d s(A))

Sources:
- Chiang, Kutz and Burley, *Practical and Controllable Subsurface Scattering for Production Path
  Tracing* (SIGGRAPH 2016 talk);
- Christensen and Burley, *Approximate Reflectance Profiles for Efficient Subsurface Scattering*
  (2015);
- Chandrasekhar, *Radiative Transfer* (1950), for the H-function.

## Checked against exact answers (`tests/test_subsurface_random_walk.cpp`)

| case | walk | exact |
|---|---|---|
| no scattering, τ = 1: transmitted e^−τ | 0.3680 | 0.3679 |
| no absorption, τ = 2 slab: R + T | 1.000000 | 1 |
| half-space, α = 0.5: reflectance 1 − H(1)√(1 − α) | 0.1156 | 0.1152 |
| half-space, α = 0.9 | 0.4143 | 0.4149 |
| half-space, α = 0.99 | 0.7532 | 0.7527 |
| η = 1.4 boundary, no absorption: specular + R + T | all light accounted for | 1 |

All within four standard errors (about ±0.0007).

**The same walk in a voxel slab and a polygon box** gives the analytic slab's R and T: 0.397 / 0.170
in all three. A stone floor under the voxel slab blocks transmission.

## What it measures

**Chiang's mapping works.** A half-space lit diffusely reflects close to the artist's albedo:

| A | reflected |
|---|---|
| 0.2 | 0.20 |
| 0.5 | 0.50 |
| 0.8 | 0.80 |
| 0.95 | 0.90 |

**Burley's reflectance profile is close to the truth.** Light entering at one point comes out
within 0.03 of the walk's share in each ring out to 8 d. At high albedo it misses some of the long
tail. This is what front-lit softening will use.

**Part 1's translucency formula is not close.** Light that scatters through a slab with the sun
overhead (index-matched):

| A | t / d | part 1 (A T(t/d)) | walk |
|---|---|---|---|
| 0.8 | 0.5 | 0.63 | 0.17 |
| 0.8 | 1 | 0.50 | 0.27 |
| 0.8 | 2 | 0.34 | 0.33 |
| 0.8 | 4 | 0.16 | 0.27 |
| 0.8 | 8 | 0.04 | 0.14 |

There are two reasons:
- **Thin slabs:** most light goes straight through without scattering (here 63 % at t = 0.5 d).
  That is a beam, the sun seen through the slab, not a glow; Burley's form counts it all as glow.
- **Thick slabs:** light diffuses much farther than Burley's e^(−t/3d) tail allows (3× more at 8 d).

The same thickness form is used by Unity HDRP and Unreal, so this is a known weak point of the
approach, now measured. The next part replaces it with the walk's own numbers.
