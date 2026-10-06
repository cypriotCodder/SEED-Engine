-- The gem in the cave: it glows until found.
local t = 0

function start()
  if game.data.gem then self:destroy() end
end

function update(dt)
  t = t + dt
  self:set_light{intensity = 2 + math.sin(t * 3)}
end

function on_touch(other)
  if other ~= game.player() then return end
  game.data.gem = true
  local x, y = self:position()
  sound.play("gem")
  particles.burst("sparkle", x, y)
  print("Found the gem!")
  self:destroy()
end
