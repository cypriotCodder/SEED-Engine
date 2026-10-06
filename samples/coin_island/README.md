# Coin Island

A small game made only with the Seed editor's data and Lua scripts: no C++.

- **title**: the title screen. It offers to continue when `game.data.scene` names the scene last played, which `scripts/game.lua` records; where a saved game goes on is the game's own choice.
- **island**: five coins, a crab that chases the player when near and bites (a heart each, then a moment of safety), lamps that light as the day turns to night, and a door to the cave.
- **cave**: a dark scene with flickering torches and the gem; its own hour stays while the game's clock runs on.

Scripts: `title.lua` (title and new game), `game.lua` (HUD, pause with P, game over and win), `coin.lua` and `gem.lua` (taken once, remembered in `game.data`), `crab.lua` (chasing and biting) and `door.lua` (scene changes by the door's name).

The textures, sounds and music in `assets/` were generated for this sample and are part of it.
