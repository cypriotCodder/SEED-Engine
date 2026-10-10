-- Clears a patch of ground, builds a wall at x = 8 with one gap at y = 9, then walks round it to
-- (10.5, 5.5) with e:go_to. world.find_path must avoid the wall and refuse a blocked goal.
local steps = 0

function start()
  for y = 0, 16 do
    for x = 2, 13 do
      assert(world.set_tile{x = x + 0.5, y = y + 0.5, ground = "grass", object = false, solid = false,
                            elevation = 0.3}, "the test area is loaded")
    end
  end
  for y = 0, 12 do
    if y ~= 9 then world.set_tile{x = 8.5, y = y + 0.5, solid = true} end
  end
  assert(world.find_path(5.5, 5.5, 8.5, 5.5) == nil, "a blocked goal has no path")
  local path = world.find_path(5.5, 5.5, 10.5, 5.5)
  assert(path and #path >= 2, "the path turns to pass the wall")
  local last = path[#path]
  assert(last[1] == 10.5 and last[2] == 5.5, "the path ends where asked")
  for _, p in ipairs(path) do
    assert(not world.tile(p[1], p[2]).solid, "no point of the path is in the wall")
  end
  assert(self:go_to(10.5, 5.5), "go_to finds the way")
  assert(self:moving(), "and walks it")
end

function update(dt)
  steps = steps + 1
  if steps == 300 then
    local x, y = self:position()
    assert(not self:moving(), "the walker has arrived")
    assert(math.abs(x - 10.5) < 0.01 and math.abs(y - 5.5) < 0.01, "at the goal: " .. x .. ", " .. y)
    print("Navigation checks passed.")
  end
end
