# S1 — Deep Explorer: mini-submarine physics and water volume

**Branch:** `feature/submarine-s1-deep-explorer`, based on `main` after W2 #221 merged. W3 #222 and W4 #223 remain separate draft branches. No dependency on their wake or coastal implementations.

## Architecture

S1 **reuses** DVE's `OceanField` for the moving free surface, sea state, sampled surface velocity and mean current. The new `sample_water_volume()` extends those 2D queries to arbitrary depth with an authorable floor, hydrostatic pressure and a reduced orbital motion envelope. There is no second ocean solver. A `WaterVolumeSample` also exposes optical absorption, scattering and diffuse daylight attenuation per linear RGB channel, plus turbidity and particle density; **actual underwater volumetric rendering belongs to S2**.

Water remains Z-up and SI units. Surface height is sampled at the vehicle's world X/Y at the saved simulation time. Hydrostatic pressure follows `101325 + density * 9.81 * max(0,surfaceZ-worldZ)`. Deep-water motion retains the authored mean current, while orbital surface contributions decay approximately as `exp(-depth/5m)`. The decay length is a configurable-quality approximation, not a wavelength-specific water-column spectral model. The optical coefficients are provisional physically inspired profiles to calibrate with measurements in S2.

## Submersible physics

The mini-sub is `craft_type = 6` on the existing `dve.prototype.water_vessel` role, reserving value 5 for W4's future Surfboard. Default dry mass 2350 kg, displaced volume 2.35 m³, initial ballast 60 kg, tank capacity 260 kg. It gets:
- Archimedean **full-volume** buoyancy when submerged, smooth partial immersion at the surface and gravitational weight from dry mass plus actual ballast.
- Independent forward, lateral, vertical, yaw and pitch thrust plus water-relative anisotropic translation drag, rotational damping and momentum-preserving integration at **120 Hz**. Motor power consumes finite battery.
- Bounded ballast pumps, positive/neutral/negative buoyancy, assisted PD depth hold with buoyancy feedforward, and manual vertical trimming.
- Authorable pressure/depth limit; a warning appears when exceeding the operating rating. S1 does not simulate hull implosion.
- Real geometry collision sweeps and an explicit, uniform seafloor collision floor. S1 flat bathymetry is sufficient for the test range; W4 integration can introduce local bottom depth later.

**Not** an accurately certified submarine simulator: the hull inertia is a box approximation, thruster locations are simplified, gas compression and free-surface ballast sloshing are not solved, full spatial ocean-current spectra are not simulated, and depth-hold gains require playtesting.

## Starter scene and controls

Use the **Create mini-submarine** editor action or `dve_create_submarine NEW_PROJECT_DIRECTORY`. The starter includes a yellow mini-sub, flat collidable seafloor at Z=-40, a rocky outcrop, a wreck at roughly Z=-30 and three colored survey targets.

| Control | Action |
|---|---|
| W / S | Forward / reverse thrust |
| A / D | Yaw |
| Q / E | Lateral thrusters |
| Space / Left Ctrl | Vertical thrust up / down |
| Z / X | Pump ballast out / in |
| Up / Down arrows | Pitch |
| H | Toggle depth hold (capture current depth) |
| P / R / F3 | Pause, restart, telemetry |

The HUD displays depth, ballast mass, battery, pressure and rating warning. The native player uses the shared water renderer; proper underwater color extinction and artificial lamps are **S2**.

## Save format

Ordinary W1/W2 projects still emit and restore `DVE_WATER 2`; the new sub-containing project emits `DVE_WATER 4`. The latter contains per-vessel ballast mass, battery charge, hold flag and target depth in addition to vessel keys, camera selection, fixed tick clock, pose, linear/angular velocity and trim. The reader retains W1/W2 v1/v2 parsing, validates finite data and ensures restored ballast does not exceed the vessel capacity. Version 4 deliberately avoids conflict with W3's `DVE_WATER 3` wake event format. Once W3 merges, both v3 and v4 should be supported together.

## Verification

- `dve_submarine_tests` — headless water-volume pressure, orbital decay, clarity interfaces, neutral/positive/negative buoyancy, ballast pump, depth-hold feedback, thruster and battery, depth rating.
- `dve_submarine_game_tests` — generated scene validation/export/boot, real player dive input, seafloor limit, depth-hold toggle, ballast input, full save/load.
- Existing W2 vessel, W1 ocean, flight and racing tests are unchanged.

Native flight/playability validation is still required before final physics tuning. Ocean refraction, underwater scattering/fog and lamp attenuation should follow in S2 and be evaluated in **linear HDR before tone mapping**, not as a fixed blue overlay.
