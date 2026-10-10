-- In the cave: a scene without a day, which shows its own hour while the game's clock runs on.
local steps = 0

function start()
  assert(atmosphere.daylight() == 0, "the cave shows its own hour, 02:00")
  if game.saved_scene() == nil then
    assert(math.abs(atmosphere.hour() - 8) < 0.01, "the game's clock came along: " .. atmosphere.hour())
    game.data.visited_cave = true
    game.player():set_position(4.25, 3.75)
    -- A tile replaced by a script is saved with the world.
    assert(world.tile(6.5, 3.5), "the tile is loaded")
    assert(world.set_tile{x = 6.5, y = 3.5, ground = "stone", object = "tree", solid = true, elevation = 0.5},
           "set_tile changes a loaded tile")
    local t = world.tile(6.5, 3.5)
    assert(t.material == "stone" and t.object == "tree" and t.solid and t.elevation == 0.5, "at once")
    assert(world.set_tile{x = 7.5, y = 3.5, ground = "mud", object = false, solid = false}, "a second tile")
    assert(not pcall(world.set_tile, {x = 8.5, y = 3.5, ground = "lava"}), "unknown materials are errors")
    assert(not pcall(world.set_tile, {x = 8.5, y = 3.5, elevation = 1 / 0}), "elevation has limits")
    assert(world.set_tile{x = 1e6, y = 1e6, ground = "mud"} == false, "a tile far away is not changed")
  else
    local x, y = game.player():position()
    assert(math.abs(x - 4.25) < 1e-3 and math.abs(y - 3.75) < 1e-3, "the player is where the save left it")
    local t = world.tile(6.5, 3.5)
    assert(t.material == "stone" and t.object == "tree" and t.solid and t.elevation == 0.5,
           "a replaced tile comes back with the save")
    local u = world.tile(7.5, 3.5)
    assert(u.material == "mud" and u.object == nil and not u.solid, "and so does the second")
    print("State restored.")
  end
end

function update(dt)
  steps = steps + 1
  if steps == 10 and game.saved_scene() == nil then
    assert(atmosphere.hour() > 8.05, "the game's clock runs in a scene without a day")
    assert(atmosphere.daylight() == 0, "but the cave's light stays")
    print("State saved.")
  end
end
