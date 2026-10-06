# Asset scans on a worker thread

Follow-up to the incremental asset scan (`docs/editor-asset-scan-2026-10-06.md`), which left
"move the whole scan to a worker" for a separate change. Baseline: main commit `7785b0b`.

## What changed

- Opening the Assets panel, its Refresh button, Ctrl+R and the `asset.refresh` action now
  call `NativeEditorController::start_asset_scan()`. It copies the asset database and scans
  the copy on a dedicated worker (`EditorTaskManager`, one thread). The panel keeps showing
  and querying the current records meanwhile and its summary line reads "(scanning...)".
- `update()` applies a finished scan by moving the copy in, then does what a refresh did
  before: queues the thumbnail backlog, clears a selection that no longer exists, and
  announces "Assets indexed: ..." when asked. The scroll position is kept unless the list
  got shorter than it.
- A refresh requested while a scan runs is queued and starts once the result is applied, so
  files changed mid-scan are not missed.
- Nothing edits the database alongside a scan, so the two never write the index file at the
  same time: moving or renaming an asset, regenerating a thumbnail, changing the project root
  and the synchronous scan after a prefab save first call `finish_asset_scan()`, which waits
  for the worker and applies its result. The per-frame thumbnail backlog pauses while a scan
  runs. If the database still changed underneath a scan, its result is dropped and the
  current state is scanned again.
- The mutable `asset_database()` accessor also finishes a running scan first, so callers that
  inspect or edit the database right after a refresh (tests, tools) see its result; the const
  accessor never waits. Read-only lookups inside the controller now use the const `find()`,
  which does not advance the database revision.
- The prefab save still scans synchronously, because it selects the new asset immediately.

## Measurements

20,000 files of 1 KB, warmed file cache, Release `-O2`, editor frames every 8 ms.

| | Before (scan on the UI thread) | This change |
|---|---:|---:|
| Editor blocked by a refresh | 640-700 ms | 8-12 ms to copy the database, then none |
| Slowest frame while scanning | (frozen) | 12-17 ms (the frame that applies the result) |
| Time until the new records show | 640-700 ms | 710-860 ms |

## Validation

- `dve_editor_asset_browser_tests` adds a 3,000-file background scan: `asset.refresh` returns
  at once with a scan in flight, the editor keeps updating (about 400 frames during the scan,
  the slowest under 4 ms), the result is applied with all 3,000 records and announced, a
  second refresh asked for mid-scan runs after the first is applied, and mutable access
  waits for a running scan and sees a file added just before it.
- Under ThreadSanitizer the asset browser tests report no races (3 runs).

Full build (Release, SDL3 unavailable): 251 of 252 CTest tests passed. `dve_cpack_test`
fails because `dve_desktop_editor` is not built without SDL3, as on main here.
