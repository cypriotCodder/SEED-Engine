#include "io/json.hpp"
#include "io/storage.hpp"
#include "project.hpp"
#include "starter.hpp"
#include <filesystem>
#include <iostream>

namespace {
namespace fs = std::filesystem;
using namespace seed::editor;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F>
void rejects(F&& f, const char* message) {
    try {
        f();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

void names() {
    check(check_project_name("My Game_2-x").empty(), "Valid name accepted");
    for (const char* bad : {"", " Lead", "Trail ", "Slash/Name", "Dot.Name", "Quote\"", "Ünicode"})
        check(!check_project_name(bad).empty(), bad);
    check(!check_project_name(std::string(65, 'a')).empty(), "Over-long name rejected");
    check(check_project_name(std::string(64, 'a')).empty(), "64-character name accepted");
}

void create_and_open(const fs::path& root) {
    const auto created = create_project(root, "Island Survival");
    check(created.root == fs::weakly_canonical(root / "Island Survival"),
          "Project folder named after the project");
    for (const char* folder : {"scenes", "assets", "scripts"})
        check(fs::is_directory(created.root / folder), folder);
    check(created.game_id.rfind("island-survival-", 0) == 0 && created.game_id.size() == 24,
          "Game id is a slug plus eight hex digits");

    const auto opened = open_project(created.root);
    check(opened.name == created.name && opened.game_id == created.game_id, "Project reads back");
    check(open_project(created.root / project_file_name).root == created.root,
          "Opening the file finds its folder");

    // Renaming keeps the game id.
    auto renamed = opened;
    renamed.name = "Island Survival 2";
    save_project(renamed);
    const auto reopened = open_project(created.root);
    check(reopened.name == "Island Survival 2" && reopened.game_id == created.game_id, "Rename keeps the id");

    rejects([&] { create_project(root, "Island Survival"); }, "Existing folder overwritten");
    rejects([&] { create_project(root, "Bad/Name"); }, "Invalid name created");
    const auto twin = create_project(root, "Island_Survival");
    check(twin.game_id != created.game_id, "Similar names get different ids");
}

void bad_projects(const fs::path& root) {
    rejects([&] { open_project(root / "missing"); }, "Missing folder opened");
    fs::create_directories(root / "plain");
    rejects([&] { open_project(root / "plain"); }, "Folder without a project file opened");

    const auto write = [&](const char* name, const char* text) {
        fs::create_directories(root / name);
        seed::write_text(root / name / project_file_name, text);
        return root / name;
    };
    rejects([&] { open_project(write("broken", "{\"seed_project\": 1,")); }, "Malformed JSON opened");
    rejects([&] { open_project(write("newer", R"({"seed_project": 2, "name": "A", "game_id": "a"})")); },
            "Project from a newer editor opened");
    rejects([&] { open_project(write("noid", R"({"seed_project": 1, "name": "A"})")); },
            "Missing game id opened");
    rejects([&] { open_project(write("badname", R"({"seed_project": 1, "name": "", "game_id": "a"})")); },
            "Empty name opened");
    try {
        open_project(write("typed", R"({"seed_project": "one", "name": "A", "game_id": "a"})"));
        throw std::logic_error("Wrongly typed field opened");
    } catch (const std::runtime_error& error) {
        check(std::string(error.what()).find("project.seed.json") != std::string::npos,
              "Errors name the project file");
    }
}

void recent(const fs::path& root) {
    const auto file = root / "prefs" / "recent.json";
    {
        RecentProjects list(file);
        check(list.entries().empty(), "No list yet");
        for (int i = 0; i < 14; ++i)
            list.add(root / ("p" + std::to_string(i)));
        list.add(root / "p5"); // Moves to the front instead of duplicating.
        check(list.entries().size() == RecentProjects::capacity, "List is capped");
        check(list.entries().front().filename() == "p5", "Re-added entry moves to the front");
        list.remove(list.entries().back());
    }
    RecentProjects reloaded(file);
    check(reloaded.entries().size() == RecentProjects::capacity - 1, "List persists");
    check(reloaded.entries().front().filename() == "p5" && reloaded.entries()[1].filename() == "p13",
          "Order persists");
    seed::write_text(file, "not json");
    check(RecentProjects(file).entries().empty(), "A damaged list starts empty");
}
// The starter island is valid, generates land at the centre and sea past the rim, and is saved
// as a complete project that opens in the editor.
void starter(const fs::path& root) {
    const auto project = create_project(root, "Starter Island");
    seed::Assets assets;
    starter_island(assets);
    check(assets.problems().empty(), "Starter island assets are valid");
    starter_island(assets);
    check(assets.materials.size() == 10, "Applying the starter again adds no duplicate materials");
    seed::save_assets(project.root / "assets", assets);
    check(seed::load_assets(project.root / "assets") == assets, "Starter assets read back unchanged");

    seed::Materials registry;
    assets.register_materials(registry);
    const seed::Terrain terrain(assets.terrain, registry);
    const auto centre = terrain.sample(1, {{}, {0.5F, 0.5F}});
    check(centre.elevation > 0.3F, "Spawn is on land");
    const auto sea = terrain.sample(1, {{200, 0}, {0, 0}});
    check(sea.material == registry.find("deep_water"), "Past the rim is deep water");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::invalid_argument("Usage: seed_editor_project_tests SCRATCH_DIRECTORY");
        const fs::path root = argv[1];
        fs::remove_all(root);
        fs::create_directories(root);
        names();
        create_and_open(root);
        bad_projects(root);
        recent(root);
        starter(root);
        std::cout << "Editor project checks passed.\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
