# Settings runtime integration progress

PR #62 connected the first 43 of the 134 initially unapplied settings.
The audio/navigation batch starts from main `4bd208d` (PR #60, leak checks) and
connects six more: 49 of the original 134 now have consumers, with 85 still unapplied.
The complete 134-setting behavior contract is not implemented by these batches. The registry's
`applied` flag and source-reference audit indicate wiring, not full behavioral certification.
No placeholder reads have been added to claim coverage of the remaining settings.

## Current viewport-navigation batch

Main after PR #67 connected 68 of the original 134 settings, leaving 66 unapplied.
This batch connects `camera.navigation_style` and `input.raw_mouse`: 70 connected,
64 unapplied on this branch. Wiring is not full acceptance certification.

See [viewport-navigation-settings.md](viewport-navigation-settings.md) for the published
binding table, override rules, SDL capture lifecycle, tests and host limitations.
Both settings apply live; switching clears held navigation while preserving camera pose.

## Audio/navigation batch

| Setting | Runtime behavior | Application boundary |
| --- | --- | --- |
| `audio.sample_rate` | Constructs the editor mixer and synth at the same selected 8–384 kHz rate. Both native hosts load User and Project preferences before construction; Session overrides retain precedence. SDL converts engine output to its actual device rate. | Startup/restart. Editing this setting does not tear down an active engine. |
| `audio.buffer_frames` | Requests the SDL physical buffer size before opening playback, then restores the host's previous hint. Console reports engine/device rates and requested/actual frames; ignored hints and unknown format are reported honestly. Fixed scratch storage still handles variable callback lengths. | SDL desktop startup/restart. The X11 host has no SDL playback device. |
| `audio.granular_quality` | Low: 16 grains/linear; Medium: 32/cubic; High: 64/cubic; Ultra: 64/eight-tap windowed sinc with cutoff reduction for faster playback. Limits are per voice oscillator. Authored presets/windows remain intact; tempo and pitch increments do not change. | Next render block for admission; each grain keeps its interpolation until it expires. Lowering quality never abruptly kills live grains, so the active count can temporarily exceed its new limit. |
| `audio.stream_preload_ms` | Allocates new stream rings at ceil(active engine rate × milliseconds / 1000). Zero reserves the safe 1024-frame minimum. Workers fill asynchronously; callback performs no file I/O. Explicit caller capacities still take precedence. Existing underrun telemetry remains available. | Apply/update, then the next stream open. Existing rings keep their capacities and are not resized concurrently. |
| `camera.input_acceleration` | Continuous held fly velocity follows a first-order response: the setting is its time constant in seconds (63% after one constant). Zero reaches requested speed immediately. Key-down no longer adds a separate movement step. | Live, at the next controller update. |
| `camera.input_smoothing` | Adds a first-order fly velocity filter and spreads accumulated look/orbit angular deltas over elapsed time. Zero preserves immediate mouse response. Both velocity stages are analytically integrated, keeping travel independent of frame subdivision. | Live, at the next controller update. Capture/focus/mode/rig changes and settings dialogs reset history. |

Native hosts persist Project Apply to `<project-root>/.dve/project/editor_settings.txt`;
User Apply continues to use `.dve/user/editor_settings.txt`. Both files load before
audio construction. Session values stay transient. Invalid files log an error and
retain the working layer.

These are editor preferences. The packaged player does not load the editor registry.
Stream assets must still be cooked at the active engine rate; this batch does not add
automatic recooking, hot device restart, or platform hardware latency measurements.
SDL may ignore the requested hardware buffer. An unavailable device remains nonfatal;
an unavailable format query is reported as unknown, never as the requested format.
The SDL driver contract follows its official
[buffer hint](https://wiki.libsdl.org/SDL3/SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES) and
[device format query](https://wiki.libsdl.org/SDL3/SDL_GetAudioDeviceFormat) documentation.

## Audio/navigation local validation

All 15 targeted CTest checks pass: new audio/navigation behavior, existing settings runtime,
registry/reference audit, editor, Play session, shortcuts, camera system, audio runtime,
four granular core/serialization/stereo/macro checks, SDL audio, and desktop smoke.
The new test exercises actual persisted User/Project restart and Session precedence,
stream ring allocation, 384 kHz rendering, each grain budget/interpolation, sinc DC
preservation/alias attenuation, preset preservation, analytic frame subdivision,
zero-filter behavior, and capture/focus/mode reset. SDL tests cover ignored buffer
requests, actual format reporting, failed opens, unknown formats, hint restoration,
and callbacks larger than scratch capacity or ending in a partial frame.
Local SDL tests use the deterministic shim. Hardware latency and driver negotiation
on physical devices have not been measured; platform CI supplies broader build/test coverage.

## First batch behavior and application boundaries

- Layer changes, scope/profile loads, resets and clears increment a registry revision.
  The controller validates and resolves changes once at its update boundary.
  Invalid paired dead/soft zones or near/far planes fail transactional Apply/import.
- Physics backend, workers, solver/substep settings, sleeping, CCD, determinism and Lua
  execution apply when a new Play/Simulate session starts. Gravity, fixed step and
  catch-up limit can change in an existing session without rebuilding its world.
  Unsupported compiled backend choices cannot be selected through the settings UI;
  imported requests are preserved and labeled unavailable. Reference and Box3D solver
  controls are disabled where the adapter does not implement them.
- Camera framing/collision overrides preserve each rig's authoring and restore it when
  the global override is cleared. Overrides that conflict with an authored zone pair
  retain the previous working rig and report an error. New rigs use the selected defaults. Look-ahead receives
  measured target velocity. Play uses a collision adapter whose lifetime follows its world.
  Shake scales compose multiplicatively; paused/scaled game time affects camera updates
  unless explicitly ignored. Horizon lock applies to the displayed camera.
- Audio user dB gains multiply authored bus gains. They do not rewrite them. A bounded
  group command publishes the four gains together; rejected commands remain pending and
  retry. Gain, mono and dynamic-range transitions take 20 ms in sample time. Mono averages
  stereo before output limiting. Medium/Night use stereo-linked peak compression with
  strengths 2/4; Full retains the existing tanh output curve. This is peak compression,
  not a loudness normalization or measured integrated-loudness implementation.
- Dark/light palettes affect shared editor chrome; SDL system appearance is queried by
  the host. High contrast takes precedence. Specialized panels still contain literal
  colors, so full theme-wide palette/contrast acceptance needs a follow-up.
- Focus regions receive a visible outline. Diagnostics expose actual native voxel draw
  item counts, audio voice/underrun/queue counters and camera state. They do not provide
  full GPU timing or an equivalent packaged-player diagnostics overlay.
- SDL UI navigation uses configured delay/rate and suppresses OS-generated arrow repeats.
  Focus loss clears navigation input. One active gamepad supplies radial dead-zone sticks;
  keyboard and analog movement combine, and unplugging clears only analog movement.
  Multiplayer input ownership is not implemented here. The legacy X11 event path does
  not yet use the shared repeat/gamepad bridge.
- Frame caps round up to whole milliseconds and cannot be bypassed by mouse wakeups.
  The editor retains its existing 4 ms minimum pacing interval and idle throttling.
  High DPI is read before SDL window creation and is labeled restart-required.
- Optional save validation executes before the transactional scene save.

## First batch local validation and limits

Ten targeted CTest checks pass: settings runtime behavior, registry, reference audit,
Play session, camera system, draw culling/sorting, desktop contract smoke, SDL host, SDL audio and audio runtime.
The new behavioral check verifies real body movement, camera override restoration,
look-ahead, rendered PCM, callback-size-independent gain ramps, bounded queue retry,
palette pixels, focus outline and input-repeat/stick behavior.

The build uses the deterministic SDL shim and reference physics. Actual SDL hardware,
Jolt, Box3D, Lua and legacy X11 integration have not been validated in this environment.
The full engine test suite has not been run. The packaged player does not load the editor
settings registry, so this batch is not a claim of identical player-side behavior.
PR #62 subsequently passed the platform CI checks and was merged. These partial batches
do not certify the full all-settings feature; source references alone are not behavioral tests.

## First batch connected IDs

| Setting | Consumer |
| --- | --- |
| `accessibility.camera_shake` | NativeEditorController / CameraDirector |
| `accessibility.dynamic_range` | AudioMixer output policy |
| `accessibility.focus_indicators` | native renderer focus outline |
| `accessibility.mono_audio` | AudioMixer output policy |
| `accessibility.reduced_motion` | NativeEditorController / CameraDirector |
| `audio.dialogue_gain_db` | AudioMixer user bus gain group |
| `audio.effects_gain_db` | AudioMixer user bus gain group |
| `audio.master_gain_db` | AudioMixer user bus gain group |
| `audio.music_gain_db` | AudioMixer user bus gain group |
| `camera.aim_damping` | NativeEditorController / CameraDirector |
| `camera.collision_radius` | NativeEditorController / CameraDirector |
| `camera.collision_recovery` | NativeEditorController / CameraDirector |
| `camera.dead_zone` | NativeEditorController / CameraDirector |
| `camera.horizon_lock` | NativeEditorController / CameraDirector |
| `camera.ignore_time_scale` | NativeEditorController / CameraDirector |
| `camera.look_ahead` | NativeEditorController / CameraDirector |
| `camera.position_damping` | NativeEditorController / CameraDirector |
| `camera.preserve_line_of_sight` | NativeEditorController / CameraDirector |
| `camera.shake_scale` | NativeEditorController / CameraDirector |
| `camera.show_focus_planes` | NativeEditorController / CameraDirector |
| `camera.soft_zone` | NativeEditorController / CameraDirector |
| `diagnostics.audio_stats` | native renderer diagnostic overlay |
| `diagnostics.camera_debug` | native renderer diagnostic overlay |
| `diagnostics.render_stats` | native renderer diagnostic overlay |
| `diagnostics.validation_on_save` | scene save validation |
| `editor.theme` | native renderer shared palette / SDL system theme (partial) |
| `input.controller_dead_zone` | EditorPlatformBridge radial stick processing |
| `input.ui_repeat_delay` | EditorPlatformBridge held-arrow scheduler |
| `input.ui_repeat_rate` | EditorPlatformBridge held-arrow scheduler |
| `physics.allow_sleeping` | EditorPlaySession / physics adapter factory |
| `physics.backend` | EditorPlaySession / physics adapter factory |
| `physics.continuous_collision` | EditorPlaySession / physics adapter factory |
| `physics.deterministic` | EditorPlaySession / physics adapter factory |
| `physics.fixed_timestep` | EditorPlaySession / physics adapter factory |
| `physics.gravity` | EditorPlaySession / physics adapter factory |
| `physics.max_substeps` | EditorPlaySession / physics adapter factory |
| `physics.position_iterations` | EditorPlaySession / physics adapter factory |
| `physics.substeps` | EditorPlaySession / physics adapter factory |
| `physics.velocity_iterations` | EditorPlaySession / physics adapter factory |
| `physics.worker_threads` | EditorPlaySession / physics adapter factory |
| `render.frame_limit` | SDL and X11 host pacing |
| `render.high_dpi` | SDL window creation (restart) |
| `scripting.lua` | EditorPlaySession startup-script lifecycle |

## Remaining 64 settings


These retain the unapplied marker in the UI. Each row records the intended behavior,
not a completed implementation. Existing engine subsystems may provide part of a row;
they still need actual owners, resource/lifecycle integration and behavioral checks.

| Setting | Required behavior |
| --- | --- |
| `editor.restore_workspace` | When enabled, restore panel positions, active tabs, open assets and viewport state from a versioned per-project workspace record. When disabled, start from the default layout and ignore the saved record. Do not reopen untrusted assets automatically or confuse layout recovery with unsaved-scene recovery. |
| `editor.telemetry_local` | Enable bounded local collection of frame, rendering, task and audio statistics. Turning it off stops optional collection and persistence; error reporting still works. Never transmit this data. Expose retention, clear-data and capture status so the setting has a visible meaning. |
| `render.backend` | Select the actual graphics backend used to create the rendering device at restart. Each available choice must have a functioning device path. Preserve the requested choice and display the active backend; a missing platform/backend must produce an explicit failure or user-visible fallback, never a silent no-op. |
| `render.display_mode` | Apply windowed, borderless fullscreen, or supported exclusive fullscreen to the selected game/presentation window. Remember the previous windowed size and position. Recreate presentation resources safely and restore them if the change fails. |
| `render.resolution_profile` | Set output dimensions from the selected named profile. Custom must reveal explicit width/height controls, which the current enum alone does not provide. Resize output resources and camera aspect ratios; do not confuse output size with internal resolution scale or editor UI zoom. |
| `render.preferred_display` | Resolve the stable project-local display slot to a connected monitor; zero follows the operating-system primary display. Preserve mappings across monitor enumeration changes where an identity is available. If a monitor disappears, relocate safely and show the selected fallback. |
| `render.resolution_scale` | Allocate scene-render targets at output drawable dimensions multiplied by the value, rounding safely. Render the scene there and upscale/downsample once; keep editor UI, text and picking in output/logical coordinates. Invalidate size-dependent history and attachments when scale changes. |
| `render.local_players` | Create the requested number of local-player input contexts, player-camera assignments and viewports. Removing a player tears down its owned resources without corrupting the scene. Extra views need explicit camera/player assignment rather than duplicated control of player one. |
| `render.split_screen_layout` | Choose real viewport rectangles for automatic, shared single, horizontal, vertical, three-player, quad or picture-in-picture layouts. Validate against the local-player count and show any resolved automatic layout. Adapt camera aspect ratios and pointer routing to every rectangle. |
| `render.dynamic_shared_camera` | For a two-player compatible layout, merge views when subjects are close and split when they separate, using different enter/exit thresholds and a stable blend. Preserve both players' visibility and input ownership. Turning it off freezes the ordinary configured layout. |
| `render.spectator_window` | Create or close an independent presentation surface with a spectator camera and its own aspect ratio. Closing it releases only spectator resources. Keep input focus explicit; failures to open another surface must not interrupt the main window. |
| `render.hdr` | Request high-dynamic-range presentation on a capable display with a supported swapchain format and output transform. Report requested and active modes separately. Unsupported displays remain usable in an explicitly reported SDR fallback; never label SDR output as active HDR. |
| `render.texture_budget_mb` | Bound resident scene-texture memory in mebibytes using measured allocation sizes and a streaming residency manager. Evict unpinned least-needed resources, retain fallback mips and avoid evicting in-flight or required textures. Report pinned requirements that exceed the budget. |
| `render.ao_quality` | Choose explicit ambient-occlusion sampling/resolution presets. Occlusion represents local geometric shading, not a duplicate shadow or GI term. Keep material ambient-occlusion inputs and screen/voxel estimates combined through a documented rule. |
| `render.translucent_layers` | Limit the number of transparent continuation layers per scene ray. Composite premultiplied color/transmittance in front-to-back order and define how residual background contribution is resolved at the cap. This does not change collision or picking geometry. |
| `geometry.mode` | Choose voxel-only, polygon-only or hybrid geometry support for the next configured/package build. Validate scenes and dependencies against the selected mode; preserve authored assets in the editor and reject incompatible package contents instead of silently stripping required geometry. |
| `voxel.brick_budget` | Set the cap on resident voxel render/streaming bricks, with explicit CPU/GPU accounting. Keep authoritative destruction and physics data intact. Evict only safely reloadable, unpinned representations; report pinned working sets exceeding the request. |
| `voxel.destruction_quality` | Select documented visual debris and collision-detail budgets for destruction. All presets must preserve exact authoritative voxel removal and structural connectivity; quality may change decorative debris, proxy refinement and scheduling, not which supporting voxels exist. |
| `voxel.async_connectivity` | Choose worker-based or synchronous connectivity/fracture discovery for newly submitted jobs. Workers read immutable snapshots and commit only results whose generation still matches. Switching must settle/cancel outstanding work safely without double-publishing fractures. |
| `voxel.ray_step_scale` | Scale permitted sampling steps for voxel rendering without breaking exact brick/voxel boundary traversal. Values above one must not skip thin occupied surfaces; use conservative empty-space skipping or validated interval tests. Picking and collision keep their exact independent traversal. |
| `polygon.lod_bias` | Bias screen-space mesh level-of-detail selection with a documented sign: proposed positive values favor coarser levels, negative favor finer. Retain hysteresis and available-level bounds. Do not alter source meshes or authored selection/collision behavior. |
| `polygon.instancing` | Batch compatible draws of identical meshes/material layouts into instance records while retaining per-object transforms, material overrides and stable selection IDs. Off uses ordinary draws of the same visible objects. |
| `polygon.frustum_culling` | Reject polygon draw candidates conservatively outside each camera's actual frustum. Use current transformed/skinned bounds and include the active projection. Off submits otherwise eligible objects without frustum rejection. |
| `polygon.occlusion_culling` | Use supported depth/hierarchical occlusion to omit hidden polygon draws. Treat uncertain or stale queries as visible, especially after camera cuts, destruction and movement. Off removes this culling stage without disabling frustum culling. |
| `polygon.mesh_streaming` | Load cooked mesh levels asynchronously from packaged/local assets, prioritizing visible requests and maintaining coarse resident fallbacks. Off requests an explicit resident working set before use. Missing data produces a visible diagnostic rather than a blocked frame loop. |
| `material.global_parameters` | Publish project-wide scalar/vector parameter collections into materials that reference them. Changes update the shared binding at a frame boundary. Off uses defined authored defaults and reports collection references; do not rewrite materials or silently read stale globals. |
| `material.layer_limit` | Set the maximum flattened material-layer count supported by runtime/cooked output. Validate/cook above-limit stacks with an explicit bake or rejection policy; do not silently drop visible layers. Preserve original authoring data and recompile affected resources safely. |
| `material.clear_coat` | Enable or bypass the energy-conserving clear-coat secondary lobe in eligible materials. Off restores the underlying base response, while retaining authored coat parameters for later re-enable. Reuse the same model in reference and GPU paths. |
| `material.foliage` | Enable thin two-sided foliage transmission/backlighting for eligible materials. Off uses a documented ordinary two-sided diffuse fallback without erasing authored transmission inputs or changing object geometry. |
| `material.validate_gpu_layout` | Validate CPU/shader material record sizes, offsets, alignment, identifiers and versioning when loading/building resources. A mismatch prevents publishing incompatible buffers and reports the field; disabling optional validation does not disable essential bounds/type checks. |
| `audio.spatializer` | Choose native spatialization, a functioning Steam Audio adapter, or no spatial processing. None uses the authored nonspatial mix. Keep source identity, gain and playback phase through safe transitions; report unavailable external backends rather than silently substituting. |
| `audio.hrtf` | Enable binaural head-related transfer function processing for supported stereo headphone spatializers. Off uses documented pan/distance processing. Mono output and unsupported backends must show why binaural processing is inactive. |
| `audio.loudness_normalization` | During export/cooking, measure integrated loudness and apply the selected project target plus true-peak constraint. Off leaves the intended export gain unchanged. Add explicit target/peak controls or a documented profile; a boolean alone cannot choose a loudness target. |
| `input.gamepad_prompts` | Select keyboard, Xbox, PlayStation or Switch glyph families for action bindings. Automatic follows the most recently active meaningful input device with hysteresis, excluding noisy idle axes. Display the real assigned binding, not a hardcoded button label. |
| `scripting.hot_reload` | Watch scripts and reload changed modules through a transactional compile/migrate/commit path. Keep the prior working script on failure, preserve defined state and suppress duplicate lifecycle events. Off stops automatic reload watching but does not stop ordinary script execution. |
| `scripting.migration_timeout_ms` | Bound each hot-reload state migration by the configured milliseconds using executable interruption/instruction budgets. Check time during execution, including interruptible host calls where supported; a post-completion timeout check does not stop an infinite migration. |
| `build.configuration` | Choose a real Debug, Release or Distribution build profile that controls compiler optimization, symbols, assertions and package contents through documented mappings. Show the resulting build configuration; merely saving a label is insufficient. |
| `build.target` | Select host, Windows, Linux or macOS for the generated build/package job, with correct toolchain, ABI and dependencies. Host resolves to the current platform. Reject unavailable cross-toolchains before starting rather than generating a host binary under another name. |
| `build.sanitizers` | For supported development toolchains, enable AddressSanitizer and UndefinedBehaviorSanitizer flags consistently for compilation and linkage. Keep the resulting build separate from distribution artifacts. Report incompatible platforms or profiles explicitly. |
| `build.headless` | Build/package a runtime that starts without windows or graphics presentation while retaining required simulation, scripting, assets and explicitly supported audio. Exclude presentation-only dependencies; do not mean merely hiding a window. |
| `build.deterministic_oracles` | Run selected reproducible render/audio reference checks as part of packaging verification. Store seeds, fixture versions, tolerances and results with the build. Off skips these optional checks, not essential structural/integrity validation. |
| `build.verify_dependencies` | Verify the complete required asset dependency closure and expected content hashes before publishing a package. Detect missing, stale and mismatched cooked dependencies and report the chain. Off may skip optional hash rechecking, but cannot publish dangling required references. |
| `accessibility.color_vision` | Adapt editor/diagnostic palettes and redundant symbols for the selected color-vision mode. Keep semantic distinctions visible without relying on hue alone. Do not globally alter authored scene colors unless an explicitly separate simulation preview is requested. |
| `accessibility.subtitles` | Show authored dialogue subtitles and audio captions when enabled, synchronized to actual playback/seek events and readable within safe areas. Off hides caption presentation, not dialogue audio. Required subtitle/caption authoring data and controls must exist. |
| `camera.render_target_outputs` | Allow configured camera rigs to publish named offscreen color/depth outputs to materials or downstream passes. Off releases/disables that output work and supplies a defined fallback texture to consumers. Check output-name collisions and cyclic dependencies. |
| `camera.constant_speed_dolly` | Reparameterize camera spline traversal using a cached arc-length table so equal playback time advances equal world distance. Off uses authored parameter-time traversal. Rebuild only when the path changes; preserve cue/sequence timing. |
| `camera.sequence_scrub_rate` | Scale camera-sequence preview playback by the configured factor; one is real time. Explicitly dragged timeline position still maps exactly to its requested time. Preserve cue crossings and separate preview speed from game simulation time. |
| `camera.dof_quality` | Select off or documented depth-of-field sample/resolution budgets, including a cinematic preset. Off removes the pass. Other values refine the physical-lens blur without changing focus distance or aperture; the existing depth-of-field enable flag still controls eligibility. |
| `camera.motion_blur_quality` | Select published motion-blur sample/tile budgets for an enabled authored blur effect. Quality does not change shutter duration, enable disabled effects, or bypass reduced-blur accessibility controls. Camera cuts reset history. |
| `camera.lens_effect_quality` | Select sampling/approximation budgets for enabled distortion, fisheye, aberration, flares and halation. Preserve authored lens parameters and effect masks; do not make low quality stronger or turn disabled effects on. |
| `camera.film_grain_quality` | Select documented film-grain evaluation resolution/sample budgets using stable seeded noise and time progression. Quality must not alter authored intensity, color bias or gate-weave amplitude; accessibility disable takes precedence. |
| `camera.lut_resolution` | Allocate and deterministically resample a three-dimensional color lookup table at the requested integer edge length from 16 to 64, including non-power-of-two sizes. The registry step of 16 is an editing increment, not a reason to reject other valid integers. Publish only a complete validated resource. |
| `camera.lut_streaming` | Resident preloads/pins referenced color LUTs; on-demand loads them asynchronously when needed; disabled bypasses LUT grading while retaining other camera effects. Display loading/residency state and apply the selected missing-LUT policy until data is ready. |
| `camera.tone_map_output` | Select the camera grading output transform: SDR, automatic HDR/SDR based on the active surface, or explicit HDR10. Resolve against global HDR presentation capability, tonemap once, then perform the correct color-space/output encoding. Explicit HDR10 on unsupported output must fail or remain pending visibly. |
| `camera.missing_lut_fallback` | When a referenced LUT is missing/loading/invalid, use a neutral LUT grade, the last valid resident LUT for that camera, or bypass only the LUT stage as selected. Keep unrelated exposure/tonemap work. Report fallback status and clear stale per-camera state on project changes. |
| `camera.accessibility_reduce_flare` | Constrain authored lens-flare/halation brightness and rapid variation through a documented conservative accessibility profile. Preserve the authored asset and expose the effective reduced values. This is an accessibility reduction, not a medically certified protection claim. |
| `camera.accessibility_reduce_blur` | Reduce or suppress motion blur and rapid camera smear independently of the quality preset. Preserve base camera motion and authored shutter data; expose actual effective shutter/blur settings. A high-quality preset cannot override the user's reduction. |
| `camera.accessibility_disable_grain` | Remove film grain and gate weave from the final camera result and bypass their evaluation work. Preserve authored profiles for re-enabling. Changing a grain-quality preset must not override the disable. |
| `camera.accessibility_limit_fisheye` | Cap authored fisheye/lens distortion using a documented maximum-strength profile while retaining framing as safely as possible. Preserve authored parameters and report effective distortion; quality changes cannot bypass the cap. |
| `diagnostics.capture_repro` | Record bounded, versioned deterministic input events, seeds, step/config changes and relevant starting state sufficient for a replay. Provide explicit start/stop/export and retention rules. Do not record credentials or include arbitrary asset contents without a declared need. |
| `plugins.clap_host` | Enable discovery, loading and processing of supported CLAP audio plugins; off stops new loads and safely removes/bypasses active plugin processing through a documented transition. Preserve project references and report missing plugins rather than erase instrument chains. |
| `plugins.clap_sandbox` | Run third-party plugin code in a separate supervised process when enabled, with bounded shared audio/MIDI buffers, deadlines and crash recovery. A crash bypasses/mutes that plugin safely while the editor survives. Off is an explicit in-process mode, not a hidden fallback when process hosting is absent. |
| `plugins.ffmpeg_import` | Enable the actual FFmpeg-backed authoring import route for supported media formats. Off uses native supported decoders and reports unsupported formats. Detect the installed/packaged adapter, pass bounded arguments and preserve source/channel/rate metadata. |
| `plugins.reload_on_change` | Watch compatible plugin binaries and queue safe rescans/reloads outside rendering/audio callbacks. Coalesce file events, verify stable completed writes and preserve plugin state transactionally. Off stops automatic watching; incompatible/failed reloads keep the last usable instance. |
