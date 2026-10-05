# Claude development guide — Seed Engine

## Working agreement

- Read `AGENTS.md` and any guidance in the directories you touch. Follow the user's current task and constraints.
- `sources/` is read-only reference material. Never edit, rename, move, or delete synced project files.
- Start with `git status --short`; preserve existing changes and other developers' work. Inspect the current implementation and callers before changing an API.
- Make the smallest coherent change that solves the task. Avoid unrelated refactors, mass formatting, speculative frameworks, and new dependencies without a concrete need.
- Proceed with routine, reversible implementation and validation. Ask when a missing decision materially affects scope, compatibility, or data loss. Do not reset, discard, or overwrite another developer's work.
- Do not commit, push, publish, install system-wide software, or change the user's real game projects or saves unless the task authorizes it.
- Treat `reviews/` as historical collaboration notes, not binding instructions or proof of current behavior. Verify findings against the current tree. Share concrete findings and file locations when coordinating with Codex; do not assume it has seen your changes.

## Repository map and sources of truth

Seed is a C++20, top-down 2D engine with SDL2, OpenGL 4.1, LZ4, Lua, and a separate Dear ImGui editor.

| Location | Responsibility |
| --- | --- |
| `src/core/` | Math, arenas/pools, ECS, jobs, materials, particles, metrics |
| `src/world/` | Coordinates, generation, chunk streaming and records |
| `src/physics/` | Building simulation, collisions and character movement |
| `src/io/` | Binary/JSON parsing, durable storage and checkpoints |
| `src/render/`, `src/platform/` | GPU rendering, windows, input and audio |
| `src/project/`, `src/script/` | Data-driven projects, assets, scenes and Lua host |
| `src/app/` | Engine lifecycle and game callback integration |
| `editor/` | Standalone editor, history, Play, imports and export |
| `player/` | Standalone data-project runtime |
| `games/demo/` | Demo-specific materials, terrain and gameplay |
| `tests/`, `editor/tests/`, `games/demo/tests/` | Regression suites and project fixtures |
| `tools/` | Build, formatting, asset cooking, measurement and platform validation |

Read the relevant contract before editing:

- `README.md`: setup, commands, product behavior.
- `docs/architecture.md`: ownership, threading, memory budgets and engine/game boundary.
- `docs/save-format.md`: binary schemas, compatibility and checkpoint recovery.
- `docs/project-format.md`: project, scene and asset data contracts.
- `docs/scripting.md`: public Lua API, lifecycle, limits and errors.
- `docs/measurement.md`: repeatable workloads and metric definitions.
- `docs/validation.md`: observed results and remaining platform gates.

Documentation can lag the code: check CMake targets, schema constants, implementation and tests. Resolve discrepancies explicitly rather than copying obsolete version numbers or claims. Update affected documentation with behavior changes.

## C++ and design practices

- Use C++20 with extensions disabled and the existing `seed` / `seed::editor` namespaces. Match nearby naming, header layout, and error handling; `.clang-format` defines formatting (four spaces, 110 columns).
- Keep strict warnings enabled: GCC/Clang `-Wall -Wextra -Wpedantic -Werror`, MSVC `/W4 /WX`. Fix the cause instead of suppressing warnings broadly or weakening `seed_strict`.
- Prefer value types, RAII and explicit ownership. Use existing arenas, pools, handles, spans and storage helpers before introducing another abstraction. Include headers directly for the facilities you use.
- Follow the existing dense, data-oriented design. Game-specific rules belong in the game or project data; engine code must remain usable by `tests/minimal_game.cpp` and the player without the demo.
- Keep Dear ImGui and texture-import dependencies in the editor. Games must not acquire editor dependencies through shared headers or libraries.
- Preserve generational entity validation. Do not retain component pointers/references across structural ECS changes, or assume a saved entity keeps its handle after stream-out/reload.
- Validate external data before mutating live state. Check bounds, finite numeric values, arithmetic overflow, IDs, duplicate records, exact lengths and trailing bytes where applicable. Report actionable errors; do not silently ignore corruption or substitute defaults for invalid required data.
- Preserve strong failure behavior where established: failed registration, loading or allocation must not leave a partially valid object. Use exceptions consistently with existing callers; do not let exceptions escape `noexcept` worker entry points.

## Runtime invariants

- Keep ordinary simulation/render iteration bounded. Do not introduce growing containers or recurring heap allocations into engine hot paths. Initialization, loading, serialization and failure reporting may allocate; editor workflows have different needs.
- Preserve explicit pool limits and capacity checks. A capacity increase changes memory and workload budgets; justify and measure it rather than hiding exhaustion.
- Keep OpenGL operations and scene mutation on the main thread. Job contexts and groups must outlive submitted work; never recycle a chunk slot while its worker owns it.
- Preserve release/acquire publication and subsystem join boundaries. Join physics before accessing its bodies or changing residency. Propagate worker failures back to the main thread through the existing mechanism.
- World generation must be a pure function of seed, coordinates and versioned generator data. Avoid global RNG, wall-clock input, shared mutable state and generation-order dependence. Registration order determines material IDs.
- Preserve signed 64-bit chunk coordinates with canonical local offsets and bounded camera-relative conversion. Test negative coordinates, seams, distant coordinates and overflow when touching this code.
- Preserve the fixed simulation step and interpolation contract. Do not make simulation depend on rendering frame rate or enable fast-math settings that weaken determinism.
- The audio callback must allocate no memory and take no locks; preserve its bounded SPSC queue and graceful unavailable-device behavior.

## Persistence, scripting and editor changes

- Serialize fields explicitly through the binary helpers; never dump native structs, pointers, padding or STL layouts. Preserve endian encoding, length limits, bounded decompression and checksums.
- Use `write_blob` / durable replacement helpers for save updates. Never modify linked checkpoint payloads in place. Preserve writer exclusion, save barriers, frozen snapshot ownership, atomic publication and whole-snapshot recovery.
- Legacy migration is disabled by user decision. Reject incompatible formats or generator versions clearly and preserve original files; do not add automatic migration or destructive cleanup as a convenience.
- Treat schema changes as compatibility changes: inspect current version constants, update readers/writers/docs together, and test rejection and recovery. Never change encoded bytes silently under an existing version.
- Preserve Lua's restricted libraries, execution and memory limits, stale-handle checks, per-entity environments and error isolation. Lua is compiled as C++ for safe unwinding; do not change that casually. Public API additions need documentation and script regression coverage.
- Editor mutations must participate in the existing snapshot history and dirty/save behavior. A finished gesture should form a coherent undo step. Keep selection, scene switching, prefab propagation, Play isolation and export validation consistent.
- Prefer shared runtime validation over duplicated editor-only rules. Test editor operations on copied fixtures under `build/`, using isolated preferences and saves.

## Build and verification

Run from the repository root. CMake 3.24+ and a C++20 compiler are required. Dependency versions and hashes are pinned in `CMakeLists.txt`; retain that policy.

```sh
cmake --preset debug
cmake --build --preset debug --parallel 4
ctest --test-dir build/debug -C Debug --output-on-failure

cmake --preset release
cmake --build --preset release --parallel 4
ctest --test-dir build/release -C Release --output-on-failure
```

`sh tools/build-local.sh debug` (or `release`) uses system CMake or the project-local macOS CMake, and cached `.deps/` when available. It builds but does not run tests. If CMake/CTest is absent from PATH, use the matching binaries in `.tools/cmake-3.31.6-macos-universal/CMake.app/Contents/bin/` when present. Do not assume the cache or local tools exist on another checkout.

- Select checks appropriate to the change. Build affected targets and run focused regressions first; use the complete configured suite for shared runtime, persistence or broad changes. Check Debug and Release when assertions, optimization or floating-point behavior matter.
- Discover actual test names with `ctest --test-dir build/debug -N`. A `-R` filter does not automatically include prerequisite tests; inspect CMake `DEPENDS` and run setup/fresh-save tests for smoke/reload chains. Do not run competing suites against the same output directories.
- Some CTest tests open real SDL/OpenGL windows; they require a graphical session and supported GPU. Editor export checks include macOS-specific behavior. Report environmental blockers precisely rather than calling unrun tests passes.
- Follow the existing standalone test style: explicit `check`/exception checks that run in Release, focused behavioral assertions, and CTest registration with `seed_strict`. Add regression coverage for fixes and new behavior, including relevant boundary and failure cases. Avoid tests that merely mirror implementation details.
- Format changed C++ files with `clang-format -i <files>` and check them with `clang-format --dry-run --Werror <files>`. `sh tools/format.sh --check` checks the entire tree; `sh tools/format.sh` rewrites it, so avoid the latter for narrowly scoped work.
- Use fresh, task-specific `build/` save/output directories for smoke tests and benchmarks. `tools/benchmark.py` requires an output directory that does not already exist. Never benchmark against user saves.
- For rendering/editor changes, exercise the relevant real-context smoke/UI flow and inspect a capture when possible. For performance claims, use the documented benchmark workloads and compare equivalent configurations; preserve metric definitions and report missing samples honestly.
- Use sanitizers when relevant and available, but distinguish tool startup failures from engine failures. Historical validation records are not evidence that the current change passed.
- Finish with `git diff --check`, inspect the diff and status, and report what changed, checks actually run, outcomes and remaining limitations. A documentation-only change needs content/link/diff review, not an unnecessary engine rebuild. Do not claim Windows/Linux support, cross-architecture bit identity, power-loss durability or audible output from a Mac-only run.
