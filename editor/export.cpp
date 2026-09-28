#include "export.hpp"
#include "io/storage.hpp"
#include "project/archive.hpp"
#include "project/scene_file.hpp"
#include "scripts.hpp"
#include "textures.hpp"
#include <algorithm>
#include <fcntl.h>
#include <set>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace seed::editor {
namespace fs = std::filesystem;
// The licences a shipped game must carry, embedded at build time (see editor/CMakeLists.txt).
extern const char* const game_licenses[][2];

namespace {
std::vector<fs::path> files_in(const fs::path& folder, const char* extension) {
    std::vector<fs::path> found;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(folder, error))
        if (entry.is_regular_file() && entry.path().extension() == extension) found.push_back(entry.path());
    std::sort(found.begin(), found.end());
    return found;
}

std::string escape_xml(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (c == '&')
            out += "&amp;";
        else if (c == '<')
            out += "&lt;";
        else if (c == '>')
            out += "&gt;";
        else
            out += c;
    }
    return out;
}

std::string info_plist(const Project& project) {
    const auto name = escape_xml(project.name);
    return R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>)" +
           name + R"(</string>
  <key>CFBundleDisplayName</key><string>)" +
           name + R"(</string>
  <key>CFBundleExecutable</key><string>)" +
           name + R"(</string>
  <key>CFBundleIdentifier</key><string>games.seed.)" +
           escape_xml(project.game_id) + R"(</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSMinimumSystemVersion</key><string>11.0</string>
  <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
)";
}

// Runs a system tool quietly; returns its exit status, or -1 when it could not be started.
int run_tool(const std::vector<std::string>& command) {
    std::vector<char*> argv;
    for (const auto& part : command)
        argv.push_back(const_cast<char*>(part.c_str()));
    argv.push_back(nullptr);
    // Routine notes ("replacing existing signature") are not worth showing; the status says it all.
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid{};
    const int started = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (started != 0) return -1;
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
} // namespace

std::string export_problems(const Project& project) {
    std::string out;
    Assets assets;
    try {
        assets = load_assets(project.root / "assets");
        out += assets.problems();
    } catch (const std::exception& error) {
        return std::string("assets: ") + error.what() + "\n";
    }
    const auto scripts = project.root / "scripts";
    std::set<std::string> referenced, linked_prefabs;
    for (const auto& file : files_in(project.root / "scenes", ".json")) {
        const auto where = "scenes/" + file.filename().string() + ": ";
        try {
            const auto scene = load_scene(file);
            const auto problems = scene.problems(assets);
            for (std::size_t start = 0; start < problems.size();) {
                const auto end = problems.find('\n', start);
                out += where + problems.substr(start, end - start + 1);
                start = end + 1;
            }
            // "main" missing just means flat ground (a new project); any other name is a mistake,
            // usually a terrain renamed while another scene used it.
            if (scene.terrain != "main" && !assets.terrains.count(scene.terrain))
                out += where + "uses the terrain \"" + scene.terrain + "\", which does not exist\n";
            for (const auto& e : scene.entities) {
                if (!e.script.empty()) referenced.insert(e.script);
                if (!e.prefab.empty()) linked_prefabs.insert(e.prefab);
            }
        } catch (const std::exception& error) {
            out += where + error.what() + "\n";
        }
    }
    // Prefabs: each must load and be valid itself, and every prefab a scene links must exist.
    for (const auto& file : files_in(project.root / "prefabs", ".json")) {
        const auto where = "prefabs/" + file.filename().string() + ": ";
        try {
            SceneFile one;
            one.entities = {parse_prefab(parse_json(read_text(file)))};
            if (const auto problems = one.problems(assets); !problems.empty()) out += where + problems;
            if (!one.entities[0].script.empty()) referenced.insert(one.entities[0].script);
        } catch (const std::exception& error) {
            out += where + error.what() + "\n";
        }
    }
    for (const auto& name : linked_prefabs)
        if (!fs::is_regular_file(project.root / "prefabs" / (name + ".json")))
            out += "A scene uses prefab \"" + name + "\", which does not exist\n";
    for (const auto& name : referenced)
        if (!fs::is_regular_file(scripts / name))
            out += "A scene uses scripts/" + name + ", which does not exist\n";
    Scripts checker;
    for (const auto& file : files_in(scripts, ".lua"))
        if (const auto error = checker.syntax_error(file); !error.empty()) out += "scripts/" + error + "\n";
    if (assets.materials.empty()) out += "The project has no materials\n";
    const auto textures = list_textures(project.root);
    for (const auto& material : assets.materials)
        if (!material.texture.empty() &&
            std::find(textures.begin(), textures.end(), material.texture) == textures.end())
            out += "Material \"" + material.name + "\" uses texture \"" + material.texture +
                   "\", which is not in assets/textures\n";
    return out;
}

ExportReport export_macos_app(const Project& project, const fs::path& player, const fs::path& destination,
                              bool replace) {
    if (const auto problems = export_problems(project); !problems.empty())
        throw std::runtime_error("Not exported. Fix these first:\n" + problems);
    if (!fs::is_regular_file(player))
        throw std::runtime_error("The game runtime is missing: " + player.string());

    // The archive: the project file, JSON compacted, and scripts as they are.
    ProjectArchive archive;
    const auto add_json = [&](const std::string& path) {
        archive.files[path] = to_json(parse_json(read_text(project.root / path)), true);
    };
    add_json("project.seed.json");
    for (const char* folder : {"assets", "assets/terrains", "scenes", "prefabs"})
        for (const auto& file : files_in(project.root / folder, ".json")) {
            const auto path = std::string(folder) + "/" + file.filename().string();
            if (!valid_project_path(path))
                throw std::runtime_error("Rename " + path + ": use letters, digits, '_', '-' and '.'");
            add_json(path);
        }
    for (const auto& file : files_in(project.root / "scripts", ".lua"))
        archive.files["scripts/" + file.filename().string()] = read_text(file);

    ExportReport report;
    report.app = destination / (project.name + ".app");
    if (fs::exists(report.app) && !replace) throw std::runtime_error(report.app.string() + " already exists");
    fs::create_directories(destination);
    // Build beside the target, then swap it in, so a failure never leaves half an app behind.
    auto building = destination / ("." + project.name + ".app.partial");
    fs::remove_all(building);
    try {
        const auto contents = building / "Contents";
        fs::create_directories(contents / "MacOS");
        fs::create_directories(contents / "Resources" / "licenses");
        const auto executable = contents / "MacOS" / project.name;
        fs::copy_file(player, executable);
        fs::permissions(executable, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                                        fs::perms::others_read | fs::perms::others_exec);
        write_text(contents / "Info.plist", info_plist(project));
        const auto pack = contents / "Resources" / ProjectArchive::file_name;
        archive.write(pack);
        // Compressed textures go beside the archive, where the game looks for them.
        cook_textures(project.root);
        if (fs::is_regular_file(texture_pack(project.root)))
            fs::copy_file(texture_pack(project.root), contents / "Resources" / "game.pak");
        for (const auto* license = game_licenses; (*license)[0]; ++license)
            write_text(contents / "Resources" / "licenses" / (*license)[0], (*license)[1]);
        report.files = archive.files.size();
        report.archive_bytes = static_cast<std::size_t>(fs::file_size(pack));
        // Symbols are only for debugging the engine; players do not need them (about 14% of the
        // executable). Stripping must come before signing.
        report.stripped = run_tool({"/usr/bin/strip", executable.string()}) == 0;
        const int signing = run_tool({"/usr/bin/codesign", "--force", "--sign", "-", building.string()});
        report.signed_app = signing == 0;
        if (signing == -1)
            report.signing = "codesign was not found; macOS may refuse to open the app.";
        else if (signing != 0)
            report.signing = "codesign failed; macOS may refuse to open the app.";
        if (fs::exists(report.app)) fs::remove_all(report.app);
        fs::rename(building, report.app);
    } catch (...) {
        std::error_code ignored;
        fs::remove_all(building, ignored);
        throw;
    }
    return report;
}
} // namespace seed::editor
