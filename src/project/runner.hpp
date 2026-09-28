#pragma once
#include <filesystem>

namespace seed {
// Runs a project folder as a game with no game code: the project's assets and terrain, its main
// scene, and a default player the camera follows. The player is the scene entity named "Player"
// (keeping its look), or a new one at the entity named "Spawn", or at the world origin. Movement
// uses the actions move_up, move_down, move_left and move_right, which default to WASD and the
// arrow keys when the project does not define them. Water (elevation below 0) and solid tiles
// block movement. `argc`/`argv` carry the engine's options (--smoke, --save, --seed, ...).
// Returns the process exit code, like seed::run.
int run_project(const std::filesystem::path& project, int argc, char** argv);
// Runs an exported game's archive (game.seedpack) the same way. Saves go to the player's
// Application Support folder unless --save says otherwise.
int run_archive(const std::filesystem::path& archive, int argc, char** argv);
} // namespace seed
