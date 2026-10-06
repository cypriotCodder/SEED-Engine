-- In every scene that plays: the HUD, pausing, and the end of the game, won or lost.
local state = "playing" -- or "paused", "over", "won"

function start()
  if not game.data.health then -- Started from the editor's Play, without the title: a new game.
    game.data = {score = 0, coins = 5, health = 3, taken = {}}
  end
  game.data.scene = game.scene()
  game.data.safe_until = 0
  game.set_paused(false)
  print("Entered the " .. game.scene() .. ".")
end

local function panel(title, line)
  local w, h = ui.size()
  ui.rect(0, 0, w, h, {0, 0, 0, 0.5})
  local tw = ui.text_width(title, 6)
  ui.text((w - tw) / 2, h * 0.35, title, 6, {1, 0.9, 0.4, 1})
  ui.text((w - ui.text_width(line, 2)) / 2, h * 0.35 + 70, line, 2, {1, 1, 1, 0.9})
end

local function hud()
  local d = game.data
  ui.rect(12, 12, 250, 74, {0, 0, 0, 0.45})
  ui.text(24, 22, string.format("COINS %d/%d", d.score, d.coins), 2, {1, 0.85, 0.3, 1})
  ui.text(24, 52, "HEALTH", 2, {1, 1, 1, 0.9})
  for i = 1, 3 do
    local c = i <= d.health and {0.9, 0.2, 0.25, 1} or {0.3, 0.3, 0.3, 0.8}
    ui.rect(104 + (i - 1) * 22, 52, 16, 16, c)
  end
  if d.gem then ui.text(180, 52, "GEM", 2, {0.5, 0.9, 1, 1}) end
  local hour = atmosphere.hour()
  ui.text(180, 22, string.format("%02d:%02d", math.floor(hour), math.floor(hour % 1 * 60)), 2,
          {0.8, 0.85, 1, 0.9})
end

function update(dt)
  local d = game.data
  if state == "playing" then
    if d.health <= 0 then
      state = "over"
      game.set_paused(true)
      print("Game over.")
    elseif d.score >= d.coins and d.gem then
      state = "won"
      game.set_paused(true)
      print("You win!")
    elseif input.pressed("pause") then
      state = "paused"
      game.set_paused(true)
    end
  elseif state == "paused" then
    if input.pressed("pause") or input.pressed("confirm") then
      state = "playing"
      game.set_paused(false)
    end
  elseif input.pressed("confirm") then
    game.data = {} -- Over: the title offers a new game.
    game.load_scene("title")
    return
  end
  hud()
  if state == "paused" then panel("PAUSED", "P to go on") end
  if state == "over" then panel("GAME OVER", "ENTER to try again") end
  if state == "won" then panel("YOU WIN", "Every coin and the gem. ENTER for the title") end
end
