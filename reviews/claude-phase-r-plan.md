# Phase R: split the reusable engine from the demo game (coordination note)

From: Claude, 2026-09-25. The user asked me to turn `seed_engine` into a reusable engine and move everything specific to the demo into a separate `seed_demo` game target. Behaviour must not change. Each step is one commit, and after each one I run the build, the tests and `seed_demo --smoke --damage-demo --verify-stream`.

**Before I start I need two things from you:**
1. Commit (or discard) your uncommitted work in `world.cpp`, `chunk_file.*`, `physics_tests.cpp`, `checkpoint.*` and the docs. Steps 1–3 move and edit those files, and I will not build on top of uncommitted changes that aren't mine.
2. While steps 1–3 are in progress, please don't edit `src/main.cpp`, `CMakeLists.txt`, `src/world/world.*`, `src/world/generator.hpp` or `src/world/chunk_file.*`. I'll post here when each step lands. I won't touch `src/io/*` or the metrics code at any point.

## Steps and the files each one touches

1. **Directory split.** `git mv src/main.cpp games/demo/main.cpp`. CMake builds `seed_engine` (library) and `seed_demo` (executable, links the engine). No logic changes. Your checkpoint and benchmark wiring moves with `main.cpp` unchanged. Files: `CMakeLists.txt`, `src/main.cpp` → `games/demo/main.cpp`.
2. **Pluggable world generation.** An engine-side `ChunkGenerator` struct of function pointers, in the same style as `Jobs::Job` and `ChunkHooks`: `{context, fill(context, seed, coord, Chunk&), id, version}`. `World` takes it at construction. `sample_terrain`, `Biome` and the tree rules move to `games/demo/`; noise stays in the engine. Files: `world.*`, `generator.hpp`, new `games/demo/terrain.*`, `tests/world_tests.cpp` (moves to demo tests).
3. **Pluggable structures.** The engine keeps `ChunkBodies`, the codec and the ownership rules; the platform recipe moves to the demo. `chunk_file` currently regenerates the baseline recipe with `generate_structures` directly. It will call the generator's `structures` function instead, so `encode_chunk` gets the baseline passed in (as now) and `World::write` gets it from the generator. Files: `structures.hpp`, `world.cpp`, `chunk_file.*` (signature only).
4. **Registered materials.** Renderer only (plus demo registration).
5. **Game components.** `core/ecs.hpp`, `core/scene.hpp` only.
6. **Save identity.** This one is yours to decide; see below. I'll stop before this step until we agree.
7. **Docs.** `README.md` and `docs/architecture.md`, the engine/game boundary sections only.

## Step 6 proposal: save identity (your area; please reply here)

Today `world.seed` holds `magic, version 1, generator_version, seed`, and every chunk file header repeats `generator_version`. Once games plug in their own generators, a version number alone can't tell two games apart. Proposal:

- `world.seed` version 2: `magic, 2, u64 game_id, u64 generator_id, u32 generator_version, u64 seed`. `game_id` and `generator_id` are 64-bit hashes of names the game declares (for example `"seed-demo"`, `"demo-disc-v"`). The engine rejects a save from a different game or generator with a clear error. Old saves are dropped, as the user decided.
- Chunk file header: replace `u32 generator_version` with `u64 generator_id, u32 generator_version`, so a stray chunk file copied between worlds is rejected on its own. This changes the chunk format to version 3.
- `player.delta` and the checkpoint manifest: I'd leave them unchanged, since they already bind to the seed and `world.seed` is inside every checkpoint. Tell me if your checkpoint validation needs anything more.

Questions for you: are you happy with hashed names rather than registered integer IDs? Should the checkpoint manifest record the game ID too, so recovery can refuse a checkpoint from another game before it opens any files?
