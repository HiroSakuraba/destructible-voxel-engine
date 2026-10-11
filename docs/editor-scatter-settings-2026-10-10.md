# Scatter settings, variation and several surfaces

Follow-ups to Scatter Objects ([fill](editor-scatter-2026-10-10.md)) and the
[Scatter brush](editor-scatter-brush-2026-10-10.md) (worklist ART-093).

## Settings panel (Inspector)

While the fill preview is open, or the Scatter brush is the active tool, the Inspector shows
**Scatter settings** instead of the selected object's details:

| Row | Meaning | Wheel step |
| --- | --- | --- |
| Count | Copies to place (fill only; dimmed for the brush) | 5 |
| Spacing | Minimum distance between copies across the ground | 0.25 m |
| Seed | Layout seed | 1 |
| Align to surface | Tilt copies to the slope (click toggles) | toggle |
| Random turn | Each copy turns about its up axis by up to ± this many degrees; ±180 = any way | ±15° |
| Scale min / Scale max | Each copy gets a uniform scale in this range | 0.05 |
| Brush radius | Same setting as Ctrl+wheel on the ring (brush only) | ×1.15 |
| Brush density | Share of a full dab, 5–100% (brush only) | 5% |

Click a value to type a new one (Enter applies, Esc cancels; a bad value stays open with the
reason). The wheel over a row nudges it. The fill preview re-plans immediately. The fill
preview's keys (`[` `]`, Shift+`[` `]`, N, A) still work.

- **Turn and scale do not move spots.** They come from their own seeded stream, so changing
  them never re-rolls the layout, and copy *i* always gets the same turn and size for a seed.
- **Scale changes voxel size**, not voxel count (a 2× copy has the same voxels at twice the
  size), so it costs the same as an unscaled copy: no resampling, no extra voxels. It scales
  about the copy's footprint, so it still stands on its spot. Children scale with it.
  - The scale is limited per copy so every voxel size stays inside the editor's range
    (0.001–10 m, the Scale tool's limits); scale itself is 0.1–10, never zero or negative.
  - Only plain voxel objects grow. 3D text and Gabor volumes keep their voxel size (a larger
    voxel would only re-voxelize them coarser at the same size), and polygon meshes keep
    their size; their positions inside the copy still spread with the scale.
- **Density** caps how many copies a dab keeps, counting copies already under the brush, so
  going over an area again (or the overlap between dabs in a stroke) does not build up past it.
- One brush stroke adds at most 1000 copies (the fill's count cap), however long or dense it is;
  release and paint again for more.

## Saved with the scene

Each scatter group stores the settings that made it, as an editor-only component
(`dve.editor_scatter`) on the group. The settings are saved and loaded with the scene and dropped
from runtime exports.

- Brush strokes into an existing group update its stored settings in the same undo step.
- The first fill or brush session in a scene starts from the **newest** scatter group's settings.
- Selecting a scatter group (or a copy inside one) before starting Scatter Objects or picking the
  brush takes **that** group's settings. With the brush and the group itself selected, strokes
  then go into that group.
- Changing a setting in the panel is not an undo step by itself (no undo spam); it is stored on
  the group in the same single undo step as the fill or stroke that uses it.
- Older scenes load unchanged: a group without the component, or a component with missing,
  extra or wrongly typed keys, falls back to the defaults for those keys.

## Several surfaces (two-step selection)

1. Select what to copy (and/or a prefab in Assets), then **Create > Set Scatter Sources**.
2. Select any number of voxel surfaces, then **Create > Scatter Objects**. Every selected voxel
   object that isn't a source is a surface. Each candidate spot first picks a surface, weighted
   by its footprint, so copies spread evenly across all of them and no tries are wasted on gaps
   between them. The preview lists the surfaces and says "(set sources)".
   Hidden objects, objects isolated out of view (View > Isolate Selection) and earlier scatter
   copies are never surfaces, the same ground rules as the brush. Locked objects can be
   surfaces: scattering onto them adds new objects and does not change them.
3. **Create > Clear Scatter Sources** goes back to the one-surface rule (last clicked = surface).
   A hidden or isolated-out last-clicked surface is refused with a reason there too.

The brush also paints the set sources when they are set.

## Brush shortcut and Command Center

- **Create > Scatter Brush** (also found in the Command Center: "scatter", "paint", "foliage").
- Default key **Y** in the viewport (rebindable in the shortcut editor). The toolbar tooltip shows it.
- Picking the tool again starts a new session and group, unless a scatter group is selected.
- The accessibility tree now gives each toolbar button its real shortcut instead of its position.

## Other fixes

- The Scatter, Slice and Boolean preview panels now stop short of the camera preview inset at
  any `camera.preview_size`; they used to run underneath it.
- `CompoundCommand` gained `empty()` and `size()`.
- Headless screenshots: `--type TEXT` and `--wheel x,y,steps[,ctrl]` join `--key-down` and
  `--drag`, and all four now run in command-line order.

## Code

- `include/dve/editor_scatter.hpp`:
  - `ScatterSettings` (yaw, scale, brush radius and density), `sanitize_scatter_settings`;
  - `ScatterSample::yawRadians/scale`, `assign_scatter_variation`;
  - multi-target `plan_scatter` / `build_scatter_command`;
  - `make_scatter_settings_component`, `read_scatter_settings`, `append_scatter_settings_update`;
  - `ScatterDabSettings::density`.
- Controller:
  - `scatter_setting_rows`, `set_scatter_setting`, `nudge_scatter_setting`;
  - `set_scatter_sources`, `clear_scatter_sources`, `scatter_targets`;
  - layout `scatterSettingRows`;
  - actions `scatter.brush`, `scatter.set_sources`, `scatter.clear_sources`.

## Tests

`tests/test_editor_scatter_settings.cpp` (`dve_editor_scatter_settings_tests`):

- **Variation:** ranges; no effect on positions; deterministic.
- **Scaled and turned copies:** voxel size; still on the ground and centred; the turn is about
  up only.
- **Density:** proportional, and does not build up.
- **Two surfaces:** both covered, nothing in the gap, a non-voxel surface refused.
- **Settings component:** round trip (including a 64-bit seed), save and load, update and undo.
- **Inspector panel:** type, refuse bad input, toggle, wheel, min/max coupling, drawing, hidden
  object toggles.
- **Workflow:** set sources, then fill two surfaces; group stores settings; a selected group
  hands its settings over; a reopened scene continues from the newest group.
- **Shortcut and menus:** Y, menu items, accessibility shortcut.
- **Review follow-ups:** hidden and isolated-out objects are not surfaces (several or one);
  voxel size stays within 0.001–10 m; older or partial settings components load with defaults;
  Play hides the panel with the brush selected; Y typed into a text field is text; no shortcut
  conflicts for `scatter.brush` in any built-in profile.

Existing scatter and brush tests are unchanged and pass.

## Play / Simulate

The panel is shown only in Edit mode: starting Play or Simulate hides it (even with the brush
tool still selected) and drops any value being typed.

## What's not done

- Lightweight instances (copies that share one source's voxels) — a separate design.
- Slope or height limits, and per-source weights.
- The brush still previews markers, not ghost copies, so turn and scale show only after release.
- Settings changed in the panel without painting or committing are not stored anywhere.
- The panel does not scroll: in a very short Inspector the rows that do not fit are not shown
  (their values still apply and the fill keys still work).
- Panel edits are not undoable on their own.
