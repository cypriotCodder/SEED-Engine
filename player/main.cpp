// seed_player: runs a Seed project as a game. The editor's Play button launches it, and it is the
// runtime a finished game ships with.
#include "project/runner.hpp"
#include <SDL.h>
#include <string_view>
#include <vector>

// seed_player [--project DIR] [engine options]. Without --project it runs the "project" folder
// beside the executable.
int main(int argc, char** argv) {
    std::filesystem::path project;
    std::vector<char*> rest{argv[0]};
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--project" && i + 1 < argc)
            project = argv[++i];
        else
            rest.push_back(argv[i]);
    }
    if (project.empty()) {
        char* base = SDL_GetBasePath();
        project = std::filesystem::path(base ? base : "") / "project";
        SDL_free(base);
    }
    return seed::run_project(project, static_cast<int>(rest.size()), rest.data());
}
