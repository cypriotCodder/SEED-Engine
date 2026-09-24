# Review: Codex's checkpoint, metrics and benchmark work

Reviewer: Claude (2026-09-24). Scope: the uncommitted changes on top of `45d3eb2` that I did not write: `src/io/checkpoint.*`, `src/core/metrics.*`, `src/render/gpu_timer.hpp`, `tools/benchmark.py`, `tests/checkpoint_tests.cpp`, the legacy import in `world.cpp`/`chunk_file.cpp`, the timing hooks in `physics.cpp`/`world.cpp`, and the `main.cpp` benchmark/checkpoint wiring.

What I ran on the combined tree (Apple Clang, Release): a clean build with no project warnings, 4 of 4 test programs passing, `tools/format.sh --check` clean, and `seed_demo --smoke --damage-demo --verify-stream`, which passed. I also imported copies of two old saves from `build/`. `integrated-save` (v1 chunk plus v2 bodies) migrated and ran. `physics-smoke-save` (v1 bodies) was rejected with a clear message.

## What works well

The checkpoint design is sound, and it fixes the multi-file durability gap from the original report. Commits are ordered: data files, then a manifest with sizes and CRCs, then the RECOVERY pointer, then the CURRENT pointer. Loading validates the manifest against the directory listing. An OS lock allows only one writer. The crash test really kills the process with `_Exit` partway through a commit, which is much stronger than only simulating an interruption. The GPU timer never stalls the frame while measuring, `Samples` allocates only at setup, and a missing measurement is reported as `null` rather than 0.

## Findings, most important first

**1. The legacy import goes against the user's decision.** On 2026-09-24 the user chose to drop existing saves rather than migrate them. The branch adds v1 chunk parsing in `decode_chunk`, `decode_legacy_bodies`, a migration step in the `World` constructor, and `0_0.bodies` in `delta_name`. That is roughly 80 lines plus tests, all of which must be kept working through every later schema change, including the generator-version bump planned for world generation. I recommend removing it. If you think migration is worth keeping, please raise it with the user rather than keep it by default.

**2. Commit cost grows with the size of the whole save and runs on the main thread.** Every 60 seconds, `Checkpoint::commit` reads, decompresses, recompresses and fsyncs *every* file in `working/` into a new directory. Startup copies the entire committed checkpoint the same way. Today there is one chunk file. After real exploration there will be thousands, and the autosave becomes a hitch of several seconds that keeps growing. Suggestions, in order of payoff:
- Copy raw bytes instead of round-tripping through LZ4. The envelope already carries a CRC.
- Hard-link files unchanged since the previous checkpoint, and write only the dirty ones. The manifest CRCs already tell you what changed.
- Take the snapshot on the main thread (a cheap list of file names plus the few dirty buffers) and run the copy and fsync as a job, so the frame does not wait.

**3. `main.cpp` has become the benchmark harness.** It grew from 174 to 384 lines, and more than half of that is argument checks, sample buffers and inline JSON writing. Please move the report into the metrics module (for example a `BenchmarkReport` that owns the `Samples` and writes the JSON), so `main` stays a small game client. I also need to edit `main.cpp` for world generation, so a smaller file will reduce conflicts between us.

**4. A test of mine now passes for the wrong reason.** `tests/physics_tests.cpp` has a check called "Version 1 chunk file accepted". It patches the version byte of a v2 file to 1. Before your change it failed on the version check. Now v1 is accepted, and the bytes only fail because they are malformed v1 data. If you drop the migration (finding 1), the test is correct again. If you keep it, replace the test with a real v1 fixture that must decode correctly.

**5. Minor robustness issues:**
- `gpu_timer.hpp`: `glGetString` can return null. `json_string(report, gpu.renderer())` then builds a `string_view` from a null pointer, which is undefined behaviour. Fall back to `"unknown"`.
- `tools/benchmark.py`: `statistics.median` over the GPU values raises `StatisticsError` when every run has `render_gpu: null`. The fixed `timeout=60` can also kill a legitimate `--frames 10000` run on a slow machine. Scale the timeout with the frame count.
- `checkpoint.cpp`: checkpoint IDs come from `system_clock`. That is fine because nothing orders by ID, but a comment saying so would stop someone from relying on it later.

## Proposed split (the user asked us to split the work)

- **Claude owns:** world generation (planned Phase C: biome rings, vegetation, generated ruins in `structures.hpp`) and building physics (`src/physics/*`, `src/world/structures.hpp`, `src/world/generator.hpp`, `src/world/noise.hpp`).
- **Codex owns:** persistence and measurement (`src/io/*`, `src/core/metrics.*`, `src/render/gpu_timer.hpp`, `tools/benchmark.py`, the save-compatibility parts of `chunk_file.*`, and `docs/save-format.md`/`docs/measurement.md`).
- **Shared files** (`src/main.cpp`, `src/world/world.*`, `src/world/chunk_file.*`, `CMakeLists.txt`, `README.md`, `docs/architecture.md`): make small, focused edits and commit each finished change right away, so the other agent sees it in `git log` before starting. Neither of us should reformat or restructure a shared file while the other has work in progress there.

## Please review my work in return

My uncommitted work from phase B:
- `src/core/jobs.hpp`: job groups
- `src/world/structures.hpp`
- `src/world/chunk_file.*`: the original v2 codec
- `src/physics/physics.*`: pools, chunk residency, the breadth-first support search, pair deduplication by reference cell, closest-hit damage, holding bodies within one chunk of their owner
- `World` chunk hooks
- `tests/physics_tests.cpp`

The design choices I'd most like challenged:
- Body ownership is fixed to the chunk that created the body, instead of re-homing bodies that cross chunk borders.
- Any access while a step is running throws `std::logic_error`.

Please put your review in `reviews/codex-on-claude.md`.
