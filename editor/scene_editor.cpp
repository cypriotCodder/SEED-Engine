#include "scene_editor.hpp"
#include "folder_dialog.hpp"
#include "io/storage.hpp"
#include "project.hpp"
#include "selection_edit.hpp"
#include "starter.hpp"
#include "textures.hpp"
#include "widgets.hpp"
#include "world/noise.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <imgui_stdlib.h>
#include <optional>
#include <set>

namespace seed::editor {
namespace {
namespace fs = std::filesystem;
constexpr float marker_size = 0.6F;              // World size of entities without a visual.
constexpr float min_zoom = 0.1F, max_zoom = 256; // 0.1: a 300-chunk world fits on screen.
constexpr std::uint64_t view_reach = 512;        // Chunks from the camera that the view considers.
constexpr float pi = 3.14159265F;
// Handle geometry, in logical pixels so handles keep their size at every zoom.
constexpr float arm = 72, grip = 7, ring = 62, reach = 7;
constexpr float snap_steps[] = {0, 0.25F, 0.5F, 1};
constexpr const char* snap_labels[] = {"Snap off", "Snap 1/4", "Snap 1/2", "Snap 1"};
constexpr float angle_step = pi / 12; // 15 degrees, when snapping.

Vec2 extent(const SceneEntity& e) {
    return e.visual ? e.visual->size : Vec2{marker_size, marker_size};
}
ImU32 rgba(float r, float g, float b, float a) {
    return ImGui::ColorConvertFloat4ToU32({r, g, b, a});
}
std::string title(const char* name, bool edited, const char* id) {
    return std::string(name) + (edited ? " *" : "") + id;
}
float distance_to_segment(ImVec2 p, ImVec2 a, ImVec2 b) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float t = std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / (dx * dx + dy * dy), 0.0F, 1.0F);
    const float x = a.x + t * dx - p.x, y = a.y + t * dy - p.y;
    return std::sqrt(x * x + y * y);
}
bool inside_square(ImVec2 p, ImVec2 centre, float half) {
    return std::abs(p.x - centre.x) <= half && std::abs(p.y - centre.y) <= half;
}
float snap_to(float value, float step) {
    return step > 0 ? std::round(value / step) * step : value;
}
// The average of some positions, measured from the first so it stays exact far from the origin.
template<class Positions>
WorldPosition centre_of(const Positions& positions) {
    const WorldPosition first = *positions.begin();
    Vec2 sum{};
    std::size_t count = 0;
    for (const WorldPosition& p : positions) {
        sum = sum + relative(p, first);
        ++count;
    }
    auto centre = first;
    centre.move(sum * (1.0F / static_cast<float>(count)));
    return centre;
}
} // namespace

SceneEditor::SceneEditor(Log log) : log_(std::move(log)) {}
SceneEditor::~SceneEditor() = default;

void SceneEditor::load(const fs::path& project, const std::string& name) {
    const auto folder = project / "scenes";
    const auto file = folder / (name + ".json");
    SceneFile scene = fs::exists(file) ? load_scene(file) : SceneFile{};
    folder_ = folder;
    name_ = name;
    saved_ = edited_ = std::move(scene);
    // Prefabs belong to the project; a broken file is reported and left out.
    prefabs_.clear();
    std::error_code prefab_error;
    for (const auto& entry : fs::directory_iterator(project / "prefabs", prefab_error)) {
        const auto name = entry.path().stem().string();
        if (entry.path().extension() != ".json" || !valid_prefab_name(name)) continue;
        try {
            auto prefab = parse_prefab(parse_json(read_text(entry.path())));
            prefab.name = name;
            prefabs_[name] = prefab;
        } catch (const std::exception& failure) {
            log_(true, "prefabs/" + entry.path().filename().string() + ": " + failure.what());
        }
    }
    saved_prefabs_ = prefabs_;
    // Placed copies follow their prefab, even if it changed while this scene was closed.
    sync_prefab_copies();
    saved_ = edited_;
    clear_selection();
    if (!edited_.entities.empty()) select_only(0);
    camera_ = {};
    drag_ = Drag::none;
    focus_ = 3;
    scene_names_.clear();
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(folder_, error))
        if (entry.path().extension() == ".json") scene_names_.push_back(entry.path().stem().string());
    if (std::find(scene_names_.begin(), scene_names_.end(), name_) == scene_names_.end())
        scene_names_.push_back(name_);
    std::sort(scene_names_.begin(), scene_names_.end());
}

void SceneEditor::unload() {
    folder_.clear();
    name_.clear();
    saved_ = edited_ = {};
    prefabs_.clear();
    saved_prefabs_.clear();
    clear_selection();
    view_visible_ = false;
}

bool SceneEditor::save(const Assets& assets) {
    if (!dirty()) return true;
    SceneFile check = edited_;
    for (const auto& [name, prefab] : prefabs_)
        check.entities.push_back(prefab);
    if (const auto problems = check.problems(assets); !problems.empty()) {
        log_(true, "Scene \"" + name_ + "\" not saved. Fix these first:\n" + problems);
        return false;
    }
    const auto folder = folder_.parent_path() / "prefabs";
    if (prefabs_ != saved_prefabs_) {
        fs::create_directories(folder);
        for (const auto& [name, prefab] : prefabs_)
            if (!saved_prefabs_.count(name) || !(saved_prefabs_.at(name) == prefab))
                write_text(folder / (name + ".json"), to_json(prefab_json(prefab)));
        for (const auto& [name, prefab] : saved_prefabs_)
            if (!prefabs_.count(name)) fs::remove(folder / (name + ".json")); // Deleted in the Prefabs panel.
        saved_prefabs_ = prefabs_;
        log_(false, "Saved prefabs.");
    }
    if (edited_ != saved_) {
        save_scene(folder_ / (name_ + ".json"), edited_);
        saved_ = edited_;
        log_(false, "Saved scene \"" + name_ + "\".");
    }
    return true;
}

void SceneEditor::sync_prefab_copies() {
    for (auto& e : edited_.entities)
        if (const auto found = prefabs_.find(e.prefab); !e.prefab.empty() && found != prefabs_.end())
            apply_prefab(found->second, e);
}

void SceneEditor::place_prefab(const std::string& name, WorldPosition at) {
    const auto found = prefabs_.find(name);
    if (found == prefabs_.end()) return;
    if (edited_.entities.size() >= SceneFile::capacity)
        return log_(true, "A scene holds at most 4096 entities.");
    auto entity = found->second;
    entity.prefab = name;
    entity.position = at;
    // Named after the prefab, numbered when the name is taken.
    const auto taken = [&](const std::string& candidate) {
        return std::any_of(edited_.entities.begin(), edited_.entities.end(),
                           [&](const SceneEntity& e) { return e.name == candidate; });
    };
    entity.name = name;
    for (int n = 2; taken(entity.name); ++n)
        entity.name = name + " " + std::to_string(n);
    edited_.entities.push_back(entity);
    select_only(static_cast<int>(edited_.entities.size()) - 1);
}

void SceneEditor::make_prefab(const std::string& name) {
    if (primary_ < 0 || !valid_prefab_name(name) || prefabs_.count(name)) return;
    auto& e = edited_.entities[static_cast<std::size_t>(primary_)];
    auto prefab = e;
    prefab.name = name;
    prefab.position = {};
    prefab.angle = 0;
    prefab.prefab.clear();
    prefab.hidden = prefab.locked = false;
    prefabs_[name] = prefab;
    e.prefab = name;
    log_(false, "Made prefab \"" + name + "\" from \"" + e.name +
                    "\"; drag it from the Prefabs panel to place copies.");
}

void SceneEditor::sync(const Assets& assets) {
    const bool materials_changed = !(assets.materials == rendered_ && (renderer_ || rendered_.empty())) ||
                                   pack_version_ != rendered_pack_version_;
    // The scene's own terrain; a name the project lacks shows flat ground.
    const auto found = assets.terrains.find(edited_.terrain);
    const TerrainAsset terrain = found == assets.terrains.end() ? TerrainAsset{} : found->second;
    if (materials_changed || terrain != compiled_terrain_) {
        // Recompile the terrain against the preview's material order.
        compiled_terrain_ = terrain;
        terrain_.reset();
        terrain_error_.clear();
        cache_.cells.clear();
        if (terrain.enabled() && !assets.materials.empty()) try {
                Materials registry;
                Assets names;
                names.materials = assets.materials;
                for (auto& m : names.materials)
                    m.texture.clear();
                names.register_materials(registry);
                terrain_ = std::make_unique<Terrain>(terrain, registry);
            } catch (const std::exception&) {
                terrain_error_ = "Terrain not shown: fix the problems in the Terrain panel.";
            }
    }
    if (!materials_changed) return;
    rendered_ = assets.materials;
    renderer_.reset();
    renderer_error_.clear();
    if (rendered_.empty()) return;
    rendered_pack_version_ = pack_version_;
    try {
        // The preview draws the project's cooked textures; a material whose texture is not cooked
        // (a missing file, or not yet imported) previews as its generated tile.
        const auto pack = !pack_path_.empty() && fs::exists(pack_path_) ? std::make_unique<Pack>(pack_path_)
                                                                        : std::make_unique<Pack>();
        Assets preview;
        preview.materials = rendered_;
        for (auto& material : preview.materials)
            if (!material.texture.empty() && !pack->has(material.texture)) material.texture.clear();
        Materials registry;
        preview.register_materials(registry);
        renderer_ = std::make_unique<Renderer>(*pack, registry);
        game_lighting_ = renderer_->lighting;
    } catch (const std::exception& error) {
        renderer_error_ = error.what(); // Usually a material being edited; fixed on the next change.
    }
}

ImVec2 SceneEditor::to_screen(WorldPosition position) const {
    const auto r = relative(position, camera_);
    const ImVec2 center{(view_min_.x + view_max_.x) / 2, (view_min_.y + view_max_.y) / 2};
    return {center.x + r.x * zoom_, center.y - r.y * zoom_};
}

WorldPosition SceneEditor::to_world(ImVec2 screen) const {
    const ImVec2 center{(view_min_.x + view_max_.x) / 2, (view_min_.y + view_max_.y) / 2};
    auto p = camera_;
    p.move({(screen.x - center.x) / zoom_, (center.y - screen.y) / zoom_});
    return p;
}

int SceneEditor::pick(ImVec2 screen) const {
    const auto point = to_world(screen);
    for (int i = static_cast<int>(edited_.entities.size()) - 1; i >= 0; --i) {
        const auto& e = edited_.entities[static_cast<std::size_t>(i)];
        if (e.hidden || e.locked || !nearby(e.position.chunk, camera_.chunk, view_reach)) continue;
        const auto r = relative(point, e.position);
        // Into the entity's own frame: rotate by -angle.
        const float c = std::cos(-e.angle), s = std::sin(-e.angle);
        const float x = r.x * c - r.y * s, y = r.x * s + r.y * c;
        const auto size = extent(e);
        if (std::abs(x) <= size.x / 2 && std::abs(y) <= size.y / 2) return i;
    }
    return -1;
}

bool SceneEditor::is_selected(int index) const {
    return std::binary_search(selection_.begin(), selection_.end(), index);
}

void SceneEditor::select_only(int index) {
    selection_ = {index};
    primary_ = index;
    terrain_selected_ = atmosphere_selected_ = false;
}

void SceneEditor::toggle(int index) {
    terrain_selected_ = atmosphere_selected_ = false;
    const auto at = std::lower_bound(selection_.begin(), selection_.end(), index);
    if (at != selection_.end() && *at == index) {
        selection_.erase(at);
        if (primary_ == index) primary_ = selection_.empty() ? -1 : selection_.back();
    } else {
        selection_.insert(at, index);
        primary_ = index;
    }
}

void SceneEditor::clear_selection() {
    selection_.clear();
    primary_ = -1;
}

void SceneEditor::clamp_selection() {
    const int count = static_cast<int>(edited_.entities.size());
    std::erase_if(selection_, [&](int i) { return i >= count; });
    if (!is_selected(primary_)) primary_ = selection_.empty() ? -1 : selection_.back();
}

bool SceneEditor::selection_editable() const {
    return !selection_.empty() && std::none_of(selection_.begin(), selection_.end(), [&](int i) {
        const auto& e = edited_.entities[static_cast<std::size_t>(i)];
        return e.hidden || e.locked;
    });
}

WorldPosition SceneEditor::selection_centre() const {
    std::vector<WorldPosition> positions;
    for (const int i : selection_)
        positions.push_back(edited_.entities[static_cast<std::size_t>(i)].position);
    return centre_of(positions);
}

float SceneEditor::snap_step() const {
    return snap_steps[snap_];
}

void SceneEditor::create_at(WorldPosition position) {
    if (edited_.entities.size() >= SceneFile::capacity)
        return log_(true, "A scene holds at most 4096 entities.");
    SceneEntity entity;
    int n = static_cast<int>(edited_.entities.size()) + 1;
    auto taken = [&](const std::string& name) {
        return std::any_of(edited_.entities.begin(), edited_.entities.end(),
                           [&](const SceneEntity& e) { return e.name == name; });
    };
    do
        entity.name = "Entity " + std::to_string(n++);
    while (taken(entity.name));
    entity.position = position;
    edited_.entities.push_back(entity);
    select_only(static_cast<int>(edited_.entities.size()) - 1);
}

void SceneEditor::mark(const char* name) {
    controls_[name] = {(ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) / 2,
                       (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) / 2};
}

void SceneEditor::make_player(int index) {
    for (int i = 0; i < static_cast<int>(edited_.entities.size()); ++i) {
        auto& e = edited_.entities[static_cast<std::size_t>(i)];
        if (i != index && e.character && e.character->player) {
            e.character->player.reset();
            log_(false, "\"" + e.name + "\" is now an NPC; the scene has one player.");
        }
    }
    auto& chosen = edited_.entities[static_cast<std::size_t>(index)];
    if (!chosen.character) chosen.character = SceneCharacter{};
    if (!chosen.character->player) chosen.character->player = ScenePlayer{};
}

void SceneEditor::create_character(WorldPosition position, bool player, const Assets& assets) {
    if (player)
        for (int i = 0; i < static_cast<int>(edited_.entities.size()); ++i) {
            const auto& e = edited_.entities[static_cast<std::size_t>(i)];
            if (e.character && e.character->player) {
                select_only(i);
                frame_selection();
                return log_(false,
                            "This scene already has a player, \"" + e.name + "\"; it is now selected.");
            }
        }
    create_at(position);
    auto& e = edited_.entities.back();
    int n = 1;
    const auto taken = [&](const std::string& name) {
        return std::any_of(edited_.entities.begin(), edited_.entities.end(),
                           [&](const SceneEntity& other) { return &other != &e && other.name == name; });
    };
    e.name = player ? "Player" : "NPC";
    while (taken(e.name))
        e.name = (player ? "Player " : "NPC ") + std::to_string(++n);
    if (!assets.materials.empty()) e.visual = SceneVisual{assets.materials.front().name, {0.6F, 0.6F}};
    e.character = SceneCharacter{};
    if (player) make_player(static_cast<int>(edited_.entities.size()) - 1);
}

void SceneEditor::duplicate_selection() {
    if (selection_.empty()) return;
    if (edited_.entities.size() + selection_.size() > SceneFile::capacity)
        return log_(true, "A scene holds at most 4096 entities.");
    // Copies go to the end, keeping their order, a tile down and right so they are visible; they
    // become the selection.
    std::vector<int> copies;
    int new_primary = -1;
    for (const int i : selection_) {
        auto copy = edited_.entities[static_cast<std::size_t>(i)];
        copy.position.move({1, -1});
        copy.hidden = copy.locked = false;
        if (copy.character) copy.character->player.reset(); // Copies of the player are NPCs.
        edited_.entities.push_back(copy);
        copies.push_back(static_cast<int>(edited_.entities.size()) - 1);
        if (i == primary_) new_primary = copies.back();
    }
    selection_ = copies;
    primary_ = new_primary >= 0 ? new_primary : copies.back();
}

void SceneEditor::delete_selection() {
    for (auto i = selection_.rbegin(); i != selection_.rend(); ++i)
        edited_.entities.erase(edited_.entities.begin() + *i);
    clear_selection();
}

void SceneEditor::frame_selection() {
    if (!selection_.empty()) camera_ = selection_centre();
}

void SceneEditor::copy_selection() const {
    // The clipboard carries plain scene JSON, so entities paste into other scenes, other projects
    // and other editor windows.
    if (selection_.empty()) return;
    SceneFile clip;
    for (const int i : selection_)
        clip.entities.push_back(edited_.entities[static_cast<std::size_t>(i)]);
    ImGui::SetClipboardText(to_json(scene_json(clip)).c_str());
}

void SceneEditor::paste() {
    const char* text = ImGui::GetClipboardText();
    if (!text || !*text) return;
    SceneFile clip;
    try {
        clip = parse_scene(parse_json(text));
    } catch (const std::exception&) {
        return; // The clipboard holds something else: nothing to paste.
    }
    if (clip.entities.empty()) return;
    if (edited_.entities.size() + clip.entities.size() > SceneFile::capacity)
        return log_(true, "A scene holds at most 4096 entities.");
    std::vector<WorldPosition> positions;
    for (const auto& e : clip.entities)
        positions.push_back(e.position);
    const auto centre = centre_of(positions);
    // Pasted where they were, a tile down and right, when that is in view; otherwise centred in
    // the view, keeping their arrangement.
    const float half_w = (view_max_.x - view_min_.x) / 2 / zoom_,
                half_h = (view_max_.y - view_min_.y) / 2 / zoom_;
    bool in_view = nearby(centre.chunk, camera_.chunk, 64);
    if (in_view) {
        const auto offset = relative(centre, camera_);
        in_view = std::abs(offset.x) < half_w && std::abs(offset.y) < half_h;
    }
    selection_.clear();
    for (auto e : clip.entities) {
        if (in_view)
            e.position.move({1, -1});
        else {
            // Measured from the group's centre, which is always a short, exact distance.
            const auto from_centre = relative(e.position, centre);
            e.position = camera_;
            e.position.move(from_centre);
        }
        e.hidden = e.locked = false;
        if (e.character) e.character->player.reset(); // Pasted characters are NPCs.
        edited_.entities.push_back(e);
        selection_.push_back(static_cast<int>(edited_.entities.size()) - 1);
    }
    primary_ = selection_.back();
}

bool SceneEditor::draw(Assets& assets) {
    if (!loaded()) return false;
    assets_ = &assets;
    sync(assets);
    clamp_selection();
    if (show_scene || maximized)
        scene_view();
    else
        view_visible_ = false;
    if (maximized) return false;
    if (show_hierarchy) hierarchy();
    bool changed = false;
    if (show_inspector) {
        if (terrain_selected_)
            changed = terrain_inspector(assets);
        else if (atmosphere_selected_)
            atmosphere_inspector();
        else
            changed = inspector(assets);
    }
    if (show_prefabs) prefabs_panel();
    return changed;
}

void SceneEditor::atmosphere_inspector() {
    if (!ImGui::Begin(inspector_id, &show_inspector)) {
        ImGui::End();
        return;
    }
    auto& a = edited_.atmosphere;
    ImGui::TextColored({0.45F, 0.9F, 0.65F, 1}, "Atmosphere");
    ImGui::TextDisabled("This scene's light and air.");
    if (!lit_) {
        if (ImGui::Button("Show in the Scene View (Lit)", {-1, 0})) lit_ = true;
        mark("atmosphere lit");
    }
    const auto color = [&](const char* label, const char* id, std::array<float, 3>& value, const char* tip) {
        ImGui::TextUnformatted(label);
        ImGui::SetNextItemWidth(-1);
        ImGui::ColorEdit3(id, value.data(), ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
        ImGui::SetItemTooltip("%s", tip);
    };
    ImGui::SeparatorText("Light");
    color("Ambient (day)", "##ambient", a.ambient,
          "Light present everywhere in daytime, before lights add their own.");
    color("Background", "##background", a.background,
          "The colour behind everything, where nothing is drawn.");
    ImGui::SeparatorText("Haze");
    color("Haze colour", "##haze", a.haze, "Blended in towards the edges of the view, like distance fading.");
    ImGui::TextUnformatted("Haze amount");
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##haze amount", &a.haze_amount, 0, 1);

    ImGui::SeparatorText("Day and night");
    bool cycle = a.day_length > 0;
    if (ImGui::Checkbox("Day turns into night", &cycle)) a.day_length = cycle ? 300 : 0;
    mark("day cycle");
    ImGui::SetItemTooltip(
        "Time passes while the game runs; scripts can read and set it with atmosphere.hour().");
    if (cycle) {
        ImGui::TextUnformatted("Length of a day (seconds)");
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##day length", &a.day_length, 10, 3600, "%.0f", ImGuiSliderFlags_Logarithmic);
    }
    ImGui::TextUnformatted(cycle ? "Starting hour" : "Hour");
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##hour", &a.hour, 0, 24, "%.1f");
    ImGui::SetItemTooltip("12 is noon, 0 is midnight. Full daylight is 9 to 15, full night 21 to 3.");
    color("Ambient (night)", "##night", a.night, "Light present everywhere at night.");

    ImGui::SeparatorText("Preview");
    bool previewing = preview_hour_ >= 0;
    if (ImGui::Checkbox("Preview another hour", &previewing)) preview_hour_ = previewing ? a.hour : -1;
    ImGui::SetItemTooltip("Shows the Scene view at another time of day without changing the scene.");
    if (previewing) {
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##preview hour", &preview_hour_, 0, 24, "%.1f");
    }
    if (const auto problems = a.problems(); !problems.empty())
        ImGui::TextColored({0.95F, 0.55F, 0.4F, 1}, "%s", problems.c_str());
    if (ImGui::Button("Reset to Defaults", {-1, 0})) a = {};
    ImGui::End();
}

bool SceneEditor::terrain_inspector(Assets& assets) {
    if (!ImGui::Begin(inspector_id, &show_inspector)) {
        ImGui::End();
        return false;
    }
    bool changed = false;
    auto& terrains = assets.terrains;
    const auto found = terrains.find(edited_.terrain);
    ImGui::TextColored({0.45F, 0.9F, 0.65F, 1}, "Terrain");
    ImGui::TextDisabled("The ground this scene's world is generated from.");
    ImGui::TextUnformatted("Terrain used by this scene");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##terrain", edited_.terrain.c_str())) {
        for (const auto& [name, terrain] : terrains)
            if (ImGui::Selectable(name.c_str(), name == edited_.terrain)) edited_.terrain = name;
        ImGui::EndCombo();
    }
    mark("terrain choice");
    if (found == terrains.end())
        ImGui::TextColored({0.95F, 0.65F, 0.35F, 1}, "\"%s\" does not exist yet, so the ground is flat.",
                           edited_.terrain.c_str());

    // New, copy and rename share one name popup. It is opened below, at this window's level: a
    // popup opened from inside the New menu would belong to that menu and never appear.
    bool ask = false;
    const auto ask_name = [&](int action, const std::string& suggestion) {
        terrain_action_ = action;
        terrain_name_ = suggestion;
        for (int n = 2; terrains.count(terrain_name_); ++n)
            terrain_name_ = suggestion + "_" + std::to_string(n);
        ask = true;
    };
    if (ImGui::Button("New")) ImGui::OpenPopup("new terrain");
    mark("terrain new");
    if (ImGui::BeginPopup("new terrain")) {
        if (ImGui::MenuItem("Starter Island")) ask_name(1, "island");
        mark("terrain new starter");
        if (ImGui::MenuItem("Blank")) ask_name(0, "terrain");
        mark("terrain new blank");
        if (found != terrains.end() && ImGui::MenuItem("Copy of This Terrain")) ask_name(2, edited_.terrain);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(found == terrains.end());
    if (ImGui::Button("Rename")) ask_name(3, edited_.terrain + "_renamed");
    ImGui::SameLine();
    if (ImGui::Button("Delete")) ImGui::OpenPopup("delete terrain");
    ImGui::EndDisabled();
    if (ask) ImGui::OpenPopup("terrain name");
    if (ImGui::BeginPopup("terrain name")) {
        const char* titles[] = {"New blank terrain", "New starter island", "Copy of this terrain",
                                "Rename terrain"};
        ImGui::TextUnformatted(titles[terrain_action_]);
        ImGui::SetNextItemWidth(220);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##name", &terrain_name_, ImGuiInputTextFlags_EnterReturnsTrue);
        const bool valid = valid_terrain_name(terrain_name_), taken = terrains.count(terrain_name_) > 0;
        if (!valid) ImGui::TextColored({0.95F, 0.65F, 0.35F, 1}, "Use letters, digits, '_' and '-'.");
        if (taken) ImGui::TextColored({0.95F, 0.65F, 0.35F, 1}, "A terrain with this name exists.");
        ImGui::BeginDisabled(!valid || taken);
        if (ImGui::Button("OK", {220, 0}) || (enter && valid && !taken)) {
            const auto old_name = edited_.terrain;
            switch (terrain_action_) {
            case 0:
                terrains[terrain_name_] = {};
                break;
            case 1:
                starter_island(assets, terrain_name_);
                break;
            case 2:
                terrains[terrain_name_] = terrains.at(old_name);
                break;
            default:
                terrains[terrain_name_] = terrains.at(old_name);
                terrains.erase(old_name);
                log_(false, "Renamed terrain \"" + old_name + "\" to \"" + terrain_name_ +
                                "\". Other scenes that used it need it picked again.");
            }
            edited_.terrain = terrain_name_; // The new terrain is the one this scene uses.
            changed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        mark("terrain name ok");
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("delete terrain")) {
        ImGui::Text("Delete the terrain \"%s\"?", edited_.terrain.c_str());
        ImGui::TextDisabled("Scenes that use it get flat ground until another is picked.");
        if (ImGui::Button("Delete", {110, 0})) {
            terrains.erase(edited_.terrain);
            edited_.terrain = terrains.empty() ? "main" : terrains.begin()->first;
            changed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {110, 0})) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (const auto current = terrains.find(edited_.terrain); current != terrains.end()) {
        const auto& t = current->second;
        ImGui::SeparatorText("Summary");
        ImGui::Text("%s, %.0f chunks across", t.island ? "Island" : "Endless world", t.radius * 2);
        ImGui::Text("Seed %llu", static_cast<unsigned long long>(t.default_seed));
        ImGui::Text("%zu fields, %zu rules", t.fields.size(), t.rules.size());
        if (ImGui::Button("Edit Fields and Rules", {-1, 0}) && open_terrain_editor) open_terrain_editor();
        ImGui::SetItemTooltip(
            "Opens the Terrain panel: shape, noise fields and the rules that pick materials.");
        brush_section(assets);
    } else if (tool_ == Tool::paint)
        ImGui::TextDisabled("Create a terrain to paint on it.");
    ImGui::End();
    return changed;
}

SceneEditor::Drag SceneEditor::handle_under(ImVec2 mouse) const {
    if (!selection_editable()) return Drag::none;
    const auto c = to_screen(selection_centre());
    const ImVec2 x_end{c.x + arm, c.y}, y_end{c.x, c.y - arm};
    switch (tool_) {
    case Tool::move:
        if (inside_square(mouse, c, grip + 2)) return Drag::free;
        if (distance_to_segment(mouse, c, x_end) <= reach) return Drag::axis_x;
        if (distance_to_segment(mouse, c, y_end) <= reach) return Drag::axis_y;
        break;
    case Tool::rotate: {
        const float dx = mouse.x - c.x, dy = mouse.y - c.y;
        if (std::abs(std::sqrt(dx * dx + dy * dy) - ring) <= reach) return Drag::rotate;
        break;
    }
    case Tool::scale:
        if (inside_square(mouse, c, grip + 2)) return Drag::scale_both;
        if (inside_square(mouse, x_end, grip + 2) || distance_to_segment(mouse, c, x_end) <= reach)
            return Drag::scale_x;
        if (inside_square(mouse, y_end, grip + 2) || distance_to_segment(mouse, c, y_end) <= reach)
            return Drag::scale_y;
        break;
    case Tool::paint:
        break;
    }
    return Drag::none;
}

void SceneEditor::begin_drag(Drag drag, ImVec2 mouse) {
    drag_ = drag;
    grab_ = to_world(mouse);
    grab_screen_ = mouse;
    if (drag == Drag::box) return;
    pivot_ = selection_centre();
    grabbed_.clear();
    for (const int i : selection_) {
        const auto& e = edited_.entities[static_cast<std::size_t>(i)];
        grabbed_.push_back({e.position, e.angle, e.visual ? e.visual->size : Vec2{}});
    }
}

void SceneEditor::continue_drag(ImVec2 mouse) {
    // Shift flips snapping for this drag: off when it is on, half tiles when it is off.
    const float step = ImGui::GetIO().KeyShift ? (snap_ ? 0.0F : 0.5F) : snap_step();
    const auto pointer = to_world(mouse);
    const auto moved = relative(pointer, grab_);
    const bool single = selection_.size() == 1;
    const auto c = to_screen(pivot_);
    for (std::size_t k = 0; k < selection_.size() && k < grabbed_.size(); ++k) {
        auto& e = edited_.entities[static_cast<std::size_t>(selection_[k])];
        const auto& g = grabbed_[k];
        switch (drag_) {
        case Drag::body:
        case Drag::free:
        case Drag::axis_x:
        case Drag::axis_y: {
            const Vec2 delta{drag_ == Drag::axis_y ? 0 : moved.x, drag_ == Drag::axis_x ? 0 : moved.y};
            auto target = g.position;
            if (single && step > 0) {
                // One entity lands on the grid; a group keeps its spacing and moves in grid steps.
                target.move(delta);
                double x = global_coordinate(target.chunk.x, target.local.x);
                double y = global_coordinate(target.chunk.y, target.local.y);
                if (drag_ != Drag::axis_y) x = std::round(x / step) * step;
                if (drag_ != Drag::axis_x) y = std::round(y / step) * step;
                target = from_global(x, y);
            } else
                target.move({snap_to(delta.x, step), snap_to(delta.y, step)});
            e.position = target;
            break;
        }
        case Drag::rotate: {
            const auto from = relative(grab_, pivot_), to = relative(pointer, pivot_);
            float turn = std::atan2(to.y, to.x) - std::atan2(from.y, from.x);
            if (step > 0) turn = snap_to(turn, angle_step);
            e.angle = g.angle + turn;
            // A group turns about its centre.
            const auto offset = relative(g.position, pivot_);
            const float cs = std::cos(turn), sn = std::sin(turn);
            e.position = pivot_;
            e.position.move({offset.x * cs - offset.y * sn, offset.x * sn + offset.y * cs});
            break;
        }
        case Drag::scale_x:
        case Drag::scale_y:
        case Drag::scale_both: {
            if (!e.visual) break; // Only visuals have a size.
            // Factors from how far the pointer is from the centre now, against where it grabbed.
            const float fx = (mouse.x - c.x) / std::max(1.0F, grab_screen_.x - c.x);
            const float fy = (c.y - mouse.y) / std::max(1.0F, c.y - grab_screen_.y);
            const float gx = grab_screen_.x - c.x, gy = grab_screen_.y - c.y;
            const float dx = mouse.x - c.x, dy = mouse.y - c.y;
            const float both = std::sqrt(dx * dx + dy * dy) / std::max(1.0F, std::sqrt(gx * gx + gy * gy));
            Vec2 size = g.size;
            if (drag_ != Drag::scale_y) size.x *= drag_ == Drag::scale_both ? both : fx;
            if (drag_ != Drag::scale_x) size.y *= drag_ == Drag::scale_both ? both : fy;
            const float smallest = std::max(step, 0.05F);
            e.visual->size = {std::clamp(snap_to(size.x, step), smallest, 64.0F),
                              std::clamp(snap_to(size.y, step), smallest, 64.0F)};
            break;
        }
        case Drag::none:
        case Drag::box:
        case Drag::paint:
            break;
        }
    }
}

void SceneEditor::toolbar() {
    const auto tool_button = [&](const char* label, Tool tool, const char* tip) {
        const bool active = tool_ == tool;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (tool != Tool::move) toolbar_next(button_width(label));
        if (ImGui::Button(label)) tool_ = tool;
        if (active) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s", tip);
    };
    tool_button("Move", Tool::move,
                "Drag the arrows to move along an axis, or the centre to move freely (W).");
    tool_button("Rotate", Tool::rotate, "Drag the ring to turn the selection about its centre (E).");
    tool_button("Scale", Tool::scale,
                "Drag an end to resize along that axis, or the centre to resize evenly (R).");
    const bool was_painting = tool_ == Tool::paint;
    tool_button(
        "Paint", Tool::paint,
        "Paint the terrain: ground, objects, height and blocking (B).\nBrush settings are in the Inspector.");
    mark("paint tool");
    if (tool_ == Tool::paint && !was_painting) {
        clear_selection();
        terrain_selected_ = true;
        atmosphere_selected_ = false;
    }
    const float snap_width = ImGui::GetFontSize() * 7;
    toolbar_next(snap_width);
    ImGui::SetNextItemWidth(snap_width);
    ImGui::Combo("##snap", &snap_, snap_labels, 4);
    ImGui::SetItemTooltip("Moves land on this grid, sizes use it, and rotation snaps to 15 degrees.\n"
                          "Hold Shift while dragging to flip snapping.");
    toolbar_next(button_width("View"));
    if (ImGui::Button("View")) ImGui::OpenPopup("scene display");
    ImGui::SetItemTooltip("Lighting and terrain display options.");
    if (ImGui::BeginPopup("scene display")) {
        ImGui::Checkbox("Lit", &lit_);
        ImGui::SetItemTooltip("Show the game's lighting and lights instead of flat full brightness.");
        ImGui::Checkbox("Terrain", &show_terrain_);
        if (terrain_)
            ImGui::SetItemTooltip(
                "Show the generated world for the seed in the Terrain panel.\nLast refill: %.1f ms",
                cache_ms_);
        else
            ImGui::SetItemTooltip("%s", terrain_error_.empty() ? "This project has no terrain yet."
                                                               : terrain_error_.c_str());
        ImGui::EndPopup();
    }
    toolbar_next(button_width("Frame"));
    ImGui::BeginDisabled(selection_.empty());
    if (ImGui::Button("Frame")) frame_selection();
    ImGui::SetItemTooltip("Centre the view on the selection (F).");
    ImGui::EndDisabled();
    toolbar_next(ImGui::GetFontSize() * 4);
    ImGui::TextDisabled(zoom_ >= 3.2F ? "%.0f%%" : "%.1f%%", zoom_ / 32 * 100);
}

void SceneEditor::scene_view() {
    // The Scene tab is in front after a project opens; the request waits until docking settles.
    if (focus_ && --focus_ == 0) ImGui::SetNextWindowFocus();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    if (maximized) {
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->WorkPos);
        ImGui::SetNextWindowSize(ImGui::GetMainViewport()->WorkSize);
    }
    const bool open =
        ImGui::Begin(title("Scene", dirty(), maximized ? "###SceneMaximized" : scene_id).c_str(),
                     maximized ? nullptr : &show_scene,
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                         (maximized ? ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                                          ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                                    : 0));
    ImGui::PopStyleVar();
    view_visible_ = false;
    if (!open) {
        ImGui::End();
        return;
    }
    auto& io = ImGui::GetIO();
    auto* draw = ImGui::GetWindowDrawList();

    const ImVec2 top = ImGui::GetCursorScreenPos();
    const float inset = ImGui::GetStyle().FramePadding.x;
    ImGui::SetCursorScreenPos({top.x + inset, top.y + 4});
    ImGui::BeginGroup();
    toolbar();
    ImGui::EndGroup();
    const ImVec2 view_top{top.x, ImGui::GetItemRectMax().y + ImGui::GetStyle().ItemSpacing.y};
    ImGui::SetCursorScreenPos(view_top);
    const ImVec2 size = ImGui::GetContentRegionAvail();
    if (size.x < 2 || size.y < 2) {
        ImGui::End();
        return;
    }
    ImGui::InvisibleButton("view", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    // A prefab dragged from the Prefabs panel lands where it is dropped.
    if (ImGui::BeginDragDropTarget()) {
        if (const auto* payload = ImGui::AcceptDragDropPayload("seed.prefab")) {
            const std::string name(static_cast<const char*>(payload->Data),
                                   static_cast<std::size_t>(payload->DataSize));
            auto at = to_world(io.MousePos);
            if (const float step = snap_step(); step > 0)
                at = from_global(std::round(global_coordinate(at.chunk.x, at.local.x) / step) * step,
                                 std::round(global_coordinate(at.chunk.y, at.local.y) / step) * step);
            place_prefab(name, at);
        }
        ImGui::EndDragDropTarget();
    }
    view_min_ = view_top;
    view_max_ = {view_top.x + size.x, view_top.y + size.y};
    view_visible_ = true;
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = io.MousePos;

    // Zoom about the pointer, so the point under it stays put.
    if (hovered && io.MouseWheel != 0) {
        const auto before = to_world(mouse);
        zoom_ = std::clamp(zoom_ * std::pow(1.15F, io.MouseWheel), min_zoom, max_zoom);
        camera_.move(relative(before, to_world(mouse)));
    }
    // Pan with the right or middle button.
    if (ImGui::IsItemActive() &&
        (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle)))
        camera_.move({-io.MouseDelta.x / zoom_, io.MouseDelta.y / zoom_});

    // Left button: a handle, an entity (select it, then drag to move the selection), or empty
    // ground (drag a box to select; a plain click clears the selection).
    if (tool_ == Tool::paint) {
        // The paint tool paints with the left button; it never selects or moves.
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            if (!terrain_)
                log_(true, terrain_error_.empty() ? "Pick or create a terrain for this scene before painting."
                                                  : terrain_error_);
            else if (io.KeyAlt)
                pick_brush(to_world(mouse));
            else {
                drag_ = Drag::paint;
                ++stroke_;
                shape_from_ = to_world(mouse);
                if (brush_.shape == Shape::fill) paint_fill(shape_from_);
            }
        }
        if (drag_ == Drag::paint && brush_.shape == Shape::freehand &&
            ImGui::IsMouseDown(ImGuiMouseButton_Left))
            paint_at(to_world(mouse), std::min(io.DeltaTime, 0.1F));
    } else if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        const bool adding = io.KeyShift || io.KeyCtrl;
        if (const auto handle = handle_under(mouse); handle != Drag::none)
            begin_drag(handle, mouse);
        else if (const int under = pick(mouse); under >= 0) {
            if (adding)
                toggle(under);
            else if (!is_selected(under))
                select_only(under);
            else
                primary_ = under;
            if (is_selected(under) && selection_editable()) begin_drag(Drag::body, mouse);
        } else {
            box_additive_ = adding;
            begin_drag(Drag::box, mouse);
        }
    }
    if (drag_ != Drag::none && drag_ != Drag::box && drag_ != Drag::paint &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2))
        continue_drag(mouse);
    if (drag_ != Drag::none && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (drag_ == Drag::paint && (brush_.shape == Shape::rectangle || brush_.shape == Shape::line))
            paint_shape(to_world(mouse));
        if (drag_ == Drag::box) {
            if (!box_additive_) clear_selection();
            const ImVec2 lo{std::min(grab_screen_.x, mouse.x), std::min(grab_screen_.y, mouse.y)};
            const ImVec2 hi{std::max(grab_screen_.x, mouse.x), std::max(grab_screen_.y, mouse.y)};
            if (hi.x - lo.x > 3 || hi.y - lo.y > 3)
                for (int i = 0; i < static_cast<int>(edited_.entities.size()); ++i) {
                    const auto& e = edited_.entities[static_cast<std::size_t>(i)];
                    if (e.hidden || e.locked || !nearby(e.position.chunk, camera_.chunk, view_reach))
                        continue;
                    const auto p = to_screen(e.position);
                    if (p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y && !is_selected(i))
                        toggle(i);
                }
        }
        drag_ = Drag::none;
    }
    // A right click that did not pan opens the context menu.
    if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
        io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Right] < 16) {
        menu_at_ = to_world(mouse);
        if (const int under = pick(mouse); under >= 0 && !is_selected(under)) select_only(under);
        ImGui::OpenPopup("view menu");
    }
    if (ImGui::BeginPopup("view menu")) {
        if (ImGui::MenuItem("Create Entity Here")) create_at(menu_at_);
        if (ImGui::MenuItem("Create Player Here")) create_character(menu_at_, true, *assets_);
        if (ImGui::MenuItem("Create NPC Here")) create_character(menu_at_, false, *assets_);
        if (ImGui::MenuItem("Paste", "Cmd+V")) paste();
        if (!selection_.empty()) {
            ImGui::Separator();
            if (ImGui::MenuItem("Copy", "Cmd+C")) copy_selection();
            if (ImGui::MenuItem("Duplicate", "Cmd+D")) duplicate_selection();
            if (ImGui::MenuItem("Delete", "Del")) delete_selection();
        }
        ImGui::EndPopup();
    }
    if (ImGui::IsWindowFocused() && !ImGui::IsAnyItemActive()) {
        if (ImGui::Shortcut(ImGuiKey_Delete) || ImGui::Shortcut(ImGuiKey_Backspace)) delete_selection();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D)) duplicate_selection();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C)) copy_selection();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_X)) {
            copy_selection();
            delete_selection();
        }
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V)) paste();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_A))
            for (int i = 0; i < static_cast<int>(edited_.entities.size()); ++i) {
                const auto& e = edited_.entities[static_cast<std::size_t>(i)];
                if (!e.hidden && !e.locked && !is_selected(i)) toggle(i);
            }
        if (ImGui::Shortcut(ImGuiKey_Escape)) clear_selection();
        if (ImGui::Shortcut(ImGuiKey_F)) frame_selection();
        if (ImGui::Shortcut(ImGuiKey_W)) tool_ = Tool::move;
        if (ImGui::Shortcut(ImGuiKey_E)) tool_ = Tool::rotate;
        if (ImGui::Shortcut(ImGuiKey_R)) tool_ = Tool::scale;
        if (ImGui::Shortcut(ImGuiKey_B)) {
            tool_ = Tool::paint;
            clear_selection();
            terrain_selected_ = true;
            atmosphere_selected_ = false;
        }
    }

    // The scene itself, then overlays on top.
    draw->PushClipRect(view_min_, view_max_, true);
    if (renderer_) {
        draw->AddCallback(draw_callback, this);
        draw->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    } else
        draw->AddRectFilled(view_min_, view_max_, rgba(0.07F, 0.08F, 0.09F, 1));
    overlays(draw);
    if (hovered) {
        const auto p = to_world(mouse);
        char text[96];
        std::snprintf(text, sizeof(text), "%.2f, %.2f", global_coordinate(p.chunk.x, p.local.x),
                      global_coordinate(p.chunk.y, p.local.y));
        draw->AddText({view_min_.x + 10, view_min_.y + 8}, rgba(1, 1, 1, 0.7F), text);
    }
    draw->PopClipRect();
    ImGui::End();
}

void SceneEditor::overlays(ImDrawList* draw) {
    const auto grid = [&](float spacing, ImU32 color) {
        // Lines sit at whole multiples of `spacing` in global coordinates. Chunk origins are
        // multiples of chunk_side, so the camera's local offset alone decides where they fall.
        const float half_w = (view_max_.x - view_min_.x) / 2 / zoom_,
                    half_h = (view_max_.y - view_min_.y) / 2 / zoom_;
        const float x0 = std::ceil((camera_.local.x - half_w) / spacing) * spacing - camera_.local.x;
        const float y0 = std::ceil((camera_.local.y - half_h) / spacing) * spacing - camera_.local.y;
        const ImVec2 c{(view_min_.x + view_max_.x) / 2, (view_min_.y + view_max_.y) / 2};
        for (float x = x0; x <= half_w; x += spacing)
            draw->AddLine({c.x + x * zoom_, view_min_.y}, {c.x + x * zoom_, view_max_.y}, color);
        for (float y = y0; y <= half_h; y += spacing)
            draw->AddLine({view_min_.x, c.y - y * zoom_}, {view_max_.x, c.y - y * zoom_}, color);
    };
    if (zoom_ >= 12) grid(1, rgba(1, 1, 1, 0.06F));
    if (zoom_ * chunk_side >= 16) grid(chunk_side, rgba(1, 1, 1, 0.18F));
    for (std::size_t i = 0; i < edited_.entities.size(); ++i) {
        const auto& e = edited_.entities[i];
        if (e.hidden || !nearby(e.position.chunk, camera_.chunk, view_reach)) continue;
        const auto centre = to_screen(e.position);
        if (!e.visual || !renderer_) { // A diamond marks entities with nothing to draw.
            const float r = marker_size / 2 * zoom_;
            const ImU32 color = e.light ? rgba(1, 0.8F, 0.4F, 0.9F) : rgba(0.6F, 0.8F, 1, 0.9F);
            draw->AddQuadFilled({centre.x, centre.y - r}, {centre.x + r, centre.y}, {centre.x, centre.y + r},
                                {centre.x - r, centre.y}, color);
        }
        if (e.character) {
            // The collision box, and for the player what the game camera will show.
            const auto& c = *e.character;
            const ImVec2 half{c.collision.x / 2 * zoom_, c.collision.y / 2 * zoom_};
            draw->AddRect({centre.x - half.x, centre.y - half.y}, {centre.x + half.x, centre.y + half.y},
                          rgba(0.4F, 0.85F, 1, 0.8F), 0, 1.5F);
            if (c.player) {
                const float w = game_view_.x / c.player->zoom / 2 * zoom_,
                            h = game_view_.y / c.player->zoom / 2 * zoom_;
                draw->AddRect({centre.x - w, centre.y - h}, {centre.x + w, centre.y + h},
                              rgba(1, 1, 1, 0.35F), 0, 1.0F);
                draw->AddText({centre.x - w + 4, centre.y - h + 2}, rgba(1, 1, 1, 0.55F), "Game camera");
            }
        }
        if (!is_selected(static_cast<int>(i))) continue;
        const auto size = extent(e);
        const float c = std::cos(e.angle), s = std::sin(e.angle);
        ImVec2 corners[4];
        const float hx[4] = {-1, 1, 1, -1}, hy[4] = {-1, -1, 1, 1};
        for (int k = 0; k < 4; ++k) {
            const float x = hx[k] * size.x / 2, y = hy[k] * size.y / 2;
            corners[k] = {centre.x + (x * c - y * s) * zoom_, centre.y - (x * s + y * c) * zoom_};
        }
        const bool primary = static_cast<int>(i) == primary_;
        draw->AddPolyline(corners, 4, rgba(0.45F, 0.9F, 0.65F, primary ? 1.0F : 0.6F), primary ? 2.0F : 1.5F,
                          ImDrawFlags_Closed);
        if (e.light) draw->AddCircle(centre, e.light->radius * zoom_, rgba(1, 0.8F, 0.4F, 0.5F), 64, 1.5F);
    }
    const auto mouse = ImGui::GetIO().MousePos;
    if (tool_ == Tool::paint && ImGui::IsMouseHoveringRect(view_min_, view_max_)) {
        const auto at = to_world(mouse);
        const double cx = global_coordinate(at.chunk.x, at.local.x),
                     cy = global_coordinate(at.chunk.y, at.local.y);
        const float r = std::max(0.5F, brush_.size / 2);
        // Height and blocking do not show in the view itself, so those brushes mark them nearby.
        if ((brush_.kind == Brush::raise || brush_.kind == Brush::lower || brush_.kind == Brush::flatten ||
             brush_.kind == Brush::solid) &&
            zoom_ >= 6) {
            const auto reach = static_cast<std::int64_t>(std::ceil(r * 2 + 3));
            const auto tx = static_cast<std::int64_t>(std::floor(cx)),
                       ty = static_cast<std::int64_t>(std::floor(cy));
            for (auto y = ty - reach; y <= ty + reach; ++y)
                for (auto x = tx - reach; x <= tx + reach; ++x) {
                    ViewTile tile{};
                    if (!view_tile(x, y, tile) || (tile.elevation >= 0 && !tile.solid)) continue;
                    const auto p = to_screen(from_global(static_cast<double>(x), static_cast<double>(y)));
                    const ImVec2 lo{p.x, p.y - zoom_}, hi{p.x + zoom_, p.y};
                    if (tile.elevation < 0) draw->AddRectFilled(lo, hi, rgba(0.2F, 0.45F, 1, 0.35F));
                    if (tile.solid) draw->AddRect(lo, hi, rgba(1, 0.3F, 0.25F, 0.8F), 0, 0, 1.5F);
                }
        }
        const ImU32 outline = rgba(1, 1, 1, 0.85F);
        // A rectangle or line being dragged shows the tiles it will paint when let go.
        if (drag_ == Drag::paint && brush_.shape == Shape::rectangle) {
            const auto a = to_screen(shape_from_);
            const auto corner = [&](float sx, float sy, bool high) {
                const auto w = to_world({sx, sy});
                const double gx = std::floor(global_coordinate(w.chunk.x, w.local.x)) + (high ? 1 : 0),
                             gy = std::floor(global_coordinate(w.chunk.y, w.local.y)) + (high ? 1 : 0);
                return to_screen(from_global(gx, gy));
            };
            const auto lo = corner(std::min(a.x, mouse.x), std::max(a.y, mouse.y), false);
            const auto hi = corner(std::max(a.x, mouse.x), std::min(a.y, mouse.y), true);
            draw->AddRectFilled({lo.x, hi.y}, {hi.x, lo.y}, rgba(1, 1, 1, 0.12F));
            draw->AddRect({lo.x, hi.y}, {hi.x, lo.y}, outline, 0, 0, 1.5F);
        } else if (drag_ == Drag::paint && brush_.shape == Shape::line)
            draw->AddLine(to_screen(shape_from_), mouse, rgba(1, 1, 1, 0.35F), std::max(1.5F, r * 2 * zoom_));
        if (brush_.shape == Shape::fill)
            draw->AddRect({mouse.x - zoom_ / 2, mouse.y - zoom_ / 2},
                          {mouse.x + zoom_ / 2, mouse.y + zoom_ / 2}, outline, 0, 0, 1.5F);
        else if (brush_.shape == Shape::rectangle && drag_ == Drag::paint) {
            // The rectangle above is the outline.
        } else if (brush_.square)
            draw->AddRect({mouse.x - r * zoom_, mouse.y - r * zoom_},
                          {mouse.x + r * zoom_, mouse.y + r * zoom_}, outline, 0, 0, 1.5F);
        else
            draw->AddCircle(mouse, r * zoom_, outline, 48, 1.5F);
    }
    if (drag_ == Drag::box) {
        const ImVec2 lo{std::min(grab_screen_.x, mouse.x), std::min(grab_screen_.y, mouse.y)};
        const ImVec2 hi{std::max(grab_screen_.x, mouse.x), std::max(grab_screen_.y, mouse.y)};
        draw->AddRectFilled(lo, hi, rgba(0.45F, 0.9F, 0.65F, 0.08F));
        draw->AddRect(lo, hi, rgba(0.45F, 0.9F, 0.65F, 0.7F));
    }
    // The current tool's handles; the one under the pointer, or being dragged, is highlighted.
    if (selection_editable()) {
        const auto c = to_screen(drag_ == Drag::none || drag_ == Drag::box ? selection_centre() : pivot_);
        const bool dragging_handle = drag_ != Drag::none && drag_ != Drag::body && drag_ != Drag::box;
        const auto hot = dragging_handle ? drag_ : handle_under(mouse);
        const auto tint = [&](Drag which, ImU32 normal) {
            return hot == which ? rgba(1, 0.95F, 0.4F, 1) : normal;
        };
        const ImU32 red = rgba(0.92F, 0.35F, 0.3F, 1), green = rgba(0.4F, 0.85F, 0.35F, 1);
        const ImU32 white = rgba(1, 1, 1, 0.85F);
        switch (tool_) {
        case Tool::move:
            draw->AddLine(c, {c.x + arm - 10, c.y}, tint(Drag::axis_x, red), 2.5F);
            draw->AddTriangleFilled({c.x + arm, c.y}, {c.x + arm - 12, c.y - 6}, {c.x + arm - 12, c.y + 6},
                                    tint(Drag::axis_x, red));
            draw->AddLine(c, {c.x, c.y - arm + 10}, tint(Drag::axis_y, green), 2.5F);
            draw->AddTriangleFilled({c.x, c.y - arm}, {c.x - 6, c.y - arm + 12}, {c.x + 6, c.y - arm + 12},
                                    tint(Drag::axis_y, green));
            draw->AddRectFilled({c.x - grip, c.y - grip}, {c.x + grip, c.y + grip}, tint(Drag::free, white));
            break;
        case Tool::rotate:
            draw->AddCircle(c, ring, tint(Drag::rotate, rgba(0.35F, 0.6F, 1, 1)), 64, 2.5F);
            draw->AddCircleFilled(c, 3, white);
            break;
        case Tool::scale:
            draw->AddLine(c, {c.x + arm, c.y}, tint(Drag::scale_x, red), 2.5F);
            draw->AddRectFilled({c.x + arm - grip, c.y - grip}, {c.x + arm + grip, c.y + grip},
                                tint(Drag::scale_x, red));
            draw->AddLine(c, {c.x, c.y - arm}, tint(Drag::scale_y, green), 2.5F);
            draw->AddRectFilled({c.x - grip, c.y - arm - grip}, {c.x + grip, c.y - arm + grip},
                                tint(Drag::scale_y, green));
            draw->AddRectFilled({c.x - grip, c.y - grip}, {c.x + grip, c.y + grip},
                                tint(Drag::scale_both, white));
            break;
        case Tool::paint:
            break;
        }
    }
    if (!renderer_) {
        const char* message = renderer_error_.empty()
                                  ? "Add a material in the Materials panel to see visuals."
                                  : renderer_error_.c_str();
        draw->AddText({view_min_.x + 12, view_max_.y - 28}, rgba(1, 1, 1, 0.6F), message);
    }
}

void SceneEditor::draw_callback(const ImDrawList*, const ImDrawCmd* command) {
    auto& self = *static_cast<SceneEditor*>(command->UserCallbackData);
    try {
        self.render();
    } catch (const std::exception& error) {
        self.render_error_ = error.what();
        self.show_scene = false;
    }
}

void SceneEditor::render() {
    if (!view_visible_ || !renderer_) return;
    const auto& io = ImGui::GetIO();
    const float sx = io.DisplayFramebufferScale.x, sy = io.DisplayFramebufferScale.y;
    const int width = static_cast<int>((view_max_.x - view_min_.x) * sx);
    const int height = static_cast<int>((view_max_.y - view_min_.y) * sy);
    if (width < 1 || height < 1) return;
    auto& r = *renderer_;
    // Lit shows the scene's atmosphere at the preview hour, with lights flickering as in the game.
    const float hour = preview_hour_ >= 0 ? preview_hour_ : edited_.atmosphere.hour;
    const float day = daylight(hour);
    r.lighting = lighting_at(edited_.atmosphere, hour);
    if (!lit_) {
        r.lighting = game_lighting_;
        r.lighting.ambient = {1, 1, 1};
        r.lighting.haze_amount = 0;
    }
    r.begin(width, height, 0, 0, zoom_ * sx);
    r.set_time(ImGui::GetTime()); // Animated materials play in the preview.
    // OpenGL counts rows from the bottom of the window.
    r.set_output(static_cast<int>(view_min_.x * sx), static_cast<int>((io.DisplaySize.y - view_max_.y) * sy));
    const float half_w = (view_max_.x - view_min_.x) / 2 / zoom_,
                half_h = (view_max_.y - view_min_.y) / 2 / zoom_;
    if (show_terrain_ && terrain_) draw_terrain(half_w, half_h); // Under the entities.
    std::size_t lights = 0;
    for (const auto& e : edited_.entities) {
        if (e.hidden || !nearby(e.position.chunk, camera_.chunk, view_reach)) continue;
        const auto at = relative(e.position, camera_);
        if (e.visual) {
            const auto found = std::find_if(rendered_.begin(), rendered_.end(), [&](const MaterialAsset& m) {
                return m.name == e.visual->material;
            });
            if (found != rendered_.end())
                r.sprite(static_cast<MaterialId>(found - rendered_.begin()), at.x, at.y, e.visual->size.x,
                         e.visual->size.y, e.angle);
        }
        // The renderer draws at most 32 lights; the rest are left out of the preview.
        if (lit_ && e.light && lights < 32 && std::abs(at.x) < half_w + e.light->radius &&
            std::abs(at.y) < half_h + e.light->radius) {
            const auto& l = *e.light;
            const LightComponent shown{l.color, l.radius, l.intensity, l.height, l.flicker, l.night_only};
            if (const float intensity =
                    light_intensity(shown, day, ImGui::GetTime(), static_cast<unsigned>(lights));
                intensity > 0) {
                r.light(at.x, at.y, l.radius, l.color[0], l.color[1], l.color[2], intensity, l.height);
                ++lights;
            }
        }
    }
    r.finish();
    r.set_output(0, 0);
}

void SceneEditor::draw_terrain(float half_w, float half_h) {
    constexpr float max_cells = 30000; // Visible sprites; keeps the view to one draw batch.
    int block = 1;
    while (block < 256 &&
           (2 * half_w / static_cast<float>(block)) * (2 * half_h / static_cast<float>(block)) > max_cells)
        block *= 2;
    const auto seed = compiled_terrain_.default_seed;
    // The visible cells, counted from the camera chunk's corner.
    const auto cell = [&](float offset) {
        return static_cast<int>(std::floor(offset / static_cast<float>(block)));
    };
    // Refill when the view leaves the cached area, the block size changes, or the terrain does.
    bool covered = !cache_.cells.empty() && cache_.block == block && cache_.version == terrain_->version() &&
                   cache_.seed == seed && nearby(cache_.origin, camera_.chunk, 64);
    int vx0{}, vy0{}, vx1{}, vy1{};
    if (covered) {
        const auto shift = relative({camera_.chunk, {}}, {cache_.origin, {}}); // Whole chunks: exact.
        vx0 = cell(shift.x + camera_.local.x - half_w);
        vx1 = cell(shift.x + camera_.local.x + half_w);
        vy0 = cell(shift.y + camera_.local.y - half_h);
        vy1 = cell(shift.y + camera_.local.y + half_h);
        covered = vx0 >= cache_.x0 && vy0 >= cache_.y0 && vx1 < cache_.x0 + cache_.columns &&
                  vy1 < cache_.y0 + cache_.rows;
    }
    if (!covered) {
        const auto start = std::chrono::steady_clock::now();
        // A margin of half a view on each side lets small pans reuse the cache.
        cache_.origin = camera_.chunk;
        cache_.block = block;
        cache_.version = terrain_->version();
        cache_.seed = seed;
        cache_.x0 = cell(camera_.local.x - half_w * 1.5F);
        cache_.y0 = cell(camera_.local.y - half_h * 1.5F);
        cache_.columns = cell(camera_.local.x + half_w * 1.5F) - cache_.x0 + 1;
        cache_.rows = cell(camera_.local.y + half_h * 1.5F) - cache_.y0 + 1;
        cache_.cells.resize(static_cast<std::size_t>(cache_.columns) * static_cast<std::size_t>(cache_.rows));
        for (int y = 0; y < cache_.rows; ++y)
            for (int x = 0; x < cache_.columns; ++x) {
                WorldPosition at{cache_.origin, {}};
                at.move({(static_cast<float>(cache_.x0 + x) + 0.5F) * static_cast<float>(block),
                         (static_cast<float>(cache_.y0 + y) + 0.5F) * static_cast<float>(block)});
                const auto sample = terrain_->sample(seed, at);
                cache_.cells[static_cast<std::size_t>(y * cache_.columns + x)] = {
                    sample.material, sample.object, sample.solid, sample.elevation};
            }
        cache_ms_ =
            std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
        vx0 = cell(camera_.local.x - half_w);
        vx1 = cell(camera_.local.x + half_w);
        vy0 = cell(camera_.local.y - half_h);
        vy1 = cell(camera_.local.y + half_h);
    }
    const auto origin = relative({cache_.origin, {}}, camera_);
    const float size = static_cast<float>(block);
    // Painted tiles over the generated ones; each paint material by its place in the preview.
    const auto& paint = edited_.paint;
    std::vector<int> painted_ids;
    for (const auto& name : paint.materials) {
        const auto found = std::find_if(rendered_.begin(), rendered_.end(),
                                        [&](const MaterialAsset& m) { return m.name == name; });
        painted_ids.push_back(found == rendered_.end() ? -1 : static_cast<int>(found - rendered_.begin()));
    }
    const auto ox = cache_.origin.x * chunk_side, oy = cache_.origin.y * chunk_side;
    // The ground shown at a cached cell, painted or generated; `fallback` outside the cache.
    const auto ground_at = [&](int x, int y, MaterialId fallback) {
        if (x < cache_.x0 || y < cache_.y0 || x >= cache_.x0 + cache_.columns || y >= cache_.y0 + cache_.rows)
            return fallback;
        auto ground =
            cache_.cells[static_cast<std::size_t>((y - cache_.y0) * cache_.columns + (x - cache_.x0))].ground;
        if (const auto* p = paint.empty() ? nullptr : paint.find(ox + x, oy + y))
            if ((p->mask & paint_ground) && painted_ids[p->ground] >= 0)
                ground = static_cast<MaterialId>(painted_ids[p->ground]);
        return ground;
    };
    for (int y = vy0; y <= vy1; ++y)
        for (int x = vx0; x <= vx1; ++x) {
            auto cell =
                cache_.cells[static_cast<std::size_t>((y - cache_.y0) * cache_.columns + (x - cache_.x0))];
            if (!paint.empty()) {
                // A block shows the paint of the tile its sample stands for.
                const auto half = block / 2;
                if (const auto* p = paint.find(ox + std::int64_t{x} * block + half,
                                               oy + std::int64_t{y} * block + half)) {
                    if ((p->mask & paint_ground) && painted_ids[p->ground] >= 0)
                        cell.ground = static_cast<MaterialId>(painted_ids[p->ground]);
                    if (p->mask & paint_object)
                        cell.object = p->object && painted_ids[p->object - 1u] >= 0
                                          ? tile_object(static_cast<MaterialId>(painted_ids[p->object - 1u]))
                                          : no_object;
                }
            }
            const float cx = origin.x + (static_cast<float>(x) + 0.5F) * size;
            const float cy = origin.y + (static_cast<float>(y) + 0.5F) * size;
            if (block == 1) {
                const auto g = cell.ground;
                renderer_->ground_blended(g,
                                          {ground_at(x + 1, y, g), ground_at(x - 1, y, g),
                                           ground_at(x, y + 1, g), ground_at(x, y - 1, g),
                                           ground_at(x + 1, y + 1, g), ground_at(x - 1, y + 1, g),
                                           ground_at(x + 1, y - 1, g), ground_at(x - 1, y - 1, g)},
                                          cx, cy, global_coordinate(cache_.origin.x, static_cast<float>(x)),
                                          global_coordinate(cache_.origin.y, static_cast<float>(y)));
            } else
                renderer_->sprite(cell.ground, cx, cy, size, size);
            // Objects show only up close: zoomed out, a block's one sample would blow a single tree
            // up to the size of the whole block.
            if (block == 1 && cell.object != no_object)
                renderer_->sprite(static_cast<MaterialId>(cell.object - 1), cx, cy);
        }
}

bool SceneEditor::view_tile(std::int64_t x, std::int64_t y, ViewTile& out) const {
    if (cache_.block != 1 || cache_.cells.empty()) return false;
    const auto cx = x - cache_.origin.x * chunk_side - cache_.x0,
               cy = y - cache_.origin.y * chunk_side - cache_.y0;
    if (cx < 0 || cy < 0 || cx >= cache_.columns || cy >= cache_.rows) return false;
    const auto& cell = cache_.cells[static_cast<std::size_t>(cy * cache_.columns + cx)];
    out = {cell.elevation, cell.solid};
    if (const auto* p = edited_.paint.find(x, y)) {
        if (p->mask & paint_height) out.elevation = p->elevation;
        if (p->mask & paint_solid) out.solid = p->solid;
    }
    return true;
}

void SceneEditor::paint_at(WorldPosition centre, float dt) {
    if (!terrain_) return;
    const double cx = global_coordinate(centre.chunk.x, centre.local.x),
                 cy = global_coordinate(centre.chunk.y, centre.local.y);
    const double r = std::max(0.5, brush_.size / 2.0);
    const auto x0 = static_cast<std::int64_t>(std::floor(cx - r)),
               x1 = static_cast<std::int64_t>(std::floor(cx + r));
    const auto y0 = static_cast<std::int64_t>(std::floor(cy - r)),
               y1 = static_cast<std::int64_t>(std::floor(cy + r));
    for (auto y = y0; y <= y1; ++y)
        for (auto x = x0; x <= x1; ++x) {
            const double dx = static_cast<double>(x) + 0.5 - cx, dy = static_cast<double>(y) + 0.5 - cy;
            const double d = brush_.square ? std::max(std::abs(dx), std::abs(dy)) : std::hypot(dx, dy);
            // A one-tile brush always paints the tile under the pointer.
            if (d > r && !(brush_.size <= 1 && std::abs(dx) <= 0.5 && std::abs(dy) <= 0.5)) continue;
            // Height brushes ease off towards a round brush's edge.
            const float falloff =
                brush_.square ? 1.0F : static_cast<float>(std::clamp(1 - (d / r) * (d / r), 0.15, 1.0));
            paint_tile(x, y, falloff, dt);
        }
}

void SceneEditor::paint_tile(std::int64_t x, std::int64_t y, float falloff, float dt) {
    auto& paint = edited_.paint;
    const auto seed = compiled_terrain_.default_seed;
    const auto hash = [&](std::uint64_t salt) {
        return world_hash(stroke_ * 2 + salt, static_cast<std::uint64_t>(x), static_cast<std::uint64_t>(y));
    };
    // Partial strength paints a fixed share of tiles per stroke, so holding still does not fill the
    // rest in.
    const bool chosen = brush_.strength >= 1 || static_cast<float>(hash(0) % 1000) < brush_.strength * 1000;
    const auto* existing = paint.find(x, y);
    PaintedTile tile = existing ? *existing : PaintedTile{};
    switch (brush_.kind) {
    case Brush::ground:
        if (!chosen || brush_.ground.empty()) return;
        tile.mask |= paint_ground;
        tile.ground = paint.material(brush_.ground);
        break;
    case Brush::object: {
        if (!chosen) return;
        // A mixed brush picks one of its objects for each tile.
        const auto pick = brush_.mix.empty() ? 0 : hash(1) % (brush_.mix.size() + 1);
        const auto& object = pick == 0 ? brush_.object : brush_.mix[pick - 1];
        tile.mask |= paint_object | paint_solid;
        tile.object = object.empty() ? 0 : static_cast<std::uint8_t>(paint.material(object) + 1);
        tile.solid = !object.empty() && brush_.object_solid;
        break;
    }
    case Brush::raise:
    case Brush::lower:
    case Brush::flatten: {
        const float from =
            (tile.mask & paint_height)
                ? tile.elevation
                : terrain_->sample(seed, from_global(double(x) + 0.5, double(y) + 0.5)).elevation;
        const float amount = brush_.strength * dt * 2 * falloff;
        float to = brush_.kind == Brush::raise ? from + amount
                   : brush_.kind == Brush::lower
                       ? from - amount
                       : from + std::clamp(brush_.height - from, -amount * 4, amount * 4);
        tile.mask |= paint_height;
        tile.elevation = std::clamp(to, -4.0F, 4.0F);
        break;
    }
    case Brush::solid:
        tile.mask |= paint_solid;
        tile.solid = brush_.block;
        break;
    case Brush::erase:
        if (!chosen) return;
        tile = {};
        break;
    }
    if (!(existing && *existing == tile)) paint.set(x, y, tile);
}

namespace {
// Height brushes change a shape's tiles as much as holding the brush still this long would; flatten
// levels them fully at full strength.
constexpr float shape_seconds = 0.25F;
// Fill stops after this many tiles, so a click on open sea does not paint the whole view.
constexpr std::size_t fill_capacity = 1 << 16;
} // namespace

void SceneEditor::paint_shape(WorldPosition to) {
    if (!terrain_) return;
    const double ax = global_coordinate(shape_from_.chunk.x, shape_from_.local.x),
                 ay = global_coordinate(shape_from_.chunk.y, shape_from_.local.y);
    const double bx = global_coordinate(to.chunk.x, to.local.x),
                 by = global_coordinate(to.chunk.y, to.local.y);
    if (brush_.shape == Shape::rectangle) {
        // Every tile the rectangle touches, corners included.
        const auto x0 = static_cast<std::int64_t>(std::floor(std::min(ax, bx))),
                   x1 = static_cast<std::int64_t>(std::floor(std::max(ax, bx)));
        const auto y0 = static_cast<std::int64_t>(std::floor(std::min(ay, by))),
                   y1 = static_cast<std::int64_t>(std::floor(std::max(ay, by)));
        if ((x1 - x0 + 1) * (y1 - y0 + 1) > static_cast<std::int64_t>(fill_capacity)) {
            log_(true, "That rectangle is too large to paint at once; paint it in parts.");
            return;
        }
        for (auto y = y0; y <= y1; ++y)
            for (auto x = x0; x <= x1; ++x)
                paint_tile(x, y, 1, shape_seconds);
        return;
    }
    // A line: the brush stamped along it every half tile, each tile painted once.
    const double length = std::hypot(bx - ax, by - ay);
    if (length > 4096) {
        log_(true, "That line is too long to paint at once; paint it in parts.");
        return;
    }
    const double r = std::max(0.5, brush_.size / 2.0);
    std::set<std::pair<std::int64_t, std::int64_t>> done;
    const auto steps = static_cast<int>(std::ceil(length * 2));
    for (int i = 0; i <= steps; ++i) {
        const double t = steps ? static_cast<double>(i) / steps : 0;
        const double cx = ax + (bx - ax) * t, cy = ay + (by - ay) * t;
        for (auto y = static_cast<std::int64_t>(std::floor(cy - r));
             y <= static_cast<std::int64_t>(std::floor(cy + r)); ++y)
            for (auto x = static_cast<std::int64_t>(std::floor(cx - r));
                 x <= static_cast<std::int64_t>(std::floor(cx + r)); ++x) {
                const double dx = static_cast<double>(x) + 0.5 - cx, dy = static_cast<double>(y) + 0.5 - cy;
                const double d = brush_.square ? std::max(std::abs(dx), std::abs(dy)) : std::hypot(dx, dy);
                if (d > r && !(brush_.size <= 1 && std::abs(dx) <= 0.5 && std::abs(dy) <= 0.5)) continue;
                if (done.insert({x, y}).second) paint_tile(x, y, 1, shape_seconds);
            }
    }
}

void SceneEditor::paint_fill(WorldPosition at) {
    if (!terrain_) return;
    const auto sx = static_cast<std::int64_t>(std::floor(global_coordinate(at.chunk.x, at.local.x))),
               sy = static_cast<std::int64_t>(std::floor(global_coordinate(at.chunk.y, at.local.y)));
    std::string ground, object;
    if (!shown_materials(sx, sy, ground, object)) {
        log_(true, "Zoom in until single tiles show to fill.");
        return;
    }
    // The connected tiles in view showing the same ground and object, found before any changes.
    std::vector<std::pair<std::int64_t, std::int64_t>> region, open{{sx, sy}};
    std::set<std::pair<std::int64_t, std::int64_t>> seen{{sx, sy}};
    std::string g, o;
    while (!open.empty()) {
        const auto [x, y] = open.back();
        open.pop_back();
        region.push_back({x, y});
        if (region.size() == fill_capacity) {
            log_(true, "Fill stopped after 65,536 tiles; the area is larger than that.");
            break;
        }
        for (const auto& [nx, ny] :
             {std::pair{x + 1, y}, std::pair{x - 1, y}, std::pair{x, y + 1}, std::pair{x, y - 1}})
            if (!seen.count({nx, ny}) && shown_materials(nx, ny, g, o) && g == ground && o == object) {
                seen.insert({nx, ny});
                open.push_back({nx, ny});
            }
    }
    for (const auto& [x, y] : region)
        paint_tile(x, y, 1, shape_seconds);
}

void SceneEditor::pick_brush(WorldPosition at) {
    const auto x = static_cast<std::int64_t>(std::floor(global_coordinate(at.chunk.x, at.local.x))),
               y = static_cast<std::int64_t>(std::floor(global_coordinate(at.chunk.y, at.local.y)));
    std::string ground, object;
    if (!shown_materials(x, y, ground, object)) return;
    if (brush_.kind == Brush::object) {
        brush_.object = object;
        brush_.mix.clear();
    } else {
        brush_.kind = Brush::ground;
        brush_.ground = ground;
    }
}

bool SceneEditor::shown_materials(std::int64_t x, std::int64_t y, std::string& ground,
                                  std::string& object) const {
    if (cache_.block != 1 || cache_.cells.empty()) return false;
    const auto cx = x - cache_.origin.x * chunk_side - cache_.x0,
               cy = y - cache_.origin.y * chunk_side - cache_.y0;
    if (cx < 0 || cy < 0 || cx >= cache_.columns || cy >= cache_.rows) return false;
    const auto& cell = cache_.cells[static_cast<std::size_t>(cy * cache_.columns + cx)];
    const auto name = [&](MaterialId id) {
        return id < rendered_.size() ? rendered_[id].name : std::string();
    };
    ground = name(cell.ground);
    object = cell.object == no_object ? std::string() : name(static_cast<MaterialId>(cell.object - 1));
    if (const auto* p = edited_.paint.find(x, y)) {
        const auto& names = edited_.paint.materials;
        if (p->mask & paint_ground) ground = names[p->ground];
        if (p->mask & paint_object) object = p->object ? names[p->object - 1u] : std::string();
    }
    return true;
}

void SceneEditor::brush_section(const Assets& assets) {
    ImGui::SeparatorText("Paint");
    const bool active = tool_ == Tool::paint;
    if (!active) {
        if (ImGui::Button("Paint Terrain (B)", {-1, 0})) tool_ = Tool::paint;
        mark("start painting");
        ImGui::TextDisabled("%zu painted tiles.", edited_.paint.tiles());
        return;
    }
    static const char* kinds[] = {"Ground", "Objects", "Raise", "Lower", "Flatten", "Solid", "Erase"};
    static const char* tips[] = {
        "Paint a ground material.",
        "Place an object, such as a tree or rock, on each tile; None removes objects.",
        "Raise the ground. Ground below 0 is water, which characters cannot walk into.",
        "Lower the ground; take it below 0 for water.",
        "Level the ground to a height: below 0 makes water, above makes land.",
        "Block walking, or clear blocking, without changing how the ground looks.",
        "Take paint off, back to the generated terrain."};
    for (int i = 0; i < 7; ++i) {
        const bool on = brush_.kind == static_cast<Brush>(i);
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (i > 0) toolbar_next(button_width(kinds[i]));
        if (ImGui::Button(kinds[i])) brush_.kind = static_cast<Brush>(i);
        if (on) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s", tips[i]);
        mark((std::string("brush ") + kinds[i]).c_str());
    }
    static const char* shapes[] = {"Freehand", "Rectangle", "Line", "Fill"};
    static const char* shape_tips[] = {
        "Paint where the pointer goes while the button is held.",
        "Drag out a rectangle; its tiles are painted when you let go.",
        "Drag a line; the brush paints along it when you let go.",
        "Click to paint every connected tile in view with the same ground and object."};
    for (int i = 0; i < 4; ++i) {
        const bool on = brush_.shape == static_cast<Shape>(i);
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (i > 0) toolbar_next(button_width(shapes[i]));
        if (ImGui::Button(shapes[i])) brush_.shape = static_cast<Shape>(i);
        if (on) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s", shape_tips[i]);
        mark((std::string("shape ") + shapes[i]).c_str());
    }
    ImGui::TextUnformatted("Size (tiles)");
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##brush size", &brush_.size, 1, 64, "%.0f", ImGuiSliderFlags_Logarithmic);
    int shape = brush_.square ? 1 : 0;
    ImGui::RadioButton("Round", &shape, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Square", &shape, 1);
    brush_.square = shape == 1;
    if (brush_.kind != Brush::solid) {
        ImGui::TextUnformatted("Strength");
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##brush strength", &brush_.strength, 0.05F, 1, "%.2f");
        ImGui::SetItemTooltip(brush_.kind == Brush::raise || brush_.kind == Brush::lower ||
                                      brush_.kind == Brush::flatten
                                  ? "How fast the height changes while the button is held."
                                  : "The share of tiles under the brush that each stroke paints.");
    }
    const auto material_combo = [&](const char* id, std::string& value, const char* none, float width = -1) {
        ImGui::SetNextItemWidth(width);
        if (ImGui::BeginCombo(id, value.empty() ? none : value.c_str())) {
            if (none && ImGui::Selectable(none, value.empty())) value.clear();
            for (const auto& m : assets.materials)
                if (ImGui::Selectable(m.name.c_str(), m.name == value)) value = m.name;
            ImGui::EndCombo();
        }
    };
    switch (brush_.kind) {
    case Brush::ground:
        if (brush_.ground.empty() && !assets.materials.empty()) brush_.ground = assets.materials.front().name;
        ImGui::TextUnformatted("Material");
        material_combo("##brush ground", brush_.ground, nullptr);
        mark("brush material");
        break;
    case Brush::object:
        ImGui::TextUnformatted("Object");
        material_combo("##brush object", brush_.object, "None (remove)");
        // Further objects mixed in, each tile taking one of them at random.
        for (std::size_t i = 0; i < brush_.mix.size();) {
            ImGui::PushID(static_cast<int>(i));
            material_combo("##mix", brush_.mix[i], "None (remove)",
                           -button_width("x") - ImGui::GetStyle().ItemSpacing.x);
            ImGui::SameLine();
            const bool removed = ImGui::Button("x");
            ImGui::PopID();
            if (removed)
                brush_.mix.erase(brush_.mix.begin() + static_cast<std::ptrdiff_t>(i));
            else
                ++i;
        }
        if (brush_.mix.size() < 7 && ImGui::Button("Mix In Another")) brush_.mix.push_back(brush_.object);
        ImGui::SetItemTooltip(
            "Each painted tile takes one of the objects at random, for varied woods and rocks.\n"
            "Lower the strength to scatter them.");
        mark("brush mix");
        if (!brush_.object.empty() || !brush_.mix.empty())
            ImGui::Checkbox("Blocks walking", &brush_.object_solid);
        break;
    case Brush::flatten:
        ImGui::TextUnformatted("Height");
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##brush height", &brush_.height, -1, 2, "%.2f");
        if (ImGui::Button("Water")) brush_.height = -0.3F;
        ImGui::SameLine();
        if (ImGui::Button("Shore")) brush_.height = 0.02F;
        ImGui::SameLine();
        if (ImGui::Button("Land")) brush_.height = 0.15F;
        break;
    case Brush::solid: {
        int block = brush_.block ? 0 : 1;
        ImGui::RadioButton("Block", &block, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Clear", &block, 1);
        brush_.block = block == 0;
        break;
    }
    default:
        break;
    }
    ImGui::TextDisabled("%zu painted tiles. Undo takes back a whole stroke.", edited_.paint.tiles());
    ImGui::TextDisabled("Alt+click picks the ground (or object) under the pointer.");
    if (brush_.kind == Brush::raise || brush_.kind == Brush::lower || brush_.kind == Brush::flatten ||
        brush_.kind == Brush::solid)
        ImGui::TextDisabled("Near the brush, blue shows water and red shows blocked tiles.");
    if (ImGui::Button("Done Painting", {-1, 0})) tool_ = Tool::move;
    mark("done painting");
}

void SceneEditor::scene_menu() {
    const bool locked = dirty();
    ImGui::SetNextItemWidth(-button_width("Edit") - ImGui::GetStyle().ItemSpacing.x);
    ImGui::BeginDisabled(locked);
    if (ImGui::BeginCombo("##scene", name_.c_str())) {
        for (const auto& name : scene_names_)
            if (ImGui::Selectable(name.c_str(), name == name_) && name != name_)
                load(folder_.parent_path(), name);
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (locked) ImGui::SetItemTooltip("Save or revert this scene before switching.");
    ImGui::SameLine();
    ImGui::BeginDisabled(locked);
    if (ImGui::Button("New", {-1, 0})) {
        new_scene_.clear();
        ImGui::OpenPopup("New scene");
    }
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("New scene")) {
        ImGui::TextUnformatted("Scene name");
        ImGui::SetNextItemWidth(220);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##new", &new_scene_, ImGuiInputTextFlags_EnterReturnsTrue);
        const auto problem = check_project_name(new_scene_);
        const bool exists =
            std::find(scene_names_.begin(), scene_names_.end(), new_scene_) != scene_names_.end();
        if (!problem.empty() && !new_scene_.empty())
            ImGui::TextColored({0.95F, 0.65F, 0.35F, 1}, "%s", problem.c_str());
        if (exists) ImGui::TextColored({0.95F, 0.65F, 0.35F, 1}, "A scene with this name exists.");
        ImGui::BeginDisabled(!problem.empty() || exists);
        if (ImGui::Button("Create", {220, 0}) || (enter && problem.empty() && !exists)) {
            save_scene(folder_ / (new_scene_ + ".json"), {});
            load(folder_.parent_path(), new_scene_);
            log_(false, "Created scene \"" + name_ + "\".");
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
}

void SceneEditor::prefabs_panel() {
    prefab_controls_.new_from_selection = {-1, -1};
    prefab_controls_.rows.clear();
    if (!ImGui::Begin(prefabs_id, &show_prefabs)) {
        ImGui::End();
        return;
    }
    ImGui::BeginDisabled(primary_ < 0);
    if (ImGui::SmallButton("New from Selection")) {
        // Suggest the entity's name, keeping only the characters prefab names allow.
        new_prefab_.clear();
        for (const char c : edited_.entities[static_cast<std::size_t>(primary_)].name)
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')
                new_prefab_ += c;
            else if (c == ' ')
                new_prefab_ += '_';
        ImGui::OpenPopup("New prefab");
    }
    prefab_controls_.new_from_selection = {(ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) / 2,
                                           (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) / 2};
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("New prefab")) {
        ImGui::TextUnformatted("Prefab name");
        ImGui::SetNextItemWidth(200);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##prefab", &new_prefab_, ImGuiInputTextFlags_EnterReturnsTrue);
        const bool valid = valid_prefab_name(new_prefab_), exists = prefabs_.count(new_prefab_) > 0;
        if (!valid) ImGui::TextColored({0.95F, 0.65F, 0.35F, 1}, "Use letters, digits, '_' and '-'.");
        if (exists) ImGui::TextColored({0.95F, 0.65F, 0.35F, 1}, "A prefab with this name exists.");
        ImGui::BeginDisabled(!valid || exists);
        if (ImGui::Button("Create", {200, 0}) || (enter && valid && !exists)) {
            make_prefab(new_prefab_);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
    ImGui::BeginChild("prefabs", {0, 0}, ImGuiChildFlags_Borders);
    std::string remove;
    for (const auto& [name, prefab] : prefabs_) {
        ImGui::PushID(name.c_str());
        ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
        prefab_controls_.rows[name] = {(ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) / 2,
                                       (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) / 2};
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            place_prefab(name, camera_);
        ImGui::SetItemTooltip(
            "Drag into the Scene view to place a copy, or double-click to place one at the centre.");
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("seed.prefab", name.data(), name.size());
            ImGui::Text("Place %s", name.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Place at Centre")) place_prefab(name, camera_);
            if (ImGui::MenuItem("Delete Prefab")) remove = name;
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (!remove.empty()) {
        // Copies keep their components and simply stop following the deleted prefab.
        for (auto& e : edited_.entities)
            if (e.prefab == remove) e.prefab.clear();
        prefabs_.erase(remove);
        log_(false, "Deleted prefab \"" + remove +
                        "\"; its copies stay as plain entities. Save to remove the file.");
    }
    if (prefabs_.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Select an entity and press New from Selection to make a reusable prefab.");
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
    ImGui::End();
}

void SceneEditor::reorder(std::vector<int> moving, int target) {
    std::vector<SceneEntity> moved;
    for (const int i : moving)
        moved.push_back(edited_.entities[static_cast<std::size_t>(i)]);
    // Remove from the back so earlier indices stay valid, counting removals before the target.
    int before_target = 0;
    for (auto i = moving.rbegin(); i != moving.rend(); ++i) {
        edited_.entities.erase(edited_.entities.begin() + *i);
        if (*i < target) ++before_target;
    }
    const int at = target - before_target;
    edited_.entities.insert(edited_.entities.begin() + at, moved.begin(), moved.end());
    // Keep the moved entities selected in their new places.
    const int primary_offset =
        static_cast<int>(std::find(moving.begin(), moving.end(), primary_) - moving.begin());
    selection_.clear();
    for (int k = 0; k < static_cast<int>(moved.size()); ++k)
        selection_.push_back(at + k);
    primary_ = primary_offset < static_cast<int>(moved.size()) ? at + primary_offset : selection_.back();
}

void SceneEditor::hierarchy() {
    if (!ImGui::Begin(hierarchy_id, &show_hierarchy)) {
        ImGui::End();
        return;
    }
    scene_menu();
    // The scene's terrain is an object too: selecting it shows its settings in the Inspector.
    if (ImGui::Selectable(("Terrain: " + edited_.terrain).c_str(), terrain_selected_)) {
        clear_selection();
        terrain_selected_ = true;
        atmosphere_selected_ = false;
    }
    mark("terrain row");
    // So is its atmosphere: ambient light, haze and the time of day.
    if (ImGui::Selectable("Atmosphere", atmosphere_selected_)) {
        clear_selection();
        terrain_selected_ = false;
        atmosphere_selected_ = true;
        if (tool_ == Tool::paint) tool_ = Tool::move;
    }
    mark("atmosphere row");
    if (ImGui::SmallButton("Create")) ImGui::OpenPopup("create");
    mark("create");
    if (ImGui::BeginPopup("create")) {
        if (ImGui::MenuItem("Entity")) create_at(camera_);
        if (ImGui::MenuItem("Player")) create_character(camera_, true, *assets_);
        mark("create player");
        if (ImGui::MenuItem("NPC")) create_character(camera_, false, *assets_);
        mark("create npc");
        ImGui::EndPopup();
    }
    toolbar_next(button_width("Duplicate"));
    ImGui::BeginDisabled(selection_.empty());
    if (ImGui::SmallButton("Duplicate")) duplicate_selection();
    toolbar_next(button_width("Delete"));
    if (ImGui::SmallButton("Delete")) delete_selection();
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##search", "Search", &search_);
    search_box_ = {(ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) / 2,
                   (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) / 2};
    std::string needle = search_;
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    const auto matches = [&](const std::string& name) {
        if (needle.empty()) return true;
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return lower.find(needle) != std::string::npos;
    };

    rows_.assign(edited_.entities.size(), {});
    const auto centre = [] {
        return ImVec2{(ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) / 2,
                      (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) / 2};
    };
    ImGui::BeginChild("entities", {0, 0}, ImGuiChildFlags_Borders);
    if (ImGui::BeginTable("rows", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Show", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Lock", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        std::optional<std::pair<std::vector<int>, int>> move; // Applied after the loop.
        for (int i = 0; i < static_cast<int>(edited_.entities.size()); ++i) {
            auto& e = edited_.entities[static_cast<std::size_t>(i)];
            if (!matches(e.name)) continue;
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (renaming_ == i) {
                // Return keeps the new name; Escape or clicking elsewhere drops it. On the frame the
                // field is asked for focus it is not active yet, so that frame never cancels.
                ImGui::SetNextItemWidth(-1);
                const bool starting = rename_focus_;
                if (starting) ImGui::SetKeyboardFocusHere();
                rename_focus_ = false;
                if (ImGui::InputText("##rename", &rename_text_,
                                     ImGuiInputTextFlags_EnterReturnsTrue |
                                         ImGuiInputTextFlags_AutoSelectAll)) {
                    if (!rename_text_.empty()) e.name = rename_text_;
                    renaming_ = -1;
                } else if (!starting && !ImGui::IsItemActive())
                    renaming_ = -1;
                rows_[static_cast<std::size_t>(i)].name = centre();
            } else {
                if (e.hidden)
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                if (ImGui::Selectable(e.name.empty() ? "(unnamed)" : e.name.c_str(), is_selected(i),
                                      ImGuiSelectableFlags_AllowDoubleClick |
                                          ImGuiSelectableFlags_SpanAllColumns |
                                          ImGuiSelectableFlags_AllowOverlap)) {
                    const auto& io = ImGui::GetIO();
                    if (io.KeyCtrl)
                        toggle(i);
                    else if (io.KeyShift && primary_ >= 0) {
                        // A range from the primary selection to here.
                        for (int k = std::min(primary_, i); k <= std::max(primary_, i); ++k)
                            if (!is_selected(k) &&
                                matches(edited_.entities[static_cast<std::size_t>(k)].name))
                                toggle(k);
                        primary_ = i;
                    } else
                        select_only(i);
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) frame_selection();
                }
                if (e.hidden) ImGui::PopStyleColor();
                rows_[static_cast<std::size_t>(i)].name = centre();
                // Drag a row onto another to move it (or the whole selection, if it is selected)
                // before that row. Order is drawing order: later entities draw on top.
                if (needle.empty() && ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload("seed.entity", &i, sizeof i);
                    const auto count = is_selected(i) ? selection_.size() : 1;
                    if (count > 1)
                        ImGui::Text("%zu entities", count);
                    else
                        ImGui::TextUnformatted(e.name.c_str());
                    ImGui::EndDragDropSource();
                }
                if (needle.empty() && ImGui::BeginDragDropTarget()) {
                    if (const auto* payload = ImGui::AcceptDragDropPayload("seed.entity")) {
                        const int dragged = *static_cast<const int*>(payload->Data);
                        move = std::pair{is_selected(dragged) ? selection_ : std::vector<int>{dragged}, i};
                    }
                    ImGui::EndDragDropTarget();
                }
                if (ImGui::BeginPopupContextItem()) {
                    if (!is_selected(i)) select_only(i);
                    if (ImGui::MenuItem("Rename", "Return")) {
                        renaming_ = i;
                        rename_text_ = e.name;
                        rename_focus_ = true;
                    }
                    if (ImGui::MenuItem("Frame", "F")) frame_selection();
                    if (ImGui::MenuItem("Copy", "Cmd+C")) copy_selection();
                    if (ImGui::MenuItem("Duplicate", "Cmd+D")) duplicate_selection();
                    if (ImGui::MenuItem("Delete", "Del")) delete_selection();
                    ImGui::EndPopup();
                }
            }
            ImGui::TableNextColumn();
            bool shown = !e.hidden;
            if (ImGui::Checkbox("##show", &shown)) {
                e.hidden = !shown;
                if (e.hidden && is_selected(i)) toggle(i); // Hidden entities leave the view's selection.
            }
            ImGui::SetItemTooltip("Show in the Scene view. The game always shows it.");
            rows_[static_cast<std::size_t>(i)].show = centre();
            ImGui::TableNextColumn();
            ImGui::Checkbox("##lock", &e.locked);
            ImGui::SetItemTooltip("Locked entities cannot be picked or moved in the Scene view.");
            rows_[static_cast<std::size_t>(i)].lock = centre();
            ImGui::PopID();
        }
        ImGui::EndTable();
        // Below the last row: drop here to move to the end.
        if (needle.empty()) {
            ImGui::InvisibleButton("end", {std::max(1.0F, ImGui::GetContentRegionAvail().x),
                                           std::max(8.0F, ImGui::GetContentRegionAvail().y)});
            if (ImGui::BeginDragDropTarget()) {
                if (const auto* payload = ImGui::AcceptDragDropPayload("seed.entity")) {
                    const int dragged = *static_cast<const int*>(payload->Data);
                    move = std::pair{is_selected(dragged) ? selection_ : std::vector<int>{dragged},
                                     static_cast<int>(edited_.entities.size())};
                }
                ImGui::EndDragDropTarget();
            }
        }
        if (move) reorder(move->first, move->second);
    }
    if (edited_.entities.empty())
        ImGui::TextDisabled("Empty scene. Press Create, or right-click in the Scene view.");
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive() &&
        renaming_ < 0) {
        if (ImGui::Shortcut(ImGuiKey_Delete) || ImGui::Shortcut(ImGuiKey_Backspace)) delete_selection();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D)) duplicate_selection();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C)) copy_selection();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V)) paste();
        if (ImGui::Shortcut(ImGuiKey_F)) frame_selection();
        if ((ImGui::Shortcut(ImGuiKey_Enter) || ImGui::Shortcut(ImGuiKey_F2)) && primary_ >= 0) {
            renaming_ = primary_;
            rename_text_ = edited_.entities[static_cast<std::size_t>(primary_)].name;
            rename_focus_ = true;
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

void SceneEditor::character_section(SceneEntity& e, const Assets& assets) {
    if (!e.character) return;
    bool keep = true;
    if (ImGui::CollapsingHeader("Character", &keep, ImGuiTreeNodeFlags_DefaultOpen)) {
        auto& c = *e.character;
        const auto row = [](const char* label) {
            ImGui::TextUnformatted(label);
            ImGui::SetNextItemWidth(-1);
        };
        bool is_player = c.player.has_value();
        if (ImGui::Checkbox("This is the player", &is_player)) {
            if (is_player)
                make_player(primary_);
            else
                c.player.reset();
        }
        mark("is player");
        ImGui::SetItemTooltip(
            "The keys move the player and the camera follows it. Other characters are NPCs,\n"
            "moved by their scripts with walk, walk_to and stop.");
        row("Walk / run speed (tiles/s)");
        ImGui::DragFloat2("##speeds", &c.speed, 0.1F, 0, 100, "%.1f");
        row("Acceleration (tiles/s\xc2\xb2, 0 = instant)");
        ImGui::DragFloat("##acceleration", &c.acceleration, 0.5F, 0, 10000, "%.1f");
        row("Collision size");
        ImGui::DragFloat2("##collision", &c.collision.x, 0.02F, 0.05F, 16, "%.2f");
        ImGui::TextUnformatted("Blocked by");
        ImGui::Checkbox("Water", &c.water);
        ImGui::SameLine();
        ImGui::Checkbox("Solid tiles", &c.solid);
        ImGui::SameLine();
        ImGui::Checkbox("Buildings", &c.buildings);
        ImGui::Checkbox("Face the way it walks", &c.face_movement);
        if (c.player) {
            auto& p = *c.player;
            ImGui::SeparatorText("Player");
            // Actions from the Input panel, plus the defaults the game adds when they are missing.
            std::vector<std::string> actions;
            for (const auto& a : assets.actions)
                actions.push_back(a.name);
            for (const char* d : {"move_up", "move_down", "move_left", "move_right", "run"})
                if (std::find(actions.begin(), actions.end(), d) == actions.end()) actions.emplace_back(d);
            const auto action = [&](const char* label, std::string& value) {
                ImGui::TextUnformatted(label);
                ImGui::SameLine(70);
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo((std::string("##") + label).c_str(), value.c_str())) {
                    for (const auto& a : actions)
                        if (ImGui::Selectable(a.c_str(), a == value)) value = a;
                    ImGui::EndCombo();
                }
            };
            ImGui::Checkbox("Moves with the keys", &p.input);
            ImGui::SetItemTooltip("Off leaves movement to the player's script (walk, walk_to, stop).");
            ImGui::BeginDisabled(!p.input);
            action("Up", p.up);
            action("Down", p.down);
            action("Left", p.left);
            action("Right", p.right);
            action("Run", p.run);
            ImGui::EndDisabled();
            ImGui::TextDisabled("move_* and run default to WASD, the arrows and Shift.");
            row("Camera zoom (pixels per tile)");
            ImGui::SliderFloat("##zoom", &p.zoom, 2, 512, "%.0f", ImGuiSliderFlags_Logarithmic);
            row("Camera smoothing (seconds)");
            ImGui::SliderFloat("##smoothing", &p.smoothing, 0, 2, p.smoothing == 0 ? "exact" : "%.2f");
            row("Camera dead zone (tiles)");
            ImGui::SliderFloat("##dead_zone", &p.dead_zone, 0, 16, "%.1f");
            ImGui::Checkbox("Saved games resume where the player was", &p.resume);
        }
    }
    if (!keep) e.character.reset();
}

bool SceneEditor::visual_material(SceneVisual& v, Assets& assets) {
    auto& materials = assets.materials;
    bool changed = false, make_new = false;
    ImGui::TextUnformatted("Material");
    ImGui::SetNextItemWidth(-button_width("Edit") - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::BeginCombo("##material", v.material.c_str())) {
        for (const auto& m : materials)
            if (ImGui::Selectable(m.name.c_str(), m.name == v.material)) v.material = m.name;
        ImGui::Separator();
        if (ImGui::Selectable("New Material...")) make_new = true;
        ImGui::EndCombo();
    }
    mark("visual material");
    ImGui::SameLine();
    if (ImGui::Button("Edit", {-1, 0}) && open_material) open_material(v.material);
    ImGui::SetItemTooltip("Show this material in the Materials panel, with all its settings.");
    const auto taken = [&](const std::string& name) {
        return std::any_of(materials.begin(), materials.end(),
                           [&](const MaterialAsset& m) { return m.name == name; });
    };
    const auto copy_of = [&](const std::string& base) {
        auto name = base + "_2";
        for (int n = 3; taken(name); ++n)
            name = base + "_" + std::to_string(n);
        return name;
    };
    auto found = std::find_if(materials.begin(), materials.end(),
                              [&](const MaterialAsset& m) { return m.name == v.material; });
    const auto add = [&](MaterialAsset m, const char* verb) {
        if (materials.size() >= Materials::capacity)
            return log_(true, "A project has at most 64 materials; delete one in the Materials panel first.");
        log_(false, std::string(verb) + " material \"" + m.name + "\".");
        v.material = m.name;
        materials.push_back(std::move(m));
        changed = true;
    };
    if (make_new) {
        MaterialAsset m = found != materials.end() ? *found : MaterialAsset{};
        m.name = copy_of(found != materials.end() ? found->name : "material");
        add(std::move(m), "Made");
        return changed;
    }
    if (found == materials.end()) {
        ImGui::TextColored({0.95F, 0.55F, 0.4F, 1}, "No material named \"%s\".", v.material.c_str());
        return changed;
    }
    auto& m = *found;
    // A texture drawn instead of the material's generated tile.
    const auto project = folder_.parent_path();
    ImGui::TextUnformatted("Texture");
    ImGui::SetNextItemWidth(-button_width("Import...") - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::BeginCombo("##visual texture", m.texture.empty() ? "None (generated)" : m.texture.c_str())) {
        if (ImGui::Selectable("None (generated)", m.texture.empty())) {
            m.texture.clear();
            changed = true;
        }
        for (const auto& name : list_textures(project))
            if (ImGui::Selectable(name.c_str(), name == m.texture)) {
                m.texture = name;
                changed = true;
            }
        ImGui::EndCombo();
    }
    mark("visual texture");
    ImGui::SameLine();
    if (ImGui::Button("Import...", {-1, 0}))
        if (const auto chosen = choose_image("Choose an image to use as a texture")) try {
                m.texture = import_texture(project, *chosen);
                changed = true;
                if (textures_changed) textures_changed();
                log_(false, "Imported texture \"" + m.texture + "\" into assets/textures.");
            } catch (const std::exception& error) {
                log_(true, error.what());
            }
    // Changing a material changes everything that uses it; Make Unique gives the selection its own.
    int others = 0;
    for (std::size_t i = 0; i < edited_.entities.size(); ++i) {
        const auto& other = edited_.entities[i];
        if (other.visual && other.visual->material == m.name && !is_selected(static_cast<int>(i))) ++others;
    }
    for (const auto& [name, terrain] : assets.terrains)
        for (const auto& rule : terrain.rules)
            others += (rule.material == m.name) + (rule.scatter && rule.scatter->material == m.name);
    for (const auto& particle : assets.particles)
        others += particle.material == m.name;
    if (const auto& names = edited_.paint.materials;
        std::find(names.begin(), names.end(), m.name) != names.end())
        ++others;
    if (others > 0) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Also used in %d other place%s in this project or scene.", others,
                            others == 1 ? "" : "s");
        ImGui::PopTextWrapPos();
        if (ImGui::Button("Make Unique", {-1, 0})) {
            auto copy = m;
            copy.name = copy_of(m.name);
            add(std::move(copy), "Gave the selection its own");
        }
        mark("make unique");
        ImGui::SetItemTooltip(
            "Copy this material for the selected entities, so changing it changes only them.");
    }
    return changed;
}

void SceneEditor::reveal_entity(const std::string& name) {
    for (std::size_t i = 0; i < edited_.entities.size(); ++i) {
        if (edited_.entities[i].name != name) continue;
        select_only(static_cast<int>(i));
        frame_selection();
        show_scene = show_inspector = true;
        focus_ = 2;
        return;
    }
}

void SceneEditor::multi_inspector(const Assets& assets) {
    std::vector<SceneEntity*> entities;
    entities.push_back(&edited_.entities[static_cast<std::size_t>(primary_)]);
    for (int index : selection_)
        if (index != primary_) entities.push_back(&edited_.entities[static_cast<std::size_t>(index)]);
    ImGui::Text("%zu entities selected", entities.size());
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Mixed fields show the value from the first applicable entity. Edit a field or press "
                        "Set to apply only that field.");
    ImGui::PopTextWrapPos();

    const auto label = [&](const char* name, bool mixed) {
        ImGui::TextUnformatted(name);
        if (mixed) {
            ImGui::SameLine();
            ImGui::TextColored({0.85F, 0.73F, 0.43F, 1}, "Mixed");
        }
    };
    const auto number = [&](const char* name, auto get, float low, float high) {
        float* first = nullptr;
        for (auto* entity : entities)
            if ((first = get(*entity))) break;
        if (!first) return;
        ImGui::PushID(name);
        const bool mixed = selection_mixed(entities, get);
        label(name, mixed);
        float value = *first;
        ImGui::SetNextItemWidth(mixed ? -button_width("Set") - ImGui::GetStyle().ItemSpacing.x : -1);
        bool changed =
            ImGui::DragFloat("##value", &value, 0.05F, low, high, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        mark(name);
        if (mixed) {
            ImGui::SameLine();
            changed |= ImGui::Button("Set");
            ImGui::SetItemTooltip("Apply the displayed value to every selected entity with this component.");
        }
        if (changed && std::isfinite(value)) set_selection_field(entities, get, value);
        ImGui::PopID();
    };
    const auto toggle_field = [&](const char* name, auto get) {
        bool* first = nullptr;
        for (auto* entity : entities)
            if ((first = get(*entity))) break;
        if (!first) return;
        const bool mixed = selection_mixed(entities, get);
        bool value = *first;
        const auto text = std::string(name) + (mixed ? " (Mixed)" : "");
        if (ImGui::Checkbox((text + "###" + name).c_str(), &value)) set_selection_field(entities, get, value);
    };
    ImGui::SeparatorText("Transform");
    ImGui::Checkbox("Absolute positions", &absolute_position_);
    mark("absolute positions");
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled(absolute_position_
                            ? "Set one axis to the same coordinate for all selected entities."
                            : "Relative: changing an axis moves the group by the same offset.");
    ImGui::PopTextWrapPos();
    for (int axis = 0; axis < 2; ++axis) {
        const auto coordinate = [axis](const SceneEntity& entity) {
            return axis == 0 ? global_coordinate(entity.position.chunk.x, entity.position.local.x)
                             : global_coordinate(entity.position.chunk.y, entity.position.local.y);
        };
        double value = coordinate(*entities.front());
        const bool mixed = std::any_of(entities.begin(), entities.end(),
                                       [&](const auto* entity) { return coordinate(*entity) != value; });
        ImGui::PushID(axis);
        label(axis == 0 ? "Position X" : "Position Y", mixed);
        const bool can_set = mixed && absolute_position_;
        ImGui::SetNextItemWidth(can_set ? -button_width("Set") - ImGui::GetStyle().ItemSpacing.x : -1);
        bool changed =
            ImGui::DragScalar("##position", ImGuiDataType_Double, &value, 0.05F, nullptr, nullptr, "%.2f");
        mark(axis == 0 ? "multi position x" : "multi position y");
        if (can_set) {
            ImGui::SameLine();
            changed |= ImGui::Button("Set");
            mark(axis == 0 ? "set position x" : "set position y");
        }
        if (changed && std::isfinite(value)) {
            try {
                set_selection_position(entities, axis, value, absolute_position_);
            } catch (const std::exception& error) {
                log_(true, error.what());
            }
        }
        ImGui::PopID();
    }
    // Angles are stored in radians, but displayed and edited in degrees.
    const auto angle = [](SceneEntity& e) {
        return &e.angle;
    };
    const bool mixed_angle = selection_mixed(entities, angle);
    label("Rotation (degrees)", mixed_angle);
    float degrees = entities.front()->angle * 180 / pi;
    ImGui::SetNextItemWidth(mixed_angle ? -button_width("Set") - ImGui::GetStyle().ItemSpacing.x : -1);
    bool angle_changed =
        ImGui::DragFloat("##rotation", &degrees, 1, -360, 360, "%.1f", ImGuiSliderFlags_AlwaysClamp);
    if (mixed_angle) {
        ImGui::SameLine();
        angle_changed |= ImGui::Button("Set##rotation");
    }
    if (angle_changed && std::isfinite(degrees)) set_selection_field(entities, angle, degrees * pi / 180);

    const auto component = [&](const char* name, auto member, auto create) {
        const auto count = std::count_if(entities.begin(), entities.end(),
                                         [&](const auto* e) { return (e->*member).has_value(); });
        const auto heading =
            std::string(name) + " (" + std::to_string(count) + "/" + std::to_string(entities.size()) + ")";
        if (!ImGui::CollapsingHeader(heading.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) return false;
        ImGui::PushID(name);
        if (count < static_cast<std::ptrdiff_t>(entities.size()) && ImGui::SmallButton("Add to missing"))
            for (auto* e : entities)
                if (!(e->*member)) (e->*member) = create();
        if (count > 0) {
            if (count < static_cast<std::ptrdiff_t>(entities.size()))
                toolbar_next(button_width("Remove from all"));
            if (ImGui::SmallButton("Remove from all"))
                for (auto* e : entities)
                    (e->*member).reset();
        }
        ImGui::PopID();
        return true;
    };
    if (component("Visual", &SceneEntity::visual, [&] {
            SceneVisual visual;
            if (!assets.materials.empty()) visual.material = assets.materials.front().name;
            return visual;
        })) {
        const auto material = [](SceneEntity& e) {
            return e.visual ? &e.visual->material : nullptr;
        };
        std::string* first = nullptr;
        for (auto* e : entities)
            if ((first = material(*e))) break;
        if (first) {
            label("Material", selection_mixed(entities, material));
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##multi material",
                                  selection_mixed(entities, material) ? "Mixed" : first->c_str())) {
                for (const auto& m : assets.materials)
                    if (ImGui::Selectable(m.name.c_str())) set_selection_field(entities, material, m.name);
                ImGui::EndCombo();
            }
        }
        number("Size X", [](SceneEntity& e) { return e.visual ? &e.visual->size.x : nullptr; }, 0.05F, 64);
        number("Size Y", [](SceneEntity& e) { return e.visual ? &e.visual->size.y : nullptr; }, 0.05F, 64);
    }
    if (component("Light", &SceneEntity::light, [] { return SceneLight{}; })) {
        const char* channels[] = {"Red", "Green", "Blue"};
        for (int i = 0; i < 3; ++i)
            number(
                channels[i],
                [i](SceneEntity& e) {
                    return e.light ? &e.light->color[static_cast<std::size_t>(i)] : nullptr;
                },
                0, 1);
        number("Radius", [](SceneEntity& e) { return e.light ? &e.light->radius : nullptr; }, 0.5F, 64);
        number("Intensity", [](SceneEntity& e) { return e.light ? &e.light->intensity : nullptr; }, 0, 8);
        number("Height", [](SceneEntity& e) { return e.light ? &e.light->height : nullptr; }, 0.1F, 16);
        number("Flicker", [](SceneEntity& e) { return e.light ? &e.light->flicker : nullptr; }, 0, 1);
        toggle_field("Only at night",
                     [](SceneEntity& e) { return e.light ? &e.light->night_only : nullptr; });
    }
    if (component("Character", &SceneEntity::character, [] { return SceneCharacter{}; })) {
        number(
            "Walk speed", [](SceneEntity& e) { return e.character ? &e.character->speed : nullptr; }, 0, 100);
        number(
            "Run speed", [](SceneEntity& e) { return e.character ? &e.character->run_speed : nullptr; }, 0,
            100);
        number(
            "Acceleration", [](SceneEntity& e) { return e.character ? &e.character->acceleration : nullptr; },
            0, 10000);
        number(
            "Collision X", [](SceneEntity& e) { return e.character ? &e.character->collision.x : nullptr; },
            0.05F, 16);
        number(
            "Collision Y", [](SceneEntity& e) { return e.character ? &e.character->collision.y : nullptr; },
            0.05F, 16);
        toggle_field("Water blocks",
                     [](SceneEntity& e) { return e.character ? &e.character->water : nullptr; });
        toggle_field("Solid blocks",
                     [](SceneEntity& e) { return e.character ? &e.character->solid : nullptr; });
        toggle_field("Buildings block",
                     [](SceneEntity& e) { return e.character ? &e.character->buildings : nullptr; });
        toggle_field("Face movement",
                     [](SceneEntity& e) { return e.character ? &e.character->face_movement : nullptr; });
        ImGui::TextWrapped("Select one character to edit player controls and camera settings.");
    }
    ImGui::SeparatorText("Script");
    const auto script = [](SceneEntity& e) {
        return &e.script;
    };
    const bool mixed_script = selection_mixed(entities, script);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##multi script", mixed_script ? "Mixed"
                                            : entities.front()->script.empty()
                                                ? "None"
                                                : entities.front()->script.c_str())) {
        if (ImGui::Selectable("None")) set_selection_field(entities, script, std::string{});
        for (const auto& name : Scripts::list(folder_.parent_path() / "scripts"))
            if (ImGui::Selectable(name.c_str())) set_selection_field(entities, script, name);
        ImGui::EndCombo();
    }
}

bool SceneEditor::inspector(Assets& assets) {
    if (!ImGui::Begin(inspector_id, &show_inspector)) {
        ImGui::End();
        return false;
    }
    bool assets_changed = false;
    if (primary_ < 0) {
        ImGui::TextDisabled("Select an entity in the Scene view or the Hierarchy.");
        ImGui::End();
        return false;
    }
    if (selection_.size() > 1) {
        multi_inspector(assets);
        ImGui::End();
        return false;
    }
    auto& e = edited_.entities[static_cast<std::size_t>(primary_)];
    ImGui::TextUnformatted("Name");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##name", &e.name);

    double position[2] = {global_coordinate(e.position.chunk.x, e.position.local.x),
                          global_coordinate(e.position.chunk.y, e.position.local.y)};
    ImGui::TextUnformatted("Position");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::DragScalarN("##position", ImGuiDataType_Double, position, 2, 0.05F, nullptr, nullptr, "%.2f"))
        e.position = from_global(position[0], position[1]);
    float degrees = e.angle * 180 / pi;
    ImGui::TextUnformatted("Rotation (degrees)");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::DragFloat("##angle", &degrees, 1, -360, 360, "%.1f")) e.angle = degrees * pi / 180;

    prefab_controls_.apply = {-1, -1};
    if (!e.prefab.empty()) {
        // A placed prefab: its components come from the prefab. Edits here can be applied to the
        // prefab (and so to every copy), reverted, or kept by unlinking this copy.
        ImGui::Separator();
        const auto found = prefabs_.find(e.prefab);
        if (found == prefabs_.end()) {
            ImGui::TextColored({0.95F, 0.55F, 0.4F, 1}, "Prefab \"%s\" is missing.", e.prefab.c_str());
            if (ImGui::Button("Unlink", {-1, 0})) e.prefab.clear();
        } else {
            SceneEntity as_prefab = e;
            apply_prefab(found->second, as_prefab);
            const bool modified = !(as_prefab == e);
            ImGui::Text("Prefab: %s%s", e.prefab.c_str(), modified ? "  (modified)" : "");
            const float third = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3;
            ImGui::BeginDisabled(!modified);
            if (ImGui::Button("Apply", {third, 0})) {
                auto updated = e;
                updated.name = found->second.name;
                updated.position = {};
                updated.angle = 0;
                updated.prefab.clear();
                updated.hidden = updated.locked = false;
                found->second = updated;
                sync_prefab_copies();
                log_(false, "Applied to prefab \"" + e.prefab + "\" and its copies in this scene.");
            }
            prefab_controls_.apply = {(ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) / 2,
                                      (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) / 2};
            ImGui::SetItemTooltip("Make these components the prefab's, for every copy.");
            ImGui::SameLine();
            if (ImGui::Button("Revert", {third, 0})) apply_prefab(found->second, e);
            ImGui::SetItemTooltip("Take the prefab's components back.");
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Unlink", {third, 0})) e.prefab.clear();
            ImGui::SetItemTooltip("Keep this entity as it is, no longer following the prefab.");
        }
    }

    if (e.visual) {
        bool keep = true;
        if (ImGui::CollapsingHeader("Visual", &keep, ImGuiTreeNodeFlags_DefaultOpen)) {
            auto& v = *e.visual;
            assets_changed |= visual_material(v, assets);
            ImGui::TextUnformatted("Size");
            ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat2("##size", &v.size.x, 0.02F, 0.05F, 64, "%.2f");
        }
        if (!keep) e.visual.reset();
    }
    if (e.light) {
        bool keep = true;
        if (ImGui::CollapsingHeader("Light", &keep, ImGuiTreeNodeFlags_DefaultOpen)) {
            auto& l = *e.light;
            ImGui::TextUnformatted("Color");
            ImGui::SetNextItemWidth(-1);
            ImGui::ColorEdit3("##color", l.color.data());
            ImGui::TextUnformatted("Radius");
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##radius", &l.radius, 0.5F, 64, "%.1f", ImGuiSliderFlags_Logarithmic);
            ImGui::TextUnformatted("Intensity");
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##intensity", &l.intensity, 0, 8);
            ImGui::TextUnformatted("Height");
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##height", &l.height, 0.1F, 16);
            ImGui::SetItemTooltip("Higher lights spread more evenly; low lights graze surfaces.");
            ImGui::TextUnformatted("Flicker");
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##flicker", &l.flicker, 0, 1);
            ImGui::SetItemTooltip("How much the light wavers, as a flame or a failing lamp does.");
            ImGui::Checkbox("Only at night", &l.night_only);
            ImGui::SetItemTooltip("Lit as the scene's daylight fades, like a street lamp; out by day.");
            ImGui::TextDisabled("Turn on View > Lit in the Scene view to see lights.");
        }
        if (!keep) e.light.reset();
    }
    character_section(e, assets);
    const auto script_folder = folder_.parent_path() / "scripts";
    if (!e.script.empty()) {
        bool keep = true;
        if (ImGui::CollapsingHeader("Script", &keep, ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SetNextItemWidth(-button_width("Edit") - ImGui::GetStyle().ItemSpacing.x);
            if (ImGui::BeginCombo("##script", e.script.c_str())) {
                for (const auto& name : Scripts::list(script_folder))
                    if (ImGui::Selectable(name.c_str(), name == e.script)) e.script = name;
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button("Edit", {-1, 0})) {
                const auto url = "file://" + (script_folder / e.script).string();
                if (SDL_OpenURL(url.c_str()) != 0)
                    log_(true, std::string("Cannot open the script: ") + SDL_GetError());
            }
            ImGui::SetItemTooltip("Open in your code editor. Changes apply the next time you press Play.");
            const auto error = scripts_.syntax_error(script_folder / e.script);
            ImGui::PushTextWrapPos(0);
            if (error.empty())
                ImGui::TextDisabled("No syntax errors.");
            else
                ImGui::TextColored({0.95F, 0.55F, 0.4F, 1}, "%s", error.c_str());
            ImGui::PopTextWrapPos();
        }
        if (!keep) e.script.clear();
    }
    ImGui::Dummy({0, 4});
    const bool full = e.visual && e.light && e.character && !e.script.empty();
    ImGui::BeginDisabled(full);
    if (ImGui::Button("Add Component", {-1, 0})) ImGui::OpenPopup("add component");
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("add component")) {
        if (!e.visual && ImGui::MenuItem("Visual")) {
            e.visual = SceneVisual{};
            if (!assets.materials.empty()) e.visual->material = assets.materials.front().name;
        }
        if (!e.light && ImGui::MenuItem("Light")) e.light = SceneLight{};
        if (!e.character && ImGui::MenuItem("Character")) e.character = SceneCharacter{};
        if (e.script.empty() && ImGui::BeginMenu("Script")) {
            for (const auto& name : Scripts::list(script_folder))
                if (ImGui::MenuItem(name.c_str())) e.script = name;
            ImGui::Separator();
            ImGui::TextUnformatted("New script");
            ImGui::SetNextItemWidth(180);
            ImGui::InputTextWithHint("##new script", "name.lua", &new_script_);
            ImGui::SameLine();
            if (ImGui::Button("Create")) {
                auto name = new_script_;
                if (!name.ends_with(".lua")) name += ".lua";
                try {
                    Scripts::create(script_folder, name);
                    e.script = name;
                    new_script_.clear();
                    log_(false, "Created scripts/" + name + ".");
                    ImGui::CloseCurrentPopup();
                } catch (const std::exception& error) {
                    log_(true, error.what());
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }
    // This entity's problems, from the same checks that block saving.
    SceneFile one;
    one.entities = {e};
    if (const auto problems = one.problems(assets); !problems.empty()) {
        ImGui::Separator();
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored({0.95F, 0.55F, 0.4F, 1}, "%s", problems.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::End();
    return assets_changed;
}
} // namespace seed::editor
