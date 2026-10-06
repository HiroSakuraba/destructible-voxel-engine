# Editor performance fixes

This change reduces repeated menu work, releases completed-task inputs, and makes
SDL desktop frame pacing interruptible by input. It also fixes a draw-list copy
in the interaction benchmark. Baseline: main commit `5cc00327c6f55d6e393f794129c6f02eda41e8e6`.

## Behavior and compatibility

- Menu IDs use an index rather than repeated linear scans. Menu ordering and
  normalized search fields are cached, and the most recent query retains ranked
  action indices. Only returned actions are copied. Enable-state changes update
  ranking, shortcut changes update search fields, and additions rebuild structure.
  Existing ordering, fuzzy scores, visibility and returned action metadata are preserved.
- Legacy mutable `find()` remains supported. Returning a mutable pointer disables
  cache reuse for that registry, since a caller can edit the pointer again later.
  Editor reads use const lookup; normal setters do not expose mutable pointers.
- Completed, failed and cancelled tasks release their work closures before completion
  becomes observable through `wait_idle()`. Closure destruction occurs outside the
  manager lock. Completed metadata history defaults to 256 entries; a third constructor
  argument configures the limit (minimum 1). Older terminal handles can be evicted;
  queued/running tasks are not evicted by this limit.
- Desktop pacing budgets 8 ms for the entire frame, instead of adding 8 ms after
  rendering/presentation. Remaining time uses `SDL_WaitEventTimeout` with a null
  event pointer, which wakes on input and leaves that event for normal polling.
  There is no extra wait when the frame has already exhausted its budget.
- The interaction benchmark binds the cached draw vector by reference, so cache
  access timing no longer includes a vector copy.

## Measurements

A single-threaded registry microbenchmark uses 32 warm-up calls and 1,000 measured
calls per case. Original and revised registry sources were both compiled with
GCC 13.3.0, C++23 and `-O2` on the same execution environment. Allocation counts
are ordinary `operator new` calls within each operation, including returned
result copies. The counter itself adds overhead to the timing.

| Operation | Allocations/call before | After | Median before (us) | After | p99 before (us) | After |
|---|---:|---:|---:|---:|---:|---:|
| Search, eight rotating queries | 1397.12 | 26.5 | 139.299 | 40.371 | 194.293 | 59.870 |
| Search, unchanged query | 1375 | 22 | 111.087 | 0.421 | 127.742 | 0.461 |
| Advanced Tools menu | 206 | 184 | 20.321 | 5.979 | 34.121 | 6.931 |

Changing-query search allocated approximately 98.1% fewer times. Search result
counts were unchanged (6,375 rotating-query results, 5,000 repeated-query results,
and 51,000 menu rows across 1,000 calls each). These are warmed registry timings,
not whole-editor frame rates or measured input-to-display latency. Cache construction
still has a first-use cost and retains normalized strings and index storage.

A separate lifetime probe captured an 8 MiB buffer in a task. The original manager
kept it alive after `wait_idle()`; the revised manager released it before that call
returned. This demonstrates captured-input release, not an RSS measurement.

The checked-in benchmark can be run with:

```sh
cmake --build build --target dve_editor_menu_bench dve_editor_interaction_bench
./build/dve_editor_menu_bench
./build/dve_editor_interaction_bench editor_interaction.json
```

To reproduce the before/after registry comparison from this checkout without
rebuilding the whole engine:

```sh
git show 5cc00327c6f55d6e393f794129c6f02eda41e8e6:src/editor_workspace.cpp > /tmp/dve_workspace_before.cpp
g++ -O2 -std=c++23 -ffunction-sections -fdata-sections -I include apps/editor_menu_bench.cpp /tmp/dve_workspace_before.cpp -Wl,--gc-sections -o /tmp/dve_menu_before
g++ -O2 -std=c++23 -ffunction-sections -fdata-sections -I include apps/editor_menu_bench.cpp src/editor_workspace.cpp -Wl,--gc-sections -o /tmp/dve_menu_after
/tmp/dve_menu_before
/tmp/dve_menu_after
```

## Validation

Eight focused test executables passed: editor, menu command center, settings,
shortcuts, render cache, SDL host, desktop editor contract, and AI assistant.
Regressions cover input release after success/failure/cancellation, bounded task
history, search limits and tie ordering, invalidation after state/shortcut/addition
changes, legacy pointer edits, and event waiting without consuming queued input.
The interaction benchmark also passed.

An additional comparison compiled the original and revised registry implementations
and compared output across default action-label queries, empty and fuzzy queries,
multiple limits, all default menus, state changes and direct pointer edits. Outputs
matched exactly.

The desktop executable was compiled and exercised against the repository's SDL
contract shim. Native SDL3/display hardware was unavailable here, so actual display
latency and native event-wakeup timing remain unmeasured. This was a focused test
run, not the entire engine test suite.

Scene revision counters, incremental diagnostic representation, and asynchronous
Vulkan submission are outside this change. In particular, scene objects and bricks
currently allow direct mutation; introducing revision counters requires covering
those mutation paths before replacing the existing correctness checks.
