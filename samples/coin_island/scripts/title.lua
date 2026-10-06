-- The title screen: the island behind, the world paused, the music starting. Where a saved game
-- goes on is this script's choice: game.lua records the last scene played in game.data.scene.
local t = 0

local function new_game()
  game.data = {score = 0, coins = 5, health = 3, taken = {}}
  game.load_scene("island", "Start")
end

local function centre(y, text, scale, color)
  local w = ui.size()
  ui.text((w - ui.text_width(text, scale)) / 2, y, text, scale, color)
end

function start()
  game.set_paused(true)
  music.play("theme")
  if game.data.scene then
    print(string.format("Title: a saved game in the %s, %d coins, health %d.", game.data.scene,
                        game.data.score, game.data.health))
  else
    print("Title: no saved game.")
  end
end

function update(dt)
  t = t + dt
  local w, h = ui.size()
  ui.rect(0, 0, w, h, {0.02, 0.04, 0.08, 0.55})
  centre(h * 0.25, "COIN ISLAND", 8, {1, 0.85, 0.3, 1})
  centre(h * 0.25 + 84, "Collect every coin and find the gem in the cave.", 2, {0.9, 0.9, 1, 0.9})
  local blink = math.floor(t * 2) % 2 == 0 and 1 or 0.55
  if game.data.scene then
    centre(h * 0.58, "ENTER  continue", 3, {1, 1, 1, blink})
    centre(h * 0.58 + 40, "N  new game", 3, {1, 1, 1, 0.8})
    if input.pressed("confirm") then
      game.load_scene(game.data.scene) -- The player comes back where it was.
    elseif input.pressed("new_game") then
      new_game()
    end
  else
    centre(h * 0.58, "ENTER  start", 3, {1, 1, 1, blink})
    if input.pressed("confirm") then new_game() end
  end
  centre(h - 40, "Arrows or WASD to walk, Shift to run, P to pause", 2, {0.8, 0.8, 0.8, 0.7})
end
