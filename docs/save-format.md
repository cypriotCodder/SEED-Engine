# Binary formats

All integers are unsigned little-endian unless specified otherwise. Signed chunk coordinates store the two's-complement 64-bit representation. Floats store IEEE-754 binary32 bits. There are no serialized native structs, pointers, STL layouts, or padding.

## Shared LZ4 envelope, version 1

The 20-byte header contains five u32 values: magic `0x344c4453`, envelope version, uncompressed byte count, compressed byte count, CRC32 of uncompressed payload. One raw LZ4 block follows. The reader limits files/payloads to 64 MiB, checks exact lengths, uses bounded decompression, and checks CRC32 before parsing payloads. CRC is for corruption detection, not authentication.

## World metadata

`world.seed` version 2 contains u32 magic `0x444c5257`, u32 schema version 2, u64 game ID, u64 generator ID, u32 generator version, and u64 seed. The game ID and generator ID are 64-bit FNV-1a hashes of the stable names the game declares (`Game::id` and `WorldGenerator::name`). Opening a save with a different game, generator, generator version or seed is an error with a message naming which one differs; the save is not changed. Untouched generated chunks do not create files.

## Several worlds in one save

A game made of several scenes keeps each scene's world in the same save, every file name starting with the scene's name and a dot: `cave.world.seed`, `cave.12_-3.chunk`, `cave.player.delta`. The scene name uses letters, digits, `_` and `-` (at most 64). Leaving a scene writes its world and the player's position under its prefix; coming back reads them, so each scene remembers its own changes. Games written in C++ with one world use no prefix. Project games saved before scenes had prefixes stored their one world without one; those saves are no longer read, and such a game starts a new world.

## Chunk changes, version 4

`X_Y.chunk` contains u32 magic `0x4b4e4843`, u32 schema version 4, u64 generator ID, u32 generator version, u64 seed, two signed 64-bit chunk coordinates, and u16 terrain-record count. Each terrain record is u16 tile index (row-major 32×32) plus u8 change flags. The game defines what each change bit means (`WorldGenerator::edit_bits` and `apply_edit`); in the demo, bit 0 removes a tree and bit 1 excavates terrain into water. Bits outside the game's mask are rejected. Repeated edits compact into one record per tile.

Next are u16 body-record count and records containing u16 body ID plus an 81-byte body state: u8 existence flag; current and previous positions (each two i64 chunk coordinates and two f32 local offsets); then eight f32 values: half-width, half-height, angle, previous angle, height, previous height, inverse mass, health. Generated recipe IDs remain stable, and unchanged recipe bodies are omitted. Destroyed recipe bodies remain tombstones. Built bodies are stored densely after the recipe; destroyed built bodies are omitted. Built-body indices are not persistent external handles.

Next, u16 broken-joint count precedes u16 joint recipe IDs. Endpoints/rest lengths are regenerated. Bodies and joints belong to the chunk that generated or placed them.

Finally, the saved-entity section: u16 entity count (at most 1,024), u32 section byte length, then that many records. Each record is a position (two i64 chunk coordinates and two canonical f32 local offsets), f32 angle, f32 visual width, f32 visual height, u8 material ID, u16 payload length (at most 4,096) and the game's payload bytes. The engine checks the record structure while loading; the game's `load_entity` must consume each payload exactly. Records keep scene order and are rewritten whole whenever a chunk's saved entities change. Unknown versions, duplicate IDs, invalid state, and trailing bytes are errors.

Legacy migration is disabled by user decision. Terrain-only chunk version 1, standalone `0_0.bodies` files, flat pre-checkpoint saves, and mismatched generator versions are rejected with an explicit message. Choose a new save directory; old files are not deleted or rewritten. World metadata version 1 and chunk versions 1 to 3 from earlier builds are rejected the same way; the checkpoint container is still version 1.

## Player

`player.delta` contains u32 magic `0x52594c50`, u32 version 1, u32 generator version, u64 seed, two i64 chunk coordinates, and two f32 local offsets. A player still at the initial origin needs no file. The seed is the world's own: each scene's terrain has its default seed, and the start scene the seed the game started with.

## Asset archive

`demo.pak` contains u32 magic `0x4b504453`, u32 version 1, and u32 entry count (up to 64). Each entry contains u16 name length, name bytes, u8 format (1 = BC3), u16 width, u16 height, u32 block byte count, u32 block CRC32, and BC3 blocks. Names are unique. Dimensions are positive multiples of four, at most 4096. The whole archive also uses the shared compressed envelope.

## Whole-world checkpoints, version 1

The save root contains `writer.lock`, private `working/`, immutable `checkpoints/<id>/`, atomic `CURRENT`, and, after a second commit, `RECOVERY`. The exclusive OS lock lasts for the open save session and is released automatically after a process crash. Stream-out writes only to `working/`.

At commit, physics finishes and resident terrain/building changes plus player state are written to the working directory. After all chunk readers/writers join, the main thread renames that directory into a new checkpoint and creates an empty working directory. This freezes the dirty files without enumerating or copying them during capture. A worker links unchanged files from the prior checkpoint, validates every envelope, and writes the manifest last. Filesystems without hard links use a durable byte-for-byte copy; snapshots never recompress file contents. After directory synchronization, `RECOVERY` is replaced with the previous known-good ID, then `CURRENT` is replaced atomically. Only after publication are older/orphan numeric checkpoint directories pruned, keeping current plus previous. Legacy files are never deleted to make an incompatible save load. Only seed and delta payloads are retained; generated terrain, textures, and audio are not persisted.

Both pointer files use the shared LZ4 envelope. Their payload is u32 magic `0x52545043`, u32 version 1, u64 current ID, u64 previous ID (zero when absent). IDs identify immutable snapshots, not world-generation seeds. The manifest payload is u32 magic `0x464e4d43`, u32 version 1, u64 checkpoint ID, u32 file count, then lexicographically sorted entries: u16 filename length, filename bytes, u32 uncompressed length, u32 uncompressed CRC32. Filenames must be recognized flat save filenames; duplicates, traversal paths, symlinks, missing files, and mismatched checksums are rejected. Each envelope also has its own CRC.

Startup validates the whole manifest before using a checkpoint. If current is corrupt, it tries the previous ID and then the recovery pointer. Recovery is reported. If neither committed checkpoint is valid, startup fails and preserves the save. Unknown checkpoint schema versions fail explicitly rather than rolling back. Unreferenced checkpoint directories and stale working files are never treated as committed saves. A crash before the first publication leaves a new world with no committed player changes. Pre-checkpoint flat saves are rejected before any working-directory cleanup.

Interactive saves start every 60 seconds or on F5. Publication uses its own job group, errors return to the main thread, and shutdown joins any pending publication before committing final state. Only one publication runs at a time. Recovery rolls back the **whole** snapshot, so it cannot mix player/building/terrain ages. A session that fails publication must be reopened before another commit; it cannot silently drop the failed snapshot's changes on retry.

During publication, reads resolve through the new working directory, then the frozen snapshot, then its preceding checkpoint. The world save barrier must finish existing readers/writers before rotating directories again. Working files must be replaced through `write_blob`, never edited in place. Unchanged files can share inodes across working/current/previous snapshots; atomic replacement preserves old versions, but a disk fault or external in-place write to a shared inode can corrupt more than one snapshot. Retained checkpoints are crash-consistency recovery, not independent backup copies.

Dirty world/player serialization and the reader/writer barrier still happen on the main thread. Snapshot capture takes a constant number of directory operations; validation, manifest generation, unchanged-file linking/copying, flushes and pruning run on the worker. Startup validates the complete snapshot and populates working files using links or raw copies. Save size and background work still scale with accumulated changes. Removing a file from working does not delete a prior delta: reset it through a new versioned delta record instead.

POSIX writes flush files and synchronize renamed-file parent directories. Windows uses flushed files and `MoveFileExW` with replacement/write-through. Process-interruption and corruption recovery were exercised on this Mac; power-loss durability and Windows/Linux behavior have not been validated here.
