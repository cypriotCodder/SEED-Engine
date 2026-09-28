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
// selection outlines, markers and the move/rotate/scale handles are editor overlays drawn by
// ImGui on top.
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
    void revert() {
        edited_ = saved_;
        clamp_selection();
    }
    const SceneFile& scene() const { return edited_; }
    // Replaces the edited scene, e.g. from undo.
    void set(const SceneFile& scene) {
        edited_ = scene;
        clamp_selection();
        drag_ = Drag::none;
    }
    // True while the mouse is changing the scene; an edit is finished only when this ends.
    bool busy() const { return drag_ != Drag::none && drag_ != Drag::box; }
    const std::string& name() const { return name_; }
    const std::vector<int>& selection() const { return selection_; }
    // Where a world position appears in the Scene view, in ImGui's screen coordinates.
    ImVec2 screen_of(WorldPosition position) const { return to_screen(position); }
    // Where the Hierarchy drew things last frame, for automated UI tests: each entity's row and its
    // Show and Lock checkboxes (x = -1 when filtered out), and the search box.
    struct Row {
        ImVec2 name{-1, -1}, show{-1, -1}, lock{-1, -1};
    };
    const std::vector<Row>& rows() const { return rows_; }
    ImVec2 search_box() const { return search_box_; }

    // Draws the panels. Call once per frame between ImGui::NewFrame and ImGui::Render.
    void draw(const Assets& assets);
    // An error raised while drawing the view during ImGui's rendering, cleared by the call.
    std::string take_error() { return std::exchange(render_error_, {}); }

    bool show_scene{true}, show_hierarchy{true}, show_inspector{true};
    static constexpr const char* scene_id = "###Scene";
    static constexpr const char* hierarchy_id = "Hierarchy";
    static constexpr const char* inspector_id = "Inspector";

private:
    enum class Tool { move, rotate, scale };
    // What a left-button drag in the Scene view is doing.
    enum class Drag { none, body, free, axis_x, axis_y, rotate, scale_x, scale_y, scale_both, box };

    // Draws the scene into the Scene view's rectangle. Runs as an ImGui draw callback, so the
    // scene lands at its place in ImGui's draw order: under the view's overlays, and under any
    // window floating above the view.
    static void draw_callback(const ImDrawList*, const ImDrawCmd* command);
    void render();
    void sync(const Assets& assets); // Rebuilds the preview renderer when materials changed.
    void scene_view();
    void toolbar();
    void overlays(ImDrawList* draw);
    void hierarchy();
    void inspector(const Assets& assets);
    void scene_menu();
    // Moves the entities in `moving` (sorted indices) to just before `target` (size() for the end),
    // keeping their order and the selection.
    void reorder(std::vector<int> moving, int target);
    ImVec2 to_screen(WorldPosition position) const;
    WorldPosition to_world(ImVec2 screen) const;
    int pick(ImVec2 screen) const; // Topmost pickable entity under a screen point, or -1.

    // Selection: sorted, unique entity indices; `primary_` is the one the Inspector shows in full.
    bool is_selected(int index) const;
    void select_only(int index);
    void toggle(int index);
    void clear_selection();
    void clamp_selection();
    bool selection_editable() const; // Nothing hidden or locked is selected.
    WorldPosition selection_centre() const;

    // Handles and drags.
    Drag handle_under(ImVec2 mouse) const;
    void begin_drag(Drag drag, ImVec2 mouse);
    void continue_drag(ImVec2 mouse);
    float snap_step() const; // World units; 0 when snapping is off.

    void create_at(WorldPosition position);
    void duplicate_selection();
    void delete_selection();
    void frame_selection();
    void copy_selection() const;
    void paste();

    Log log_;
    std::filesystem::path folder_; // The project's scenes/ folder.
    std::string name_;
    std::vector<std::string> scene_names_;
    SceneFile saved_, edited_;
    std::vector<int> selection_;
    int primary_{-1};

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

    Tool tool_{Tool::move};
    int snap_{0}; // Index into the snap steps: off, 1/4, 1/2, 1 tile.
    Drag drag_{Drag::none};
    WorldPosition grab_{};  // Pointer position when the drag started.
    WorldPosition pivot_{}; // Centre of the selection when the drag started.
    ImVec2 grab_screen_{};
    struct Grabbed {
        WorldPosition position;
        float angle;
        Vec2 size;
    };
    std::vector<Grabbed> grabbed_; // Per selected entity, at the drag's start.
    bool box_additive_{};

    WorldPosition menu_at_{}; // Where the context menu was opened.
    Scripts scripts_;
    std::string new_script_;
    std::string new_scene_;
    std::string search_;
    int renaming_{-1}; // Entity whose name is being edited in the Hierarchy, or -1.
    std::string rename_text_;
    bool rename_focus_{};
    std::vector<Row> rows_;
    ImVec2 search_box_{-1, -1};
    int focus_{};
};
} // namespace seed::editor
