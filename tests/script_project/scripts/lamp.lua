-- Checks the time of day and lights. The scene starts at 20:00 with a 60-second day, and this
-- entity has a flickering light that is lit only at night.
local steps = 0

function start()
  assert(math.abs(atmosphere.hour() - 20) < 1e-6, "the scene starts at its hour")
  local l = self:light()
  assert(l and l.radius == 5 and l.night_only and math.abs(l.flicker - 0.5) < 1e-6, "light() reads the light")
  self:set_light{intensity = 3, color = {0.2, 0.4, 1}}
  l = self:light()
  assert(l.intensity == 3 and math.abs(l.color[3] - 1) < 1e-6 and l.radius == 5, "set_light changes only what it is given")
  local box = world.spawn{name = "Torch", x = 6.5, y = 8.5}
  assert(box:light() == nil, "entities without a light have none")
  box:set_light{radius = 3}
  assert(box:light().radius == 3 and box:light().intensity == 2, "set_light gives an entity a light")
  assert(not pcall(self.set_light, self, {radius = 0}), "light values have limits")
  assert(not pcall(atmosphere.set_hour, 25), "the hour has limits")
end

function update(dt)
  steps = steps + 1
  if steps == 31 then
    -- 30 steps of 1/60 s each: half a second of a 60-second day is 0.2 hours.
    assert(math.abs(atmosphere.hour() - 20.2) < 1e-3, "time passes by the day length: " .. atmosphere.hour())
    atmosphere.set_hour(0)
    assert(atmosphere.daylight() == 0, "midnight is dark")
    atmosphere.set_hour(12)
    assert(atmosphere.daylight() == 1, "noon is full daylight")
    print("Atmosphere checks passed.")
  end
end
