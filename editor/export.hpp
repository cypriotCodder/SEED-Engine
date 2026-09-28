#pragma once
#include "project.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace seed::editor {
struct ExportReport {
    std::filesystem::path app;
    std::size_t files{}, archive_bytes{};
    bool signed_app{}, stripped{};
    std::string signing; // What codesign said, when it failed or was missing.
};

// Checks everything a game needs before shipping it: asset and scene problems, script syntax,
// and that every script a scene names exists. Returns one problem per line; empty when ready.
std::string export_problems(const Project& project);

// Builds `destination/<name>.app`: the player, the project packed as game.seedpack, an Info.plist
// and the licences of the libraries a game contains, then signs it ad hoc so macOS will run it.
// The app is built beside the target and moved into place only when complete. An existing app is
// replaced only when `replace` is true. Throws with the problems when the project is not ready.
ExportReport export_macos_app(const Project& project, const std::filesystem::path& player,
                              const std::filesystem::path& destination, bool replace);
} // namespace seed::editor
