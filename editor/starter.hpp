#pragma once
#include "project/assets.hpp"

namespace seed::editor {
// Adds the starter island's materials (those missing) and replaces the terrain with an island of
// ocean, beach, meadow, forest, swamp, plains, mountains and snow, with scattered trees.
void starter_island(Assets& assets);
} // namespace seed::editor
