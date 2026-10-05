-- In the cave: a scene without a day, which shows its own hour while the game's clock runs on.
local steps = 0

function start()
  assert(atmosphere.daylight() == 0, "the cave shows its own hour, 02:00")
  if game.saved_scene() == nil then
    assert(math.abs(atmosphere.hour() - 8) < 0.01, "the game's clock came along: " .. atmosphere.hour())
    game.data.visited_cave = true
    game.player():set_position(4.25, 3.75)
  else
    local x, y = game.player():position()
    assert(math.abs(x - 4.25) < 1e-3 and math.abs(y - 3.75) < 1e-3, "the player is where the save left it")
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
