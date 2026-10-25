# F2: Wind, thermals, ridge lift and aerodynamic energy checks

This PR stacks onto F1 (`feature/flying-six-modes-f1`, PR #218). It intentionally avoids Jolt, new animated geometry, asset downloads, and changes to the DVE_FLIGHT 1 save layout.

## New authorable prototype role

`dve.prototype.flight_wind` must be assigned to a **bodyless marker**. Its properties are:

| Property | Type | Limits | Semantics |
|---|---|---|---|
| `key` | string | unique, 1..64 | Stable authoring key |
| `kind` | int | 0 or 1 | Thermal (0), ridge (1) |
| `radius` | number | 5..5000 metres | Thermal core radius or ridge along-axis half-length |
| `height` | number | 10..5000 metres | Thermal height; ridge vertical half-extent |
| `strength` | number | 0..30 m/s | Core rise or peak ridge lift at 10 m/s wind |
| `sink_fraction` | number | 0..2 | Compensating outer thermal sink / lee downdraft fraction |

At most 128 volumes are permitted. The marker pose supplies the center and ridge orientation. For ridges, local **+X** follows the crest, local **+Y** points toward the nominal windward face. The active windward side flips automatically when ambient wind reverses.

Aircraft properties `wind_x`/`wind_y` continue to work. Two new optional properties `wind_shear` (signed m/s logarithmic altitude response) and `wind_gust` (deterministic horizontal gust amplitude) modify the background flow. Missing properties use 0 and are valid in older scenes.

## Physics and numerical contract

All aerodynamic forces are calculated from `v_air = v_world - wind(worldPosition, simulationTime)` at the 120 Hz physics tick. Lift is perpendicular to air-relative velocity, drag opposes it, and glider/bird propulsion is not silently added. A pure `flight_aerodynamics()` function provides an independent force evaluator used by tests. Dynamic pressure incorporates a simple exponentially declining atmospheric density. Past critical angle of attack, the lift coefficient decays while drag rises. Assisted bank turns remain adjustable through the existing `assisted` aircraft property; gravity, buoyancy, wings and powered modes share the same continuous state.

The `FlightWindField` returns 3D airflow, with attribution for thermal rise and ridge rise. A finite inner thermal core has a smooth outward decay and an annular sink; the column gradually fades near bottom and top. Ridgeline lift follows windward side, ridge extent, altitude and incoming wind speed, with downward air in its lee. The source positions drift deterministically with elapsed simulation time. All fields have bounded magnitudes, finite input validation, and sorted stable IDs.

Flight HUD shows indicated airspeed, altitude and a smoothed climb/sink **variometer**. F3 adds bank angle, angle of attack, instantaneous glide ratio, the specific-energy change estimate, 3-component wind vector and attributed thermal/ridge lift. Full instrument state is derived, not permanently serialized.

In still air, an unpowered glider's specific mechanical energy should not increase. This is measured with kinetic energy per mass plus `g * altitude`; small tolerances account for discrete fixed-step integration. In rising air, gaining energy from the atmospheric flow is legitimate. In hover and powered flight, fuel/battery provide energy; the glider energy contract does not apply.

## Wind Garden

The starter includes (1) an authored rising-air column around (50,280), (2) a bird soaring column around (20,520) and (3) a ridge near the east canyon wall whose windward/lee sides depend on wind direction. These are markers, not collision geometry, and may be moved and tuned in the editor. Older exported F1 scenes that do not contain wind markers still load, but are **calm** until wind zones are added.

## Tests and validation

`dve_flight_physics_tests` covers calm-air initial state, thermal core/edge/annular sink, vertical bounds, deterministic shear/gust samples, wind reversal across a ridge, stalled lift collapse, post-stall drag, wind-relative dynamic pressure, and a long unpowered still-air glide with energy-accounting checks.

`dve_flying_game_tests` continues validating six-mode transitions, fixed 60/120 Hz stepping, project export/player boot and save/load; F2 adds authored wind marker validation and an in-game thermal-versus-calm flight comparison.

**Still out of scope:** validated real-world aerodynamic coefficient tables, turbulence CFD, geographic mesoscale weather, procedural cloud rendering, morphing meshes, spatial audio and deformable wings. F2 is a physically better *game* baseline rather than an aircraft certification simulator.
