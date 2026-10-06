-- A door to another scene, chosen by the door's name.
local targets = {CaveDoor = {"cave", "Entry"}, ExitDoor = {"island", "FromCave"}}
local target = targets[self:name()]

function on_touch(other)
  if other == game.player() and target then
    sound.play("door")
    game.load_scene(target[1], target[2])
  end
end
