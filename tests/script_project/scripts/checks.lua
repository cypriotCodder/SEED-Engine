-- Exercises the script API. Any failed assert is a script error, which fails the test run.
local box = world.find("Box")
assert(box and box:name() == "Box", "world.find finds scene entities by name")

local steps, spark = 0, nil

function start()
  local x, y = self:position()
  assert(math.abs(x - 2.5) < 1e-6 and math.abs(y - 1.5) < 1e-6, "self is placed where the scene put it")

  spark = world.spawn{name = "Spark", x = 4, y = 4, material = "stone", size = 0.5, script = "spinner.lua"}
  assert(world.find("Spark") == spark, "spawned entities can be found")
  local w, h = spark:size()
  assert(w == 0.5 and h == 0.5, "spawn size")

  local tile = world.tile(0.5, 0.5)
  assert(tile and tile.elevation > 0 and type(tile.material) == "string", "the tile under the player is land")
  assert(world.tile(1e6, 1e6) == nil, "tiles far away are not loaded")

  assert(input.held("move_up") == false, "no keys are held in automated runs")
  local ok, message = pcall(input.held, "fly")
  assert(not ok and message:find("Unknown action"), "an unknown action is a clear error")

  box:destroy()
  assert(not box:alive(), "destroy removes the entity")
  assert(not pcall(function() return box:position() end), "a destroyed entity cannot be used")
  assert(world.find("Box") == nil, "a destroyed entity is no longer found")
  assert(not pcall(function() world.find("Player"):destroy() end), "the camera's entity cannot be destroyed")

  assert(dofile == nil and loadfile == nil and load == nil and io == nil and os == nil and require == nil,
         "scripts are sandboxed")
  assert(spin_count == nil, "another script's globals are its own")
end

function update(dt)
  steps = steps + 1
  local before = self:position()
  self:move(3 * dt, 0) -- Walk east across open meadow.
  assert(self:position() > before, "move walks")
  if steps == 30 then
    assert(spark:angle() > 0.3, "the spawned entity's own script runs")
    assert(game.time() > 0.45, "game time advances")
    print("Script checks passed.")
  end
end
