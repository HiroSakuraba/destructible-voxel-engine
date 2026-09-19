# v2.35.3 — Composite Input and Rebinding Reliability

- Added deterministic weighted 1D composite bindings for digital and analog controls, including
  signed values, cancellation, configurable actuation thresholds, and per-binding gesture state.
- Made context consumption composite-aware and conflict detection operate on overlapping control
  footprints instead of exact primary-control matches.
- Fixed self-conflicts during rebinding and added deterministic conflict-owner diagnostics.
- Added version-2 remapping persistence with temporary-file publication, recoverable replacement,
  transactional loading, strict validation, and backward-compatible loading of the original
  unversioned format.
- Added focused coverage for composite actuation, cancellation, priority consumption, analog
  thresholds, conflict detection, round trips, corrupt-version rejection, and legacy loading.

# v2.35.1 — Incremental Destruction-Aware Navigation

- Replaced whole-source dynamic navigation rebuilding with bounded dirty-tile source
  classification and polygon replacement.
- Reuses untouched navigation polygons and deterministically restitches shared-edge portals
  across rebuilt and retained tile borders.
- Preserves transactional publication: failed tile builds leave the live mesh and dirty set
  available for retry.
- Adds telemetry for rebuilt tiles, candidate source triangles, reused polygons, and rebuilt
  polygons.
- Adds deterministic destruction, disconnection, restoration, cross-tile continuity, geometry
  preservation, and repeat-build hashing checks.

# v2.35 — Runtime and Tooling Foundations

- Added deterministic navigation agents and destructible-voxel dirty navigation tracking.
- Added deterministic `.dvepak` build/mount support, dependency collection, integrity hashes,
  editor stripping, and incremental package reuse.
- Added profiler, asset dependency, input, save-game, retargeting, morph, CCD IK, AI,
  networking, editor history, and plugin registration foundations.
- Added explicit torque/angular impulse and deterministic ray, overlap, and sphere query-all
  contracts with filters and physical-material metadata.
- Added focused deterministic tests and strict/sanitized standalone validation paths.
- Advanced the CMake project version to 2.35.0.

# v2.34 — Physics Integration Repair

- Corrected the reproducible Jolt dependency from the unavailable v5.6.1 tag to the official
  immutable v5.6.0 release and updated the active presets and format documents.
- Added solver-neutral point-force, point-impulse, contact-sink, and contact-material hooks to
  `IRigidBodyWorld`; existing backend-specific APIs now override that shared contract.
- Added exact torque-producing point forces to Jolt and Box3D.
- Corrected physical marionette strings to load their authored body attachment points and to
  include angular point velocity in spring damping.
- Advanced the CMake project version to 2.34.0. Historical v2.01/v2.02 evidence mentioning a
  supplied 5.6.1 development snapshot is retained as historical evidence, not a fetchable release.

# v2.32 — Fully Articulated Marionette

- Added `MarionetteRuntime`, a six-string puppet controller layered on skeletal animation, control rig, ragdoll, rigid-body, and platform gamepad events.
- Added generated humanoid setup assets: 11 model-space controls, pelvis/chest/head drives, four two-bone IK chains, 15 rigid bodies, and 14 articulated joints.
- Added Assisted Rig, Hybrid, and Physical control modes with bounded spring-damper forces and per-string telemetry.
- Added `MarionetteInputRouter` for tracked two-controller VR input and a DualSense-style SDL mapping.
- Added focused tests for setup generation, stable hashing, controller mapping, assisted pose publication, ragdoll activation, and physical string acceleration.
- The focused linked CTest passes. Full engine registry, hardware VR, haptics, and production Jolt image/play testing remain outside this working release.

# v2.30 — Navigation Mesh and Heightmap Terrain

- Added a deterministic triangle navigation mesh with walkable-slope filtering, vertical step stitching, portal-radius clearance, agent-height clearance, stable tile coordinates, diagnostics, and hard polygon limits.
- Added nearest-point and A* path queries, area costs, inclusion/exclusion flags, query-time AABB obstacles, funnel smoothing, partial paths, and off-mesh links.
- Added `.dnav` version-1 persistence with rebuild-on-load validation and content-hash verification.
- Added navigation geometry extraction from transformed polygon assets and exposed top surfaces of destructible voxel objects.
- Added 8/16-bit grayscale PNG, PGM P2/P5, and RAW16 little-/big-endian heightmap import with explicit flip, invert, dimensions, scale, spacing, offset, and origin.
- Added heightmap cooking into `.dmesh`, optional closed solid `.dvox`, and `.dnav`, with bounded geometry and voxel working sets.
- Added the `dve_cook_heightmap` command-line tool and Terrain/Navigation asset-browser kinds.
- Added focused navigation, terrain, persistence, CLI, and asset-browser regression coverage.
- Passed the complete editor-enabled Unix Makefiles build, 170/170 configured tests, five changed production units under warnings-as-errors, and focused AddressSanitizer/UndefinedBehaviorSanitizer/leak execution.

# v2.29.4 — Shadow and Runtime UI Corrections

- Corrected cascaded-shadow caster depth to use the same absolute light-space origin as the sampler, with far-origin regression coverage at approximately 0 m, 50 m, and 500 m.
- Matched caster Y to the Vulkan positive-height viewport convention and added near-depth pancaking for casters extending toward the light.
- Updated the compute resolve to sample and conservatively combine both static and dynamic shadow-atlas layers.
- Added keyboard fallback steps for continuous sliders and real spatial Left/Right/Up/Down focus navigation.
- Added UTF-8 caret editing, Home/End/Delete, insertion at caret, text-safe Space handling, filtered paste, commit events, and a caret render primitive.
- Made List widgets pointer/keyboard interactive and unified input paths on rebuild-resolved enablement.
- Ensured viewports created after accessibility configuration inherit effective reduced motion and shake scale.
- Added `dve_v2294_shadow_ui_fixes_tests`; passed the complete Unix Makefiles build, 170/170 tests, strict changed-unit compilation, shader source validation, and focused ASan/UBSan/leak checks.
- Physical execution of the canonical caster/resolve shaders remains required for GPU shadow certification.

# v2.29.3 — Ray-Lighting Units and GI Sampling Corrections

- Added an explicit metres-per-voxel render constant and converted all AO, GI, contact-shadow, soft-shadow, and subsurface tracer distances at the shader boundary.
- Added direct-sun next-event contribution at GI bounce points, distance-weighted AO, Hammersley/Cranley-Patterson sampling, and non-quantized GI origins.
- Added temporal lighting accumulation and spatial denoising contracts plus focused CPU/source regression coverage.
- Passed the complete Unix Makefiles build and 169/169 registered tests in the v2.29.3 working package.
- The compute shaders remain source-validated pending physical dispatch and image parity.

# v2.29.2 — Lighting and Shadow Corrections

- Corrected the live split-sum BRDF LUT integration from the punctual-light Smith approximation to the image-based-lighting form `k = roughness² / 2`.
- Added a public exact BRDF integration oracle and numerical regression at `NdotV = 0.7`, roughness `0.4`, and 512 samples.
- Added seamless CPU cubemap filtering across face edges, deterministic filtered source mips, and solid-angle source-LOD selection for GGX specular prefiltering.
- Corrected two-sided foliage transmission by adding Lambertian `1/π` normalization and using the hemisphere opposite the viewer-facing shading normal.
- Corrected clear-coat base attenuation to account for both coat-interface crossings.
- Replaced uniform AO on the full IBL result with diffuse AO plus Lagarde-style roughness/view-dependent specular occlusion.
- Disabled hemisphere ambient whenever environment lighting is active, preventing diffuse sky illumination from being counted twice.
- Made receiver and normal shadow bias operational in CPU atlas-coordinate generation, added out-of-cascade rejection, and exposed half-texel-safe cascade-tile bounds.
- Updated PCF shader sampling to use cascade index/count and clamp every tap to the selected atlas tile.
- Added `dve_v2292_lighting_rendering_fixes_tests`; passed the complete Unix Makefiles build, 168/168 tests, strict compilation, shader source validation, and ASan/UBSan/leak checks.
- Physical compilation and dispatch of `shade_primary.hlsl` and cascaded-shadow resolve remain unverified; shader corrections are protected by source-equation checks pending a GPU differential harness.

# v2.29.1 — Cinematic Camera Optics Corrections

- Corrected anamorphic projection so delivery keeps square pixels; anamorphic squeeze now controls filmback delivery aspect, oval bokeh, and flare rather than compressing scene geometry.
- Corrected anisotropic bokeh reach tests, split-diopter seam behavior, foreground blur coverage, gate-fit circle-of-confusion evaluation, and resolution-independent depth-of-field limits.
- Added physically meaningful interpolation spaces: focus distance in diopters, aperture and focal length logarithmically, field of view in tangent space, and color temperature in mireds; blade-count blends remain valid throughout transitions.
- Corrected bloom exposure ordering and wide-kernel behavior, radial chromatic aberration, focus breathing, pre-tone-map vignetting, post-tone-map LUT sampling, midtone-weighted grain, wide halation, and anamorphic flare.
- Added a permanent 37-check optical regression test covering anamorphic geometry, bokeh shape, split diopter, foreground coverage, gate fit, blending, filmback-derived circle of confusion, and LUT behavior.
- Independently passed the full Unix Makefiles build, 167/167 registered tests, strict warnings-as-errors compilation for all changed production C++ units, shader-contract validation, and ASan/UBSan/leak execution of the optics harness.
- Physical compilation and dispatch of the cinematic HLSL shaders remain unverified in this environment.

# v2.29.0 — Cinematic Camera Menus and Native UX

- Added 41 stable menu and Command Center actions for camera scopes, presets, filmbacks, profile operations, sequencing, viewport control, overlays, LUT/color routing, and diagnostics.
- Added a pointer-operable native Cinematic Camera panel with eight sections, project/shot/camera/preview ownership, local undo/redo, and complete-profile sequencer keyframing.
- Added project Camera settings for cinematic defaults, quality tiers, LUT policy, output grading, and accessibility reductions.
- Added safe-frame, aspect-matte, focus, split-diopter, motion-vector, exposure, and graded/neutral viewport overlays.
- Corrected new-sequence construction and X11 `None` macro compatibility.
- Passed the complete build, 166/166 tests, eight strict warnings-as-errors changed-unit checks, and focused ASan/UBSan/leak execution.

# v2.28.0 — Cinematic Camera, Lens, Framing, and Film Pipeline

- Added Super 16, Super 35, full-frame, anamorphic, IMAX 15-perf, and IMAX digital filmbacks plus Academy, IMAX, widescreen, scope, and custom framing.
- Added Brown-Conrady distortion, equidistant fisheye, chromatic aberration, anamorphic squeeze/flare, breathing, gate weave, split diopter, shaped bokeh, cat-eye bokeh, LUT grading, lift/gamma/gain, grain, halation, sharpening, and motion-vector blur.
- Added ten editable cinematic presets, a deterministic CPU reference pipeline, `.cube` LUT parsing, camera-sequence v3 persistence, transactional sequencer preset APIs, and accessibility reductions.
- Expanded the fixed camera GPU packet to 544 bytes and added automatic C++/HLSL ABI validation plus cinematic DOF and post-process compute-shader contracts.
- Passed strict changed-unit compilation, 6/6 focused camera tests, focused ASan/UBSan/leak execution, 48-shader validation, the complete configured build, and 157/157 registered tests.
- Physical backend dispatch, GPU image parity, LUT residency, motion-vector producer integration, complete native inspectors, and representative-hardware certification remain follow-up work.

# v2.27.0 — Packed Brick-Palette GPU Resolve Conformance

- Added a packed CPU oracle that resolves directly from the exact v2.26 brick-palette upload packet and checks baked, single-material, palette2, and palette4 paths.
- Added fixed-layout analytic material and resolve-request records plus a nine-binding compute contract that preserves the six v2.26 renderer bindings.
- Added one canonical HLSL compute resolver for DXIL/SPIR-V/Metal-source builds and automated C++/HLSL ABI drift validation.
- Added a cross-backend RHI publication, compute-pipeline, dispatch, fence, readback, lifetime, and statistics harness.
- Verified every sample in representative palette2/palette4 bricks, strict changed-unit compilation, focused ASan/UBSan/leak execution, 46-shader contract validation, a complete configured engine build, and 132/132 registered tests.
- Backend shader compilation and physical GPU arithmetic were not available in this environment; texture-resident voxel shading and hardware parity remain follow-up work.

# v2.26.0 — Production Brick-Palette Cooking and Renderer Submission

- Connected real `VoxelObject` bricks and normal model/scene asset-pipeline wrappers to deterministic `DVEBPAL` cooking.
- Added platform retention profiles, dependency keys, retained/canonical byte accounting, and regional recooking from applied brick edits.
- Added fixed-layout renderer upload packets, material remap arenas, palette2/palette4 mapping contracts, and six-binding RHI resource layouts.
- Added a cross-backend Null-RHI mirror with exact readback, bind-group lifetime, and no-draw submission validation.
- Passed strict focused tests, ASan/UBSan/leak checks, the complete 698-step Linux release build, and 163/163 registered tests.
- Physical backend palette shaders, live material residency, and representative-device certification remain follow-up work.

# v2.25.0 — Merged Brick Palette Material Switcher

- Replaced the provisional DVMP payload with one brick-oriented DVEBPAL version-2 format.
- Merged deterministic brick cooking, all overflow policies, cross-brick remapping, mapping-aware CPU reference shading, exact texture-sample accounting, and LOD crossfades.
- Retained live fallback/recook switching, atomic activation, generation tracking, and memory accounting.
- Corrected order-sensitive floating accumulation and retained full per-sample canonical material weights for real recooking.

# v2.24.0 — Selectable Voxel Material Policy

- Added baked-properties, single-material, deferred-palette, and hybrid voxel material policies while preserving the conventional renderer path.
- Added project, asset, platform, distance, LOD, memory-pressure, destructibility, recent-edit, palette-limit, and overflow inputs to deterministic per-brick selection.
- Added safe cooked fallback and recook decisions that require canonical source membership and never reconstruct lost material identities by guesswork.
- Added two-/four-way deferred path contracts, JSON/report hashing, 3D diagnostics integration, project settings, and a stable Tools/Command Center action.
- Added retained six-brick evidence and focused/editor regression coverage.
- Deferred brick binary payloads and physical GPU material-blending shaders remain follow-up work.

# v2.23.0 — Unified 3D Rendering Diagnostics

- Added renderer-neutral per-camera 3D diagnostics for meshes, submeshes, triangles, vertices, instances, materials, shaders, pipelines, and exact adjacent pipeline-state breaks.
- Added mesh/material/shader/pipeline/texture and mip residency inventories, missing-reference auditing, CPU/GPU skinning attribution, LOD decisions, shadow-atlas evidence, clustered-light counts, and occlusion inventories.
- Added deterministic CPU/reference depth-complexity rasterization, RGBA heatmaps, stable capture hashes, JSON export, and Mobile/Desktop/High-End/custom budgets.
- Added **Tools → Rendering → 3D Rendering Diagnostics** with the registered `F11` shortcut and Command Center discovery.
- Added retained reference capture, focused core/editor tests, 35-suite rendering regression, strict warnings-as-errors checks, and ASan/UBSan/leak validation.

# v2.21.0 — 4K, High-DPI, and Multi-Display Presentation

- Added built-in HD through 8K, ultrawide, super-ultrawide, and portrait resolution profiles plus exact 4K integer-scaling evidence.
- Added mixed-DPI display topology, window placement, work-area clamping, preferred-display recovery, refresh-rate metadata, and presentation budgets.
- Added one-to-four-player and picture-in-picture viewport layouts with unique camera, HUD, input, audio, safe-area, and budget ownership.
- Added camera-runtime synchronization, HUD and pointer remapping, shared-camera split/merge hysteresis, listener mix policies, and mixed-refresh frame pacing.
- Added independent stable-ID RHI surface/swapchain reconciliation, acquire/present, resize/out-of-date recreation, removal, and shutdown.
- Added version-2 game display settings, editor display/multi-viewport settings, deterministic demos, focused regressions, strict compilation, and sanitizer validation.

# v2.20.0 — Menu Command Center and Information Architecture

- Added compact Primary, optional Advanced, and Palette-only command visibility while preserving stable action IDs and shortcut compatibility.
- Expanded the Command Center to fuzzy multi-token search over commands, settings, panels, assets, scene objects, and documentation with provider-specific prefixes.
- Added persistent local favorites, bounded recent-command history, advanced-menu preference, dangerous-action metadata, and explicit disabled-command explanations.
- Unified current-state command availability across menus, shortcuts, automation, context menus, accessibility output, and command search.
- Added focused regression, native-editor smoke, strict compilation, and ASan/UBSan/leak validation.

# v2.19.0 — Sprite Diagnostics and Tile-World Desktop Editor

- Added per-camera draw-call, batch-break, sprite, tile, visible-pixel, fragment, texture, and palette reporting.
- Added exact bounded atlas occupancy/overlap/fragmentation analysis, residency inventories, sorting-layer visualization, pixel-snapping evidence, missing-reference audits, and profile budgets.
- Added deterministic CPU/reference overdraw rasterization and RGBA heatmaps for sprite and tile presentation.
- Added a complete native tile-world desktop panel with semantic inspectors, layer controls, pointer-driven collision resizing, autotile/parallax/chunk views, regional recook overlays, prefab overrides, validation, saving, and one-click play testing.
- Added focused strict, regression, and ASan/UBSan/leak validation.

# v2.18.0 — Sprite Production Presentation

- Completed direct sprite-track editing with direct canvas handles, complete numeric fields, atomic multiframe clipboard/duplicate/mirror/retime/preset operations, searchable lanes, exact item diagnostics, and onion-skin combat/socket overlays.
- Added a polished native sprite animation-state graph panel with deterministic node/edge drawing, transition inspectors, runtime tracing, validation badges, comments, groups, minimap, breadcrumbs, edge reroutes, atomic graph copy/paste, and subgraph groundwork.
- Added original indexed sprite and particle assets for the Sun Route vertical slice and connected the deterministic game state to real sprite, tile, palette, particle, camera, parallax, HUD, and screen-shake presentation data.
- Added **Sprite → Play Original Sprite Level** (`F9`) and the packaged `dve_sprite_vertical_slice_v218_demo` headless presentation executable.
- Added a deterministic sprite-particle renderer supporting burst/continuous emitters, flipbooks, XY/XZ billboards, velocity stretch, curves over life, sorting/batching, event/socket spawning, trails, beams, and CPU reference rasterization.
- Added strict compilation, focused release, inherited regression, sanitizer, deterministic replay, patch-reproduction, and archive-manifest evidence.

# v2.17.0 — Native Chiptune Authoring and Original Vertical Slice

- Completed the native tracker authoring workflow with pattern/order editing, keyboard/piano entry, instrument and song preview, envelopes/wavetables, effect helpers, SFX customization, mixer routing, clipboard/history, and atomic publication.
- Added an original deterministic side-scrolling gameplay proof with tracker-authored music and effects.

# v2.16.0 — Tile World and CPU Hair Mainline Merge

- Merged the parallel v2.13 native tile-world authoring line into the newer v2.15 CPU hair line using v2.12 as the common base.
- Preserved all tile-world source, formats, samples, semantic queries, authoring tests, and updated tilemap regressions.
- Preserved all v2.13-v2.15 CPU hair simulation, self-collision, visible-ribbon expansion, runtime integration, tests, and benchmarks.
- Resolved shared CMake, README, changelog, release-manifest, and historical work-plan conflicts; no newer hair implementation was replaced by older branch content.
- Added merged build/test and archive verification evidence.

# v2.15.0 — CPU Visible-Hair Ribbon Expansion and Reference Rendering

- Added deterministic guide-to-child-strand expansion that turns simulated CPU guides into many inexpensive visible strands without adding solver particles.
- Added camera-facing tapered ribbon packets with stable topology, child spread/jitter, per-guide layer propagation, bounds, zero-copy spans, and reusable storage.
- Added guide, strand, point, and projected-size LOD controls plus a runtime-owner bridge and bounded packet validation.
- Added 32-strand worker batching over the persistent JobSystem, raising four-worker expansion speedup to 2.41–2.69× over serial in the release benchmark.
- Added a deterministic opaque-depth-aware CPU reference renderer for tests, thumbnails, and graphics-backend bring-up; production GPU upload/draw integration remains explicit follow-up work.
- Added topology, taper, deterministic rebuild, LOD, owner lookup, invalid-setting, depth, identifier, strict-warning, sanitizer, and benchmark evidence.

# v2.14.0 — CPU Hair Spatial-Hash Self-Collision

- Added optional bounded cross-guide particle self-collision to the batched CPU hair runtime.
- Added a preallocated generation-tagged uniform hash, 27-cell queries, and a hard candidate cap per point.
- Added deterministic Jacobi correction generation in worker jobs and stable authority-thread application.
- Added independent `maximumSelfCollisionGuides` LOD without reducing ordinary simulated guides.
- Added self-collision iteration, radius, stiffness, and neighbor controls plus detailed telemetry.
- Added overlap separation, serial/parallel equivalence, LOD, validation, strict compile, sanitizer, and stress benchmark evidence.

# v2.13.0 — Batched CPU Hair Runtime

- Added a clean-room batched XPBD guide-hair solver with contiguous groom storage, persistent JobSystem workers, deterministic independent-strand jobs, timestep-aware stretch/bend constraints, and zero-copy render views.
- Added gravity, wind drag, damping, velocity limiting, sphere/capsule/plane collision, sleeping, active-guide LOD, update-rate division, root teleport handling, and per-guide skinned root targets.
- Added strict Sisir-style ASCII PLY import, bounded content-hashed `.dvehair` assets, and the `dve_cook_hair` offline converter.
- Added `CpuHairRuntime` and `GameWorld` ownership so hair follows normal polygon, voxel, marker, animated, attached, and rigid-body-backed objects; enable state and destruction propagate automatically.
- Eliminated per-tick owner-list allocation and stationary-root traversal in the GameWorld integration path.
- Added focused import, round-trip, deterministic parallelism, collision, LOD, transform, lifecycle, and malformed-data tests.
- Measured 2,048 × 16-point guides at 3.971 ms/frame on four workers, 3.84× faster than serial and 1.75× faster than the generic rope path in the release test environment.


# v2.13.0 (parallel branch) — Native Tileset and Level Authoring

- Added reusable canonical `.dvetileset` assets with texture dimensions, margin/spacing grid slicing, tile semantics, animated atlas ranges, and deterministic four-neighbor autotile rules.
- Added `DVE_TILEMAP 2` visual, collision, hazard, trigger, and object layers while retaining bounded v1 migration and embedded resolved tileset data.
- Added transactional tileset and map authoring sessions with recoverable publication, 128-level undo/redo, exact regional edit impact, stable object IDs, clean external reload, and dirty conflict protection.
- Added platform-neutral tileset-palette and level-canvas workspaces with zoom/pan, selection, pencil, eraser, rectangle, flood fill, terrain painting, autotile refresh, object placement, object movement, and preview frames.
- Added semantic world queries, deterministic animated visible-tile extraction, chunk diagnostics, and an original v2.13 sample tileset/level.
- Added v2.13 authoring tests and retained native tile collision, Physics2D, side-view controller, and v2.10-v2.12 sprite animation regressions.

# v2.12.0 — Sprite Visual Graph, Blend Gameplay, and Root-Motion Production Pass

- Added `DVE_SPRITE_MACHINE 3` with Linear, SmoothStep, EaseIn, and EaseOut blend curves plus explicit gameplay-track,
  interval-event, and root-motion policies; retained verified v1/v2 reads.
- Added weighted source/destination gameplay samples, deterministic merged interval-event delivery, and complete
  rollback validation for active blend policies.
- Connected animation blends to dual alpha-weighted sprite render submissions while preserving sorting and palette
  packets.
- Added a platform-neutral visual animation-graph workspace with layout frames, ports, edges, zoom/pan, marquee,
  multi-node drag, transition creation, search, copy/paste, delete, and live-state highlighting.
- Added step-up, ground snap with slope limits, moving-platform displacement, support normals, collision accounting,
  and residual-motion requeue to the neutral 2D root-motion adapter.
- Corrected the initial v3 writer to serialize every new blend-policy field and added deterministic cross-source event
  ordering and zero-scale residual protection.
- Added v2.12 codec/runtime/graph/motion regression coverage and retained the focused 10/10 v2.04-v2.12 sprite suite.

# v2.11.0 — Sprite Animation Authoring, Blending, and Collision-Aware Motion

- Added `DVE_SPRITE_MACHINE 2` graph positions, deterministic transition durations, normalized-time synchronization,
  interruption-source policy, v1 legacy verification, and migration.
- Added source/destination blend samples and weights, transition serials, non-interruptible transitions, and complete
  blend-state rollback snapshots.
- Added a transactional state-machine graph document with validated state/parameter/transition editing, reference
  repair, selection, save/open, bounded undo/redo, and external-change handling.
- Added deterministic side-effect command identities and a rollback-aware idempotency ledger for interval events and
  state transitions.
- Added shape-cast collision-aware 2D root motion with skin distance, bounded sliding, residual reporting, and a
  side-view-controller adapter.
- Added v2.11 codec, blending, authoring, rollback-ledger, and collision-motion regression coverage and retained the
  focused 8/8 v2.04-v2.11 sprite suite.

# v2.10.0 — Sprite Animation State Runtime and Gameplay Adapters

- Added pure forward/reverse interval queries for frame, combat-boundary, socket, property, and root-motion events
  across skipped frames, Once/Loop/PingPong playback, loop crossings, rollback, and resimulation.
- Reworked SpriteRuntime event delivery to retain every crossed owner-tagged boundary with stable track identity,
  authored/playback sequence identity, cycle, direction, and explicit truncation.
- Added bounded, content-hashed `DVE_SPRITE_MACHINE 1` assets with typed parameters, states, state-local/any-state
  transitions, priority, exit time, minimum residence time, playback speed, and trigger consumption.
- Added deterministic controller binding, transition events, force-state operations, complete rollback snapshots,
  and validation-backed restore.
- Added Transform/Velocity root-motion adapters for neutral Physics2D bodies and GameWorld objects.
- Added a GameWorld socket-attachment bridge with transform, visibility, missing-socket, and diagnostic policy.
- Added v2.10 interval/state/adaptor regression coverage and retained the focused 8/8 v2.04-v2.10 sprite suite.

# v2.09.0 — Sprite Precision Authoring and Gameplay Integration

- Added `DVE_SPRITE 4` stable 64-bit identities for combat, socket, property, and root-motion track items.
- Added deterministic legacy-ID migration, uniqueness validation, semantic hashing, and v1-v3 compatibility.
- Added stable native track rows, ID-addressed updates/removal, combat transform/range edits, duplication,
  X mirroring, keyboard nudging/resizing/rotation, and selected-item deletion.
- Preserved original identities through timeline reorders and allocated fresh identities for duplicated or split
  combat segments.
- Added skipped-frame and loop-aware root-motion interval accumulation, per-owner pending motion, consumption,
  flip/gameplay-plane conversion, and actor-orientation output.
- Added renderer-neutral socket attachments and gameplay debug packets with stable track identities.
- Added v2.09 codec, precision-authoring, runtime-integration, and native-panel regression coverage.
- Passed strict compilation and the focused 7/7 headless sprite regression set.

# v2.08.0 — Sprite Combat Tracks, Sockets, Properties, and Root Motion

- Added `DVE_SPRITE 3` combat windows, socket keys, typed properties, and root-motion records with strict
  validation, semantic hashing, and backward-compatible v1/v2 loading.
- Added deterministic gameplay-track sampling and flip-aware world transforms for runtime owners.
- Added transactional track authoring, bounded undo/redo, exact metadata-preserving timeline permutations,
  and contiguous combat-window reconstruction after reorder.
- Added the native Sprite Editor Tracks inspector, canvas combat/socket overlays, and four timeline marker lanes.
- Added v2.08 codec/runtime/authoring tests and extended the native authoring regression.
- Fixed Vulkan command-list virtual texture-state validation for barriers recorded before a render pass.
- Passed all eleven sprite-focused CTest targets, including Vulkan sprite render/readback.

# v2.07.0 — Indexed Sprite Rendering and Native Palette Authoring

- Added optional indexed sprite fragment bytecode, a palette storage-buffer layout, indexed blend pipelines,
  palette packet uploads, binding, cache reuse, telemetry, and explicit missing-packet validation.
- Hardened palette cache identity with content hash, resolved-color hash, and entry-count collision checks.
- Added HLSL and Vulkan GLSL indexed lookup sources and extended the checked-in shader inventory.
- Added transactional palette authoring with banks, swatches, transparency, cycles, deterministic import,
  fixed-step preview, bounded undo/redo, save/reload, and dirty external-change conflict handling.
- Added the native Sprite Editor Palette inspector and source-art palette creation/link workflow.
- Added palette-aware source/cooked-atlas preview and timeline virtualization beyond the visible frame strip.
- Added v2.07 palette-authoring and indexed-Null-RHI tests and retained all historical sprite regressions.
- Physical GPU indexed output remains unclaimed until compiled backend modules and device readback evidence exist.

# v2.06.0 — Indexed Sprite Palettes and Deterministic Cycling

- Added strict versioned `.dvepalette` assets with named banks, stable transparency, cycle tracks, and
  semantic hashes.
- Added deterministic exact/nearest/reject RGBA-to-index conversion with bounded diagnostics.
- Added four-file indexed sprite publication, palette-aware dependency evidence, and incremental verification.
- Added `DVE_SPRITE 2` palette references while retaining verified legacy v1 reads.
- Added runtime bank overrides, non-recursive swaps, 240 Hz cycle sampling, phase offsets, stable visual-state
  hashes, and deduplicated renderer-neutral palette packets.
- Prevented transparent-index remapping, extreme-clock overflow, and unnecessary packet churn between cycle steps.
- Made the existing RGBA-only RHI renderer reject indexed packets explicitly until GPU palette lookup exists.
- Added v2.06 runtime/cooker tests and corrected stale historical component-schema expectations.

# v2.05.0 — Sprite Cooking and Source Hot Reload

- Added deterministic PNG atlas publication and cooked `.dvesprite` output from retained source-space
  authoring data.
- Added a versioned cook manifest, content-derived dependency keys, output-hash verification, and
  incremental `up_to_date` behavior.
- Added staged three-file atlas/asset/manifest replacement with backup restoration on recoverable failure.
- Added portable source-file polling and transactional automatic reimport with selection/preview preservation.
- Added the lightweight `dve_sprite_tools` library and headless `dve_cook_sprite` CLI.
- Moved v2.04/v2.05 sprite-authoring tests outside the RHI-only test block and retained v2.04 regression
  coverage.

# v2.04.0 — Sprite Authoring, 2D Queries, Joints, and Platformer Collision

- Added native CPU sprite authoring for PNG, JPEG, and `.dvesprite` assets with canvas navigation,
  grid/freeform slicing, pivots, trim, multiframe timeline edits, playback, onion-skin state, atlas
  preview, selection-preserving reimport, and source-valid save behavior.
- Added neutral circle, capsule, box, and convex overlap/shape-cast APIs with filters, ignored bodies,
  sensors, stable ordering, tags, surface velocity, NativeTile support, and exact Box2D support.
- Added neutral revolute, prismatic, distance, weld, wheel, and motor joints with Box2D 3.1/3.2
  implementation, scene serialization, limits, motors, springs, break thresholds, and break events.
- Added wall, ceiling, and ledge state, surrounding-body velocities, moving one-way platforms,
  one-application support/conveyor transport, crush grace, and extended controller snapshots.
- Corrected release tests that relied on disabled `assert`, packed-preview reimport/save ownership,
  surface-velocity accumulation, and native moving one-way contact semantics.
- Passed focused release, strict, upstream Box2D 3.2, retained 3.1 contract, native X11, and
  ASan/UBSan validation. No GPU work was introduced.

# v2.03.0 — Menu and Settings UX

- Reorganized fourteen top-level editor menus into File, Edit, Create, View, Tools, Build, Window,
  and Help while preserving stable command IDs and shortcut bindings.
- Moved camera operations into View and grouped voxel, polygon, material, rendering, audio, and
  physics domains under sectioned Tools menus.
- Added descriptions and searchable keywords to menu actions and expanded menu search ranking to
  include action labels, sections, descriptions, and aliases.
- Completed native command-palette rendering with command and setting results, keyboard/pointer
  navigation, `>`/`@` result filters, session recents, and session favorites.
- Reworked settings into explicit User, Project, and Session tabs with category/search navigation,
  Changed and Advanced filters, inherited/default/source details, and dependency-aware editing.
- Added direct text entry for numeric and string options, transactional Apply/Cancel behavior,
  per-option and per-category reset-to-inherited actions, and keyboard accelerators.
- Fixed the native X11 host build by isolating Xlib's global `None` macro from DVE enum names.
- Passed focused release, desktop smoke, native X11, strict warning-as-error, and ASan/UBSan tests.

# v2.02.0 — Jolt Production Feature Completion

- Added editable Jolt height-field terrain backed by authoritative DVE samples and safe shape replacement.
- Added mutable compound bodies with stable child handles and runtime box add/modify/remove operations.
- Added Jolt soft bodies from DVE assets, vertex controls, impulses, runtime state, telemetry, and snapshot topology.
- Added CPU hair/rope strands through Jolt Cosserat rod constraints with optional pinned roots.
- Added wheeled `VehicleConstraint` integration with suspension, steering, brakes, handbrake, engine,
  transmission, differentials, wheel contacts, runtime input, and chassis-bound lifetime management.
- Added optional Jolt debug-render capture into backend-neutral CPU line, triangle, and text packets.
- Added bounded, versioned `.dvejoltreplay` serialization around complete solver snapshots.
- Added Windows MSVC and macOS arm64 Clang Jolt configure/build/test presets; execution remains pending.
- Fixed height-field edits that exceeded compressed source extrema by rebuilding the authoritative shape.
- Fixed soft-body activation and shape teardown lock ordering found by upstream runtime and sanitizer tests.
- Passed release, editor, combined Box2D/Box3D/Jolt, and ASan/UBSan validation against supplied Jolt 5.6.1.

# v2.01.0 — Upstream Jolt 5.6 Production Integration

- Expanded the optional Jolt dependency contract to verified 5.5.x-5.6.x APIs and pinned network
  fetching to v5.6.1; source versions are read from public Jolt header macros.
- Added backend-neutral 3D solver configuration for workers, substeps, velocity/position iterations,
  deterministic mode, sleeping, and continuous-collision policy, routed through play-in-editor.
- Added static triangle meshes, broadphase optimization, closest ray and sphere casts, caller-buffered
  AABB overlaps, exact Jolt version/configuration telemetry, and strict upstream tests.
- Added full solver snapshots including DVE interpolation/contact timing and virtual-character state.
- Added a `CharacterVirtual` capsule bridge with support velocity, slopes, stairs, floor adhesion,
  jumping, optional inner collision body, state restoration, and snapshot participation.
- Updated editor physics selection to Automatic/Jolt/Box3D/Reference and added Jolt solver controls.
- Added dedicated Jolt and all-physics CMake presets.
- Enabled matching Jolt C++ RTTI automatically for ASan/UBSan builds, preserving UBSan vptr checks.
- Passed five Jolt-focused release/editor tests, seven combined upstream physics tests, and an
  instrumented upstream Jolt ASan/UBSan run.

# v2.00.0 — Optional Box3D 3D Physics Integration

- Added optional upstream Box3D 0.1.x as a selectable `IRigidBodyWorld` backend.
- Added Automatic/Jolt/Box3D/Reference backend resolution and play-in-editor telemetry.
- Added compound bodies, DVE-authoritative mass/inertia, constraints, contact audio events, static
  triangle meshes, material-aware ray casts, AABB overlaps, and deterministic resource ownership.
- Added installed/source/pinned-fetch/legacy dependency routes and combined Box2D+Box3D validation.

# v1.99.0 — Upstream Box2D 3.2 Compatibility and Runtime Validation

- Expanded the optional Box2D dependency range to supported 3.1.x and 3.2.x releases with detected
  minor-version API selection; Box2D 2.x and untested 3.3+ versions fail configuration.
- Adapted pre-solve callbacks and fixed-rotation body creation to Box2D 3.2's point/normal callback
  and `b2MotionLocks` API while retaining the 3.1-compatible contract path.
- Enabled sensor-event participation on visitor shapes and disabled 3.2 contact recycling on DVE
  bodies so sensor begin/end and one-way drop-through behavior remain reliable.
- Removed Box2D API access from the locked pre-solve callback by caching body velocity and AABBs
  before each fixed solver step.
- Added a real-upstream Box2D integration suite covering body/collider types, motion locks, sensors,
  one-way landing/drop-through, slopes, ray and AABB queries, tile recooking, impulses, filtering,
  and malformed collider rejection.
- Built and linked the full DVE core against the supplied Box2D 3.2.0 source snapshot and passed
  focused ASan/UBSan with both DVE and Box2D instrumented. The retained 3.1 contract suite also passes.

# v1.98.0 — Native Slopes, Moving Platforms, Queries, and Incremental Tile Recooking

- Added `SlopeUpRight` and `SlopeUpLeft` tile collision values to the canonical tile-map format.
- Added 45-degree native swept-AABB slope resolution and diagonal ground normals.
- Added native kinematic box-platform collision, ray identification, support velocity, and
  controller-compatible moving supports.
- Added `Physics2DTileRegion`, `Physics2DOverlapHit`, neutral AABB queries, tile-map overlap tests,
  and regional tile-map updates with capability reporting.
- Reworked Box2D tile collision into 16x16 chunks with local solid/one-way merging, triangle slope
  shapes, and dirty-region recooking.
- Expanded tile-map, native physics, controller, and Box2D contract regression coverage.
- Passed strict compiler, integrated CMake/CTest, ASan, and UBSan validation. Actual upstream
  Box2D 3.1.1 execution remains a build-agent validation item because it was unavailable locally.

# v1.97.0

- Added a shared side-view character controller for NativeTile and Box2D worlds.
- Added acceleration/braking, coyote time, buffered and variable-height jumps, rise/fall gravity
  policies, terminal velocity, facing, and one-way drop-through.
- Added three-point ground probes, slope normals/tangent motion, moving-support tracking and
  inherited velocity, sensor-fed ladders, and neutral conveyor surface velocity.
- Added knockback and snapshot/restore replay boundaries.
- Added runtime gravity-scale mutation and the `dve.sideview_character` component schema.
- Added strict, integrated CMake/CTest, Box2D contract, and ASan/UBSan coverage.

# v1.96.0

- Added a backend-neutral sprite/2D physics world with selectable NativeTile and Box2D backends.
- Added optional Box2D 3.1.1 integration through installed, source-checkout, pinned-fetch, and
  controlled legacy dependency routes; Box2D 2.x is rejected.
- Added pixel/meter conversion, static/kinematic/dynamic bodies, multiple colliders, box/circle/
  capsule/convex shapes, sensors, collision filters, materials, events, impulses, ray casts, and CCD.
- Added greedy solid-tile rectangle cooking and merged one-way platforms with pre-solve pass-through
  and timed drop-through behavior.
- Added 2D physics world/body/collider component schemas and explicit capability reporting.
- Added strict warning-as-error, full-core, shipping-path, CTest, and ASan/UBSan coverage.

# v1.95.0

- Integrated the tracker/PSG and tile-world additions into the v1.94 source and build graph.
- Replaced callback-time vector copying with bounded fixed-capacity runtime instruments.
- Added fractional tick scheduling, wavetable oscillation, pitch/duty envelopes, stereo pan,
  transpose/fine tune, filters, bit/sample-hold reduction, and additional tracker effects.
- Added procedural coin, jump, laser, explosion, hit, power-up, confirm, and cancel SFX presets.
- Added direct `DecodedAudioAsset`, WAV, `.dvesample`, and complete SFX-bank export paths.
- Added canonical tile-map serialization, layered parallax, merged tile collision, swept AABB,
  one-way platform drop-through, camera follow/clamp, and parser allocation/overflow protection.
- Added expanded tests and reference `.dvechip`/`.dvetilemap` assets.

# v1.94.0

- Added `SpriteAuthoringSession` with atomic asset mutations, bounded named undo/redo, monotonic
  revisions, saved-content dirty tracking, and transactional `.dvesprite` open/save.
- Added bounded RGBA8 grid/freeform slicing with configurable geometry, stable naming/order,
  alpha-threshold trimming, optional empty-cell omission, and preserved source/pivot metadata.
- Added pivot, duration, event, clip, and timeline authoring operations with complete validation and
  rollback on duplicate names, invalid frame references, or out-of-bounds slices.
- Added playback, seek, pause, and exact selected-frame preview using the production sprite runtime,
  with preview packets exercised through `SpriteRhiRenderer` and the Null RHI.
- Added strict `-Werror`, ASan/UBSan, authoring, sprite runtime/RHI, platform/RHI, shader-contract,
  and live-ragdoll regression coverage.
- This is an original-game authoring workflow; native visual widgets, PNG/JPEG decoder binding,
  packing, palette tools, and onion-skin presentation remain explicit follow-on work.

# v1.93.0

- Added validated `VertexBufferLayoutDesc` and `VertexAttributeDesc` contracts to the public RHI,
  Null backend, and dynamically loaded Vulkan graphics pipeline path.
- Declared the 36-byte sprite vertex ABI as location 0 `float3` position, location 1 `float2` UV,
  and location 2 `float4` linear color; indexed draws now reject a mismatched bound stride.
- Added production HLSL/GLSL sprite sources, checked-in glslang-generated SPIR-V modules, hashes,
  and a bounded runtime module loader.
- Added a Vulkan offscreen sprite reference test covering texture sampling, vertex tint, indexed
  quad submission, integer scaling, and letterbox clearing, with optional PPM output.
- Passed strict source compilation, shader manifest validation, Null-RHI sprite execution,
  sprite-core and platform/RHI regressions, and Vulkan backend/test compilation.
- The Vulkan image test skips when a device is absent and fails closed under
  `DVE_REQUIRE_VULKAN=1`; this environment reports an incompatible driver, so no device image or
  physical-GPU result is claimed.

# v1.92.0

- Added a render-bridge sprite consumer with deterministic XY/XZ logical projection, dynamic
  vertex/index uploads, contiguous indexed batch submission, and explicit frame statistics.
- Added whole-target letterbox clearing plus fit/fill viewport and clipped-scissor recording.
- Added texture-view registration, nearest/linear sampling groups, a visible missing-texture
  fallback, and opaque/alpha/additive/multiply sprite pipelines.
- Extended RHI/Vulkan blend translation with multiply blending.
- Added high-DPI window-to-drawable-to-logical pointer mapping and tests for letterbox behavior.
- Added strict-warning, Null-RHI execution, Vulkan compilation, platform/RHI, sprite runtime, and
  live-ragdoll regression coverage.
- Physical-GPU sprite output, production shader binaries, atlas import, visual sprite authoring,
  tile maps, and deterministic 2D collision remain follow-on work.

# v1.91.0

- Added logical-resolution presentation, integer fit/fill and fractional scaling policies,
  letterbox-aware input mapping, and camera-relative XY/XZ pixel snapping.
- Added canonical, bounded, transactional `.dvesprite` assets with stable content hashes, atlas
  frames, trim/pivot metadata, animation clips, events, palette/material identity, and corruption
  rejection.
- Added deterministic sprite playback, one-shot/loop/ping-pong sampling, owner-ordered events,
  flip/tint state, renderer-neutral quad packets, and contiguous render batches.
- Added a shared semantic sort contract for sprites, flat meshes, particles, and full 3D models
  viewed through a 2D/2.5D camera.
- Added sprite editor classification/drop routing, a native `dve.sprite` component schema, focused
  tests, strict warning checks, and Jolt/control-rig/ragdoll regressions.
- This is an original-game toolkit, not a ROM emulator or a facility for copying existing games.
- Tile maps, deterministic 2D collision/controllers, native RHI packet consumption, sprite/tile
  authoring tools, and the original side-scroller vertical slice remain follow-on work.

# v1.90.0

- Implemented native Jolt 5.5.0 ball, hinge, cone-twist, and fixed constraints with authored local anchors, reference frames, and angular limits.
- Added stable constraint handles, explicit teardown, observable counts, handle reuse, and automatic cleanup before connected bodies are destroyed.
- Added native collision/settlement/recovery coverage for `RagdollRuntime`, plus fixed-reference restoration and all-kind constraint smoke coverage.
- Bounded authored joint angles to Jolt's valid domain and preserved transactional ragdoll failure behavior.
- Corrected the default Jolt temporary arena for the configured contact capacity, repaired the pinned-source CMake check, and decoupled headless Jolt smoke tests from image/benchmark dependencies.
- Per-ragdoll collision groups, motors, break thresholds, serialized tuning assets, replication, editor authoring, and broader collision shapes remain follow-on work.

# v1.89.0

- Added deterministic `.dvesoft` persistence with canonical output, stable field-wise hashes, bounded reads, transactional replacement/rollback, and corruption rejection.
- Added object-owned deformable runtime instances for raw cloth, rope, vegetation, deformable props, and smooth B-spline cloth proxies.
- Added live renderer-neutral triangle/line packets and particle/edge/triangle collision proxies with normals, bounds, frame identity, deformation hashes, and publication telemetry.
- Integrated deformable lifecycle, ticking, enable/disable, impulses, radial impulses, pinned-vertex targets, geometry classification, raycasts, sphere overlaps, and cleanup into `GameWorld`.
- Added closed surface faces to tetrahedral deformable boxes, `.dvesoft` editor routing, a `dve.soft_body` component schema, and focused optimized/sanitizer/regression coverage.
- Self-collision, two-way rigid coupling, tearing/plasticity, native renderer upload, visual authoring, Jolt execution, and physical GPU/device acceptance remain follow-on work.

# v1.88.0

- Added canonical, hashed, bounded, transactional `.dveui` serialization plus asset-backed runtime loading, safe hot reload, and stable-name focus restoration.
- Added localized UI text with locale fallback, placeholders, plural/select blocks, missing-key diagnostics, pseudo-localization, and live locale selection.
- Added clipped backend-neutral render primitives for ordinary and world-space canvases, including progress fills, slider geometry, text/image packets, and focus rings.
- Added pointer hit testing/capture, button and slider interaction, keyboard/controller action routing, UTF-8 text entry/backspace, modal focus scopes, and accessibility focus events.
- Added UI asset classification, editor file-drop routing, a `dve.game_ui` component schema, focused corruption/hot-reload tests, and v1.81 UI regression coverage.
- Native font shaping/rasterization, IME composition, touch gestures, a visual UI editor, and physical-device/GPU acceptance remain follow-on work.

# v1.87.0

- Added solver-neutral ball, hinge, cone-twist, and fixed rigid-body constraint handles with validation, explicit unsupported-backend behavior, transactional lifetime, and a deterministic live reference implementation.
- Added runtime ragdoll body/inertia construction from animated bone poses, joint-anchor derivation, activation velocities/impulses, per-body simulation weights, and stable-pose detection.
- Added face-up/face-down recovery classification, orientation-specific get-up clip selection, upright root alignment, captured physical pose blending, body/constraint teardown, and animation playback handoff.
- Integrated ragdoll bind/activate/recover APIs and lifecycle cleanup into `GameWorld`; ragdoll pose publication runs after physics, animation, and Control Rig evaluation, while accumulated root motion is discarded during physical ownership.
- Added focused constraint, partial blend, settlement, alignment, clip-selection, recovery, cleanup, GameWorld integration, strict-warning, sanitizer, and v1.81/v1.82/GameWorld regression coverage.
- The bundled Jolt adapter does not yet create constraints through this new neutral contract; it rejects live ragdoll activation explicitly until a tested Jolt constraint bridge is supplied.

# v1.86.0

- Added validated single- and multi-record Control Rig inspector transactions with complete-document rollback and one revision/undo entry per successful action.
- Added native inspector presentation for selected control/node properties, common values, mixed values, adjustment buttons, and active inline edits.
- Added bounded numeric adjustment, categorical/reference cycling, control/node name editing, and case-insensitive exact or unique-substring bone/control lookup.
- Routed native text input and editing keys to the Control Rig inspector and safely cancelled edits across document, tab, close, and selection transitions.
- Added focused atomicity, invalid-link rollback, multi-selection, undo, mixed-value, native text, bone-search, strict-warning, sanitizer, and v1.83-v1.85 regression coverage.
- Control animation curves, richer pickers/dropdowns, color editing, multi-item drag transactions, and advanced rig solvers remain follow-on work.

# v1.85.0

- Added multi-document Control Rig tabs with dirty and recovered state, recent-document ordering, de-duplicated opens, and guarded close behavior.
- Added paired `.dverig`/`.dverigui` save transactions with staging verification, backups, two-file commit, rollback, and retained backups if rollback itself cannot finish.
- Added timed sidecar autosaves, newer-autosave recovery, recovered-tab presentation, promotion through normal save, and stale-sidecar cleanup.
- Added asset-browser open routing for Control Rig runtime/layout assets with same-stem or unique-sibling skeleton discovery.
- Added focused failure-injection, byte-preservation, tabs, dirty-state, autosave, recovery, native-routing, strict-warning, sanitizer, and regression coverage.
- Explicit skeleton asset identity, full inspector editing, persistent recent lists, and control animation curves remain follow-on work.

# v1.84.0

- Connected control-rig authoring to the native editor canvas and input controller shared by platform hosts.
- Added graph painting data for cards, typed pins, links, comments, selection, validation badges, and context actions.
- Added pan, cursor-anchored zoom, card drag, marquee selection, link drag, disconnect, duplicate, delete, frame, undo, and redo interaction paths.
- Added a native rig preview with control picking and captured X/Y/Z transform-gizmo drags committed as one undoable control-default mutation.
- Added `Window > Control Rig Editor`, a `Ctrl+8` shortcut, a deterministic demonstration rig, focused interaction tests, and regression coverage.
- Asset-backed rig tabs, paired save transactions, animation curves, production 3D depth handling, and GPU solving remain follow-on work.

# v1.83.0

- Added snapshot-based undo/redo for control-rig graph edits, layout, comments, control shapes, transforms, matching, and space changes.
- Added typed graph pins/links with cycle-safe phase-aware topological compilation into the runtime rig node order.
- Added deterministic, hash-coupled `.dverigui` persistence for layouts, links, comments, and viewport control metadata.
- Added renderer-neutral viewport control geometry and ray picking for five control-shape families.
- Added preserve-model space switching, FK/IK matching, opt-in per-node evaluation traces, and rig-to-clip baking.
- Added focused authoring, persistence, graph-validation, viewport, tracing, matching, baking, and regression coverage.
- Native platform painting/pointer routing, scale/shear controls, full-body IK, and GPU rig evaluation remain explicit follow-on work.

# v1.82.0

- Added stable transform controls with model, bone, and nested control spaces plus bounded translation and rotation limits.
- Added deterministic three-phase control-rig execution and weighted Set Bone, Copy Bone, Parent, Aim, Two-Bone IK, and FABRIK nodes.
- Added an object-owned runtime with control overrides, reset/enable APIs, resolved control publication, diagnostics, and post-animation pose output.
- Integrated control rigs into `GameWorld` object lifetime and fixed-update ordering before socket attachment resolution.
- Added deterministic `.dverig` serialization with hashes, bounded reads, transactional replacement, asset classification, and a native component schema.
- Added focused validation, persistence, constraints, IK/FABRIK, space, phase, limit, and `GameWorld` integration tests.
- No visual graph authoring, GPU rig evaluation, scale/shear controls, full-body IK, or production deformation claim is included.

# v1.81.0

- Added typed, validated animation-controller state machines with prioritized transitions, conditions, triggers, playback speed, and crossfades.
- Added loop-safe root translation/rotation extraction, crossfade motion blending, pose-root removal, explicit consumption, and `GameWorld` application.
- Added a deterministic analytic two-bone IK reference with weighted solving and bounded optional stretch.
- Added validated solver-neutral ragdoll recipes, body-transform-to-local-pose conversion, and animated/simulated blend state.
- Added a retained gameplay UI/HUD runtime with screen/world canvases, recursive layout, clipping, bindings, focus navigation, interaction events, accessibility metadata, and renderer-neutral draw commands.
- Added a default health/prompt HUD, `GameWorld` UI publication, focused end-to-end tests, and strict-warning verification.
- No GPU skinning/UI rasterization, physical-device, platform-input, serialized controller/UI document, or live physics-constraint claim is included.

# v1.80.0

- Added deterministic skeleton and animation-clip assets with strict validation, bounded reads, hashes, and transactional writes.
- Added bind-pose, clip-sampling, shortest-arc rotation interpolation, two-pose and normalized pose-stack blending, and hierarchical model-pose evaluation.
- Added a four-influence CPU linear-blend skinning reference for positions and normals.
- Added a runtime animation service with clip playback, crossfades, pose publication, and object lifetime cleanup.
- Upgraded hard attachments to resolve named skeletal sockets when a parent owns an animation instance.
- Added a skeletal animator component schema and asset-browser classification for animation assets.
- Added focused tests while retaining explicit nonclaims for GPU skinning, glTF skin/animation import, scale tracks, root motion, state machines, IK, retargeting, and ragdolls.

# v1.78.0

- Added schema-driven native component inspection plus undoable component creation, removal, enable state, reordering, and typed property editing.
- Added normalized first-class tags, groups, and numeric layers across scene v5, Play-in-Editor, `GameWorld`, and Lua.
- Added synchronous lifecycle events for object, overlap, component, and pool transitions.
- Added deterministic fixed-capacity object pools with stable recycled IDs, prototype reconstruction, explicit exhaustion, and deep-copied voxel state.
- Upgraded prefab manifests to v2 with bounded snapshot variants, parent provenance, source content hashes, transactional source propagation, conflict previews, and preserved instance overrides.
- Hardened prefab updates to resolve newly added hierarchy dependencies without assuming template-ID order.
- Extended `dve_prefab_tool` with parent/depth inspection, typed variant creation, and transactional source-update output.
- Added optimized Lua-on/Lua-off and GCC ASan/UBSan/leak regression coverage. No new physical-GPU, Jolt-joint, or desktop-present claim is included.

# v1.77.0

- Added an open typed component model shared by editor objects, prefabs, Play-in-Editor, `GameWorld`, and Lua.
- Added attachment-local transforms, parent/child resolution, cycle rejection, preserve-world attach/detach, and runtime hard synchronization for attached dynamic bodies.
- Upgraded editor scenes to format v4 with backward-compatible reads and serialized attachments, components, prefab provenance, and per-instance overrides.
- Added deterministic `.dveprefab` capture/load/instantiate workflows with stable instance provenance and undoable native-editor authoring.
- Added `dve_prefab_tool` for headless capture, inspection, and instantiation.
- Added prefab recognition and native Assets-panel instantiation, hierarchy/inspector composition diagnostics, and Lua component query/mutation APIs.
- Added focused optimized, Lua-enabled, Lua-disabled, and sanitizer regression coverage. Attachments are hard transform relationships, not physics joints; nested prefabs and variants remain deferred.

# v1.76.0

- Added a persistent content/asset database with stable IDs, generations, tags, import provenance, and health states.
- Added text search, kind/tag/generated/issue filters, deterministic sorting, and manual-move recovery by unique fingerprint.
- Added dependency and reverse-reference extraction with unresolved project paths retained as explicit issues.
- Added transactional reference-safe rename/move with rollback and preserved asset identity.
- Added deterministic 64x64 PPM thumbnails and the headless `dve_asset_index` CLI.
- Integrated search, issue filtering, refresh, navigation, rename, reference counts, and thumbnail actions into the native Assets tab.
- Added focused optimized and ASan/UBSan regression coverage; LeakSanitizer initialization is unavailable in this container.

# v1.75.0

- Added an authoritative Play/Simulate session controller backed by an isolated runtime `GameWorld`.
- Added bounded deterministic fixed stepping, Pause, one-tick Step, and fixed-tick telemetry.
- Added complete document, dirty-state, selection, and editor-camera restoration on Stop or failed startup.
- Added explicit Keep Runtime Changes for synchronized authored-object transforms and deletions.
- Added native gameplay input routing, camera eject/possession, Lua startup/log/input integration, and HUD overlay state.
- Blocked authored-content commands and direct editing while a runtime session is active.
- Added optimized Lua-on/Lua-off and sanitizer regression coverage; Jolt and physical-GPU execution are not claimed.

# v1.74.0

- Added CPU per-pixel layer composition for paint, rust, dirt, wetness, snow, scorch, fracture exposure,
  and custom overlays with mask-authoritative height blending.
- Added reoriented normal composition, perceptual roughness, bounded metallic, additive emissive, and
  independent opacity policies shared by material layers and projected decals.
- Extended `.dmesh` to minor version 3 with serialized layer mask/height bindings and legacy 1.x reads.
- Added destruction-aware interior, fracture-replacement, and fracture-overlay material assignment.
- Added geometry-independent projected decals, deterministic CPU clustering, and polygon reference rendering.
- Added bounded offline subdivision and normal-direction vertex displacement with explicit visual/collision policy.
- Added generation-checked texture-residency references and stale-reference invalidation after hot reload.
- Expanded CPU material previews from twelve to fourteen views with layer-mask and layer-coverage diagnostics.
- Added focused optimized and sanitizer regression coverage; no new live-GPU execution is claimed.

# v1.73.0

- Added a deterministic CPU-only polygon-material authoring analyzer with text and JSON output.
- Added stable capability classification for CPU reference, live polygon, and shadow-caster execution.
- Added bounded material sample-cost estimates and diagnostics for mapping, transparency, tangents,
  texture color space, alpha masks, detail settings, parallax, and uniform material layers.
- Added `dve_material_audit` for offline validation and strict CI use without creating an RHI device.
- Added twelve CPU material diagnostic views and `dve_material_preview` for deterministic PPM exports.
- Added focused authoring and polygon-renderer regression coverage; no new GPU execution is claimed.

# v1.72.0

- Added renderer-owned `MaterialResourceResidency` shared by main and shadow material descriptor tables.
- Eliminated duplicate authored texture, texture-view, and sampler allocations across the two passes.
- Added asset-level retain/release accounting so resources survive one table's invalidation and are
  evicted only after the final table releases the asset.
- Preserved standalone table construction through private residency caches.
- Added shared-residency allocation/lifetime regression coverage under Null RHI and retained
  SwiftShader Vulkan descriptor validation.
- Verified nine focused tests and 42 shader source contracts.

# v1.71.0

- Enabled live base-color, metallic/roughness, normal, emissive, and opacity texture sampling.
- Added explicit sRGB-to-linear conversion for authored color channels.
- Published authored alpha cutoff and color-space flags in the fixed-size material mapping record.
- Added derivative-generated tangent-space normal mapping and masked-material discard semantics.
- Verified seven affected material/renderer tests and 42 shader source contracts.

# v1.70.0

- Added a persistent main-pass descriptor table with per-asset aligned GPU material and mapping buffers.
- Added per-material exact buffer ranges and nine authored texture-channel descriptors with neutral fallbacks.
- Bound the persistent descriptor to every live polygon draw and replaced fixed IBL material scalars with
  `GpuMaterialRecord` values.
- Added executable mapping-record reads and UV-transform evaluation in the live material shader.
- Combined material records/textures and layered CSM views into one descriptor set to retain a portable
  four-set graphics-pipeline layout.
- Added Null-RHI byte-exact record tests and SwiftShader descriptor/live-frame validation.

# v1.69.0

- Split cascaded shadows into independently sampled static and dynamic D32 atlases.
- Added region-scoped depth clears to the RHI and native Vulkan `vkCmdClearAttachments` execution.
- Added separate static/dynamic dirty-cascade sets and update telemetry.
- Dynamic dirty regions are cleared before redraw, eliminating stale depth from moved or removed casters.
- Static atlas content remains preserved unless an explicit static refresh is requested.
- Primary IBL material shading combines static and dynamic comparison visibility.

# v1.68.0

- Added persistent base-color/opacity texture descriptors for alpha-masked shadow casters.
- Added cooked image and sampler deduplication plus hot-reload invalidation.
- Replaced texture-alpha fallback accounting with actual sampled coverage in the caster shader.
- Added static-caster preservation during explicit partial cascade updates and rejected unsafe full-clear combinations.
- Added focused descriptor-cache, textured-mask, static-preservation, Null-RHI, and software-Vulkan validation.

# v1.67.0

- Added first-class depth-only render passes and depth-only graphics pipelines in Null RHI and Vulkan.
- Removed the cascaded-shadow color companion; the D32 atlas is now the only caster attachment.
- Added aligned per-object and per-cascade uniform-buffer ranges with explicit bind groups.
- Split live frame, object, and cascade shader constants into stable b8/b9/b10 contracts.
- Added opaque vertex-only shadow pipelines and alpha-tested depth-only caster pipelines.
- Added constant/vertex-alpha masked-caster selection and explicit telemetry when texture alpha still requires a fallback.
- Extended Vulkan native-limit reporting with uniform-buffer offset alignment.
- Added optional SPIR-V reflection parity checks and a fail-closed `--require-reflection` evidence mode.

# v1.66.0

- Added executable live skybox, IBL-material, and cascaded-shadow-caster frame recording.
- Added frame constant publication and graphics bind-group integration.
- Added a renderable CSM atlas companion target for the current one-color RHI contract.
- Added graphics HLSL contracts for skybox, IBL polygon shading, and CSM casters.
- Added focused Null-RHI frame orchestration tests and retained SwiftShader resource validation.

# Changelog

## v1.65.0 — Production Environment Lighting Integration

- Added persistent `.dveibl` environment-lighting assets with deterministic validation and content hashes.
- Added CPU/reference skybox rendering with rotation, intensity, and exposure controls.
- Added RGBA16F diffuse-irradiance, specular-prefilter, and BRDF-LUT upload plus reusable sampled bindings.
- Added RHI comparison samplers in Null RHI and Vulkan.
- Added a live D32 cascaded-shadow atlas resource, comparison-sampled binding, dirty-cascade frame plan, and world-to-atlas coordinates.
- Integrated environment IBL resources into the primary material-shading HLSL contract and added a skybox compute contract.
- Retained explicit limits: no physical-GPU images, live caster draws, reflection-probe capture, or Virtual Shadow Maps are claimed.

## v1.64.0 — Environment Lighting and Cascaded Shadows

- Added equirectangular HDR environment conversion to cubemaps.
- Added deterministic diffuse-irradiance, GGX specular-prefilter, and BRDF-LUT CPU/reference baking.
- Added metallic/roughness IBL evaluation and bounded reflection-probe selection with box projection.
- Added stable cascaded-shadow split planning, texel snapping, atlas viewports, transition bands, and dirty-cascade queries.
- Added HLSL contracts for cubemap IBL evaluation and cascaded-shadow PCF resolve.
- Added focused environment-lighting and CSM tests.

## v1.63.0 — Material Mapping Foundation

- Added deterministic UV transforms, world/object triplanar projection, detail mapping, and unified offset/steep/POM traversal.
- Extended polygon material assets and `.dmesh` minor version 2 with height and detail texture channels and mapping settings.
- Integrated the mapping modes into the CPU polygon reference renderer with triplanar normal reorientation, distance fading, and telemetry.
- Added byte-exact C++/HLSL mapping records and a checked-in material-mapping shader reference contract.
- Added RHI sampler handles/descriptions, sampled texture plus sampler descriptors, graphics bind groups, 2D-array views, cube textures, and cube views.
- Implemented those contracts in Null RHI and Vulkan and exercised the Vulkan path through SwiftShader software Vulkan.
- Retained explicit limits: height mapping is visual-only, triplanar POM is disabled, tessellation/displacement, IBL, lightmaps, CSM, and VSM remain future work.

## v1.62.0 — Production-Pass Specialization Conformance

- Added deterministic SPIR-V specializations bound to the exact source hashes of `fluoddity_clear_accumulation.hlsl`, `gabor_temporal_resolve.hlsl`, and `grid_fluid_divergence.hlsl`.
- Added complete 256-lane Fluoddity clear, 256-value Gabor temporal, and 64-cell grid-divergence CPU/device readback comparisons.
- Added explicit `production_pass_specialization` and `production_pass_specialization_readback` evidence classes so hand-specialized kernels cannot be mistaken for externally compiled production HLSL.
- Extended kernel evidence with source path/hash, compiler/specializer identity, and specialization assumptions; bumped native conformance schema to v3.
- Added strict `--require-production-readback` policies to the native and external validation runners.
- Extended Vulkan capability reporting with `timestampPeriod` and emitted converted nanosecond metrics in addition to raw device ticks.
- Added a SwiftShader CTest for the production specializations and retained explicit software-Vulkan classification.
- Verified 14 selected Release regressions and focused AddressSanitizer, UndefinedBehaviorSanitizer, and leak-detection runs.
- No physical-GPU, exact HLSL compiler, full production-pass, or performance claim is made.

## v1.61.0 — Simulation Kernel Conformance and Timestamp Evidence

- Added four exact integer algorithmic SPIR-V readback fixtures covering particle updates, second-difference stencils, temporal accumulation, and signed atomic scatter.
- Added array-level CPU/device mismatch, error, and hash evidence plus deterministic kernel bytecode metadata.
- Added `array_comparisons.csv` and `kernels.csv` to native evidence bundles.
- Implemented Vulkan timestamp query-pool creation, reset, writes, 64-bit resolution, handle validation, duplicate-index rejection, and queue-family valid-bit reporting.
- Added raw device-tick timing metrics to the generic compute probe and every algorithmic kernel fixture.
- Added strict `--require-kernel-readback` and `--require-timestamps` policies and external-workflow forwarding.
- Kept Null-RHI timestamps contract-only and rejected them for the strict device-timestamp requirement.
- Verified the focused Release matrix 15/15 and the sanitizer matrix 3/3 plus sanitized SwiftShader Vulkan compute/timestamp execution.
- Retained explicit software-Vulkan classification; no physical-GPU, nanosecond timing, or production-shader parity claim is made.

## v1.60.0 — Simulation Validation and Device Runner

- Added optional `DVE_BUILD_DEVICE_TEST_RUNNER` and a headless Null/Vulkan runner for device, simulation, and combined evidence suites.
- Added common metric comparators, deterministic fixture aggregation, transactional JSON/Markdown/CSV evidence writing, run IDs, timings, warnings, and artifact inventories.
- Extended RHI device capabilities with adapter class, IDs, API/driver versions, queue family, software/native-limit flags, device-local memory, native compute limits, and format capability reporting.
- Added executable Vulkan storage-write/readback and signed-atomic probes plus explicit Null-RHI contract-only behavior.
- Added bounded conformance fixtures for grid smoke, FLIP/APIC, XPBD cloth, PBF, VFX event accounting, and Fluoddity replay.
- Added external shader compilation evidence for Vulkan, DXIL, and Metal toolchains without treating missing compilers or source inspection as compiled evidence.
- Added a one-command external validation wrapper with strict physical-adapter and shader-backend requirements, root summary, SHA-256 inventory, and compressed evidence archive.
- Classified only integrated/discrete adapters as confirmed physical GPUs; CPU/software, virtual, and unknown adapters cannot satisfy `--require-physical`.
- Corrected prior SwiftShader terminology: SwiftShader execution is software Vulkan evidence, not physical-GPU validation.

## v1.59.0 — FLIP/APIC Liquid Foundation

- Added optional `DVE_ENABLE_FLIP_LIQUIDS` with a required dependency on the native grid-fluid module and no dependency on the PBF/XPBD liquid path.
- Added APIC particle state, stable IDs, affine particle-to-grid velocity reconstruction, PIC/FLIP blending, APIC gradient recovery, bounded affine rows, and midpoint advection.
- Added explicit air/liquid/solid classification, deterministic narrow-band particle level sets, free-surface matrix-free PCG projection, static solid rejection, and bounded velocity extrapolation.
- Added deterministic per-cell reseeding, overpopulation removal, global/per-cell budgets, surface snapshots, phase/velocity sampling, exact memory estimates, and detailed telemetry.
- Added a sixteen-kind FLIP/APIC GPU frame plan and common-RHI recorder integration.
- Verified the focused solver under Release, AddressSanitizer, UndefinedBehaviorSanitizer, and leak detection; five selected simulation/RHI tests pass together.
- Added the v1 contract, implementation report, release evidence, and remaining-work plan with explicit nonclaims for production GPU execution, surface meshing, fractional boundaries, secondary particles, and ST-FLIP.

## v1.58.0 — Native Grid Fluid Foundation

- Added optional `DVE_ENABLE_GRID_FLUIDS` with staggered 3D MAC velocity storage, centered density/temperature/fuel/flame/pressure fields, solid occupancy, validation, and exact memory accounting.
- Added shared CFL-derived adaptive substeps, midpoint semi-Lagrangian tracing, clamped MacCormack scalar/velocity correction, gravity, buoyancy, dissipation, combustion, and vorticity confinement.
- Added matrix-free diagonally preconditioned conjugate-gradient pressure projection with pressure warm starts, residual and divergence telemetry, and closed solid/domain boundaries.
- Added sphere emitters, deterministic PIC particle-to-grid deposition, PIC/FLIP blended grid-to-particle updates, and immutable renderer-facing volume snapshots.
- Added full grid-fluid GPU frame planning, common-RHI dispatch recording, and seven checked-in HLSL stages. Shader source contracts now cover 32 shaders.
- Retained the supplied Mantaflow Apache-2.0 license and audit notes while excluding Python scenes, generated-kernel tooling, GUI/Blender ownership, examples, and the complete Mantaflow runtime.
- Added research review, format contract, implementation report, remaining-work plan, focused CPU/RHI tests, and explicit nonclaims for production GPU smoke and complete FLIP/APIC liquids.

## v1.57.0 — B-Spline Cloth Foundation

- Added optional `DVE_ENABLE_BSPLINE_CLOTH` and a native quadratic open-uniform tensor B-spline surface implementation with analytic first and second parametric derivatives.
- Added conversion from regular DVE XPBD cloth grids to smooth C1 control surfaces, plus a runtime proxy that updates an independent-resolution embedded render/collision mesh from solver control positions.
- Added full reference quadrature and a BS-Cloth-inspired split reduced membrane/bending scheme with deterministic control-to-quadrature incidence maps.
- Added stretch/shear membrane energy, quadratic bending energy, precomputable bending stencils, lumped masses, and parametric seam penalties.
- Added deterministic patch, quadrature, and embedded-support hashes and focused continuity, rest-energy, deformation, mass, seam, and runtime-update tests.
- Retained BS-Cloth Apache-2.0 attribution while excluding the standalone renderer, YAML workflow, bundled assets/dependencies, sparse Newton solver, and IPC contact stack.
- Verified six simulation-adjacent tests and a separate two-test minimal build with all optional simulation modules disabled.

## v1.56.0 — Simulation Quality and Energy Compiler

- Added a shared adaptive simulation-step planner based on speed, characteristic length, maximum displacement fraction, maximum substep duration, and bounded substep count.
- Added an optional native Simulation Energy Compiler inspired by YASPS relational differentiation: deterministic scalar DAGs, analytic gradient/Hessian evaluation, fixed/free/affine parameterizations, joined spring compilation, DOF mapping, Hessian block incidence, and deterministic value-only HLSL emission.
- Added trilinear Fluoddity trail sampling, density interpolation, adaptive substep planning, and new validation/telemetry.
- Added true cloth dihedral-shell constraints, persistent XPBD multipliers, adaptive stepping, pressure, compliant unilateral ground contact, multiplier decay, friction, and Jolt-recipe preservation.
- Added bounded PBF neighbors and overflow telemetry, iterative neighbor rebuilds, correction clamping, normalized viscosity, vorticity confinement, adaptive stepping, and a future ST-FLIP temporal sample/deposition plan.
- Added VFX point-attractor, velocity-limit, and sphere-collision modules plus bounded spawn/death/collision events and a GPU BuildEvents pass.
- Added SIGGRAPH 2026 simulation research review, energy-compiler contract, implementation report, remaining-work plan, and retained YASPS MIT license notice.
- Verified 18 selected enabled-feature tests and 2 modular disabled-feature tests.

## v1.55.0 — Simulation Foundations

- Added a bounded Fluoddity CPU reference solver, signed fixed-point trail accumulation, resolve/diffusion, a 944-byte CPU/HLSL constant contract, three checked-in HLSL stages, and common-RHI GPU resource/dispatch recording.
- Added native cloth, rope, vegetation, and tetrahedral deformable-prop assets, a bounded XPBD runtime, and a tested Jolt-facing soft-body topology recipe.
- Added a DVE-native VFX module graph, deterministic compiler, CPU preview interpreter, and reset/spawn/update/compact/sort/indirect GPU pass plan.
- Added a bounded PBF/XPBD liquid prototype with spatial hashing, density constraints, correction, viscosity, boundaries, telemetry, and a complete GPU frame plan.
- Added independent feature switches and focused tests; nine simulation/regression tests and all 25 shader source contracts pass.
- Kept physical shader compilation/execution, a linked Jolt soft-body backend, production VFX kernels, and production liquid GPU performance explicit as follow-on work.

## v1.54.0 — Vulkan Compute Contract Foundation

- Added software-Vulkan-executed compute pipelines, descriptor-set layouts, bind groups, compute
  command recording, descriptor binding, dispatch, submission, and dispatch statistics.
- Added an RHI-level compute bind-group operation and deterministic Null-backend checks for queue
  legality, render-pass exclusion, required descriptor sets, and exact layout compatibility.
- Added compute pipeline descriptor-layout declarations rather than relying on implicit shader slots.
- Added signed `R32Sint` and `RGBA32Sint` texture formats and queryable sampled/storage/atomic/render/
  depth capabilities sourced from Vulkan physical-device format properties.
- Added Vulkan descriptor writes for uniform buffers, read-only and read-write storage buffers, and
  storage textures; sampled textures await a first-class sampler contract.
- Added a SwiftShader software-Vulkan regression that writes a storage buffer, binds a signed 3D
  storage volume, validates signed storage-image atomics, and proves a 32-invocation signed atomic sum.
- Kept the existing Fluoddity and Gabor contracts regression-clean. This release established a shared
  compute contract and software-Vulkan execution path, not physical-GPU validation or completed
  Fluoddity/Gabor production execution.

## v1.53.0 — Fluoddity Native Simulation Foundation

- Added the optional `DVE_ENABLE_FLUODDITY` native rule, preset, cooker, runtime-planning, and test
  module without importing the supplied Python/OpenGL/OptiX application stack.
- Added version-7 JSON migration and canonical little-endian `.dfluoddity` assets with structured
  ten-center/120-float Fourier rules, sweep/jitter data, source-compatible float seeds, cohorts,
  boundary/initial conditions, appearance data, notes, compatibility metadata, stable relative
  provenance, semantic hashes, transactional writes, and strict corruption/trailing-data rejection.
- Added CPU reference evaluation, source-compatible rule generation/mutation, parameter sweep/jitter
  evaluation, and low/medium/high/custom memory profiles.
- Added `RuntimeFluoddityWorld` with stable field identities, content-addressed rules, play/pause/step/
  reset, capped fixed-step planning, dispatch and trail-ping-pong contracts, allocation resets, and
  global GPU-budget rejection.
- Added `dve_cook_fluoddity`, the approval-bound `dve.project.cook_fluoddity` MCP operation, and
  allowlisted build/test tasks.
- Cooked and bundled all 151 supplied presets. Directory-location-independent repeat cooking is
  byte-identical; 151 post-write checks passed with 71 explicit legacy-metadata warnings and no
  errors.
- Retained the Fluoddity3D MIT license and source-compatible RGB-velocity/zero-alpha trail semantics.
- No physical Vulkan/D3D12/Metal simulation, editor object, rendering, collision field, checkpoint,
  or networking result is claimed.

## v1.51.0 — Gabor Volume Production Integration

- Added bounded ASCII and binary-little-endian Gabor Fields PLY import, pyramid merging, canonical
  field-by-field `.dgabor` serialization, legacy DGABOR1 migration, deterministic content hashes,
  transactional writes, and strict corruption/trailing-data validation.
- Added first-class editor Gabor-volume objects with transforms, bounds, viewport picking and
  framing, cloning, scene save/load, undoable material edits, project-local import/cooking, and a
  native Gabor Volume Inspector.
- Added Create and Rendering menu controls for empty/imported volumes, absorption,
  emission-absorption, experimental scattering, quality, continuous LOD, temporal accumulation,
  volume shadows, and detailed project settings.
- Added `RuntimeGaborVolumeWorld` snapshots with stable object identities, content-addressed assets,
  visibility, selection, temporal, and scene/volume shadow participation.
- Added transactional RHI residency for packed Gabor primitives and level ranges, bounded screen-tile
  work planning, hybrid-scene depth composition contracts, temporal-history resets, volume-shadow
  scheduling, and ordered compute dispatch recording.
- Added five Gabor HLSL stages and shared ABI declarations. The shader source-contract manifest now
  validates 22 shaders.
- Added `dve_cook_gabor`, the approval-bound `dve.project.cook_gabor` MCP tool, and allowlisted
  Gabor build/test tasks.
- Added deterministic CPU previews, runtime/editor/AI regression tests, and focused Clang
  AddressSanitizer/UndefinedBehaviorSanitizer coverage.
- Retained the supplied Gabor Fields MIT license and source notice without embedding its custom
  Mitsuba/DrJIT/CUDA optimization stack.
- No physical Vulkan, Direct3D 12, or Metal image/timing result is claimed. Multiple scattering and
  optimized projected-ellipsoid tile assignment remain experimental production work.

## v1.50.0 — 3D Text Production Integration

- Added transactional GPU residency for Slug curve/band atlases, analytic face quads, and PBR extrusion-side geometry.
- Added ordered render recording for analytic back faces, extrusion sides, analytic front faces, and optional selection overlays with per-object constant bindings.
- Added runtime 3D-text objects, live snapshot/frame-plan conversion, stable object/material identities, visibility, selection, shadow, and GI flags.
- Added native editor creation, inspection, recooking, undo/redo, scene persistence, viewport picking/framing, software preview proxies, and project-local font dependency packaging.
- Advanced `.dtext` to minor version 1 with separate face and side material IDs while retaining v1.49 minor-version compatibility.
- Added `RGBA16Float` and `RG16Uint` RHI texture formats required by Slug atlases, including Null and dynamically loaded Vulkan backend support.
- Expanded the command-line and approval-bound MCP cooker with alignment, fill rule, spacing, and separate face/side material controls.
- Added focused runtime, renderer, editor, MCP, Vulkan compile, shader-contract, and sanitizer validation while keeping physical GPU image evidence explicit as follow-on work.

## v1.49.0 — Slug-Based 3D Text Foundation

- Integrated the supplied open-source Slug vertex and pixel algorithms with retained dual MIT/Apache-2.0 attribution.
- Added bounded TrueType `glyf` parsing, Unicode cmap formats 4/12, metrics, legacy kerning, simple glyphs, and common composite transforms.
- Added Slug curve/band atlas cooking, front/back face render packets, standard-material extrusion side meshes, bounds, and deterministic previews.
- Added the versioned `.dtext` asset format, `dve_cook_text3d` command-line cooker, and approval-bound MCP/assistant cooking tool.
- Added graphics-stage shader-contract and dry-run compiler support for the new Slug vertex/pixel shaders.
- Added synthetic-font round-trip tests and real-font demonstration evidence without redistributing a font file.
- Kept CFF/CFF2, complex OpenType shaping, bidirectional text, variable-font axes, a native editor text panel, and physical GPU pipeline evidence explicit as follow-on work.

## v1.48.0 — Authenticated UDP Transport and Multiplayer Services

- Added a real nonblocking UDP implementation of `IReplicationTransport`.
- Added reliable ordered fragmentation, per-fragment acknowledgements, retransmission, bounded reassembly, and unreliable sequenced snapshots.
- Added pre-shared-key HMAC-SHA-256 peer authentication, session-key derivation, fresh nonces, session IDs, authenticated endpoint learning, replay-resistant nonce history, retries, deadlines, and explicit authenticated/not-encrypted capabilities.
- Added server input ownership and tick/rate/magnitude validation with live character application.
- Added remote-player snapshot interpolation, bounded extrapolation, stale rejection, and teleport resets.
- Added SHA-256-verified late-join checkpoint chunking and bounded post-baseline delta buffering.
- Added a localhost UDP demonstration, named AI validation task, normal regression matrix, and focused AddressSanitizer/UndefinedBehaviorSanitizer coverage.
- Kept encryption, NAT traversal, matchmaking, and platform account services explicit as follow-on work.

## v1.47.0 — Prediction Execution, Brick Repair, and Transport Interface

- Added a socket-neutral `IReplicationTransport` contract and peer-bound endpoint wrapper.
- Adapted the deterministic loss/reorder/duplication simulator to the shared transport interface.
- Added targeted `GameplayRuntime` prediction and authoritative-state entry points.
- Added executed client reconciliation: apply authority, trim acknowledgements, replay unacknowledged input, and rebuild prediction history.
- Added complete 8x8x8 brick snapshot extraction with authoritative generation and stable content hashes.
- Added hash-verified brick-repair response encoding, authority construction, replica application, and transactional collision rebuild.
- Preserved dynamic rigid-body velocity across authoritative brick repair.
- Added focused network-runtime tests, named validation tasks, documentation, and reproducible evidence.

## v1.46.0 — Exact Polygon Character Collision and Network Simulation

- Replaced sampled polygon capsule collision with exact segment-to-triangle distance queries through the polygon BVH.
- Added continuous capsule sweeps by conservative advancement with bounded binary refinement, stable triangle/material identity, and polygon depenetration.
- Added playable character regression coverage on exact polygon floors and transformed polygon objects.
- Advanced gameplay replication packets to v2 by attaching explicit brick coordinates while retaining v1 decode compatibility.
- Added a deterministic datagram simulator with latency, bandwidth, loss, duplication, reordering, fragmentation, reassembly, reliable ordered ACK/retransmit, and unreliable sequenced delivery.
- Added selective brick-repair request codecs and causal destruction divergence tracking.
- Added bounded client-prediction history and authoritative reconciliation output with replay tails and hard-snap policy.
- Added named AI validation tasks, a reproducible network demonstration, and focused sanitizer coverage.
- Kept Jolt physical character-query validation explicit as unfinished because the pinned Jolt 5.5.0 dependency and physical solver environment were unavailable in this sandbox.

## v1.45.0 — Playable Runtime Foundation

- Added a fixed-step capsule character controller with acceleration, braking, sliding, steps, slopes, grounding, jumping, crouching, depenetration, moving-platform inheritance, and destructible-floor recovery.
- Added player identities and exclusive player-to-pawn possession.
- Added box/sphere trigger volumes with character/tag filtering, Enter/Stay/Exit events, one-shot behavior, and serialized state.
- Added deterministic fixed-tick input recording and replay.
- Added Lua bindings for characters, players, possession, input, triggers, state inspection, and trigger callbacks.
- Added a versioned binary gameplay replication-state contract with stable network IDs, quantized character state, support IDs, trigger state, ordered destruction edits, brick revisions, content hashes, strict decoding, and stable hashes.
- Added bounded AI validation tasks for the playable runtime and replication contract plus a reproducible demonstration program.
- Added normal and focused Clang AddressSanitizer/UndefinedBehaviorSanitizer regression coverage.

## v1.44.0 — Default Global Illumination, Low Reflectivity, and Shadow Modes

- Enabled bounded voxel one-bounce global illumination by default, with ambient-hemisphere and disabled alternatives.
- Added hard, soft-area, contact, and hybrid soft-plus-contact directional shadow modes plus an explicit disabled mode.
- Added validated GI/shadow controls to `RenderEnvironment`, editor settings, Lua scripting, GPU constant packing, and the shared HLSL ABI.
- Added deterministic CPU voxel reference tracing for shadows and one-bounce diffuse GI, including ray-budget and hit telemetry.
- Added a backend-independent seven-pass voxel lighting frame plan and source contracts for shadow/GI generation, resolution, and primary shading.
- Changed `Engine/Materials/StandardSurface` to the named low-reflectivity default: roughness 0.62 and specular 0.25, approximately 2% dielectric F0.
- Expanded the shader manifest to 15 checked-in shaders and added C++/HLSL render-environment ABI validation.
- Added normal and focused AddressSanitizer/UndefinedBehaviorSanitizer coverage for environment packing, all shadow scheduling modes, GI scheduling, hybrid reference composition, and material defaults.

## v1.43.0 — New Project Defaults and Starter Oval

- Replaced the desktop editor demo scene as the new-project default with one selected, framed voxel ellipsoid named `Starter Oval`.
- Added the canonical opaque `Engine/Materials/StandardSurface` dielectric material and synchronized editor, master-material, and GPU fallback values.
- Changed the active paint material for a new project to Standard Surface and separated New Project from New Scene destructive actions.
- Added neutral studio environment defaults while retaining the existing editor floor/grid rather than inserting extra editable scene objects.
- Added the semantic-aware unlit `Engine/Shaders/TexturePreview` contract with color, linear-data, tangent-normal, alpha, channel, false-color, HDR-exposure, mip, array-layer, and UV controls.
- Added deterministic missing-texture fallbacks for color, normal, metallic/roughness, ambient occlusion, opacity, and emissive semantics.
- Expanded the shader manifest to 12 checked-in shaders and added drift tests across the new-project material definitions.
- Added normal and focused AddressSanitizer/UndefinedBehaviorSanitizer coverage for the editor template, material defaults, environment defaults, texture preview, and fallback resources.

## v1.42.0 — Deferred UX and Rendering Completion

- Unified native menu rendering and hit testing; added responsive minimum-width layout, hover switching, keyboard navigation, Alt mnemonics, wheel scrolling, clamping, and context-menu keyboard interaction.
- Added stable object/material IDs to camera collision hits and frame-local occluder-fade requests.
- Added transactional replace-if-changed texture residency with unchanged detection, queued replacement, resident replacement, failure preservation, and replacement telemetry.
- Added linear-light base-color/emissive mip filtering and renormalized normal-map mip filtering.
- Added material shading/blend badges and semantic warnings in the Assets panel.
- Added shader inventory/include/entry/thread/binding validation plus C++/HLSL material ABI and parameter-capacity checks that run without a GPU compiler.
- Regenerated the shader manifest to correct bloom/tonemap thread groups and populate resource bindings.
- Added normal and AddressSanitizer/UndefinedBehaviorSanitizer coverage for the affected camera, editor, texture, polygon, and material paths.

## v1.41.0 — Live-Editor MCP Host

- Added a `NativeEditorController`-owned MCP host that exposes the actual open editor document to external local MCP clients.
- Added an authenticated stdio proxy over owner-only Unix-domain-socket IPC, discovered by canonical project root without placing credentials in process arguments.
- Marshalled all external requests onto the editor update thread through bounded queues, preserving single-thread document ownership.
- Added per-connection MCP protocol sessions, strict initialize/initialized lifecycle checks, supported-version validation, request deadlines, message/client/queue limits, and clean shutdown.
- Added **Window > Live MCP Host** and desktop command-line options for normal and read-only hosting.
- Unified external MCP mutation proposals with the editor Assistant review queue while preventing cross-client approval replay and unrelated embedded-client continuation.
- Added the non-secret `dve://editor/live-mcp` resource and live-host status tool.
- Added normal integration, authentication, protocol-state, undo-stack, proxy, X11 smoke, and focused AddressSanitizer/UndefinedBehaviorSanitizer tests.
- Unix-like IPC is implemented and tested; Windows named pipes and physical macOS validation are deferred.

## v1.40.0 — Closed-Loop AI Engineering Workflow

- Added exact tool, canonical-argument, and client/session-actor approval binding, preventing an approved request ID from being replayed with modified arguments or by another local client.
- Added unified-diff preview and transactional multi-file patch application with required base hashes, strict context matching, changed-file summaries, rollback bundles, and guarded byte-identical restoration.
- Added fixed-name, project-confined validation tasks with direct process launch, deadlines, bounded combined logs, structured exit results, and no assistant-supplied executable or arguments.
- Moved editor assistant calls to the worker-task system with UI-thread polling, cancellation state, bounded curl retries/timeouts, and late-result suppression.
- Added Responses model and token-usage telemetry across multi-turn tool calls.
- Added read-only MCP resources for validation tasks, pending proposals, editor context, settings, and audit history.
- Added richer editor context and settings tools while retaining undoable scene mutations and approval requirements.
- Added deterministic release-manifest generation and exact payload verification.
- Added regression coverage for altered-argument and cross-actor approval replay, stale patch hashes, post-edit rollback refusal, MCP resources, named-task execution, asynchronous cancellation, active-panel reattachment rejection, and editor context/settings.
- Focused AI assistant, GameWorld, and editor regression suites pass in the sandbox build configurations; Clang AddressSanitizer/UndefinedBehaviorSanitizer runs pass with leak detection disabled because LeakSanitizer crashes in this container before the tests execute.

## v1.39.0 — AI Assistant, MCP Server, and OpenAI Responses Bridge

- Added a shared AI tool registry used by the editor panel, MCP server, and OpenAI Responses API client.
- Added 10 bounded standalone tools and 9 additional undoable editor-document tools.
- Added JSON-RPC MCP initialize, ping, tools, resources, and prompts support over stdio.
- Added a loopback Streamable-HTTP adapter with bearer-token, origin, session, size, and content checks.
- Added a native editor AI Assistant panel with approval cards and automatic continuation after Approve/Deny.
- Added project-root confinement, text/source allowlists, size limits, hash-guarded atomic writes, exact-call approval replay, destructive-action confirmation, and JSONL audit logging.
- Added an OpenAI Responses client using function tools, `function_call_output`, `previous_response_id`, `gpt-5.6`, and `OPENAI_API_KEY`.
- Added editor-scene inspection, selection, creation, rename, transform, delete, undo, and redo tools.
- Added reproducible MCP, API, and native-editor demos plus configuration examples and format specifications.
- Full HYBRID Lua/editor/audio/Vulkan suite passes 48/48; focused AI core and GameWorld sanitizer suites pass 2/2.
- No live OpenAI request was claimed or performed without a user-supplied API key.

## v1.38.0 — Context-Aware Keyboard and Mouse Shortcuts

- Replaced native-editor menu-string scanning and hard-coded productive keys with a typed command and binding registry.
- Added 13 input contexts, context precedence, primary/secondary bindings, keyboard, Mouse1–Mouse4, wheel, press, hold, and double-click gestures.
- Added DVE Default, Unity Familiar, Unreal Familiar, Accessibility One-Handed, and Blank Custom profiles.
- Added conflict detection, explicit override behavior, transactional `.dveshortcuts` import/export, profile duplication/reset, command/key/category search, and a visual keyboard map model.
- Added a searchable modal shortcut editor with profile/context navigation, capture, unbind, reset, conflict status, mouse/wheel capture, and active menu-label synchronization.
- Added continuous fly-navigation key state, context-specific Mouse4 behavior, configurable double-click timing/distance, and double-LMB frame selection.
- Preserved Window-panel shortcuts while moving camera-bookmark storage to Ctrl+Alt+1…9 to remove viewport shadowing.
- Full HYBRID Lua/editor/audio/Vulkan suite passes 47/47; four focused editor suites pass under AddressSanitizer and UndefinedBehaviorSanitizer.

## v1.37 — Camera rendering, constant-speed sequencing, scripting, and safety

- Added per-viewport camera frame planning, aligned GPU packet uploads, temporal reset reasons, split-screen and named-target Vulkan execution.
- Added bounded physical depth-of-field processing and focus-plane preview.
- Added camera sequence format 2 with constant-speed arc-length spline evaluation, events, and preserved metadata.
- Added transactional multi-select, ripple, roll, trim, move, retime, undo, and redo operations to the camera sequencer model.
- Added runtime camera save state format 2 and Lua playback/state/shake/accessibility control.
- Added pull-forward, preserve-framing, fade-occluder, and shoulder-swap obstruction strategies plus camera-volume constraints.
- Added standard, reduced-motion, and photosensitive camera profiles with horizon lock and coordinated shake/bloom/DOF limits.

## v1.36.0 — GameWorld Cameras, Split-Screen Outputs, and Cinematic Sequencing

- Added GameWorld-owned camera runtime viewports with independent directors, output channels,
  normalized split-screen rectangles, named render targets, live object target bindings, per-rig
  post-processing, final camera matrices, and temporal-history reset generations.
- Added a unified GameWorld voxel/polygon camera collision adapter using bounded sphere-sweep probes.
- Added deterministic `.dvecamseq` assets with Catmull-Rom dollies, physical lens/focus/post keys,
  rig/dolly shots, cuts and blends, markers, state triggers, play/pause/seek/loop, and runtime playback.
- Added thin-lens depth-of-field ranges, aspect/safe-frame guides, frustum visibility, and projected
  screen-size helpers for camera culling and LOD validation.
- Added an editor sequencer model with snapping, shot/key editing, timeline layout, and
  OpenTimelineIO-compatible metadata export.
- Added settings dependencies, changed-only views, category resets, named profiles, orphan
  diagnostics, and additional camera output/accessibility/sequencer settings.
- Full HYBRID Lua/editor/audio/RHI matrix passes 44/44; six focused camera/settings/GameWorld suites
  pass under AddressSanitizer and UndefinedBehaviorSanitizer.

## v1.35.0 — Camera Director, Physical Lenses, and Unified Settings

- Added virtual camera rigs, priority/channel arbitration, state-driven selection, custom blends,
  follow/orbit/third-person composers, target prediction, screen offsets, dead/soft zones,
  collision pull-in with damped recovery, physical lens metadata, and layered camera shakes.
- Added backward-compatible `.dvecamera` format 2 with complete rig, lens, framing, collision,
  blend, and state persistence.
- Integrated authored camera rigs into the editor with picture-in-picture previews, frustums,
  bookmarks, camera modes, lens controls, collision toggles, and live navigation preferences.
- Added a centralized typed settings registry with four scopes, capability filtering, validation,
  search, advanced filtering, staged Apply/Discard, apply-policy and restart indicators, and
  transactional `.dvesettings` persistence.
- Expanded and organized the editor menu hierarchy across camera, viewport, rendering, geometry,
  materials, audio, physics, build, accessibility, diagnostics, and plugins.
- Full HYBRID Lua/editor/audio/RHI matrix passes 41/41; focused camera/settings/editor sanitizer
  suites pass.


## v1.34.0 — Vulkan Offscreen Graphics, Full Material Textures, and Real Hybrid Reference

- Advanced the Vulkan RHI from buffers to offscreen color/depth textures, image transfer/readback,
  views, render passes, framebuffers, shader modules, graphics pipelines, dynamic viewport/scissor,
  vertex/index binding, indexed instanced draws, synchronization, and readback statistics.
- Added a SwiftShader-backed required-execution Vulkan test with a source-built minimal SPIR-V
  program and pixel-level CPU coverage oracle. This is real Vulkan execution on a software ICD, not
  a physical-GPU timing claim.
- Advanced `.dmesh` to minor version 1 while retaining minor-version-0 loading. Added UV1 and
  base-color, metallic/roughness, normal, emissive, and opacity texture bindings with independent UV
  sets, color-space metadata, and normal scale.
- Added multi-channel texture sampling, tangent-space normal evaluation, alpha masks, emissive maps,
  correct sRGB decoding, generated mips, and screen-space mip selection to the CPU reference renderer.
- Added a bounded texture residency manager with asynchronous request queues, explicit RHI pumping,
  mip generation, LRU eviction, byte budgets, duplicate detection, and compression-fallback telemetry.
- Added an authoritative `VoxelObject` reference renderer and direct shared-target hybrid rendering,
  replacing the prior synthetic voxel layer for validation of normalized depth, IDs, and translucent
  blending.
- Full Lua/editor/audio HYBRID suite passes 39/39; dependency-minimal HYBRID suite passes 32/32;
  VOXEL/POLYGON profile checks and six focused ASan/UBSan suites pass.
- Presentation, physical-device performance, production descriptor/material shaders, D3D12/Metal,
  animation, Jolt mesh/convex collision, and graphical hybrid authoring remain explicit future work.

## v1.33.0 — Polygon Rendering Reference and Hybrid Scene Packaging

- Extended the RHI with render-pass, graphics-pipeline, depth, viewport/scissor, texture transfer,
  vertex/index binding, and indexed-instanced draw contracts.
- Added strict Null-RHI graphics-state/range validation and graphics submission telemetry.
- Added a deterministic CPU polygon rasterizer with shared materials/environment, depth, culling,
  masking, sorted translucency, object/material IDs, sRGB base-color textures, mip generation, and
  screen-space mip and mesh-LOD selection.
- Added normalized-depth voxel/polygon layer composition and reproducible visible evidence.
- Added immutable deduplicated mesh heaps and separate instance buffers.
- Added material-aware triangle BVH ray/overlap queries integrated into `GameWorld`.
- Added content-hashed `.dvescene` mixed-scene manifests with build-profile checks and dependencies.
- Preserved VOXEL, POLYGON, and HYBRID profiles; full HYBRID Lua/audio/editor suite passes 36/36.
- Recorded that Vulkan/D3D12/Metal raster execution, full PBR texture sets, animation, convex
  collision, and graphical hybrid authoring remain future work.

## v1.32.0 — Hybrid Voxel and Polygon Geometry Profiles

- Added `DVE_GEOMETRY_MODE=VOXEL|POLYGON|HYBRID`, with hybrid as the default and explicit runtime
  rejection of unsupported geometry kinds.
- Added versioned `DVEMESH1` `.dmesh` assets with semantic content hashes, strict bounds/count/
  reference validation, shared DVE materials, indexed submeshes, normals, tangents, UVs, colors,
  and retained RGBA texture payloads.
- Added `dve_cook_mesh` for GLTF 2.0, GLB, and geometry-only OBJ preservation alongside the existing
  model-to-voxel cooker.
- Added deterministic reference mesh publication, a unified hybrid geometry world, polygon static
  and dynamic AABB collision descriptors, triangle raycasts, sphere overlap, and `.dmesh` dispatch
  through `GameWorld` and Lua asset spawning.
- Added polygon vertex/index/draw/material RHI buffers with byte-exact Null-device readback.
- Added explicit CMake presets for voxel-only, polygon-only, and hybrid Linux/headless builds.
- Added profile-aware polygon tests, complete hybrid regression coverage, sanitizer coverage, and a
  reproducible voxel-plus-polygon evidence demo.
- Hardware raster drawing, texture upload/sampling, skeletal animation, mesh LOD, and final
  triangle/convex collision remain v1.33 work rather than being overstated in this checkpoint.

## v1.31.0 — Material Parameter Collections, Clear Coat, Foliage, and Layers

- Added a fixed-slot Material Parameter Collection with canonical time, wind, wetness, snow,
  time-of-day tint, and wind-direction parameters plus custom scalar/vector slots.
- Bridged canonical and explicitly material-bound scalar values through `world.set_global` without
  consuming GPU slots for unrelated gameplay globals; added vector global scripting and readback.
- Added versioned `.dvematparams` save/load with stable-slot checks, corruption/trailing-data
  rejection, and transactional MaterialLibrary reload.
- Added actual Clear Coat and Two-Sided Foliage shading paths rather than enum-only presets. Clear
  Coat uses a second GGX dielectric lobe; foliage adds colored back-light transmission, wrap,
  two-sided normal handling, and shared wind-driven shading-normal variation.
- Added up to four deterministic material layers with Lerp, Multiply, or Additive blending, runtime
  weight changes, cycle/reference validation, and CPU flattening before GPU upload.
- Advanced editor material persistence to version 3 and DVOX material persistence to v1.2 for
  clear-coat, foliage, and layer data while retaining older readers.
- Expanded GPU material records and layout tests, Lua integration tests, a reproducible material
  feature demo, and a sample script. GPU visual execution remains unverified in this environment.

## v1.30.1 — Physical Modeling, Master Materials, Shading, and Render Environment Merge

- Merged the duplicated physical-model synthesizer branches into the v1.30 workstation without
  replacing the newer editor/audio architecture. Added 78 physical instrument, game, nature, and
  mechanical presets.
- Repaired note tuning, pickup/feedback separation, delay initialization, pitch-bend modal refresh,
  Nyquist suppression, throat-formant state, material parsing, driver operating ranges, and strict
  physical-parameter validation. Advanced `.dvesynth` writing to format 5.
- Added a deterministic preset audit and audition showcase. All 78 presets load and render finite;
  73 are technically clean and five inharmonic bell/plate patches remain marked for listening review.
- Merged master/instance materials, runtime overrides, GPU material/environment packing, Lua material
  and environment control, Cook–Torrance shading, AO, shadows, subsurface transmission, layered
  translucency, bloom, tonemapping, and exact sRGB output encoding.
- Advanced editor material persistence to version 2 so specular, shading/blend mode, and subsurface
  fields survive save/load; retained version-1 migration.
- Optimized dependency-minimal and Lua configurations pass 33/33 tests. Focused GCC AddressSanitizer
  and UndefinedBehaviorSanitizer suites pass for synthesizer, presets, materials, environment, and
  editor persistence. GPU execution remains unverified.

## v1.30.0 — Native Audio Workspace, Device Capture, Comping, Metering, and Warp Editing

- Advanced `DVEAEDT1` sessions to version 3 while retaining older session loading. Added explicit,
  non-overlapping comp segments with per-boundary fades and lane selection by timeline frame.
- Added a shared-canvas audio-workstation controller and renderer with command descriptors, grouped
  undo gestures, waveform/spectrogram/combined displays, take lanes, comp overlays, beat grid,
  markers, loop/punch ranges, selection, playhead, hit testing, and editing-tool foundations.
- Added SDL3 recording-device enumeration and callback-safe microphone/line input. A bounded SPSC
  ring and adaptive worker-side resampler reconcile input and engine clocks with drift/drop telemetry.
- Added mono/stereo EBU R128/BS.1770-style loudness measurement, momentary/short-term windows,
  loudness range, and four-times true-peak estimation.
- Added offline DC removal, normalization, silence trim, fades, reverse, channel conversion, WSOLA
  time stretching, duration-preserving pitch shift, and piecewise warp-marker rendering.
- Added internal effect racks with wet/dry, bypass, state, sidechain input, missing-effect placeholders,
  and deterministic-render checks. Added a control-thread CLAP bundle scanner/cache with hashing and
  blacklist persistence; plugin binary loading remains deferred to an isolated helper.
- Added hashed `.dveinteractiveaudio` persistence and deterministic sequence, weighted-random, and
  shuffle containers with cooldown and no-repeat behavior.
- Added a deterministic release demo and combined waveform/spectrogram screenshot.
- Expanded the optimized package suite to 26/26 passing tests; focused GCC AddressSanitizer and
  UndefinedBehaviorSanitizer suites pass 3/3.

## v1.29.0 — Workstation Session, Analysis, Recovery, and Interactive Music Foundation

- Expanded the callback-safe mixer recorder from one compatibility tap to eight fixed fan-out slots,
  allowing dry synth, master, and bus stems to be captured during the same render pass.
- Advanced `DVEAEDT1` sessions to version 2 while retaining version-1 loading. Added armed tracks,
  take lanes, active comp lanes, markers, tempo/time-signature maps, punch regions, pre/post-roll,
  automation lanes, sends, and selectable fade curves.
- Added slip editing, ripple deletion, roll-boundary editing, source looping, and deterministic
  linear/equal-power/smooth crossfades.
- Added multiresolution waveform peak caches, bounded spectrograms, sample/approximate true peak, RMS,
  DC offset, crest factor, approximate integrated loudness, clipping/silence counts, transient
  detection, and tempo estimation.
- Added atomic session saves and append-only recovery journals with content hashes, compacting, and
  recovery from a truncated final record.
- Added a platform-neutral audio timeline draw model with waveform columns, beat grid, markers,
  playhead, loop/punch overlays, clip hit testing, and marker/beat snapping.
- Added a deterministic interactive-music graph with states, conditional/vertical stems, gameplay
  parameters, stingers, and immediate/beat/bar/marker-quantized transitions.
- Updated `dve_audio_edit_demo` to record dry/master stems simultaneously and generate a version-2
  remix session, journal, bounce, waveform/spectral analysis, and adaptive transition evidence.
- Expanded the optimized package suite to 24/24 passing tests. Focused GCC AddressSanitizer and
  UndefinedBehaviorSanitizer suites pass 3/3.

## v1.28.0 — Audio Recording, Non-Destructive Editing, and Broad Media Import

- Added one callback-safe mixer capture tap selectable from dry synth, master, music, dialogue,
  effects, ambience, user-interface, or reverb-input audio.
- Added `AudioTakeRecorder` with a fixed SPSC ring, worker-side take assembly, and captured/committed/
  dropped/buffered frame telemetry.
- Added versioned `DVEAEDT1` `.dveaudioedit` sessions and immutable `AudioEditSourceLibrary` media.
- Added multitrack clips with timeline/source ranges, loop duration, reverse, gain, pan, fades,
  mute/solo, move, trim, split, delete, and bounded snapshot undo/redo.
- Added play, pause, stop, seek, project looping, rewind, deterministic bounce, and float32/PCM16 WAV
  export.
- Added optional FFmpeg command-line authoring import after native WAV and optional libsndfile,
  enabling tested MP3 and MP4/AAC ingestion and broad first-audio-stream container decoding.
- Added strict session-load validation for source references, identifier uniqueness, finite controls,
  clip ranges, timeline overflow, loop regions, metadata, and trailing data.
- Added `dve_audio_edit_demo` producing a recorded synth take and reverse-layer remix.
- Executed cooker acceptance for WAV, MP3, MP4/AAC, M4A/AAC, FLAC, Ogg Vorbis, and Opus.
- Expanded the optimized headless suite to 18/18 passing tests; focused GCC AddressSanitizer and
  UndefinedBehaviorSanitizer audio suites pass 3/3.

## v1.27.1 — Unified Recipe Cooker and Canonical Sample-Map Format

- Reconciled the integrated sample-map/granular runtime with the independently developed production
  multisample authoring branch.
- Resolved a critical binary collision: both branches used `DVESMAP1` for incompatible layouts. The
  canonical writer now emits `DVESMAP2`; the loader retains the earlier integrated-runtime v1 layout.
- Added `sample_map_recipe.hpp/.cpp` and `dve_cook_sample_map` for 19-field human-readable recipes,
  `.dvesample` dependency resolution, source deduplication, resident/hybrid prefix assembly, and
  source hash propagation.
- Increased the fixed resident map pool from 65,536 to 131,072 mono frames.
- Imported and recooked the Eightfold Production Multisample recipe and its three source assets.
- Added strict rejection of overlapping cross-group layers, stereo sources, and unsupported loop
  semantics instead of silently degrading the standalone prototype's behavior.
- Added recipe/cooker and legacy integrated-map compatibility tests.
- Completed the optimized headless build with 16/16 tests passing and focused GCC AddressSanitizer +
  UndefinedBehaviorSanitizer coverage for all three sampler-related suites.

## v1.27.0 — Production Sample-Map and Granular Runtime Checkpoint

- Added deterministic version-1 `.dvesamplemap` save/load with semantic content hashes, strict
  bounds, malformed/trailing-data rejection, and fixed callback-safe resident storage.
- Added up to eight sources and thirty-two key/velocity zones with root notes, cent tuning, gain,
  pan, reverse, round robin, attack/release triggers, and half-open playback/loop metadata.
- Added resident, streamed, and hybrid source policies plus a generation-tagged 32 x 2,048-frame
  fixed page cache. A control/worker publisher supplies complete pages; callback reads are bounded,
  lock-free, and fail closed to silence.
- Added triple-buffered immutable sample-map publication at render-block boundaries. Map generation
  changes retire mapped voices to prevent stale snapshot ownership.
- Added mapped source-rate conversion, release layers, internal loop-boundary wrapping, reverse
  endpoint correction, and equal-power loop crossfades.
- Added granular envelope curvature, stereo motion, reverse probability, random and quantized pitch,
  velocity/timbre density modulation, load-dependent grain budgets, and profiler telemetry.
- Corrected mapped sample/granular stereo so it is not collapsed before voice panning.
- Renamed `SampleLoopMode::None` to `Disabled` to preserve Xlib-first header compatibility.
- Added `dve_audio_sample_map_tests` covering format integrity, stream pages, round robin, release
  triggers, internal loop endpoints, fail-closed underruns, overload shedding, and preset round trips.
- Converted the sample/granular demo and benchmark to the production sample-map path.
- Validated the dependency-minimal optimized suite at 16/16 and both audio suites under GCC 14.2
  AddressSanitizer and UndefinedBehaviorSanitizer.
- Recorded remaining work explicitly: automatic stream prefetch, long physical-device stress runs,
  SIMD measurement, authoring tools, and the scripting/runtime bridge.

## v1.26.0 — Scripted Asset Runtime and Sample/Granular Synthesizer

- Integrated the Lua gameplay/runtime branch into the v1.25 audio/editor baseline without replacing the newer rigid-body SI conversion boundary.
- Added `GameWorld::spawn_asset` and Lua `world.spawn_asset` for cooked `.dvox` objects with material-specific density tables that survive later fragmentation.
- Added runtime-only Lua 5.4 linking fallback declarations for Linux systems that ship `liblua5.4.so.0` without development headers.
- Integrated the X11 game-host source, real action/axis input path, 60 Hz pacing, focus verification, and XTest smoke logic; the Jolt-dependent executable remains conditional.
- Replaced additional public enum names vulnerable to Xlib macros, including `None` and `Success`, and added an X11-header compatibility translation-unit test.
- Added resident sample and granular oscillator algorithms to every synth oscillator slot.
- Added a validated 16,384-frame mono `SynthSampleBank`, WAV/FLAC/Ogg import through the existing decoder, root-note/key-tracking, start/end/loop ranges, reverse, one-shot, and velocity response.
- Added a deterministic fixed-capacity granular scheduler with eight grains per oscillator, position, duration, density, spray, pitch, stereo spread, freeze, reverse, and Hann/Triangle/Tukey windows.
- Advanced `.dvesynth` to version 5 with deterministic migration from versions 1–4 and complete embedded sample-bank round-tripping.
- Added freehand eight-frame wavetable painting, frame selection, normalize, direct-current removal, and phase alignment to the native editor.
- Added `--synth-preset` to the X11 editor host for reproducible panel screenshots and acceptance runs.
- Added sample/granular demonstration assets and a dedicated twelve-voice performance benchmark.
- Validated 21/21 optimized Release tests, 21/21 Lua Release tests, 22/22 sanitizer tests, 15/15 dependency-minimal tests, native X11 smoke, and 116 portable files.
- Explicitly deferred streamed sample oscillation, multisample key zones, callback-side SIMD voice batching, physical audio/MIDI hardware acceptance, and Jolt-linked X11 execution in this environment.

## v1.25.0 — Eightfold Expressive Synthesizer

- Added MIDI Polyphonic Expression lower, upper, and dual-zone operation with independent member-channel pitch bend, pressure, and Control Change 74 timbre.
- Added configurable MPE master/member bend ranges and master-channel sustain propagation.
- Added a validated 128-note microtuning table with Scala scale, optional Keyboard Mapping, and MIDI Tuning Standard single-note/bulk imports.
- Added one-to-four-voice unison with detune, stereo spread, phase spread, and level preservation.
- Added oscillator and filter quality tiers for normal playback, high quality, and offline rendering.
- Added modulation-route polarity, per-route smoothing, and real-time modulation telemetry.
- Added arpeggiator step conditions, accent, slide, automation curves, fill state, transport restart, and four macro-automation lanes.
- Added wavetable drawing, direct-current removal, normalization, phase alignment, spectral morphing, and deterministic multi-frame import utilities.
- Added chord-memory capture and deterministic sorting from played notes.
- Added preset author/category/version/tag/favorite metadata and recursive filtering support.
- Expanded the synthesizer editor from six to seven pages with a dedicated Expression page.
- Expanded the sixteen-step arpeggiator editor from ten to sixteen controls per step.
- Advanced `.dvesynth` to version 4 while retaining deterministic version-1, version-2, and version-3 migration.
- Added focused regression coverage for MPE isolation, master sustain, Scala and MIDI Tuning Standard imports, microtuning round-trip, unison, quality tiers, modulation telemetry, wavetable authoring, chord capture, and editor controls.
- Added expressive demonstration WAV, preset, Standard MIDI File, maximum-load benchmark, typical-patch benchmark, and X11 editor screenshot.
- Measured the 128-frame maximum expressive patch at 79.06% p99 callback deadline use and the typical patch at 24.21%.
- Explicitly deferred granular/sample oscillators, dedicated graphical wavetable painting, callback-side SIMD voice batching, and physical audio/MIDI/MPE acceptance.

## v1.24.0 — Eightfold Modular Synthesizer

- Added two reusable per-voice LFOs with six waveforms, free/tempo-synchronized rates, phase, depth, fade-in, and key synchronization.
- Added a fixed sixteen-route modulation matrix, four macros, and eight MIDI-learn mappings.
- Added oscillator hard sync, linear/exponential FM, ring modulation, one-to-three-octave sub oscillators, and realtime wavetable playback.
- Added WAV wavetable import, eight frames, 128 samples per frame, five mip levels, and frame/sample interpolation.
- Extended amplitude and filter envelopes to Delay–Attack–Hold–Decay–Sustain–Release.
- Added 1×/2×/4× nonlinear-filter processing, serial MS-20 high-pass/low-pass controls, and stronger self-oscillation calibration.
- Added eight user chord-memory slots, chord detection, scale/root constraints, and sample-scheduled strumming.
- Added arpeggiator ratchets, ties, rests, per-step macro automation, game/MIDI clocks, and Standard MIDI File export.
- Added a tag-indexed preset library, five packaged presets, A/B preset capture, morphing, and a complete Presets/MIDI Learn editor page.
- Expanded the synthesizer editor from four to six pages and added wavetable visualization and full oscillator-routing controls.
- Advanced `.dvesynth` to version 3 while retaining version-1 and version-2 migration.
- Compiled active modulation routes and precomputed static pan gains during preset publication.
- Replaced repeated callback-side nonlinear `tanh` calls with a bounded rational saturator, reducing the 128-frame maximum-patch p99 to 52.28% of deadline.
- Added a typical-patch benchmark measuring 10.44% estimated single-core use and 11.57% p99 deadline use at 128 frames.
- Renamed public modulation sentinel enumerators to avoid collision with Xlib's `None` macro.
- Validated 22/22 optimized Release tests, 21/21 sanitizer tests, 13/13 headless tests, X11 editor smoke, and 112 portable files.

## v1.23.0 — Eightfold Hybrid Synthesizer

- Expanded every 16-voice synthesizer voice from six to ten waveform choices: sine, saw, square, triangle, pulse, noise, supersaw, organ, folded sine, and digital.
- Added per-oscillator pulse-width modulation depth/rate, variable shape, semitone tuning, cent tuning, phase, and key synchronization.
- Added Moog-ladder-style, Korg-MS-20-style, Oberheim-SEM-style, and clean state-variable filter models with multimode outputs, drive, bass compensation, morphing, and alternate MS-20 revision behavior.
- Added fully editable amplitude and filter Attack, Decay, Sustain, Release envelopes.
- Added global reference tuning, transpose, fine cents, and bounded analog drift.
- Added sixteen chord types, custom intervals, inversions, octave spread, and velocity scaling.
- Added a sample-accurate arpeggiator with seven modes, six divisions, swing, gate, latch, octave range, MIDI output, and a complete sixteen-step editor for enable, transpose, octave, velocity, gate, and probability.
- Added generated chord-note MIDI output and held-note/current-step telemetry.
- Advanced `.dvesynth` to version 2 while retaining version-1 loading.
- Rebuilt the editor instrument as Oscillators, Filter + Envelopes, Performance, and Effects pages.
- Added focused regression tests for all waveforms, pulse-width modulation, four filter models, chord voicing, sample-frame arpeggiation, preset migration, and editor controls.
- Corrected a duplicated PolyBLEP falling-edge term in the pulse oscillator and reduced unnecessary per-sample trigonometric work.

## v1.22.0 — Bounded Native-Audio and Reflection Foundation

- Added a fixed 16-source Steam Audio direct-simulation pool with deterministic priority stealing.
- Preallocated a binaural HRTF effect for every native source slot and exposed callback-safe source processing.
- Added immutable multi-source result publication and source-pool telemetry.
- Added a bounded lock-free multi-producer physics-contact queue between solver callbacks and the audio accumulator.
- Added a solver-neutral contact sink and post-step drain contract.
- Added Jolt Physics 5.5 `ContactListener` source integration, collision-response impulse estimation, body/material mapping, and contact telemetry.
- Corrected the Jolt dependency pin from the unavailable 5.6 assumption to official v5.5.0.
- Added reflection quality tiers, worker-budget degradation/recovery, and immutable reflection-field publication.
- Added order-0/1/2 Ambisonic environmental-field encoding and stereo decoding.
- Added a deterministic 16-source Steam Audio ABI-contract benchmark.
- Expanded optimized Release validation to 21 tests, sanitizer validation to 20 tests, and headless validation to 13 tests.
- Preserved the explicit boundary that actual Steam Audio, Jolt, SDL3 devices, MIDI hardware, Windows, and macOS were not available in this environment.

## v1.21.0 — Generation-Locked Wall-Collapse Acoustics and Direct Simulation

- Added a fixed-capacity `PhysicsContactAudioAccumulator` that groups listener-side samples by unordered body pair, material pair, spatial cell, and time window without allocation during recording.
- Added explicit impulse estimation from normal relative velocity and effective mass when Jolt does not provide a usable impulse.
- Added `DestructionCommitCoordinator`, which records voxel edits, contacts, fragment splits, structural strain, and an acoustic build request into one authoritative generation transaction.
- Made queue publication atomic at the transaction level: failed submissions do not partially publish audio or acoustic state.
- Expanded the optional Steam Audio adapter with scene, static mesh, material, simulator, source, direct-simulation, and immutable result-publication ownership.
- Added coarse acoustic-box to counter-clockwise triangle conversion and per-material absorption, scattering, and transmission mapping.
- Enforced exact source-generation agreement between acoustic scene publication and direct simulation.
- Kept scene commits and occlusion/transmission simulation off the audio callback; callback queries only read immutable published state.
- Added `SpatializationTransition`, a generation-tagged, smoothstep control-to-audio crossfade with no allocation, locks, logging, or middleware calls in `advance()`.
- Added an end-to-end wall-collapse regression and deterministic WAV demonstration that transitions from an intact-wall response to an open-room response over 180 ms.
- Added node-specific Audio Event Graph property controls with validation, clamping, cycling, compilation, undo, and redo.
- Defaulted the graph editor selection to a parameterized synth-note node so the property inspector is immediately useful.
- Expanded optimized validation to 19/19 tests, sanitized validation to 18/18 tests with benchmarks disabled, and dependency-minimal validation to 10/10 tests.
- Recorded that the actual Steam Audio SDK, real Jolt callback registration, physical SDL3/RtMidi devices, reflections, Ambisonics, and native Windows/macOS builds remain unexecuted.

## v1.20.0 — Draggable Audio Authoring, Generation-Committed Destruction, and Native HRTF Adapter

- Advanced `.dveaudio` to version 4 with finite serialized editor node positions and backward loading of versions 2 and 3.
- Rebuilt the Audio Event Graph panel as a searchable, draggable, port-connected authoring surface.
- Added all 14 current node types to the palette, immediate cycle/arity/self-edge checks, inline compiler diagnostics, and live audition.
- Added subtree copy/paste, deletion, 64-level snapshot undo/redo, deterministic reseeding, and canonical save/open.
- Added a bounded 32-entry single-producer/single-consumer destruction commit queue.
- Coupled voxel edits, physics contacts, fragment splits, structural strain, and acoustic rebuild requests to one authoritative generation.
- Added stale-generation rejection, non-blocking overflow rejection, queue discard, and detailed ingress telemetry.
- Added optional `dve_audio_spatial_steam` with SDK-hidden context/HRTF ownership and one fixed-block binaural effect per source.
- Added built-in or SOFA HRTF selection, nearest/bilinear interpolation, spatial blend, normalized directions, preallocated channel buffers, and retained context lifetime.
- Added a deterministic fake Steam Audio ABI so the production adapter source is compiled and executed when the real SDK is absent.
- Updated the X11 smoke host for targeted panel screenshots without mutating unrelated panel state.
- Validated 18/18 optimized Release tests, 18/18 sanitizer tests, and 9/9 dependency-minimal headless tests.
- Passed the platform-boundary scan over 105 portable files.

## v1.19.0 — Production Audio Assets, Streaming, Event Authoring, and Acoustic Publication

- Added dependency-free WAV import and optional libsndfile-backed FLAC/Ogg/WAV decoding.
- Added canonical 48 kHz conversion, normalization, cues, loop regions, signal metadata, and deterministic hashes.
- Added versioned `.dvesample` cooking plus the `dve_cook_audio` command-line tool.
- Added background streamed-audio workers and lock-free callback rings.
- Integrated up to eight streamed voices into the seven-bus real-time mixer.
- Added `.dveaudio` version 3 serialization, version 2 migration, stream nodes, full no-repeat history, scatter, cooldown, blend, and loop execution.
- Added the shared Audio Event Graph panel at `Window -> Audio Event Graph` / `Ctrl+6`.
- Added neutral voxel, contact, fragment-split, and structural-strain ingestion records feeding bounded material-mapped destruction events.
- Added latest-request-wins asynchronous acoustic-grid, merged-surface, and room/portal publication.
- Made acoustic snapshots and spatializer replacement atomic at callback boundaries.
- Added a middleware-neutral Steam Audio simulation/effect contract with deterministic fallback.
- Added bus snapshots, transition interpolation, callback percentile telemetry, stream telemetry, and voice-steal diagnostics.
- Added resident+dense synth and four-stream callback benchmarks.
- Validated 16/16 optimized Release tests and 15/15 sanitizer tests.

## v1.18 — Game audio mixer, events, virtualization, and destruction acoustics

- Promoted the Eightfold synthesizer into a source inside a general seven-bus game-audio mixer.
- Added resident mono/stereo sample registration, interpolation, resampling, looping, source handles, and scheduled playback.
- Added 128 logical sample voices, a 48-voice physical budget, priority/distance scoring, and timeline-preserving virtualization.
- Added immutable compiled event graphs with sample, synth, random-no-repeat, sequence, layer, delay, gain, bus, and parameter-switch nodes.
- Added sample-frame event dispatch shared by sample and synthesizer sources.
- Added a middleware-neutral spatializer contract and deterministic equal-power stereo fallback with distance, Doppler, occlusion, transmission, and reverb sends.
- Added coarse acoustic occupancy traces, approximate wall thickness, and dynamic room/portal path transmission.
- Added a bounded spatial/temporal destruction compiler and perceptual onset/fracture/resonance/debris/structure/tail recipes.
- Added seven mixer buses, low-pass filters, dialogue ducking, shared reverb, meters, and master limiting.
- Extended the SDL3 audio callback adapter to render either the standalone synthesizer or the complete mixer.
- Added `Window -> Audio Mixer`, Ctrl+5, bus meters, gain/mute controls, stop-all, and event audition.
- Added a maximum-load runtime benchmark: 128 sample timelines, 48 mixed sample voices, 80 virtual voices, and 16 x 8-oscillator synth voices.
- Expanded validation to 15/15 optimized Release tests and 14/14 sanitizer tests.

## v1.17 — Eightfold audio synthesizer

- Added an eight-oscillator-per-voice, sixteen-voice polyphonic synthesizer at 48 kHz float stereo.
- Added sine, anti-aliased saw/square/pulse, triangle, and deterministic noise waveforms.
- Added amplitude and filter ADSR envelopes, multimode per-voice filters, velocity, pressure, sustain, modulation, and pitch bend.
- Added distortion, three-band EQ, chorus, phaser, ping-pong delay, reverb, compressor, and limiter.
- Added fixed-capacity sample-frame MIDI scheduling, deterministic voice stealing, MIDI through, and full-duplex virtual MIDI.
- Added an optional RtMidi 6 adapter plus installed-package and pinned-fetch CMake paths.
- Added complete `.dvesynth` preset validation, serialization, save/load, and deterministic float-WAV output.
- Added an SDL3 audio-stream adapter and deterministic callback contract test.
- Added `Window -> Synthesizer`, Ctrl+4, eight oscillator strips, effects, metering, panic, MIDI-through, and on-screen/computer keyboards.
- Added explicit key-up forwarding and UTF-8-safe compatibility with the shared editor input path.
- Added maximum-load audio benchmarking for 16 voices x 8 oscillators x all effects.
- Expanded the available suite to 12/12 Release and 12/12 sanitizer tests.

## v1.16.0 — SDL Host, Presentation Contracts, and Vulkan Buffer Publication

- Added `SdlApplicationHost` with high-DPI windows, normalized keyboard/mouse/text/composition,
  clipboard, file drop, asynchronous dialogs, gamepads, timing, and opaque native handles.
- Added the shared `dve_desktop_editor` entry point and SDL editor canvas.
- Added a deterministic SDL3 API shim that compiles and executes the exact production host and
  desktop-loop logic in dependency-minimal continuous integration.
- Corrected editor text handling so UTF-8 input is appended atomically and Backspace removes a
  complete code point.
- Expanded the RHI with textures, texture views, swapchains, acquire/present, explicit resource
  states, bind-group layouts, bind groups, debug labels, timestamp pools, and device-loss status.
- Hardened `NullDevice` with ownership, binding, lifetime, state, presentation, timestamp, and
  device-loss validation.
- Added a dynamically loaded Vulkan 1.0 buffer backend with instance/device/queue setup,
  memory-type selection, staging, device-local copies, barriers, fences, upload, and readback.
- Executed packed-brickmap publication and byte-exact readback through the real Vulkan API using
  SwiftShader in this environment.
- Added portable-boundary enforcement over 85 neutral source files.
- Expanded optimized and sanitizer validation to 9/9 passing tests and headless validation to
  5/5 passing tests.
- Retained explicit unsupported diagnostics for Vulkan texture, swapchain, compute-pipeline,
  binding, and timestamp operations not yet implemented.

## v1.15.0 — Cross-Platform Host and RHI Foundation

- Added the platform-neutral `IApplicationHost` contract and deterministic `HeadlessApplicationHost`.
- Added `EditorPlatformBridge` for normalized operating-system event routing.
- Extracted complete editor visual composition behind `IEditorCanvas`; the X11 host is now an adapter.
- Added a typed Render Hardware Interface and strict Null validation backend.
- Added `PackedBrickmapRhiMirror` with three-buffer upload and byte-exact readback tests.
- Moved Direct3D 12 source out of `dve_core` into a Windows-only backend target.
- Added HLSL shader manifest and DXIL/SPIR-V/Metal compiler driver.
- Added Linux, Windows, macOS, sanitizer, and dependency-minimal CMake presets.
- Added an independent asset-pipeline-test option so headless validation does not require PNG/JPEG.
- Expanded the optimized and sanitizer suites to 7/7 passing tests; headless preset passes 3/3.
- Retained and revalidated the X11 smoke host after renderer extraction.

## v1.14.0 — Audited GUI Workflows, Hierarchy-Safe Commands, Background Diagnostics, and SI Physics Repair

- Integrated the submitted GUI menu, context-menu, marquee, hierarchy-filter, group/ungroup, snapping, camera-preset, material-picker, bottom-tab, console, and dirty-title workflows.
- Integrated cancellable background object diagnostics and the conditional Jolt-backed editor Simulate source.
- Corrected recursive deletion so the complete descendant closure is validated, snapshotted, removed, and restored transactionally.
- Corrected copy, cut, paste, and duplicate to include descendants and remap internal parent identifiers.
- Added locked-descendant protection for subtree deletion and lock enforcement for rename and reparent commands.
- Corrected grouping so selected parent-child subtrees are preserved rather than flattened.
- Corrected nested ungroup so surviving children are promoted before empty selected groups are removed.
- Routed File > Exit, Alt+F4, window close, and dirty New Scene through explicit destructive confirmation.
- Added generic shortcut dispatch from the menu registry, covering displayed clipboard, creation, overlay, build, and help shortcuts.
- Added selection pruning after structural undo/redo and layout recomputation after every structural action.
- Made numeric transform parsing reject partial tokens, non-finite values, surplus components, and trailing garbage.
- Closed the diagnostics and import-preview submit/store cancellation window and cleared stale results, statistics, and errors.
- Moved body-translation scaling into `make_rigid_body_desc`, making it the single voxel-to-SI conversion boundary and correcting moving-split tangential velocity.
- Made `EditorJoltSimulation::step` reject unsuccessful physics updates before publishing transforms.
- Added adversarial hierarchy, lock, clipboard, nested-group, dirty-confirmation, shortcut, stale-selection, numeric-input, cancellation, and SI-conversion regressions.
- Validated 6/6 optimized Release tests, 6/6 AddressSanitizer/UndefinedBehaviorSanitizer tests with leak finalization disabled, a native X11 smoke, a runtime-only build, and 30 consecutive editor-test runs.
- Syntax-checked the conditional Jolt editor source and smoke program; real Jolt linking/execution was not reproduced in this environment.

## v1.13.0 — Multi-Selection, Rotation, Live Import Preview, Diagnostics, and Accessible Workflows

- Replaced the single editor selection slot with a stable multi-selection set and primary selection.
- Added batch transform and object-policy commands with locked-object validation and all-or-nothing execution.
- Added group translation and rotation gizmos, shared-pivot rotation, local/world transform spaces, snapping, numeric transform APIs, select-all, and keyboard nudging.
- Added exact discrete voxel-object rescaling with undo/redo, deterministic material conflicts, anchor propagation, thin-feature preservation, and editor memory bounds.
- Corrected the initial rescale implementation, which produced 27 cells from a one-cell 2x scale because independently transformed closed cubes overlapped at boundaries; the partition mapping now produces exactly eight cells.
- Added structural diagnostics for occupancy, surface voxels, bricks, connected components, anchored/detached components, materials, mass, center of mass, inertia, collision boxes, bounds, and finite-value validation.
- Added ranked command search and a keyboard-operated Ctrl+K command palette.
- Added a persistent asynchronous model-import preview service with progress, cancellation, stale-generation rejection, and transactional scene-package publication.
- Added semantic accessibility-tree generation and deterministic JSON export for menus, tools, hierarchy, viewport, inspector, bottom panel, and status.
- Added file-drop routing for project, scene, model, texture, material, import-recipe, DVOX, and DVOXSCENE files.
- Added a bounded, de-duplicated recent-project registry with persistence and missing-file cleanup.
- Expanded the X11 editor to show multi-selection state, Euler rotation, transform space, mass, component, detached-component, and collision diagnostics.
- Expanded the editor benchmark to cover command search, exact selection diagnostics, and batch translate/undo in addition to picking and draw-list construction.
- Added tests for multi-selection, numeric rotation, local/world switching, voxel scaling, diagnostics, command search, accessibility export, live preview, cancellation, file routing, and recent projects.
- Validated six optimized Release tests in Linux/X11. Win32, Direct3D 12 editor execution, GPU picking, Jolt Simulate mode, and platform screen-reader bridges remain unvalidated.

## v1.12.0 — Native Editor Host, Exact Picking, Gizmos, and Material Workflows

- Added `NativeEditorController`, a platform-neutral input/layout/action layer over the existing editor workspace and command stack.
- Added the Linux/X11 `dve_native_editor_x11` desktop application with an actual window, event loop, double-buffered rendering, resize handling, menu popups, hierarchy, viewport, inspector, diagnostics, and status bar.
- Added perspective and orthographic viewport math, orbit, pan, zoom, fly movement, framing, and world-to-screen projection.
- Added exact object and voxel picking with authoritative voxel DDA, object-local voxel-size conversion, material/normal/world-position results, and nearest-object selection.
- Added local brick-bounds clipping before voxel traversal; corrected the initial 8 ms median empty-space picking cost to 0.697 microseconds in the demo benchmark.
- Added X/Y/Z translation gizmos with screen-space hit testing, live preview, snapping, cancellation, and undoable transform commands.
- Connected Add, Remove, Paint, Box, Beam, and Anchor native tools to deterministic editor commands and merged stroke history.
- Connected menu and keyboard actions for undo/redo, grid/collision/anchor overlays, Edit/Simulate/Play/Stop, save, framing, tool selection, and UI scale.
- Added scene hierarchy selection and undoable Visible, Locked, Anchored, Structural, and Collision inspector toggles.
- Added editor material presets and deterministic `.dvematerials` serialization covering appearance, density, strength, fracture, flammability, thermal behavior, transparency, and structural classification.
- Added keyboard focus traversal and continuity for high-contrast, reduced-motion, and UI-scale preferences.
- Added an automated Xvfb native GUI smoke test that renders a real window, switches simulation state, captures a 1280×800 frame, and exits.
- Added material, projection, picking, draw-list, controller, focus, and mode-action tests.
- Validated 6/6 optimized Release tests and 6/6 AddressSanitizer/UndefinedBehaviorSanitizer tests with leak detection disabled for the established shutdown issue.
- Validated a runtime-only build with editor, native editor, cooker, tests, and benchmarks disabled.
- Recorded that Win32, Direct3D 12 editor rendering, GPU picking, live Jolt simulation, native file dialogs, and screen-reader integration remain v1.13 work.

## v1.11.0 — Human-Centered Editor, Tools, Menus, and Workflow Foundation

- Added the separate `dve_editor` library and `dve_editor_demo` application.
- Added project directory creation, path containment, deterministic project files, and recent-scene metadata.
- Added stable authoring objects with hierarchy, rigid transforms, policy flags, voxel assets, source/import paths, and anchor masks.
- Added transactional revisioned `.dvescene` persistence and self-contained autosave recovery packages.
- Added command-based undo/redo, bounded history, redo-tail invalidation, merged brush strokes, command preconditions, and compound rollback.
- Added cube/sphere add/remove/paint brushes, box/hollow-box, line/beam, and anchor tools.
- Added Edit, Simulate, and Play workspace states with temporary snapshots and automatic restoration.
- Added Project, Assets, Scene Hierarchy, Viewport, Inspector, Problems, Tasks, Console, Profiler, and Build panel state.
- Added 34 default menu actions and shortcut-conflict checking.
- Added actionable problem records and bounded persistent background tasks with progress, phases, cancellation, and exception capture.
- Added a six-stage model-import workflow around the existing glTF/GLB/OBJ cooker.
- Added editor UI scaling, high contrast, reduced motion, color-blind-safe diagnostics, autosave, and destructive-action preferences.
- Added separate game menu, settings, accessibility, input binding, prompt, and tool-wheel models.
- Added an interactive self-contained browser prototype for the intended editor and in-game menu workflows.
- Added editor tests covering commands, tools, persistence, recovery, workspace restoration, menus, tasks, importing, settings, bindings, and HUD behavior.
- Validated five optimized Release tests. Native desktop GUI, Direct3D 12 viewport, picking, and gizmos remain v1.12 work.

## v1.10.0 — Persistent Publication, Queue Policy, and Concrete Backend Paths

- Added `PersistentByteHeap`, a deterministic best-fit aligned allocator with generation-stamped handles, coalescing, fragmentation telemetry, and explicit relocation accounting.
- Added `PersistentRuntimeBrickmapWorld` with shared index/record/material heaps, stable object handles, power-of-two capacity reservations, allocate-copy-swap growth, dirty byte-range detection, and exact readback validation.
- Extended `IRuntimeBrickmapWorld` with incremental `update_object` publication and backend capability reporting.
- Added pending upload-range and heap-byte views used by concrete GPU backends.
- Added incremental hot reload that preserves renderer handles, stages replacement physics first, migrates body state, validates renderer readback, and rolls back renderer and physics publication on failure.
- Added `RuntimeSceneJobPriority`, optional deadlines, configurable pending-job limits, and retained-memory publication backpressure to the persistent staging executor.
- Added deterministic queue selection by priority, deadline, and submission order, with queue-wait and stage-time telemetry.
- Added `RuntimeScenePublicationQueue`, a bounded authority-thread queue for completed full-scene and deferred-object staging.
- Added Windows-only `D3D12RuntimeBrickmapWorld` source using three shared DEFAULT buffers, a persistently mapped upload ring, fence-owned copy batches, dirty-range copies, readback comparison, and device-removal diagnostics.
- Extended the D3D12 smoke source to cover persistent publication and an incremental dirty update.
- Added `dve_jolt_scene_publication_smoke` for real Jolt static/dynamic scene publication, stepping, hot reload, telemetry, and unload when Jolt v5.6.0 is available.
- Added deterministic authored-like house and city-block package generation with mixed materials, glass, alpha masks, anchors, and movable objects.
- Added authored-scene and dirty-publication benchmarks.
- Recorded 481,908 dirty bytes versus 421,014,640 hypothetical full-upload bytes over 1,000 edits, with zero readback failures.
- Added persistent-allocation, incremental-publication, priority, deadline, queue-limit, publication-queue, and stable-handle hot-reload tests.
- Validated four optimized release tests, three AddressSanitizer/UndefinedBehaviorSanitizer tests, and a runtime-only dependency-minimal build.
- Recorded that Direct3D 12, Windows/MSVC, real GPU execution, and Jolt scene execution remain unvalidated in the Linux environment.

## v1.8.0 — Asynchronous Staging, Transactional Publication, Checkpoints, and Hot Reload

- Split scene construction into background-capable staging and authority-thread publication.
- Added `stage_scene_package`, `stage_scene_package_async`, and move-only `RuntimeSceneStaging`.
- Added deterministic parallel object staging with configurable 1–64 worker settings and coordinator participation.
- Added progress callbacks for manifest, asset I/O, derived data, renderer publication, physics publication, commit, and completion.
- Added copyable cancellation tokens and transactional cancellation checks at every publication boundary.
- Added saturating staging-memory estimates and caller-defined maximum staging memory.
- Added authority-thread enforcement for publication, deferred load/unload, checkpoints, and hot reload.
- Added the narrow `IRuntimeBrickmapWorld` capability with rollback-safe batch creation and destruction.
- Added `ReferenceRuntimeBrickmapWorld` with deep packed-scene ownership, stable slot reuse, counts, and exact readback hashes.
- Added renderer publication to full scene load, deferred load, object unload, and scene unload.
- Added post-publication readback-hash verification before scene commit.
- Extended `IRigidBodyWorld` with rollback-safe dynamic and static batch creation.
- Added static collision publication for anchored objects and consistent static/dynamic body telemetry.
- Added Jolt backend source for immutable static compounds and all-or-nothing static/dynamic batch insertion.
- Added self-contained scene checkpoints containing a generated manifest, one DVOX per reserved object, and a checksummed binary state file.
- Added exact restoration of loaded/deferred residency and complete rigid-body interpolation, velocity, and sleeping state.
- Added checkpoint asset compatibility validation before publication and transactional cleanup after state-restore failure.
- Added compatibility-checked hot reload that retains the same scene handle and stable ID set.
- Added material/property hot reload, dynamic-body state migration, default topology-change rejection, and explicit topology migration opt-in.
- Added rollback that republishes the old renderer/physics scene and body states when replacement publication fails.
- Added deterministic worker-count, cancellation, staging-memory, wrong-thread, renderer rollback, checkpoint, and hot-reload tests.
- Added 1/2/4/8-worker asynchronous scene-staging benchmarks and a deferred-publication benchmark.
- Recorded that the tiny three-object fixture is fastest with one worker because per-request thread overhead dominates.
- Confirmed optimized release tests, AddressSanitizer/UndefinedBehaviorSanitizer tests, and a separate runtime-only build.

## v1.7.0 — Transactional Runtime Scene Packages

- Added the runtime-only `RuntimeSceneWorld` and strict `.dvoxscene.json` version-1 parser.
- Added required/unknown-field validation, exact integer parsing, and decimal-string support for uint64 IDs above JSON's exact numeric range.
- Added unique ID/index checks, contiguous index validation, parent validation, and cycle detection.
- Added rigid world-matrix validation with finite affine, orthonormal, right-handed requirements.
- Added canonical package-root containment, traversal rejection, symlink-escape rejection, duplicate canonical path rejection, and per-file size limits.
- Added all-or-nothing scene staging across DVOX loading, content hashes, material references, packed brickmaps, connectivity, collision proxies, and physics descriptors.
- Added anchor-aware initial connectivity and CPU packed-brickmap validation for every loaded object.
- Added material-density quantization and solver-neutral initial dynamic-body descriptors.
- Added optional rigid-body publication with rollback on partial creation failure.
- Added deferred object load/unload/reload while reserving every manifest stable ID.
- Added optional uniform/external voxel-size policy while supporting mixed per-object voxel sizes by default.
- Added deterministic scene/world state hashes and RAII body cleanup.
- Hardened DVOX reading against section-size overflow, excessive material counts, empty brick records, unsorted/duplicate keys, overlapping/gapped payload ranges, and unreferenced payload bytes.
- Advanced the writer to DVOX 1.1 full-content hashing, covering voxel size, material definitions, brick records, generations, and payloads while retaining DVOX 1.0 read compatibility.
- Added bounded DVOX reads and revalidation of deferred asset size and canonical path at publication time.
- Updated scene-manifest writing to serialize uint64 IDs above 2^53-1 as decimal strings.
- Added the `dve_runtime_scene_bench` all-object and deferred-loading benchmark.
- Added corrupt, missing, duplicate, cyclic, path-escaping, non-rigid, voxel-policy, deferred, rollback, destructor-cleanup, and uint64-ID tests.
- Fixed an AddressSanitizer-detected rollback use-after-free by retaining published physics handles instead of pointers into exception-unwound staging storage.

## v1.6.0 — Textured Materials and Multi-Object Scene Cooking

- Isolated the offline importer into `dve_asset_pipeline`, keeping PNG/JPEG dependencies out of the runtime core.
- Added PNG decoding through libpng and baseline/progressive JPEG decoding through libjpeg.
- Added external, data-URI, and GLB buffer-view image loading with decoded-image limits.
- Added glTF image, sampler, texture, base-color-texture, TEXCOORD_0, vertex-color, and node-extras import.
- Added nearest/bilinear texture sampling, clamp/repeat/mirrored-repeat wrapping, sRGB-to-linear conversion, and barycentric UV/color interpolation.
- Added alpha-mask rejection before surface publication.
- Corrected coplanar alpha-hole backfilling by selecting the nearest geometrically intersecting candidate before material acceptance.
- Added deterministic weighted median-cut palette reduction with explicit 2–256 material limits.
- Preserved physical source-material identity across texture-derived color variants.
- Added `node.extras.dve` and exact-name sidecar overrides for object conversion policy.
- Added hierarchy-preserving multi-object cooking with stable IDs, parent links, node paths, anchoring, structural, and collision metadata.
- Added bakeable node-scale handling: scale enters voxel geometry and published transforms remain rigid.
- Added rejection of collision-enabled nodes with shear, projective terms, or singular transforms.
- Added deterministic `.dvoxscene.json` manifests and one `.dvox` file per cooked object.
- Added PNG checker, JPEG, alpha-mask, palette-determinism, node hierarchy, scale-baking, and sidecar tests.
- Added a textured-wall benchmark alongside the solid-cube benchmark.
- Added reproducible textured and multi-object examples.

## v1.5.0 — Offline Model Import and Voxel Asset Cooker

- Added a static glTF 2.0 / GLB importer for core scenes, node transforms, triangle primitives, accessors, materials, external buffers, GLB BIN chunks, and base64 data buffers.
- Added a basic OBJ geometry fallback with polygon triangulation.
- Added conservative triangle-box surface voxelization.
- Added solid, shell, and surface-only conversion modes.
- Added exterior flood fill, shell dilation, 1x/2x/4x supersampling, coverage downsampling, and thin-feature preservation.
- Added material-definition conversion and direct packing into the existing sparse 8³ `VoxelObject`.
- Added versioned `.dvox` serialization with encoding-specific payloads for UniformSolid, MaskUniform, LocalPalette4, and Palette8 bricks.
- Added `.dvox` content-hash validation and round-trip loading.
- Added the `dve_cook_model` CLI, global JSON settings sidecars, HTML import reports, and a procedural asset-cooker benchmark.
- Added boundary-edge, non-manifold-edge, and disconnected-island diagnostics without silent repair.
- Added an exact half-open convention for grid-aligned model bounds.
- Added glTF, GLB, cube, surface, shell, sidecar, deterministic-file, topology, and all-brick-encoding tests.
- Recorded explicit limitations: no texture sampling, compressed glTF extensions, sparse accessors, skins/animation, node-level cooker metadata, or multi-object output yet.

## v1.4.1 — Jolt Backend Hardening

- Corrected the Jolt compound center-of-mass frame with `OffsetCenterOfMassShape` so heterogeneous-material fragments use the authoritative physical COM.
- Added centralized rigid-body descriptor validation, quaternion normalization, and positive-definite float-space inertia checks.
- Added configurable Jolt workers, body/pair/contact capacities, temporary memory, and explicit box convex radius.
- Captured all `EPhysicsUpdateError` capacity failures in persistent telemetry.
- Moved initial velocities into `BodyCreationSettings` before body insertion.
- Added full state restore, separate teleport semantics, and explicit total/dynamic/awake/sleeping counters.
- Added optional linear-cast continuous collision and `DebrisNoSelf` collision filtering.
- Added moving-parent child-COM velocity inheritance and descriptor helper.
- Added heterogeneous-COM, off-center impulse, batch creation, and state-semantics test coverage to the Jolt smoke source.
- Added configurable 1–4096-body Jolt collapse benchmark with percentile and capacity-error output.
- Added pinned Jolt dependency paths and compile-time v5.6.0 enforcement.
- Added the v1.4.1 formal HTML notebook and hardening report.

## v1.4.0 — Packed GPU Brickmap and Primary Software Tracer

- Added fixed-layout packed GPU brick records and object-local brick index grid.
- Added compact material arena with free-span reuse.
- Added generation-validated incremental packed-scene updates and readback hashes.
- Added exact two-level CPU HDDA and Shader Model 6.0 HLSL primary tracer contract.
- Validated 100,000 authority/mirror/packed rays with zero mismatches.
- Removed independently normalized traversal directions after the oracle exposed five edge-hit discrepancies.
- Added fence-owned upload-ring model.
- Added Windows-only D3D12 queue/buffer/readback backend source and smoke application.
- Added DXC shader compilation script.
- Added rigid-body adapter, inertia validation, and deterministic reference body world.
- Fixed removal-only damage creating empty brick headers in absent space.
- Added packed-scene, upload-ring, no-op damage, ray-oracle, and rigid-adapter tests.
- Added primary tracer benchmark and v1.5 GPU execution plan.

## v1.3.0 — Compact Connectivity and Player Safety

- Compact boundary-port connectivity.
- Retained CSR graph.
- Transformed player ray/capsule queries and post-edit resolution.
- Exact bounded fragment mass properties and merged box proxies.

Earlier history is retained in the prior architecture notebooks and source documents.

## v1.9 — Persistent staging and asynchronous deferred streaming

- Added `RuntimeSceneStagingExecutor` with one reusable worker pool.
- Added fair full-scene/deferred-object queues.
- Added retained-result memory reservations and executor telemetry.
- Added cancellation of queued and running jobs during shutdown.
- Added `stage_deferred_object`, `stage_deferred_object_async`, and `publish_staged_deferred_object`.
- Refactored synchronous deferred loading onto the staged implementation.
- Added monotonic object residency generations.
- Rejected duplicate, unload/reload-stale, and hot-reload-stale deferred results.
- Incremented residency generations across hot reload even for deferred objects.
- Added deterministic synthetic scene-scale package generation and benchmark.
- Recorded queued-work comparisons through 500 objects and 10.24 million voxels.
- Added executor, async deferred, cancellation, memory-budget, and stale-result tests.

## v2.31 — Dynamic Mesh Kernel and Headless Geometry Graph

- Added half-edge `EditableMesh` with generation-safe handles and five attribute domains.
- Added `MeshBuilder` and conversion to/from `CookedPolygonAsset`.
- Added `GeometrySet`, first-class instances, typed node registry, schema migration, cycle checks, demand evaluation, hashing, memory cache, cancellation, and stale request rejection.
- Added Box, Grid, Transform, Join, Merge by Distance, Set Position, Set Material, and Bake `.dmesh` nodes.
- Added `dve_geometry_graph_bake` and integrated behavior tests.
