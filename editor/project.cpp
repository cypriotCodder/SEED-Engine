#include "project.hpp"
#include "io/json.hpp"
#include "io/storage.hpp"
#include "project/scene_file.hpp"
#include <algorithm>
#include <cstdio>
#include <random>

namespace seed::editor {
namespace {
namespace fs = std::filesystem;

// A lowercase, dash-separated form of the name plus random hex, so two projects named alike
// still get different save identities.
std::string new_game_id(std::string_view name) {
    std::string slug;
    for (const char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            slug += c;
        else if (c >= 'A' && c <= 'Z')
            slug += static_cast<char>(c - 'A' + 'a');
        else if (!slug.empty() && slug.back() != '-')
            slug += '-';
    }
    while (!slug.empty() && slug.back() == '-')
        slug.pop_back();
    if (slug.empty()) slug = "game";
    std::random_device device;
    char suffix[9];
    std::snprintf(suffix, sizeof(suffix), "%08x", static_cast<unsigned>(device()));
    return slug + "-" + suffix;
}

Json to_json_value(const Project& project) {
    auto file = Json::object();
    file.set("seed_project", project.format);
    file.set("name", project.name);
    file.set("game_id", project.game_id);
    file.set("game", game_settings_json(project.game));
    return file;
}

fs::path full_path(const fs::path& path) {
    return fs::weakly_canonical(fs::absolute(path));
}
} // namespace

std::string check_project_name(std::string_view name) {
    if (name.empty()) return "Enter a project name.";
    if (name.size() > 64) return "Use at most 64 characters.";
    if (name.front() == ' ' || name.back() == ' ') return "Remove spaces at the start or end.";
    for (const char c : name) {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                             c == ' ' || c == '-' || c == '_';
        if (!allowed) return "Use only letters, digits, spaces, '-' and '_'.";
    }
    return {};
}

Project create_project(const fs::path& parent, std::string_view name) {
    if (const auto problem = check_project_name(name); !problem.empty()) throw std::invalid_argument(problem);
    if (parent.empty()) throw std::invalid_argument("Choose a location for the project.");
    Project project;
    project.root = full_path(parent / std::string(name));
    if (fs::exists(project.root))
        throw std::runtime_error("A folder named \"" + std::string(name) + "\" already exists there.");
    project.name = std::string(name);
    project.game_id = new_game_id(name);
    fs::create_directories(project.root);
    for (const char* folder : {"scenes", "assets", "scripts"})
        fs::create_directory(project.root / folder);
    write_text(project.root / ".gitignore", "# Per-user editor state\n.seed/\n");
    save_scene(project.root / "scenes" / "main.json", {});
    save_project(project);
    return project;
}

Project open_project(const fs::path& path) {
    auto root = full_path(path);
    if (root.filename() == project_file_name) root = root.parent_path();
    const auto file = root / project_file_name;
    if (!fs::is_directory(root)) throw std::runtime_error("Folder not found: " + root.string());
    if (!fs::exists(file))
        throw std::runtime_error("Not a Seed project (no project.seed.json): " + root.string());
    try {
        const auto json = parse_json(read_text(file, 1024 * 1024));
        Project project;
        project.root = root;
        project.format = static_cast<int>(json.at("seed_project").as_int(1, 1000000));
        if (project.format > project_format)
            throw std::runtime_error("It was made by a newer editor (format " +
                                     std::to_string(project.format) + ").");
        project.name = json.at("name").as_string();
        if (const auto problem = check_project_name(project.name); !problem.empty())
            throw std::runtime_error("Invalid name: " + problem);
        project.game_id = json.at("game_id").as_string();
        if (project.game_id.empty()) throw std::runtime_error("Empty game_id.");
        project.game = parse_game_settings(json.find("game"));
        if (const auto problem = project.game.problems(); !problem.empty()) throw std::runtime_error(problem);
        return project;
    } catch (const std::exception& error) {
        throw std::runtime_error("Cannot open " + file.string() + ": " + error.what());
    }
}

void save_project(const Project& project) {
    if (const auto problem = check_project_name(project.name); !problem.empty())
        throw std::invalid_argument(problem);
    if (const auto problem = project.game.problems(); !problem.empty()) throw std::invalid_argument(problem);
    write_text(project.root / project_file_name, to_json(to_json_value(project)));
}

fs::path user_state_directory(const Project& project) {
    const auto directory = project.root / ".seed";
    fs::create_directories(directory);
    return directory;
}

RecentProjects::RecentProjects(fs::path file) : file_(std::move(file)) {
    if (!fs::exists(file_)) return;
    // A damaged list is not worth refusing to start over; it is rewritten on the next change.
    try {
        const auto file = parse_json(read_text(file_, 1024 * 1024)); // Must outlive the loop.
        for (const auto& item : file.at("projects").items())
            if (entries_.size() < capacity) entries_.emplace_back(item.as_string());
    } catch (const std::exception&) {
        entries_.clear();
    }
}

void RecentProjects::add(const fs::path& root) {
    const auto path = full_path(root); // A copy, so `root` may be an entry of this list.
    entries_.erase(std::remove(entries_.begin(), entries_.end(), path), entries_.end());
    entries_.insert(entries_.begin(), path);
    if (entries_.size() > capacity) entries_.resize(capacity);
    save();
}

void RecentProjects::remove(const fs::path& root) {
    const auto path = root; // `root` may refer to an entry that remove() shifts.
    entries_.erase(std::remove(entries_.begin(), entries_.end(), path), entries_.end());
    save();
}

void RecentProjects::save() const {
    auto list = Json::array();
    for (const auto& entry : entries_)
        list.push(entry.string());
    auto file = Json::object();
    file.set("projects", list);
    fs::create_directories(file_.parent_path());
    write_text(file_, to_json(file));
}
} // namespace seed::editor
