# Commands that now do what they say

The `3129a23` audit listed editor commands that reported success while doing nothing (ART-114),
plus related gaps. Every command below now does its job, or is refused with a reason and an
alternative.

## Voxel and viewport

The Voxel commands are listed under the Tools menu; the Command Center shows them as "Tools > ...".

| Command | Now |
| --- | --- |
| **Voxel > Flood Fill** | Arms a one-shot fill. The next viewport click repaints the clicked voxel's face-connected region of the same material with the active material, as one undo step. The viewport shows "Flood Fill: click a voxel"; Esc cancels. Refuses empty cells, locked objects, and regions that already use that material. The region is capped at 2,000,000 voxels. **F is no longer bound to it**: in voxel tools F used to shadow Frame Selection. |
| **View > Drop Selection to Surface** (End) | Each selected object (with its subtree) drops straight down onto the highest surface under its footprint, sampled with a 5×5 grid of rays. Only visible objects that are not being dropped count; with nothing below, it drops onto the y = 0 grid. One undo step. Reports objects already resting and objects with nothing below. Attached children follow their parent. |
| **Voxel > Rebuild Collision** | Rebuilds the merged box proxy that Play, Boolean and Join use, for the selection (or every voxel object), and reports the box count. |
| **View > Collision Shapes** | Draws the selected objects' actual proxy boxes at their world pose, instead of a bounds box. Capped at 4,000 boxes, with a note when more exist. The **Collision** shading mode draws every visible object's proxy. |
| Inspector "Collision boxes" | Now counts the **merged** proxy, so it matches what Play builds. It used to count the unmerged one. |
| **Voxel > Rebuild Brickmaps** | Disabled in the menu and the Command Center with the reason: this viewport draws voxels directly, and brickmaps belong to the runtime renderer. |
| **View > Shading** (Alt+1…9; Alt+6 is Collision; the other eight are also in the View menu) | The viewport reads `viewport.shading_mode`. A tag at the bottom left names the mode ("View: Wireframe  (Alt+1 Lit)"), and the menu checks the active one. Modes are described below. |
| **View > X-Ray Selection** | The selection's voxels are drawn again on top of everything, so they show through what is in front of them. |
| **View > Follow Selection** (Shift+F, now also in the View menu) | Frames the selection, then moves the camera with it as it moves. Off when toggled again, when nothing is selected, or when Play starts. It used to only print "toggled". |

The shading modes:

- **Lit:** the default depth shading.
- **Unlit:** flat material colour.
- **Wireframe:** outlines only.
- **Material IDs:** one distinct colour per material.
- **Normals:** the camera-facing exposed face of each voxel, shown as an RGB normal.
- **Lighting Only:** grey.
- **Voxel Debug:** one colour per 8×8×8 brick.
- **Overdraw:** how many voxel squares cover each 8 px cell: 1 blue, 2–3 green, 4–7 yellow, 8 or more red.

## Build > Validate Project

Validate Project lists real problems in **Problems** and switches to that tab. It checks:

- the scene structure;
- missing source assets, fonts and prefab files;
- invalid components, and components whose asset references (script, audio and so on) are
  missing or empty;
- voxel objects with no voxels, children or components;
- collision proxies that do not cover their voxels;
- stale or broken assets in the index.

Each run replaces its own previous results. A clean run leaves one "Validation passed" note. It
used to clear Problems and say "completed".

## File menu

The native editor has no OS file dialog, so these commands open a picker inside the editor:
type to filter, Up/Down to choose, Enter to accept, Esc to close.

- **Open Scene (Ctrl+O):**
  - Lists the project's recent scenes first, then every `.dvescene` under the project. Hidden,
    `cooked` and `build` folders are skipped.
  - You can also type a path; it must be a `.dvescene` file.
  - With unsaved changes it asks first, like New Scene.
  - Opening clears undo and isolation, frames the scene, and records it as recent in
    `project.dveproject`.
- **Save As (Ctrl+Shift+S):**
  - Type a name and the scene is saved as `scenes/<name>.dvescene` in the project. A relative path
    with folders (`levels/Night`) saves under the project. Clicking an existing scene fills in its
    name, keeping its folder when it is not in `scenes/`.
  - The target must stay inside the project: `..` escapes, rooted paths outside it, and names with
    `< > : " | ? * \` are refused with the reason. Rooted means `has_root_directory()` or
    `has_root_name()`, not `is_absolute()` (on Windows `"/foo"` is not absolute).
  - An existing file needs a second Enter to replace it, and the confirmation is for that exact
    file: choosing another row with Up/Down asks again.
  - The old name's recovery copy is removed once the save succeeds.
  - The saved file becomes the current scene and is recorded as recent.
  - **Save** on a never-saved scene now opens Save As, instead of failing with "pass --scene".
- **Open Project (Ctrl+Shift+O, now bound):**
  - Lists folders that hold a `project.dveproject`: the current project, its children and
    siblings, and the working directory's children. Each is shown by its project name.
  - You can also type a folder path.
  - Opening activates the project (asset index, workspace state) and opens its most recent scene,
    or a new scene if it has none.
- **Import Model (Ctrl+I, now bound; Polygon > Import Polygon Model runs the same command):**
  - Select an OBJ, glTF or GLB file in the Assets panel first; the command is disabled with that
    reason until you do.
  - The model is voxelized one object per mesh node, keeping node transforms. (Follow-up: an
    options panel comes first; see `docs/editor-honest-followups-2026-10-11.md`.)
  - The objects are added under an "Imported <name>" group in one undo step. The group is selected
    and framed.
  - A summary reports the objects, voxels and voxel size; any warnings go to the Console.
  - Not done yet: the step-by-step import options (node choices, voxel settings, preview) and
    reimport.

## Honest fallbacks

- **Material > Assign / Apply:** "Not in this editor yet: use the Paint tool (5) or Voxel > Flood
  Fill".
- **Material instance and preview commands:** say they are not available, and point to the
  Material Library.
- **Polygon mesh editing commands:** say mesh editing is not available, and that models import as
  voxels.
- **Audio and sequencer shortcuts outside their editors:** say to open that editor first.
- **Audio > Import Audio**, **Create > Polygon Mesh**, **Build > Cook Scene**, **Build > Package
  Game** and **Help > Documentation:** disabled in the menu and Command Center with the reason
  and the alternative. None of them had a handler; they fell through to "not connected".
- **Any other unconnected action:** "Not available in this editor build: <id>", shown as an error.

All of these return failure instead of reporting success.

The polygon/audio/sequencer catch-all now runs at the end of dispatch. It used to run first and
shadow real handlers: Polygon > LOD/Collision/Streaming Settings and Audio > Audio Settings, Mixer,
Synthesizer, Tracker, Interactive Audio and Music and Sound Editor reported "Shortcut dispatched"
on main instead of opening.

## Menus and gating

- Flood Fill, Rebuild Collision, Rebuild Brickmaps (disabled) and Drop Selection to Surface are now
  menu items, so they appear in the Command Center with their state.
- Disabled with a reason: Flood Fill and the file pickers during Play/Simulate; Drop to Surface
  with nothing selected; Import Model without a selected model; Follow Selection with nothing
  selected.
- Flood Fill: starting Play/Simulate, Join, Slice, Boolean or Scatter disarms it. Arming it closes
  an open Join/Slice/Boolean/Scatter preview, one preview at a time, as Join does. A fill never runs
  during Play, and hidden or isolated-out objects are refused.
- Drop to Surface: a hidden or isolated-out selected object is refused with the reason, the same
  rule as Join and Scatter (#99/#100).
- Opening a scene also clears an armed fill, Follow Selection, text edits and gizmo drags.

## What's not done

- **Long-job progress and cancel:** done in the follow-up, see
  `docs/editor-honest-followups-2026-10-11.md`.
- **Import options:** options panel, estimate, import cap and Reimport are done in the follow-up.
- **OS file dialogs:** the pickers are in-editor lists; there is no native Open/Save dialog.
- **Material editing commands:** Assign, Apply, Link, instances and previews are refused with a
  reason, not implemented.
- **Polygon mesh editing:** vertex, edge and face tools, bevel, extrude and UV tools are refused.
- **Audio > Import Audio, Build > Cook Scene / Package Game, Help > Documentation:** disabled; use
  the command-line `dve_cook_*` and `dve_pack` tools and the `docs/` folder.
- **Brickmaps:** Rebuild Brickmaps stays disabled; the runtime renderer owns brickmaps.
- **Shading modes:** these are colourings of the software viewport's voxel squares, not GPU
  render passes. Lighting Only now uses the scene's sun (N·L plus sky/ground ambient; follow-up).
- **Shortcut-only commands:** now menu items, disabled with their reason (follow-up).

## Tests

`tests/test_editor_honest_commands.cpp` (`dve_editor_honest_commands_tests`):

- **Flood Fill:** arm, Esc, click fill (region only), undo, refusals.
- **Drop to Surface:** onto the floor, the child moves too, already resting, undo, onto the grid,
  nothing selected.
- **Collision:** rebuild reports the merged count, the Inspector matches Play, the proxy follows
  edits and collision off, Collision Shapes draws the boxes, Rebuild Brickmaps is refused.
- **Shading and X-Ray:** every shading mode draws with its tag, Wireframe outlines, Overdraw heat
  map, menu check, X-Ray.
- **Follow Selection:** refused with nothing selected, the camera follows a move.
- **Validate Project:** finds a missing script, a missing source and an empty object; a clean run
  shows only "passed".
- **File pickers:** Open Scene (list, filter, open, recent, ask on unsaved changes); Save As (new
  name, replace confirmation, Save on a new scene); Open Project (sibling list, opens its scene).
- **Import Model:** needs a selected model, imports an OBJ crate as a group, summary, one undo.
- **Fallbacks:** say what is not available.
- **Review fixes:** Flood Fill refuses hidden objects, closes a Join preview when armed, is
  disarmed by Simulate and is disabled in Play. Drop to Surface refuses hidden and isolated-out
  objects, and its menu item is disabled with a reason. Save As does not carry a confirmation
  across rows, refuses paths outside the project, saves `levels/Night`, and prefills the folder.
  Open Scene accepts a typed absolute path and refuses non-scene files. Menu and Command Center
  reasons for the disabled commands, the Ctrl+I and Ctrl+Shift+O shortcuts, and Polygon > LOD and
  Audio Settings are no longer shadowed.
