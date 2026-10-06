# Editor asset scan: incremental hashing and deferred thumbnails

`EditorAssetDatabase::scan` ran on the editor's UI thread when opening the Assets tab,
on Refresh/Ctrl+R and after every prefab save, and its cost grew with the size of the
whole project even when nothing had changed. Baseline: main commit `de51c33`.

## What changed

- **Unchanged files are not re-read.** A file whose size and modification time match
  the previous index keeps its stored content hash. Files modified within two seconds
  of the previous index write are always re-hashed, so a same-size edit that lands in
  the same time-stamp tick (coarse file systems) cannot be missed; this is the same
  "racily clean" rule git uses. Hashes are persisted in the index, so this also holds
  across editor restarts. `EditorAssetScanReport` now reports `hashedFiles` and
  `reusedHashes`.
- **Dependency text is not re-read.** Tokens extracted from text assets are cached in
  memory by path and content hash and re-resolved against the current paths on each
  scan.
- **Relative paths are computed lexically.** `std::filesystem::relative` resolved both
  paths with extra system calls for every file; entries come from iterating under the
  project root, so `lexically_relative` gives the same result.
- **The index is not rewritten when nothing changed.** If no file was hashed and every
  record matches what was loaded, the file on disk already holds exactly these records.
- **Thumbnails no longer block the editor.** `generate_missing_thumbnails` looked each
  asset up again by id with a linear search (quadratic overall) and wrote each 64x64
  image three bytes at a time; it now writes from the record and in one call (output is
  byte-identical). The editor scans with `EditorAssetScanOptions::generateThumbnails =
  false` and fills missing thumbnails from `update()` within a 2 ms budget per frame via
  the resumable `generate_missing_thumbnails_for`; an error is logged once and pauses
  the backlog. `scan()` without options keeps writing thumbnails before returning, so
  `dve_asset_index` and existing callers are unchanged.

## Measurements

Warmed file cache, GCC 13.3 `-O2`, same machine. Repeat scans are the common case
(refresh, prefab save).

| Project | Scan | main | This change (editor mode) |
|---|---|---:|---:|
| 550 files, 607 MB (4 MB textures + text scenes) | repeat, nothing changed | 1,000-1,040 ms | 9-12 ms |
| 20,000 files of 1 KB | first scan (no index) | 16,020 ms | 328 ms (thumbnails then filled per frame) |
| 20,000 files of 1 KB | repeat, nothing changed | 680-1,100 ms | 320-430 ms |

With thumbnails written inside the scan (the command-line default), the 20,000-file
first scan takes 6.9 s instead of 16.0 s.

What remains on a repeat scan of a very large project is mostly re-reading the index
file (about 130 ms for 20,000 records) and one or two `stat` calls per file. Moving the
whole scan to a worker thread would hide that, but callers (prefab save, the asset
actions) currently expect the records to be updated when `scan()` returns, so that is
left for a separate change.

## Validation

`dve_editor_asset_browser_tests` adds:

- incremental rescans: unchanged files are not read, the index is not rewritten, hashes
  are reused by a fresh database (as after a restart), dependencies survive without
  re-reading text, a same-size edit with a new time stamp is detected and bumps the
  generation, a file modified just before the index write is re-hashed, and a touched
  but unchanged file keeps its generation;
- budgeted thumbnails: a scan without thumbnails writes none, a 1 us budget finishes
  over several calls and writes every thumbnail exactly once, and the default scan still
  writes them.

Full build (Release, SDL3 unavailable): 248 of 249 CTest tests passed. `dve_cpack_test`
fails because `dve_desktop_editor` is not built without SDL3, as on main here.
