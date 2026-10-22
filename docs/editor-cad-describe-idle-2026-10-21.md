# Exact CAD descriptions: no per-frame scan (21 Oct 2026)

A fix for the exact CAD Measure work (#16x).

`poll_cad_descriptions()` runs from `update()` every frame. It walked every object to find CAD
sources that had not been read yet, copying each exact source's strings, even when nothing had
changed. With the 10,004-object scene in `dve_editor_frame_work_tests`, `update()` took about
450 µs, over that test's 400 µs limit, so the test failed on main.

The scan now runs only when the scene may have changed since the last scan found nothing to read:
- the object count changes;
- the undo history changes (count, undo label, undo/redo state);
- a description finishes;
- a test calls `wait_for_cad_descriptions`.

`update()` on that scene drops to about 1 µs. Behaviour is otherwise unchanged.
