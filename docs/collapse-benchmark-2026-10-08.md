# GameWorld collapse benchmark

`dve_collapse_bench` times GameWorld destruction end to end: carving, finding loose pieces,
splitting, building bodies and stepping physics. It runs every scene twice, once with
`damage_sphere` (finishes inside the call) and once with `queue_damage_sphere` (spread over
ticks by the logical unit budget, default 4096 units per tick). It uses
`ReferenceRigidBodyWorld` and fixed hit lists, so every count in the output is the same on
every run; only the timings vary.

```
dve_collapse_bench                      full scenes
dve_collapse_bench --quick              smaller scenes, used in CI
dve_collapse_bench --out result.json    also write the JSON to a file
```

## Scenes

| Scene | What it does |
|---|---|
| pockmark | 60 small hits, one per tick, on a static 64×32×8 wall that never splits. One body rebuild per hit. |
| bridge | A 128×4×4 span cut in six places over 30 ticks. Six pieces fall. |
| tower | A 32-wide tower on a 4×4 neck. One hit cuts the neck; the top (about 41,000 voxels) falls onto the base and settles. |
| spray | Twelve hits on one 64×64×8 wall in the same tick, five bursts ten ticks apart. |

`--quick` halves the sizes and hit counts.

## Output

One JSON object with a `results` array, one entry per scene and mode:

- counts: `voxelsAtStart`, `hits`, `ticks`, `ticksToDrain` (ticks from the last hit until the
  queue is empty, which is the visible delay on the queued path), `damageEvents`,
  `fragments`, `removedVoxels`, `rejected` (queued requests refused), `objectsAtEnd`,
  `voxelsAtEnd`
- timings in milliseconds: `totalMs`, `damageCallMaxMs` (longest single `damage_sphere`
  call), and per-tick `tickP50Ms`, `tickP95Ms`, `tickP99Ms`, `tickMaxMs`. On the sync path,
  per-tick times include the `damage_sphere` calls made that tick.

## CI

The `ctest (linux-gcc-release)` job runs `dve_collapse_bench --quick` after the test suite and
uploads `collapse-bench.json` as the `collapse-bench` artifact. The step has
`continue-on-error: true`: it reports and never fails the build.

## Baseline (main @ 8c4f3f4, 2026-10-08)

Release build, GCC, 2-core cloud VM, full scenes. Counts were identical across two runs;
timings varied by a few percent.

| Scene | Mode | Hits | Delay (ticks) | Rejected | Total ms | Longest call ms | Tick p95 ms |
|---|---|---|---|---|---|---|---|
| pockmark | sync | 60 | 1 | 0 | 23.8 | 0.56 | 0.47 |
| pockmark | queued | 60 | 425 | 0 | 595 | – | 3.60 |
| bridge | sync | 6 | 1 | 0 | 0.6 | 0.12 | 0.00 |
| bridge | queued | 6 | 1 | 0 | 3.4 | – | 0.00 |
| tower | sync | 1 | 1 | 0 | 4.1 | 4.03 | 0.00 |
| tower | queued | 1 | 45 | 0 | 69.5 | – | 1.82 |
| spray | sync | 60 | 1 | 0 | 44.1 | 0.89 | 8.25 |
| spray | queued | 60 | 928 | 16 | 1255 | – | 5.67 |

What it shows: on these sizes the queued path costs about 6 to 28 times more total CPU than
the synchronous path, and its visible delay grows with object size, because each queued hit
walks the whole object voxel by voxel. On the pockmark and spray walls, a whole synchronous
hit costs less than one tick of queued work. In the spray scene the queue backs up past its 64-request limit
and 16 hits are refused.
