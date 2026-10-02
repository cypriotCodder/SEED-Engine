# Seed Engine

A C++20 engine for top-down, procedurally generated 2D games: chunk streaming, destructible building physics, generated materials, normal-map lighting and crash-safe checkpoints. `seed_engine` (under `src/`) is the reusable library. `seed_demo` (under `games/demo/`) is one game built on it: an explorable island with a damageable timber platform, digging and building.

The local macOS build and OpenGL demonstrations have been exercised. Windows/Linux validation is tracked separately in [the validation record](docs/validation.md). Do not treat implementation coverage as platform release sign-off.

## Build and run

Use CMake 3.24+ and a C++20 compiler. GCC/Clang compile project code with `-Wall -Wextra -Wpedantic -Werror`; MSVC uses `/W4 /WX`. SDL2 2.30.11, LZ4 1.10.0, Lua 5.4.9 and Dear ImGui 1.92.9b (docking release, used only by the editor) are fetched with pinned SHA-256 checksums. Lua is compiled as C++ so script errors unwind engine code safely; games that never use scripts do not link it. On Linux, the SDL video backend also needs X11/Wayland development libraries and an OpenGL driver. Rendering requires OpenGL 4.1 and BC3/S3TC texture support.

```sh
cmake --preset release
cmake --build --preset release --parallel
ctest --test-dir build/release -C Release --output-on-failure
./build/release/games/demo/seed_demo
```

With a multi-configuration Windows generator, run `build/release/games/demo/Release/seed_demo.exe`. Keep `demo.pak` beside the executable. `SEED_FETCH_SDL=OFF` selects an installed SDL2 CMake package.

This workspace also contains a project-local CMake installation and downloaded dependencies. `sh tools/build-local.sh release` uses them without a system-wide installation.

Code style is defined by `.clang-format`. `sh tools/format.sh` reformats project sources; `sh tools/format.sh --check` reports unformatted files without editing them.

## Seed Editor

`seed_editor` (under `editor/`) is the application for building games, separate from the games themselves; games never link it or Dear ImGui. It opens on a hub where you create a project or open a recent one. The open project shows in a dockable workspace:

- **Scene**, **Hierarchy** and **Inspector**: the Scene view draws the scene with the engine's own renderer (optionally with the game's lighting). Click to select; Shift+click adds, and dragging on empty ground selects with a box. Drag a selection to move it, or use the handles: **Move** (W) with per-axis arrows, **Rotate** (E) with a ring that turns a group about its centre, and **Scale** (R). Snapping to 1/4, 1/2 or whole tiles and 15° steps is in the toolbar; Shift flips it while dragging. Cmd+C/Cmd+X/Cmd+V copy, cut and paste entities as JSON, so they move between scenes, projects and editor windows; Cmd+D duplicates, Cmd+A selects all, Delete deletes, F frames. Right-drag or middle-drag pans, scrolling zooms. With several entities selected, the Inspector applies each change to all of them. The Hierarchy searches by name, renames in place (Return or F2), reorders by dragging rows (later entities draw on top), and has Show and Lock columns: hidden entities are left out of the Scene view and locked ones cannot be picked there; games ignore both.
- **Project**, **Console** and **Project Settings** panels. Project Settings also sets the game's start scene, window title, window size and fullscreen, used by Play and exported apps.
- **Characters**: a Character component makes any entity walk with collisions (speed, run speed, acceleration, collision size, what blocks it, facing). One character is the player: the keys move it and its settings choose the actions, camera zoom, smoothing and dead zone, and whether saved games resume where it was. The rest are NPCs, walked by their scripts. **Create → Player / NPC** in the Hierarchy or the Scene view's right-click menu adds them; the Scene view shows collision boxes and the player's camera frame.
- **Prefabs**: reusable entities in `prefabs/`. **New from Selection** makes one from an entity; drag it from the Prefabs panel into the Scene view (or double-click) to place copies. A placed copy has its own name and placement; its visual, light and script are the prefab's. In the Inspector, **Apply** makes a copy's changes the prefab's, updating every copy; **Revert** takes the prefab's back and **Unlink** keeps the copy as a plain entity. Scripts spawn prefabs with `world.spawn{prefab = "name", x = .., y = ..}`.
- **Terrain**: a project can have several terrains, and each scene picks one. The scene's terrain is the first row of the Hierarchy; selecting it shows its choice, New (blank, starter island or a copy), Rename and Delete in the Inspector, and the Terrain panel edits it: the world generator as data: island or endless world, noise fields, and ordered rules that pick each tile's material, with scattered trees and rocks. **Create Starter Island** fills in a complete biome-ring island. The Scene view previews the generated world live and zooms out far enough to show a whole island.
- **Atmosphere**: each scene's Atmosphere row in the Hierarchy sets its ambient light, background and haze, and an optional day that turns into night (day length, starting hour, night light), previewed with Lit at any hour. Lights can flicker like flames and can be lit only at night; scripts read and set the time with `atmosphere.hour()` and change lights with `e:set_light{}`.
- **Terrain painting**: the **Paint** tool (B) paints over the generated terrain with round or square brushes of any size and strength: ground materials, objects (or none, to clear them), raise, lower and flatten (below 0 is water), blocking, and erase back to generated. The brush outline follows the pointer, and height and blocking brushes mark water and blocked tiles nearby. Each stroke is one undo step. Paint is saved beside the scene and applied as games generate chunks, so it costs nothing per frame.
- **Materials**, **Input**, **Sounds** and **Particles** panels that edit the project's data assets, with validation as you type. A material can use an imported PNG, JPG or TGA texture, set how fast characters walk on it, and carry tags for scripts (`world.surface`, `on_surface`). The Inspector's Visual section changes an object's material and texture in place, makes new materials, and **Make Unique** copies a shared material so a change affects only the selection. Input bindings are recorded by pressing the key or button; sounds can be played while you tune them.
- **Scripts**: Lua 5.4 gameplay scripts attached to entities with the Inspector's Script component, checked for syntax as you work. See [scripting](docs/scripting.md).
- **Play** (Cmd+P) saves the project and runs it as a real game in its own window, with the game's output in the Console; **Stop** or closing the game window returns to editing. Every Play starts a fresh world, so the project's saves are untouched.
- **File → Export macOS App…** builds a standalone `<Name>.app`: the player, the project checked and packed into one compressed `game.seedpack`, an `Info.plist`, and the licences of the libraries a game contains. The executable is stripped and the app signed ad hoc so it runs on this Mac. Exported games save in `~/Library/Application Support/Seed/<game id>/`. Export refuses projects with problems, including script syntax errors and missing scripts.
- Cmd+Z and Shift+Cmd+Z undo and redo any edit in these panels (100 steps, per scene). Cmd+S saves; closing or quitting with unsaved changes asks first.

Projects are folders of JSON text files, described in [the project format](docs/project-format.md). `seed_player` runs a project with no game code: its assets and terrain, its main scene, and a player (the scene entity named `Player`, or one at a `Spawn` marker) moved with the `move_up`/`move_down`/`move_left`/`move_right` actions, which default to WASD and the arrow keys. `seed_player --project tests/sample_project` runs the sample project. A C++ game can still use a project's assets by setting `Game::project_assets`, as [tests/minimal_game.cpp](tests/minimal_game.cpp) does for materials.

```sh
./build/release/editor/seed_editor
```

Distributing to other Macs needs a Developer ID signature and notarization, which export does not do yet. `-DSEED_BUILD_EDITOR=OFF` skips building the editor.

## Making a new game

A game is an executable that links `seed_engine` and hands `seed::run` a `seed::Game`: a struct of callbacks plus a context pointer. At minimum it provides:

- `id` (stable; recorded in saves), `name` and `default_save`
- materials: a project assets folder (`project_assets`) or a `materials` callback that registers them in a fixed order
- `world`: a `WorldGenerator` with a stable name, a version and a `terrain` callback that fills a chunk's tiles (plus `structures` and edit rules if the game has them)
- `setup`: creates the game's entities and returns the one the camera and streaming follow

It then adds the callbacks it needs: `step` for movement, `act` for pointer input, `render` to draw, and `shutdown` for end-of-run checks. Entities made with `Engine::create_saved` are stored in the file of the chunk they stand in; the engine keeps their transform and visual, and the game's `save_entity`/`load_entity` pair writes and reads its other components. [tests/minimal_game.cpp](tests/minimal_game.cpp) is a complete game in about 150 lines, and `games/demo/` is a fuller one. [Architecture](docs/architecture.md) describes the engine/game boundary and lists what the engine still assumes about a game.

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
