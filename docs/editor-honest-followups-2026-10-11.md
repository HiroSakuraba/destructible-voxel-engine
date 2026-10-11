# Editor: progress and cancel for long jobs, import options, Lighting Only (2026-10-11)

Follow-ups to #101 (honest commands, merged as 36d2a7c), from its "What's not done" list.

## 1. Long jobs: progress and cancel

Import Model, Reimport Model, large Flood Fills, Validate Project and Rebuild Collision now run as
**long jobs**:

- **Where the work runs:** on a worker thread (`EditorTaskManager`, one worker). The worker reads
  a snapshot of its inputs, never the live scene:
  - Flood Fill: a copy of the object and of the voxel selection.
  - Rebuild Collision: copies of the objects' voxels.
  - Validate Project: a copy of the scene.
  - Import and Reimport: the parsed model and the chosen options.
- **Progress:** an overlay at the bottom of the viewport shows the job, its percent, a progress
  bar, the phase (for example "Voxelizing 2 of 5: Wheel" or "Filling: 49,152 voxels so far") and a
  **Cancel** button.
- **Cancel:** Esc or the Cancel button. The result is dropped, so the scene is unchanged and there
  is no undo entry. The worker stops at its next check (between meshes, or every 16k voxels).
- **Commit:** on the UI thread, as one undo step. Validate's results go to Problems, and Rebuild
  Collision's to the proxy cache; neither is an undo entry.
- **No edits while a job runs:**
  - Keys other than Esc, clicks (except Cancel), drags, the wheel and typing are refused, with
    "Busy: … Esc to cancel".
  - Every command except the viewport shading modes is refused too, including Undo, Play and a
    second job.
  - Starting a job ends any stroke, gizmo drag or text edit first.
- **The scene changed anyway:** live MCP or AI tools can still edit the scene. If they do, the
  result is not applied ("was not applied: the scene changed while it ran"). The check is a
  fingerprint of the command history and each object's name, parent, voxel revision, transform,
  flags and components. `EditorDocument::revision()` is the *saved* revision, so it cannot tell.
- **Small work finishes at once:** work under 250,000 voxels (or file bytes / 4 for reading a
  model) goes through the same code with no overlay, so small fills and validations feel the same
  as before. `set_long_job_threshold()` changes this; tests set it to 0.
- **Deterministic and headless:** `wait_for_long_job()` waits for the worker, then applies or
  drops the result as `update()` would. A cancel is final even if the worker already finished,
  because results are applied only by the UI thread.

## 2. Import options and Reimport

**File > Import Model (Ctrl+I)** first reads the model (a long job for big files), then opens an
in-editor options panel, like the file pickers:

- **Voxel size:** from 0.005 m to 10 m. Left/Right steps through presets; you can also type a
  number and press Enter.
- **Scale:** from 0.001 to 1000, applied about the model origin. The cooker bakes it into each
  mesh.
- **Fill:** Solid, Shell or Surface only.
- **Meshes:** one row per mesh node. Click or Space toggles it. Nodes that share a name are toggled
  together, because the cooker's node rules are keyed by name.
- **Estimate:** "up to N voxels", the sum of the included meshes' bounding boxes in voxel cells,
  with mesh and triangle counts. It is an upper bound: a solid fill reaches it only for box-shaped
  meshes. The panel says so.
- **Hard cap:** 32,000,000 estimated voxels. Above it, Import is refused before any voxelizing, with
  "Too large to import: about N voxels at X m; the limit is 32,000,000. Use a larger voxel size, a
  smaller scale, or leave meshes out". The voxelizer's working budget is set to the same number.
- **Running the import:** Enter (or the Import button) runs it as a long job, one mesh at a time.
  The parts go under "Imported <name>" as **one undo step**. Esc, Cancel or a click outside the
  panel closes it without importing.
- **Stored settings:** the choices are saved on the group as a `dve.editor_import` component.

**File > Reimport Model** (with the import group or any of its parts selected):

- It reopens the panel with the stored settings. Groups imported before this change start from
  their parts' voxel size.
- It rebuilds the parts from the source file. The group keeps its **id, name, transform, parent**,
  flags, tags and other components, and the new parts are placed relative to the group's current
  transform.
- It is **one undo step**; Undo restores the previous parts.
- Per-part edits (moved or recoloured parts) are replaced by the file's contents. The menu item is
  disabled, with a reason, when:
  - nothing imported is selected;
  - the group or a part is locked, hidden or isolated out of view;
  - the source file is missing;
  - Play is running.

Rules from #95–#102 that apply:

- **One preview at a time:** opening the panel closes any Slice, Boolean, Join or Scatter preview
  and disarms Flood Fill.
- **Isolation:** imported objects join an active isolation, like Paste. Reimported parts join it if
  their group was isolated.
- **Modal panel:** it is modal, and any other command closes it first.

## 3. Shortcut-only commands show their disabled state ahead of time

The polygon tools and the material commands now have menu items, disabled with the #101 reasons:

- Polygon: Vertex/Edge/Face/Object mode, Component toggle, Bevel, Extrude, Inset, UV Tools,
  Recalculate and Flip Normals.
- Materials: Assign, Apply, Link, Create Instance, Create Master, Preview, Pin Preview.

They are always in the **Command Center** (with the reason on the row). They are in **Tools**
(sections "Mesh Editing (not in this editor)" and "Material Editing (not in this editor)") when
View > Advanced Menus is on, so the default Tools menu does not get longer.

Their Polygon/Material Editor keys (B, E, M, P, …) are not shown next to these rows. Those keys
apply only in editors this build does not have, and next to a viewport menu they would read as
viewport keys; M, for example, is the selection domain. Running one still says why it is not
available.

## 4. Lighting Only

View > Lighting Only (Alt+8) used to be flat grey. It now shades each voxel's camera-facing
exposed face like a white surface under the scene's light:

- **Sun:** the sun's N·L, using the sun direction, colour and intensity.
- **Ambient:** sky and ground colours blended by how far the face points up.
- **Exposure and source:** both scaled by exposure. The values come from the same
  `RenderEnvironment` settings Play uses.
- **Code:** `editor_lighting_preview.hpp` (`lighting_only_color`), which is unit-tested.

There are no shadows or bounce light; those are the GPU renderer's.

## What's not done

- **Native OS file dialogs:** not built. It would take a per-platform backend: GTK/portal
  (`org.freedesktop.portal.FileChooser` over D-Bus) on Linux, `IFileOpenDialog` on Windows and
  `NSOpenPanel` on macOS. The dialog would run off the editor's frame loop, and the chosen path
  would come back through the same `open_scene_file` / `save_scene_as` / `open_import_options`
  entry points. The X11 host has no toolkit today, so this is a new dependency and a packaging
  decision.
- **Material editing:** not built. It would take a material asset format with instances and
  parameter overrides, an editor panel (graph or parameter list) with a preview sphere rendered by
  the GPU path, and "Apply" that recompiles and swaps the voxel palette entries, all undoable.
  Today Paint, Flood Fill and the Material Library cover per-voxel materials.
- **Polygon mesh editing:** not built. Models import as voxels, and there is no editable polygon
  mesh in the scene. It would take:
  - a half-edge mesh object type with save/load;
  - vertex/edge/face picking in the viewport;
  - the Bevel/Extrude/Inset operators, UV tools and normals, all as undoable commands;
  - a re-voxelize step to feed destruction and physics.
- **Long jobs in other places:** Join, Slice, Boolean and Separate Islands still run on the UI
  thread. They have previews and caps, but no progress overlay.
- **Cancelling the parser:** cancel stops between meshes and every 16k fill voxels. Reading a
  model file (`import_model`) cannot be interrupted part-way, so its result is simply dropped.
- **Import options:** there are no per-mesh settings (mode, anchored, collision per node) and no
  thumbnail preview of the result; the estimate is a bounding-box upper bound, not a voxelized
  count.
- **Reimport:** it does not keep edits made to individual parts.
- **Lighting Only:** no shadows, no GI and no point lights; it is per voxel square, not per pixel.

## Tests

`tests/test_editor_honest_followups.cpp` (`dve_editor_honest_followups_tests`):

- **Long jobs:**
  - A background Flood Fill shows the overlay (label, %, Cancel, inside the viewport).
  - Undo, Play and a second job are refused while it runs, keys do not edit, and shading still
    switches.
  - It commits one undo step that one Undo restores.
  - Esc and the Cancel button leave the scene and the undo stack unchanged.
  - A new job runs after a cancel.
  - A result computed from a scene that changed meanwhile is not applied.
  - Validate and Rebuild Collision run as jobs, cancel, and add no undo entry.
  - Small work finishes at once.
- **Import options:**
  - The panel lists the glTF's two meshes.
  - Voxel size steps and typed values change the estimate, and scale raises it.
  - A bad value is refused; a mesh can be left out.
  - Over the cap is refused with the limit, and the panel fits.
  - Import is one undo step and stores its settings.
  - Esc imports nothing, and there is no import during Play.
- **Import as a long job:** reading and importing are jobs, cancel adds nothing, and a full run
  adds the group and two parts in one undo step.
- **Reimport:**
  - It is found from a part.
  - It keeps the name, parent and moved transform, and rebuilds the parts at the new voxel size
    where the group is.
  - It is one undo step that Undo restores.
  - Locked and hidden groups are refused.
- **Previews:** opening Import closes a Slice preview.
- **Helpers:** the bounding-box estimate, scale, nothing-included refusal, settings round-trip and
  model extensions.
- **Shortcut-only commands:**
  - Disabled with reasons, with no viewport-looking key.
  - In Tools only with Advanced Menus, and in the Command Center with the reason.
  - M still toggles the selection domain.
- **Lighting Only:** brighter toward the sun, ambient on the far side, follows the sun direction,
  exposure scales it, and the viewport draws the lit shades.
