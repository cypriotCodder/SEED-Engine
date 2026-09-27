#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace seed::editor {
// A game project on disk: a folder holding project.seed.json plus the project's scenes, assets and
// scripts. Everything the editor writes is JSON text, so projects diff and merge in version control.
constexpr std::string_view project_file_name = "project.seed.json";
constexpr int project_format = 1;

struct Project {
    std::filesystem::path root; // The project folder.
    std::string name;           // Shown in the editor and used as the default window title.
    // Stable identity recorded in the game's saves; generated once and never changed.
    std::string game_id;
    int format{project_format};
};

// Names are 1-64 characters of letters, digits, spaces, '-' and '_', not starting or ending with
// a space. Returns an empty string when valid, otherwise a message for the user.
std::string check_project_name(std::string_view name);

// Creates `parent/name` with a project file, the standard folders and a .gitignore for the
// editor's per-user files. Throws if the name is invalid or the folder already exists.
Project create_project(const std::filesystem::path& parent, std::string_view name);
// Opens a project folder, or the project file inside it. Throws with a readable message when the
// folder is not a project or its file is malformed or from a newer editor.
Project open_project(const std::filesystem::path& path);
// Writes the project file atomically.
void save_project(const Project& project);
// The folder for the editor's per-user state (window layout); ignored by version control.
std::filesystem::path user_state_directory(const Project& project);

// Recently opened projects, most recent first, stored as JSON in the editor's preferences folder.
class RecentProjects final {
public:
    static constexpr std::size_t capacity = 12;
    explicit RecentProjects(std::filesystem::path file);
    const std::vector<std::filesystem::path>& entries() const { return entries_; }
    // Moves `root` to the front, dropping the oldest entry beyond capacity, and saves.
    void add(const std::filesystem::path& root);
    void remove(const std::filesystem::path& root);

private:
    void save() const;
    std::filesystem::path file_;
    std::vector<std::filesystem::path> entries_;
};
} // namespace seed::editor
