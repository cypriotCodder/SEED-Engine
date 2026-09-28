// seed_player: runs a Seed project as a game. The editor's Play button launches it, and it is the
// runtime a finished game ships with.
#include "project/archive.hpp"
#include "project/runner.hpp"
#include <SDL.h>
#include <string_view>
#include <vector>

// seed_player [--project DIR] [engine options]. Without --project it runs the game.seedpack
// beside it (an exported game: SDL reports an app bundle's Resources folder as the base), or else
// a "project" folder there.
int main(int argc, char** argv) {
    std::filesystem::path project;
    std::vector<char*> rest{argv[0]};
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--project" && i + 1 < argc)
            project = argv[++i];
        else
            rest.push_back(argv[i]);
    }
    const int count = static_cast<int>(rest.size());
    if (!project.empty()) return seed::run_project(project, count, rest.data());
    char* base_path = SDL_GetBasePath();
    const std::filesystem::path base = base_path ? base_path : "";
    SDL_free(base_path);
    if (std::filesystem::exists(base / seed::ProjectArchive::file_name))
        return seed::run_archive(base / seed::ProjectArchive::file_name, count, rest.data());
    return seed::run_project(base / "project", count, rest.data());
}
