#pragma once
#include "project/scene_file.hpp"
#include "project/terrain.hpp"
#include "render/renderer.hpp"
#include "scripts.hpp"
#include <filesystem>
#include <functional>
#include <imgui.h>
#include <map>
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
    bool dirty() const { return edited_ != saved_ || prefabs_ != saved_prefabs_; }
    // Saves the scene; false, logging why, when it has problems.
    bool save(const Assets& assets);
    void revert() {
        edited_ = saved_;
        prefabs_ = saved_prefabs_;
        clamp_selection();
    }
    const SceneFile& scene() const { return edited_; }
    // The project's prefabs by name, as edited (prefabs/<name>.json); saved with the scene.
    using Prefabs = std::map<std::string, SceneEntity>;
    const Prefabs& prefabs() const { return prefabs_; }
    void set_prefabs(const Prefabs& prefabs) { prefabs_ = prefabs; }
    // Replaces the edited scene, e.g. from undo.
    void set(const SceneFile& scene) {
        edited_ = scene;
        clamp_selection();
        drag_ = Drag::none;
    }
    // True while the mouse is changing the scene; an edit is finished only when this ends.
    bool busy() const { return drag_ != Drag::none && drag_ != Drag::box; }
    const std::string& name() const { return name_; }
    const std::vector<std::string>& scene_names() const { return scene_names_; }
    // The game window's logical size, for the Scene view's player camera frame.
    void set_game_view(int width, int height) {
        game_view_ = {static_cast<float>(width), static_cast<float>(height)};
    }
    const std::vector<int>& selection() const { return selection_; }
    void reveal_entity(const std::string& name);
    // Where a world position appears in the Scene view, in ImGui's screen coordinates.
    ImVec2 screen_of(WorldPosition position) const { return to_screen(position); }
    // Where the Hierarchy drew things last frame, for automated UI tests: each entity's row and its
    // Show and Lock checkboxes (x = -1 when filtered out), and the search box.
    struct Row {
        ImVec2 name{-1, -1}, show{-1, -1}, lock{-1, -1};
    };
    const std::vector<Row>& rows() const { return rows_; }
    ImVec2 search_box() const { return search_box_; }

    // Draws the panels. Call once per frame between ImGui::NewFrame and ImGui::Render. Returns true
    // when it changed `assets` (the Terrain object creates, renames and deletes terrains).
    bool draw(Assets& assets);
    // The project's cooked texture pack changed (or appeared, or went away): the preview reloads it.
    void set_texture_pack(const std::filesystem::path& pack) {
        pack_path_ = pack;
        ++pack_version_;
    }
    // Called to bring up the full terrain editor (the Terrain panel).
    std::function<void()> open_terrain_editor;
    // Called to show a material in the Materials panel.
    std::function<void(const std::string&)> open_material;
    // Called after the Inspector imported a texture, so the texture pack is rebuilt.
    std::function<void()> textures_changed;
    bool terrain_selected() const { return terrain_selected_; }
    bool atmosphere_selected() const { return atmosphere_selected_; }
    // An error raised while drawing the view during ImGui's rendering, cleared by the call.
    std::string take_error() { return std::exchange(render_error_, {}); }

    bool maximized{};
    bool show_scene{true}, show_hierarchy{true}, show_inspector{true}, show_prefabs{true};
    static constexpr const char* scene_id = "###Scene";
    static constexpr const char* hierarchy_id = "Hierarchy";
    static constexpr const char* inspector_id = "Inspector";
    static constexpr const char* prefabs_id = "Prefabs";

    // Where the Prefabs panel and the Inspector's prefab bar drew their controls last frame, for
    // automated UI tests.
    struct PrefabControls {
        ImVec2 new_from_selection{-1, -1}, apply{-1, -1};
        std::map<std::string, ImVec2> rows;
    };
    const PrefabControls& prefab_controls() const { return prefab_controls_; }
    // Other named controls drawn last frame ("create", "create player", "create npc", "is player").
    ImVec2 control(const std::string& name) const {
        const auto found = controls_.find(name);
        return found == controls_.end() ? ImVec2{-1, -1} : found->second;
    }

private:
    enum class Tool { move, rotate, scale, paint };
    // What a left-button drag in the Scene view is doing.
    enum class Drag { none, body, free, axis_x, axis_y, rotate, scale_x, scale_y, scale_both, box, paint };

    // Terrain painting. A freehand stroke paints the tiles under the brush each frame the button is
    // held; a rectangle or line paints its tiles when the button is let go; fill paints the
    // connected area of the clicked tile's ground and object.
    enum class Brush { ground, object, raise, lower, flatten, solid, erase };
    enum class Shape { freehand, rectangle, line, fill };
    struct BrushSettings {
        Brush kind{Brush::ground};
        Shape shape{Shape::freehand};
        float size{3};      // Tiles across.
        bool square{};      // Otherwise round.
        float strength{1};  // Ground and objects: the share of tiles painted; height: how fast.
        std::string ground; // Material painted by the ground brush.
        std::string object; // Object placed by the object brush; empty removes objects.
        // Further objects the object brush mixes in: each tile takes one of them or `object`.
        std::vector<std::string> mix;
        bool object_solid{true};
        float height{0.15F}; // Height the flatten brush levels to; below 0 is water.
        bool block{true};    // The solid brush blocks walking, or clears blocking.
    } brush_;
    unsigned stroke_{}; // Counts strokes, so partial-strength strokes pick different tiles.
    void brush_section(const Assets& assets);
    WorldPosition shape_from_{}; // Where a rectangle or line stroke began.
    void paint_at(WorldPosition centre, float dt);
    // Applies the brush to one tile; `falloff` (0 to 1) eases height brushes.
    void paint_tile(std::int64_t x, std::int64_t y, float falloff, float dt);
    // Rectangle and line strokes, from shape_from_ to `to`; fill from the tile at `at`.
    void paint_shape(WorldPosition to);
    void paint_fill(WorldPosition at);
    // Alt+click: the brush takes the ground or object of the tile under the pointer.
    void pick_brush(WorldPosition at);
    // The ground and object names shown at global tile (x, y), paint included; false outside the
    // cached view or when zoomed out past single tiles.
    bool shown_materials(std::int64_t x, std::int64_t y, std::string& ground, std::string& object) const;
    // Ground under global tile (x, y) as the view shows it, paint included; false when the tile is
    // outside the cached view.
    struct ViewTile {
        float elevation;
        bool solid;
    };
    bool view_tile(std::int64_t x, std::int64_t y, ViewTile& out) const;

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
    // Returns true when it changed `assets` (materials made or edited from the Visual section).
    bool inspector(Assets& assets);
    void multi_inspector(const Assets& assets);
    bool absolute_position_{};
    bool visual_material(SceneVisual& visual, Assets& assets);
    void character_section(SceneEntity& e, const Assets& assets);
    // The Inspector for the scene's Terrain object; returns true when it changed `assets`.
    bool terrain_inspector(Assets& assets);
    bool terrain_selected_{};
    // The Inspector for the scene's atmosphere: its light, haze and time of day.
    void atmosphere_inspector();
    bool atmosphere_selected_{};
    float preview_hour_{-1};   // Hour the Lit view shows instead of the scene's; -1 for the scene's.
    std::string terrain_name_; // Name being typed in the new/rename terrain popup.
    int terrain_action_{};     // What that popup does: 0 blank, 1 starter island, 2 copy, 3 rename.
    void scene_menu();
    void prefabs_panel();
    // Places a copy of a prefab at `at`, linked to it, and selects it.
    void place_prefab(const std::string& name, WorldPosition at);
    // Makes a prefab from the primary entity and links that entity to it.
    void make_prefab(const std::string& name);
    // Copies each linked prefab's components into the scene's placed copies.
    void sync_prefab_copies();
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
    // Creates a character: the player (unless the scene has one, which is then selected) or an NPC.
    void create_character(WorldPosition position, bool player, const Assets& assets);
    // Makes `index` the scene's one player, taking the role from any other character.
    void make_player(int index);
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
    std::filesystem::path pack_path_;
    int pack_version_{}, rendered_pack_version_{-1};
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
            bool solid;
            float elevation;
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

    WorldPosition menu_at_{};   // Where the context menu was opened.
    const Assets* assets_{};    // This frame's assets, for creating characters from menus.
    Vec2 game_view_{1280, 720}; // The game window's logical size, for the camera frame.
    Scripts scripts_;
    std::string new_script_;
    std::string new_scene_;
    std::string search_;
    Prefabs prefabs_, saved_prefabs_;
    std::string new_prefab_;
    PrefabControls prefab_controls_;
    std::map<std::string, ImVec2> controls_;
    void mark(const char* name); // Records where the last item was drawn, under `name`.
    int renaming_{-1};           // Entity whose name is being edited in the Hierarchy, or -1.
    std::string rename_text_;
    bool rename_focus_{};
    std::vector<Row> rows_;
    ImVec2 search_box_{-1, -1};
    int focus_{};
};
} // namespace seed::editor
