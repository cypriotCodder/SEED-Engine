#include "assets/pack.hpp"
#include "export.hpp"
#include "io/json.hpp"
#include "io/storage.hpp"
#include "project.hpp"
#include "project/archive.hpp"
#include "project/scene_file.hpp"
#include "scripts.hpp"
#include "starter.hpp"
#include "textures.hpp"
#include <array>
#include <chrono>
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

    // Game settings: defaults for new projects, round trips, and validation.
    check(reopened.game == seed::GameSettings{}, "New projects start in main at 1280x720");
    auto configured = reopened;
    configured.game.start_scene = "level2";
    configured.game.title = "Island!";
    configured.game.width = 800;
    configured.game.height = 600;
    configured.game.fullscreen = true;
    save_project(configured);
    check(open_project(created.root).game == configured.game, "Game settings round trip");
    configured.game.width = 10;
    rejects([&] { save_project(configured); }, "A tiny window saved");
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
    starter_island(assets, "main");
    check(assets.problems().empty(), "Starter island assets are valid");
    starter_island(assets, "main");
    check(assets.materials.size() == 10, "Applying the starter again adds no duplicate materials");
    seed::save_assets(project.root / "assets", assets);
    check(seed::load_assets(project.root / "assets") == assets, "Starter assets read back unchanged");

    seed::Materials registry;
    assets.register_materials(registry);
    const seed::Terrain terrain(assets.terrains.at("main"), registry);
    const auto centre = terrain.sample(1, {{}, {0.5F, 0.5F}});
    check(centre.elevation > 0.3F, "Spawn is on land");
    const auto sea = terrain.sample(1, {{200, 0}, {0, 0}});
    check(sea.material == registry.find("deep_water"), "Past the rim is deep water");
}
// A project's terrains: an older project's single terrain.json becomes "main" and moves to
// terrains/main.json on save; terrains are added and removed as files.
void terrains(const fs::path& root) {
    const auto folder = root / "assets";
    fs::create_directories(folder);
    seed::Assets old;
    starter_island(old, "main");
    seed::write_text(folder / "terrain.json",
                     seed::to_json(seed::terrain_file_json(old.terrains.at("main"))));
    seed::write_text(folder / "materials.json", seed::to_json(seed::assets_json(old, "materials")));
    auto loaded = seed::load_assets(folder);
    check(loaded.terrains.size() == 1 && loaded.terrains.at("main") == old.terrains.at("main"),
          "An old terrain.json loads as the terrain \"main\"");
    check(seed::load_terrain(seed::folder_files(folder), "main") == old.terrains.at("main"),
          "Games find the old terrain too");

    auto previous = loaded;
    loaded.terrains["desert"] = {};
    seed::save_assets(folder, loaded, &previous);
    check(fs::exists(folder / "terrains" / "main.json") && fs::exists(folder / "terrains" / "desert.json") &&
              !fs::exists(folder / "terrain.json"),
          "Saving moves the old file and writes each terrain");
    auto fewer = loaded;
    fewer.terrains.erase("desert");
    seed::save_assets(folder, fewer, &loaded);
    check(!fs::exists(folder / "terrains" / "desert.json"), "A deleted terrain's file is removed");
    check(seed::load_assets(folder).terrains == fewer.terrains, "Terrains read back unchanged");
    check(seed::load_terrain(seed::folder_files(folder), "nowhere") == seed::TerrainAsset{},
          "A missing terrain is flat ground");
    fewer.terrains["bad name"] = {};
    check(!fewer.problems().empty(), "Terrain names are checked");
}

// New scripts start from a template that compiles; syntax errors name the line, and edits are
// noticed.
void scripts(const fs::path& root) {
    const auto folder = root / "scripts";
    Scripts::create(folder, "player.lua");
    rejects([&] { Scripts::create(folder, "player.lua"); }, "Existing script overwritten");
    rejects([&] { Scripts::create(folder, "../escape.lua"); }, "Path outside scripts/ accepted");
    rejects([&] { Scripts::create(folder, "notes.txt"); }, "Non-Lua script accepted");
    Scripts::create(folder, "enemy.lua");
    check(Scripts::list(folder) == std::vector<std::string>{"enemy.lua", "player.lua"},
          "Scripts listed in order");

    Scripts checker;
    check(checker.syntax_error(folder / "player.lua").empty(), "The template has no syntax errors");
    seed::write_text(folder / "enemy.lua", "function update(dt)\n  local x = = 1\nend\n");
    const auto error = checker.syntax_error(folder / "enemy.lua");
    check(error.find("enemy.lua:2:") != std::string::npos, "Syntax errors name the file and line");
    // An edit is noticed: the cache is keyed by modification time.
    seed::write_text(folder / "enemy.lua", "function update(dt) end\n");
    std::filesystem::last_write_time(folder / "enemy.lua",
                                     std::filesystem::file_time_type::clock::now() + std::chrono::seconds(5));
    check(checker.syntax_error(folder / "enemy.lua").empty(), "A fixed script is checked again");
    check(!checker.syntax_error(folder / "missing.lua").empty(), "A missing script is reported");
}
// Exporting builds a signed, self-contained app from a ready project, refuses broken ones, and
// replaces an existing app only when asked.
void exporting(const fs::path& root) {
    fs::create_directories(root);
    fs::copy(SEED_SAMPLE_PROJECT, root / "Sample", fs::copy_options::recursive);
    const auto project = open_project(root / "Sample");
    check(export_problems(project).empty(), "The sample project is ready to export");
    const auto report = export_macos_app(project, SEED_PLAYER_PATH, root / "out", false);
    const auto contents = report.app / "Contents";
    check(report.app == root / "out" / "Sample Island.app", "App named after the project");
    check(report.stripped && report.signed_app, "The executable is stripped and the app signed");
    check(fs::is_regular_file(contents / "MacOS" / "Sample Island"), "Executable in place");
    check((fs::status(contents / "MacOS" / "Sample Island").permissions() & fs::perms::owner_exec) !=
              fs::perms::none,
          "Executable is executable");
    const auto plist = seed::read_text(contents / "Info.plist");
    check(plist.find("<string>games.seed.sample-island-5eed0001</string>") != std::string::npos,
          "Bundle identifier");
    for (const char* license : {"SDL2.txt", "LZ4.txt", "Lua.txt"})
        check(fs::is_regular_file(contents / "Resources" / "licenses" / license), license);
    check(!fs::exists(root / "out" / ".Sample Island.app.partial"), "No partial app left behind");

    const auto archive = seed::ProjectArchive::read(contents / "Resources" / seed::ProjectArchive::file_name);
    check(archive.files.size() == report.files && archive.files.count("scenes/main.json") &&
              archive.files.count("assets/terrain.json") && archive.files.count("project.seed.json"),
          "The archive holds the project's files");
    check(archive.files.at("scenes/main.json").find('\n') == std::string::npos, "JSON is compacted");
    // Games load their one terrain by name; everything else loads as a whole.
    auto from_folder = seed::load_assets(root / "Sample" / "assets");
    const auto folder_terrains = from_folder.terrains;
    from_folder.terrains.clear();
    const auto packed = seed::subfolder(archive.reader(), "assets/");
    check(seed::load_assets(packed) == from_folder, "The archive's assets match the project's");
    check(seed::load_terrain(packed, "main") == folder_terrains.at("main"), "The archive's terrain matches");

    rejects([&] { export_macos_app(project, SEED_PLAYER_PATH, root / "out", false); },
            "Existing app replaced unasked");
    export_macos_app(project, SEED_PLAYER_PATH, root / "out", true);

    // A broken script or a missing one stops the export, and nothing is written.
    fs::create_directories(root / "Sample" / "scripts");
    seed::write_text(root / "Sample" / "scripts" / "broken.lua", "function (\n");
    check(export_problems(project).find("broken.lua:1:") != std::string::npos, "Syntax errors stop exports");
    rejects([&] { export_macos_app(project, SEED_PLAYER_PATH, root / "out2", false); },
            "Broken project exported");
    check(!fs::exists(root / "out2" / "Sample Island.app"), "No app from a broken project");
    fs::remove(root / "Sample" / "scripts" / "broken.lua");
    auto scene = seed::load_scene(root / "Sample" / "scenes" / "main.json");
    scene.entities[0].script = "ghost.lua";
    seed::save_scene(root / "Sample" / "scenes" / "main.json", scene);
    check(export_problems(project).find("scripts/ghost.lua, which does not exist") != std::string::npos,
          "Missing scripts stop exports");
}
// An uncompressed 32-bit TGA, top row first, every pixel `bgra`.
void write_tga(const fs::path& file, int width, int height, std::array<unsigned char, 4> bgra) {
    std::string bytes(18, '\0');
    bytes[2] = 2;
    bytes[12] = char(width), bytes[14] = char(height);
    bytes[16] = 32, bytes[17] = 0x28;
    for (int i = 0; i < width * height; ++i)
        bytes.append(reinterpret_cast<const char*>(bgra.data()), 4);
    seed::write_text(file, bytes);
}

// Imported images are copied under a free name, cooked into the project's texture pack only when
// they change, and shipped with exported games; materials may not name missing textures.
void textures(const fs::path& root) {
    fs::create_directories(root);
    fs::copy(SEED_SAMPLE_PROJECT, root / "Sample", fs::copy_options::recursive);
    const auto project = open_project(root / "Sample");
    check(!cook_textures(project.root) && !fs::exists(texture_pack(project.root)), "No textures, no pack");
    write_tga(root / "Stone Wall.tga", 6, 5, {10, 20, 30, 255});
    rejects([&] { import_texture(project.root, root / "missing.tga"); }, "A missing image imported");
    seed::write_text(root / "junk.png", "not an image");
    rejects([&] { import_texture(project.root, root / "junk.png"); }, "An undecodable image imported");
    check(import_texture(project.root, root / "Stone Wall.tga") == "Stone_Wall", "Names are cleaned");
    check(import_texture(project.root, root / "Stone Wall.tga") == "Stone_Wall_2",
          "Taken names are numbered");
    check(list_textures(project.root) == std::vector<std::string>{"Stone_Wall", "Stone_Wall_2"},
          "Textures listed");
    const auto image = read_texture(texture_folder(project.root) / "Stone_Wall.tga", "x");
    check(image.width == 8 && image.height == 4 && image.rgba.size() == 8 * 4 * 4 && image.rgba[0] == 30 &&
              image.rgba[3] == 255,
          "Images are resampled to multiples of four as RGBA");

    check(cook_textures(project.root), "Cooking writes the pack");
    check(!cook_textures(project.root), "An unchanged pack is not cooked again");
    {
        const seed::Pack pack(texture_pack(project.root));
        check(pack.has("Stone_Wall") && pack.texture("Stone_Wall_2").width == 8,
              "The pack holds each texture");
    }

    auto assets = seed::load_assets(project.root / "assets");
    const auto previous = assets;
    assets.materials[0].texture = "Stone_Wall";
    assets.materials[0].texture_scale = 4;
    seed::save_assets(project.root / "assets", assets, &previous);
    check(seed::load_assets(project.root / "assets") == assets, "Texture and scale read back");
    auto bad = assets;
    bad.materials[0].texture_scale = 0;
    check(!bad.problems().empty(), "Texture scales are checked");

    // Painted terrain ships beside its scene.
    auto scene = seed::load_scene(project.root / "scenes" / "main.json");
    scene.paint.set(3, 3, {seed::paint_ground, scene.paint.material(assets.materials[0].name), 0, false, 0});
    seed::save_scene(project.root / "scenes" / "main.json", scene);

    const auto report = export_macos_app(project, SEED_PLAYER_PATH, root / "out", false);
    check(fs::is_regular_file(report.app / "Contents" / "Resources" / "game.pak"), "Exports ship the pack");
    const auto archive =
        seed::ProjectArchive::read(report.app / "Contents" / "Resources" / seed::ProjectArchive::file_name);
    check(archive.files.count("scenes/main.paint") &&
              seed::decode_paint(archive.files.at("scenes/main.paint")) == scene.paint,
          "Exports ship painted terrain");
    fs::remove(texture_folder(project.root) / "Stone_Wall.tga");
    check(export_problems(project).find("\"Stone_Wall\", which is not in assets/textures") !=
              std::string::npos,
          "Missing textures stop exports");
    fs::remove(texture_folder(project.root) / "Stone_Wall_2.tga");
    check(cook_textures(project.root) && !fs::exists(texture_pack(project.root)),
          "The pack goes with the last texture");
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
        terrains(root / "terrains");
        scripts(root / "script-checks");
        exporting(root / "export");
        textures(root / "textures");
        std::cout << "Editor project checks passed.\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
