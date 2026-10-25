# W3 — Interactive boat wakes, foam and bow spray

**Parent:** W2 PR #221, stacked on merged W1 PR #220.

W3 is the first local interactive-water layer. It does not introduce a second ocean height function. Its `WaterWakeField` is a bounded collection of deterministic, analytical wake impulses that are **composed inside the existing OceanField::sample()**. The same displaced surface is therefore consumed by the CPU ocean renderer, multi-probe buoyancy, hull damping, planing evaluation and sailboat physics.

## Wake model

Each wake impulse stores a world-space emitter position, normalized vessel heading, emission clock, amplitude, wavelength, width, propagation speed and lifetime. The impulse is an expanding, Gaussian-enveloped, oscillatory surface disturbance with attenuated forward propagation. It is an **arcade Kelvin-style approximation**, not a solved free-surface Navier–Stokes system or a full Kelvin spectral wedge.

The evaluator returns surface height, analytic horizontal gradients, vertical water velocity and a bounded whitecap/foam estimate. The ocean combines derivatives before normalizing the final geometric normal. Wake queries have finite compact support with age and distance cutoffs, and are bounded in aggregate to protect buoyancy stability under many overlapping impulses.

The field accepts at most **128 impulses**. On overflow the oldest is evicted; expired impulses are pruned from the fixed-step world, and newly emitted impulses never affect another boat in the same tick. Source ordering is thus independent of whether the controlled speedboat or the pirate ship appears first in the authored scene. Emissions originate abaft the hull to avoid repeated self-buoyancy pulses on the generating vessel's own stern probes.

## Wake generation

All moving vessels can emit wakes when their **water-relative** forward speed exceeds 2.2 m/s and the hull is submerged. Emission is throttled to once every 0.16 seconds per vessel, with calibrated amplitude/width/wavelength based on speed, immersion impact and hull dimensions. Low-speed/resting vessels cannot create constant disturbance for free. Reverse motion reverses the authored trailing heading.

The wake field affects other boats via their next-tick 6-DOF buoyancy and drag calculation; there is no special boat-to-boat force trick and no independent visual-only wave displacement.

## Bow spray and rendering

At higher speeds, each emitter also spawns two bounded, short-lived ballistic spray patches around the bow. These visual particles have finite radius, opacity and lifetime and are updated at fixed steps, with gravitational fall and water-surface termination. The CPU player projects them into screen space with scene depth occlusion. Their opacity falls with age. **Spray does not alter ship forces.** Water-crest foam is calculated through the shared ocean query and appears in the regular CPU water shading pass.

The wake and spray implementation is renderer-independent until the final screen-space composition, so headless gameplay continues to work.

## Persistence and determinism

W3 writes **DVE_WATER 3** with per-boat wake emission cooldowns, the active wake events, vessel key/trim, 6-DOF states and simulation clock. It retains readers for `DVE_WATER 1` and `DVE_WATER 2` scenes. The saved impulses are validated before applying restoration; invalid/nonfinite data, invalid count and temporally impossible wake events are rejected.

Spray particles remain derived transient effects and do not affect future physics. They are cleared after a saved state is loaded.

## Verification

- `dve_water_wake_tests`: finite/deterministic wave queries, gradient/velocity, foam, compact support, expiration, bounded 128-event storage, transactional reload and physically coupled probe response.
- `dve_water_game_tests`: includes framebuffer projection of water spray, while retaining prior five-craft export, simulation and save/restore coverage.
- Existing W1 ocean, W2 vessel-dynamics, racing and flight tests remain unchanged except for their shared executable linkage.

This is not yet a depth-aware coastal shallow-water solver. Wakes do not bounce off piers, reflect from cliffs or create fully volumetric overturning waves. W4 is the appropriate stage for shoaling, breaking surf and surfboard dynamics. GPU FFT ocean and high-end underwater PBR rendering remain separate optional upgrades.
