# Measurement baseline

## Reproduce

Build Release, then run `python3 tools/benchmark.py build/release/seed_demo build/benchmark-new-run`. The destination must be new. Each workload gets three runs, a fresh save per run, 60 discarded warmup frames, and 600 measured frames. The script has a 60-second timeout for each process. Change `--runs` or `--frames` when needed. Direct demo benchmarking accepts `--benchmark REPORT.json --frames N --workload static|stream --save EMPTY_DIRECTORY` (up to 10,000 measured frames).

Both workloads use seed 20260923 and fixed 60 Hz simulation, with audio, input actions, driver vsync, and artificial frame pacing disabled. Escape/window close aborts the run; incomplete runs do not produce a successful report. Resizing during measurement is rejected. Native drawable resolution is recorded, so compare reports at the same resolution and view mode.

The static workload keeps the camera at spawn. Streaming follows chunk coordinates `(0,0) → (4,0) → (4,4) → (0,4) → (-4,4) → (-4,0) → (0,0)`, advancing every 60 measured frames and repeating. This deliberately crosses unload boundaries. It measures the existing island/ocean generator, not a densely populated world or a large construction stress scene. Chunk activation timing remains subject to normal worker scheduling.

## Definitions

All timing distributions use milliseconds and nearest-rank p50/p95/p99. Samples are preallocated and bounded. Empty distributions are `null`, not invented zero measurements. Raw distributions include count, mean, minimum, and maximum.

| Field | Scope |
| --- | --- |
| `frame_cpu` | Wall time from frame start through presentation; excludes title updates, sleep, checkpointing, and report output |
| `update_cpu` | Frame start through rendering start, including input, prior physics join/synchronization, streaming, and simulation submission |
| `stream_cpu_subset_of_update` | Main-thread streaming calls; includes activation/eviction, excludes asynchronous generation execution |
| `render_cpu` | Render submission and driver time from GPU query polling through the final pass |
| `present_cpu` | Buffer presentation, including any driver/compositor waiting |
| `render_gpu` | OpenGL elapsed query around geometry, lighting, and composite; excludes presentation |
| `previous_physics_step_worker` | Actual preceding worker simulation duration, read after its completion barrier |
| `physics_join_and_scene_sync_cpu_subset_of_update` | Main-thread wait for prior physics plus scene synchronization |

CPU and GPU work overlap: do not add their times together. Subset fields likewise are not additional frame costs. Eight GPU queries are polled without blocking; unavailable slots count as skipped samples. Remaining queries drain after the frame loop, outside frame measurement. Compare GPU sample count with measured frames.

Generation count/total/max cover the entire session, including startup and warmup, and exclude disk loading. Parallel generation totals are worker time, not elapsed startup time. Memory is the OS process-lifetime peak resident/working-set size measured after saving, not an isolated engine allocation total or dedicated GPU-memory estimate. Save size is the sum of file bytes in the save directory, including working and retained checkpoint copies, excluding filesystem allocation overhead. Checkpoint duration includes final physics completion, chunk/player serialization, manifest creation, flushes, and publication.

## Mac result — 2026-09-24

Release, Apple Clang 17, Apple M2 Pro, OpenGL 4.1 Metal, drawable 2560×1440, normal view. Values below are medians across three runs of each run's p95; they are not percentiles of pooled samples.

| Measurement | Static | Streaming |
| --- | ---: | ---: |
| CPU frame p95 | 3.467 ms | 3.667 ms |
| Across-run CPU frame p95 range | 3.392–3.505 ms | 3.375–3.994 ms |
| CPU render submission p95 | 0.468 ms | 0.512 ms |
| GPU rendering p95 | 1.169 ms | 1.188 ms |
| Physics worker p95 | 0.0245 ms | 0.0224 ms |
| Physics join + scene synchronization p95 | 0.000791 ms | 0.000917 ms |
| Generated chunks per session | 25 | 183 |
| Shutdown checkpoint, median | 1.517 ms | 1.930 ms |
| Save directory file bytes | 188 | 320 |

Peak process resident memory ranged from 108,314,624 to 109,772,800 bytes across all six runs. All runs collected 600 GPU samples with zero skips. The static scene had 19 resident bodies and five draws per frame; the streaming route visits empty ocean chunks and has fewer simulated bodies at times.

Raw reports and the summary are under `build/benchmarks/final-baseline/` in this workspace (build artifacts, not distributable assets). Earlier reports under `build/benchmarks/baseline/` predate the physics timing fields and must not be used to claim an optimization: repeated timing varies with system load. This establishes a local baseline; no Windows/Linux or target-hardware results are implied.
