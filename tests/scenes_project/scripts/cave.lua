-- In the cave: checks arriving, then goes back to main.
assert(game.scene() == "cave", "game.scene() names the cave")
assert(game.data.visits == 1, "game.data came along")
assert(world.find("Door") == nil, "the scene left behind is gone")
local x, y = game.player():position()
assert(math.abs(x - 5.5) < 1e-3 and math.abs(y - 6.5) < 1e-3, "the player arrives at the spawn entity")
local steps = 0
function update(dt)
  steps = steps + 1
  if steps == 2 then
    assert(world.tile(5.5, 6.5).material == "stone", "the cave's own terrain is loaded")
    game.data.visits = 2
    game.load_scene("main")
  end
end
