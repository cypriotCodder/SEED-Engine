-- Run twice on one save. The first run fills game.data and goes to the cave, where the game is
-- saved on quitting; the second finds it all again and chooses to go back to the cave itself.
local saved = game.saved_scene()

function start()
  if saved == nil then
    assert(next(game.data) == nil, "a new game starts with empty game.data")
    assert(math.abs(atmosphere.hour() - 8) < 1e-6, "the clock starts at the start scene's hour")
    game.data.coins = 42
    game.data.speed = 2.5
    game.data.name = "Ayla"
    game.data.flags = {door = true, [3] = "third"}
    game.load_scene("cave", "Entry")
  else
    assert(saved == "cave", "the save remembers its scene: " .. tostring(saved))
    assert(game.data.coins == 42 and math.type(game.data.coins) == "integer", "game.data integers come back")
    assert(game.data.speed == 2.5 and game.data.name == "Ayla", "game.data numbers and strings come back")
    assert(game.data.flags.door == true and game.data.flags[3] == "third", "nested tables come back")
    assert(game.data.visited_cave == true, "changes made in another scene come back")
    -- The first run saved after nearly a second of a 60-second day: 0.4 hours on from 08:00.
    local hour = atmosphere.hour()
    assert(hour > 8.3 and hour < 8.5, "the time of day comes back: " .. hour)
    assert(game.scene() == "main", "a loaded game starts in the start scene; going on is the game's choice")
    game.load_scene(saved)
  end
end
