# ASan leg fixes (2026-10-24)

Three problems kept `ctest (linux-gcc-asan)` red on main after PRs #200, #207 and #209.

1. **UBSan: `shift exponent 32 is too large for 32-bit type 'unsigned int'`** at
   `third_party/mikktspace/mikktspace.c` (`QuickSortEdges`; `QuickSort` has the same line). The
   pseudo-random pivot rotate shifted right by `32 - t` with `t == 0`. It is now guarded
   (`t ? rotate : uSeed`), which is the identity that x86 already produced, so tangents are
   unchanged. The file is marked as altered (zlib clause 2) and the change is recorded in
   `third_party/notices/MIKKTSPACE_NOTICE.md` plus a `mikktspace` entry in
   `third_party/notices/manifest.json`. Failing tests: `dve_editor_scene_export_tests`,
   `dve_player_runtime_tests`, `dve_player_export_scene`, `dve_editor_cad_detail_tests`,
   `dve_editor_exact_cad_tests`.
2. **`dve_editor_material_presets_tests`** failed on main at 6:32 PM, 7:08 PM, 7:50 PM and
   9:15 PM ET on Oct 23 with "latest result matches the requested shape/material; stale job
   cannot replace it". There was no sanitizer report: the CPU path-traced thumbnails are slow under ASan (the whole
   test takes about 15 s on an idle 8-core box, 60 s when one core is shared with busy loops), and the test shared one 30 s deadline across all waits, so a
   timeout was reported as a stale-image mismatch. It passed on later runs only because they were
   faster (33.6 s, 21.8 s of the same 30 s budget). Each wait now has its own 180 s deadline and
   timeouts have their own messages. `MaterialPreviewWorker` itself is correct (the commit checks the
   generation under the mutex).
3. **Disk full while linking** (`mold: failed to write to an output file`). ASan test executables
   that link `dve::editor` are about 630 MB each (about 210 MB of that is `.rela.dyn`, 8.8 M relative
   relocations) and the build tree has about 380 of them, leaving 0.9-3 GB of 145 GB free on recent
   green runs. The ASan leg now (a) links with `-Wl,-z,pack-relative-relocs` (about 33 % smaller
   executables, link flags only, so the ccache is unaffected), and (b) deletes the preinstalled
   Android SDK, .NET, GHC, Swift and hosted tool cache from the host through bind mounts before
   building. ASan still runs on main pushes and nightly (not on pull requests), at `-O1`, with the
   2500M compiler cache.
