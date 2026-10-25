# W2 — Vessel-specific sailing, planing and pump-jet dynamics

W2 is stacked on W1 PR #220. It adds distinct hydrodynamic/propulsion profiles without replacing the shared Gerstner ocean evaluator.

## Models

| Craft | Drive | Stabilization | Steering |
|---|---|---|---|
| Dinghy | Small engine | Displaced-volume buoyancy and damping | Water-flow rudder |
| Sailboat | Lift/drag from apparent wind at the mast | Immersed centerboard resists lateral slip; sail induces heel | Speed-dependent rudder |
| Pirate ship | Large sail area, no engine in generated starter | Distributed 9 x 5 buoyancy, large keel and inertia | Stern rudder |
| Speedboat | High-thrust propeller | Speed- and immersion-dependent planing lift | Steerage from stern flow |
| Wave runner | Pump-jet thrust | Short 3 x 3 hull, planing lift | Vectored jet, augmented by rudder |

All five share a single `OceanField` height/normal/orbital-water-velocity query. W2 uses `evaluate_vessel_dynamics()`, a pure function that derives forces and moments from boat orientation, world-relative water velocity, apparent wind, helm, throttle, and trim. The resulting loads are accumulated with W1's buoyancy and drag at the authoritative 120 Hz step.

Sails use an arcade lift-and-drag polar based on apparent wind and sheet angle, with force applied at a high center of effort to generate heel. Centerboards and keels oppose lateral slip relative to water. Rudder response strengthens with water-relative speed and loses authority near rest. Speedboats generate dynamic planing lift proportional to forward-water-speed squared, capped by the vessel's mass and disabled in reverse/airborne motion. Pump jets vector thrust directly with helm; propulsion weakens as the intake emerges. No craft receives a magical hard speed cap as part of its handling profile, although the shared W1 integrator retains a numerical maximum for stability.

This is a simplified game hydrodynamics model; it is not a CFD, VPP, foil-lift, or complete hydrostatics solver. Sail force depends on an approximate polar and should be tuned by native playtesting. Propeller cavitation, tacking AI, boom collisions, reefing and advanced sail shaping remain later enhancements.

## Authoring

The `dve.prototype.water_ocean` marker adds `wind_x` and `wind_y`, in metres per second; this is *atmospheric wind*, separate from the existing ocean `current_x/current_y`. Defaults are (5,8) m/s. The vessel component adds `craft_type` (0 dinghy, 1 sailboat, 2 pirate ship, 3 speedboat, 4 wave runner), `sail_area_m2`, `keel_area_m2`, `mast_height_m`, and `planing_factor`. W1 settings `thrust_newtons` and `rudder_torque` remain supported, and a missing `craft_type` defaults to the old motorized dinghy behavior. Up to 8 vessel objects may share one scene.

## Player controls

W/S = throttle or sail exposure, A/D = helm, Q/E = sail trim, 1–5 select craft, E = next craft when used outside trim bindings? **Note:** W2 dedicates E to positive sail trim, so selection cycling should be remapped to a distinct action; direct keys 1–5 always work. P pause, R restart, F3 show propulsion/keel diagnostics.

## Save compatibility

W2 writes `DVE_WATER 2` with ordered unique vessel keys, per-vessel sail trim, fixed-step clock and all translational/rotational states. It can restore `DVE_WATER 1` snapshots from W1 two-vessel scenes by retaining the legacy positional parser. Older scenes remain playable without migration; only new scenes contain specialized wind-powered/planing vessel profiles.

## Verification and limitations

- `dve_water_vessel_tests`: validates true zero-wind sail rest, trim-dependent sail forces, heeling moments, keel lateral drag, rudder speed dependence, pump-jet steering, forward-speed planing and airborne lift removal.
- `dve_water_game_tests`: five-craft export/boot, player switching, wind-driven sailboat, engine-powered speedboat/jet ski, and save-load roundtrip.
- W1 ocean and hull tests remain in the suite.

W3 remains wakes, wake interaction, spray, foam, storm blending and underwater shading. W4 handles shoaling, breaking-wave physics and surfing. The CPU ocean renderer remains unchanged in this PR; W2 is a vessel physics/gameplay feature.
