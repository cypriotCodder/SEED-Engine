#include "app.hpp"
#include "folder_dialog.hpp"
#include <SDL_opengl.h>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <thread>

namespace seed::editor {
namespace {
namespace fs = std::filesystem;
constexpr const char* project_window = "Project";
constexpr const char* console_window = "Console";
constexpr const char* settings_window = "Project Settings";
constexpr std::size_t file_list_limit = 5000;
constexpr std::size_t log_limit = 2000;

fs::path preferences_directory(const fs::path& override_path) {
    if (!override_path.empty()) return override_path;
    char* base = SDL_GetPrefPath("Seed", "Seed Editor");
    if (!base) throw std::runtime_error(std::string("Cannot find the preferences folder: ") + SDL_GetError());
    fs::path path(base);
    SDL_free(base);
    return path;
}

fs::path default_location() {
    const char* home = std::getenv("HOME");
    return home ? fs::path(home) / "SeedProjects" : fs::current_path();
}

void style() {
    ImGui::StyleColorsDark();
    auto& s = ImGui::GetStyle();
    s.FontSizeBase = 15;
    s.WindowRounding = 6;
    s.FrameRounding = 4;
    s.TabRounding = 4;
    s.GrabRounding = 4;
    s.WindowPadding = {10, 10};
    s.FramePadding = {8, 5};
    s.ItemSpacing = {8, 6};
    auto* colors = s.Colors;
    colors[ImGuiCol_WindowBg] = {0.11F, 0.12F, 0.13F, 1};
    colors[ImGuiCol_TitleBgActive] = {0.16F, 0.29F, 0.24F, 1};
    colors[ImGuiCol_Header] = {0.20F, 0.36F, 0.30F, 0.8F};
    colors[ImGuiCol_HeaderHovered] = {0.26F, 0.46F, 0.38F, 0.9F};
    colors[ImGuiCol_Button] = {0.20F, 0.36F, 0.30F, 0.8F};
    colors[ImGuiCol_ButtonHovered] = {0.28F, 0.50F, 0.41F, 1};
    colors[ImGuiCol_TabSelected] = {0.20F, 0.36F, 0.30F, 1};
    colors[ImGuiCol_TabHovered] = {0.28F, 0.50F, 0.41F, 1};
    colors[ImGuiCol_CheckMark] = {0.45F, 0.80F, 0.62F, 1};
    colors[ImGuiCol_DockingPreview] = {0.45F, 0.80F, 0.62F, 0.6F};
}

void open_with_system(const fs::path& path) {
    const auto url = "file://" + path.string();
    if (SDL_OpenURL(url.c_str()) != 0)
        throw std::runtime_error(std::string("Cannot open: ") + SDL_GetError());
}

std::string clock_time() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char text[16];
    std::strftime(text, sizeof(text), "%H:%M:%S", &local);
    return text;
}
} // namespace

App::App(Options options)
    : options_(std::move(options)),
      window_(true),
      preferences_(preferences_directory(options_.preferences)),
      recent_(preferences_ / "recent-projects.json"),
      new_location_(default_location().string()),
      assets_([this](bool error, const std::string& text) { log(error ? Level::error : Level::info, text); },
              !options_.smoke),
      scene_([this](bool error, const std::string& text) { log(error ? Level::error : Level::info, text); }),
      play_([this](bool error, const std::string& text) { log(error ? Level::error : Level::info, text); }) {
    window_.title("Seed Editor");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr; // Set per project; the hub has a fixed layout.
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    style();
    if (!ImGui_ImplSDL2_InitForOpenGL(window_.handle(), window_.context()) ||
        !ImGui_ImplOpenGL3_Init("#version 410 core")) {
        ImGui::DestroyContext();
        throw std::runtime_error("Could not start ImGui");
    }
    // A key or click recording an input binding is not also delivered to ImGui.
    window_.observe(this, [](void* self, const SDL_Event& event) {
        if (!static_cast<App*>(self)->assets_.capture(event)) ImGui_ImplSDL2_ProcessEvent(&event);
    });
    log(Level::info, "Seed Editor started.");
}

App::~App() {
    if (project_ && !layout_file_.empty()) ImGui::SaveIniSettingsToDisk(layout_file_.c_str());
    ImGui::GetIO().IniFilename = nullptr;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
}

void App::log(Level level, std::string text) {
    if (log_.size() == log_limit) log_.erase(log_.begin());
    log_.push_back({level, clock_time(), std::move(text)});
    log_scroll_ = true;
}

void App::open(const fs::path& path) {
    auto project = open_project(path);
    close_project();
    project_ = std::move(project);
    recent_.add(project_->root);
    edited_name_ = project_->name;
    try {
        assets_.load(project_->root / "assets");
    } catch (const std::exception& error) {
        log(Level::error, std::string("Could not load the project's assets: ") + error.what());
        assets_.unload();
    }
    try {
        scene_.load(project_->root, "main");
    } catch (const std::exception& error) {
        log(Level::error, std::string("Could not load the main scene: ") + error.what());
        scene_.unload();
    }
    layout_file_ = (user_state_directory(*project_) / "layout.ini").string();
    reset_layout_ = !fs::exists(layout_file_);
    if (!reset_layout_) ImGui::LoadIniSettingsFromDisk(layout_file_.c_str());
    ImGui::GetIO().IniFilename = layout_file_.c_str();
    window_.title(project_->name + " - Seed Editor");
    history_.reset(snapshot());
    history_scene_ = scene_.name();
    refresh_files();
    log(Level::info, "Opened project \"" + project_->name + "\" at " + project_->root.string());
}

void App::create(const fs::path& parent, const std::string& name) {
    const auto project = create_project(parent, name);
    log(Level::info, "Created project \"" + project.name + "\" (game id " + project.game_id + ").");
    open(project.root);
}

void App::close_project() {
    if (!project_) return;
    play_.stop();
    if (dirty()) log(Level::warning, "Discarded unsaved changes.");
    assets_.unload();
    scene_.unload();
    ImGui::SaveIniSettingsToDisk(layout_file_.c_str());
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::ClearIniSettings();
    log(Level::info, "Closed project \"" + project_->name + "\".");
    project_.reset();
    files_.clear();
    window_.title("Seed Editor");
}

void App::save_settings() {
    if (const auto problem = check_project_name(edited_name_); !problem.empty()) {
        log(Level::error, "Project name not saved: " + problem);
        return;
    }
    auto updated = *project_;
    updated.name = edited_name_;
    save_project(updated);
    project_ = std::move(updated);
    window_.title(project_->name + " - Seed Editor");
    log(Level::info, "Saved project settings.");
}

bool App::dirty() const {
    return project_ && (edited_name_ != project_->name || assets_.dirty() || scene_.dirty());
}

bool App::save_all() {
    if (!project_) return true;
    if (edited_name_ != project_->name) save_settings();
    const bool assets = assets_.save(); // Scenes are checked against the assets, so they go first.
    return edited_name_ == project_->name && assets && scene_.save(assets_.assets());
}

void App::leave(std::function<void()> then) {
    if (!dirty()) return later(std::move(then));
    after_prompt_ = std::move(then);
    prompt_ = true;
}

void App::unsaved_popup() {
    if (prompt_) {
        ImGui::OpenPopup("Unsaved changes");
        prompt_ = false;
    }
    if (!ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted("Save changes to this project before continuing?");
    ImGui::Dummy({0, 6});
    if (ImGui::Button("Save", {110, 0})) {
        if (save_all()) later(std::move(after_prompt_));
        ImGui::CloseCurrentPopup(); // On failure the console says why and nothing else happens.
    }
    ImGui::SameLine();
    if (ImGui::Button("Don't Save", {110, 0})) {
        assets_.revert();
        scene_.revert();
        edited_name_ = project_->name;
        later(std::move(after_prompt_));
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", {110, 0}) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        after_prompt_ = nullptr;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void App::start_play() {
    if (!project_ || play_.running()) return;
    // The game runs the files on disk, so unsaved edits are saved first.
    if (dirty() && !save_all())
        return log(Level::error, "Play needs the project saved; see the problems above.");
    // Every Play starts a new world; the project's real saves are never touched.
    const auto save = user_state_directory(*project_) / "play";
    std::filesystem::remove_all(save);
    std::vector<std::string> arguments{"--project", project_->root.string(), "--save", save.string()};
    if (options_.smoke) arguments.push_back("--smoke");
    // seed_player sits beside the editor when installed; in a build tree CMake records its path.
    char* base = SDL_GetBasePath();
    auto player = std::filesystem::path(base ? base : "") / "seed_player";
    SDL_free(base);
    if (!std::filesystem::exists(player)) player = SEED_PLAYER_PATH;
    play_.start(player, arguments);
    log(Level::info, "Playing \"" + project_->name + "\". Close the game window or press Stop to return.");
}

void App::play_controls() {
    // Centred in the menu bar, like a transport control.
    const float width = 90;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + 20, (ImGui::GetWindowWidth() - width) / 2));
    if (!play_.running()) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.20F, 0.45F, 0.30F, 1});
        if (ImGui::Button("Play", {width, 0})) start_play();
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Save and run the project in its own window (Cmd+P).");
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.55F, 0.22F, 0.20F, 1});
        if (ImGui::Button("Stop", {width, 0})) play_.stop();
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextDisabled("Playing");
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_P, ImGuiInputFlags_RouteGlobal)) {
        if (play_.running())
            play_.stop();
        else
            start_play();
    }
}

void App::record_history() {
    // An edit is finished once no field is being typed in or dragged and no button is held.
    if (ImGui::IsAnyItemActive() || ImGui::IsMouseDown(ImGuiMouseButton_Left) || scene_.busy()) return;
    auto now = snapshot();
    // Opening another scene starts a new history, so undo never reaches into a different scene.
    if (now.scene_name != history_scene_) {
        history_scene_ = now.scene_name;
        return history_.reset(std::move(now));
    }
    history_.offer(now);
}

void App::step_history(bool redo) {
    if (ImGui::IsAnyItemActive()) return; // A text field's own undo applies instead.
    const auto& state = redo ? history_.redo() : history_.undo();
    assets_.set(state.assets);
    scene_.set(state.scene);
    edited_name_ = state.project_name;
}

void App::refresh_files() {
    files_.clear();
    files_scanned_ = std::chrono::steady_clock::now();
    if (!project_) return;
    bool truncated = false;
    const auto scan = [&](const auto& self, const fs::path& directory, int depth) -> void {
        std::vector<fs::directory_entry> children;
        std::error_code error;
        for (const auto& entry :
             fs::directory_iterator(directory, fs::directory_options::skip_permission_denied, error))
            if (entry.path().filename().string().front() != '.') children.push_back(entry);
        std::sort(children.begin(), children.end(), [](const auto& a, const auto& b) {
            if (a.is_directory() != b.is_directory()) return a.is_directory();
            return a.path().filename() < b.path().filename();
        });
        for (const auto& child : children) {
            if (files_.size() == file_list_limit) {
                truncated = true;
                return;
            }
            files_.push_back({child.path(), depth, child.is_directory()});
            if (child.is_directory() && !child.is_symlink()) self(self, child.path(), depth + 1);
        }
    };
    scan(scan, project_->root, 0);
    if (truncated) log(Level::warning, "The Project panel shows only the first 5000 files.");
}

int App::run() {
    try {
        if (!options_.create_name.empty()) create(options_.create_parent, options_.create_name);
        if (!options_.open.empty()) open(options_.open);
        if (options_.play) start_play();
    } catch (const std::exception& error) {
        log(Level::error, error.what());
        if (options_.smoke) throw;
    }
    Input input;
    while (!quit_) {
        const auto frame_start = std::chrono::steady_clock::now();
        // An idle editor sleeps until the next event; a few frames after each event let hover and
        // layout settle first.
        if (!options_.smoke) {
            if (SDL_HasEvents(SDL_FIRSTEVENT, SDL_LASTEVENT))
                calm_frames_ = 0;
            else if (++calm_frames_ > 3 && SDL_WaitEventTimeout(nullptr, 500))
                calm_frames_ = 0;
        }
        window_.poll(input);
        play_.poll();
        if (input.quit) {
            input.quit = false;
            leave([this] { quit_ = true; });
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        frame();
        ImGui::Render();
        int width{}, height{};
        window_.drawable_size(width, height);
        window_.clear(0.08F, 0.09F, 0.10F);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData()); // Also draws the Scene view.
        if (const auto error = scene_.take_error(); !error.empty()) log(Level::error, "Scene view: " + error);
        ++frames_;
        if (options_.smoke && frames_ == 60 && !options_.screenshot.empty()) screenshot(options_.screenshot);
        // A smoke run with --play lasts until the game it started has finished.
        if (options_.smoke && frames_ >= 60 && !play_.running()) quit_ = true;
        if (options_.smoke && play_.running()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        window_.present();
        if (pending_) {
            const auto action = std::move(pending_);
            pending_ = nullptr;
            try {
                action();
            } catch (const std::exception& error) {
                log(Level::error, error.what());
                if (!project_) hub_error_ = error.what();
                if (options_.smoke) throw;
            }
        }
        if (!options_.smoke) std::this_thread::sleep_until(frame_start + std::chrono::microseconds(16667));
    }
    return options_.play && play_.last_exit() != 0 ? 1 : 0;
}

void App::frame() {
    try {
        if (project_)
            workspace();
        else
            hub();
    } catch (const std::exception& error) {
        // Every user action reports failure here rather than closing the editor.
        log(Level::error, error.what());
        if (!project_) hub_error_ = error.what();
    }
}

void App::hub() {
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("Hub", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::SetCursorPos({32, 28});
    ImGui::PushFont(nullptr, 30);
    ImGui::TextUnformatted("Seed Engine");
    ImGui::PopFont();
    ImGui::SetCursorPosX(32);
    ImGui::TextDisabled("Create a project or open one to start building your game.");
    ImGui::Dummy({0, 12});
    ImGui::SetCursorPosX(32);
    ImGui::BeginChild("HubBody", {-32, -32});
    if (ImGui::BeginTable("HubColumns", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Recent", ImGuiTableColumnFlags_WidthStretch, 1.4F);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthStretch, 1.0F);
        ImGui::TableNextColumn();
        ImGui::SeparatorText("Recent projects");
        if (recent_.entries().empty()) ImGui::TextDisabled("Projects you open will appear here.");
        std::optional<fs::path> open_now, remove_now;
        for (const auto& entry : recent_.entries()) {
            ImGui::PushID(entry.string().c_str());
            const bool exists = fs::exists(entry / project_file_name);
            ImGui::BeginDisabled(!exists);
            if (ImGui::Selectable("##entry", false, ImGuiSelectableFlags_AllowOverlap, {0, 42}))
                open_now = entry;
            ImGui::EndDisabled();
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Remove from list")) remove_now = entry;
                if (exists && ImGui::MenuItem("Show in Finder")) open_with_system(entry);
                ImGui::EndPopup();
            }
            const auto row_min = ImGui::GetItemRectMin(), row_max = ImGui::GetItemRectMax();
            ImGui::SameLine(10);
            ImGui::PushClipRect(row_min, row_max, true); // Long paths stay inside their column.
            ImGui::BeginGroup();
            ImGui::TextUnformatted(entry.filename().string().c_str());
            ImGui::TextDisabled("%s%s", entry.string().c_str(), exists ? "" : "  (missing)");
            ImGui::EndGroup();
            ImGui::PopClipRect();
            ImGui::PopID();
        }
        if (remove_now) recent_.remove(*remove_now);
        if (open_now) {
            hub_error_.clear();
            later([this, path = *open_now] { open(path); });
        }

        ImGui::TableNextColumn();
        ImGui::SeparatorText("New project");
        ImGui::TextUnformatted("Name");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##name", &new_name_);
        ImGui::TextUnformatted("Location");
        const bool dialog = folder_dialog_available();
        ImGui::SetNextItemWidth(dialog ? -90 : -1);
        ImGui::InputText("##location", &new_location_);
        if (dialog) {
            ImGui::SameLine();
            if (ImGui::Button("Browse...##location", {-1, 0}))
                if (const auto chosen = choose_folder("Choose where to create the project", new_location_))
                    new_location_ = chosen->string();
        }
        const auto problem = check_project_name(new_name_);
        if (!problem.empty())
            ImGui::TextColored({0.95F, 0.65F, 0.35F, 1}, "%s", problem.c_str());
        else
            ImGui::TextDisabled("Creates %s", (fs::path(new_location_) / new_name_).string().c_str());
        ImGui::BeginDisabled(!problem.empty() || new_location_.empty());
        if (ImGui::Button("Create project", {-1, 34})) {
            hub_error_.clear();
            later([this] { create(new_location_, new_name_); });
        }
        ImGui::EndDisabled();

        ImGui::Dummy({0, 10});
        ImGui::SeparatorText("Open project");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##open", "Project folder", &open_path_);
        if (dialog && ImGui::Button("Browse...##open", {-1, 0}))
            if (const auto chosen = choose_folder("Choose a Seed project folder", default_location())) {
                hub_error_.clear();
                later([this, path = *chosen] { open(path); });
            }
        ImGui::BeginDisabled(open_path_.empty());
        if (ImGui::Button("Open", {-1, 0})) {
            hub_error_.clear();
            later([this] { open(open_path_); });
        }
        ImGui::EndDisabled();
        if (!hub_error_.empty()) {
            ImGui::Dummy({0, 6});
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored({0.95F, 0.4F, 0.35F, 1}, "%s", hub_error_.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::End();
}

void App::workspace() {
    menu_bar();
    const ImGuiID dockspace = ImGui::GetID("Workspace");
    if (reset_layout_) {
        build_default_layout(dockspace);
        reset_layout_ = false;
    }
    ImGui::DockSpaceOverViewport(dockspace, ImGui::GetMainViewport(),
                                 ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_NoCloseButton);
    if (std::chrono::steady_clock::now() - files_scanned_ > std::chrono::seconds(2)) refresh_files();
    // Scene panels first: a dock node lists tabs in the order windows first appear, so Scene and
    // Inspector lead their nodes.
    scene_.draw(assets_.assets());
    if (show_project_) project_panel();
    if (show_console_) console_panel();
    if (show_settings_) settings_panel();
    assets_.draw();
    about_popup();
    unsaved_popup();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal)) step_history(false);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal))
        step_history(true);
    record_history();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteGlobal)) save_all();
}

void App::build_default_layout(unsigned dockspace) {
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->WorkSize);
    ImGuiID center = dockspace;
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.2F, nullptr, &center);
    const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.26F, nullptr, &center);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.26F, nullptr, &center);
    const ImGuiID left_bottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.45F, nullptr, &left);
    ImGui::DockBuilderDockWindow(SceneEditor::hierarchy_id, left);
    ImGui::DockBuilderDockWindow(project_window, left_bottom);
    ImGui::DockBuilderDockWindow(settings_window, right);
    ImGui::DockBuilderDockWindow(SceneEditor::inspector_id, right);
    ImGui::DockBuilderDockWindow(console_window, bottom);
    // The centre holds the Scene view, with the asset panels as further tabs.
    ImGui::DockBuilderDockWindow(SceneEditor::scene_id, center);
    for (const char* id : AssetPanels::window_ids)
        ImGui::DockBuilderDockWindow(id, center);
    ImGui::DockBuilderFinish(dockspace);
    show_project_ = show_console_ = show_settings_ = true;
    assets_.show_materials = assets_.show_input = assets_.show_sounds = assets_.show_particles =
        assets_.show_terrain = true;
    scene_.show_scene = scene_.show_hierarchy = scene_.show_inspector = true;
}

void App::menu_bar() {
    if (!ImGui::BeginMainMenuBar()) return;
    bool close = false;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open Project...", nullptr, false, folder_dialog_available()))
            if (const auto chosen =
                    choose_folder("Choose a Seed project folder", project_->root.parent_path()))
                later([this, path = *chosen] { open(path); });
        if (ImGui::MenuItem("Save", "Cmd+S", false, dirty())) save_all();
        if (ImGui::MenuItem("Revert Unsaved Changes", nullptr, false, dirty())) {
            assets_.revert();
            scene_.revert();
            edited_name_ = project_->name;
            log(Level::info, "Reverted unsaved changes.");
        }
        if (ImGui::MenuItem("Show Project in Finder")) open_with_system(project_->root);
        ImGui::Separator();
        if (ImGui::MenuItem("Close Project")) close = true;
        if (ImGui::MenuItem("Quit", "Cmd+Q")) leave([this] { quit_ = true; });
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Cmd+Z", false, history_.can_undo())) step_history(false);
        if (ImGui::MenuItem("Redo", "Shift+Cmd+Z", false, history_.can_redo())) step_history(true);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Window")) {
        ImGui::MenuItem("Scene", nullptr, &scene_.show_scene);
        ImGui::MenuItem("Hierarchy", nullptr, &scene_.show_hierarchy);
        ImGui::MenuItem("Inspector", nullptr, &scene_.show_inspector);
        ImGui::Separator();
        ImGui::MenuItem(project_window, nullptr, &show_project_);
        ImGui::MenuItem(console_window, nullptr, &show_console_);
        ImGui::MenuItem(settings_window, nullptr, &show_settings_);
        ImGui::Separator();
        ImGui::MenuItem("Materials", nullptr, &assets_.show_materials);
        ImGui::MenuItem("Input", nullptr, &assets_.show_input);
        ImGui::MenuItem("Sounds", nullptr, &assets_.show_sounds);
        ImGui::MenuItem("Particles", nullptr, &assets_.show_particles);
        ImGui::MenuItem("Terrain", nullptr, &assets_.show_terrain);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Layout")) reset_layout_ = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About Seed Editor")) show_about_ = true;
        ImGui::EndMenu();
    }
    play_controls();
    ImGui::EndMainMenuBar();
    if (close) leave([this] { close_project(); });
}

void App::project_panel() {
    if (!ImGui::Begin(project_window, &show_project_)) {
        ImGui::End();
        return;
    }
    if (ImGui::SmallButton("Refresh")) refresh_files();
    ImGui::SameLine();
    if (ImGui::SmallButton("Show in Finder")) open_with_system(project_->root);
    ImGui::Separator();
    ImGui::BeginChild("Files");
    int open_levels = 0, visible_depth = 0;
    for (const auto& entry : files_) {
        if (entry.depth > visible_depth) continue; // Inside a collapsed folder.
        while (open_levels > entry.depth) {
            ImGui::TreePop();
            --open_levels;
        }
        visible_depth = entry.depth;
        const auto name = entry.path.filename().string();
        ImGui::PushID(entry.path.string().c_str());
        if (entry.directory) {
            if (ImGui::TreeNodeEx(name.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth)) {
                ++open_levels;
                visible_depth = entry.depth + 1;
            }
        } else {
            ImGui::TreeNodeEx(name.c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                ImGuiTreeNodeFlags_SpanAvailWidth);
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                open_with_system(entry.path);
        }
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Show in Finder"))
                open_with_system(entry.directory ? entry.path : entry.path.parent_path());
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    while (open_levels-- > 0)
        ImGui::TreePop();
    if (files_.empty()) ImGui::TextDisabled("The project folder is empty.");
    ImGui::EndChild();
    ImGui::End();
}

void App::console_panel() {
    if (!ImGui::Begin(console_window, &show_console_)) {
        ImGui::End();
        return;
    }
    if (ImGui::SmallButton("Clear")) log_.clear();
    ImGui::Separator();
    ImGui::BeginChild("Lines");
    for (const auto& line : log_) {
        const ImVec4 color = line.level == Level::error     ? ImVec4{0.95F, 0.45F, 0.40F, 1}
                             : line.level == Level::warning ? ImVec4{0.95F, 0.78F, 0.40F, 1}
                                                            : ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::TextDisabled("%s", line.time.c_str());
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(line.text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    if (log_scroll_) {
        ImGui::SetScrollHereY(1);
        log_scroll_ = false;
    }
    ImGui::EndChild();
    ImGui::End();
}

void App::settings_panel() {
    if (!ImGui::Begin(settings_window, &show_settings_)) {
        ImGui::End();
        return;
    }
    ImGui::TextUnformatted("Name");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##project-name", &edited_name_);
    const auto problem = check_project_name(edited_name_);
    if (!problem.empty()) ImGui::TextColored({0.95F, 0.65F, 0.35F, 1}, "%s", problem.c_str());
    ImGui::Dummy({0, 4});
    ImGui::TextUnformatted("Game ID");
    ImGui::TextDisabled("%s", project_->game_id.c_str());
    ImGui::SetItemTooltip("Recorded in the game's saves. It never changes, even if you rename the project.");
    ImGui::TextUnformatted("Location");
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", project_->root.string().c_str());
    ImGui::PopTextWrapPos();
    ImGui::Dummy({0, 8});
    const bool changed = edited_name_ != project_->name;
    ImGui::BeginDisabled(!changed || !problem.empty());
    if (ImGui::Button("Save", {100, 0})) save_settings();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!changed);
    if (ImGui::Button("Revert", {100, 0})) edited_name_ = project_->name;
    ImGui::EndDisabled();
    ImGui::End();
}

void App::about_popup() {
    if (show_about_) {
        ImGui::OpenPopup("About Seed Editor");
        show_about_ = false;
    }
    if (ImGui::BeginPopupModal("About Seed Editor", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Seed Editor 0.1.0");
        ImGui::TextDisabled("Dear ImGui %s, SDL %d.%d.%d", IMGUI_VERSION, SDL_MAJOR_VERSION,
                            SDL_MINOR_VERSION, SDL_PATCHLEVEL);
        if (ImGui::Button("Close", {120, 0})) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void App::screenshot(const fs::path& path) {
    using ReadPixels = void(APIENTRY*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
    const auto read = reinterpret_cast<ReadPixels>(SDL_GL_GetProcAddress("glReadPixels"));
    if (!read) throw std::runtime_error("Missing OpenGL function: glReadPixels");
    int width{}, height{};
    window_.drawable_size(width, height);
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    read(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << width << ' ' << height << "\n255\n";
    for (int y = height - 1; y >= 0; --y)
        for (int x = 0; x < width; ++x)
            out.write(
                reinterpret_cast<const char*>(pixels.data() + (static_cast<std::size_t>(y) * width + x) * 4),
                3);
    if (!out) throw std::runtime_error("Cannot write screenshot " + path.string());
}
} // namespace seed::editor
