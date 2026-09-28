-- Checks characters and the camera against the movement rules. Distances are compared with the
-- exact step-by-step result: each fixed step (1/60 s) sets the velocity, then moves.
local walker, starter, goer = world.find("Walker"), world.find("Starter"), world.find("Goer")
local steps, walker_x, starter_x = 0, 0, 0

function start()
  assert(game.player() == world.find("Player"), "game.player() is the player")
  assert(math.abs(camera.zoom() - 30) < 1e-6, "the camera starts at the player's zoom")
  camera.set_zoom(24)
  assert(math.abs(camera.zoom() - 24) < 1e-6, "camera.set_zoom changes the zoom")
  assert(not pcall(camera.set_zoom, 1), "zoom has limits")
  assert(not pcall(function() self:walk(1, 0) end), "only characters can walk")
  local walking, running = walker:speed()
  assert(walking == 5 and running == 8, "speed() gives the walking and running speeds")
  walker_x = walker:position()
  starter_x = starter:position()
  walker:walk(1, 0)
  starter:walk(1, 0)
  goer:walk_to(15.5, 6.5)
  assert(goer:moving(), "walk_to starts moving")
end

function update(dt)
  steps = steps + 1
  if steps == 30 then
    -- Steps 1 to 29 have moved so far; this update runs before step 30 moves.
    local moved = 29
    local expected_walker = moved * 5 * dt
    local expected_starter, speed = 0, 0
    for _ = 1, moved do
      speed = math.min(5, speed + 10 * dt)
      expected_starter = expected_starter + speed * dt
    end
    local w = walker:position() - walker_x
    local s = starter:position() - starter_x
    assert(math.abs(w - expected_walker) < 1e-3, "walks at full speed at once: " .. w)
    assert(math.abs(s - expected_starter) < 1e-3, "acceleration builds speed up: " .. s)
    assert(math.abs(walker:angle()) < 1e-6, "face_movement turns it to face east")
  end
  if steps == 55 then
    local x, y = goer:position()
    assert(math.abs(x - 15.5) < 1e-3 and math.abs(y - 6.5) < 1e-3, "walk_to arrives exactly")
    assert(not goer:moving() and goer:walk_to(15.5, 6.5), "and stops, reporting arrival")
    walker:stop()
    assert(not walker:moving(), "stop stops")
    walker:set_speed(2)
    assert(walker:speed() == 2, "set_speed changes the speed")
    print("Character checks passed.")
  end
end
