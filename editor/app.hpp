#pragma once
#include "asset_panels.hpp"
#include "console.hpp"
#include "export.hpp"
#include "history.hpp"
#include "platform/window.hpp"
#include "play.hpp"
#include "project.hpp"
#include "scene_editor.hpp"
#include "splash.hpp"
#include "ui_test.hpp"
#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace seed::editor {
struct Options {
    bool smoke{};                        // Draw 60 frames, then exit.
    std::filesystem::path screenshot;    // Written on the last smoke frame.
    std::filesystem::path open;          // Project to open at startup.
    std::filesystem::path create_parent; // With create_name: create this project at startup.
    std::string create_name;
    std::filesystem::path preferences; // Overrides the per-user preferences folder.
    bool play{};    // Press Play once the project opens; with smoke, wait for the game to finish.
    bool ui_test{}; // Drive the opened project's Scene view with scripted input and check it.
    std::filesystem::path export_to; // Export the opened project here (replacing), then carry on.
    bool splash{};                   // Show the launch screen even in a smoke run (automated runs skip it).
};

// The Seed editor: a hub for creating and opening projects, and a dockable workspace for the open
// project. One project is open at a time.
class App final {
public:
    explicit App(Options options);
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;
    int run();

private:
    using Level = LogLevel;
    struct FileEntry {
        std::filesystem::path path;
        int depth{};
        bool directory{};
    };

    void log(Level level, std::string text);
    // Runs `action` after the current frame, when ImGui state (layout, settings) may change safely.
    void later(std::function<void()> action) { pending_ = std::move(action); }
    void open(const std::filesystem::path& path);
    void create(const std::filesystem::path& parent, const std::string& name);
    void close_project();
    void save_settings();
    bool dirty() const;
    // Saves settings and assets; false if something could not be saved.
    bool save_all();
    // Runs `then` after the current frame, first asking to save when there are unsaved changes.
    void leave(std::function<void()> then);
    void unsaved_popup();
    void record_history();
    void start_play();
    // Re-cooks the project's textures if their files changed; tells the preview when the pack did.
    void update_textures(bool always);
    std::string texture_error_;
    std::filesystem::path player_path() const;
    // Saves, then exports to `destination`; `replace` allows overwriting an existing app.
    bool export_app(const std::filesystem::path& destination, bool replace);
    void export_popup();
    void play_controls();
    void step_history(bool redo);
    void refresh_files();

    void frame();
    void hub();
    void workspace();
    void menu_bar();
    void appearance_menu();
    void project_panel();
    void console_panel();
    void settings_panel();
    void about_popup();
    void build_default_layout(unsigned dockspace);
    void workspace_menu();
    void save_workspace();
    void load_workspace();
    enum class Workspace { scene, terrain, assets };
    Workspace workspace_{Workspace::scene};
    void screenshot(const std::filesystem::path& path);

    Options options_;
    Window window_;
    std::filesystem::path preferences_;
    RecentProjects recent_;
    std::optional<Project> project_;
    std::string layout_file_; // ImGui keeps a pointer to this string.
    bool reset_layout_{};
    int ui_scale_{100};
    int ui_font_size_{17};
    bool compact_ui_{};
    bool quit_{};

    // Hub form state.
    std::string new_name_{"New Game"};
    std::string new_location_;
    std::string open_path_;
    std::string hub_error_;

    // Workspace state.
    bool show_project_{true}, show_console_{true}, show_settings_{true}, show_about_{};
    std::string edited_name_;
    GameSettings edited_game_;
    bool settings_changed() const { return edited_name_ != project_->name || edited_game_ != project_->game; }
    std::vector<FileEntry> files_;
    std::chrono::steady_clock::time_point files_scanned_{};
    std::vector<LogLine> log_;
    bool log_scroll_{};
    std::array<bool, 3> console_levels_{true, true, true};
    std::string console_search_;
    bool console_group_{true}, console_follow_{true}, focus_console_{};
    unsigned frames_{};
    unsigned calm_frames_{};
    std::function<void()> pending_;
    std::function<void()> after_prompt_; // Waiting on the unsaved-changes prompt.
    bool prompt_{};
    AssetPanels assets_;
    SceneEditor scene_;
    PlaySession play_;
    std::unique_ptr<UiTest> ui_test_;
    std::unique_ptr<Splash> splash_;       // The launch screen, until it has faded.
    std::filesystem::path pending_export_; // Waiting for the user to allow replacing an app.
    bool export_failed_{};
    // What undo restores: every edit to the project's assets, the open scene and its settings.
    struct Snapshot {
        Assets assets;
        SceneFile scene;
        SceneEditor::Prefabs prefabs;
        std::string scene_name, project_name;
        GameSettings game;
        bool operator==(const Snapshot&) const = default;
    };
    Snapshot snapshot() const {
        return {assets_.assets(), scene_.scene(), scene_.prefabs(),
                scene_.name(),    edited_name_,   edited_game_};
    }
    History<Snapshot> history_;
    std::string history_scene_;
};
} // namespace seed::editor
