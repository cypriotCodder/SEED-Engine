#pragma once
#include "platform/audio.hpp"
#include "project/assets.hpp"
#include <SDL.h>
#include <filesystem>
#include <functional>
#include <string>

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
    int rule_{-1}, field_{-1};
    int capturing_{-1}; // Action waiting for a key or button press, or -1.
};
} // namespace seed::editor
