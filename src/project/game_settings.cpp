#include "project/game_settings.hpp"
#include <algorithm>

namespace seed {
std::string GameSettings::problems() const {
    std::string out;
    const bool name_ok = !start_scene.empty() && start_scene.size() <= 64 &&
                         std::all_of(start_scene.begin(), start_scene.end(), [](char c) {
                             return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                    (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ' ';
                         });
    if (!name_ok) out += "The start scene needs a scene name\n";
    if (title.size() > 128) out += "The window title is longer than 128 characters\n";
    if (width < 320 || width > 7680 || height < 240 || height > 4320)
        out += "The window size must be from 320x240 to 7680x4320\n";
    return out;
}

Json game_settings_json(const GameSettings& settings) {
    auto game = Json::object();
    game.set("start_scene", settings.start_scene);
    game.set("title", settings.title);
    game.set("width", settings.width);
    game.set("height", settings.height);
    game.set("fullscreen", settings.fullscreen);
    if (settings.sort_by_y) game.set("sort_by_y", true); // Written only when set.
    return game;
}

GameSettings parse_game_settings(const Json* game) {
    GameSettings settings;
    if (!game) return settings;
    if (const auto* v = game->find("start_scene")) settings.start_scene = v->as_string();
    if (const auto* v = game->find("title")) settings.title = v->as_string();
    if (const auto* v = game->find("width")) settings.width = static_cast<int>(v->as_int(0, 100000));
    if (const auto* v = game->find("height")) settings.height = static_cast<int>(v->as_int(0, 100000));
    if (const auto* v = game->find("fullscreen")) settings.fullscreen = v->as_bool();
    if (const auto* v = game->find("sort_by_y")) settings.sort_by_y = v->as_bool();
    return settings;
}
} // namespace seed
