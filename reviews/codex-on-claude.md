# Review: Claude's chunk residency, job groups, and building physics

Reviewer: Codex, 2026-09-24. Reviewed the current combined tree at `b15b916`, focusing on the Phase B changes named in `claude-on-codex.md`: job groups, structure recipes, the version-2 chunk codec, physics pools and residency, support traversal, collision pair deduplication, damage selection, world hooks, and their tests. This is a review only; engine sources were not changed.

Four correctness findings follow. The existing tests pass, but they do not cover these cases.

## 1. [P2] Destroyed built bodies consume pool slots until their owner unloads

Location: `src/physics/physics.cpp:411–413`, with allocation checks at `422–437`.

`damage()` destroys the scene entity and sets `state.exists = false`, but leaves the physics slot live and never returns it to `free_bodies`. `build()` counts only existing built bodies against the per-chunk budget, so repeatedly placing and destroying a block consumes the global pool even though the chunk has no surviving blocks. `store()` omits destroyed built bodies from the save but does not reclaim their resident slots either. Unloading the owner is currently the only reclamation path.

**Reproduced:** attach an empty chunk at `(8,8)`, repeatedly `build({{8,8},{12,12}})` and `damage(...,100)`. After exactly 4,096 successful cycles, `count()` is zero and the next `build()` returns false. The exhausted pool also prevents other chunks from allocating bodies.

**Suggested fix:** immediately reclaim destroyed non-recipe bodies, which have no generated joints or persistent tombstone requirement. Keep recipe destruction state separately or retain its tombstone as needed. Ensure reclamation does not destroy the already-destroyed scene entity twice. Add a regression that exceeds the global capacity in sequential build/destroy operations while keeping live occupancy small, including a save/reload check.

## 2. [P2] Resident neighbors outside the simulation radius disappear from collisions

Location: `src/physics/physics.cpp:186–187` and `231–234`; related loading bounds in `src/world/world.cpp:129–131`.

`contacts()` inserts only `active()` bodies into the spatial hash, and `active()` tests the **owner** chunk against the simulation radius. A simulated body can overlap a resident body whose owner is just outside that radius, but the second body is omitted entirely—even when it is an anchored obstacle. The documented extra resident ring therefore does not provide collision neighbors. In addition, World initially requests only the same ±2 region that physics simulates; retention out to ±3 is hysteresis for previously visited chunks, not a guaranteed loaded halo.

**Reproduced:** anchor the simulation at chunk `(0,0)`. Attach a movable half-size `(0.5,0.5)` body in owner `(2,0)` at local `(31.8,12)` and an anchored body in owner `(3,0)` at local `(0.2,12)`, both at ground height. After a step, their penetration is still `0.599998`: the anchored neighbor was never considered by the contact solver. Both owners were explicitly attached, so this reproduction does not depend on loading latency.

**Suggested fix:** distinguish bodies eligible for integration from bodies eligible as collision neighbors. Provide a genuinely loaded collision halo around the active region, or defer boundary simulation until its neighbors are available. Decide explicitly whether non-simulated neighbors act as static obstacles or whether their collision response wakes them. Add a boundary test against a stationary obstacle and a case where the outer neighbor has not loaded yet.

## 3. [P2] Statistics readers bypass the pending-step guard and can race the worker

Location: `src/physics/physics.cpp:486–500`.

`unsupported()` and `grounded_unsupported()` do not call `require_idle()`. They read `supported` and `height` while `support()` and `simulate()` can write those same fields on the worker. Calling these methods between `begin_step()` and `finish_step()` can therefore cause a C++ data race, contrary to the class's stated contract that state access in that interval throws. The current demo calls these methods after joining; the defect is in the reusable public API.

**Reproduced without introducing a race in the test:** hold the sole worker behind a gate, call `begin_step()`, then call `grounded_unsupported()` before releasing the gate. It returns instead of throwing. The gate makes this a deterministic contract check; the data-race conclusion follows from the worker's writes, not from a claimed sanitizer result.

**Suggested fix:** apply `require_idle()` to both readers, or publish an immutable statistics snapshot at `finish_step()`. Make `count()` consistent with the documented access contract too, though its currently read fields are not modified by simulation. Test each state reader while a step is pending.

## 4. [P2] A failed chunk attachment leaves partial residency and allocations behind

Location: `src/physics/physics.cpp:93–116`, with the related allocation ordering at `63–74`.

`attach()` marks the resident slot used before allocating bodies and joints. If a capacity check or scene allocation fails midway, it throws without rolling back the bodies, entities, joints, or resident entry already created. Retrying the same chunk then reports “Chunk bodies attached twice.” Through the world activation hook, the chunk remains ready while physics already regards it as resident, so the two systems disagree. `allocate_body()` also consumes a free slot before `Scene::create()` succeeds.

**Reproduced:** fill the physics pool with 4,095 bodies across valid-size chunks, then attach a new chunk containing two bodies. The attachment throws, but the live count rises to 4,096. Release a different 256-body chunk to free capacity and retry: attachment still throws a duplicate-residency error.

**Suggested fix:** preflight available body/joint/resident capacity and recipe bounds before mutation, then make attachment transactional for failures that can still occur. Publish the resident entry only after successful construction, or use rollback bookkeeping that restores every allocation and adjacency list. Restore the free-body slot when scene creation fails. Test that a rejected attachment leaves counts and residency unchanged and succeeds on retry after capacity becomes available.

## Design decisions

Fixed owner chunks are a reasonable first implementation for buildings: they keep recipes, joints, and deltas together and avoid a cross-chunk persistence transaction for each movement. The movement clamp is an explicit gameplay restriction, not a general solution for freely traveling rigid bodies. I would keep that ownership model for now and fix the collision/loading boundary above. If later gameplay needs vehicles or debris traveling indefinitely, revisit ownership with an explicit migration contract.

Rejecting state access during a pending step is also a sound, simple ownership rule for a single-programmer engine. Complete its enforcement rather than adding locks to every physics accessor. Immutable debug statistics could be published at the join if rendering needs them during the next worker step.

The job-group wait correctly separates completion bookkeeping by subsystem under the same mutex. It still shares a FIFO worker queue, so waiting for one's own group does not promise scheduling priority over queued generation. That is a performance consideration to measure, not a correctness finding here. The support traversal marks bodies before enqueueing, and the reference-cell collision deduplication appears consistent for the broad-phase snapshot; I did not identify a separate defect in either algorithm.

## Verification

The Release build completed with no warning/error lines in `build/review/build.log`. All four existing CTest programs passed. A separate, project-local probe compiled with `-std=c++20 -Wall -Wextra -Werror` reproduced the four findings against the current physics implementation. Its source is `build/review/physics_probe.cpp`; it does not change production code or the existing tests.

Observed probe output:

```text
build/destroy cycles=4096 live=0 next_build=0
simulation-boundary overlap=1 depth=0.599998
pending-step statistics rejected=0
failed_attach=1 live_after_failure=4096 (before=4095)
retry_after_freeing_capacity_rejected_as_duplicate=1
```

Validation was limited to this Mac. No graphics run or ThreadSanitizer run was needed or claimed for this review. Persistence/measurement findings in the companion review are outside this review's Phase B scope.
