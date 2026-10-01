-- Walks east across ground painted in the editor: a strip of test_mud (speed 0.5, tags "sticky
-- slow") from x 16 to 63 at y 20, and a blocked water tile at (25, 22).
local surfaces, steps, start_x = {}, 0, 0

function start()
  start_x = self:position()
  self:walk(1, 0)
end

function on_surface(material)
  surfaces[#surfaces + 1] = material
end

function update(dt)
  steps = steps + 1
  if steps == 20 then
    -- Steps 1 to 19 have moved, each at half of 4 tiles per second.
    local x = self:position()
    assert(math.abs((x - start_x) - 19 * 2 * dt) < 1e-3, "painted mud halves walking speed: " .. (x - start_x))
    assert(#surfaces == 1 and surfaces[1] == "test_mud", "on_surface reports the painted ground once")
    local ground = world.surface(x, 20.5)
    assert(ground.material == "test_mud" and math.abs(ground.speed - 0.5) < 1e-6, "world.surface gives the speed")
    assert(#ground.tags == 2 and ground.tags[1] == "sticky" and ground.tags[2] == "slow", "and the tags")
    local tile = world.tile(25.5, 22.5)
    assert(tile.solid and tile.elevation < 0, "painted blocking and height apply")
    print("Surface checks passed.")
  end
end
