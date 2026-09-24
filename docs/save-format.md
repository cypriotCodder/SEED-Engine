# Binary formats

All integers are unsigned little-endian unless specified otherwise. Signed chunk coordinates store the two's-complement 64-bit representation. Floats store IEEE-754 binary32 bits. There are no serialized native structs, pointers, STL layouts, or padding.

## Shared LZ4 envelope, version 1

The 20-byte header contains five u32 values: magic `0x344c4453`, envelope version, uncompressed byte count, compressed byte count, CRC32 of uncompressed payload. One raw LZ4 block follows. The reader limits files/payloads to 64 MiB, checks exact lengths, uses bounded decompression, and checks CRC32 before parsing payloads. CRC is for corruption detection, not authentication.

## World metadata

`world.seed` contains u32 magic `0x444c5257`, u32 schema version 1, u32 generator version, and u64 seed. Loading an incompatible seed or generator is an error. Untouched generated chunks do not create files.

## Terrain changes

`X_Y.chunk` contains u32 magic `0x4b4e4843`, u32 schema version 1, u32 generator version, u64 seed, two signed 64-bit chunk coordinates, and u16 record count. Each sorted record is u16 tile index (row-major 32×32) plus u8 change flags. Bit 0 removes vegetation; bit 1 excavates terrain into water. Repeated edits compact into one record per tile. Duplicate indices, unknown flags, oversized counts, and trailing bytes are errors.

## Building state

`0_0.bodies` currently stores the demo's globally bounded building set. Version 2 contains u32 magic `0x59444f42`, u32 version 2, u32 generator version, u64 seed, u16 total body slots, and u16 changed-record count.

Each record begins with u16 stable recipe/body ID, followed by 81 bytes: u8 existence flag; current and previous positions (each two i64 chunk coordinates and two f32 local offsets); then eight f32 values: half-width, half-height, angle, previous angle, height, previous height, inverse mass, health. Existing recipe bodies are omitted if identical to the baseline. Added bodies always have records. Deleted IDs remain tombstones and are not reused.

After bodies, a u16 broken-joint count is followed by u16 joint recipe IDs. Joint endpoints/rest lengths are regenerated. The reader also accepts the initial local-development version 1 and rewrites it to version 2 on save. Unknown versions are rejected.

## Player

`player.delta` contains u32 magic `0x52594c50`, u32 version 1, u32 generator version, u64 seed, two i64 chunk coordinates, and two f32 local offsets. A player still at the initial origin needs no file.

## Asset archive

`demo.pak` contains u32 magic `0x4b504453`, u32 version 1, and u32 entry count (up to 64). Each entry contains u16 name length, name bytes, u8 format (1 = BC3), u16 width, u16 height, u32 block byte count, u32 block CRC32, and BC3 blocks. Names are unique. Dimensions are positive multiples of four, at most 4096. The whole archive also uses the shared compressed envelope.

## Durability boundary

Replacement is atomic per file. The world is not a single multi-file transaction, so abrupt process/power loss between file replacements can expose different checkpoint ages. Shutdown save failures are reported; they do not claim successful persistence. Multi-file recovery/journaling remains a release-hardening item.
