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

double global(std::int64_t chunk, float local) {
    return static_cast<double>(chunk) * chunk_side + local;
}
WorldPosition from_global(double x, double y) {
    WorldPosition p;
    p.chunk = {static_cast<std::int64_t>(std::floor(x / chunk_side)),
               static_cast<std::int64_t>(std::floor(y / chunk_side))};
    p.local = {static_cast<float>(x - static_cast<double>(p.chunk.x) * chunk_side),
               static_cast<float>(y - static_cast<double>(p.chunk.y) * chunk_side)};
    p.move({}); // Rounding can leave an offset of exactly chunk_side.
    return p;
}
Vec2 extent(const SceneEntity& e) {
    return e.visual ? e.visual->size : Vec2{marker_size, marker_size};
}
ImU32 rgba(float r, float g, float b, float a) {
    return ImGui::ColorConvertFloat4ToU32({r, g, b, a});
}
std::string title(const char* name, bool edited, const char* id) {
    return std::string(name) + (edited ? " *" : "") + id;
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
    selected_ = edited_.entities.empty() ? -1 : 0;
    camera_ = {};
    moving_ = false;
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
    selected_ = -1;
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
        if (!nearby(e.position.chunk, camera_.chunk, view_reach)) continue;
        const auto r = relative(point, e.position);
        // Into the entity's own frame: rotate by -angle.
        const float c = std::cos(-e.angle), s = std::sin(-e.angle);
        const float x = r.x * c - r.y * s, y = r.x * s + r.y * c;
        const auto size = extent(e);
        if (std::abs(x) <= size.x / 2 && std::abs(y) <= size.y / 2) return i;
    }
    return -1;
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
    selected_ = static_cast<int>(edited_.entities.size()) - 1;
}

void SceneEditor::duplicate_selected() {
    if (selected_ < 0 || edited_.entities.size() >= SceneFile::capacity) return;
    auto copy = edited_.entities[static_cast<std::size_t>(selected_)];
    copy.position.move({1, -1}); // Offset so the copy is visible.
    edited_.entities.insert(edited_.entities.begin() + selected_ + 1, copy);
    ++selected_;
}

void SceneEditor::delete_selected() {
    if (selected_ < 0) return;
    edited_.entities.erase(edited_.entities.begin() + selected_);
    selected_ = std::min(selected_, static_cast<int>(edited_.entities.size()) - 1);
}

void SceneEditor::frame_selection() {
    if (selected_ >= 0) camera_ = edited_.entities[static_cast<std::size_t>(selected_)].position;
}

void SceneEditor::draw(const Assets& assets) {
    if (!loaded()) return;
    sync(assets);
    if (selected_ >= static_cast<int>(edited_.entities.size())) selected_ = -1;
    if (show_scene)
        scene_view();
    else
        view_visible_ = false;
    if (show_hierarchy) hierarchy();
    if (show_inspector) inspector(assets);
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
    ImGui::BeginDisabled(selected_ < 0);
    if (ImGui::Button("Frame")) frame_selection();
    ImGui::SetItemTooltip("Centre the view on the selected entity (F).");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled(zoom_ >= 3.2F ? "%.0f%%" : "%.1f%%", zoom_ / 32 * 100);
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
    // Select with a left click; dragging moves the selection. Shift snaps to half-unit steps.
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        selected_ = pick(mouse);
        moving_ = selected_ >= 0;
        if (moving_) {
            grab_ = to_world(mouse);
            grab_entity_ = edited_.entities[static_cast<std::size_t>(selected_)].position;
        }
    }
    if (moving_ && selected_ >= 0 && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2)) {
        auto target = grab_entity_;
        target.move(relative(to_world(mouse), grab_));
        if (io.KeyShift)
            target = from_global(std::round(global(target.chunk.x, target.local.x) * 2) / 2,
                                 std::round(global(target.chunk.y, target.local.y) * 2) / 2);
        edited_.entities[static_cast<std::size_t>(selected_)].position = target;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) moving_ = false;
    // A right click that did not pan opens the context menu.
    if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
        io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Right] < 16) {
        menu_at_ = to_world(mouse);
        const int under = pick(mouse);
        if (under >= 0) selected_ = under;
        ImGui::OpenPopup("view menu");
    }
    if (ImGui::BeginPopup("view menu")) {
        if (ImGui::MenuItem("Create Entity Here")) create_at(menu_at_);
        if (selected_ >= 0) {
            ImGui::Separator();
            if (ImGui::MenuItem("Duplicate", "Cmd+D")) duplicate_selected();
            if (ImGui::MenuItem("Delete", "Del")) delete_selected();
        }
        ImGui::EndPopup();
    }
    if (ImGui::IsWindowFocused()) {
        if (ImGui::Shortcut(ImGuiKey_Delete) || ImGui::Shortcut(ImGuiKey_Backspace)) delete_selected();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D)) duplicate_selected();
        if (ImGui::Shortcut(ImGuiKey_F)) frame_selection();
    }

    // The scene itself, then overlays on top: grid, markers, selection.
    draw->PushClipRect(view_min_, view_max_, true);
    if (renderer_) {
        draw->AddCallback(draw_callback, this);
        draw->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    } else
        draw->AddRectFilled(view_min_, view_max_, rgba(0.07F, 0.08F, 0.09F, 1));
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
        if (!nearby(e.position.chunk, camera_.chunk, view_reach)) continue;
        const auto centre = to_screen(e.position);
        const bool selected = static_cast<int>(i) == selected_;
        if (!e.visual || !renderer_) { // A diamond marks entities with nothing to draw.
            const float r = marker_size / 2 * zoom_;
            const ImU32 color = e.light ? rgba(1, 0.8F, 0.4F, 0.9F) : rgba(0.6F, 0.8F, 1, 0.9F);
            draw->AddQuadFilled({centre.x, centre.y - r}, {centre.x + r, centre.y}, {centre.x, centre.y + r},
                                {centre.x - r, centre.y}, color);
        }
        if (!selected) continue;
        const auto size = extent(e);
        const float c = std::cos(e.angle), s = std::sin(e.angle);
        ImVec2 corners[4];
        const float hx[4] = {-1, 1, 1, -1}, hy[4] = {-1, -1, 1, 1};
        for (int k = 0; k < 4; ++k) {
            const float x = hx[k] * size.x / 2, y = hy[k] * size.y / 2;
            corners[k] = {centre.x + (x * c - y * s) * zoom_, centre.y - (x * s + y * c) * zoom_};
        }
        draw->AddPolyline(corners, 4, rgba(0.45F, 0.9F, 0.65F, 1), 2.0F, ImDrawFlags_Closed);
        if (e.light) draw->AddCircle(centre, e.light->radius * zoom_, rgba(1, 0.8F, 0.4F, 0.5F), 64, 1.5F);
    }
    if (!renderer_) {
        const char* message = renderer_error_.empty()
                                  ? "Add a material in the Materials panel to see visuals."
                                  : renderer_error_.c_str();
        draw->AddText({view_min_.x + 12, view_max_.y - 28}, rgba(1, 1, 1, 0.6F), message);
    }
    if (hovered) {
        const auto p = to_world(mouse);
        char text[96];
        std::snprintf(text, sizeof(text), "%.2f, %.2f", global(p.chunk.x, p.local.x),
                      global(p.chunk.y, p.local.y));
        draw->AddText({view_min_.x + 10, view_min_.y + 8}, rgba(1, 1, 1, 0.7F), text);
    }
    draw->PopClipRect();
    ImGui::End();
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
    if (lit_)
        r.lighting = game_lighting_;
    else {
        r.lighting = game_lighting_;
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
        if (!nearby(e.position.chunk, camera_.chunk, view_reach)) continue;
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
    ImGui::BeginDisabled(selected_ < 0);
    if (ImGui::SmallButton("Duplicate")) duplicate_selected();
    ImGui::SameLine();
    if (ImGui::SmallButton("Delete")) delete_selected();
    ImGui::EndDisabled();
    ImGui::BeginChild("entities", {0, 0}, ImGuiChildFlags_Borders);
    for (int i = 0; i < static_cast<int>(edited_.entities.size()); ++i) {
        const auto& e = edited_.entities[static_cast<std::size_t>(i)];
        ImGui::PushID(i);
        if (ImGui::Selectable(e.name.empty() ? "(unnamed)" : e.name.c_str(), selected_ == i,
                              ImGuiSelectableFlags_AllowDoubleClick)) {
            selected_ = i;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) frame_selection();
        }
        if (ImGui::BeginPopupContextItem()) {
            selected_ = i;
            if (ImGui::MenuItem("Frame", "F")) frame_selection();
            if (ImGui::MenuItem("Duplicate", "Cmd+D")) duplicate_selected();
            if (ImGui::MenuItem("Delete", "Del")) delete_selected();
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (edited_.entities.empty())
        ImGui::TextDisabled("Empty scene. Press Create, or right-click in the Scene view.");
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive()) {
        if (ImGui::Shortcut(ImGuiKey_Delete) || ImGui::Shortcut(ImGuiKey_Backspace)) delete_selected();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D)) duplicate_selected();
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
    if (selected_ < 0) {
        ImGui::TextDisabled("Select an entity in the Scene view or the Hierarchy.");
        ImGui::End();
        return;
    }
    auto& e = edited_.entities[static_cast<std::size_t>(selected_)];
    ImGui::TextUnformatted("Name");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##name", &e.name);

    double position[2] = {global(e.position.chunk.x, e.position.local.x),
                          global(e.position.chunk.y, e.position.local.y)};
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
    ImGui::Dummy({0, 4});
    const bool full = e.visual && e.light;
    ImGui::BeginDisabled(full);
    if (ImGui::Button("Add Component", {-1, 0})) ImGui::OpenPopup("add component");
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("add component")) {
        if (!e.visual && ImGui::MenuItem("Visual")) {
            e.visual = SceneVisual{};
            if (!assets.materials.empty()) e.visual->material = assets.materials.front().name;
        }
        if (!e.light && ImGui::MenuItem("Light")) e.light = SceneLight{};
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
}
} // namespace seed::editor
