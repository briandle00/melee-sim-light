# Performance baseline

Measured at `8cf34043` on 2026-09-21: AMD Ryzen 9 9950X3D, CPU 0 on the
96 MiB V-cache CCD, Linux x86-64, GCC 13.3.0, performance governor.
Use the root Makefile's strict native-release flags.

| Resident matches | Median match-frames/second |
|---|---:|
| 256 | 124,141.5 |
| 512 | 120,606.5 |

Each median uses eight runs. These are measured medians, not minimum throughput.

```sh
make benchmark-9950x3d-vcache-256 BENCHMARK_MATCH_FRAMES=262144
make benchmark-9950x3d-vcache-512 BENCHMARK_MATCH_FRAMES=262144
```

The workload is 508 human recordings with 4,684,452 input frames. Four CPU
recordings are excluded by the controller-only tape format. Each timed run uses
one single-threaded batch, 262,144 match-frames, eight warmup ticks, and a
128-frame observation/terminal history. Resident and logical batch sizes match.
Input tapes and initialization are outside the timed loop.

The recorded digests are `64020ea131e1b3a7` at 256 matches and `8ae6f774c68378ea`
at 512, with 15 and 19 resets respectively. Changing the corpus changes the
comparison workload and can change these digests.

Compare changes on the same host, compiler and workload. Keep measurements and
validation results in the commit description; put raw logs under ignored
`reports/triage/`. Historical experiments remain in Git history.

## PR review recheck (2026-10-12)

Alternating strict-release executables built in the same checkout with GCC 13.3,
CPU 0, performance governor, 256 resident matches, 524,288 timed match-frames,
eight warmup ticks and the same 510-recording tape manifest. Four runs per version:

| Version | Mean match-frames/second | Range |
| --- | ---: | ---: |
| Pre-review main `45bae153` | 121,400 | 119,304–122,305 |
| Main after #77/#68, `d395cc72` | 121,469 | 120,675–122,204 |
| Initial narrowed #67 candidate | 118,529 | 117,938–118,936 |

The initial narrowed #67 candidate was 2.42% slower than current main on this workload;
merged main shows no cumulative slowdown against the pre-review baseline.
Main and pre-review output digests match (`c6e5d56628c52a37`); #67's intended
RNG changes produce `38a4d0d243799a3c`. This measures end-to-end throughput,
not isolated instruction overhead: RNG-driven gameplay can change the workload.

Two main/candidate runs each in main/candidate/candidate/main order on subsets:
Dream Land (83 tapes) averages 150,394 → 136,812 FPS (−9.03%); the other stages
(427 tapes) average 123,546 → 121,572 FPS (−1.60%). These results supersede the
prior two-run conclusion that #67 had no noticeable cost. The repair below
addresses the regression. Raw results and preserved candidate diff are under
`reports/triage/pr67_perf_recheck/`.

### Particle animation repair

Disabling the new RNG callback left Dream Land's slowdown intact while restoring
main's output digest. Profiling found 317,611 object-1 animation selections in
524,288 frames: loading visual tracks, then discarding them, dominated the added
cost. The replacement loads only source particle tracks and their AObj clocks;
it keeps source selection, event order and interpolation without visual animation
construction. No replay toggle or persistent match state was added.

Final main/original/optimized/optimized/original/main comparison on CPU 4 (same
V-cache CCD), using the same mixed-stage workload and two runs per version:

| Version | Mean FPS | Runs |
| --- | ---: | --- |
| Main `d395cc72` | 121,664 | 121,098 / 122,230 |
| Previous narrowed #67 | 119,361 | 119,906 / 118,815 |
| Particle-only animation loader | 121,624 | 121,182 / 122,065 |

The final difference from main is −0.03%; the earlier candidate was −1.89% in
this comparison. CPU 0's three-run recheck was noisier: main averaged 121,754,
optimized 120,849 (−0.74%), with medians 121,986 and 122,930 respectively. The
optimized candidate's low run coincided with 5.31% sibling-thread activity.
These bounded checks no longer show a repeatable regression; they do not prove
zero instruction overhead. Original/optimized free-running digests match on both
the mixed workload (`38a4d0d243799a3c`) and Dream Land (`ae7d8192c3e420e4`).

Strict-release validation passes all 517 recordings / 4,768,869 frames and their
output locks. Source/native smoke and native/Wasm parity pass. Raw diagnostic,
profiling and benchmark results are under `reports/triage/pr67_cost/`.

## Static-pose blend review (#58)

Against main `24784d88`, CPU 4, batch 256, 524,288 timed match-frames, two
strict-release runs per version in main/candidate/candidate/main order:

| Workload | Main FPS | Corrected blend FPS | Change |
| --- | ---: | ---: | ---: |
| Same 510 human tapes | 121,881 | 123,545 | +1.37% |
| Tilted-shield probe | 162,200 | 157,491 | −2.90% |

The mixed result is evidence of no observed slowdown, not a claimed speedup.
The shield probe uses eight FD dittos (Fox, Falco, Marth, Sheik, Dr. Mario,
Falcon, Link, Samus), 20 tilted-shield frames followed by 60 neutral frames;
a debugger confirmed it reaches `ftAnim_80070108` with partial blend weights.
Its higher shielding frequency exposes a repeatable cost hidden by the mixed
workload. Both workloads retain identical main/candidate output digests.
The fix preserves the existing batched arithmetic and adds no allocations or
state; it blends eligible unanimated joints that were incorrectly skipped.

Validation: 518/518 recordings, 4,782,367 frames; all 517 prior output locks
unchanged, one new lock. Source/native smoke, Wasm parity and 40 affected tooling
tests pass. Raw results and probe generation are in `reports/triage/pr58_review/`.

### Presentation-only joint exclusion

Profiling put 4.81% of the shield probe's instrumented time in
`ftAnim_80070108`, including 3.74% in its blend kernel. Removing the animation
shortcut also admitted the shared `JOBJ_MSL_GAMEPLAY_COLD` placeholder that
`ftparts.c` uses for presentation-only bones: its part flags are clear, so it
passes `msl_fighter_part_can_animate`. The retained fix checks that existing
marker, while still blending every eligible unanimated gameplay joint.

Final paired main/candidate/candidate/main runs with the same settings above:

| Workload | Main FPS (runs) | Final FPS (runs) | Change |
| --- | ---: | ---: | ---: |
| Tilted shield | 165,807 (165,769 / 165,844) | 164,672 (164,758 / 164,585) | −0.68% |
| Same 510 human tapes | 124,608 (123,704 / 125,511) | 123,693 (123,754 / 123,631) | −0.73% |

This removes most of the original 2.9% shield-heavy penalty, not literally all
measured cost. Both workload digests are unchanged. All 518 replay locks and
4,782,367 compared frames pass; source/native smoke and Wasm parity pass.
There is no new state, allocation, cache, or arithmetic change.

Rejected alternatives: compact quaternion worklists were 1.43% slower than the
unoptimized PR; shared vector math and independent axis comparisons recovered
less than 1% together. Both were reverted in favor of the narrower owner fix.
Raw profiles, experiments and final measurements: `reports/triage/pr58_perf/`.

## Memory

Measured at `24ee1e58` on 2026-09-24 (16-core Linux host, private resident bytes
from `/proc/<pid>/smaps_rollup`). Immutable game data is loaded once per process
and shared by every batch: about 300 MB (GameData arena ~103 MiB, native DAT
arena ~108 MiB, compiled sample values ~97 MiB). Processes forked after
`msl_game_data_acquire` share those pages copy-on-write and dirty only a few MB
while stepping, so a forked worker holding a batch costs about 10 MB plus its
environments.

Each environment's Match arena is zeroed at construction and does not grow with
play. Singles matches on Final Destination:

| Matchup | Arena per environment |
|---|---:|
| Fox, Falco, Falcon, Jigglypuff, Yoshi, Luigi pairs | 946–954 KiB |
| Marth vs Sheik | 1,079 KiB |
| Ice Climbers vs Ice Climbers | 1,106 KiB |
| Peach vs Peach | 1,715 KiB |
| Peach vs Ice Climbers | 1,760 KiB |

Item reserves (`msl_item_reserve_runtime_pools`: the ItCo.dat article bound,
ItemLink and DynamicBoneTable pools) are 47–59% of that arena. A host running
`E` environments over `P` forked workers needs roughly
`300 MB + 10 MB × P + 1.0–1.7 MB × E` for the simulator.

## Frozen FD replay capability

PR #43 against main `8e4fa0a7`, Linux amd64 GCC 13.3, CPU 4 on the V-cache
CCD, performance governor. Two strict-release runs per cell; 256 resident
matches, 524,288 timed match-frames, eight warmup ticks, history 128.
The fixed 510-tape corpus is the same as the preceding PR checks. Candidate
copies insert only the new zero capability byte and update the tape header;
all input bytes are identical. The FD subset contains 139 tapes.

| Workload | Main FPS | Candidate FPS | Change |
| --- | ---: | ---: | ---: |
| Mixed, patch off | 122,594 | 122,023 | −0.47% |
| FD only, patch off | 140,776.5 | 140,211.5 | −0.40% |
| FD only, patch on | 140,776.5 | 141,161.5 | +0.27% |

These short samples show no material slowdown; their spread exceeds the
mean differences. The enabled case changes the simulated rules and is not
an equivalent-computation speedup. Default digests match main (`38a4d0d243799a3c`
mixed, `04e4f4f92b207d08` FD). No additional allocations or tables; the FD
callback adds one capability check. Public API constructors keep the bit clear.

The supplied FD Marth/Icies replay improves from 12,798/12,846 to exact.
All 519 replays (4,795,213 frames) pass, preserving all 518 previous locks.
Native checks, Wasm parity and viewer schema pass. The existing Python selection
passes all 151 tests, with its usual timing-test exclusion; no new synthetic
gameplay tests.
Raw logs: `reports/triage/pr43_review/`.
