# Project format

A Seed project is a folder the editor creates. Every file is UTF-8 JSON written with two-space indentation and stable member order, so projects diff and merge cleanly in version control. Shipped games will read a compact binary form produced by the export step; until then games read these files directly.

```
My Game/
  project.seed.json   project identity
  assets/             data assets, one file per kind, including terrain.json
  scenes/             scenes, one file each; new projects start with main.json
  prefabs/            reusable entities, one file each
  scripts/            (Lua gameplay scripts, planned)
  .seed/              per-user editor state such as the window layout; ignored by git
```

## project.seed.json

`seed_project` (format, currently 1), `name` (1–64 letters, digits, spaces, `-`, `_`), `game_id`, and `game`: `start_scene` (the scene the game starts in, default `main`), `title` (window title; empty uses the name), `width` and `height` (logical window size, 320x240 to 7680x4320, default 1280x720) and `fullscreen`. Project files without `game` get the defaults. The game ID is a slug of the name plus eight random hex digits, generated once. Saves record it, so it never changes, even when the project is renamed. The editor refuses projects from a newer format.

## assets/

Each file is `{"format": 1, "<kind>": [...]}`. A missing file is an empty list. **List order is registration order and so decides each entry's ID.** Tiles store material IDs, so moving a material changes what existing saved worlds show; particle styles refer to materials by name and are unaffected. Every entry is validated with the same checks the running engine uses; the editor will not save invalid assets, and a game refuses to start with them.

- `materials.json`: `name`, `color` ([r, g, b], 0–255), `pattern` (`speckle`, `water`, `planks`, `round`), `variation` (1–255), optional `texture` (an image in `assets/textures`, by file name without the extension) and `texture_scale` (1–256: how many tiles one copy of the texture covers on the ground; 1 draws the whole image on each tile). At most 64.

### textures/

PNG, JPG or TGA images, named with letters, digits, `_` and `-`. The editor resamples each to sides that are multiples of four (at most 4096), compresses them to BC3 and writes `.seed/textures.pak`; it rebuilds the pack only when a file is added, removed or changed. Play uses that pack, and an export ships it as `Contents/Resources/game.pak`, so games never decode images. Exports stop when a material names a missing texture.
- `actions.json`: `name` and `bindings`, up to four input names: an SDL key name such as `"W"`, `"Space"` or `"Left Shift"`, or `"Mouse Left"`, `"Mouse Middle"`, `"Mouse Right"`, `"Mouse X1"`, `"Mouse X2"`. The engine's `quit`, `checkpoint` and `screenshot` actions come first and their names are reserved. At most 61.
- `sounds.json`: `name`, `frequency` (Hz), `variation` (Hz), `gain` (0–1), `decay` (volume multiplier per sample at 48 kHz, below 1), `tone` (0 noise to 1 pure tone). At most 32.
- `particles.json`: `name`, `material` (a material name), `count` (1–512), `speed`, `speed_range`, `life`, `size`, `drag` (0–1), `spin`, `shade`. At most 32.

Floats are written in the shortest form that reads back to the same 32-bit value.

## prefabs/

Each `<name>.json` is `{"format": 1, "entity": {...}}`: one entity in the scene format, placed at the origin. Names use letters, digits, `_` and `-`. Export checks every prefab and that every prefab a scene links exists.

### terrains/<name>.json

Each terrain is `{"format": 1, "terrain": {...}}`, describing a world generator; a project can have several, and each scene names the one it uses (scene field `terrain`, default `main`). Projects from before named terrains kept one `assets/terrain.json`; it loads as `main` and moves to `terrains/main.json` when the editor saves. The generator version saved with worlds is a hash of these settings (without `default_seed`), so any change starts new worlds instead of mixing old saves with new terrain.

- `island` (true: a disc of land in endless ocean; false: endless), `radius` (chunks; also the unit of `distance` terms), `coast` (island falloff as a fraction of the radius), `warp` (domain warp in tiles, 0 to 256) and `warp_wavelength`, `default_seed` (the world the editor previews and the game's default).
- `fields`: up to 8 of `{name, base, terms}`; one must be `elevation` (the tile height: below 0 is water; on islands the coast lowers it near the rim). A field is `base` plus the sum of its terms, each `{type, wavelength, amplitude, warped}`: `perlin` (smooth noise in -1..1, power-of-two wavelength up to 1024), `fractal` (octaves from 128 down to 8), `ridged` (1 - 2|perlin|, crests), `distance` (0 at the centre, 1 at the rim), `spot` (exp(-(d/wavelength)^2), a bump at the centre). `warped` samples at the domain-warped position. Each term has its own noise stream, derived from its field's name and position in the list.
- `rules`: up to 64 of `{name, material, when, solid, scatter}`. Each tile takes the first rule whose `when` ranges (`{field, min, max}`, either end optional) all hold; a tile no rule matches takes the last rule's material. `scatter` (`{material, one_in, solid}`) stands an object (`Tile::object`) on a hashed one tile in `one_in`, drawn over the ground, for trees and rocks. An empty rule list means no terrain.

A game with `Game::project_assets` set and no `world.terrain` callback generates its world from this file.

## scenes/

Each `<name>.json` is `{"format": 1, "terrain": "<name>", "entities": [...]}`, at most 4096 entities, drawn in list order (later entries on top). An entity has `name` (1–64 characters; need not be unique), `chunk` ([x, y], whole numbers), `position` ([x, y] within the chunk; offsets outside 0–32 are moved into the right chunk on load), `angle` (radians) and optional components:

- `visual`: `material` (a material name) and `size` ([w, h], above 0 and at most 64).
- `light`: `color` ([r, g, b], 0–16), `radius` (up to 256), `intensity` (0–64) and `height` (above 0, at most 64).

`character` (optional) makes the entity walk with collisions: `speed` and `run_speed` (tiles per second), `acceleration` (tiles per second squared; 0 is instant), `collision` ([w, h]), `blocked_by` (`water`, `solid`, `buildings`: true or false) and `face_movement`. One character per scene may have `player`: `actions` (`up`, `down`, `left`, `right`, `run`; action names from `assets/actions.json`, where `move_*` and `run` default to WASD, the arrows and Shift), `input` (moves with the keys; false leaves movement to its script), camera `zoom` (pixels per tile), `smoothing` (seconds) and `dead_zone` (tiles), and `resume` (a saved game starts where the player was). Other characters are NPCs, moved by their scripts. Scenes from before characters mark the player by naming an entity "Player"; that still works.

`prefab` (optional) links a placed copy to `prefabs/<name>.json`; the copy's `visual`, `light` and `script` are the prefab's, stored in the scene too so games need not resolve prefabs. `editor` (optional) holds editor-only `hidden` and `locked` flags that games ignore.

An entity with no components is a named marker, such as a spawn point. Material references are checked against `assets/materials.json`; the editor will not save a scene with problems.

A C++ game uses a project's assets by setting `Game::project_assets` to the folder; the engine registers them before calling the game's own `materials`, `actions` and `effects` callbacks, which may add more.

## Exported games

An exported app carries the project as `Contents/Resources/game.seedpack`: the shared LZ4 envelope (checksummed; see [binary formats](save-format.md)) around u32 magic `0x4b415053` ("SPAK"), u32 version 1, u32 file count, then, in path order, u16 path length, path, u32 size and bytes for `project.seed.json`, `assets/*.json`, `scenes/*.json` (all JSON re-written without whitespace) and `scripts/*.lua`. Paths are relative and limited to letters, digits, `_`, `-` and `.` with `/` separators; unsafe, duplicate or out-of-order paths and trailing bytes are rejected. `seed_player` runs `game.seedpack` when it sits in the base folder SDL reports, which is `Contents/Resources` inside an app bundle.
