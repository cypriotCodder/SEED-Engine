#pragma once
#include "platform/window.hpp"
#include "project.hpp"
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
    enum class Level { info, warning, error };
    struct LogLine {
        Level level;
        std::string time, text;
    };
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
    void refresh_files();

    void frame();
    void hub();
    void workspace();
    void menu_bar();
    void project_panel();
    void console_panel();
    void settings_panel();
    void about_popup();
    void build_default_layout(unsigned dockspace);
    void screenshot(const std::filesystem::path& path);

    Options options_;
    Window window_;
    std::filesystem::path preferences_;
    RecentProjects recent_;
    std::optional<Project> project_;
    std::string layout_file_; // ImGui keeps a pointer to this string.
    bool reset_layout_{};
    bool quit_{};

    // Hub form state.
    std::string new_name_{"New Game"};
    std::string new_location_;
    std::string open_path_;
    std::string hub_error_;

    // Workspace state.
    bool show_project_{true}, show_console_{true}, show_settings_{true}, show_about_{};
    std::string edited_name_;
    std::vector<FileEntry> files_;
    std::chrono::steady_clock::time_point files_scanned_{};
    std::vector<LogLine> log_;
    bool log_scroll_{};
    unsigned frames_{};
    unsigned calm_frames_{};
    std::function<void()> pending_;
};
} // namespace seed::editor
