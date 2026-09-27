-- Turns its entity one radian per second.
spin_count = 0

function update(dt)
  spin_count = spin_count + 1
  self:set_angle(self:angle() + dt)
end
