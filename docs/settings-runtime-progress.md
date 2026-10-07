# Settings runtime integration progress

Based on main `28d391f1b3c7b41a514dfee7dcc12a3fa360ebd1` (PR #59, parallel draw-list sort).

This is the first implementation batch for the 134 settings identified as unapplied.
It connects 43 IDs to editor/runtime consumers; 91 remain unapplied. The complete
134-setting behavior contract is not implemented by this batch. The registry's
`applied` flag and source-reference audit indicate wiring, not full behavioral certification.
No placeholder reads have been added to claim coverage of the remaining settings.

## Behavior and application boundaries

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

## Validation and limits

Ten targeted CTest checks pass: settings runtime behavior, registry, reference audit,
Play session, camera system, draw culling/sorting, desktop contract smoke, SDL host, SDL audio and audio runtime.
The new behavioral check verifies real body movement, camera override restoration,
look-ahead, rendered PCM, callback-size-independent gain ramps, bounded queue retry,
palette pixels, focus outline and input-repeat/stick behavior.

The build uses the deterministic SDL shim and reference physics. Actual SDL hardware,
Jolt, Box3D, Lua and legacy X11 integration have not been validated in this environment.
The full engine test suite has not been run. The packaged player does not load the editor
settings registry, so this batch is not a claim of identical player-side behavior.
The draft must remain open until platform integration and remaining acceptance work are
complete; passing the source audit alone is insufficient to merge an all-settings feature.

## Connected IDs in this batch

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

## Remaining 91 settings

These retain the unapplied marker in the UI. Each row records the intended behavior,
not a completed implementation. Existing engine subsystems may provide part of a row;
they still need actual owners, resource/lifecycle integration and behavioral checks.

| Setting | Required behavior |
| --- | --- |
| `editor.restore_workspace` | When enabled, restore panel positions, active tabs, open assets and viewport state from a versioned per-project workspace record. When disabled, start from the default layout and ignore the saved record. Do not reopen untrusted assets automatically or confuse layout recovery with unsaved-scene recovery. |
| `editor.telemetry_local` | Enable bounded local collection of frame, rendering, task and audio statistics. Turning it off stops optional collection and persistence; error reporting still works. Never transmit this data. Expose retention, clear-data and capture status so the setting has a visible meaning. |
| `camera.navigation_style` | Select a documented navigation binding profile for DVE, Unity, Unreal or Blender conventions. Route orbit, pan, fly, dolly and modifiers through the input action system. Show each profile's actual bindings; switching must release held actions and preserve camera pose and user overrides. |
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
| `render.exposure` | Apply a linear scene-light multiplier before tonemapping. One is neutral; doubling adds one exposure stop. Combine with authored camera exposure once, never multiply again in a later output transform. UI colors remain unaffected. |
| `render.tonemap` | Select an explicit ACES-inspired, Reinhard or clamp curve in the common scene-color pipeline. Publish the exact curve/variant and preserve a reference implementation. Apply tonemapping once before output encoding; camera grading/output selection must not tone-map again. |
| `render.bloom` | Enable or bypass the HDR bloom extraction, blur and composite passes. Off must also remove their transient work and allocations where possible. Bloom is scene-only, preserves exposure ordering and does not contaminate editor UI. |
| `render.bloom_threshold` | Set the soft-knee bloom extraction threshold in linear, exposed scene-light units. Define the knee alongside the threshold and keep it stable across presets. Zero permits all positive radiance; raising the threshold cannot increase extracted energy. |
| `render.texture_filter` | Select nearest, bilinear, trilinear, or anisotropic samplers for applicable scene textures. Mipmap-dependent modes need valid mip chains; missing mips must use an explicit effective fallback. UI glyph atlases retain their own appropriate sampling rules. |
| `render.anisotropy` | Set the requested maximum anisotropic sample level only when anisotropic filtering is selected. Clamp to device-supported limits and display the effective level. Do not rebuild texture pixels or use an unsupported sampler silently. |
| `render.texture_budget_mb` | Bound resident scene-texture memory in mebibytes using measured allocation sizes and a streaming residency manager. Evict unpinned least-needed resources, retain fallback mips and avoid evicting in-flight or required textures. Report pinned requirements that exceed the budget. |
| `render.gi_mode` | Switch indirect lighting between off, ambient approximation, and voxel one-bounce tracing. Off removes indirect contribution and work but retains direct light. Ambient must use a documented source; one-bounce uses voxel occupancy/materials and reacts to destruction. |
| `render.gi_quality` | Select documented sample counts, resolution and temporal treatment for the active indirect-light method. Quality presets change cost/noise, not physical intensity or trace distance. Disabled GI schedules no work regardless of the preset. |
| `render.gi_intensity` | Multiply only diffuse indirect illumination; zero removes its contribution, one is neutral. Keep direct lighting, emissive radiance and ambient-occlusion policy separate so this is not a hidden overall exposure control. |
| `render.gi_distance` | Limit one-bounce searches to this distance in meters. Stop tracing beyond the limit without deleting distant geometry or direct shadows. Preserve conservative occupancy traversal so thin blockers inside the limit are detected. |
| `render.shadow_mode` | Select off, hard visibility, soft area-light sampling, short-range contact shadows, or a documented hybrid. Hybrid combines complementary estimates without multiplying the same occlusion twice. Off removes shadow work but keeps the light. |
| `render.shadow_quality` | Select published shadow sampling/resolution budgets. Higher quality improves stability and noise while retaining the same light size, strength and contact distance. Shadow-off bypasses all preset work. |
| `render.shadow_strength` | Blend between unshadowed and fully shadowed direct-light visibility using the configured zero-to-one strength. Zero ignores shadow attenuation; one uses full visibility results. Do not darken indirect light a second time. |
| `render.shadow_softness` | Treat the value as the directional sun's angular radius in radians. Zero approaches hard shadows; positive values widen penumbrae according to blocker/receiver geometry rather than a uniform image blur. |
| `render.contact_shadow_distance` | Bound short-range contact-shadow visibility queries in meters. Detect thin nearby blockers, fade the contribution smoothly near the endpoint and leave far directional shadows to their own method. |
| `render.ao_quality` | Choose explicit ambient-occlusion sampling/resolution presets. Occlusion represents local geometric shading, not a duplicate shadow or GI term. Keep material ambient-occlusion inputs and screen/voxel estimates combined through a documented rule. |
| `render.translucent_layers` | Limit the number of transparent continuation layers per scene ray. Composite premultiplied color/transmittance in front-to-back order and define how residual background contribution is resolved at the cap. This does not change collision or picking geometry. |
| `geometry.mode` | Choose voxel-only, polygon-only or hybrid geometry support for the next configured/package build. Validate scenes and dependencies against the selected mode; preserve authored assets in the editor and reject incompatible package contents instead of silently stripping required geometry. |
| `voxel.brick_budget` | Set the cap on resident voxel render/streaming bricks, with explicit CPU/GPU accounting. Keep authoritative destruction and physics data intact. Evict only safely reloadable, unpinned representations; report pinned working sets exceeding the request. |
| `voxel.destruction_quality` | Select documented visual debris and collision-detail budgets for destruction. All presets must preserve exact authoritative voxel removal and structural connectivity; quality may change decorative debris, proxy refinement and scheduling, not which supporting voxels exist. |
| `voxel.debris_limit` | Cap active decorative debris bodies. Zero suppresses decorative debris creation; exceeding the cap reuses or retires least-relevant decorative bodies through a deterministic policy. Essential structural fragments remain authoritative and are counted separately. |
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
| `material.subsurface` | Enable thickness-aware subsurface transmission where a valid thickness representation exists. Off uses the ordinary surface model. Missing thickness data needs a reported fallback, and reference/GPU models must share units and bounded energy rules. |
| `material.validate_gpu_layout` | Validate CPU/shader material record sizes, offsets, alignment, identifiers and versioning when loading/building resources. A mismatch prevents publishing incompatible buffers and reports the field; disabling optional validation does not disable essential bounds/type checks. |
| `audio.sample_rate` | Request the device/engine sample rate in hertz at audio-engine restart. Negotiate actual hardware support and report the actual rate; resample imported clips and synth/control timing consistently. Retain the previous working device if reopening fails. |
| `audio.buffer_frames` | Request the hardware callback block size in frames on device restart. Accept/report the size actually negotiated, size rings safely and avoid assuming callback length is constant. The setting must visibly change latency or report a driver-fixed block size. |
| `audio.spatializer` | Choose native spatialization, a functioning Steam Audio adapter, or no spatial processing. None uses the authored nonspatial mix. Keep source identity, gain and playback phase through safe transitions; report unavailable external backends rather than silently substituting. |
| `audio.hrtf` | Enable binaural head-related transfer function processing for supported stereo headphone spatializers. Off uses documented pan/distance processing. Mono output and unsupported backends must show why binaural processing is inactive. |
| `audio.granular_quality` | Select published active-grain limits and interpolation/window quality for granular synthesis. Admission remains bounded and stealing/fading avoids clicks. Presets change cost and fidelity without changing tempo, pitch or automation semantics. |
| `audio.stream_preload_ms` | Set the target look-ahead of decoded audio in milliseconds, converted using the active sample rate. Decode/read on workers, not the callback. Zero minimizes deliberate prebuffering but still uses a safe minimal ring and clearly reports underruns. |
| `audio.loudness_normalization` | During export/cooking, measure integrated loudness and apply the selected project target plus true-peak constraint. Off leaves the intended export gain unchanged. Add explicit target/peak controls or a documented profile; a boolean alone cannot choose a loudness target. |
| `input.gamepad_prompts` | Select keyboard, Xbox, PlayStation or Switch glyph families for action bindings. Automatic follows the most recently active meaningful input device with hysteresis, excluding noisy idle axes. Display the real assigned binding, not a hardcoded button label. |
| `input.raw_mouse` | Use raw relative pointer motion for captured viewport look when supported; off uses the platform's normal pointer processing. Preserve normal absolute UI pointing, escape-to-release capture and focus-loss recovery. |
| `scripting.hot_reload` | Watch scripts and reload changed modules through a transactional compile/migrate/commit path. Keep the prior working script on failure, preserve defined state and suppress duplicate lifecycle events. Off stops automatic reload watching but does not stop ordinary script execution. |
| `scripting.migration_timeout_ms` | Bound each hot-reload state migration by the configured milliseconds using executable interruption/instruction budgets. Check time during execution, including interruptible host calls where supported; a post-completion timeout check does not stop an infinite migration. |
| `scripting.strict_errors` | When enabled, unresolved script compile, binding or migration errors fail scene/build validation. When disabled, allow a clearly diagnosed disabled/fallback script state according to policy; never execute invalid code or hide errors. |
| `build.configuration` | Choose a real Debug, Release or Distribution build profile that controls compiler optimization, symbols, assertions and package contents through documented mappings. Show the resulting build configuration; merely saving a label is insufficient. |
| `build.target` | Select host, Windows, Linux or macOS for the generated build/package job, with correct toolchain, ABI and dependencies. Host resolves to the current platform. Reject unavailable cross-toolchains before starting rather than generating a host binary under another name. |
| `build.sanitizers` | For supported development toolchains, enable AddressSanitizer and UndefinedBehaviorSanitizer flags consistently for compilation and linkage. Keep the resulting build separate from distribution artifacts. Report incompatible platforms or profiles explicitly. |
| `build.headless` | Build/package a runtime that starts without windows or graphics presentation while retaining required simulation, scripting, assets and explicitly supported audio. Exclude presentation-only dependencies; do not mean merely hiding a window. |
| `build.deterministic_oracles` | Run selected reproducible render/audio reference checks as part of packaging verification. Store seeds, fixture versions, tolerances and results with the build. Off skips these optional checks, not essential structural/integrity validation. |
| `build.verify_dependencies` | Verify the complete required asset dependency closure and expected content hashes before publishing a package. Detect missing, stale and mismatched cooked dependencies and report the chain. Off may skip optional hash rechecking, but cannot publish dangling required references. |
| `accessibility.color_vision` | Adapt editor/diagnostic palettes and redundant symbols for the selected color-vision mode. Keep semantic distinctions visible without relying on hue alone. Do not globally alter authored scene colors unless an explicitly separate simulation preview is requested. |
| `accessibility.subtitles` | Show authored dialogue subtitles and audio captions when enabled, synchronized to actual playback/seek events and readable within safe areas. Off hides caption presentation, not dialogue audio. Required subtitle/caption authoring data and controls must exist. |
| `camera.input_acceleration` | Treat the configured seconds as movement acceleration time: zero reaches requested fly speed immediately, positive values approach it smoothly with a documented response. Apply to continuous camera movement, not repeat events or mouse position. |
| `camera.input_smoothing` | Low-pass filter camera look/movement input with this time constant in seconds. Zero bypasses smoothing. Reset history on capture/focus/rig changes to avoid residual drift; do not double-smooth input in the platform layer. |
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
| `diagnostics.gpu_markers` | Emit meaningful named graphics-processing-unit pass/resource markers through the active backend's debug-label facility. Off bypasses optional label work. Only claim support when the backend/debug facility is actually available. |
| `diagnostics.capture_repro` | Record bounded, versioned deterministic input events, seeds, step/config changes and relevant starting state sufficient for a replay. Provide explicit start/stop/export and retention rules. Do not record credentials or include arbitrary asset contents without a declared need. |
| `plugins.clap_host` | Enable discovery, loading and processing of supported CLAP audio plugins; off stops new loads and safely removes/bypasses active plugin processing through a documented transition. Preserve project references and report missing plugins rather than erase instrument chains. |
| `plugins.clap_sandbox` | Run third-party plugin code in a separate supervised process when enabled, with bounded shared audio/MIDI buffers, deadlines and crash recovery. A crash bypasses/mutes that plugin safely while the editor survives. Off is an explicit in-process mode, not a hidden fallback when process hosting is absent. |
| `plugins.ffmpeg_import` | Enable the actual FFmpeg-backed authoring import route for supported media formats. Off uses native supported decoders and reports unsupported formats. Detect the installed/packaged adapter, pass bounded arguments and preserve source/channel/rate metadata. |
| `plugins.reload_on_change` | Watch compatible plugin binaries and queue safe rescans/reloads outside rendering/audio callbacks. Coalesce file events, verify stable completed writes and preserve plugin state transactionally. Off stops automatic watching; incompatible/failed reloads keep the last usable instance. |
