# Phase R: changes in your area (for review when you're back)

From: Claude, 2026-09-25. The user asked me to carry phase R alone while you were out of usage, including the persistence step we had planned to agree on together (see `claude-phase-r-plan.md`). The user approved the save-identity proposal as written. Please review these commits and reply in `reviews/`.

Persistence and measurement changes:

- **`world.seed` version 2** (commit `1603187`): u32 magic, u32 version 2, u64 game ID, u64 generator ID, u32 generator version, u64 seed. The IDs are FNV-1a hashes of `Game::id` and `WorldGenerator::name`. Each mismatch gives its own error, and the save is not changed.
- **Chunk files version 3**: the u64 generator ID now comes before the generator version in the header. `docs/save-format.md` is updated. I did not add the game ID to the checkpoint manifest (the question in my plan note). `world.seed` sits inside every checkpoint, so a checkpoint from another game fails when the world opens. If you want recovery to reject it earlier, that change is yours to make.
- **`player.delta` is unchanged on disk.** `load_player` and `save_player` now take the generator version as a parameter instead of a global constant. `tests/checkpoint_tests.cpp` passes `1` explicitly (commit `143e4ab`); it was implicitly 2 before.
- **Checkpoint wiring and the benchmark** moved from the demo's `main.cpp` into `src/app/app.cpp` (commit `d861114`) with no logic change. `Engine::write_deltas` is the old `save_checkpoint` body. The verify-stream sequence now calls it too; it's identical apart from a physics join, which is a no-op there. `BenchmarkMetadata::generator_version` is now filled by the engine from the world generator. `damage_demo` and `overview` are filled by the demo through `Game::describe`. The schema is unchanged.
- **The asset pack is optional** (commit `3d9beb6`): `Pack()` is an empty pack, for games with no textures.

Every step was checked against a baseline recorded before phase R: smoke output text, a screenshot hash, and the saved chunk bytes. The saved bytes changed only in step 8, as intended. Both benchmark workloads still produce complete reports.

## G5: saved entities (chunk file version 4)

The user chose per-chunk entity saving. `Engine::create_saved` adds an engine `Saved{owner}` component; `update_owners` moves ownership to the entity's current chunk only while that chunk is active, so an entity outside the active area stays with its last owner rather than being dropped. The engine chains its entity hooks around the physics `ChunkHooks`: release and store encode the owner's entities into `Chunk::entities` (and release destroys them), and activate recreates them and calls `Game::load_entity`. Records carry transform, visual and a game payload of at most 4 KiB; the chunk file appends u16 count, u32 length and the records. Decoding checks the record structure on the worker, and material IDs and payload consumption are checked on the main thread at activation. Dirty detection compares encoded bytes, so a reordering in the dense set can cause a harmless extra write. Please look at whether generated (recipe) entities are needed: today only runtime-created entities are saved, and a game that creates saved entities in `setup` would duplicate them on every load.
