# Validation record

Validation is intentionally limited to this Mac at the user's request. No Docker image was created, and no validation packages were installed system-wide. The prepared container recipe and cross-compilation script have not been run.

## Compatibility policy — 2026-09-25

Per the user's decision, legacy migration has been removed. Current checkpoint-container version 1 and chunk version 2 remain unchanged. Flat saves, old chunk schemas, standalone building files and incompatible generator versions are rejected, preserving original files. Earlier migration results below are historical.

The AddressSanitizer + UBSan checkpoint run stalled before `main`, recursively entering AddressSanitizer's initialization mutex from the macOS allocator/dyld path. A one-second process sample confirmed that stack; the process was stopped. This run is not an AddressSanitizer pass. Its sample is `build/review/checkpoint-sanitized.sample.txt`.

## Background checkpoint validation — 2026-09-25

The checkpoint suite now also freezes publication on a worker while working files change, checks rejection of overlapping publications, verifies asynchronous error propagation, exercises the raw-copy fallback, and confirms that returning a player to spawn overrides an earlier checkpoint position. Five CTest programs pass on the combined generator-version-2 tree. The Python benchmark regression handles an entirely absent GPU sample set and a long frame-count timeout.

The Mac graphics demo passed terrain/building stream-out and reload through the frozen checkpoint layers during background publication, then reopened with 14 collapsed pieces still grounded. The separate 1,000-file container workload and its capture/publication timings are recorded in [measurement.md](measurement.md). These checks do not establish power-loss or Windows/Linux guarantees.

## Measurement and checkpoint validation — 2026-09-24

Release and Debug builds use the strict warning settings. Four focused test executables cover core logic, storage, chunk/physics behavior and legacy record migration, and checkpoint recovery/statistics. Checkpoint tests exercise interruptions after file writes, manifest publication, recovery-pointer publication, and current-pointer publication. A child exits abruptly without destructors before publication, then the parent reopens the last complete save. Other checks cover writer exclusion, manifest/payload corruption, current-pointer corruption, bounded retention, unchanged original flat files, and unknown checkpoint versions.

Real Mac OpenGL runs imported and reloaded the previous compact standalone building save (15 grounded pieces). A combined run edited terrain, destroyed a plank, streamed terrain/buildings out and back, collapsed the platform, committed, and reopened with all 14 remaining fallen pieces grounded. Six final benchmark runs measured 600 frames each after 60 warmup frames; see [measurement definitions and results](measurement.md). F5 and the timed autosave use the same tested checkpoint operation; physical keyboard activation and a power-loss scenario were not independently exercised.

## Earlier observed results

The following records describe the earlier implementation, before chunk-owned building storage and checkpoint publication; their save sizes/formats are historical.

| Check | Result |
| --- | --- |
| Apple Clang 17, C++20, Debug and Release engine builds | Passed with `-Wall -Wextra -Wpedantic -Werror` |
| Core logic | Allocators, pool reuse, stale entities, negative coordinates, distant lattice hashing, noise seams/repeatability, AABB/SAT, binary encoding, CRC vector, BC3 block layout passed |
| AddressSanitizer + UndefinedBehaviorSanitizer on core logic | Passed |
| Storage logic | LZ4 round trip, replacement, corrupted payload rejection, unknown envelope-version rejection, archive loading, missing-asset rejection passed |
| SDL window/OpenGL context | Real OpenGL 4.1 context and frame presentation passed |
| Instancing/atlas | 10,000-sprite frame rendered and visually inspected |
| Terrain streaming | A tile edit survived generation → edit → unload → reload |
| Building collapse | Four removed supports left 15 unsupported pieces; all 15 reached ground height |
| Persistence | The damaged scene reloaded in the optimized build; version-1 body records migrated to compact version 2, then version 2 reloaded successfully |
| Lighting | Color/normal buffers, half-resolution light accumulation, and final composite rendered and were visually inspected |
| Integrated rendering | Five draw calls per captured frame |
| Release package | About 1.8 MiB on macOS, including a 437-byte compressed asset archive and dependency notices |
| Final packaged run | Passed after the macOS display-wait fix; both intact and damaged scenes rendered and exited |
| Save sizes | Untouched island: 40 bytes; tested damaged building: 514 bytes, plus a 46-byte terrain delta and 40-byte world metadata |

The size is a measurement of this demo, not a content-budget guarantee for a future game. The current local benchmark is documented above; no benchmark across Windows/Linux target hardware has been performed.

## Actual platform issues found

SDL2's macOS source emits Apple SDK deprecation warnings in a clean third-party build. They were not suppressed or represented as a warning-free dependency build. Project-owned C++ is warning-free under the strict settings above.

A later run stalled in `Cocoa_GL_SwapWindow`, waiting on SDL's display-link condition variable. A process sample identified that wait, with engine job workers idle. The macOS development path now disables driver vsync and uses explicit frame pacing. Smoke runs disable vsync and hardware audio. The final installed package passed three subsequent real-context runs, including compact-save migration/reload and an intact-scene capture. The stalled test process was terminated; no test processes were intentionally left running.

CoreAudio also reported a device-start failure on that later run; the engine logged audio as disabled. Procedural sound synthesis is implemented, but actual audible output has not been independently verified.

## Milestone status

All eight requested systems have implementations and a runnable integrated local demo. This is not a declaration that every release gate is complete. In particular, there is no Windows/Linux compile or runtime evidence, and third-party macOS deprecation warnings remain. The display-wait fix passed its final packaged runs.

| Milestone | Local evidence | Remaining gate |
| --- | --- | --- |
| 1. Scaffold/window/input | Build and real window run | Windows/Linux build and input/device checks |
| 2. Renderer/atlas/batching | Instanced image inspected | Target GPU/driver checks |
| 3. ECS/loop | Lifetime tests and runnable scene | Target compiler checks |
| 4. Generation/streaming | Noise tests and actual delta unload/reload | Cross-architecture determinism measurement |
| 5. Custom physics | Collision tests, support collapse, persisted bodies | Broader stability/performance checks |
| 6. Asset pipeline | Cooker, compressed archive, direct BC3 upload | Target packaging/texture support checks |
| 7. Lighting | Actual normal/light/composite frame inspected | Target framebuffer/driver checks |
| 8. Integrated demo | Final local package, intact and damaged-scene reloads | Target release validation |

## Scope bounds relevant to shipping

Buildings now stream with their owner chunks within bounded resident pools; ownership does not migrate when bodies move. The construction control places loose blocks, not a full snapping/stacking system. Saves publish a whole-world checkpoint with one previous checkpoint retained. Snapshot capture rotates the working directory; unchanged-file merging and full validation run on a worker. Dirty-state serialization remains synchronous, and startup/background costs still scale with save size. These are explicit limits of the current delivery.
