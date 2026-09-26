# Seed Engine

A C++20 engine for top-down, procedurally generated 2D games: chunk streaming, destructible building physics, generated materials, normal-map lighting and crash-safe checkpoints. `seed_engine` (under `src/`) is the reusable library. `seed_demo` (under `games/demo/`) is one game built on it: an explorable island with a damageable timber platform, digging and building.

The local macOS build and OpenGL demonstrations have been exercised. Windows/Linux validation is tracked separately in [the validation record](docs/validation.md). Do not treat implementation coverage as platform release sign-off.

## Build and run

Use CMake 3.24+ and a C++20 compiler. GCC/Clang compile project code with `-Wall -Wextra -Wpedantic -Werror`; MSVC uses `/W4 /WX`. SDL2 2.30.11 and LZ4 1.10.0 are fetched with pinned SHA-256 checksums. On Linux, the SDL video backend also needs X11/Wayland development libraries and an OpenGL driver. Rendering requires OpenGL 4.1 and BC3/S3TC texture support.

```sh
cmake --preset release
cmake --build --preset release --parallel
ctest --test-dir build/release -C Release --output-on-failure
./build/release/games/demo/seed_demo
```

With a multi-configuration Windows generator, run `build/release/games/demo/Release/seed_demo.exe`. Keep `demo.pak` beside the executable. `SEED_FETCH_SDL=OFF` selects an installed SDL2 CMake package.

This workspace also contains a project-local CMake installation and downloaded dependencies. `sh tools/build-local.sh release` uses them without a system-wide installation.

Code style is defined by `.clang-format`. `sh tools/format.sh` reformats project sources; `sh tools/format.sh --check` reports unformatted files without editing them.

## Making a new game

A game is an executable that links `seed_engine` and hands `seed::run` a `seed::Game`: a struct of callbacks plus a context pointer. At minimum it provides:

- `id` (stable; recorded in saves), `name` and `default_save`
- `materials`: registers the game's materials in a fixed order
- `world`: a `WorldGenerator` with a stable name, a version and a `terrain` callback that fills a chunk's tiles (plus `structures` and edit rules if the game has them)
- `setup`: creates the game's entities and returns the one the camera and streaming follow

It then adds the callbacks it needs: `step` for movement, `act` for pointer input, `render` to draw, and `shutdown` for end-of-run checks. Entities made with `Engine::create_saved` are stored in the file of the chunk they stand in; the engine keeps their transform and visual, and the game's `save_entity`/`load_entity` pair writes and reads its other components. [tests/minimal_game.cpp](tests/minimal_game.cpp) is a complete game in about 150 lines, and `games/demo/` is a fuller one. [Architecture](docs/architecture.md) describes the engine/game boundary and lists what the engine still assumes about a game.

Every game gets the editor: F1 opens Dear ImGui panels over the running game showing frame time, draw calls, streaming, physics, entity and save state. A game adds its own panels through `Game::editor`. Input the editor is using (a click on a panel, typing in a field) does not reach the game.

Add a game as a subdirectory with its own `CMakeLists.txt`, as `games/demo/` does. `-DSEED_BUILD_DEMO=OFF` builds the engine and its tests without the demo.

## Demo controls

| Control | Action |
| --- | --- |
| W/A/S/D | Walk; water, trees, and building pieces block movement |
| Left mouse, held | Damage a structure piece or remove a tree within four world units |
| Right mouse, held | Excavate a tile; digging under a support can destroy it |
| B | Place a timber block at the pointer within reach |
| Tab | Toggle the wider island view |
| F5 | Commit a checkpoint now |
| Escape / window close | Commit a checkpoint and quit |
| F12 | Write a frame when `--screenshot FILE.ppm` was supplied |
| F1 | Open or close the editor (`--editor` starts with it open) |

The four stone piers support the generated timber platform. Removing all supports makes the remaining pieces fall to ground level. Ground-plane contacts use a spatial hash and oriented-box SAT; vertical support/gravity are a separate top-down model. New blocks are loose ground-level bodies, not an editor or a complete construction game.

```sh
./build/release/games/demo/seed_demo --seed 12345 --save saves/another-island
./build/release/games/demo/seed_demo --smoke --damage-demo --verify-stream \
  --save build/check-save --screenshot build/check.ppm
```

The second command edits a tile, streams it out and back, checks the saved delta, removes the platform supports, simulates sixty frames, verifies pieces reached the ground, captures a frame, and saves. Use a separate save directory because these checks deliberately change the world. `--overview` starts with the wider view. A mismatched seed/generator or corrupt file produces an explicit error.

Smoke runs skip hardware audio and driver vsync. The macOS development build uses explicit frame pacing because an SDL Cocoa display-link wait stalled during validation when display timing stopped progressing.

## Measurement and persistence

Interactive play starts background checkpoints every 60 seconds or on F5, and completes a final checkpoint on exit. Terrain, chunk-owned buildings, and the player commit together. An exclusive writer lock prevents simultaneous saves; an interrupted commit leaves the prior checkpoint loadable. The previous complete checkpoint is retained for corruption recovery. Legacy flat saves and incompatible generator versions are rejected; use a new save directory. See [the save contract](docs/save-format.md) for recovery and migration details.

Repeatable benchmarks disable audio, frame pacing, and player input, discard 60 warmup frames, and require a fresh save directory:

```sh
./build/release/games/demo/seed_demo --benchmark build/static.json --frames 600 \
  --workload static --save build/benchmark-fresh-save
python3 tools/benchmark.py build/release/games/demo/seed_demo build/benchmark-new-run
```

The second command records three static and three streaming runs, with separate saves and a summary. Its output directory must not already exist. Reports include CPU/GPU timing percentiles, physics worker time, chunk-generation totals, resident body counts, process peak memory, checkpoint duration, and save size. See [measurement definitions and the Mac baseline](docs/measurement.md).

## Assets and distribution

The cooker emits one checksummed LZ4 archive. Artist-authored inputs are uncompressed 24/32-bit TGA files with dimensions divisible by four, at most 4096×4096. It converts them to BC3 before packing. Input image stems are asset names.

```sh
./build/release/seed_cook output.pak art/flame.tga art/sign.tga
cmake --install build/release --prefix build/distributable
```

The demo cooker supplies a tiny flame sample automatically; its archive is 437 bytes. Terrain, foliage, the material atlas, normal maps, and impact audio are generated in memory. Source images and dependency sources are not installed. The local macOS release executable measured about 1.8 MB; this is an engine/demo measurement, not a guarantee about a future game's content size.

## Architecture and current bounds

[Architecture](docs/architecture.md) covers data ownership, streaming, threading, and fixed memory budgets. [Save format](docs/save-format.md) specifies byte order, versioning, and records. [Validation](docs/validation.md) records checks and outstanding gates.

The current budgets are 49 resident terrain chunks, 8,192 ECS entities, a physics pool of 4,096 bodies and 16,384 joints, 256 body slots per chunk, 512 cosmetic particles, 32 visible lights, and 32,768 sprites per batch. Batches flush when full. Buildings stream with their owner chunks and retain that ownership while moving nearby; construction currently places loose blocks. The demo island is a small workload, so its benchmark is a baseline rather than a full game performance target.

No networking, editor, 3D renderer, or third-party physics engine is included. Files under `sources/` remain read-only.
