-- A crab: chases the player when near, else goes home. Touching it costs a heart, then the player
-- is safe for a moment and knocked back.
local home_x, home_y = self:position()
local touching = false

local function distance(ax, ay, bx, by)
  return math.sqrt((ax - bx) ^ 2 + (ay - by) ^ 2)
end

local function bite(player)
  local d = game.data
  d.health = d.health - 1
  d.safe_until = game.time() + 1.5
  sound.play("hurt")
  local px, py = player:position()
  local x, y = self:position()
  -- Away from the crab; from right on top of it, away from the crab's home.
  if distance(px, py, x, y) < 0.05 then x, y = home_x, home_y end
  local away = math.max(distance(px, py, x, y), 0.01)
  player:set_position(px + (px - x) / away * 1.5, py + (py - y) / away * 1.5)
  print("Ouch! Health " .. d.health .. ".")
end

function update(dt)
  if game.paused() then return end
  local player = game.player()
  local px, py = player:position()
  local x, y = self:position()
  if touching and game.time() >= game.data.safe_until and game.data.health > 0 then bite(player) end
  if distance(px, py, x, y) < 5 then
    self:walk_to(px, py)
  elseif distance(home_x, home_y, x, y) > 0.2 then
    self:walk_to(home_x, home_y)
  end
end

function on_touch(other)
  if other == game.player() then touching = true end
end

function on_leave(other)
  if other == game.player() then touching = false end
end
