#pragma once
#include "io/json.hpp"
#include <string>

namespace seed {
// How a project runs as a game, stored in project.seed.json under "game".
struct GameSettings {
    std::string start_scene{"main"}; // scenes/<start_scene>.json
    std::string title;               // Window title; empty uses the project's name.
    int width{1280}, height{720};    // Window size in logical pixels.
    bool fullscreen{};
    bool operator==(const GameSettings&) const = default;
    // Every problem, one per line; empty when valid.
    std::string problems() const;
};

Json game_settings_json(const GameSettings& settings);
// Reads the "game" object of a project file; missing fields keep their defaults.
GameSettings parse_game_settings(const Json* game);
} // namespace seed
