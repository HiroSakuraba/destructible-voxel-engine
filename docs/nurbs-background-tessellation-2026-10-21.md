# Background adaptive NURBS CV transforms

Adaptive-surface CV gizmo drags submit immutable snapshots to one owned worker. Input handlers do not tessellate. Each request supersedes the running request and replaces the one pending request; cooperative stop checks run during fixed and adaptive sampling. Only the latest generation can be delivered. The worker never accesses the live document or controller.

The viewport keeps the last complete display while the next one is computed. A completed preview installs its full-quality source and polygon cache together on the editor thread. Releasing the mouse or pressing Enter waits through subsequent editor updates without blocking the input handler. Escape or focus loss cancels and restores the pre-drag source/cache. Selection changes, document revisions, changed placement, locking/hiding, deletion and changed source/cache invalidate the active transform. External source/cache changes are preserved. An unchanged drag creates no command.

A successful release adds one prepared EditNurbsCommand. Its before/after tessellations are prepared and validated by the worker. Commit, undo and redo reuse these immutable caches with stale-source and stale-geometry checks instead of re-tessellating. Current material/texture resources are preserved by cached command application. Superseded, failed and cancelled requests cannot add history. Worker destruction cancels and joins its owned thread.

Autosave skips an active NURBS drag. Editor commands other than view actions are refused until the transform finishes or is cancelled; Undo first cancels the transform. Direct document snapshots still contain a complete matching source/cache pair, never a half-built mesh. No job state is serialized. The source codec is unchanged: fixed defaults continue to encode v2, nondefault/adaptive policy uses v3, and v1/v2 remain readable.

## Autodesk workflow references

- [Maya: Set NURBS Tessellation Options](https://help.autodesk.com/cloudhelp/2027/ENU/Maya-Rendering/files/GUID-F4CCA0A7-C964-4BA5-AE5C-81A6696820F6.htm) documents approximation controls, object-space chord height and Escape interruption for automatic tessellation runup. DVE follows the principle of interruptible evaluation and artist-selected approximation, using its existing local-space presets. The page does not establish that Maya uses this queue architecture.
- [Softimage 2014: Geometry Approximation on NURBS](https://download.autodesk.com/global/docs/softimage2014/en_us/userguide/files/nurbs_basics_GeometryApproximationonNURBS.htm) separates Hardware Display from rendered Surface/Surface Trim approximation. DVE keeps exact editable rational controls and a derived display cache, but does not yet offer separate viewport/render policies.

This is a scoped responsiveness improvement, not Maya/Softimage feature parity. There is no coarse proxy or camera-dependent LOD: a preview uses the selected complete fixed/adaptive display policy. Fixed-density drags, numeric property edits, display-preset changes, constructors and explicit polygon bakes still use their existing synchronous paths. Scheduling those paths, independent interactive/render quality, persistent patch adjacency and synchronized boundary tessellation remain follow-ups. Sampled adaptive error tests remain bounded but uncertified.

## Validation

14/14 selected local CTest suites passed: background, adaptive display, modeling, length, degree, boundaries/join, display, topology, construction, control tools, NURBS core/editor, native polygon, subdivision and scene export.

The dedicated background suite covers pre-cancelled tessellation, newest-request delivery, cancellation/reuse/shutdown, worker failure without partial application, prepared stale-cache/lock rejection, material-preserving cached history, native asynchronous drag/release/cancel, source/cache coherence, external edits, selection/deletion and scene save/reopen. It uses bounded polling and thread yields rather than sleeps. Headless regression tests do not establish windowed/GPU performance or artist acceptance.
