-- A coin: taken once for good. game.data.taken remembers it across scenes and saves.
local key = game.scene() .. "/" .. self:name()

function start()
  if game.data.taken and game.data.taken[key] then self:destroy() end
end

function on_touch(other)
  if other ~= game.player() then return end
  local d = game.data
  d.taken[key] = true
  d.score = d.score + 1
  local x, y = self:position()
  sound.play("coin")
  particles.burst("sparkle", x, y)
  print(string.format("Coin %d/%d.", d.score, d.coins))
  self:destroy()
end
