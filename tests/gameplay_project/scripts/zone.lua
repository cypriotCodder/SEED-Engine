-- An area the Collector walks through: records entries and exits for gameplay.lua to check.
function on_enter(other)
  game.data.zone = (game.data.zone or "") .. "enter " .. other:name() .. ";"
end

function on_exit(other)
  game.data.zone = (game.data.zone or "") .. "exit " .. other:name() .. ";"
end
