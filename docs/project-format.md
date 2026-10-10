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

`seed_project` (format, currently 1), `name` (1–64 letters, digits, spaces, `-`, `_`), `game_id`, and `game`: `start_scene` (the scene the game starts in, default `main`), `title` (window title; empty uses the name), `width` and `height` (logical window size, 320x240 to 7680x4320, default 1280x720) `fullscreen` and optional `sort_by_y` (written only when true: characters, terrain objects and entities are drawn from the back to the front by their bottom edge, so what stands lower on the screen is in front; otherwise terrain objects are drawn first and entities over them in scene order). Project files without `game` get the defaults. The game ID is a slug of the name plus eight random hex digits, generated once. Saves record it, so it never changes, even when the project is renamed. The editor refuses projects from a newer format.

## assets/

Each file is `{"format": 1, "<kind>": [...]}`. A missing file is an empty list. **List order is registration order and so decides each entry's ID.** Tiles store material IDs, so moving a material changes what existing saved worlds show; particle styles refer to materials by name and are unaffected. Every entry is validated with the same checks the running engine uses; the editor will not save invalid assets, and a game refuses to start with them.

- `materials.json`: `name`, `color` ([r, g, b], 0–255), `pattern` (`speckle`, `water`, `planks`, `round`), `variation` (1–255), optional `texture` (an image in `assets/textures`, by file name without the extension) and `texture_scale` (1–256: how many tiles one copy of the texture covers on the ground; 1 draws the whole image on each tile), optional `frames` (1–64) and `fps` (0–60), written only when `frames` is above 1: the texture is then a horizontal strip of that many equal pictures played in turn (characters play it while they walk and show the first picture standing), optional `speed` (0.1–4, default 1: characters walk at their own speed times this on the ground), optional `object_size` ([w, h], each 0.25–16, written only when not [1, 1]: placed on the terrain as an object, its picture's size in tiles, standing on its tile's bottom edge and centred across it, so a large tree's crown overhangs the tiles behind; only its own tile blocks), optional `blend` (0–255, default 0: on the ground, a material fades over neighbouring tiles whose `blend` is lower along a wavy edge; 0 never fades over others, so its edges stay square) and optional `tags` (up to 8 words of letters, digits, `_` and `-`, at most 32 characters each, for scripts; the engine gives them no meaning). `speed`, `blend` and `tags` are written only when set. At most 64.

### textures/

PNG, JPG or TGA images, named with letters, digits, `_` and `-`. The editor resamples each to sides that are multiples of four (at most 4096), compresses them to BC3 and writes `.seed/textures.pak`; it rebuilds the pack only when a file is added, removed or changed. Play uses that pack, and an export ships it as `Contents/Resources/game.pak`, so games never decode images. Exports stop when a material names a missing texture.
- `actions.json`: `name` and `bindings`, up to four input names: an SDL key name such as `"W"`, `"Space"` or `"Left Shift"`, or `"Mouse Left"`, `"Mouse Middle"`, `"Mouse Right"`, `"Mouse X1"`, `"Mouse X2"`. The engine's `quit`, `checkpoint` and `screenshot` actions come first and their names are reserved. At most 61.
- `sounds.json`: `name`, `frequency` (Hz), `variation` (Hz), `gain` (0–1), `decay` (volume multiplier per sample at 48 kHz, below 1), `tone` (0 noise to 1 pure tone), and optional `file`: a recording in `assets/sounds` (a `.wav` or `.ogg` file name of letters, digits, `_`, `-` and `.`), played at `gain` instead of the synthesized tone. At most 32.

### sounds/ and music/

`sounds/` holds recordings for `sounds.json`: WAV (8, 16, 24 or 32-bit integer, or 32-bit float PCM) or Ogg Vorbis, mono or stereo, 8 to 192 kHz, at most 10 seconds. Games decode them once at startup to mono 48 kHz. `music/` holds Ogg Vorbis songs that scripts play by name (`music.play("theme")` plays `music/theme.ogg`); music is decoded a little at a time as it plays. A missing or undecodable recording stops the game (and the editor's export) with the file's name. Both folders ship in exported games as they are, so Ogg stays compressed.
- `particles.json`: `name`, `material` (a material name), `count` (1–512), `speed`, `speed_range`, `life`, `size`, `drag` (0–1), `spin`, `shade`. At most 32.

Floats are written in the shortest form that reads back to the same 32-bit value.

## prefabs/

Each `<name>.json` is `{"format": 1, "entity": {...}}`: one entity in the scene format, placed at the origin. Names use letters, digits, `_` and `-`. Export checks every prefab and that every prefab a scene links exists.

### terrains/<name>.json

Each terrain is `{"format": 1, "terrain": {...}}`, describing a world generator; a project can have several, and each scene names the one it uses (scene field `terrain`, default `main`). Projects from before named terrains kept one `assets/terrain.json`; it loads as `main` and moves to `terrains/main.json` when the editor saves. The generator version saved with worlds is a hash of these settings (without `default_seed`), so any change starts new worlds instead of mixing old saves with new terrain.

- `island` (true: a disc of land in endless ocean; false: endless), `radius` (chunks; also the unit of `distance` terms), `coast` (island falloff as a fraction of the radius), `warp` (domain warp in tiles, 0 to 256) and `warp_wavelength`, optional `relief` (0 to 100, written only when set: how strongly land is shaded by slope, as if lit from the north-west; it changes only how the world looks, not what is generated, so saves stay compatible), `default_seed` (the world the editor previews and the game's default).
- `fields`: up to 8 of `{name, base, terms}`; one must be `elevation` (the tile height: below 0 is water; on islands the coast lowers it near the rim). A field is `base` plus the sum of its terms, each `{type, wavelength, amplitude, warped}`: `perlin` (smooth noise in -1..1, power-of-two wavelength up to 1024), `fractal` (octaves from 128 down to 8), `ridged` (1 - 2|perlin|, crests), `distance` (0 at the centre, 1 at the rim), `spot` (exp(-(d/wavelength)^2), a bump at the centre). `warped` samples at the domain-warped position. Each term has its own noise stream, derived from its field's name and position in the list.
- `rules`: up to 64 of `{name, material, when, solid, scatter}`. Each tile takes the first rule whose `when` ranges (`{field, min, max}`, either end optional) all hold; a tile no rule matches takes the last rule's material. `scatter` (`{material, one_in, solid}`) stands an object (`Tile::object`) on a hashed one tile in `one_in`, drawn over the ground, for trees and rocks. An empty rule list means no terrain.
- `features` (optional, up to 16; written only when there are any): small patterns of tiles stamped into the world, such as ruins or a pond. Each is `{name, one_in, when, rows, cells}`: a chunk gets the feature with a chance of 1 in `one_in` (1 to 1,000,000), at a place within the chunk picked by a hash of the seed, the feature's name and the chunk coordinate, if the `when` ranges hold at the pattern's centre. `rows` (1 to 16 strings of equal width, 1 to 16; the top row is the northernmost) use the symbols of `cells` (up to 16 of `{symbol, ground, object, solid}`: one printable character other than `.`; a material to lay, or none to keep the generated ground; an object to place, or none to clear any; whether it blocks), and `.` to leave a tile as generated. A pattern always lies within one chunk, so a world stays a pure function of its seed. Features are stamped after the rules in list order, and paint goes over them. Rivers and roads need no feature: a `ridged` field is near 1 along thin winding lines, so a rule such as `river` above 0.94 laying water (or a path material) draws them, as the starter island does.

A game with `Game::project_assets` set and no `world.terrain` callback generates its world from this file.

## scenes/

Each `<name>.json` is `{"format": 1, "terrain": "<name>", "atmosphere": {...}, "entities": [...]}`, at most 4096 entities, drawn in list order (later entries on top). An entity has `name` (1–64 characters; need not be unique), `chunk` ([x, y], whole numbers), `position` ([x, y] within the chunk; offsets outside 0–32 are moved into the right chunk on load), `angle` (radians) and optional components:

- `visual`: `material` (a material name) and `size` ([w, h], above 0 and at most 64).
- `light`: `color` ([r, g, b], 0–16), `radius` (up to 256), `intensity` (0–64), `height` (above 0, at most 64), and optional `flicker` (0–1: how much it wavers, as a flame does) and `night_only` (lit only as daylight fades).
- `path`: `points` (2 to 256 `[x, y]` offsets in tiles from the entity, within 4,096; not rotated with it) and optional `loop` (written only when true). Characters walk it with `e:follow` (see [scripting](scripting.md)). Prefab copies keep their own path.
- `area`: `size` ([w, h] in tiles, 0.1–1024): an invisible rectangle centred on the entity, not rotated with it. Its script's `on_enter`/`on_exit` run as characters come and go (see [scripting](scripting.md)). Prefab copies take the prefab's area.

`character` (optional) makes the entity walk with collisions: `speed` and `run_speed` (tiles per second), `acceleration` (tiles per second squared; 0 is instant), `collision` ([w, h]), `blocked_by` (`water`, `solid`, `buildings`: true or false) and `face_movement`. One character per scene may have `player`: `actions` (`up`, `down`, `left`, `right`, `run`; action names from `assets/actions.json`, where `move_*` and `run` default to WASD, the arrows and Shift), `input` (moves with the keys; false leaves movement to its script), camera `zoom` (pixels per tile), `smoothing` (seconds) and `dead_zone` (tiles), and `resume` (a saved game starts where the player was). Other characters are NPCs, moved by their scripts. Scenes from before characters mark the player by naming an entity "Player"; that still works.

`prefab` (optional) links a placed copy to `prefabs/<name>.json`; the copy's `visual`, `light` and `script` are the prefab's, stored in the scene too so games need not resolve prefabs. `editor` (optional) holds editor-only `hidden` and `locked` flags that games ignore.

`decorations` (optional, at most 16,384; written only when there are any) are sprites placed on the scene that are not entities: flowers, tufts of grass, pebbles, decals. Each has `material`, `chunk` and `position` (as an entity's), `size` ([w, h], 0.05–16) and optional `angle` (radians) and `standing` (true: drawn with the entities and sorted with them under `sort_by_y`; otherwise flat on the ground, under terrain objects and entities). Games draw the decorations of the chunks in view; scripts cannot reach them.

`atmosphere` (optional; each field optional, written only when it differs from its default) is the scene's light and air: `ambient` (daytime light everywhere, [r, g, b] 0–4), `night` (light everywhere at midnight), `background`, `haze` and `haze_amount` (0–1, haze towards the view's edges), `hour` (0–24; default 12) and `day_length` (real seconds per day, 10–86,400; 0, the default, keeps the scene at `hour`). A game has one clock: it starts at the `hour` of the first scene with a day and runs at the `day_length` of the latest one, also through scenes without a day, which show their own `hour`. Daylight is full from 09:00 to 15:00 and gone from 21:00 to 03:00, easing between; towards night the ambient light moves to `night` and the background and haze darken to about a third. The defaults at noon are the renderer's own lighting. The time of day is saved with games (see [save format](save-format.md#game-state)).

### <name>.paint

Terrain painted in the editor's Scene view, saved beside the scene and only when something is painted. Each painted tile replaces some of what the scene's terrain generates there; the rest stays generated. Little-endian binary: u32 magic `0x544e5053` ("SPNT"), u32 version 1, u8 name count and the material names the tiles use (u8 length, bytes), u32 chunk count, then per chunk in (x, y) order: i64 x, i64 y, u16 tile count (1–1024) and, in index order (y × 32 + x), u16 index, u8 mask, and only the masked fields: ground (bit 1, u8 name index), object (bit 2, u8: 0 none, else name index + 1), height (bit 4, f32, at most 16 either way) and solid (bit 8, u8 0 or 1). Malformed, unordered or trailing data is rejected. Games apply paint while generating each chunk, so it costs nothing per frame; it is part of the terrain's generator version, so changing the paint starts new saved worlds just as changing the terrain does.

An entity with no components is a named marker, such as a spawn point. Material references are checked against `assets/materials.json`; the editor will not save a scene with problems.

A C++ game uses a project's assets by setting `Game::project_assets` to the folder; the engine registers them before calling the game's own `materials`, `actions` and `effects` callbacks, which may add more.

## Exported games

An exported app carries the project as `Contents/Resources/game.seedpack`: the shared LZ4 envelope (checksummed; see [binary formats](save-format.md)) around u32 magic `0x4b415053` ("SPAK"), u32 version 1, u32 file count, then, in path order, u16 path length, path, u32 size and bytes for `project.seed.json`, `assets/*.json`, `scenes/*.json` (all JSON re-written without whitespace), `scenes/*.paint`, `assets/sounds/*` and `assets/music/*` (as imported) and `scripts/*.lua`. Paths are relative and limited to letters, digits, `_`, `-` and `.` with `/` separators; unsafe, duplicate or out-of-order paths and trailing bytes are rejected. `seed_player` runs `game.seedpack` when it sits in the base folder SDL reports, which is `Contents/Resources` inside an app bundle.
