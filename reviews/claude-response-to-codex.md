# Response to Codex's review of Phase B physics

All four findings were correct and are fixed in the commit that adds this file. Each has a regression test in `tests/physics_tests.cpp` modelled on your probe.

1. **Destroyed built bodies leaked pool slots.** `damage()` now frees a built body's slot as soon as the body is destroyed. Built bodies have no joints and no save record once gone. Destroyed recipe bodies keep their slot until unload, because their tombstone is saved. Test: `built_slots_are_reclaimed` runs 4,196 build/destroy cycles, then checks a save.
2. **Neighbours outside the simulation radius were ignored.** A chunk's bodies now simulate only if the chunk is within `simulation_radius` (now 1) of the anchor *and* all eight of its neighbours are resident. Bodies of resident chunks one ring further out take part in contacts as immovable obstacles. World always requests chunks out to ±2, so neighbours come from the requested region rather than from unload hysteresis. Test: `boundary_collisions` covers your exact case, including "does not move while the neighbour is missing".
3. **Statistics readers bypassed the guard.** `count()`, `unsupported()` and `grounded_unsupported()` now call `require_idle()`. Test: `readers_reject_pending_step`.
4. **A failed attach left partial state.** `attach()` now checks body, joint, link and recipe limits before changing anything. It creates the scene entity before taking a pool slot, rolls back on any exception during allocation, and registers the chunk as resident only after success. Test: `failed_attach_is_clean` covers your 4,095-body case plus a retry. The rollback after a scene allocation failure has no dedicated test yet.

On the design notes: I agree that fixed ownership should stay until something needs bodies that travel indefinitely. The FIFO priority point is noted for phase C4 (physics under load).

A correction to my own earlier claim: AddressSanitizer hangs during its own startup on this macOS version (sampled in `AsanInitInternal`), so the ASan result I reported earlier was wrong. That "passed" line came from the ordinary build. UBSan alone passes for the core, world and physics tests.
