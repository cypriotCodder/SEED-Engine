#pragma once
#include "project/scene_file.hpp"
#include "project/terrain.hpp"
#include "render/renderer.hpp"
#include "scripts.hpp"
#include <filesystem>
#include <functional>
#include <imgui.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace seed::editor {
// The Scene view, Hierarchy and Inspector panels for one scene file of the open project. The
// view draws with the engine's own renderer, so what you see is what the game draws; grid lines,
// selection outlines and markers are editor overlays drawn by ImGui on top.
class SceneEditor final {
public:
    using Log = std::function<void(bool error, const std::string& text)>;
    explicit SceneEditor(Log log);
    ~SceneEditor();

    // Opens scenes/<name>.json in `project`, creating an empty scene if the file does not exist.
    void load(const std::filesystem::path& project, const std::string& name);
    void unload();
    bool loaded() const { return !folder_.empty(); }
    bool dirty() const { return edited_ != saved_; }
    // Saves the scene; false, logging why, when it has problems.
    bool save(const Assets& assets);
    void revert() { edited_ = saved_; }
    const SceneFile& scene() const { return edited_; }
    // Replaces the edited scene, e.g. from undo.
    void set(const SceneFile& scene) {
        edited_ = scene;
        if (selected_ >= static_cast<int>(edited_.entities.size()))
            selected_ = static_cast<int>(edited_.entities.size()) - 1;
        moving_ = false;
    }
    // True while the mouse is moving an entity; an edit is finished only when this ends.
    bool busy() const { return moving_; }
    const std::string& name() const { return name_; }

    // Draws the panels. Call once per frame between ImGui::NewFrame and ImGui::Render.
    void draw(const Assets& assets);
    // An error raised while drawing the view during ImGui's rendering, cleared by the call.
    std::string take_error() { return std::exchange(render_error_, {}); }

    bool show_scene{true}, show_hierarchy{true}, show_inspector{true};
    static constexpr const char* scene_id = "###Scene";
    static constexpr const char* hierarchy_id = "Hierarchy";
    static constexpr const char* inspector_id = "Inspector";

private:
    // Draws the scene into the Scene view's rectangle. Runs as an ImGui draw callback, so the
    // scene lands at its place in ImGui's draw order: under the view's overlays, and under any
    // window floating above the view.
    static void draw_callback(const ImDrawList*, const ImDrawCmd* command);
    void render();
    void sync(const Assets& assets); // Rebuilds the preview renderer when materials changed.
    void scene_view();
    void hierarchy();
    void inspector(const Assets& assets);
    void scene_menu();
    ImVec2 to_screen(WorldPosition position) const;
    WorldPosition to_world(ImVec2 screen) const;
    int pick(ImVec2 screen) const; // Topmost entity under a screen point, or -1.
    void create_at(WorldPosition position);
    void duplicate_selected();
    void delete_selected();
    void frame_selection();

    Log log_;
    std::filesystem::path folder_; // The project's scenes/ folder.
    std::string name_;
    std::vector<std::string> scene_names_;
    SceneFile saved_, edited_;
    int selected_{-1};

    // Preview renderer and the materials it was built from (textures replaced by generated tiles).
    std::unique_ptr<Renderer> renderer_;
    std::vector<MaterialAsset> rendered_;
    // Terrain preview: the compiled terrain, and sampled tiles cached for the area around the view.
    // Zoomed out, one sample stands for a block of tiles so the sprite count stays bounded.
    void draw_terrain(float half_w, float half_h);
    std::unique_ptr<Terrain> terrain_;
    TerrainAsset compiled_terrain_;
    std::string terrain_error_;
    bool show_terrain_{true};
    struct TerrainCache {
        ChunkCoord origin{}; // Cells are counted from this chunk's corner.
        int block{}, x0{}, y0{}, columns{}, rows{};
        std::uint32_t version{};
        std::uint64_t seed{};
        struct Cell {
            MaterialId ground;
            std::uint8_t object; // As Tile::object.
        };
        std::vector<Cell> cells;
    } cache_;
    float cache_ms_{}; // Time the last refill took, shown in the toolbar tooltip.
    Lighting game_lighting_;
    std::string renderer_error_, render_error_;

    WorldPosition camera_{};
    float zoom_{32}; // Logical pixels per world unit.
    bool lit_{};     // Game lighting instead of flat full brightness.
    ImVec2 view_min_{}, view_max_{};
    bool view_visible_{};
    bool moving_{};
    WorldPosition grab_{}; // Pointer position when a move started.
    WorldPosition grab_entity_{};
    WorldPosition menu_at_{}; // Where the context menu was opened.
    Scripts scripts_;
    std::string new_script_;
    std::string new_scene_;
    int focus_{};
};
} // namespace seed::editor
