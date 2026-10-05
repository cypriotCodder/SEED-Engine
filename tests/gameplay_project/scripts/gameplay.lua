-- Runs east at 8 tiles per second through a Coin (x 31.5) and then a Post (x 32.5), checking touch
-- events, nearby queries, ui drawing, the pointer and pausing.
local touched, left, steps, paused_x = {}, {}, 0, 0

function on_touch(other) touched[#touched + 1] = other:name() end
function on_leave(other) left[#left + 1] = other:name() end

function start()
  self:walk(1, 0)
  local near = world.near(31.5, 12.5, 0.1, "Coin")
  assert(#near == 1 and near[1] == world.find("Coin"), "world.near finds by name")
  local around = world.near(30.5, 12.5, 2.2)
  assert(around[1] == self and #around >= 3, "world.near lists the nearest first: " .. #around)
  local w, h = ui.size()
  assert(w > 0 and h > 0, "ui.size gives the screen size")
  assert(ui.text_width("ab", 2) > 0, "ui.text_width measures text")
  local x, y = input.pointer()
  local sx, sy = input.screen_pointer()
  assert(type(x) == "number" and type(sy) == "number", "the pointer has a position")
  assert(not pcall(ui.text, 0, 0, "x", 99), "text scale has limits")
end

function update(dt)
  steps = steps + 1
  ui.rect(10, 10, 120, 16, {0, 0, 0, 0.5})
  ui.text(14, 12, "Coins: " .. #touched)
  if steps == 20 then
    local _, playing = self:frame()
    assert(playing, "a walking character's animation plays")
    self:set_frame(2)
    local frame, still_playing = self:frame()
    assert(frame == 2 and not still_playing, "set_frame shows one frame")
    assert(not pcall(self.set_frame, self, 4), "frames are checked against the material")
    self:animate()
    assert(select(2, self:frame()), "animate plays again")
  end
  if steps == 25 then
    assert(table.concat(touched, ",") == "Coin,Post", "on_touch in order: " .. table.concat(touched, ","))
    assert(table.concat(left, ",") == "Coin,Post", "on_leave in order: " .. table.concat(left, ","))
    game.set_paused(true)
    paused_x = self:position()
  end
  if steps == 35 then
    assert(game.paused() and self:position() == paused_x, "a paused game holds still")
    game.set_paused(false)
    print("Gameplay checks passed.")
  end
end
