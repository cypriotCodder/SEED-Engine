# Validation record

Validation is intentionally limited to this Mac at the user's request. No Docker image was created, and no validation packages were installed system-wide. The prepared container recipe and cross-compilation script have not been run.

## Observed results

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

The size is a measurement of this demo, not a content-budget guarantee for a future game. No frame-rate benchmark across target hardware has been performed.

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

The demo's 256-body building set remains resident; terrain streams, but arbitrary building-body migration between chunks does not yet exist. The construction control places loose blocks, not a full snapping/stacking system. Save replacement is atomic per file, not a whole-world transaction. These are explicit limits of the current delivery, not hidden stubs.
