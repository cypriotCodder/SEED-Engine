# Project format

A Seed project is a folder the editor creates. Every file is UTF-8 JSON written with two-space indentation and stable member order, so projects diff and merge cleanly in version control. Shipped games will read a compact binary form produced by the export step; until then games read these files directly.

```
My Game/
  project.seed.json   project identity
  assets/             data assets, one file per kind
  scenes/             scenes, one file each; new projects start with main.json
  scripts/            (Lua gameplay scripts, planned)
  .seed/              per-user editor state such as the window layout; ignored by git
```

## project.seed.json

`seed_project` (format, currently 1), `name` (1–64 letters, digits, spaces, `-`, `_`) and `game_id`. The game ID is a slug of the name plus eight random hex digits, generated once. Saves record it, so it never changes, even when the project is renamed. The editor refuses projects from a newer format.

## assets/

Each file is `{"format": 1, "<kind>": [...]}`. A missing file is an empty list. **List order is registration order and so decides each entry's ID.** Tiles store material IDs, so moving a material changes what existing saved worlds show; particle styles refer to materials by name and are unaffected. Every entry is validated with the same checks the running engine uses; the editor will not save invalid assets, and a game refuses to start with them.

- `materials.json`: `name`, `color` ([r, g, b], 0–255), `pattern` (`speckle`, `water`, `planks`, `round`), `variation` (1–255), optional `texture` (a name in the game's asset pack). At most 64.
- `actions.json`: `name` and `bindings`, up to four input names: an SDL key name such as `"W"`, `"Space"` or `"Left Shift"`, or `"Mouse Left"`, `"Mouse Middle"`, `"Mouse Right"`, `"Mouse X1"`, `"Mouse X2"`. The engine's `quit`, `checkpoint` and `screenshot` actions come first and their names are reserved. At most 61.
- `sounds.json`: `name`, `frequency` (Hz), `variation` (Hz), `gain` (0–1), `decay` (volume multiplier per sample at 48 kHz, below 1), `tone` (0 noise to 1 pure tone). At most 32.
- `particles.json`: `name`, `material` (a material name), `count` (1–512), `speed`, `speed_range`, `life`, `size`, `drag` (0–1), `spin`, `shade`. At most 32.

Floats are written in the shortest form that reads back to the same 32-bit value.

## scenes/

Each `<name>.json` is `{"format": 1, "entities": [...]}`, at most 4096 entities, drawn in list order (later entries on top). An entity has `name` (1–64 characters; need not be unique), `chunk` ([x, y], whole numbers), `position` ([x, y] within the chunk; offsets outside 0–32 are moved into the right chunk on load), `angle` (radians) and optional components:

- `visual`: `material` (a material name) and `size` ([w, h], above 0 and at most 64).
- `light`: `color` ([r, g, b], 0–16), `radius` (up to 256), `intensity` (0–64) and `height` (above 0, at most 64).

An entity with no components is a named marker, such as a spawn point. Material references are checked against `assets/materials.json`; the editor will not save a scene with problems.

A C++ game uses a project's assets by setting `Game::project_assets` to the folder; the engine registers them before calling the game's own `materials`, `actions` and `effects` callbacks, which may add more.
