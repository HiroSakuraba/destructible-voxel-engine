# W4 — Coastal shoaling, breaking-wave approximation and playable surfboard

**Parent:** PR #222 (W3, which stacks on #221); W1 #220 is merged. W4 is independently testable as a headless physics module with a generated beach starter.

## Three cooperating water systems

- W1: offshore Gerstner ocean, surface normal/height/velocity and 6-DOF buoyancy.
- W3: local physical boat wakes, surface response and bow-spray rendering.
- **W4: local depth-aware coastal swell, crest breaking/whitewater flow and surfboard dynamics.**

The W4 surf component is an *authorable, bounded local addend* to `OceanField::sample`. It is queried by exactly the same physics/render code as W1/W3. No separate visual sine-wave shader was added.

## Bathymetry and break fronts

A single `dve.prototype.water_coast` marker encodes a straight nearshore line: world X center and Y waterline from its transform, offshore region extending toward -Y. `half_width_m` bounds the coastal zone in X; `offshore_length_m` bounds it in -Y. `beach_slope` sets the bathymetric rise toward the shoreline. The approach amplitude increases in shallow water with a bounded shoaling gain; phase speed is capped and depends on local depth; a second harmonic increases crest asymmetry without representing a physical overturning lip. Breaking fraction depends on the ratio of local water depth to swell amplitude and the passing crest. Shoreward whitewater drift and accumulated foam use that same fraction. Lateral phase skew (`peel`) makes the break front advance across the crest rather than across the entire shore simultaneously.

All of these are **game approximations**. They do *not* solve mass-conserving shallow-water equations, resolve a fully three-dimensional tube/barrel, or reproduce bathymetric refraction to engineering accuracy. Wave gradients and whitewater flow are deterministic and available in headless builds. At the authored beach, depth decreases to zero; the renderer omits ocean pixels on dry sand and hull probes do not calculate buoyancy in dry land.

## Surfboard / rider mechanics

`WatercraftType::Surfboard` (type 5) adds a 95 kg rider+board with 4×2 floatation probes. It shares 120 Hz 6-DOF integration, collision sweeps, camera and authoring with the W1/W2 vessels. Its pure `evaluate_surfboard()` computes:
1. Contact-weighted downslope gravity force from the water-surface normal, so carving down the wave face converts gravitational potential into horizontal speed.
2. Water-relative lateral fin/rail resistance, with steer-to-carve sideforce at the stern.
3. Limited *rider-powered* paddling; this energy input is distinct from passive water propulsion.
4. Water-surface normal alignment, angular damping, stance stability and a deterministic wipeout threshold for excessive roll, steep broken faces at low speed, and beaching.

The surfboard has no engine, no sails and no pump-jet. W/S provides paddle effort, A/D steers/carves, 6 selects the board, and Tab cycles boats. F3 includes breaking, balance, board force and wake diagnostic state.

## Editor beach

The Water Ships starter includes the original five W2 craft, one board with a minimal standing rider silhouette, a sandy collision shoreline, and a Coast marker with editable slope/width/length/swell/period/peel. The default beach has its surf zone centered near world (0,-240), with the board starting offshore near (0,-330). These can be relocated/retuned without source edits; other W1–W3 scenes without a coastal marker retain their previous deep-water behavior.

## Tests and safety

`dve_water_surf_tests` checks valid/invalid coast authoring, bounded coastal extent, monotonically shoaling bathymetry, dry boundary, mobile wave faces, breaking crests with shoreward flow and foam, physical ocean composition, nonzero rider carvings and balance/wipeout, and zero airborne rail/face force. `dve_water_game_tests` checks the six-craft exported project, coast initialization, surfboard selection, rider motion and finite numerical state alongside earlier W3 wake-save and rendering checks.

W3 first fixed its 4 KB component save limit by bounding stored wake events before W4 was branched. W4 retains the W1/W2/W3 `DVE_WATER 3` state layout; board pose and velocity already participate in the per-vessel state array. Surfboard wipeout counters are **derived diagnostics** and reset after load; they do not affect physics. No multiplayer synchronization, finite-volume shoreline solver, true surf lip rendering, wave fracture or beach foam texture atlas is claimed.

## Future work

- Heightfield shallow-water patches with conservative transport and deterministic coupling to open-ocean boundaries.
- Articulated surf stance/IK, trick scoring, aerial grabs, surfboards with separate fins and movable rider center-of-mass.
- Breaking barrel geometry and more accurate coastal spray, foam diffusion and bathymetric wave refraction.
- Underwater refraction, shore wet-sand shading, GPU FFT upgrade.
