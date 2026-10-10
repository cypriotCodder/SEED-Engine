#pragma once
#include "assets/sound_file.hpp"
#include "platform/audio.hpp"
#include "project/assets.hpp"
#include <SDL.h>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>

namespace seed::editor {

// Editor panels for a project's data assets: materials, input actions, sounds and particle
// styles. Edits stay in memory until saved; invalid assets cannot be saved.
class AssetPanels final {
public:
    using Log = std::function<void(bool error, const std::string& text)>;
    AssetPanels(Log log, bool audio);

    // Loads the assets in `folder` (the project's assets/ folder), replacing any unsaved edits.
    void load(const std::filesystem::path& folder);
    void unload();
    bool dirty() const { return edited_ != saved_; }
    const Assets& assets() const { return edited_; }
    // Write access for other panels (the scene editor creates, renames and deletes terrains);
    // call changed() after changing anything through it.
    Assets& edit() { return edited_; }
    void changed() { refresh(); }
    // The terrain the Terrain panel edits: the one the open scene uses.
    void set_terrain(const std::string& name) { terrain_name_ = name; }
    // True once after a texture was imported, so the texture pack is rebuilt straight away.
    bool take_textures_changed() { return std::exchange(textures_changed_, false); }
    // Selects a material in the Materials panel and brings the panel forward.
    void select_material(const std::string& name);
    // Replaces the edited assets, e.g. from undo; selections stay where they still fit.
    void set(const Assets& assets);
    // Saves the kinds that changed. Returns false, logging why, when the assets have problems.
    bool save();
    void revert() { edited_ = saved_, refresh(); }
    // Gives a raw event to the panels first; returns true if it was used to record a binding.
    bool capture(const SDL_Event& event);

    bool show_materials{true}, show_input{true}, show_sounds{true}, show_particles{true}, show_terrain{true};
    void draw();

    // Window identities for docking (titles change with the unsaved marker).
    static constexpr const char* window_ids[] = {"###Terrain", "###Materials", "###Input", "###Sounds",
                                                 "###Particles"};

private:
    void refresh() { problems_ = edited_.problems(); }
    void materials();
    void input();
    void sounds();
    void particles();
    void terrain(); // In terrain_panel.cpp.
    void problems(const char* kind);

    Log log_;
    Audio audio_;
    std::filesystem::path folder_;
    Assets saved_, edited_;
    std::string problems_;
    int material_{-1}, action_{-1}, sound_{-1}, particle_{-1}; // Selected rows.
    int rule_{-1}, field_{-1}, feature_{-1};
    int capturing_{-1}; // Action waiting for a key or button press, or -1.
    std::string terrain_name_{"main"};
    bool textures_changed_{};
    // Recordings decoded for Play, by file name, and the music being previewed.
    std::map<std::string, std::vector<float>> previews_;
    std::unique_ptr<MusicStream> preview_music_;
    std::string preview_music_name_;
    void music();
    bool focus_materials_{};
};
} // namespace seed::editor
