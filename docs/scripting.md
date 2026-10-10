# Gameplay scripts

Projects add behaviour with Lua 5.4 scripts in their `scripts/` folder. Attach one to an entity with the Inspector's **Script** component (**Add Component → Script**, or **New script**); **Edit** opens it in your code editor. Scripts run when you press **Play** and in shipped games.

```lua
-- scripts/player.lua
local speed = 6

function update(dt)
  local x = input.axis("move_left", "move_right")
  local y = input.axis("move_down", "move_up")
  self:move(x * speed * dt, y * speed * dt)
  if input.pressed("attack") then
    local px, py = self:position()
    particles.burst("chips", px, py)
    sound.play("impact")
  end
end
```

## How scripts run

Each attached script runs in its own environment: its globals and `local`s belong to that entity alone, and `self` is the entity. The file's top level runs when the game starts, after every scene entity exists. `start()`, if defined, runs once before the first update; `update(dt)`, if defined, runs every fixed step (60 per second; `dt` is in seconds). `on_touch(other)` and `on_leave(other)`, if defined, run when the entity's box starts or stops overlapping another entity's: a character's box is its collision box, anything else's is its visual's size (unrotated); entities with neither have no box. On an entity with an Area, `on_enter(other)` and `on_exit(other)`, if defined, run when a character's centre comes into or leaves the area's rectangle (centred on the entity, unrotated), after the step's touches; a character inside when the game starts enters on the first step. On a character, `on_surface(material)`, if defined, runs when it steps onto a different ground material, and once when it first stands on loaded ground.

The player moves with the keys unless its Character's "Moves with the keys" is off; its script can add to that, or take over with `walk` and `walk_to`. NPCs move only through their scripts.

Positions are global tile coordinates: one unit per tile, `x` to the right and `y` up, as the editor's Scene view and Inspector show them.

## API

**Entities** (`self`, and anything `world.find` or `world.spawn` returns):

| Method | |
| --- | --- |
| `e:position()` | `x, y` |
| `e:set_position(x, y)` | Jump there (no collision). |
| `e:move(dx, dy)` | Walk, sliding along water, solid tiles and buildings; returns `false` if blocked. |
| `e:angle()`, `e:set_angle(radians)` | Rotation, counter-clockwise. |
| `e:size()`, `e:set_size(w [, h])` | Visual size; the entity needs a visual. |
| `e:set_material(name)` | Change its look to another material. |
| `e:frame()` | For an animated material: the frame showing (from 0) and whether the animation is playing. |
| `e:set_frame(n)` | Show frame `n` (from 0) and hold it; a character stops animating by itself. |
| `e:animate()` | Play the animation again (a character: while it walks). |
| `e:name()` | The name given in the scene or at spawn. |
| `e:alive()` | `false` once destroyed. |
| `e:destroy()` | Remove it (not the entity the camera follows). |

**Characters** (entities with a Character component: the player and NPCs) walk by the rules set in the Inspector (speed, acceleration, collision, what blocks them):

| Method | |
| --- | --- |
| `e:walk(x, y [, run])` | Keep walking in this direction (length up to 1) until told otherwise. |
| `e:walk_to(x, y [, run])` | Walk to a point and stop there; returns `true` once there. |
| `e:follow(route [, loop [, run]])` | Walk through a route's points in turn: `route` is a list of `{x, y}` points (1 to 4,096) or an entity with a Path (whose Loop setting applies unless `loop` is given). With `loop` it goes round for ever. A character that cannot reach a point keeps trying; `walk`, `walk_to`, `stop` or another `follow` end the route. `e:moving()` stays true until it ends. |
| `e:go_to(x, y [, run])` | Find a way round walls, water and trees to a point (`world.find_path`) and follow it; returns `false`, without moving, when there is none. |
| `e:stop()` | Stop at once, ending any route. |
| `e:moving()` | Whether it is walking or heading somewhere. |
| `e:speed()`, `e:set_speed(walk [, run])` | Speeds in tiles per second. |

Entities compare with `==`. Using a destroyed entity is an error, except `alive()`.

**Modules**:

| Function | |
| --- | --- |
| `input.held(action)`, `input.pressed(action)`, `input.released(action)` | Actions from the Input panel; `pressed` and `released` are true for one frame. |
| `input.pointer()` | The mouse's position in the world: `x, y`. |
| `input.screen_pointer()` | The mouse's position on screen in logical pixels from the top-left: `x, y`. |
| `input.axis(negative, positive)` | -1, 0 or 1 from two actions. |
| `world.near(x, y, radius [, name])` | The entities within `radius` tiles (at most 256), nearest first, optionally only those with that name. |
| `world.find(name)` | The first entity with that name, or `nil`. |
| `world.spawn{x=, y=, name=, material=, size=, angle=, script=}` | A new entity; `material` gives it a visual, `size` is a number or `{w, h}`, `script` attaches a script. |
| `world.spawn{prefab=, x=, y=, name=, angle=}` | A copy of a prefab, with its look, light and script. |
| `world.tile(x, y)` | `{material=, object=, solid=, elevation=}` for a loaded tile (`object` is `nil` when nothing stands there), or `nil` far from the camera. Water has elevation below 0. |
| `atmosphere.hour()` / `atmosphere.set_hour(h)` | The game's time of day, 0 to 24. One clock serves the whole game and is saved with it: it starts at the hour of the first scene with a day and runs at the day length of the latest one, in every scene. Before any scene with a day, each scene starts at its own hour. |
| `atmosphere.daylight()` | How bright the scene is now: 1 from 09:00 to 15:00, 0 from 21:00 to 03:00. A scene without a day keeps its own hour's light while the game's clock runs on. |
| `e:light()` | `{color={r,g,b}, radius=, intensity=, height=, flicker=, night_only=}`, or `nil` without a light. |
| `e:set_light{...}` | Changes the fields given (same names and limits as the Light component); an entity without a light gets one. |
| `world.find_path(x0, y0, x1, y1)` | The points to walk through from one point to another, as `{{x, y}, ...}` (the turns, then the destination itself), or `nil` if the destination's tile is blocked or no way is found among loaded tiles. It steps between tile centres, diagonally only where both tiles beside the step are open, avoiding what blocks walking by default (water, solid tiles and unloaded ground). It searches at most 16,384 tiles; the ends may be at most 512 tiles apart. |
| `world.path(e)` | The points of an entity's Path, as `{{x, y}, ...}`, and whether it loops; `nil` for an entity without one. |
| `world.set_tile{x=, y=, ground=, object=, solid=, elevation=}` | Changes a loaded tile and returns `true`, or returns `false` far from the camera, changing nothing. Fields left out keep their value; `object = false` (or `""`) removes the object; `elevation` is -1,000 to 1,000 (below 0 is water). An unknown material is an error. The tile is saved whole with the world, so the change lasts. |
| `world.surface(x, y)` | `{material=, speed=, tags={...}}`: the ground material at a loaded tile, how fast characters walk on it (a multiple of their own speed, applied by the engine) and its tags from the Materials panel, or `nil` far from the camera. |
| `sound.play(name)` | A sound from the Sounds panel, synthesized or recorded. |
| `music.play(name [, loop])` | Plays `assets/music/<name>.ogg`, replacing any music; loops unless `loop` is `false`. An unknown or broken file is an error. |
| `music.stop()`, `music.playing()` | Stops the music; whether music is playing (a song that has not looped ends by itself). |
| `music.set_volume(v)` | Music volume, 0 to 1 (default 0.6). |
| `particles.burst(name, x, y)` | A particle style from the Particles panel. |
| `camera.follow(entity)` | The camera, and world streaming, follow this entity. |
| `camera.zoom()`, `camera.set_zoom(pixels_per_tile)` | The camera's zoom, from 2 to 512. |
| `game.player()` | The player entity. |
| `game.load_scene(name [, spawn])` | Goes to another scene once this step is over: this scene's world is saved and its entities and scripts end; the other scene's world, entities and scripts take their place. The player appears at the entity named `spawn` there, else where it last left that scene (if its Character resumes), else at its scene position. |
| `game.scene()` | The name of the scene that is playing. |
| `game.data` | A table every script shares that lasts across scene changes and is saved with the game, for things such as score, inventory or which coins are taken. Booleans, numbers, strings and tables of them are saved; anything else (functions, entities) is left out of the save and reported as a script error. At most 32 tables deep and 4 MiB; a table holding itself cannot be saved. Loaded before any script runs. |
| `game.saved_scene()` | The scene the loaded save was made in, or nil for a new game. A loaded game always starts in the start scene; going on to the saved scene (`game.load_scene(game.saved_scene())`), showing a title screen first or anything else is the game's choice. |
| `game.set_paused(on)`, `game.paused()` | A paused game keeps running scripts (for title, pause and game-over screens) while characters, touches, surfaces and the time of day stand still. |
| `ui.rect(x, y, w, h [, {r,g,b,a}])` | A filled rectangle over the game, in logical pixels from the top-left; colours 0 to 1. |
| `ui.text(x, y, text [, scale [, {r,g,b,a}]])` | Text in the built-in pixel font, 6 x 8 pixels per character at scale 1 (default 2, up to 16). |
| `ui.text_width(text [, scale])`, `ui.size()` | The width text will take; the screen's width and height. |
| `game.time()` | Seconds of game time since the game started. |

Lua's `string`, `table`, `math` and `utf8` libraries and `print` are available; `print` output appears in the editor's Console during Play.

What a step draws with `ui` stays on screen until the next step, which starts from nothing; draw the whole interface in every `update`. At most 4,096 items per step.

## Limits and errors

Scripts cannot read or write files, reach the OS, load modules or load code at run time. A call that runs longer than 0.25 s (usually an endless loop) is stopped, and all scripts together may use 64 MiB. A script that fails, whether with a syntax error, a runtime error or a stopped call, is reported with its file, line and a stack trace, then switched off while the rest of the game carries on. In automated runs (`--smoke`) any script error fails the run.

Saves keep `game.data`, the time of day, the scene playing, the player's position in each scene, terrain edits and buildings. Entities created by scripts, other changes scripts make to entities, and scripts' own variables are not saved: a scene starts from its scene file each time, so record what should last (a coin taken, a door opened) in `game.data` and apply it in the entity's `start()`, for example `if game.data.taken[self:name()] then self:destroy() end`.
