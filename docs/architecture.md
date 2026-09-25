# Architecture

## Approved world model

The view is top-down. X/Y describe the ground plane. Elevation and structural support are separate simulation quantities. A world position is a pair of signed 64-bit chunk coordinates plus a local float offset in [0,32). Camera-relative subtraction only operates within a bounded neighborhood, avoiding conversion of enormous global coordinates to floats. Overflow is reported.

Gradient Perlin noise hashes integer lattice coordinates with a 64-bit seed. Five octave layers produce elevation; an independent layer produces moisture. Elevation/moisture thresholds choose water, beach, meadow, and rock. The current generator intentionally produces one island surrounded by ocean. Version 1 uses strict floating-point contraction settings; cross-architecture bit identity still needs measured validation before extending the compatibility promise.

## Ownership and memory

`seed_engine` owns platform, rendering, streaming, persistence, and physics code. The demo drives their lifetime explicitly. There are no inheritance hierarchies. `Scene` provides generational entity handles and dense sparse-set transform/visual components. Systems may retain handles, never component addresses across structural edits. Packed components are required to be trivially copyable.

A 2 MiB arena backs entity/component storage. A fixed pool holds 49 terrain chunks. The particle pool contains 512 slots. Physics uses fixed pools for 4,096 bodies, 16,384 center-distance joints, and spatial-hash entries. Each chunk stores at most 256 bodies and 1,024 recipe joints. The renderer allocates its 32,768-instance CPU store once and orphans the GPU stream buffer on each flush. Ordinary frame iteration does not grow STL containers. Allocation is allowed during initialization, loading, serialization, and failure reporting.

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

## Physics and destruction

Ground-plane bodies use Verlet position/orientation integration. A fixed spatial hash emits candidates; SAT rejects non-contacting oriented boxes. Position corrections include angular response using rectangle inertia. Connected timber pieces exclude mutual contact to avoid fighting their joint constraints.

A support graph floods from fixed piers. Unsupported components undergo vertical Verlet gravity and settle at ground height. Distance constraints can break under excess stretch. Direct damage removes pieces; terrain excavation can remove a support. The model is deliberately narrow: no continuous collision detection, general joint editor, multilevel construction tools, or fluid simulation. The current fixed time step and movement bounds avoid high-speed gameplay in the demo.

## Audio

Impact sounds are synthesized from damped oscillators and noise. A bounded SPSC queue feeds the SDL audio callback; the callback allocates no memory and takes no locks. If no audio device is available, startup logs that sound is disabled. Cosmetic particles and active sounds are transient and are not persisted.
