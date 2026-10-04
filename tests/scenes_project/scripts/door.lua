-- In the main scene: goes to the cave, and checks the way back.
local steps = 0
function update(dt)
  steps = steps + 1
  if steps == 3 then
    if game.data.visits == nil then
      assert(game.scene() == "main", "the game starts in main")
      game.data.visits = 1
      game.player():set_position(1.25, 0.75) -- Where the player leaves from.
      game.load_scene("cave", "Entry")
    else
      assert(game.data.visits == 2, "game.data outlives scene changes")
      local x, y = game.player():position()
      assert(math.abs(x - 1.25) < 1e-3 and math.abs(y - 0.75) < 1e-3,
             "the player comes back where it left: " .. x .. ", " .. y)
      print("Scene checks passed.")
    end
  end
end
