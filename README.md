# Destructible Voxel Engine — v2.35 Runtime and Tooling Foundations

## v2.35 runtime and tooling foundations

- Adds deterministic navigation agents with path following, waypoint advancement, replanning,
  stuck detection, and explicit off-mesh traversal state.
- Adds voxel-edit dirty-region tracking and transactional navigation rebuilds. This release
  preserves path continuity but still rebuilds the compact mesh as a whole; stitched partial
  tile replacement is a documented follow-up.
- Adds deterministic `.dvepak` archives with manifests, dependency-first collection, hashes,
  editor-only stripping, incremental reuse, mounting, and integrity verification.
- Adds low-overhead CPU profiling scopes, frame timelines, counters, memory categories, and
  deterministic JSON capture.
- Adds an asset dependency graph, missing-reference diagnostics, rename fix-up, source
  fingerprints, and deterministic reimport ordering.
- Adds prioritized input contexts, composite chords, press/release/hold/tap/double-tap
  triggers, conflict detection, and persistent remapping.
- Adds versioned, atomic save slots with migrations, section hashes, backup rotation, and
  corruption recovery.
- Adds CPU humanoid retargeting, morph targets, CCD inverse kinematics, blackboards, behavior
  trees, perception, steering, rollback buffers, interpolation, and replication relevancy.
- Extends the rigid-body contract with torque, angular impulse, query-all ray/AABB/sphere
  operations, ignored-body filters, stable hit order, and physical-material metadata.
- Adds undoable editor operations and plugin registration for commands, panels, importers,
  components, inspectors, and scripting bindings.
- Adds the `dve_pack` CLI and the `dve_v235_foundations_tests` deterministic contract suite.

See `docs/V235_FOUNDATIONS.md` for API boundaries, validation, and remaining production work.

## Previous release: v2.34 physics integration repair

## v2.34 physics integration repair

- Pins the reproducible Jolt fetch route to the official v5.6.0 release.
- Promotes contact-sink registration, contact-material assignment, point impulses, and point
  forces into the solver-neutral `IRigidBodyWorld` contract.
- Implements torque-producing point forces in Jolt and Box3D while retaining a conservative
  center-of-mass fallback for reference backends.
- Applies marionette spring forces at their authored body anchors and includes angular point
  velocity in damping, restoring the expected limb torque.
- Retains Jolt as the automatic production 3D preference, Box3D as an optional experimental
  backend, Box2D for 2D rigid bodies, and DVE's native deformation/fluid solvers.
- See `destructible_voxel_engine_v2_34_physics_integration_report.md`.

## v2.32 fully articulated marionette

- Adds a six-string humanoid marionette runtime over the existing skeleton, control-rig, ragdoll, rigid-body, and platform input systems.
- Generates an 11-control rig with pelvis/chest/head controls, four two-bone IK chains, and elbow/knee pole controls.
- Generates a 15-body articulated ragdoll with spine, neck, shoulder, elbow, wrist, hip, knee, and ankle constraints.
- Supports Assisted Rig, Hybrid, and Physical modes.
- Adds a unified input router for tracked VR hand controllers and SDL/DualSense-style gamepad events.
- Uses six spring-damper strings for head, pelvis, hands, and feet, with per-string telemetry and bounded forces.
- Focused linked validation passes against the actual engine animation, control-rig, ragdoll, rigid-body, and platform-event implementations.
- Point-force torque is available through the solver-neutral physics interface. Physical crossbar collision, haptics, OpenXR device plumbing, and a native marionette editor remain follow-up work.
- See `destructible_voxel_engine_marionette_format_v1.md`, `destructible_voxel_engine_v2_32_marionette_report.md`, and `destructible_voxel_engine_v2_32_remaining_work_plan.md`.

## v2.30 navigation mesh and heightmap terrain

- Adds a deterministic CPU triangle navmesh for polygon, voxel, heightmap, and directly authored world geometry.
- Adds slope, step-height, agent-radius portal clearance, agent-height clearance, area/flag filters, query obstacles, off-mesh links, partial paths, A* corridors, funnel smoothing, and `.dnav` persistence.
- Extracts navigation triangles from transformed polygon assets and exposed destructible-voxel top faces.
- Imports 8/16-bit grayscale PNG, PGM P2/P5, and RAW16 heightmaps with explicit world scale, origin, elevation, byte order, flip, and inversion controls.
- Cooks the same heightmap into `.dmesh` polygon terrain, optional solid destructible `.dvox` terrain, and `.dnav` navigation through `dve_cook_heightmap`.
- Adds conservative Terrain and Navigation asset-browser classifications and explicit resource limits for large sources.
- Passes the complete editor-enabled Unix Makefiles build, 170/170 configured tests, strict changed-unit compilation, CLI round trips, and focused ASan/UBSan/leak checks.
- Native terrain import UI, tiled streaming terrain, polygon-region merging, complete boundary erosion, and dirty nav-tile rebuilding remain follow-up work.
- See `destructible_voxel_engine_navigation_mesh_format_v1.md`, `destructible_voxel_engine_heightmap_terrain_format_v1.md`, `destructible_voxel_engine_v2_30_navigation_heightmap_report.md`, and `destructible_voxel_engine_v2_30_remaining_work_plan.md`.

## v2.29.4 shadow and runtime UI corrections

- Uses one absolute light-space depth origin for both cascaded-shadow casters and samplers, including regression cases hundreds of metres from the world origin.
- Matches Vulkan caster Y to sampler V, pancakes near-depth casters, and resolves both static and dynamic shadow-atlas layers.
- Makes continuous sliders keyboard-operable and implements rectangle-based spatial focus navigation.
- Adds UTF-8 caret editing, text-safe Space handling, Home/End/Delete, filtered paste, commit events, clickable lists, and common resolved enablement.
- Ensures late-created viewports inherit reduced-motion and shake settings.
- Adds `dve_v2294_shadow_ui_fixes_tests` and passes the complete Unix Makefiles build, 170/170 tests, strict changed-unit compilation, shader validation, and focused sanitizer/leak execution.
- Canonical shadow HLSL is source-validated; physical backend execution remains required.
- See `destructible_voxel_engine_v2_29_4_shadow_ui_fixes_report.md`, `destructible_voxel_engine_shadow_ui_corrections_v1.md`, and `destructible_voxel_engine_v2_29_4_remaining_work_plan.md`.

## v2.29.3 ray-lighting units and GI sampling corrections

- Converts metre-authored ray distances into voxel tracer units through an explicit metres-per-voxel render constant.
- Adds a sun contribution at GI bounce points, distance-weighted AO, stratified sampling, actual hit-position GI origins, and temporal/spatial filtering contracts.
- Adds focused shader/GI regressions and raises the working registry to 169 tests.
- The canonical ray-lighting compute shaders remain source-validated rather than physically dispatched.

## v2.29.2 lighting and shadow corrections

- Corrects the live CPU split-sum BRDF integration to use the image-based-lighting Smith factor `k = roughness² / 2`, restoring the environment-specular scale that was 13–16% low at common roughness values.
- Adds seamless CPU cubemap face-crossing filters, filtered source mip chains, and GGX solid-angle source-LOD selection for rough specular prefiltering.
- Separates diffuse ambient occlusion from roughness/view-dependent specular occlusion and prevents hemisphere ambient from double-counting diffuse sky lighting when an environment is active.
- Corrects two-sided foliage transmission normalization and hemisphere selection, and applies two-interface attenuation to clear-coat base lighting.
- Makes receiver and normal shadow bias operational, rejects out-of-cascade coordinates safely, and clamps every PCF tap to the selected cascade tile's half-texel interior.
- Adds `dve_v2292_lighting_rendering_fixes_tests`, covering the numerical BRDF target, cubemap seams, source mips, rough-prefilter smoothing, specular occlusion, shadow bias/bounds, and canonical shader equations.
- Passes a complete Unix Makefiles build, 168/168 registered tests, strict warnings-as-errors compilation, 48-shader source validation, and ASan/UBSan/leak execution.
- The CPU IBL path is live and tested. The corrected shading and shadow HLSL remains source-validated rather than physically dispatched in this environment.
- See `destructible_voxel_engine_v2_29_2_lighting_rendering_fixes_report.md`, `destructible_voxel_engine_lighting_rendering_corrections_v1.md`, and `destructible_voxel_engine_v2_29_2_remaining_work_plan.md`.

## v2.29.1 camera optics corrections

- Preserves square-pixel scene geometry for anamorphic cameras and derives the delivery frame from `sensorWidth * squeeze / sensorHeight`.
- Corrects physical depth-of-field behavior across filmbacks, gate-fit modes, resolutions, foreground occlusion, split-diopter seams, and anisotropic bokeh.
- Uses diopter-, stop-, tangent-, logarithmic-, and mired-space interpolation where camera controls are not meaningful under linear interpolation in their display units.
- Corrects exposure and ordering for bloom, vignetting, LUT grading, film grain, halation, flare, chromatic aberration, and focus breathing.
- Adds `dve_v2291_camera_optics_tests`, a permanent 37-check optical regression target.
- Passes the complete Unix Makefiles build, 167/167 registered tests, strict changed-unit compilation, shader-contract validation, and sanitized optics execution.
- See `destructible_voxel_engine_v2_29_1_camera_optics_fixes_report.md` and `destructible_voxel_engine_v2_29_1_remaining_work_plan.md`.

## v2.29 cinematic camera menus and native UX

- Exposes the v2.28 cinematic pipeline through 41 stable View, Window, Project Settings, and Command Center actions without adding another top-level menu.
- Adds explicit Camera Instance, Shot Override, Project Default, and temporary Viewport Preview ownership scopes.
- Adds a pointer-operable native Cinematic Camera panel with eight sections, ten presets, seven filmback/custom routes, clipboard, reset, keyframing, sequencer state, and local undo/redo.
- Adds focus, split-diopter, safe-frame, aspect-matte, motion-vector, exposure, and graded/neutral viewport overlays.
- Persists named project-default presets and filmbacks, initializes new camera rigs from project defaults, and creates valid complete-profile sequencer keys.
- Fixes X11 public-header compatibility by renaming the zero-valued disabled distortion enumerator from `None` to `Disabled`.
- Passes strict changed-unit compilation, focused ASan/UBSan/leak validation, the complete configured build, and 166/166 registered tests.
- See `destructible_voxel_engine_cinematic_camera_ux_format_v1.md`, `destructible_voxel_engine_v2_29_cinematic_camera_ux_report.md`, and `destructible_voxel_engine_v2_29_remaining_work_plan.md`.

## v2.28 cinematic camera, lens, framing, and film pipeline

- Adds Super 16, Super 35, full-frame, anamorphic, IMAX 15-perf, and IMAX digital physical filmbacks plus Academy, IMAX, 1.85, 2.39, and custom framing mattes.
- Adds split diopter, physical shaped bokeh, aperture blades, anamorphic and cat-eye bokeh, Brown-Conrady distortion, equidistant fisheye, chromatic aberration, anamorphic squeeze/flare, breathing, and gate weave.
- Adds temperature/tint, lift/gamma/gain, `.cube` 3D LUT grading, ACES/Reinhard/linear tone mapping, grain, halation, sharpening, vignette, and motion-vector blur.
- Adds ten editable cinematic presets, sequence-v3 persistence, transactional sequencer APIs, a deterministic CPU reference pipeline, a fixed 544-byte GPU packet, and two canonical compute-shader contracts.
- Passes strict changed-unit compilation, 6/6 focused tests, ASan/UBSan/leak validation, 48-shader contract validation, the complete configured build, and 157/157 registered tests.
- Physical backend dispatch, GPU image parity, LUT residency, complete native inspectors, and representative-device performance certification remain explicit follow-up work.
- See `destructible_voxel_engine_cinematic_camera_format_v1.md`, `destructible_voxel_engine_v2_28_cinematic_camera_report.md`, and `destructible_voxel_engine_v2_28_remaining_work_plan.md`.

## v2.27 packed brick-palette GPU resolve conformance

- Preserves the exact six-buffer v2.26 renderer submission contract and extends it with analytic material, resolve-request, and writable output buffers.
- Adds a packed CPU oracle that decodes the submitted records, remaps, palette slots, byte-packed weights, baked samples, and single-material IDs rather than returning to authoring data.
- Adds one canonical HLSL compute resolver for baked, single-material, palette2, and palette4 paths with asset-UV and world-triplanar evaluation.
- Adds a cross-backend RHI compute harness for publication, binding lifetime, pipeline creation, dispatch, fence wait, and readback.
- Adds automatic C++/HLSL ABI drift checks and verifies every sample in representative palette2/palette4 bricks against the CPU shading reference.
- Passes strict changed-unit compilation, focused ASan/UBSan/leak execution, 46-shader contract validation, the complete configured engine build, and 132/132 registered tests.
- Physical shader compilation/execution, texture residency, final voxel surface integration, and representative-hardware parity remain explicit follow-up work.
- See `destructible_voxel_engine_v2_27_brick_palette_gpu_resolve_report.md`, `destructible_voxel_engine_material_switcher_design_v2.md`, and `destructible_voxel_engine_feature_gap_backlog.md`.

## v2.26 production brick-palette cooking and renderer submission

- Connects the merged material switcher to ordinary `VoxelObject` brick cooking and model/scene asset-pipeline wrappers.
- Adds platform retention profiles, deterministic dependency keys, per-brick retained representation sets, byte accounting, and regional recooking from `AppliedBrickEdit` records.
- Adds fixed-layout renderer upload packets for baked, single-material, palette2, and palette4 representations, with material remaps and validated buffer ranges.
- Adds palette2/palette4 asset-UV and world-triplanar pipeline contracts plus a six-binding cross-backend RHI layout.
- Adds a Null-RHI mirror with exact buffer readback, bind-group lifetime, and submission validation.
- Passes strict focused and sanitizer suites, the complete 698-step Linux engine build, and 163/163 registered tests.
- Physical Vulkan, Direct3D 12, and Metal palette shaders, live residency integration, and hardware certification remain explicit follow-up work.
- See `destructible_voxel_engine_v2_26_renderer_submission_report.md`, `destructible_voxel_engine_material_switcher_design_v2.md`, and `destructible_voxel_engine_feature_gap_backlog.md`.

## v2.25 merged material switcher

- Uses one versioned `DVEBPAL` brick payload for single, palette2, and palette4 material representations.
- Merges brick-level cooking, overflow diagnostics, cross-brick remapping, world-triplanar/asset-UV CPU reference shading, exact sample-cost accounting, and deferred-to-baked crossfades.
- Retains live representation switching, fallback during recook, atomic activation, generation tracking, and retained-memory accounting.
- Stores full canonical per-sample contributions for genuine recooking and uses order-independent deterministic accumulation.

## v2.24 selectable voxel material policy

- Adds selectable **Baked Properties**, **Single Material**, **Deferred Brick Palette**, and **Hybrid** policies without removing the conventional material path.
- Adds per-project, per-asset, platform, distance, LOD, destructibility, recent-edit, and memory-pressure selection with deterministic fallback and recook evidence.
- Adds two- and four-material deferred-path contracts, palette-overflow policies, canonical-source checks, runtime-switch availability, stable JSON reports, and integration with 3D Rendering Diagnostics.
- Adds project settings and the stable **Voxel Material Policy** command under `Tools → Voxel Materials` and Command Center.
- Passes 39/39 affected regressions, 11/11 strict changed-unit checks, and 3/3 focused ASan/UBSan/leak suites.
- v2.25 completes deterministic brick-palette cooking, CPU reference shading, retained representations, recooking, and live runtime switching; v2.26 connects those contracts to production voxel cooking and renderer submission.
- See `destructible_voxel_engine_material_switcher_design_v2.md`, `destructible_voxel_engine_brick_palette_format_v1.md`, `destructible_voxel_engine_v2_25_merged_material_switcher_report.md`, and `destructible_voxel_engine_feature_gap_backlog.md`.


## v2.23 unified 3D rendering diagnostics

- Adds a renderer-neutral `Render3DDiagnosticsInput`/`Render3DDiagnosticsReport` path for geometry, pipeline-state changes, residency, LOD, skinning, shadows, lights, occlusion, reference validation, budgets, and deterministic depth-complexity evidence.
- Adds the native **3D Rendering Diagnostics** panel through `Tools → Rendering` and `F11`; renderer integrations can replace the retained reference report through `set_render3d_diagnostics_report`.
- Preserves the v2.19 Sprite Diagnostics path and unifies at the reporting, validation, residency, budget, heatmap, and editor layers rather than forcing mesh draws and sprite quads into one packet type.
- See `destructible_voxel_engine_render3d_diagnostics_format_v1.md`, `destructible_voxel_engine_v2_23_3d_rendering_diagnostics_report.md`, and `destructible_voxel_engine_v2_23_remaining_work_plan.md`.


## v2.22 pixel-art creation and multi-part sprite rigs

- Adds a native **Pixel Art Studio** (`F7`) for indexed and RGBA layered animation frames with pencil, eraser, fill, line, rectangle, selection transforms, wrap painting, palette remapping, onion preview, frame/layer operations, and bounded undo/redo.
- Adds versioned `.dvepixel` source documents and deterministic publication into the existing PNG, `.dvepalette`, and `.dvesprite` cooking path rather than introducing a parallel runtime sprite format.
- Adds a versioned `.dvespriterig` asset with hierarchical bones, sprite parts, clip keys, draw-order/visibility changes, two-bone IK, equipment and appearance variants, deterministic pose sampling, bake planning, validation, hashing, and transactional authoring.
- Adds the native **Multi-Part Sprite Rig** panel (`Shift+F7`) with live bone/part/IK preview and menu/Command Center integration.
- Includes retained original `v222_sun_pilot` pixel source, indexed sheet, palette, sprite asset, and rig asset plus a deterministic authoring demo.
- Passes 22/22 focused and inherited sprite/editor regressions and strict warnings-as-errors checks for all ten changed translation units.
- See `destructible_voxel_engine_sprite_pixel_art_format_v1.md`,
  `destructible_voxel_engine_sprite_rig2d_format_v1.md`,
  `destructible_voxel_engine_v2_22_pixel_art_rig_authoring_report.md`, and
  `destructible_voxel_engine_v2_22_remaining_work_plan.md`.


## v2.21 4K, high-DPI, and multi-display presentation

- Adds tested 720p, 1080p, 1440p, 4K, 5K, 8K, ultrawide, super-ultrawide, and portrait resolution profiles while retaining logical pixel-art resolution independence.
- Proves exact integer scaling to 3840x2160 for 320x180, 640x360, 960x540, 1280x720, and 1920x1080 logical targets.
- Adds display topology, logical/work-area/native-pixel metadata, mixed-DPI window placement, disconnected-display recovery, fullscreen/windowed contracts, and presentation budgets.
- Adds one-to-four-player, asymmetric three-player, automatic two-player, and picture-in-picture layouts with per-player camera, HUD, input, audio-listener, safe-area, and diagnostic-budget ownership.
- Adds direct `GameCameraRuntime` synchronization, HUD/pointer remapping, dynamic shared-camera split/merge hysteresis, listener-mixing policies, and mixed-refresh pacing plans.
- Adds stable-ID independent RHI swapchain management for game, spectator, diagnostics, or auxiliary windows, including one-surface resize/out-of-date recreation and clean removal.
- Extends game/editor settings for display mode, preferred display, high-DPI, resolution profile, local-player count, split-screen layout, dynamic shared camera, and spectator windows.
- See `destructible_voxel_engine_display_presentation_format_v1.md`,
  `destructible_voxel_engine_v2_21_4k_multi_display_report.md`, and
  `destructible_voxel_engine_v2_21_remaining_work_plan.md`.


## v2.20 menu command center and information architecture

- Reworks the existing eight-menu editor shell into compact **Primary**, opt-in **Advanced**, and **Palette-only** command tiers without changing stable command IDs or shortcut bindings.
- Renames the command palette to **Command Center** and adds fuzzy multi-token search across commands, settings, panels, assets, scene objects, and indexed documentation. Provider prefixes are `>`, `@`, `/`, `#`, `:`, and `?`.
- Adds explicit disabled-command explanations shared by menus, shortcuts, automation, context menus, and Command Center results. Availability is refreshed at the dispatch boundary to avoid stale selection or play-state decisions.
- Persists local advanced-menu preference, favorites, and bounded command history under `.dve/user/editor_menu_state.txt`; state is validated, forward-bounded, and never written into project assets.
- Adds task-oriented menu metadata, dangerous-action presentation, documentation indexing, panel/object/asset activation, and accessibility-tree filtering that matches the visible compact or advanced menu mode.
- Passes 10/10 focused and inherited editor workflows, native X11 smoke, strict warnings-as-errors checks for all seven changed translation units, and 4/4 focused ASan/UBSan/leak suites.
- See `destructible_voxel_engine_menu_command_center_format_v2.md`,
  `destructible_voxel_engine_v2_20_menu_command_center_report.md`,
  `destructible_voxel_engine_v2_20_release_evidence.txt`, and
  `destructible_voxel_engine_v2_20_remaining_work_plan.md`.


## v2.19 sprite diagnostics and tile-world desktop editor

- Adds deterministic per-camera sprite diagnostics with draw-call and batch-break evidence, sprite/tile/visible-pixel/fragment counts, exact atlas occupancy and overlap accounting, fragmentation, texture/palette residency, sorting-layer visualization, pixel-snap checks, missing-reference audits, and per-profile budgets.
- Adds a CPU/reference overdraw rasterizer and RGBA heatmap that includes sprite quads and optional visible tiles, with stable content hashes for regression evidence.
- Promotes the v2.13 tile authoring document/canvas into a native desktop panel with tile, terrain, layer, object, trigger, hazard, and spawn inspectors; layer controls; direct object/collision handles; autotile visualization; parallax preview; chunk diagnostics; recook overlays; prefab placement/overrides; validation; save; and one-click play testing.
- Adds **Sprite → Tile World Editor** (`F8`) and **Sprite → Sprite Diagnostics** (`F10`) while retaining **Sprite → Play Original Sprite Level** (`F9`).
- Passes 3/3 focused release tests, 8/8 focused/inherited integration tests, strict warnings-as-errors checks for all changed implementation units, and 3/3 focused ASan/UBSan/leak suites.
- See `destructible_voxel_engine_v2_19_sprite_diagnostics_tile_world_editor_report.md`,
  `destructible_voxel_engine_v2_19_release_evidence.txt`, and
  `destructible_voxel_engine_v2_19_remaining_work_plan.md`.


## v2.18 sprite production presentation

- Completes direct sprite-track editing with canvas translate/resize handles, complete numeric value editing, atomic multiframe clipboard operations, duplicate/mirror/retime/presets, searchable lanes, item-local diagnostics, and combat/socket onion overlays.
- Promotes the platform-neutral animation graph into a native editor panel with rendered nodes and transitions, inspectors, live state/transition highlighting, validation badges, comments, groups, minimap, breadcrumbs, reroutes, and atomic graph copy/paste. Subgraphs are retained as groundwork; blend trees and animation layers remain future composition depth.
- Adds reproducible original indexed sprite sheets and a particle flipbook for the Sun Route vertical slice: player, enemy, projectile, collectible, checkpoint, and hazard clips, palette variants/cycling, authored gameplay tracks, camera/parallax/HUD/shake, production sprite/tile submissions, and an in-editor **Play Original Sprite Level** command (`F9`).
- Adds a deterministic renderer-neutral sprite-particle system with burst and continuous emitters, flipbooks, XY/XZ billboards, velocity stretch, color/size/rotation over life, sorting/batching, animation-event/socket spawning, trails, beams, and a CPU reference rasterizer.
- Adds `dve_sprite_vertical_slice_v218_demo` as the packaged headless presentation proof and `tools/generate_sprite_pixels_v218.py` for deterministic PNG reproduction. The retained replay finishes with the same v2.17 gameplay state hash and a stable v2.18 presentation hash.
- Passes 4/4 focused release tests, 22/22 inherited sprite/tile/audio regressions, strict warnings-as-errors compilation for all changed implementation units, and 4/4 focused ASan/UBSan/leak suites.
- See `destructible_voxel_engine_v2_18_sprite_production_presentation_report.md`,
  `destructible_voxel_engine_v2_18_release_evidence.txt`, and
  `destructible_voxel_engine_v2_18_remaining_work_plan.md`.

## v2.17 native chiptune authoring and deterministic vertical slice

- Added the native Pattern/Instrument/SFX tracker panel, keyboard and piano entry, transactional pattern/order editing, envelope/wavetable drawing, effect helpers, SFX customization, bus-routed live preview, clipboard/history workflow, and atomic `.dvechip` publication.
- Added an original deterministic side-scrolling logic proof with movement, hazards, enemy/projectile interaction, collectible, checkpoint, HUD, save/restart, replay hashing, and tracker-authored music/SFX.
- See `destructible_voxel_engine_v2_17_chiptune_authoring_vertical_slice_report.md` and `destructible_voxel_engine_v2_17_release_evidence.txt`.

## v2.16 mainline merge

- Reconciles the parallel v2.13 native tile-world authoring branch with the v2.13-v2.15 CPU hair branch on top of the shared v2.12 sprite foundation.
- Retains the complete tileset/tilemap authoring stack, semantic world queries, animated tile extraction, original sample world, and tile collision/controller regressions.
- Retains the complete batched CPU guide solver, bounded self-collision, guide-to-child ribbon expansion, GameWorld ownership, cooker, tests, and benchmarks.
- Resolves the shared build, documentation, release-manifest, and historical work-plan conflicts without replacing newer hair/runtime files with the older parallel branch.
- Adds a merged regression target for both feature families and records branch provenance in `destructible_voxel_engine_v2_16_mainline_merge_report.md`.
- See `destructible_voxel_engine_v2_16_release_evidence.txt` and `destructible_voxel_engine_v2_16_remaining_work_plan.md` for validation and continuation.

## v2.15 guide-to-visible-hair ribbon rendering

- Added deterministic procedural child strands around the simulated guide curves, preserving the guide centerline while adding stable disk-distributed children.
- Added camera-facing tapered ribbon vertices, triangle indices, per-triangle layer IDs, packet bounds, topology hashes, and zero-copy packet spans suitable for renderer upload.
- Added guide-count, visible-strand, sampled-point, and projected-size LOD with distributed guide selection rather than truncating one side of the groom.
- Added a `CpuHairRuntime` owner bridge and bounded packet validation for one million strands, sixteen million vertices, and ninety-six million indices.
- Batched 32 visible strands per persistent worker job and capped the default pool at four workers to avoid per-strand scheduling and oversubscription overhead.
- Added an opaque-depth-aware deterministic CPU reference renderer for regression tests, editor thumbnails, and graphics-backend bring-up. It is not presented as the production GPU path.
- Measured 8,192 visible strands from 2,048 simulated guides at 4.78 ms/frame for four-worker expansion, 2.69× faster than serial expansion in the release test environment.
- See `destructible_voxel_engine_v2_15_cpu_hair_ribbon_rendering_report.md`,
  `destructible_voxel_engine_v2_15_cpu_hair_ribbon_rendering_benchmark.json`,
  `destructible_voxel_engine_v2_15_cpu_hair_release_evidence.txt`, and
  `destructible_voxel_engine_v2_15_remaining_work_plan.md`.

## v2.14 bounded spatial-hash hair self-collision

- Added optional cross-guide particle self-collision using a preallocated uniform spatial hash rather than guide-pair enumeration.
- Added deterministic parallel Jacobi queries with a stable serial correction pass and no cross-thread point writes.
- Added hard per-point neighbor caps, independent self-collision guide LOD, radius/stiffness/iteration controls, wake behavior, and external-collider reprojection.
- Added self-collision point/test/projection/hash-health telemetry and all-sleeping skip behavior.
- Retained the original v2.13 one-pass simulation path when self-collision is disabled.
- Added overlap, LOD, invalid-setting, and serial/parallel determinism tests plus strict-warning and sanitizer evidence.
- Measured 2,048 guides × 16 points at 14.86 ms/frame with full dense self-collision, or 7.29 ms/frame with a 512-guide self-collision budget, on four workers in the release test environment.
- See `destructible_voxel_engine_v2_14_cpu_hair_self_collision_report.md`,
  `destructible_voxel_engine_v2_14_cpu_hair_self_collision_benchmark.json`,
  `destructible_voxel_engine_v2_14_cpu_hair_release_evidence.txt`, and
  `destructible_voxel_engine_v2_14_remaining_work_plan.md`.

## v2.13 batched CPU hair runtime

- Added a clean-room, multithreaded XPBD guide-hair solver optimized around contiguous groom storage and independent strand jobs.
- Added Sisir-style ASCII PLY import plus strict, bounded, content-hashed `.dvehair` runtime assets and `dve_cook_hair`.
- Added sphere, capsule, and plane collision; root transforms and per-guide skinned root targets; sleeping; guide-budget LOD; update-rate control; wind; impulses; and zero-copy render spans.
- Integrated hair with `GameWorld`: ordinary polygon, voxel, marker, attached, animated, and rigid-body-backed objects can own hair; transforms, enable state, and destruction synchronize automatically.
- Added deterministic serial/parallel tests, malformed-import coverage, GameWorld lifecycle tests, and a comparative benchmark.
- Measured 2,048 guides × 16 points at 3.97 ms/frame on four workers in the release test environment, approximately 3.84× faster than serial and 1.75× faster than the generic rope representation.
- See `destructible_voxel_engine_cpu_hair_format_v1.md`,
  `destructible_voxel_engine_v2_13_cpu_hair_report.md`,
  `destructible_voxel_engine_v2_13_cpu_hair_release_evidence.txt`, and
  `destructible_voxel_engine_v2_13_remaining_work_plan.md`.


## parallel v2.13 native tileset and level authoring branch

- Added canonical reusable `.dvetileset` assets with source dimensions, margin/spacing grid slicing, per-tile collision and gameplay semantics, animation ranges, and deterministic four-neighbor autotile rules.
- Added `DVE_TILEMAP 2` visual, collision, hazard, trigger, and object layers while retaining bounded v1 migration and a self-contained resolved tileset copy.
- Added transactional tileset and tilemap documents with recoverable save/open, 128-level undo/redo, dirty external-change conflict handling, exact regional edit impact, and stable object identities.
- Added platform-neutral palette and map workspaces for zoom/pan, tile selection, pencil/eraser/rectangle/fill/terrain painting, autotiling, object creation and movement, and deterministic preview geometry.
- Added semantic world queries, animated visible-tile extraction, chunk diagnostics, and compatibility with native tile collision, Physics2D, and side-view controller paths.
- Added an original sample tileset and level demonstrating terrain, animated water, slopes, one-way platforms, ladders, conveyors, hazards, triggers, and object layers.
- See `destructible_voxel_engine_tileset_format_v1.md`,
  `destructible_voxel_engine_tilemap_format_v2.md`,
  `destructible_voxel_engine_tilemap_authoring_format_v1.md`,
  `destructible_voxel_engine_v2_13_tile_world_authoring_report.md`,
  `destructible_voxel_engine_v2_13_release_evidence.txt`, and
  `destructible_voxel_engine_v2_13_tile_world_remaining_work_plan.md`.


## v2.12 sprite visual graph, blend gameplay, and root-motion production pass

- Added `DVE_SPRITE_MACHINE 3` blend curves and explicit track, event, and root-motion policies while retaining
  verified v1/v2 loading and migration.
- Added weighted source/destination combat, socket, and property samples plus deterministic cross-source event
  merging during active transitions.
- Connected active animation blends to dual alpha-weighted sprite submissions, including palette packet retention.
- Added a visual graph workspace with node/edge frames, zoom, pan, marquee and additive selection, drag-created
  transitions, live node movement, search, copy/paste, delete, and live runtime highlighting.
- Extended collision-aware root motion with step-up, slope-limited ground snap, moving-platform displacement, support
  normals, accurate collision telemetry, and guarded residual-motion requeue.
- Passed strict warning-as-error compilation and the focused 10/10 v2.04-v2.12 sprite regression set.
- See `destructible_voxel_engine_sprite_animation_state_machine_format_v3.md`,
  `destructible_voxel_engine_sprite_animation_graph_authoring_format_v1.md`,
  `destructible_voxel_engine_sprite_runtime_integration_format_v3.md`,
  `destructible_voxel_engine_v2_12_sprite_visual_graph_blend_gameplay_report.md`,
  `destructible_voxel_engine_v2_12_release_evidence.txt`, and
  `destructible_voxel_engine_v2_12_remaining_work_plan.md`.


## v2.11 sprite animation authoring, blending, and collision-aware motion

- Added `DVE_SPRITE_MACHINE 2` graph positions, transition duration, normalized-time synchronization, and explicit
  interruption-source policy while retaining verified v1 loading and migration.
- Added deterministic transition blending with source/destination samples, weights, transition serials, and complete
  blend-state rollback snapshots.
- Added a transactional, native-ready state-machine graph document with states, parameters, transitions, selection,
  reference repair, validation, save/open, bounded undo/redo, and external-change conflict handling.
- Added deterministic side-effect command identities and an idempotency ledger for interval and transition events
  across rollback resimulation.
- Added collision-aware 2D root-motion application using shape casts, skin distance, bounded surface sliding, residual
  reporting, and a side-view-controller adapter.
- Passed strict warning-as-error compilation and the focused 8/8 v2.04-v2.11 sprite regression set.
- See `destructible_voxel_engine_sprite_animation_state_machine_format_v2.md`,
  `destructible_voxel_engine_sprite_animation_authoring_format_v1.md`,
  `destructible_voxel_engine_sprite_runtime_integration_format_v2.md`,
  `destructible_voxel_engine_v2_11_sprite_animation_authoring_blending_report.md`,
  `destructible_voxel_engine_v2_11_release_evidence.txt`, and
  `destructible_voxel_engine_v2_11_remaining_work_plan.md`.


## v2.10 sprite animation state runtime and gameplay adapters

- Added pure forward/reverse interval event queries for frame events, combat activation/deactivation, socket keys,
  typed property keys, and root-motion keys across skipped frames, loop crossings, and PingPong traversal.
- Reworked `SpriteRuntime::tick` to expose all crossed owner-tagged boundaries rather than only the final sampled
  frame, with explicit truncation and loop-crossing evidence.
- Added bounded, content-hashed `.dvesm` state-machine assets with typed parameters, named states, state-local and
  any-state transitions, priorities, exit time, minimum residence time, and trigger consumption.
- Added deterministic state-machine runtime ownership, transition events, force-state operations, complete rollback
  snapshots, and validated restore.
- Added Transform/Velocity root-motion adapters for neutral Physics2D bodies and `GameWorld` objects.
- Added a socket-to-`GameWorld` bridge with transform application, visibility inheritance, missing-socket policy,
  and structured diagnostics.
- Passed strict warning-as-error compilation and the focused 8/8 headless sprite regression set.
- See `destructible_voxel_engine_sprite_animation_state_machine_format_v1.md`,
  `destructible_voxel_engine_sprite_runtime_integration_format_v1.md`,
  `destructible_voxel_engine_v2_10_sprite_animation_runtime_report.md`,
  `destructible_voxel_engine_v2_10_release_evidence.txt`, and
  `destructible_voxel_engine_v2_10_remaining_work_plan.md`.


This repository is a buildable C++23 checkpoint for a hybrid destructible-voxel and indexed-polygon
engine with editor, runtime, rendering, audio, gameplay, networking, AI/MCP, 3D text, Gabor-volume,
and native simulation subsystems.

## v2.09 sprite precision authoring and gameplay integration

- Added `DVE_SPRITE 4` persistent 64-bit IDs for combat windows, sockets, typed properties, and root-motion
  keys, with uniqueness validation, semantic hashing, deterministic legacy migration, and verified v1-v3 reads.
- Added stable native Tracks-inspector rows and ID-addressed transactional edits rather than relying on vector
  position or delete-most-recent behavior.
- Added combat translate/resize/rotate/range operations, duplicate and X-mirror commands, one-pixel arrow-key
  nudging, Shift resizing, Alt rotation, Ctrl+D duplication, M mirroring, and selected-item deletion.
- Added skipped-frame and loop-aware root-motion accumulation, per-owner pending motion, destructive consumption,
  flip/gameplay-plane conversion, and world-orientation output.
- Added renderer-neutral sprite-socket attachments and per-owner gameplay debug packets carrying stable track IDs,
  active combat volumes, sockets, properties, and pending root motion.
- Passed strict warning-as-error compilation and the focused 7/7 headless sprite regression set.
- See `destructible_voxel_engine_sprite2d_format_v4.md`,
  `destructible_voxel_engine_sprite_authoring_format_v4.md`,
  `destructible_voxel_engine_v2_09_sprite_precision_gameplay_integration_report.md`,
  `destructible_voxel_engine_v2_09_release_evidence.txt`, and
  `destructible_voxel_engine_v2_09_remaining_work_plan.md`.


## v2.08 sprite combat tracks, sockets, properties, and root motion

- Added `DVE_SPRITE 3` with bounded, hashed clip-local combat windows, named sockets, typed property keys,
  and root-motion keys while preserving verified v1/v2 reads.
- Added deterministic track sampling through Once, Loop, and PingPong playback with explicit authored-sequence
  identity, step/linear interpolation, and flip-aware world-space output.
- Added transactional authoring APIs and metadata-preserving exact timeline permutations; combat membership is
  reconstructed when reordered frames no longer form one contiguous interval.
- Added a native Sprite Editor Tracks inspector with hitbox, hurtbox, socket, property, and root-motion creation,
  canvas overlays, timeline lane markers, undo/redo, and coordinated saving.
- Corrected Vulkan command-list texture-state validation so render passes recognize barriers recorded earlier in
  the same list; the ordinary Vulkan sprite render/readback regression now passes.
- All eleven sprite-focused CTest targets pass.
- See `destructible_voxel_engine_sprite2d_format_v3.md`,
  `destructible_voxel_engine_v2_08_sprite_combat_tracks_sockets_report.md`,
  `destructible_voxel_engine_v2_08_release_evidence.txt`, and
  `destructible_voxel_engine_v2_08_remaining_work_plan.md`.


## v2.07 indexed sprite rendering and native palette authoring

- Added an indexed sprite RHI pipeline contract with an optional palette fragment module, a read-only
  256-entry RGBA8 storage buffer, palette bind group 1, blend-mode-matched indexed pipelines, and
  palette-aware frame telemetry.
- Added deterministic palette resource caching keyed by palette asset and visual state, with content,
  resolved-color, and entry-count collision checks rather than trusting a state hash by itself.
- Added canonical indexed fragment shader sources for HLSL and Vulkan GLSL. Shader inventory validation
  passes, but a compiled indexed SPIR-V module and physical indexed image evidence are not included because
  no indexed shader compiler artifact was available. The ordinary Vulkan sprite path is validated separately.
- Added `SpritePaletteAuthoringSession`: transactional bank/swatch/transparency/cycle editing, deterministic
  RGBA import, 128-step undo/redo, fixed-240-Hz preview, bounded saving, clean external reload, and dirty-file
  conflict protection.
- Added a native Sprite Editor Palette inspector with source-art palette creation/linking, swatch and channel
  editing, bank and cycle controls, live deterministic preview, coordinated saving, and external-change state.
- Added timeline virtualization and wheel scrolling while preserving actual frame indices for selection,
  stepping, range selection, and drag reordering.
- Added focused v2.07 palette-authoring and indexed-Null-RHI tests while retaining the complete sprite
  regression suite.
- See `destructible_voxel_engine_sprite_palette_authoring_format_v1.md`,
  `destructible_voxel_engine_sprite_rhi_format_v2.md`,
  `destructible_voxel_engine_sprite_shader_format_v2.md`,
  `destructible_voxel_engine_sprite_authoring_format_v3.md`,
  `destructible_voxel_engine_v2_07_indexed_rendering_palette_authoring_report.md`,
  `destructible_voxel_engine_v2_07_release_evidence.txt`, and
  `destructible_voxel_engine_v2_07_remaining_work_plan.md`.


## v2.06 indexed sprite palettes and deterministic cycling

- Added versioned `.dvepalette` assets with named banks, up to 256 RGBA8 entries, a stable transparent
  index, bounded non-overlapping cycle tracks, strict parsing, and semantic content hashes.
- Added deterministic packed-atlas conversion with explicit `error`, `exact`, and `nearest` policies,
  import diagnostics, and a portable red-channel indexed PNG transport.
- Extended the cooker to publish atlas, `DVE_SPRITE 2` asset, `DVE_SPRITE_COOK 2` manifest, and palette
  in one recoverable transaction; palette-only changes invalidate incremental evidence.
- Added runtime palette registration, per-instance bank overrides and swaps, fixed 240 Hz forward/reverse/
  ping-pong cycles, phase offsets, visually stable packet identities, and deduplicated palette packets.
- Retained legacy `DVE_SPRITE 1` reads and unchanged RGBA sprite behavior. The current RHI renderer rejects
  indexed packets explicitly; GPU palette lookup and physical-device validation remain future work.
- Added focused palette codec/runtime/cooker tests and corrected stale historical component-schema counts.
- See `destructible_voxel_engine_sprite_palette_format_v1.md`,
  `destructible_voxel_engine_sprite2d_format_v2.md`,
  `destructible_voxel_engine_sprite_cook_hot_reload_format_v2.md`,
  `destructible_voxel_engine_v2_06_indexed_sprite_palettes_report.md`,
  `destructible_voxel_engine_v2_06_release_evidence.txt`, and
  `destructible_voxel_engine_v2_06_remaining_work_plan.md`.


## v2.05 sprite cooking and source hot reload

- Added real deterministic packed-atlas publication: RGBA8 PNG output, a cooked `.dvesprite` with
  packed dimensions/rectangles and the generated texture reference, plus a versioned dependency manifest.
- Added verified incremental cooking. An output is `up_to_date` only when the manifest matches and the
  existing atlas and cooked asset independently match their expected hashes.
- Added recoverable three-file publication using sibling staging files and backups, so atlas, sprite asset,
  and dependency evidence are replaced together rather than leaving mixed versions after a failure.
- Added portable source-image watching and automatic transactional reimport. Compatible changes rebuild
  active packed previews while preserving selection; incompatible changes leave the current document intact.
- Split sprite authoring/cooking into the lightweight `dve_sprite_tools` library and added the headless
  `dve_cook_sprite` command without editor, audio, platform, or RHI dependencies.
- Existing `.dvesprite` assets and runtime rendering contracts remain unchanged. No GPU implementation or
  physical-GPU validation was added.
- See `destructible_voxel_engine_sprite_cook_hot_reload_format_v1.md`,
  `destructible_voxel_engine_v2_05_sprite_cooking_hot_reload_report.md`,
  `destructible_voxel_engine_v2_05_release_evidence.txt`, and
  `destructible_voxel_engine_v2_05_remaining_work_plan.md`.


## v2.04 sprite authoring, 2D queries, joints, and platformer collision

- Added a native CPU sprite-authoring workspace for PNG, JPEG, and `.dvesprite` assets with canvas
  zoom/pan, grid and freeform slicing, pivots, trim, multiframe timeline editing, frame reordering,
  events, playback controls, onion-skin state, deterministic atlas preview, and transactional save/reimport.
- Preserved source-space authoring data while packed previews are active, so reimport and `.dvesprite`
  saves remain valid until the cooker publishes a packed texture asset.
- Added backend-neutral box, circle, capsule, and convex-polygon overlap and shape-cast queries with
  filters, ignored bodies, sensors, tile-map policy, stable result ordering, tags, and surface velocity.
- Added revolute, prismatic, distance, weld, wheel, and motor joints to the neutral 2D contract and
  scene-component schema, with Box2D 3.1/3.2 construction, state, cleanup, limits, motors, springs,
  break thresholds, and break events. NativeTile rejects joints explicitly.
- Expanded the side-view controller with wall, ceiling, and ledge probes, surrounding-body velocity,
  moving one-way platforms, non-accumulating support/conveyor transport, crush grace, and snapshots.
- Added release, strict warning-as-error, real upstream Box2D 3.2, retained 3.1 contract, native X11,
  and ASan/UBSan coverage. No GPU implementation or validation was added.
- See `destructible_voxel_engine_sprite_authoring_format_v2.md`,
  `destructible_voxel_engine_physics2d_box2d_format_v2.md`,
  `destructible_voxel_engine_sideview_character_controller_format_v2.md`,
  `destructible_voxel_engine_v2_04_sprite_authoring_queries_joints_platformer_report.md`,
  `destructible_voxel_engine_v2_04_release_evidence.txt`, and
  `destructible_voxel_engine_v2_04_remaining_work_plan.md`.


## v2.03 menu and settings UX

- Reduced the native editor from fourteen top-level menus to eight familiar application menus:
  File, Edit, Create, View, Tools, Build, Window, and Help.
- Consolidated voxel, polygon, material, rendering, audio, and physics tools under categorized Tools
  sections; camera commands now live under View without changing stable command identifiers.
- Made the command palette visible and useful: it searches commands and settings, supports command-only
  (`>`) and settings-only (`@`) modes, keyboard and pointer selection, session recents, and session pins.
- Reworked Preferences and Project Settings around explicit User/Project/Session scope tabs, searchable
  categories, Changed and Advanced filters, source/default details, direct value entry, and transactional
  reset-to-inherited actions.
- Added command descriptions and keywords so labels do not need to carry every discoverability synonym.
- Preserved existing menu action IDs, setting IDs, layered registry semantics, and shortcut profiles.
- Added native renderer, workspace, settings, keyboard, pointer, migration, desktop-smoke, X11-host,
  strict-compiler, and ASan/UBSan regression coverage.
- See `destructible_voxel_engine_menu_and_settings_format_v1.md`,
  `destructible_voxel_engine_v2_03_menu_options_ux_report.md`,
  `destructible_voxel_engine_v2_03_release_evidence.txt`, and
  `destructible_voxel_engine_v2_03_remaining_work_plan.md`.


## v2.02 Jolt production feature completion

- Added Jolt height-field terrain with bounded authored updates. DVE retains authoritative samples
  and rebuilds/replaces the compressed Jolt shape so edits may introduce new height extrema safely.
- Added mutable compound bodies with stable DVE child handles and add/modify/remove box operations.
- Added Jolt soft-body creation from DVE `SoftBodyAsset`, vertex state/control, impulses, pressure,
  damping, friction, sleeping, and snapshot topology participation.
- Added production CPU hair/rope strands using Jolt Cosserat rod constraints, including pinned roots.
  Jolt's separate GPU Hair/ComputeSystem path remains outside DVE until a compute-resource bridge exists.
- Added wheeled vehicles over `VehicleConstraint` with suspension, steering, brakes, handbrakes,
  engine/transmission settings, differentials, contact state, input, telemetry, and safe chassis lifetime.
- Added CPU debug-frame capture for shapes, wireframes, bounds, centers of mass, velocities,
  constraints/limits, soft-body vertices/constraints, and rods when debug-render support is compiled.
- Added versioned `.dvejoltreplay` files over complete solver snapshots with bounded parsing,
  simulation-frame indexing, topology checks, and persistent restore.
- Added Linux release/sanitizer validation plus Windows MSVC and macOS arm64 Clang Jolt presets.
  Those two platform presets are configuration support only and were not executed in this Linux run.
- See `destructible_voxel_engine_physics3d_jolt_format_v3.md`,
  `destructible_voxel_engine_v2_02_jolt_completion_report.md`,
  `destructible_voxel_engine_v2_02_release_evidence.txt`, and
  `destructible_voxel_engine_v2_02_remaining_work_plan.md`.

## v2.01 upstream Jolt 5.6 integration

- Verified and extended DVE against the supplied upstream Jolt 5.6.1 source rather than the prior
  5.5-oriented assumption. Source checkouts, packages, pinned fetches, and controlled legacy pairs
  now enforce the supported 5.5.x-5.6.x API range.
- Added explicit worker/substep/iteration, determinism, sleep, contact, and allocator tuning with
  runtime telemetry, and routed authored play-session settings through the neutral 3D factory.
- Added static triangle meshes, closest ray and sphere casts, caller-buffered AABB overlaps, broadphase
  optimization, full solver snapshots, and a Jolt `CharacterVirtual` bridge with optional inner body.
- Updated the editor backend selector to Automatic/Jolt/Box3D/Reference and added Jolt solver controls.
- Added `linux-gcc-jolt-release` and `linux-gcc-jolt-box-physics-release` presets.
- Fixed sanitizer compatibility by enabling matching Jolt RTTI when ASan/UBSan is active.
- Real upstream Jolt release, editor, snapshot, query, character, combined-backend, and sanitizer tests pass.
- See `destructible_voxel_engine_physics3d_jolt_format_v2.md`,
  `destructible_voxel_engine_v2_01_jolt_integration_report.md`,
  `destructible_voxel_engine_v2_01_release_evidence.txt`, and
  `destructible_voxel_engine_v2_01_remaining_work_plan.md`.

## v2.00 optional Box3D 3D physics

- Added optional upstream Box3D 0.1.x behind the same `IRigidBodyWorld` contract as Jolt and the
  reference adapter, with Automatic/Jolt/Box3D/Reference runtime selection.
- Added compound voxel boxes, authoritative mass/inertia, constraints, contacts, triangle meshes,
  material-aware ray casts, AABB overlaps, play-in-editor selection, and coexistence with Box2D.
- Jolt remains the first automatic 3D backend; Box3D is an explicit alternative and fallback.
- See `destructible_voxel_engine_physics3d_box3d_format_v1.md` and
  `destructible_voxel_engine_v2_00_box3d_integration_report.md`.

## v1.98 native slopes, platforms, queries, and chunk recooking

- Added authored 45-degree `SlopeUpRight` and `SlopeUpLeft` tile collision with diagonal ground
  normals, swept native AABB landing, slope-aware controller probes, and Box2D triangle cooking.
- Added native kinematic box platforms, platform-aware ray hits, surface/support velocity, and
  rider collision so the shared side-view controller can use moving supports without Box2D.
- Added backend-neutral `query_aabb`, `overlaps_tile_map`, `Physics2DTileRegion`, and
  `update_tile_map_region` APIs with explicit capability reporting.
- Added 16x16 Box2D collision chunks: solid rectangles and one-way runs merge within each chunk,
  slope tiles cook as convex triangles, and edited/destructible regions recook only affected chunks.
- Added live native collision-grid region updates and allocation-free body/collider AABB queries.
- Strict focused builds, integrated `dve_core`, four CTest targets, and ASan/UBSan checks pass.
- See `destructible_voxel_engine_v1_98_tile_physics_report.md`,
  `destructible_voxel_engine_v1_98_release_evidence.txt`, and
  `destructible_voxel_engine_v1_98_remaining_work_plan.md`.

## v1.97 side-view character controller

- Added a backend-neutral `CharacterController2D` over NativeTile or optional Box2D physics.
- Added ground/air acceleration and braking, coyote time, buffered and variable-height jumping,
  rise/fall gravity policies, terminal fall speed, facing state, and one-way drop-through.
- Added three-point ground probes, walkable-slope filtering, tangent motion, support-body tracking,
  moving-platform velocity inheritance, sensor-fed ladder climbing/centering, and explicit conveyor surface velocity.
- Added immediate knockback plus snapshot/restore boundaries for replay and rollback integration.
- Added `Physics2DWorld::set_body_gravity_scale` and the `dve.sideview_character` component schema.
- Strict focused builds, integrated `dve_core`, CTest, Box2D contract, and ASan/UBSan checks pass.
- See `destructible_voxel_engine_sideview_character_controller_format_v1.md`,
  `destructible_voxel_engine_v1_97_sideview_character_report.md`,
  `destructible_voxel_engine_v1_97_release_evidence.txt`, and
  `destructible_voxel_engine_v1_97_remaining_work_plan.md`.


## v1.99 upstream Box2D 3.2 validation and compatibility

- Added explicit Box2D 3.1.x/3.2.x version detection for installed packages, source checkouts,
  pinned fetches, and legacy library pairs.
- Added compile-time adaptation for Box2D 3.2 pre-solve callbacks and `b2MotionLocks`.
- Enabled sensor-event participation on visitor shapes, disabled 3.2 contact recycling for DVE
  bodies so one-way drop-through contacts are re-evaluated, and removed all locked-world Box2D API
  calls from pre-solve by caching body velocity/AABBs before each solver step.
- Added a real-upstream integration suite covering shapes, filters, sensors, motion locks, one-way
  platforms, slopes, ray/AABB queries, tile chunk recooking, impulses, and validation failures.
- Built and linked the complete DVE core against the supplied Box2D 3.2.0 source snapshot; the
  focused DVE and Box2D code paths also pass ASan/UBSan together.
- The pinned network fetch remains v3.1.1 for reproducible builds; source/package routes accept
  supported 3.1.x or 3.2.x versions.

## v1.96 optional Box2D sprite physics

- Added a backend-neutral `Physics2DWorld` contract so sprite, tile, flat-mesh, and 3D-in-a-2D-view
  games can use either the built-in deterministic tile solver or optional Box2D rigid-body physics.
- Added a Box2D 3.1.x/3.2.x adapter using the current C handle API, with pixel-to-meter conversion,
  static/kinematic/dynamic bodies, multiple colliders, boxes, circles, capsules, convex polygons,
  sensors, material/filter settings, buffered events, impulses, ray casts, and CCD body flags.
- Cooked canonical `.dvetilemap` collision into merged solid rectangles and merged one-way platform
  runs with upward pass-through and timed drop-through pre-solve handling.
- Added installed-package, version-checked source, explicit pinned-fetch, and controlled legacy
  dependency routes behind `DVE_ENABLE_BOX2D=ON`; incompatible Box2D 2.x and untested 3.3+ APIs are rejected.
- Added `dve.physics2d_world`, `dve.physics2d_body`, and repeatable `dve.physics2d_collider`
  component schemas plus strict, sanitizer, full-core, contract, and shipping-path tests.
- The native solver remains the default and remains available in builds that omit Box2D.
- See `destructible_voxel_engine_physics2d_box2d_format_v1.md`,
  `destructible_voxel_engine_v1_96_box2d_sprite_physics_report.md`,
  `destructible_voxel_engine_v1_96_box2d_release_evidence.txt`, and
  `destructible_voxel_engine_v1_96_remaining_work_plan.md`.

## v1.95 chiptune and tile-world foundation

- Integrated Claude's tracker/PSG and side-scroller additions without replacing the existing DVE
  audio, sprite, camera, asset, or build architecture.
- Reworked chiptune runtime instruments into fixed-capacity callback-safe state and added exact
  fractional tick scheduling, stereo panning, transpose/fine tune, pitch and duty envelopes,
  wavetables, filters, bit/sample-rate reduction, volume slide, note cut, retrigger, wave/duty/pan
  effects, and eight procedural game-SFX presets.
- Added a `DecodedAudioAsset` bridge plus a cooker that writes canonical `.dvechip`, PCM16 WAV,
  cooked `.dvesample`, individual SFX, or a complete preset bank.
- Integrated canonical `.dvetilemap` assets, layered parallax, tile flips, bounded parsing, merged
  collision grids, allocation-free swept AABB collision, solid/one-way platforms, intentional
  drop-through, and dead-zone/clamped camera support.
- Added expanded regression tests, strict warning-as-error checks, sanitizer coverage, updated
  format documents, reference assets, backlog, and next-work plan.
- This remains an original-game creation engine; no ROM execution, emulation, proprietary asset
  extraction, or copied commercial game content is included.
- See `destructible_voxel_engine_v1_95_chiptune_tilemap_report.md`,
  `destructible_voxel_engine_chiptune_format_v1.md`,
  `destructible_voxel_engine_tilemap_format_v1.md`, and
  `destructible_voxel_engine_v1_95_remaining_work_plan.md`.

## v1.94 sprite authoring foundation

- Added a reusable transactional sprite-authoring session over the canonical `.dvesprite` format,
  including bounded history, named undo/redo, revision state, saved-hash dirty tracking, and
  transactional open/save.
- Added grid and freeform RGBA8 slicing with bounds/byte validation, configurable origin, cell,
  spacing, rows/columns, frame naming, alpha threshold, transparent-cell policy, and stable order.
- Added transparent trimming that preserves untrimmed source dimensions, bottom-left source
  offsets, pivots, durations, and frame events.
- Added frame pivot/timing/event edits plus clip add/update/remove and timeline reordering with
  complete-document rollback after invalid operations.
- Added play/pause/seek preview sampling, exact paused selected-frame preview, and renderer-neutral
  packets consumed successfully by the production sprite RHI bridge.
- Focused, strict-warning, ASan/UBSan, sprite runtime/RHI, platform/RHI, shader, and ragdoll
  regressions pass. Native canvas widgets, decoder wiring, atlas packing, palettes, and onion-skin
  presentation remain follow-on work.
- See `destructible_voxel_engine_sprite_authoring_format_v1.md`,
  `destructible_voxel_engine_v1_94_sprite_authoring_report.md`, and
  `destructible_voxel_engine_v1_94_remaining_work_plan.md`.

## v1.93 production sprite shader path

- Added an explicit RHI vertex-buffer/attribute contract with format, location, offset, stride,
  input-rate, range, and duplicate-location validation in Null and Vulkan backends.
- Added production HLSL sprite shaders and Vulkan GLSL equivalents for position, UV, vertex tint,
  sampled texture, and all existing sprite blend pipelines.
- Added checked-in SPIR-V modules generated by glslang plus a bounded runtime loader that rejects
  missing, oversized, unaligned, truncated, and non-SPIR-V files before RHI submission.
- Added an offscreen Vulkan image oracle for a textured/tinted sprite and integer-fit letterbox,
  including optional PPM evidence output and a fail-closed `DVE_REQUIRE_VULKAN` mode.
- Null-RHI, sprite-runtime, platform/RHI, shader-contract, strict-warning, and Vulkan compilation
  checks pass. This environment has no compatible Vulkan device, so physical/device image
  execution remains an explicit acceptance gate rather than a claimed result.
- The original-game-only scope remains unchanged: no ROM execution, console emulation,
  proprietary asset extraction, or copying shipped games.
- See `destructible_voxel_engine_sprite_shader_format_v1.md`,
  `destructible_voxel_engine_v1_93_sprite_shader_report.md`, and
  `destructible_voxel_engine_v1_93_remaining_work_plan.md`.

## v1.92 logical sprite RHI bridge

- Added a public render-bridge consumer for v1.91 sprite packets with deterministic XY/XZ
  world-to-logical projection and a documented 36-byte GPU vertex upload contract.
- Added integer/fractional logical viewport composition, full-target letterbox clearing, clipped
  scissor calculation, high-DPI window-pointer mapping, and output-to-logical input rejection.
- Added dynamic indexed quad uploads, one draw per semantic batch, registered or visible-fallback
  texture bindings, nearest/linear samplers, and opaque/alpha/additive/multiply pipelines.
- Extended the RHI blend contract and Vulkan translation with multiply blending.
- Verified the command path in the Null RHI, compiled the Vulkan backend, and preserved the
  platform/RHI, sprite runtime, and live-ragdoll contracts.
- This checkpoint does not claim physical-GPU pixels; production shader modules, device image
  evidence, and the visual sprite editor remain follow-on work.
- See `destructible_voxel_engine_sprite_rhi_format_v1.md`,
  `destructible_voxel_engine_v1_92_sprite_rhi_bridge_report.md`, and
  `destructible_voxel_engine_v1_92_remaining_work_plan.md`.

## v1.91 original-game sprite and 2.5D foundation

- Added logical-resolution presentation with integer fit/fill, fractional fallback, centered
  letterboxing/cropping, reversible input mapping, and camera-relative XY/XZ pixel snapping.
- Added canonical, bounded, content-hashed `.dvesprite` assets for atlas frames, trim/pivot
  metadata, animation clips, loop/ping-pong modes, frame timing/events, sampling, materials,
  palette banks, and pixels-per-world-unit.
- Added deterministic sprite playback, owner-ordered events, flip/tint state, XY/XZ quad packets,
  semantic draw ordering, and contiguous texture/material/palette/blend batches.
- Added one shared sort contract for sprites, flat meshes, particles, and 3D models in a 2D view.
- Added editor asset classification/file routing and a `dve.sprite` component schema.
- The roadmap is explicitly for original game creation: it does not execute ROMs, emulate consoles,
  extract proprietary assets, or recreate shipped games.
- See `destructible_voxel_engine_sprite2d_format_v1.md`,
  `destructible_voxel_engine_v1_91_sprite2d_foundation_report.md`, and
  `destructible_voxel_engine_v1_91_remaining_work_plan.md`.

## v1.90 production rigid-body constraints

- Implemented the neutral ball, hinge, cone-twist, and fixed constraint API in the pinned Jolt Physics 5.5.0 backend.
- Preserved authored local anchors, axes, angular limits, swing cones, and reference rotations through Jolt local center-of-mass frames.
- Added stable Jolt constraint handles, explicit destruction, handle reuse, observable counts, and automatic cleanup when either connected body is destroyed.
- Exercised a live Jolt-backed ragdoll falling onto static collision, settling, blending into the skeletal pose, recovering through a get-up clip, and releasing every body and constraint.
- Raised the default Jolt temporary arena to match the advertised contact capacity, repaired pinned-source CMake validation, and made native Jolt smoke tests independent of image/benchmark dependencies.
- Verified the native Release path against Jolt 5.5.0 plus the reference `GameWorld` and ragdoll regressions.
- See `destructible_voxel_engine_v1_90_jolt_constraint_runtime_report.md`,
  `destructible_voxel_engine_v1_90_jolt_constraint_release_evidence.txt`, and
  `destructible_voxel_engine_v1_90_remaining_work_plan.md`.

## v1.89 soft bodies and deformers

- Added canonical, hashed, bounded, transactional `.dvesoft` assets for cloth, rope, vegetation, and tetrahedral deformable props.
- Added `DeformableRuntime`, which owns soft-body instances, smooth B-spline cloth proxies, live render packets, collision proxies, impulses, radial impulses, and pinned-vertex targets.
- Integrated deformables with `GameWorld` marker ownership, fixed-step simulation, enable/disable, generic gameplay impulses, destruction cleanup, geometry classification, raycasts, and sphere overlaps.
- Added surface topology for tetrahedral deformable boxes and independent-resolution smooth cloth render meshes with per-frame normals, bounds, hashes, and telemetry.
- Added `.dvesoft` editor routing and the `dve.soft_body` component schema.
- Verified optimized strict-warning, sanitizer, soft-body, B-spline, `GameWorld`, feature-disabled, and v1.81 animation/UI regression paths.
- See `destructible_voxel_engine_deformable_runtime_format_v1.md`,
  `destructible_voxel_engine_v1_89_deformable_runtime_report.md`, and
  `destructible_voxel_engine_v1_89_remaining_work_plan.md`.

## v1.88 gameplay UI renderer, input, localization, and assets

- Added deterministic, versioned `.dveui` persistence with bounded reads, content hashes, canonical output, transactional writes, editor classification, file-drop routing, and transactional hot reload.
- Added backend-neutral quad, text, image, progress, slider, and focus-ring render primitives with clipping, layer order, accessibility scale/contrast, and screen/world canvas metadata.
- Added mouse hit testing, hover, pointer capture, button/slider input, keyboard and controller action routing, UTF-8 text entry, focus traversal, modal scopes, and separate accessibility focus events.
- Added locale tables, fallback chains, placeholders, plural/select formatting, missing-key diagnostics, pseudo-localization, and runtime locale switching.
- Added the `dve.game_ui` component schema and focused serialization, localization, input, modal, rendering, corruption, and hot-reload verification.
- See `destructible_voxel_engine_gameplay_ui_hud_format_v2.md`,
  `destructible_voxel_engine_v1_88_gameplay_ui_runtime_report.md`, and
  `destructible_voxel_engine_v1_88_remaining_work_plan.md`.

## v1.87 live ragdoll constraints and animation recovery

- Added a backend-neutral rigid-body constraint contract for ball, hinge, cone-twist, and fixed joints, plus transactional constraint lifetime and a deterministic live reference solver.
- Added an object-owned `RagdollRuntime` that creates body/constraint sets from the current animated pose, applies impulses, blends per body, publishes physical poses, and detects deterministic settling.
- Added face-up/face-down classification, orientation-specific get-up clip selection, upright root alignment, captured-pose recovery blending, physics teardown, and animation ownership handoff.
- Integrated marker-based skeletal actors into `GameWorld`, including activation/recovery APIs, post-physics pose order, root-motion suppression while physical, and destruction cleanup.
- See `destructible_voxel_engine_ragdoll_runtime_format_v1.md`,
  `destructible_voxel_engine_v1_87_live_ragdoll_recovery_report.md`, and
  `destructible_voxel_engine_v1_87_remaining_work_plan.md`.

## v1.86 control rig inspector authoring

- Added validated atomic replacement of control/node record batches and viewport visual batches, with full graph/runtime validation and one undo record per inspector action.
- Added native inspector rows for control and node identity, type, space, references, visibility, limits, defaults, phase, enable state, weight, solver flags, iterations, and tolerance.
- Added common-value multi-selection editing with explicit `Multiple` presentation and all-or-nothing rollback.
- Added inline control/node renaming and case-insensitive exact or unique-substring bone/control reference lookup, plus cyclic reference buttons.
- Routed native text input, Enter, Backspace, Escape, tab switching, and selection changes through explicit inspector edit state.
- See `destructible_voxel_engine_control_rig_inspector_format_v1.md`,
  `destructible_voxel_engine_v1_86_control_rig_inspector_authoring_report.md`, and
  `destructible_voxel_engine_v1_86_remaining_work_plan.md`.

## v1.85 control rig asset workflow

- Added multi-document Control Rig tabs with active/dirty/recovered presentation, recent-document ordering, safe switching, and unsaved-close refusal.
- Added verified paired transactions for `.dverig` and `.dverigui`: both files stage, read back, commit together, and restore the prior pair after any partial commit failure.
- Added deterministic fault injection covering the runtime-committed/layout-failed boundary without changing either previous file.
- Added timed autosave pairs, newer-autosave detection, explicit recovered state, save promotion, and obsolete-autosave cleanup.
- Added asset-browser Enter/double-click routing for `.dverig` and `.dverigui`, sibling-skeleton discovery, tab de-duplication, and native tab painting.
- See `destructible_voxel_engine_v1_85_control_rig_asset_workflow_report.md` and
  `destructible_voxel_engine_v1_85_remaining_work_plan.md`.

## v1.84 native control rig editor

- Added a native, platform-neutral control-rig overlay painted through the same immediate-mode canvas used by the X11 and SDL editor hosts.
- Added graph cards, typed pins and links, comments, grid navigation, cursor-anchored zoom, panning, card dragging, marquee selection, context creation, validation badges, and keyboard editing.
- Added a native rig preview with control-shape lines, hover/picking, selection, three-axis transform gizmos, explicit pointer capture, preview updates, and one-record transform commits.
- Added a `Window > Control Rig Editor` action and `Ctrl+8` binding, with controller routing for pointer, wheel, keyboard, and resize events.
- Added a deterministic demo document and focused interaction, strict-warning, sanitizer, and v1.81-v1.83 regression coverage.
- See `destructible_voxel_engine_v1_84_native_control_rig_editor_report.md` and
  `destructible_voxel_engine_v1_84_remaining_work_plan.md`.

## v1.83 control rig authoring and debugging

- Added an undoable control-rig authoring session with stable control/node creation, removal safeguards, graph movement, comments, control defaults, viewport shapes, and bounded snapshot history.
- Added typed control/data and execution pins with direction/type checks, single-input enforcement, duplicate rejection, backward-phase rejection, cycle rollback, and stable topological compilation inside each solve phase.
- Added deterministic `.dverigui` graph-layout persistence tied to the runtime rig content hash, including node/control layout, links, comments, shapes, colors, sizes, and counters.
- Added viewport wire-shape generation and ray picking for cross, box, circle, sphere, and arrow controls.
- Added preserve-model control-space rebasing, FK control-to-bone matching, two-bone FK/IK matching, optional runtime node traces, and deterministic rig-to-animation baking.
- Classified `.dverigui` alongside `.dverig` as a text Animation asset.
- Added focused full-core and strict-warning verification. The authoring controller and renderer-neutral viewport primitives are native-editor-ready; platform canvas painting and direct pointer wiring remain follow-on work.
- See `destructible_voxel_engine_control_rig_authoring_format_v1.md`,
  `destructible_voxel_engine_v1_83_control_rig_authoring_report.md`, and
  `destructible_voxel_engine_v1_83_remaining_work_plan.md`.

## v1.82 control rig

- Added validated control-rig assets with stable control/node IDs, transform/translation/rotation controls, model/bone/control spaces, nested-space cycle rejection, and translation/rotation limits.
- Added deterministic Pre-Solve, Forward-Solve, and Post-Solve scheduling with stable authored ordering inside each phase.
- Added Set Bone, Copy Bone, Parent Constraint, Aim Constraint, analytic Two-Bone IK, and iterative FABRIK nodes with per-node enable state and blend weight.
- Added `ControlRigRuntime` for object-owned rig instances, name/ID control overrides, reset/enable operations, model-space control publication, failure diagnostics, and post-animation pose publication.
- Integrated control-rig evaluation into `GameWorld` before root-motion consumption and socket/attachment synchronization.
- Added deterministic, hashed, bounded, transactional `.dverig` persistence, Animation asset classification, and a `dve.control_rig` component schema.
- Verified the CPU reference with strict compiler warnings and full-core tests. No visual graph editor, GPU solver, production full-body solver, or physical-device claim is included.
- See `destructible_voxel_engine_control_rig_format_v1.md`,
  `destructible_voxel_engine_v1_82_control_rig_report.md`, and
  `destructible_voxel_engine_v1_82_remaining_work_plan.md`.

## v1.81 animation controllers, IK, root motion, ragdoll, and gameplay UI

- Added validated parameterized animation controllers with prioritized conditions, typed parameters, one-shot triggers, state timing, playback-speed control, and crossfade transitions.
- Added deterministic root translation/rotation extraction across looping clips, crossfade-aware motion blending, pose root removal, explicit consumption, and `GameWorld` object-transform application.
- Added a model-space analytic two-bone IK reference with pole targeting, weighted results, chain validation, and bounded optional stretching.
- Added validated ragdoll body/joint recipes, animation-to-simulation blend state, and an external-physics-body-to-local-pose bridge.
- Added a retained gameplay UI document/runtime with screen- and world-space canvases, absolute/row/column/overlay/list layout, clipping, data binding, focus navigation, slider/button events, accessibility metadata, and renderer-neutral draw commands.
- Added a ready-to-bind gameplay HUD document for health and interaction prompts, plus fixed-update publication through `GameWorld`.
- Added strict-warning and end-to-end CPU verification. This environment exposes no physical GPU; the release makes no GPU skinning, UI rasterization, device-input, or live ragdoll-constraint execution claim.
- See `destructible_voxel_engine_animation_controller_ik_ragdoll_format_v1.md`,
  `destructible_voxel_engine_gameplay_ui_hud_format_v1.md`, and
  `destructible_voxel_engine_v1_81_animation_gameplay_ui_report.md`.

## v1.80 skeletal animation foundation

- Added validated hierarchical skeletons with deterministic `.dveskeleton` serialization, bind poses, and named bone sockets.
- Added deterministic `.dveanim` clips with per-bone translation/rotation tracks, looping/clamped sampling, and ordered events.
- Added shortest-arc quaternion interpolation, two-pose blending, normalized multi-pose blending, and model-pose evaluation.
- Added a bounded four-influence CPU linear-blend skinning reference for positions and normals.
- Added `SkeletalAnimationRuntime` with clip registration, playback, crossfades, pose publication, and object-owned lifetime.
- Resolved named skeletal sockets through `GameWorld` attachments so attached props follow sampled bone poses.
- Added the `dve.skeletal_animator` component schema and Animation classification for `.dveskeleton`/`.dveanim` assets.
- Added focused deterministic, corruption, interpolation, skinning, crossfade, and socket-attachment tests.
- See `destructible_voxel_engine_skeletal_animation_format_v1.md`,
  `destructible_voxel_engine_v1_80_skeletal_animation_foundation_report.md`, and
  `destructible_voxel_engine_v1_80_remaining_work_plan.md`.

## v1.78 scene composition workflow

- Added schema-driven native component inspection and undoable add/remove/enable/reorder/typed-property operations.
- Added first-class normalized tags, groups, and numeric layers to editor scenes, Play-in-Editor, `GameWorld`, and Lua.
- Added ordered lifecycle events for spawn, enable/disable, overlap, destruction, component changes, and pool acquire/release.
- Added deterministic fixed-capacity runtime pools with stable recycled object IDs and deep-copied prototypes.
- Upgraded editor scenes to v5 and prefab manifests to v2 while retaining supported backward reads.
- Added bounded prefab variants, stale-source previews, transactional source propagation, conflict reporting, and explicit-override preservation.
- Extended `dve_prefab_tool` with typed variant creation and transactional instance source-update commands.
- Added Lua membership, enable-state, lifecycle, and pooling APIs in both full-header and ABI-fallback Lua configurations.
- Verified optimized Lua-enabled 7/7, optimized Lua-disabled 5/5, and GCC ASan/UBSan/leak 5/5 matrices.
- See `destructible_voxel_engine_v1_78_scene_composition_workflow_report.md`,
  `destructible_voxel_engine_scene_composition_workflow_format_v1.md`, and
  `destructible_voxel_engine_v1_78_remaining_work_plan.md`.

## v1.77 prefab, component, and attachment foundation

- Added open typed components with stable per-object component IDs, schema validation for known types, and serializable unknown/custom types.
- Added local/world attachment transforms with cycle rejection, preserve-world attach/detach, editor propagation, and runtime hard synchronization.
- Upgraded editor scenes to v4 while retaining supported v1-v3 reads. Scene v4 stores attachment metadata, components, prefab provenance, and overrides.
- Added deterministic `.dveprefab` assets, capture from selection, native Assets-panel instantiation, undo/redo, and the headless `dve_prefab_tool`.
- Published components and attachments into Play-in-Editor and `GameWorld`; Lua can find objects by component and add, inspect, modify, or remove typed components.
- Added hierarchy and inspector composition diagnostics plus prefab classification in the asset browser.
- Attachments are hard transform relationships rather than constraints. Nested prefabs, variants, automatic source propagation, property drawers, lifecycle dispatch, and pooling remain later work.
- See `destructible_voxel_engine_v1_77_prefab_component_attachment_report.md`,
  `destructible_voxel_engine_prefab_component_attachment_format_v1.md`, and
  `destructible_voxel_engine_v1_77_remaining_work_plan.md`.

## v1.76 content and asset browser

- Added a persistent project asset database with stable IDs, content generations, normalized tags, import provenance,
  dependency/reverse-reference data, health states, and deterministic ordering.
- Added search, kind/tag/generated/issue filters, sorting, stable external-move recovery, and fail-closed index loading.
- Added transactional reference-safe rename and move operations for text assets with rollback on rewrite failure.
- Added deterministic 64x64 PPM identification thumbnails and a headless `dve_asset_index` CLI.
- Integrated search, issue filtering, refresh, selection, reference counts, F2 rename, and thumbnail regeneration into
  the native Assets tab. Asset mutations remain blocked during Play or Simulate.
- Optimized focused tests pass 6/6. Browser-specific AddressSanitizer and UndefinedBehaviorSanitizer pass;
  LeakSanitizer cannot initialize in this container and is not claimed.
- See `destructible_voxel_engine_v1_76_content_asset_browser_report.md`,
  `destructible_voxel_engine_asset_browser_format_v1.md`, and
  `destructible_voxel_engine_v1_76_remaining_work_plan.md`.

## v1.75 Play-in-Editor session

- Replaced the editor's Play/Simulate mode placeholders with an isolated executable `GameWorld` session.
- Added deterministic fixed stepping, bounded catch-up, Pause, one-tick Step, Stop, and explicit Keep Runtime Changes.
- Snapshot and restore now preserve the full document, dirty state, selection set, primary selection, and editor camera.
- Added native input routing, gameplay-camera possession/ejection, Lua startup/log/input integration, and HUD overlay capture.
- Active sessions reject authored-content mutations while retaining safe camera and view operations.
- Failed runtime or Lua startup restores Edit mode and the entry snapshot before returning an error.
- Verified optimized Lua-enabled and Lua-disabled configurations plus AddressSanitizer, UndefinedBehaviorSanitizer,
  and leak detection using reference physics. Jolt and physical-GPU execution are not claimed.
- See `destructible_voxel_engine_v1_75_play_in_editor_session_report.md`,
  `destructible_voxel_engine_play_in_editor_session_format_v1.md`, and
  `destructible_voxel_engine_v1_75_remaining_work_plan.md`.

## v1.74 CPU surface layers and destruction authoring

- Added a reusable CPU material-surface compositor for paint, rust, dirt, wetness, snow, scorching,
  fracture exposure, and custom per-pixel layers.
- Added mask-authoritative height blending, reoriented normal mapping, perceptual-roughness blending,
  bounded metallic blending, additive emissive, and independent opacity policies.
- Extended `.dmesh` 1.3 with up to four serialized per-material mask/height bindings while retaining
  reads of older 1.x assets.
- Added deterministic destruction-surface assignment for interior, fracture-replacement, and
  fracture-overlay materials so newly exposed faces need not inherit the exterior finish.
- Added geometry-independent projected decal data, a deterministic CPU clustered index, and polygon
  reference-renderer composition for impacts, cracks, dirt, paint, wetness, fluid residue, and related overlays.
- Added bounded offline polygon subdivision and normal-direction vertex displacement with explicit
  disabled, visual-only, and collision-affecting policies. Voxel-derived displacement remains visual-only.
- Added generation-checked `TextureResidencyManager` references so hot reload invalidates stale CPU-side
  descriptor references after the replacement resource becomes usable.
- Expanded `dve_material_preview` to fourteen deterministic CPU views, including layer mask and
  height-adjusted layer coverage, and expanded editor capability reports.
- No live GPU layer, decal, displacement, or pass-wide residency migration claim is included.
- See `destructible_voxel_engine_v1_74_cpu_surface_layers_and_destruction_authoring_report.md`,
  `destructible_voxel_engine_cpu_surface_layers_and_destruction_authoring_format_v1.md`, and
  `destructible_voxel_engine_v1_74_remaining_work_plan.md`.

## v1.73 CPU material authoring diagnostics

- Added a deterministic CPU-only material analyzer for cooked polygon assets.
- Reports CPU-reference, live-polygon, and shadow-caster support separately instead of treating serialized
  material fields as proof of executable parity.
- Added bounded reference texture-sample estimates and stable diagnostics for UV1, triplanar, detail,
  parallax, transparency, tangents, color space, masking, and uniform material layers.
- Added `dve_material_audit` for text/JSON reports and strict CI checks without creating an RHI device.
- Added twelve diagnostic views to the CPU polygon renderer and `dve_material_preview` for deterministic
  PPM preview export plus a machine-readable material audit.
- No new GPU material execution, shader behavior, descriptor allocation, or physical-device claim is included.
- See `destructible_voxel_engine_v1_73_material_authoring_diagnostics_report.md`,
  `destructible_voxel_engine_material_authoring_report_format_v1.md`, and
  `destructible_voxel_engine_v1_73_remaining_work_plan.md`.

## v1.72 shared main/shadow material residency

- Added renderer-owned `MaterialResourceResidency` for cooked polygon-material images and samplers.
- Main and shadow descriptor tables now share authored base-color/opacity resources instead of uploading
  duplicate GPU textures, views, and samplers.
- Asset-level references keep resources valid while either table still owns material bind groups.
- Final table release evicts all shared authored resources for the asset; failed publication rolls back
  a newly acquired reference.
- Standalone material tables retain compatibility by creating private residency caches.
- Nine focused Null-RHI, SwiftShader Vulkan, shader-contract, and reflection-self tests pass.
- See `destructible_voxel_engine_v1_72_shared_material_residency_report.md`,
  `destructible_voxel_engine_material_resource_residency_format_v1.md`, and
  `destructible_voxel_engine_v1_72_remaining_work_plan.md`.

## v1.71 live material texture sampling

- The live material shader samples base-color, metallic/roughness, normal, emissive, and opacity channels.
- Added explicit base-color/emissive sRGB decoding and authored alpha-cutoff execution.
- Preserved the 176-byte CPU/HLSL material-mapping record ABI while publishing color-space flags.
- See `destructible_voxel_engine_v1_71_live_material_texture_sampling_report.md` and
  `destructible_voxel_engine_v1_71_remaining_work_plan.md`.

## v1.70 persistent main-pass material records

- Added a persistent main-pass material descriptor table for cooked polygon assets.
- Each asset publishes aligned `GpuMaterialRecord` and `GpuPolygonMaterialMappingRecord` storage buffers once.
- Each material caches an exact record-range descriptor plus all nine authored mapping texture channels.
- The live polygon material pass binds those persistent descriptors and reads real base color, emissive,
  metallic, roughness, specular, alpha, and mapping-transform values instead of fixed placeholders.
- Static/dynamic CSM views are folded into the material descriptor so the live pipeline remains within
  the portable four-descriptor-set floor.
- Null RHI and SwiftShader software Vulkan validate record readback, descriptor reuse, and live binding.
- v1.71 subsequently enabled live sampling of the five production base channels.
- See `destructible_voxel_engine_v1_70_production_material_records_report.md`,
  `destructible_voxel_engine_main_material_table_format_v1.md`, and
  `destructible_voxel_engine_v1_70_remaining_work_plan.md`.

## v1.69 layered static/dynamic cascaded shadows

- Static and dynamic caster depth now live in separate D32 atlases and are sampled together.
- Dirty rectangles are cleared inside depth-only render passes instead of clearing the entire atlas.
- Dynamic casters can move or disappear without leaving stale depth, while static geometry remains cached.
- Static and dynamic dirty-cascade sets can be scheduled independently.
- Null RHI and SwiftShader execute the same region-clear and preservation path.
- See `destructible_voxel_engine_v1_69_layered_shadow_atlas_report.md`,
  `destructible_voxel_engine_layered_shadow_atlas_format_v1.md`, and
  `destructible_voxel_engine_v1_69_remaining_work_plan.md`.

## v1.68 textured shadow casters and cascade preservation

- Alpha-masked CSM casters now bind and sample cooked base-color alpha and opacity textures.
- A persistent descriptor table deduplicates images and samplers and reuses bind groups across frames.
- Missing channels use neutral opaque-white fallbacks.
- Static casters can be preserved during explicit partial cascade updates; unsafe full-atlas clears are rejected.
- Dynamic stale-depth erasure and shared streaming residency remain explicit follow-on work.
- See `destructible_voxel_engine_v1_68_textured_shadow_casters_report.md`,
  `destructible_voxel_engine_shadow_material_table_format_v1.md`, and
  `destructible_voxel_engine_v1_68_remaining_work_plan.md`.

## v1.67 shadow pipeline hardening

- CSM caster passes are genuinely depth-only; the obsolete RGBA companion atlas is removed.
- Graphics pipelines may omit the fragment stage for opaque depth-only rendering.
- Live polygon draws publish separately aligned per-object and per-cascade constant ranges.
- Alpha-masked materials select a fragment-stage depth-only pipeline using material and vertex alpha.
- Materials whose masks depend on textures are counted as explicit fallback draws until per-mesh
  texture tables are bound in the caster pass.
- Optional compiled-shader evidence now compares SPIR-V reflection names, resource kinds, descriptor
  sets, and bindings against the checked-in shader manifest.
- SwiftShader validation is software Vulkan, not physical-GPU evidence.
- See `destructible_voxel_engine_v1_67_shadow_pipeline_hardening_report.md` and
  `destructible_voxel_engine_v1_67_remaining_work_plan.md`.

## v1.65 production environment lighting integration

- Added versioned `.dveibl` persistence for source radiance, diffuse irradiance, GGX specular mips, and the split-sum BRDF LUT.
- Added deterministic skybox sampling/rendering with yaw rotation, intensity, and exposure.
- Added RGBA16F cubemap/LUT publication through the shared RHI and a reusable three-texture environment bind group.
- Added comparison samplers to Null RHI and Vulkan and used them in the cascaded-shadow atlas binding.
- Added a live D32 atlas resource and per-cascade viewport/scissor/dirty-region frame plan.
- Added source-level primary-material IBL integration and an environment-skybox shader contract.
- No physical-GPU image or performance evidence is claimed.
- See `destructible_voxel_engine_v1_65_production_environment_lighting_report.md`, `destructible_voxel_engine_environment_lighting_asset_format_v1.md`, and `destructible_voxel_engine_v1_65_remaining_work_plan.md`.

## v1.64 environment lighting and cascaded-shadow foundation

- Added deterministic equirectangular-to-cubemap conversion and seam-consistent cube direction mapping.
- Added CPU/reference diffuse irradiance convolution, GGX specular prefilter mip generation, and split-sum BRDF LUT baking.
- Added bounded IBL evaluation for metallic/roughness materials and box-projected reflection-probe selection/blending.
- Added stable 1–4 cascade planning with practical split blending, texel snapping, atlas allocation, blend bands, PCF policy, and dirty-cascade intersection queries.
- Added shared HLSL helpers and source contracts for cubemap IBL and cascaded-shadow resolve.
- This checkpoint is a CPU/reference and shader-contract foundation; it does not claim a live production shadow atlas or physical-GPU image validation.
- See `destructible_voxel_engine_v1_64_environment_lighting_csm_report.md`, `destructible_voxel_engine_environment_lighting_format_v1.md`, and `destructible_voxel_engine_v1_64_remaining_work_plan.md`.

## v1.63 material mapping foundation

- Added UV0/UV1, world-triplanar, and object-triplanar coordinate modes with deterministic transforms and blend weights.
- Added detail base-color, normal, and roughness channels with independent strengths, tiled transforms, and camera-distance fading.
- Added one bounded height system with Off, Offset Parallax, Steep Parallax, and Parallax Occlusion quality modes.
- Extended `.dmesh` to minor version 2 with backward-compatible material mapping persistence and new height/detail texture bindings.
- Integrated mapping into the CPU polygon reference renderer, including axis-aware triplanar normal reconstruction and mapping telemetry.
- Added first-class RHI samplers, combined sampled-texture bindings, graphics bind groups, 2D-array/cube views, and Vulkan cube-compatible images.
- Added a byte-exact polygon material mapping GPU record plus shared HLSL transform/triplanar/detail functions; shader contracts now cover 33 HLSL sources.
- Verified sampler/descriptor/cube-view/graphics binding execution under SwiftShader software Vulkan; no physical-GPU image or performance claim is made.
- See `destructible_voxel_engine_v1_63_material_mapping_foundation_report.md`, `destructible_voxel_engine_material_mapping_format_v1.md`, and `destructible_voxel_engine_v1_63_remaining_work_plan.md`.

## v1.62 production-pass specialization conformance

- Added bounded source-linked specializations for Fluoddity signed-accumulator clearing, Gabor temporal resolve, and grid-fluid divergence.
- Added full int32/float32 CPU-versus-device array comparisons with explicit tolerances, source paths, source hashes, specialization descriptions, and specializer identity.
- Added `--require-production-readback` to both the native runner and external wrapper.
- Extended evidence schema v3 and `kernels.csv` with source/compiler/specialization provenance.
- Added Vulkan `timestampPeriod` reporting and converted nanosecond metrics while retaining raw ticks.
- Verified the selected Release matrix 14/14 and focused ASan/UBSan/leak checks under both Null RHI and SwiftShader.
- The embedded SPIR-V is emitted by DVE's deterministic specializer, not by DXC/glslang; this is not exact production-HLSL compiler parity.
- See `destructible_voxel_engine_v1_62_production_pass_specialization_report.md` and `destructible_voxel_engine_v1_62_remaining_work_plan.md`.

## v1.61 simulation kernel conformance and timestamp evidence

- Added four exact integer SPIR-V microkernels representing particle update, grid stencil, temporal accumulation, and signed atomic scatter dataflows.
- Added full-array CPU/device comparison records with mismatch counts, maximum error, CPU/device hashes, and explicit `algorithmic_kernel_readback` classification.
- Added kernel bytecode metadata and the `array_comparisons.csv` and `kernels.csv` evidence artifacts.
- Implemented Vulkan timestamp query pools, command resets, ordered writes, 64-bit readback, queue-family timestamp-valid-bit reporting, and raw per-kernel device-tick deltas.
- Added fail-closed `--require-kernel-readback` and `--require-timestamps` policies; Null-RHI host timestamps do not satisfy the device-timestamp requirement.
- Extended the external validation wrapper to forward and record both strict policies.
- Verified the new layer through SwiftShader as software Vulkan only; no production-shader or physical-GPU claim is made.
- See `destructible_voxel_engine_v1_61_simulation_kernel_conformance_report.md` and `destructible_voxel_engine_v1_61_remaining_work_plan.md`.

## v1.60 simulation validation and device runner

- Added optional `DVE_BUILD_DEVICE_TEST_RUNNER` and a headless `dve_device_test_runner` for Null or Vulkan device suites, simulation suites, deterministic evidence output, backend requirements, and strict physical-adapter requirements.
- Extended RHI capability reporting with adapter class, vendor/device identity, driver/API versions, queue family, native compute limits, device-local memory, software classification, and per-format storage/atomic support.
- Added executable storage-write/readback and signed-atomic Vulkan probes, while the Null backend remains explicitly contract-only.
- Added shared conformance fixtures for grid smoke, FLIP/APIC liquids, XPBD cloth, PBF liquids, VFX event accounting, and Fluoddity replay.
- Added transactional JSON/Markdown/CSV evidence bundles with embedded thresholds, deterministic run IDs, native artifact hashes, external SHA-256 inventories, and explicit evidence-class warnings.
- Added optional DXC/glslang/SPIRV-Tools/SPIRV-Cross/Metal compilation evidence and a one-command external hardware workflow with fail-closed `--require-physical` and `--require-shader-backend` policies.
- Corrected prior SwiftShader terminology: SwiftShader is software Vulkan, not physical-device validation.
- See `destructible_voxel_engine_v1_60_simulation_validation_device_runner_report.md`, `destructible_voxel_engine_simulation_validation_evidence_format_v1.md`, and `destructible_voxel_engine_v1_60_remaining_work_plan.md`.

## v1.59 FLIP/APIC liquid foundation

- Added optional `DVE_ENABLE_FLIP_LIQUIDS`, explicitly layered on `DVE_ENABLE_GRID_FLUIDS` while remaining independent of the PBF/XPBD liquid module.
- Added APIC particles with stable IDs and a full affine velocity matrix, trilinear staggered particle-to-grid deposition, PIC/FLIP blending, bounded affine reconstruction, and midpoint advection.
- Added explicit air/liquid/solid classification, a narrow-band particle level set, free-surface matrix-free PCG pressure projection, no-through static obstacles, and bounded velocity extrapolation into air.
- Added deterministic per-cell reseeding, overpopulation removal, global/per-cell budgets, level-set surface snapshots, phase/velocity sampling, and detailed pressure/divergence/volume telemetry.
- Added a sixteen-stage GPU frame plan and common-RHI recording contract covering transfer, classification, pressure, projection, extrapolation, particle maintenance, and surface output.
- Verified the focused reference path under Release, AddressSanitizer, and UndefinedBehaviorSanitizer, and passed the selected FLIP/APIC, smoke/fire, PBF, VFX, and common GPU-recorder matrix.
- See `destructible_voxel_engine_v1_59_flip_apic_liquid_foundation_report.md`, `destructible_voxel_engine_flip_apic_liquid_format_v1.md`, and `destructible_voxel_engine_v1_59_remaining_work_plan.md`.

## v1.58 native grid fluid foundation

- Added optional `DVE_ENABLE_GRID_FLUIDS` with staggered MAC velocity fields, cell-centered smoke/fire fields, adaptive stepping, and deterministic bounded validation.
- Added midpoint semi-Lagrangian and clamped MacCormack advection, gravity/buoyancy, combustion, dissipation, vorticity confinement, cell-aligned obstacles, and no-through boundaries.
- Added matrix-free diagonally preconditioned conjugate-gradient pressure projection with warm starts, residual/divergence telemetry, and finite-state repair accounting.
- Added sphere emitters, PIC particle deposition, PIC/FLIP blended particle updates, and a renderer-facing density/temperature/emission/fuel volume snapshot.
- Added a complete GPU frame plan, common-RHI dispatch recorder, and seven HLSL stages for advection, combustion, forces, divergence, pressure iteration, and projection.
- Retained Mantaflow Apache-2.0 attribution without importing its Python scenes, source-generation preprocessor, GUI, Blender integration, examples, or optional dependency stack.
- See `destructible_voxel_engine_v1_58_grid_fluid_foundation_report.md`, `destructible_voxel_engine_grid_fluid_format_v1.md`, and `destructible_voxel_engine_mantaflow_research_review.md`.

## v1.57 B-spline cloth foundation

- Added optional `DVE_ENABLE_BSPLINE_CLOTH` with a dependency-free quadratic open-uniform tensor B-spline surface implementation.
- Added a runtime proxy that converts regular DVE cloth control grids into smooth C1 surfaces and independent-resolution embedded render/collision meshes.
- Added first/second surface derivatives, normals, rest-map inverse derivatives, deterministic content/support hashes, and parametric seam evaluation.
- Added full 3x3-per-span reference quadrature and the BS-Cloth split reduced scheme: dense boundary membrane integration, alternating dual-grid 2x1/1x2 interior integration, boundary bending centers, and interior dual-vertex bending points.
- Added membrane/stretch/shear energy evaluation, quadratic bending energy, precomputable control-independent bending stencils, control-to-quadrature incidence maps, and lumped mass construction.
- Retained the upstream Apache-2.0 license and research attribution without embedding its renderer, YAML application, asset corpus, MKL/CHOLMOD/TBB stack, or IPC/Newton solver.
- Verified the B-spline module alongside soft bodies, simulation quality, the energy compiler, VFX, and PBF; a separate minimal build passes with all simulation modules disabled.
- See `destructible_voxel_engine_v1_57_bspline_cloth_foundation_report.md`, `destructible_voxel_engine_bspline_cloth_format_v1.md`, and `destructible_voxel_engine_bs_cloth_research_review.md`.

## v1.56 simulation quality and energy compiler

- Added a shared displacement/CFL-derived adaptive step planner used by Fluoddity, soft bodies, and PBF liquids.
- Added trilinear Fluoddity trail sampling and bounded adaptive reference stepping.
- Replaced generic cloth bend-distance behavior with explicit shared-edge dihedral-shell constraints, persistent XPBD multipliers, pressure, compliant unilateral contact, multiplier decay, and friction.
- Stabilized PBF with neighbor caps/overflow telemetry, optional per-iteration rebuilds, correction clamps, normalized viscosity, vorticity confinement, and a future ST-FLIP temporal-sampling contract.
- Expanded VFX with point attractors, velocity limits, sphere collision, spawn/death/collision events, bounded event overflow, and a GPU event-build pass.
- Added optional `DVE_ENABLE_SIMULATION_ENERGY_COMPILER`: deterministic scalar DAGs, analytic energy/gradient/Hessian evaluation, fixed/free/affine parameterizations, joined relation compilation, block-incidence inference, and HLSL value emission.
- Retained the supplied YASPS MIT notice while avoiding Python, PyCUDA, NVCC, CUDA, or NVIDIA runtime requirements.
- Verified an 18-test enabled matrix and a separate 2-test build with all optional simulation modules disabled.
- See `destructible_voxel_engine_v1_56_simulation_quality_energy_compiler_report.md`, `destructible_voxel_engine_siggraph_2026_simulation_research_review.md`, and `destructible_voxel_engine_simulation_energy_compiler_format_v1.md`.

## v1.55 simulation foundations

- Added a bounded Fluoddity CPU oracle, portable fixed-point deposit/resolve/diffusion, three HLSL stages, packed GPU constants, and ordered RHI dispatch recording.
- Added cloth, rope, vegetation, and deformable-prop assets with an XPBD reference runtime and Jolt-facing conversion recipe.
- Added a native authored VFX module graph, deterministic compiler, CPU preview runtime, and GPU frame planner.
- Added a bounded PBF/XPBD liquid prototype and complete planned GPU pass sequence.
- Added independent feature switches and focused verification. Nine relevant tests and 25 shader source contracts pass.
- See `destructible_voxel_engine_v1_55_simulation_foundations_report.md` for exact evidence and limitations.

## v1.54 physical compute foundation

- Added descriptor-backed Vulkan compute pipeline layouts, compute pipelines, bind groups, command
  recording, descriptor-set binding, dispatch, submission, and dispatch telemetry.
- Added `bind_compute_bind_group` to the common RHI and matching deterministic validation in the Null
  backend, including queue, render-pass, required-set, and exact-layout checks.
- Added explicit compute pipeline bind-group-layout contracts so resource compatibility is validated
  before execution rather than inferred from shader convention.
- Added `R32Sint` and `RGBA32Sint` formats plus per-format sampled, storage, atomic, render-target, and
  depth/stencil capability queries. Vulkan capabilities come from physical-device format properties.
- Added Vulkan descriptor support for uniform buffers, read-only/read-write storage buffers, and
  storage textures. Sampled textures remain intentionally unsupported until the RHI has an explicit
  sampler contract.
- Added a physical Vulkan compute regression that writes and reads a storage buffer, binds a signed
  3D storage volume, verifies `R32Sint` storage-image atomic capability, and performs 32 signed atomic
  additions through a flattened fixed-point accumulator.
- Verified existing Fluoddity, runtime Fluoddity, Gabor runtime, AI/MCP, platform RHI, Vulkan buffer,
  Vulkan graphics, and Vulkan camera-output tests alongside the new compute test.
- Software Vulkan execution used SwiftShader. Discrete/integrated physical GPUs, shader reflection,
  push constants, timestamps, sampled compute textures, and complete Fluoddity/Gabor production
  shaders remain open work.

## v1.53 Fluoddity native simulation foundation

- Added optional `DVE_ENABLE_FLUODDITY` native C++23 support without Python, OpenGL, CUDA,
  OptiX, or NVIDIA-specific runtime dependencies.
- Added version-7 JSON preset migration and canonical `.dfluoddity` assets preserving the structured
  120-float Fourier rule, sweep ranges, jitter, cohorts, float rule seeds, boundary/initial modes,
  appearance metadata, notes, and auditable legacy fields.
- Added source-compatible CPU Fourier evaluation, rule generation/mutation, deterministic semantic
  hashes, transactional little-endian serialization, strict validation, and post-write verification.
- Added low/medium/high/custom quality profiles, portable signed fixed-point accumulation contracts,
  and exact particle/trail/accumulator/support memory estimates.
- Added `RuntimeFluoddityWorld` with content-addressed assets, play/pause/step/reset, bounded fixed-step
  planning, trail ping-pong state, dispatch sizing, render/interaction modes, and global GPU-budget
  rejection.
- Added `dve_cook_fluoddity`, approval-bound `dve.project.cook_fluoddity`, and allowlisted Fluoddity
  build/test tasks.
- Cooked and bundled all 151 supplied presets. Repeated and cross-root cooks produce identical
  SHA-256 inventories: 151 imported, 151 verified writes, 71 legacy-metadata warnings, zero errors.
- Retained source-compatible trail semantics: signed velocity in RGB and zero alpha. Density-alpha is
  represented as an explicit DVE extension rather than silently changing imported behavior.
- This is an asset/runtime-planning foundation. Physical compute execution, simulation shaders,
  editor field objects, rendering, collision fields, full checkpoints, and networking remain future
  work dependent on the v1.52 physical-compute program.

## v1.51 Gabor volume production integration

- Added bounded Gabor/Gaussian PLY import, pyramid merge, canonical `.dgabor` assets, legacy migration,
  deterministic hashes, strict malformed-input rejection, and deterministic emission-absorption
  previews.
- Added first-class editor Gabor-volume objects, project-local cooking/import, viewport bounds and
  picking, scene save/load, cloning, undoable material edits, and a native inspector.
- Added Create and Rendering menu options for rendering mode, quality, continuous level of detail,
  temporal accumulation, scene/volume shadows, primitive budgets, and LOD bias.
- Added a runtime volume world, content-addressed assets, transactional GPU resource replacement,
  bounded tile/dispatch planning, hybrid depth composition contracts, temporal-history resets, and
  volume-shadow scheduling.
- Added `dve_cook_gabor`, the approval-bound `dve.project.cook_gabor` MCP operation, and allowlisted
  Gabor build/test tasks.
- The shader source-contract manifest now validates 22 shaders, including tile construction,
  integration, temporal resolve, volume shadows, and composition.
- Retained the supplied Gabor Fields MIT license and reference notice. The research project’s custom
  Mitsuba, DrJIT, CUDA, training, and path-tracing stack is not a runtime dependency.
- Physical Vulkan/D3D12/Metal execution, optimized projected-ellipsoid tile binning, temporal image
  tuning, and physically validated multiple scattering remain follow-on work.

## v1.50 production 3D text

- Added a runtime 3D-text world that owns live `.dtext` instances with stable object identity,
  transforms, visibility, selection, shadow participation, and GI participation.
- Added transactional RHI residency for Slug `RGBA16F` curve atlases, `RG16_UINT` band atlases,
  analytic face geometry, and standard PBR extrusion-side geometry.
- Added ordered frame recording for back analytic faces, extrusion sides, front analytic faces, and
  selection overlays. Per-object constants can carry transforms, camera matrices, object/material
  IDs, motion history, and lighting data.
- Added native editor creation and inspection for text, project-local font path, size, extrusion,
  spacing, alignment, fill rule, and separate face/side materials. Recooking is transactional and
  participates in undo/redo and scene save/load.
- Added viewport bounds, picking, framing, selection outlines, and a software-editor proxy driven by
  the same `.dtext` asset. The proxy is an editor fallback; the production face contract remains the
  analytic Slug shader path.
- Added explicit font/license dependency collection for packaging without embedding the source font
  in `.dtext`.
- Physical Vulkan/D3D12/Metal image evidence, complex OpenType shaping, CFF/CFF2, and variable fonts
  remain follow-on work.

## v1.49 Slug-based 3D text foundation

- Added a bounded glyf-based TrueType cooker that maps Unicode cmap entries, horizontal metrics, legacy kerning, simple outlines, and common composite transforms into quadratic Bézier contours.
- Added Slug-compatible curve and band textures, front/back glyph render packets, and adapted vertex/pixel shaders derived from the supplied open-source Slug reference release.
- Added standard-material extrusion side geometry, bounds, material identity, deterministic CPU preview rendering, and a versioned `.dtext` asset format.
- Added `dve_cook_text3d` plus the approval-bound `dve.project.cook_text3d` MCP/assistant tool, so external assistants can cook project-local fonts into reviewable 3D-text assets without shell access.
- Added bundled Slug attribution and the original dual MIT/Apache-2.0 reference shaders. No font file is redistributed by this checkpoint.
- Current shaping is intentionally bounded to Unicode cmap lookup, advances, line layout, and legacy `kern`; CFF/CFF2 outlines, OpenType GSUB/GPOS shaping, bidirectional layout, variable-font axes, and physical GPU execution remain follow-on work.

## v1.48 authenticated UDP transport and multiplayer services

- Added a real nonblocking UDP backend behind `IReplicationTransport`, with IPv4 loopback/LAN endpoints, bounded message and reassembly limits, reliable ordered fragmentation/ACK/retransmission, and unreliable sequenced snapshots.
- Added pre-shared-key peer authentication using HMAC-SHA-256, fresh client/server nonces, derived per-session keys, session IDs, constant-time tag comparison, authenticated endpoint learning, handshake retries/timeouts, and replay-resistant recent-nonce tracking. Payload confidentiality is not claimed.
- Added server-owned input validation for possession, tick windows, monotonicity, movement magnitude, per-tick command rate, and jump rate, plus a helper that applies only accepted commands to the bound live character.
- Added timestamped remote-character snapshot buffers with delayed interpolation, bounded extrapolation, stale-snapshot rejection, and teleport snapping.
- Added hash-verified late-join checkpoint manifests, bounded chunk streaming over the reliable channel, progress reporting, and post-baseline delta buffering.
- Added SHA-256/HMAC known-vector tests, wrong-key handshake rejection, real localhost fragmented transfer, late-join reconstruction, focused sanitizer coverage, and a reproducible v1.48 demonstration.


## v1.47 prediction execution, authoritative repair, and transport abstraction

- `ClientGameplayPrediction` now executes local fixed-tick inputs through the production character controller, compares the recorded result with authority, restores divergent authoritative state, and replays only the unacknowledged input tail. Matching predictions only trim history.
- `GameplayRuntime` exposes targeted prediction/reconciliation operations that do not advance unrelated players, timers, physics steps, or trigger callbacks. Trigger authority remains server-owned.
- Brick-repair responses carry one complete 8x8x8 material brick, its authoritative revision, hash, object identity, coordinate, and causal edit sequence. The 560-byte codec rejects truncation, trailing data, and hash mismatches.
- Repair application rebuilds static or dynamic collision transactionally. The old collision remains active if replacement creation fails; dynamic velocity is preserved. Final-voxel removal remains an object-despawn operation.
- Gameplay networking now targets the socket-neutral `IReplicationTransport` interface. `ReplicationTransportEndpoint` binds local peer identity, and the deterministic simulator implements the same contract used by future UDP, ENet, Steam, console, or QUIC backends.
- This checkpoint does not claim a real socket backend, authentication/encryption, remote interpolation, or matchmaking.


## v1.46 exact polygon collision and deterministic network simulation

- Polygon character collision now uses exact segment-to-triangle capsule distance through the existing BVH rather than sampled spheres. Continuous sweeps use bounded conservative advancement and binary refinement, return stable material/triangle identity, and support exact polygon depenetration.
- The playable controller is regression-tested on a polygon floor, including grounding, support identity, and horizontal traversal. Dynamic transforms remain sourced from the active rigid-body backend.
- Gameplay replication packets are version 2 and carry the exact brick coordinate certified by each destruction revision/hash. Version 1 packets remain readable and default the absent coordinate to zero.
- `DeterministicReplicationTransport` models datagrams, bounded bandwidth, latency, loss, duplication, reordering, fragmentation, ACK loss, retransmission, reliable ordered channels, and unreliable sequenced snapshots. It is a deterministic validation harness, not a production socket stack.
- Large late-join checkpoints converge under loss; stale snapshots are discarded; destruction divergence emits a bounded selective brick-repair request.
- `ClientPredictionBuffer` retains a bounded fixed-tick input/prediction history and produces an authoritative correction decision plus the exact unacknowledged input tail to replay through `GameplayRuntime`.
- Jolt 5.5 exposes the required narrow-phase capsule shape-cast primitives, but the pinned dependency was not available locally; this release does not claim a compiled or physical Jolt character-adapter result.

## v1.45 playable-runtime foundation

- Added a fixed-step capsule character controller over the existing voxel, polygon-BVH, and rigid-body query contracts. It includes acceleration/braking, wall sliding, step traversal, slope checks, ground snapping, coyote time, jump buffering, crouch clearance, ceiling handling, depenetration, moving-platform inheritance, and destructible-support recovery.
- Added explicit player/pawn ownership with exclusive possession and local/remote player identity.
- Added deterministic box and sphere trigger volumes with tag/character filters, Enter/Stay/Exit callbacks, one-shot behavior, and serialized enabled/fired state.
- Added fixed-tick character input recording and deterministic replay for regression, bug reproduction, and future client-prediction comparisons.
- Added Lua character, player, trigger, input, state, and trigger-callback bindings.
- Added a versioned gameplay replication-state contract with stable network identities, quantized character/support state, trigger state, ordered causal destruction edits, brick revisions, content hashes, strict decoding, and stable frame hashes. This freezes the representation but does not claim an online transport.
- Added bounded AI build/test tasks and a reproducible playable-runtime demonstration.
- Focused normal and Clang AddressSanitizer/UndefinedBehaviorSanitizer suites cover controller movement, jump timing, crouch clearance, moving supports, trigger lifecycle, floor destruction, replay determinism, identity collisions, and binary packet validation.

## v1.44 lighting defaults

- Global illumination is enabled in every new `RenderEnvironment` through the bounded
  `VoxelOneBounce` mode. The default uses four diffuse hemisphere rays per visible voxel hit, a
  12 m bounce distance, and 0.65 intensity. `AmbientHemisphere` and `Off` remain available for
  lower-cost and diagnostic profiles.
- Directional shadows now expose `Off`, `Hard`, `Soft`, `Contact`, and `Hybrid` modes. New projects
  use four-sample soft-area shadows; Hybrid combines those area samples with a short high-detail
  contact ray. Strength, angular softness, maximum distance, contact distance, and bias are validated
  shared environment properties.
- The CPU voxel reference renderer executes the selected GI and shadow modes and reports bounded ray
  and hit counts. A backend-independent lighting frame plan orders generation, tracing, resolution,
  and final shading for the production HLSL path.
- `Engine/Materials/StandardSurface` now carries the explicit low-reflectivity preset: roughness 0.62
  and specular 0.25, corresponding to approximately 2% dielectric normal-incidence reflectance. The
  Assets panel marks this range as `REFL LOW`.
- GPU environment packing and `render_environment.hlsli` share a validated 144-byte ABI. The shader
  manifest now inventories 15 shaders, including GI and shadow generators/resolvers.
- Lua and editor settings can select GI/shadow modes without introducing separate renderer-only
  defaults. Physical Vulkan/D3D12/Metal compilation and image evidence remain backend validation work.

## v1.43 new-project defaults

- A desktop editor launched without an existing document now opens an `Untitled Project` containing
  one selected and framed voxel ellipsoid named `Starter Oval`. The template contains 1,008 occupied
  voxels at 0.25 m voxel size and rests on the existing editor grid.
- Added immutable `Engine/Materials/StandardSurface` defaults shared across editor materials, master
  materials, and the GPU missing-material record: neutral 0.5 sRGB gray (0.214 linear), metallic 0,
  roughness 0.55, specular 0.5, opaque Standard PBR.
- New-project painting starts on Standard Surface. New Project and New Scene now have distinct
  destructive-confirmation paths.
- Added neutral studio environment defaults with a soft warm key, cool sky fill, subdued ground, ACES
  tonemapping, and restrained bloom. The existing editor floor/grid remains the non-object reference
  surface so the template stays a one-object scene.
- Added the unlit `Engine/Shaders/TexturePreview` shader contract and semantic preview settings for
  sRGB color, linear data, tangent normals, alpha checkerboards, individual channels/false color, HDR
  exposure, explicit mip/array selection, and UV transforms.
- Added deterministic missing-resource textures: magenta/black color checker, flat tangent normal,
  white AO/opacity, black emissive, and neutral metallic/roughness data.
- The shader source-contract manifest now inventories 12 shaders. A compiled DXIL/SPIR-V texture
  preview and a dedicated texture-inspector panel remain backend/UI integration work.


## v1.42 deferred UX and rendering additions

- Unified top-level menu popup geometry between rendering and input. Added hover switching, Alt
  mnemonics, arrow/Home/End traversal, Enter/Space activation, Escape, wheel scrolling, selected-row
  highlights, compact minimum-width labels, and window-clamped top-level/context menus.
- Added stable object/material identity to camera collision hits and frame-local renderer-facing
  occluder-fade requests for the `FadeOccluders` strategy.
- Added transactional replace-if-changed texture uploads for editor hot reload. Unchanged content is
  a no-op; changed resident textures replace the old resource only after successful upload,
  transition, and view creation.
- Added linear-light color mip filtering and decoded/renormalized normal-map mip filtering.
- Added Assets-panel shading/blend badges and semantic material warnings for contradictory or
  ineffective transparency, emissive, subsurface, foliage, and clear-coat settings.
- Added an always-available Python shader contract validator for shader inventory, includes, entry
  points, thread groups, resource bindings, binding collisions, GPU material ABI, shading-model
  values, and material-parameter-collection capacities. Optional glslang compilation remains active
  when installed.
- Regenerated the shader manifest after detecting incorrect bloom/tonemap thread groups and missing
  resource-binding records.
- Related camera, editor, texture, polygon, and material suites pass normally and under focused
  AddressSanitizer/UndefinedBehaviorSanitizer runs. Physical GPU and platform-device validation
  remains external work.

## v1.41 live-editor MCP additions

- Added a private MCP host owned by the running `NativeEditorController`. External MCP requests now
  inspect and propose changes to the actual open editor document rather than a separate reference
  `GameWorld` process.
- Added `dve_live_editor_mcp_proxy`, a newline-delimited stdio MCP proxy for Claude Code and other
  local MCP clients. The proxy discovers the editor by canonical project root and forwards requests
  over authenticated owner-only local IPC.
- Marshalled all tools and resources through a bounded queue executed by
  `NativeEditorController::update()`. Socket threads perform framing and authentication only and
  never access the editor document directly.
- Added **Window > Live MCP Host** plus `--live-mcp`, `--live-mcp-read-only`, `--project-root`, and
  `--live-mcp-runtime-dir` desktop options.
- Unified external MCP proposals with the editor Assistant review queue. Approvals remain bound to
  the exact tool, canonical arguments, and originating MCP session; another connection cannot
  consume them.
- Added owner-only runtime descriptors and Unix-domain sockets, Linux peer-credential checks,
  random per-instance tokens, bounded messages/clients/queues/deadlines, lifecycle cleanup, and a
  non-secret `dve://editor/live-mcp` status resource.
- Added protocol lifecycle checks for initialization order and the supported MCP version, live
  document/undo integration tests, stdio proxy tests, X11 menu smoke evidence, and focused
  AddressSanitizer/UndefinedBehaviorSanitizer coverage of the IPC host and proxy.
- Unix-like IPC is implemented and tested. Windows named pipes and physical macOS validation remain
  follow-on work.

### Live editor MCP quick start

```bash
cmake --build build --target dve_native_editor_x11 dve_live_editor_mcp_proxy
./dve_native_editor_x11 --project-root /absolute/path/to/project --live-mcp
```

Configure the external MCP client to launch the proxy, not the standalone `dve_mcp_server`:

```json
{
  "mcpServers": {
    "dve-live-editor": {
      "command": "/absolute/path/to/dve_live_editor_mcp_proxy",
      "args": ["--project-root", "/absolute/path/to/project"]
    }
  }
}
```

See `docs/live_editor_mcp_v1_41.md` and
`examples/dve_live_editor_mcp_stdio_config.json` for the complete workflow and security boundary.

## v1.40 closed-loop AI engineering additions

- Moved editor assistant requests onto the existing bounded worker-task system so network calls no
  longer block the editor update thread. Added completion polling, cancellation state, deadlines,
  bounded retries, model reporting, and token-usage telemetry.
- Added exact-base-hash unified-diff preview and transactional multi-file application. Every
  successful transaction writes a rollback bundle; rollback verifies that patched files were not
  independently modified before restoring byte-identical originals.
- Added fixed-name validation tasks. Tool callers select an allowlisted task name and cannot supply
  an executable, shell text, arguments, working directory, or environment variables.
- Bound approvals to the canonical tool name, exact canonical JSON arguments, and a unique local
  client/session actor. An approval ID cannot authorize altered arguments or another client.
- Added MCP resources for named validation tasks, pending change proposals, editor context, editor
  settings, and the AI audit history.
- Added deterministic release-manifest generation and verification, replacing the stale mixed
  manifest/checksum metadata present in the v1.39 archive.
- Focused AI, GameWorld, and editor regression suites pass in the dependency-minimal Linux sandbox
  configuration. Clang AddressSanitizer/UndefinedBehaviorSanitizer runs pass with leak detection
  disabled because LeakSanitizer itself faults in this container. Physical GPU, Windows task
  execution, live API, and authenticated remote-tunnel validation remain external work.

## v1.39 AI assistant, MCP, and OpenAI API additions

- Added a reusable, typed AI tool registry shared by the native editor, the local MCP server, and
  the OpenAI Responses API client.
- Added a native AI Assistant panel with message history, approval cards, Approve/Deny controls,
  and automatic continuation after a decision.
- Added undoable editor-scene tools for inspection, selection, creation, naming, transforms,
  deletion, undo, and redo, alongside project-file and runtime-scene tools.
- Added a JSON-RPC MCP server with stdio transport, tools/resources/prompts discovery, and a
  loopback Streamable-HTTP adapter intended for an authenticated tunnel or TLS reverse proxy.
- Added a Responses API client using function tools and `function_call_output`, defaulting to
  `gpt-5.6` and loading `OPENAI_API_KEY` from the process environment.
- Added project-root path confinement, source-file allowlists, size limits, hash-guarded atomic
  writes, exact-argument approval replay, destructive-action confirmation, and JSONL audit logs.
- No shell or arbitrary-network tool is exposed. API credentials are never written to project
  files or audit logs.
- Full HYBRID Lua/editor/audio/Vulkan suite passes 48/48; focused AI core and GameWorld suites
  pass under AddressSanitizer and UndefinedBehaviorSanitizer.

### Quick start

```bash
export OPENAI_API_KEY=...
cmake --build build --target dve_native_editor dve_mcp_server
```

The native editor opens the assistant from **Window > AI Assistant**. For a local MCP client,
run `dve_mcp_server --project-root /path/to/project`. For loopback HTTP, set
`DVE_MCP_BEARER_TOKEN` and use `scripts/run_dve_mcp_loopback.sh`; the token is not placed in process
arguments. For remote access, put the adapter behind an authenticated TLS boundary and do not expose
the loopback bridge directly to the public internet.


## v1.38 shortcut and input additions

- Added a typed editor command registry and context-aware shortcut resolver covering Global,
  Viewport, Fly Navigation, Play Mode, Camera, Sequencer, Audio, Voxel, Polygon, Material,
  Asset Browser, Text Field, and Modal contexts.
- Added two bindings per command; keyboard, four-button mouse, and wheel gestures; press, hold,
  and double-click activation; continuous RMB+WASDQE fly navigation; Mouse4 navigation/sampling;
  and double-LMB framing.
- Added DVE, Unity-familiar, Unreal-familiar, one-handed accessibility, and blank profiles with
  transactional `.dveshortcuts` import/export and conflict validation.
- Added a searchable native shortcut editor with profile/context filters, capture, reset, unbind,
  menu-label synchronization, and command/key/category search.
- Full HYBRID Lua/editor/audio/Vulkan suite passes 47/47; four focused shortcut/editor/audio suites
  pass under AddressSanitizer and UndefinedBehaviorSanitizer.

## v1.36 runtime camera and sequencer additions

- `GameWorld` now owns multi-viewport camera runtime state, with independent camera directors,
  channel masks, split-screen rectangles, named render targets, live object target bindings,
  post-process profiles, camera matrices, and temporal-history reset generations.
- Added `.dvecamseq` camera timelines with deterministic dolly curves, physical lens/focus keys,
  shots, blends, markers, state triggers, scrubbing, looping, and runtime playback.
- Added a bounded world collision adapter covering voxel and polygon gameplay geometry.
- Added physical depth-of-field range calculations, aspect/safe-frame guides, frustum visibility,
  and projected-size helpers for LOD selection.
- Added an editor sequencer model with snap-aware shot/key edits and OTIO-compatible metadata export.
- Added settings dependencies, changed-only views, category resets, named profiles, orphan
  diagnostics, and camera output/accessibility/sequencer options.
- Full HYBRID Release matrix passes 44/44; six focused suites pass under ASan/UBSan.

## v1.35 camera and settings additions

- Added a production camera director with fixed, free-fly, orbit, follow, third-person,
  first-person, and cinematic rigs; priorities and output channels; state bindings; forced
  live-camera control; cuts and configurable blend curves; physical camera metadata; target
  prediction; dead/soft-zone composition; screen offsets; collision pull-in and damped recovery;
  and layered, attenuated camera shakes with reduced-motion support.
- Added versioned `.dvecamera` camera-library assets. Format 2 preserves poses, lenses, physical
  filmback data, framing, collision, post-process weights, custom blends, and state bindings,
  while retaining format-1 loading.
- Added authored editor camera rigs with frustum visualization, picture-in-picture previews,
  camera bookmarks, physical-lens control, projection/mode actions, and navigation settings that
  directly affect free-look, orbit, pan, zoom, inversion, sensitivity, and speed boost.
- Replaced scattered preference toggles with a typed settings registry using Default, User,
  Project, and Session scopes; capability filtering; validation; live/on-apply/restart policies;
  search; advanced-option filtering; staged Apply/Discard; and transactional `.dvesettings`
  persistence.
- Expanded the editor to 13 top-level menus and 133 option definitions spanning camera, viewport,
  rendering, geometry, voxels, polygons, materials, audio, input, physics, scripting, build,
  accessibility, diagnostics, and plugins. Menus support sections, ordering, checkmarks, radio
  groups, capability-aware enablement, and command search.
- Full HYBRID Lua/editor/audio/RHI suite passes 41/41, including Vulkan execution. Focused camera,
  settings, and editor suites pass under AddressSanitizer and UndefinedBehaviorSanitizer.

## v1.34 Vulkan and production-texture additions

- Advanced the dynamically loaded Vulkan backend from buffer publication to genuine offscreen
  graphics execution: RGBA/depth images, texture transfer/readback, image views, shader modules,
  render passes, framebuffers, graphics pipelines, dynamic viewport/scissor state, vertex/index
  binding, indexed instanced draws, synchronization, and deterministic readback.
- Added a required-execution Vulkan test on the installed SwiftShader ICD. The image is compared
  pixel-by-pixel with a CPU triangle-coverage oracle. This proves Vulkan API execution on a software
  device; it is not physical-GPU performance evidence.
- Advanced `.dmesh` to backward-compatible format 1.1 with UV0/UV1, base-color,
  metallic/roughness, tangent-space normal, emissive, and opacity bindings, independent UV-set and
  color-space metadata, and normal scale.
- Added sRGB/linear channel handling, normal-map evaluation, alpha masking, emissive mapping, and
  projected-texel-density mip selection to the reference renderer.
- Added a bounded texture-residency manager with thread-safe request submission, worker/render-thread
  RHI pumping, mip generation, byte budgets, LRU eviction, stable asset IDs, and upload/eviction/
  fallback telemetry. BC5/BC7 requests currently report an explicit RGBA8 fallback.
- Added an authoritative CPU voxel reference renderer that raycasts real `VoxelObject` occupancy into
  the polygon target's normalized depth/color/object/material buffers. Polygons then depth-test and
  alpha-blend directly against that target, covering polygon-in-front, voxel-in-front, and
  translucent-over-voxel behavior without the prior synthetic voxel rectangle.
- Full Lua/editor/audio HYBRID suite: 39/39 passed. Dependency-minimal suite: 32/32 passed. VOXEL and
  POLYGON profile checks and six focused ASan/UBSan suites also pass.
- Physical Vulkan timing, presentation, production descriptor/material shaders, D3D12/Metal,
  hierarchy/animation, Jolt mesh/convex collision, and graphical hybrid authoring remain subsequent
  work and are not claimed by this checkpoint.

## v1.33 polygon-rendering additions

- Extended the RHI with graphics pipelines, render passes, color/depth attachments, viewport,
  scissor, texture transfer, vertex/index binding, and indexed-instanced draw commands.
- Added strict Null-RHI graphics-state validation and deterministic texture storage/readback;
  incomplete hardware backends return explicit unsupported-operation diagnostics.
- Added a visible CPU reference rasterizer with perspective-correct indexed triangles, depth,
  culling, two-sided materials, masking, sorted translucency, object/material IDs, Clear Coat and
  foliage references, sRGB-decoded base-color textures, generated mips, and screen-space mip/LOD
  selection.
- Added normalized-depth composition for separate voxel and polygon HDR/ID layers. The packaged
  demo uses a synthetic voxel reference layer; live voxel-tracer composition remains future work.
- Added immutable content-deduplicated mesh heaps, instance buffers, allocation records, and
  byte-exact RHI readback checks.
- Added a median-split polygon triangle BVH and routed GameWorld polygon ray/overlap queries through
  it while preserving submesh material identity.
- Added versioned `.dvescene` manifests with profile compatibility and explicit dependency lists.
- Full Lua/audio/editor HYBRID suite: 36/36 passed; focused VOXEL and POLYGON profile checks and
  sanitizer suites also pass. Hardware raster execution is explicitly deferred to v1.34.


## v1.32 hybrid geometry additions

- Compile-time `VOXEL`, `POLYGON`, and `HYBRID` product profiles through
  `DVE_GEOMETRY_MODE`; hybrid is the default and unsupported runtime geometry is rejected with
  explicit diagnostics.
- Versioned, content-hashed `.dmesh` assets preserve indexed triangles, normals, generated tangents,
  UVs, vertex colors, submeshes, shared DVE materials, double-sided/alpha metadata, and imported
  RGBA texture payloads from GLTF/GLB/OBJ authoring sources.
- `dve_cook_mesh` preserves ordinary meshes instead of voxelizing them. Existing `dve_cook_model`
  remains the explicit polygon-to-voxel path.
- `RuntimeMeshWorld` and `HybridGeometryWorld` publish polygon and voxel objects through separate
  backends under one stable object/transform API.
- `GameWorld::spawn_asset` and Lua `world.spawn_asset` now dispatch both `.dvox` and `.dmesh`.
  Polygon objects support static/dynamic conservative collision, triangle raycasts, bounds overlap,
  names, transforms, and common material data.
- `MeshRhiMirror` uploads vertex, index, draw, and shared material buffers and verifies byte-exact
  Null-RHI readback. Hardware rasterization and texture sampling are deliberately deferred to v1.33.
- Added CMake and CMakePresets profile examples, strict polygon-format validation, profile-aware
  tests, a reproducible mixed-geometry demo, and focused sanitizer coverage.

## v1.31 Unreal-style material additions

- Fixed-slot Material Parameter Collection with canonical time, wind, wetness, snow, time-of-day
  tint, and wind-direction entries. Canonical values are read directly by the shading pass from one
  shared cbuffer rather than copied into every material record.
- `world.set_global` bridges canonical or explicitly bound scalar names while leaving ordinary
  gameplay globals in the script dictionary. `world.set_global_vector` controls shared vectors.
- Versioned `.dvematparams` assets preserve stable scalar/vector slots and load transactionally.
- Clear Coat shading uses a second dielectric GGX lobe with Fresnel attenuation of the base lobe.
- Two-Sided Foliage uses a view-facing front lobe, colored back-light transmission, wrap control,
  and global wind-driven shading-normal motion.
- Uniform material-layer stacks support up to four Lerp, Multiply, or Additive layers, runtime
  weight edits, cycle/reference validation, editor format v3 persistence, and DVOX v1.2 persistence.
- GPU material records now carry clear-coat and foliage controls; CPU layout, enum, persistence,
  Lua, and layer-flattening tests are included. Shader execution still requires real-device
  validation.

## v1.30.1 merge additions

- Physical-model oscillator combining waveguide, modal, reed/lip/jet, body, breath, bow, and MPE
  controls; `.dvesynth` format 5 and 78 presets across instruments, game impacts, Nature, and
  Mechanical categories. Nature/Mechanical names denote stylized procedural patches, not verified
  field-recording equivalence.
- Deterministic preset audit, strict physical-parameter validation, Xlib-safe enums, and a 14-preset
  audition showcase. Five intentionally inharmonic bell/plate patches remain flagged for listening.
- Master materials and inherited instances, runtime overrides, GPU material/environment records,
  editor material persistence version 2, and Lua material/environment controls.
- Cook–Torrance PBR, ray-traced AO/shadows, voxel-thickness subsurface transmission, layered
  translucency, bloom, tonemapping, and exact sRGB encoding shaders. CPU-side packing and persistence
  are tested; GPU execution and visual calibration are not yet verified.
- Dependency-minimal and Lua suites: 33/33 passed. Focused GCC ASan/UBSan tests pass.

## v1.30 native-editor and device-workflow additions

- `.dveaudioedit` version 3 with backward loading, explicit comp segments, equal-power comp fades,
  take-lane selection by timeline range, and strict overlap/range validation.
- Shared-canvas audio workspace with waveform, spectrogram, and combined modes; clips, lanes, comp
  ranges, beat grid, markers, playhead, selection, loop/punch overlays, hit testing, snapping, and
  select/move/trim/split/slip/ripple/roll/crossfade/scrub/shuttle/zoom command foundations.
- SDL3 recording-device enumeration and microphone/line-input adapter. The callback copies fixed
  float blocks into a bounded ring; a worker performs adaptive sample-rate reconciliation and calls
  the existing capture sink.
- Mono/stereo EBU R128/BS.1770-style K-weighting, absolute/relative gating, momentary and short-term
  windows, loudness range, and four-times true-peak estimation.
- Offline DC removal, peak/loudness normalization, silence trim, fades, reverse, channel conversion,
  deterministic WSOLA time stretching, independent pitch shift, and piecewise warp-marker render.
- Internal effect racks with bypass, wet/dry, state, missing-effect placeholders, sidechain input, and
  deterministic-render checking.
- CLAP discovery/cache foundation with bundle hashing and blacklist persistence. v1.30 does not load
  untrusted plugin code in the editor process.
- Versioned `.dveinteractiveaudio` graph persistence with hashes, sequence/random/shuffle containers,
  cooldowns, no-immediate-repeat behavior, vertical stems, conditions, stingers, and quantized
  transitions.
- Reproducible headless demonstration producing a v3 session, recovery journal, adaptive-music graph,
  remix WAV, analysis JSON, and combined waveform/spectrogram screenshot.
- Optimized suite: 26/26 passed. Focused GCC AddressSanitizer and UndefinedBehaviorSanitizer suites:
  3/3 passed.


## v1.29 workstation-foundation additions

- Fixed fan-out recording slots for simultaneous dry synth, master, and selected bus/stem capture.
- `.dveaudioedit` version 2 with backward loading of version 1 sessions.
- Take lanes, active comp lanes, armed tracks, punch-in/out regions, pre/post-roll metadata, markers,
  variable tempo/time-signature maps, beat conversion, and quantized snapping.
- Gain/pan automation lanes with step, linear, and smooth interpolation; versioned sends with
  pre/post-fader intent.
- Slip, ripple-delete, roll-boundary, equal-power/smooth/linear crossfade, and source-loop edits.
- Multiresolution waveform peak caches, bounded spectrogram generation, level/DC/crest/clipping/
  silence/transient analysis, and a deterministic tempo estimate.
- Append-only recovery journals with hashed snapshots, truncated-tail recovery, compaction, and
  atomic project saves.
- Platform-neutral timeline draw data for clips, waveform columns, beat grid, markers, playhead,
  loop/punch overlays, hit testing, and snapping.
- Interactive music states, vertically mixed stems, gameplay parameters/conditions, entry stingers,
  and immediate/beat/bar/marker-quantized transitions.
- Reproducible demo producing simultaneous recorded stems, a v2 session, recovery journal, remix WAV,
  analysis JSON, and adaptive-music transition evidence.
- Optimized suite: 24/24 passed. Focused GCC AddressSanitizer and UndefinedBehaviorSanitizer suites: 3/3 passed.

## v1.28 audio workstation additions

- `AudioTakeRecorder` and selectable mixer taps for dry synth, master, music, dialogue, effects,
  ambience, user interface, and reverb input.
- Fixed single-producer/single-consumer callback ring with worker-side take assembly and dropped-frame
  telemetry.
- Versioned `DVEAEDT1` `.dveaudioedit` sessions with immutable source references, multitrack clips,
  clip/track gain and pan, fades, reverse, source looping, mute/solo, and project loop regions.
- Edit commands for add, move, trim, split, delete, bounded snapshot undo/redo, and deterministic
  session persistence.
- Transport play, pause, stop, seek, session-loop playback, and rewind.
- Deterministic stereo bounce plus float32 and PCM16 WAV export.
- Layered import backends: native WAV, optional libsndfile, and optional FFmpeg audio-stream
  extraction for MP3 and multimedia containers including MP4.
- Executed MP3 and MP4/AAC import tests, a recorded-synth reverse-remix demonstration, and cooker
  acceptance for WAV, MP3, MP4/AAC, M4A/AAC, FLAC, Ogg Vorbis, and Opus.

## v1.27.1 merge additions

### Production sample maps

- Canonical version-2 `DVESMAP2` cooked format with deterministic semantic hashing and corruption
  rejection; the loader retains the earlier integrated-runtime `DVESMAP1` layout.
- Human-readable 19-field recipes and the `dve_cook_sample_map` offline cooker.
- Reusable `.dvesample` dependencies, source deduplication, source-hash propagation, and resident,
  streamed, or hybrid-prefix assembly on the control thread.
- Up to 8 sources, 32 key/velocity zones, and 131,072 resident mono frames per immutable map.
- Root notes, cent tuning, gain, pan, reverse, attack/release triggers, and deterministic round robin.
- Resident, streamed, and hybrid preload declarations.
- Triple-buffered map publication adopted only at audio-block boundaries.
- Thirty-two generation-tagged fixed stream pages of 2,048 frames; callback misses fail closed to
  silence and telemetry without file access.
- Half-open playback/loop intervals, internal loop endpoints, and equal-power loop crossfades.

### Production granular path

- Mapped resident or published streamed source windows.
- Envelope curvature, stereo motion, reverse probability, random/quantized pitch, and
  velocity/timbre density modulation.
- Global and per-voice grain budgets that tighten under 9–16 voice load and favor note attacks.
- Requested/admitted/steal/miss/current/maximum grain counters plus stream-page metrics.
- Correct preservation of intrinsic sample/granular stereo before voice panning.

### Reproducible evidence

- `dve_sample_granular_synth_demo` now writes a `.dvesamplemap`, `.dvesynth`, six-second float WAV,
  and JSON render oracle.
- `dve_audio_synth_sample_granular_bench` now exercises the mapped round-robin path and reports
  callback percentiles and grain telemetry at 128, 256, and 512 frames.
- `dve_audio_sample_map_tests` covers format integrity, pages, zoning, release layers, internal
  loops, underruns, overload shedding, and persistence.
- `dve_sample_map_recipe_tests` covers parsing, dependency loading, resident/hybrid/streamed assembly,
  canonical cooking, legacy integrated-runtime loading, and explicit rejection of unsupported layers.

## v1.26 foundation

### Gameplay scripting and cooked assets

- Optional Lua 5.4 gameplay binding (`DVE_ENABLE_LUA`).
- `GameWorld::spawn_asset(path, name, transform, dynamic, structural)` and Lua
  `world.spawn_asset(...)` for `.dvox` objects.
- Per-material density quantization and mass/inertia construction; fragments preserve the
  source asset's material mass table.
- Real action, axis, timer, force, impulse, object, tag, HUD, and gameplay callback paths.
- X11/XTest game-host source with real event polling, verified focus, and 60 Hz frame pacing,
  conditional on Jolt and X11/XTest development libraries.
- A narrow runtime-only Lua ABI fallback when the operating system exposes Lua 5.4 runtime
  libraries but not development headers.

### Eightfold sample and granular synthesis

- Eight oscillator slots per voice and sixteen polyphonic voices remain available.
- New `Sample` waveform with normalized start/end points, loop region, forward/reverse mode,
  one-shot behavior, root note, key tracking, and velocity-to-gain response.
- New `Granular` waveform with eight fixed grains per oscillator, grain position, size,
  density, spray, pitch offset, stereo spread, freeze, reverse, and selectable window.
- One validated resident mono sample bank per preset, up to 16,384 frames at its declared
  sample rate, imported through the engine's existing audio decoder.
- No callback allocation, file access, middleware calls, or unbounded grain creation.
- Version-5 `.dvesynth` serialization including the complete sample bank and deterministic
  migration from versions 1–4.
- Native editor controls for sample/granular parameters and waveform visualization.
- Freehand eight-frame wavetable painting with frame selection and cleanup tools.

## Validation classification

Executed for this checkpoint:

- Dependency-minimal optimized headless build: **16/16 passed**.
- Production sample-map test suite: **passed**.
- Existing synthesizer regression suite: **passed**.
- GCC 14.2 AddressSanitizer and UndefinedBehaviorSanitizer builds of the synthesizer, sample-map, and recipe suites: **passed**.
- Xlib-first portable-header compatibility test: **passed**.
- Deterministic sample-map/granular demo and Release callback benchmark: **passed**.

The configured Swift Clang 17 sanitizer runtime crashes before `main()` even for a standalone
hello-world binary in this container. GCC sanitizers were used for executable sanitizer coverage.

Not executed or not completed here:

- Automatic worker-side filling of sample-map pages from `streamPath`; page publication is currently
  an explicit control-thread API.
- Cross-group layered zones, ping-pong loops, and stereo sample-map sources. The cooker rejects these
  cases rather than silently changing playback semantics.
- Thirty-minute physical SDL3 audio tests or physical MIDI/MPE devices.
- Native Windows/MSVC or macOS/Core Audio builds.
- Explicit SIMD interpolation, runtime save games, script hot reload, shared runtime HUD canvas, and
  sample-map authoring tools.
- Jolt-linked game-host execution.

## Generation-locked destruction path

```text
Authoritative simulation generation N
        |
        +-- voxel edits
        +-- Jolt/contact samples
        +-- fragment separation
        +-- structural strain
        +-- acoustic dirty region
        |
        v
DestructionCommitCoordinator
        |
        | one owned DestructionAudioCommit, generation N
        v
DestructionAudioIngress
        |
        +---------------------------+
        |                           |
        v                           v
DestructionAudioRuntime       AsyncAcousticPublisher
cluster / classify / event    surfaces + room/portal state
        |                           |
        v                           v
scheduled mixer actions       AcousticSnapshot generation N
        |                           |
        +-------------+-------------+
                      v
            control-thread spatial result
                      |
            SpatializationTransition
                      |
       allocation-free audio-thread interpolation
```

A queue failure does not partially publish the transaction. A stale scene generation is rejected,
and direct simulation requires its source generation to match the committed acoustic scene exactly.

## Physics contact aggregation

`PhysicsContactAudioAccumulator` is intended to sit directly beside a Jolt contact callback. It has
no recording-time allocation and a maximum fixed capacity of 256 buckets. Samples are grouped by:

- unordered body identifiers;
- corresponding material pair;
- configurable world-space cell;
- configurable time window.

Explicit normal impulses are preferred. When unavailable, the accumulator estimates an impulse from
normal relative velocity and effective mass. Draining emits compact `PhysicsContactAudio` records,
preventing every contact manifold point from becoming a separate voice.

## Steam Audio direct-simulation boundary

The optional native target remains hidden behind `SteamAudioNativeContext` and
`SteamAudioNativeDirectSimulator`; no `phonon.h` types enter the portable mixer or editor API.

The current one-hero-source slice provides:

- scene and static-mesh ownership;
- box-to-triangle conversion with material indices;
- acoustic absorption, scattering, and transmission mapping;
- scene and simulator commits on the control/acoustic thread;
- configurable raycast or volumetric occlusion quality;
- distance attenuation, air absorption, directivity, occlusion, and transmission;
- immutable result publication through `std::atomic<std::shared_ptr<const ...>>`;
- query rejection when the listener/emitter has moved beyond the configured tolerance;
- scene/simulation/query timing and geometry telemetry.

The audio callback never calls Steam Audio. Scene commits and direct simulation are control/worker
operations; the callback only consumes a published direct-path value.

## Audible wall-collapse demonstration

`dve_wall_collapse_audio_demo` writes a deterministic 48 kHz floating-point stereo WAV and JSON
report. The source begins behind a concrete wall, a collapse occurs, and the spatial state crossfades
to the open room over 180 ms.

Measured states in the packaged demonstration:

| State | Direct gain | Low-pass | Reverb send |
|---|---:|---:|---:|
| Intact wall | 0.181822 | 11,490.3 Hz | 0.30 |
| Wall removed | 0.344828 | 20,000 Hz | 0.12 |

The collapse and published acoustic snapshot both use generation 501.

Run it with:

```bash
./dve_wall_collapse_audio_demo \
  --wav wall_collapse.wav \
  --json wall_collapse.json
```

## Audio Event Graph editor

Open the audio tools through:

```text
Window -> Synthesizer       Ctrl+4
Window -> Audio Mixer       Ctrl+5
Window -> Audio Event Graph Ctrl+6
```

The Audio Event Graph provides a searchable 14-node palette, dragging, connection editing,
cycle/arity checks, subtree copy/paste, deletion, compile diagnostics, auditioning, save/open, and
64-level snapshot undo/redo. v1.21 adds an actual property surface rather than a read-only inspector.
Properties are specialized by node type, including:

- sample/stream identifier, loop state, bus, and priority;
- synthesizer MIDI note, velocity, duration, and priority;
- delay, gain, threshold, blend range and curve;
- scatter interval/count, cooldown, and loop interval/count;
- random no-repeat history.

Every property adjustment is validated, compiled, and added to undo history.

## Real-time performance evidence

Dense mixed-source regression at 48 kHz stereo and 256-frame blocks:

- 128 logical resident voices.
- 48 physically mixed voices and 80 virtual voices.
- 16 synthesizer voices with eight oscillators each.
- Mean callback: **0.908 ms**.
- p99 callback: **1.146 ms**.
- 5.333 ms block deadline; p99 uses **21.48%**.
- Offline throughput: **5.87× real time**.

Four concurrently streamed stereo assets:

- Mean callback: **0.062 ms**.
- p99 callback: **0.087 ms**.
- p99 uses **1.63%** of the block deadline.
- Stream underrun frames: **0**.

These are container CPU measurements, not physical-device latency or underrun guarantees.

## v1.27 mapped sampler/granular performance

Twelve voices, one mapped sample oscillator and one mapped granular oscillator per voice, two
round-robin resident sources, and chorus/reverb/compressor/limiter:

| Block | Mean callback | p99 callback | p99 deadline use | Estimated single-core use |
|---:|---:|---:|---:|---:|
| 128 | 0.857 ms | 1.334 ms | 50.02% | 32.15% |
| 256 | 1.697 ms | 2.357 ms | 44.19% | 31.82% |
| 512 | 3.429 ms | 5.202 ms | 48.77% | 32.15% |

These are four-second container measurements, not physical-device guarantees. The packaged JSON
contains full mean/p95/p99/max callback data and granular telemetry.

## Build and test

```bash
cmake --preset linux-gcc-release
cmake --build --preset linux-gcc-release -j4
ctest --preset linux-gcc-release --output-on-failure
```

Sanitized validation:

```bash
cmake --preset linux-clang-debug -DDVE_BUILD_BENCHMARK=OFF
cmake --build --preset linux-clang-debug -j8
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 \
  ctest --preset linux-clang-debug --output-on-failure
```

Dependency-minimal validation:

```bash
cmake --preset headless-ci
cmake --build --preset headless-ci -j8
ctest --preset headless-ci --output-on-failure
```

Optional native Steam Audio configuration:

```bash
cmake -S . -B out/build/steam -DCMAKE_BUILD_TYPE=Release \
  -DDVE_ENABLE_STEAM_AUDIO=ON \
  -DDVE_STEAM_AUDIO_ROOT=/absolute/path/to/steamaudio
```

When the SDK is absent, the engine still builds the neutral spatial contract and executes the
production adapter against the deterministic fake ABI.

## Important v1.21 files

```text
include/dve/audio/physics_contact_audio_accumulator.hpp
src/audio/physics_contact_audio_accumulator.cpp
    Fixed-capacity listener-side contact aggregation.

include/dve/audio/destruction_commit_coordinator.hpp
src/audio/destruction_commit_coordinator.cpp
    One authoritative transaction for sound stimuli and acoustic rebuilds.

include/dve/audio/steam_audio_native.hpp
src/audio/steam_audio_native.cpp
    SDK-hidden HRTF, scene, material, simulator, source, and direct-path adapter.

include/dve/audio/spatial_transition.hpp
src/audio/spatial_transition.cpp
    Generation-tagged callback-safe spatial crossfade.

apps/wall_collapse_audio_demo.cpp
    Reproducible intact-wall to open-wall audible demonstration.

tests/test_audio_wall_collapse.cpp
    Contact aggregation, generation coupling, spatial transition, and mixer regression.

tests/test_steam_audio_native.cpp
    HRTF ownership plus closed/open scene and direct-simulation contract tests.

include/dve/editor_audio_event.hpp
src/editor_audio_event.cpp
src/editor_native_renderer.cpp
    Node-specific authoring properties and editor rendering.
```

## Next engineering milestone

v1.22 should execute the same vertical slice against the actual Steam Audio SDK and a real Jolt
`ContactListener`, then expand from one hero source to a bounded source pool and add reflection and
Ambisonic rendering with explicit CPU-quality tiers. Physical SDL3/RtMidi tests and 30-minute
underrun/device-loss soaks remain required before calling the audio path production-ready.

### v1.37 camera rendering and sequencing

The camera runtime now publishes one aligned GPU packet per viewport, supports split-screen and named render targets, identifies cuts/teleports/origin shifts that invalidate temporal history, and provides deterministic depth-of-field reference processing. Camera sequences use constant-speed spline reparameterization, events, and preserved metadata. Runtime cameras can be controlled from Lua and saved/restored. Obstruction policies include pull-forward, fade requests, shoulder swapping, and room constraints; reduced-motion and photosensitive profiles coordinate camera movement, shake, bloom, vignette, and depth-of-field.

## v2.31 procedural geometry foundation

The engine includes a topology-aware `EditableMesh` and headless typed geometry graph. See `destructible_voxel_engine_dynamic_mesh_geometry_graph_format_v1.md` and run `dve_geometry_graph_bake <output.dmesh>` for the initial CLI vertical slice.
