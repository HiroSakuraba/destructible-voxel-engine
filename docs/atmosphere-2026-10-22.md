# Physical atmosphere: Rayleigh, Mie and ozone (22 Oct 2026)

Stacked on `feature/voxel-normal-maps`.

Before this change, the sky was two hand-set colours (sky and ground) blended by direction. The
sun had one fixed colour at every elevation, the background was a flat clear colour (or the
player's fake gradient), and distance fog was a flat grey. Nothing scattered light in the air.

## What's new

There's a new opt-in setting, `RenderEnvironment::skyMode`:
- `Colors` (the default) is exactly the old behaviour;
- `Physical` turns on an Earth-like atmosphere.

The atmosphere follows Hillaire's "A Scalable and Production Ready Sky and Atmosphere Rendering
Technique" (EGSR 2020), with Bruneton's Earth parameters. It has three media:

| medium | what it does | scale height |
| --- | --- | --- |
| Rayleigh (air molecules) | scattering ∝ 1/λ⁴: blue sky, red sunsets | 8 km |
| Mie (aerosols) | grey scattering and absorption: haze and the bright halo round the sun (Cornette-Shanks phase, g = 0.8) | 1.2 km |
| ozone | absorption only, peaking at 25 km: the deep blue of twilight | — |

In Physical mode, one model drives all of these:

- **Background.** Every pixel nothing was drawn in shows the sky along its ray, plus the sun
  disc. It is painted before glass is drawn, so translucent surfaces blend over it.
- **The sun's colour and strength.** `sunColor × sunIntensity` now means the sunlight at the top
  of the atmosphere. Surfaces receive it through the air: about 0.93/0.84/0.73 (r/g/b) at 60°,
  deep orange-red at 2°, and nothing once the sun is below the horizon.
- **Sky light on surfaces.** The irradiance from the whole sky hemisphere around each normal.
  This feeds the AmbientHemisphere ambient, the polygon ambient and the GI bounce hits.
- **Reflections and GI rays that miss.** The sky's radiance along that direction.
- **Aerial perspective.** The air between the camera and each surface dims it and adds the light
  the air scatters toward the camera, so distant things turn hazy and blue.
  - Real air does very little over a game scene's few hundred metres, so
    `aerialPerspectiveScale` multiplies the distances it sees (1 = physical).
  - The added light goes into the fog pass and the other passes are dimmed, so the passes still
    sum to the image.

**Settings.** Each is also in Lua, in the environment table, and in script saves (version 4;
older saves load with the defaults).

| setting | default | |
| --- | --- | --- |
| `skyMode` | Colors | Lua: `world.set_environment_sky_mode("physical")` |
| `atmosphereRayleighScale` | 1 | × Earth's air |
| `atmosphereMieScale` | 1 | haze: 1 clear, 5–20 hazy |
| `atmosphereOzoneScale` | 1 | |
| `atmosphereMieAnisotropy` | 0.8 | forward scattering of haze |
| `atmosphereGroundAlbedo` | 0.3 | the planet's ground, for light bounced back into the sky |
| `observerAltitudeMeters` | 0 | world y = 0 sits at this altitude |
| `aerialPerspectiveScale` | 1 | |
| `sunDisc` | 1 | 0 hides the disc |

`globalIlluminationIntensity` still scales the sky light on surfaces, as it scales the sky
colours in Colors mode.

## How it's computed (`dve/render/atmosphere.hpp`)

Geometry runs in double precision, because radii of 6.4 million metres are beyond float
precision. Four lookup tables:

| table | size | contents | rebuilt when |
| --- | --- | --- | --- |
| transmittance | 256 × 64 | light kept from altitude r at zenith cosine μ to space (Bruneton parameterisation, node-centred) | parameters change |
| multiple scattering | 32 × 32 | Hillaire's ψ_ms: all orders past the first, by altitude and sun elevation | parameters change (both: ~1.3 s) |
| sky view | 192 × 128 | radiance by azimuth from the sun and zenith angle, square-root spaced toward the horizon | sun elevation or observer altitude changes (~0.2 s) |
| sky irradiance | 32 × 16 | light on a surface by its normal's elevation and azimuth from the sun | same as sky view |

The tables are cached and shared between threads. Each thread remembers the last model it used,
so per-pixel lookups take no lock, and only one thread builds a missing model while the others
wait for it.

## How it's checked (`dve_atmosphere_tests`)

| check | result |
| --- | --- |
| vertical transmittance vs its closed form | integration 1e-6, table 1e-5 |
| transmittance table vs direct integration (all altitudes and angles) | 0.2% |
| single scattering in a thin atmosphere vs closed form (and the λ⁻⁴ ratio) | 0.04% (5.70 vs 5.70) |
| sky-view table vs direct march (1024 steps) | 0.8%; 0.1% within 6° of the horizon |
| sky irradiance table vs hemisphere integral | 0.3% |
| **sky radiance vs path tracing (all orders, delta tracking, lit ground)** | **6.4%** (single scattering alone is 38% off) |
| level 2 km segment: transmittance vs exp(−σd) | 1e-5 |
| aerial perspective, 12 steps vs 4096 | 0.05% |
| 100 m of air: in-scattering vs source × length | 0.2% |
| floor pixel = 0.8 × sun through the air × cos | 1e-6 |
| background pixel = the sky along its ray | exact |
| far wall takes on blue from the air; passes still sum | yes; 1e-5 |
| Colors mode returns the old blend and sun, builds nothing | exact |

**About the 6.4%.** Hillaire's approximation treats the light at each point as isotropic and
the same in every horizontal direction. It comes out a few percent dark in the blue at low
elevations. The path tracer is in the test, so any later improvement can be measured.

## Not in this change

- The GPU side: tables in HLSL are the next branch, `feature/atmosphere-gpu`.
- Light shafts and local fog: `feature/volumetric-fog`.
- The CPU hair renderer still uses the sun colour directly, without the air.
- Twilight is a bit dark, because multiple scattering is the approximation above.
- There's no editor UI for these settings yet; they're set from Lua or code.
