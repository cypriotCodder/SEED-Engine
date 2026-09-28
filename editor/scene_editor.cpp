#include "scene_editor.hpp"
#include "project.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <imgui_stdlib.h>

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
    clear_selection();
    view_visible_ = false;
}

bool SceneEditor::save(const Assets& assets) {
    if (!dirty()) return true;
    if (const auto problems = edited_.problems(assets); !problems.empty()) {
        log_(true, "Scene \"" + name_ + "\" not saved. Fix these first:\n" + problems);
        return false;
    }
    save_scene(folder_ / (name_ + ".json"), edited_);
    saved_ = edited_;
    log_(false, "Saved scene \"" + name_ + "\".");
    return true;
}

void SceneEditor::sync(const Assets& assets) {
    const bool materials_changed = !(assets.materials == rendered_ && (renderer_ || rendered_.empty()));
    if (materials_changed || assets.terrain != compiled_terrain_) {
        // Recompile the terrain against the preview's material order.
        compiled_terrain_ = assets.terrain;
        terrain_.reset();
        terrain_error_.clear();
        cache_.cells.clear();
        if (assets.terrain.enabled() && !assets.materials.empty()) try {
                Materials registry;
                Assets names;
                names.materials = assets.materials;
                for (auto& m : names.materials)
                    m.texture.clear();
                names.register_materials(registry);
                terrain_ = std::make_unique<Terrain>(assets.terrain, registry);
            } catch (const std::exception&) {
                terrain_error_ = "Terrain not shown: fix the problems in the Terrain panel.";
            }
    }
    if (!materials_changed) return;
    rendered_ = assets.materials;
    renderer_.reset();
    renderer_error_.clear();
    if (rendered_.empty()) return;
    // The editor has no asset pack yet, so textured materials preview as their generated tile.
    Assets preview;
    preview.materials = rendered_;
    for (auto& material : preview.materials)
        material.texture.clear();
    try {
        Materials registry;
        preview.register_materials(registry);
        renderer_ = std::make_unique<Renderer>(Pack{}, registry);
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
}

void SceneEditor::toggle(int index) {
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
        edited_.entities.push_back(e);
        selection_.push_back(static_cast<int>(edited_.entities.size()) - 1);
    }
    primary_ = selection_.back();
}

void SceneEditor::draw(const Assets& assets) {
    if (!loaded()) return;
    sync(assets);
    clamp_selection();
    if (show_scene)
        scene_view();
    else
        view_visible_ = false;
    if (show_hierarchy) hierarchy();
    if (show_inspector) inspector(assets);
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
            break;
        }
    }
}

void SceneEditor::toolbar() {
    const auto tool_button = [&](const char* label, Tool tool, const char* tip) {
        const bool active = tool_ == tool;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(label)) tool_ = tool;
        if (active) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s", tip);
        ImGui::SameLine();
    };
    tool_button("Move", Tool::move,
                "Drag the arrows to move along an axis, or the centre to move freely (W).");
    tool_button("Rotate", Tool::rotate, "Drag the ring to turn the selection about its centre (E).");
    tool_button("Scale", Tool::scale,
                "Drag an end to resize along that axis, or the centre to resize evenly (R).");
    ImGui::SetNextItemWidth(95);
    ImGui::Combo("##snap", &snap_, snap_labels, 4);
    ImGui::SetItemTooltip("Moves land on this grid, sizes use it, and rotation snaps to 15 degrees.\n"
                          "Hold Shift while dragging to flip snapping.");
    ImGui::SameLine();
    ImGui::Checkbox("Lit", &lit_);
    ImGui::SetItemTooltip("Show the game's lighting and lights instead of flat full brightness.");
    ImGui::SameLine();
    ImGui::Checkbox("Terrain", &show_terrain_);
    if (terrain_)
        ImGui::SetItemTooltip(
            "Show the generated world for the seed in the Terrain panel.\nLast refill: %.1f ms", cache_ms_);
    else
        ImGui::SetItemTooltip("%s", terrain_error_.empty() ? "This project has no terrain yet."
                                                           : terrain_error_.c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(selection_.empty());
    if (ImGui::Button("Frame")) frame_selection();
    ImGui::SetItemTooltip("Centre the view on the selection (F).");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled(zoom_ >= 3.2F ? "%.0f%%" : "%.1f%%", zoom_ / 32 * 100);
}

void SceneEditor::scene_view() {
    // The Scene tab is in front after a project opens; the request waits until docking settles.
    if (focus_ && --focus_ == 0) ImGui::SetNextWindowFocus();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    const bool open = ImGui::Begin(title("Scene", dirty(), scene_id).c_str(), &show_scene,
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    view_visible_ = false;
    if (!open) {
        ImGui::End();
        return;
    }
    auto& io = ImGui::GetIO();
    auto* draw = ImGui::GetWindowDrawList();

    const ImVec2 top = ImGui::GetCursorScreenPos();
    const float bar = ImGui::GetFrameHeightWithSpacing() + 6;
    ImGui::SetCursorScreenPos({top.x + 8, top.y + 4});
    toolbar();
    const ImVec2 view_top{top.x, top.y + bar};
    ImGui::SetCursorScreenPos(view_top);
    const ImVec2 size = ImGui::GetContentRegionAvail();
    if (size.x < 2 || size.y < 2) {
        ImGui::End();
        return;
    }
    ImGui::InvisibleButton("view", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
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
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
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
    if (drag_ != Drag::none && drag_ != Drag::box && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2))
        continue_drag(mouse);
    if (drag_ != Drag::none && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
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
    r.lighting = game_lighting_;
    if (!lit_) {
        r.lighting.ambient = {1, 1, 1};
        r.lighting.haze_amount = 0;
    }
    r.begin(width, height, 0, 0, zoom_ * sx);
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
            r.light(at.x, at.y, l.radius, l.color[0], l.color[1], l.color[2], l.intensity, l.height);
            ++lights;
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
                cache_.cells[static_cast<std::size_t>(y * cache_.columns + x)] = {sample.material,
                                                                                  sample.object};
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
    for (int y = vy0; y <= vy1; ++y)
        for (int x = vx0; x <= vx1; ++x) {
            const auto cell =
                cache_.cells[static_cast<std::size_t>((y - cache_.y0) * cache_.columns + (x - cache_.x0))];
            const float cx = origin.x + (static_cast<float>(x) + 0.5F) * size;
            const float cy = origin.y + (static_cast<float>(y) + 0.5F) * size;
            renderer_->sprite(cell.ground, cx, cy, size, size);
            // Objects show only up close: zoomed out, a block's one sample would blow a single tree
            // up to the size of the whole block.
            if (block == 1 && cell.object != no_object)
                renderer_->sprite(static_cast<MaterialId>(cell.object - 1), cx, cy);
        }
}

void SceneEditor::scene_menu() {
    const bool locked = dirty();
    ImGui::SetNextItemWidth(-60);
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

void SceneEditor::hierarchy() {
    if (!ImGui::Begin(hierarchy_id, &show_hierarchy)) {
        ImGui::End();
        return;
    }
    scene_menu();
    if (ImGui::SmallButton("Create")) create_at(camera_);
    ImGui::SameLine();
    ImGui::BeginDisabled(selection_.empty());
    if (ImGui::SmallButton("Duplicate")) duplicate_selection();
    ImGui::SameLine();
    if (ImGui::SmallButton("Delete")) delete_selection();
    ImGui::EndDisabled();
    ImGui::BeginChild("entities", {0, 0}, ImGuiChildFlags_Borders);
    for (int i = 0; i < static_cast<int>(edited_.entities.size()); ++i) {
        const auto& e = edited_.entities[static_cast<std::size_t>(i)];
        ImGui::PushID(i);
        if (ImGui::Selectable(e.name.empty() ? "(unnamed)" : e.name.c_str(), is_selected(i),
                              ImGuiSelectableFlags_AllowDoubleClick)) {
            const auto& io = ImGui::GetIO();
            if (io.KeyCtrl)
                toggle(i);
            else if (io.KeyShift && primary_ >= 0) {
                // A range from the primary selection to here.
                for (int k = std::min(primary_, i); k <= std::max(primary_, i); ++k)
                    if (!is_selected(k)) toggle(k);
                primary_ = i;
            } else
                select_only(i);
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) frame_selection();
        }
        if (ImGui::BeginPopupContextItem()) {
            if (!is_selected(i)) select_only(i);
            if (ImGui::MenuItem("Frame", "F")) frame_selection();
            if (ImGui::MenuItem("Copy", "Cmd+C")) copy_selection();
            if (ImGui::MenuItem("Duplicate", "Cmd+D")) duplicate_selection();
            if (ImGui::MenuItem("Delete", "Del")) delete_selection();
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (edited_.entities.empty())
        ImGui::TextDisabled("Empty scene. Press Create, or right-click in the Scene view.");
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive()) {
        if (ImGui::Shortcut(ImGuiKey_Delete) || ImGui::Shortcut(ImGuiKey_Backspace)) delete_selection();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D)) duplicate_selection();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C)) copy_selection();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V)) paste();
        if (ImGui::Shortcut(ImGuiKey_F)) frame_selection();
    }
    ImGui::EndChild();
    ImGui::End();
}

void SceneEditor::inspector(const Assets& assets) {
    if (!ImGui::Begin(inspector_id, &show_inspector)) {
        ImGui::End();
        return;
    }
    if (primary_ < 0) {
        ImGui::TextDisabled("Select an entity in the Scene view or the Hierarchy.");
        ImGui::End();
        return;
    }
    // The Inspector edits the primary entity. With several selected, each change is then applied
    // to the others too: a move as the same offset, anything else as the new value.
    auto& e = edited_.entities[static_cast<std::size_t>(primary_)];
    const SceneEntity before = e;
    const bool several = selection_.size() > 1;
    if (several) {
        ImGui::TextColored({0.45F, 0.9F, 0.65F, 1}, "%zu entities selected", selection_.size());
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Showing \"%s\". Changes apply to every selected entity that has the field.",
                            e.name.c_str());
        ImGui::PopTextWrapPos();
    } else {
        ImGui::TextUnformatted("Name");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##name", &e.name);
    }

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

    if (e.visual) {
        bool keep = true;
        if (ImGui::CollapsingHeader("Visual", &keep, ImGuiTreeNodeFlags_DefaultOpen)) {
            auto& v = *e.visual;
            ImGui::TextUnformatted("Material");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##material", v.material.c_str())) {
                for (const auto& m : assets.materials)
                    if (ImGui::Selectable(m.name.c_str(), m.name == v.material)) v.material = m.name;
                ImGui::EndCombo();
            }
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
            ImGui::TextDisabled("Turn on Lit in the Scene view to see lights.");
        }
        if (!keep) e.light.reset();
    }
    const auto script_folder = folder_.parent_path() / "scripts";
    if (!e.script.empty()) {
        bool keep = true;
        if (ImGui::CollapsingHeader("Script", &keep, ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SetNextItemWidth(-70);
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
    const bool full = e.visual && e.light && !e.script.empty();
    ImGui::BeginDisabled(full);
    if (ImGui::Button("Add Component", {-1, 0})) ImGui::OpenPopup("add component");
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("add component")) {
        if (!e.visual && ImGui::MenuItem("Visual")) {
            e.visual = SceneVisual{};
            if (!assets.materials.empty()) e.visual->material = assets.materials.front().name;
        }
        if (!e.light && ImGui::MenuItem("Light")) e.light = SceneLight{};
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
    if (several && !(e == before)) {
        const auto moved = relative(e.position, before.position);
        for (const int i : selection_) {
            if (i == primary_) continue;
            auto& other = edited_.entities[static_cast<std::size_t>(i)];
            if (!(e.position == before.position)) other.position.move(moved);
            if (e.angle != before.angle) other.angle = e.angle;
            // Components: added or removed on all, or each changed field copied where present.
            if (e.visual.has_value() != before.visual.has_value())
                other.visual = e.visual ? (other.visual ? other.visual : e.visual) : std::nullopt;
            else if (e.visual && other.visual) {
                if (e.visual->material != before.visual->material)
                    other.visual->material = e.visual->material;
                if (!(e.visual->size == before.visual->size)) other.visual->size = e.visual->size;
            }
            if (e.light.has_value() != before.light.has_value())
                other.light = e.light ? (other.light ? other.light : e.light) : std::nullopt;
            else if (e.light && other.light) {
                if (e.light->color != before.light->color) other.light->color = e.light->color;
                if (e.light->radius != before.light->radius) other.light->radius = e.light->radius;
                if (e.light->intensity != before.light->intensity)
                    other.light->intensity = e.light->intensity;
                if (e.light->height != before.light->height) other.light->height = e.light->height;
            }
            if (e.script != before.script) other.script = e.script;
        }
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
}
} // namespace seed::editor
