# Architecture

## Engine and game boundary

`seed_engine` (everything under `src/`) is a library for top-down, chunk-streamed 2D games. A game is a separate executable under `games/` that links the library and describes itself to `seed::run` with a `seed::Game`: a struct of function pointers plus a context pointer, in the same style as `Jobs::Job`. Nothing in `src/` includes anything from `games/`. Building with `-DSEED_BUILD_DEMO=OFF` proves it, and `tests/minimal_game.cpp` is a second, complete game that uses only the engine.

The engine owns the frame: startup order, the common command-line options, the fixed 60 Hz step with interpolation, running the last physics step on a worker while the frame renders, streaming chunks around the game's focus entity, the camera and pointer, screenshots, benchmark measurement, autosave and the final checkpoint. Each frame it calls the game's `frame`, then `step` once per fixed step (physics idle), then `zoom`, `act` and `render`. Physics may still be running during `act` and `render`, so `act` joins it before touching bodies.

The game owns everything that makes it that game:

- **World generation.** A `WorldGenerator` supplies a stable name and version, a `terrain` callback that fills a chunk's tiles, an optional `structures` callback that builds the chunk's physics recipe, and the meaning of tile edit bits (`edit_bits`, `apply_edit`). Callbacks run on worker threads and must be pure functions of seed and coordinate, because saves store only differences from what they produce. Noise helpers stay in the engine.
- **Tiles.** An engine `Tile` is 12 bytes: `elevation`, a registered `material`, engine `flags` (`tile_solid` blocks movement and building), and four bytes the game defines; the demo keeps its moisture there. `World::edit` records game-defined edit bits and applies the game's rule.
- **Materials.** The game registers every material (name, colour, pattern, optional packed texture) before the renderer or world generation start. IDs follow registration order and index the generated atlas.
- **Components.** Beyond the engine's `Transform` and `Visual`, a game registers its own trivially copyable component types with `Scene::add_component<T>()`. They are stored densely in the same up-front arena.
- **Building visuals.** `body_visual` and `body_lift_per_height` decide how physics bodies look; physics itself names no materials.

Saves are bound to a game: `world.seed` records the game ID, the generator ID and version, and the seed, and the engine refuses a save from anything else.

## World model

The view is top-down. X/Y describe the ground plane. Elevation and structural support are separate simulation quantities. A world position is a pair of signed 64-bit chunk coordinates plus a local float offset in [0,32). Camera-relative subtraction only operates within a bounded neighbourhood, avoiding conversion of enormous global coordinates to floats. Overflow is reported.

Gradient Perlin noise hashes integer lattice coordinates with a 64-bit seed. The demo's generator (`games/demo/terrain.hpp`) builds a finite disc world with biome rings from it. Strict floating-point contraction settings are used; cross-architecture bit identity still needs measured validation before extending the compatibility promise.

## Ownership and memory

`seed_engine` owns platform, rendering, streaming, persistence, and physics code, and drives their lifetime from `seed::run`. There are no inheritance hierarchies. `Scene` provides generational entity handles and dense sparse-set transform/visual components. Systems may retain handles, never component addresses across structural edits. Packed components are required to be trivially copyable.

A 2 MiB arena (configurable per game) backs entity/component storage for up to 8,192 entities. A fixed pool holds 49 terrain chunks. The particle pool contains 512 slots. Physics uses fixed pools for 4,096 bodies, 16,384 center-distance joints, and spatial-hash entries. Each chunk stores at most 256 bodies and 1,024 recipe joints. The renderer allocates its 32,768-instance CPU store once and orphans the GPU stream buffer on each flush. Ordinary frame iteration does not grow STL containers. Allocation is allowed during initialization, loading, serialization, and failure reporting.

These are explicit current limits, not estimates of unlimited capacity. Buildings enter and leave the physics pools with their owner chunks. The global pool bounds resident work; owner identity stays fixed while a body moves within its neighboring chunks.

## Update and job ownership

The loop accumulates time into 60 Hz simulation steps and clamps long frame gaps to 100 ms. Rendering interpolates previous/current transforms. Losing window focus releases held keys.

A bounded 128-entry job queue feeds up to four worker threads. Generation, chunk I/O, asset decompression, and physics execute as jobs. Workers publish completed chunks using release/acquire atomic state transitions. A slot is never reused while its worker owns it. OpenGL and scene mutation remain on the main thread. Separate job groups let physics join only its own work. Its last step runs while the frame renders; the next frame joins before reading bodies or changing chunk residency. Scene synchronization occurs on the main thread after that join.

## Chunk lifecycle and saves

Chunks progress through empty → generating → ready → active → saving → saved → empty. Generation and disk-delta loading happen together before publication. A five-by-five desired region is surrounded by retention hysteresis out to three chunks. Clean chunks unload without writing. Dirty chunks remain owned by their save job until replacement completes.

Only the world seed/version, modified tile records, changed body states/broken joints, and player position are stored. Files use explicit little-endian encoding, LZ4, lengths, and CRC32. Save replacement writes a temporary file, flushes it, and replaces the old file; POSIX also syncs the parent directory. These writes target a private working directory. A checkpoint freezes dirty files by rotating the working directory, then a worker merges unchanged files and writes a checksummed manifest before publishing one atomic pointer. The prior checkpoint remains available for whole-world recovery. An OS writer lock prevents concurrent sessions. Reads fall back through working and frozen checkpoint layers. Dirty-state serialization still runs at a main-thread barrier; checkpoint validation, linking/copying, synchronization and publication run in their own job group.

## Rendering

A padded runtime atlas supplies generated materials. A second generated atlas supplies tangent-space normals. The offline cooker demonstrates direct BC3 GPU upload using a tiny flame texture. Instancing emits six vertices per quad and preserves submission order; switching atlas flushes the batch.

The geometry pass writes full-resolution color and normals. Local lights add into a half-resolution RGBA16F target using bounded light quads. The final pass applies illumination, tone mapping, and a mild edge haze. Both normal maps and light locations change the actual shading. There is no geometric shadow map; tree shadows are cheap sprites.

## UI

After the lit scene is composited, the renderer draws a screen-space UI layer, unlit and in submission order: `ui_rect` and `text`, in logical pixels from the top-left, scaled for high-DPI displays. Text uses a 5×7 ASCII bitmap font stored as a 475-byte table in code and expanded into a texture at startup, so no font file ships. The UI costs one extra draw call per frame, and none when a game draws no UI.

## Physics and destruction

Ground-plane bodies use Verlet position/orientation integration. A chunk's bodies simulate only while the chunk is within one chunk of the focus and all eight neighbours are resident; bodies one ring further out collide as immovable obstacles. A fixed spatial hash emits candidates; SAT rejects non-contacting oriented boxes. Position corrections include angular response using rectangle inertia. Connected timber pieces exclude mutual contact to avoid fighting their joint constraints.

A support graph floods from fixed piers. Unsupported components undergo vertical Verlet gravity and settle at ground height. Distance constraints can break under excess stretch. Direct damage removes pieces; terrain excavation can remove a support. The model is deliberately narrow: no continuous collision detection, general joint editor, multilevel construction tools, or fluid simulation. The current fixed time step and movement bounds avoid high-speed gameplay in the demo.

## Audio

Impact sounds are synthesized from damped oscillators and noise. A bounded SPSC queue feeds the SDL audio callback; the callback allocates no memory and takes no locks. If no audio device is available, startup logs that sound is disabled. Cosmetic particles and active sounds are transient and are not persisted.

## Input

Games read named actions, not keys. Each action has up to four bindings (keys or mouse buttons), and the engine derives `held`, `pressed` and `released` per action once a frame, including mouse-button edges. The engine registers `quit` (Escape), `checkpoint` (F5) and `screenshot` (F12) first; games register theirs in `Game::actions` and may rebind any action at runtime. Bindings are not yet saved between sessions, and there is no gamepad support.

## Engine assumptions a new game inherits

These parts of the engine still assume a top-down game shaped like the demo. They are real limits, not stubs, and each needs engine work before a game that differs there.

- **Top-down only.** The ground plane is X/Y. Height exists only for building physics (support and falling) and is shown as an upward screen offset. There is no side-view gravity and no layered terrain.
- **Fixed chunk and tile geometry.** Chunks are always 32×32 one-unit tiles with one tile layer. The streamed region (5×5 requested, 7×7 retained, 49 slots) and the physics simulation radius are compile-time constants.
- **Physics is for buildings.** Bodies are oriented boxes from per-chunk recipes or built blocks. They support one another from anchored pieces, fall when unsupported, and take point damage with 100 health. Game characters are kinematic boxes moved with `move_character` (sliding against blocking tiles and bodies, under a game-supplied `TileRule`); they do not push bodies, and there are no circle or polygon shapes, raycasts or entity-versus-entity queries.
- **One focus entity.** The camera always centres on it, streaming follows it, and `player.delta` saves only its position. Other game entities and components are not saved.
- **Sound and effects.** `Audio` plays one synthesized impact sound, and `Particles` has one burst style. A game cannot define its own sounds or effects yet.
- **Rendering.** Sprites come from one generated atlas of 32×32 procedural tiles, or from whole BC3 textures. UI is a screen-space layer of rectangles and text in one built-in 5×7 ASCII font. There are no widgets, layout, clipping or other fonts. Scenes have at most 32 lights, and tone mapping is fixed; clear colour, ambient light and edge haze are set through `Renderer::lighting`.
- **Assets.** The pack format holds BC3 textures only, cooked from TGA. The root CMake file still cooks the demo's `demo.pak` (the flame sample), which the storage tests also use.
- **Benchmark report.** Its schema keeps two demo-named fields (`damage_demo`, `overview`) that games fill through `describe`, and the stream workload follows a fixed route around the origin.
