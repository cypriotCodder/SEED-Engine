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

Each attached script runs in its own environment: its globals and `local`s belong to that entity alone, and `self` is the entity. The file's top level runs when the game starts, after every scene entity exists. `start()`, if defined, runs once before the first update; `update(dt)`, if defined, runs every fixed step (60 per second; `dt` is in seconds). On a character, `on_surface(material)`, if defined, runs when it steps onto a different ground material, and once when it first stands on loaded ground.

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
| `e:name()` | The name given in the scene or at spawn. |
| `e:alive()` | `false` once destroyed. |
| `e:destroy()` | Remove it (not the entity the camera follows). |

**Characters** (entities with a Character component: the player and NPCs) walk by the rules set in the Inspector (speed, acceleration, collision, what blocks them):

| Method | |
| --- | --- |
| `e:walk(x, y [, run])` | Keep walking in this direction (length up to 1) until told otherwise. |
| `e:walk_to(x, y [, run])` | Walk to a point and stop there; returns `true` once there. |
| `e:stop()` | Stop at once. |
| `e:moving()` | Whether it is walking or heading somewhere. |
| `e:speed()`, `e:set_speed(walk [, run])` | Speeds in tiles per second. |

Entities compare with `==`. Using a destroyed entity is an error, except `alive()`.

**Modules**:

| Function | |
| --- | --- |
| `input.held(action)`, `input.pressed(action)`, `input.released(action)` | Actions from the Input panel; `pressed` and `released` are true for one frame. |
| `input.axis(negative, positive)` | -1, 0 or 1 from two actions. |
| `world.find(name)` | The first entity with that name, or `nil`. |
| `world.spawn{x=, y=, name=, material=, size=, angle=, script=}` | A new entity; `material` gives it a visual, `size` is a number or `{w, h}`, `script` attaches a script. |
| `world.spawn{prefab=, x=, y=, name=, angle=}` | A copy of a prefab, with its look, light and script. |
| `world.tile(x, y)` | `{material=, object=, solid=, elevation=}` for a loaded tile (`object` is `nil` when nothing stands there), or `nil` far from the camera. Water has elevation below 0. |
| `atmosphere.hour()` / `atmosphere.set_hour(h)` | The scene's time of day, 0 to 24; it moves on by the atmosphere's day length. |
| `atmosphere.daylight()` | How bright the day is now: 1 from 09:00 to 15:00, 0 from 21:00 to 03:00. |
| `e:light()` | `{color={r,g,b}, radius=, intensity=, height=, flicker=, night_only=}`, or `nil` without a light. |
| `e:set_light{...}` | Changes the fields given (same names and limits as the Light component); an entity without a light gets one. |
| `world.surface(x, y)` | `{material=, speed=, tags={...}}`: the ground material at a loaded tile, how fast characters walk on it (a multiple of their own speed, applied by the engine) and its tags from the Materials panel, or `nil` far from the camera. |
| `sound.play(name)` | A sound from the Sounds panel. |
| `particles.burst(name, x, y)` | A particle style from the Particles panel. |
| `camera.follow(entity)` | The camera, and world streaming, follow this entity. |
| `camera.zoom()`, `camera.set_zoom(pixels_per_tile)` | The camera's zoom, from 2 to 512. |
| `game.player()` | The player entity. |
| `game.time()` | Seconds of game time since the game started. |

Lua's `string`, `table`, `math` and `utf8` libraries and `print` are available; `print` output appears in the editor's Console during Play.

## Limits and errors

Scripts cannot read or write files, reach the OS, load modules or load code at run time. A call that runs longer than 0.25 s (usually an endless loop) is stopped, and all scripts together may use 64 MiB. A script that fails, whether with a syntax error, a runtime error or a stopped call, is reported with its file, line and a stack trace, then switched off while the rest of the game carries on. In automated runs (`--smoke`) any script error fails the run.

Entities created by scripts, and changes scripts make, are not saved; saves keep the player's position, terrain edits and buildings.
