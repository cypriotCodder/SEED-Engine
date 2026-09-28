#pragma once
#include "project/assets.hpp"
#include <string>

namespace seed::editor {
// Adds the starter island's materials (those missing) and makes `terrain` an island of ocean,
// beach, meadow, forest, swamp, plains, mountains and snow, with scattered trees.
void starter_island(Assets& assets, const std::string& terrain);
} // namespace seed::editor
