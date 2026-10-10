-- Follows the scene's Route path, (5.5, 14.5) to (8.5, 14.5) to (8.5, 15.5), and stops at its end.
local steps = 0

function update(dt)
  steps = steps + 1
  if steps == 2 then
    local route = world.find("Route")
    local points, loop = world.path(route)
    assert(#points == 3 and points[2][1] == 8.5 and points[3][2] == 15.5 and not loop, "world.path reads it")
    assert(world.path(self) == nil, "entities without a path have none")
    self:follow(route)
  end
  if steps == 300 then
    local x, y = self:position()
    assert(not self:moving() and math.abs(x - 8.5) < 0.01 and math.abs(y - 15.5) < 0.01,
           "the guard walked the path to its end: " .. x .. ", " .. y)
    print("Path checks passed.")
  end
end
