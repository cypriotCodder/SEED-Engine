#include "ui_test.hpp"
#include "scene_editor.hpp"
#include <cmath>
#include <memory>

namespace seed::editor {
namespace {
// On macOS the command key arrives as Super and ImGui treats it as Ctrl, as the SDL backend does.
#ifdef __APPLE__
constexpr ImGuiKey command_key = ImGuiMod_Super;
#else
constexpr ImGuiKey command_key = ImGuiMod_Ctrl;
#endif
constexpr float pi = 3.14159265F;
} // namespace

UiTest::UiTest(SceneEditor& scene, Log log) : scene_(scene), log_(std::move(log)) {
    // The sample project: Player at (0.5, 0.5), Campfire at (3.5, 2.5) with a 0.8 visual, and a
    // 1x1 Crate at (-2, -2). The view starts at the origin, 32 pixels per tile.
    add(30, [] {}); // Let docking, and a new window's first-launch resizing, settle.

    // Click selects.
    click([this] { return at("Campfire"); });
    add(2, [this] { check(scene_.selection() == std::vector<int>{1}, "clicking an entity selects it"); });

    // Dragging moves: two tiles right.
    drag([this] { return at("Campfire"); }, {64, 0});
    add(2, [this] { check(std::abs(x_of("Campfire") - 5.5) < 0.02, "dragging moves the entity"); });

    // Undo and redo the move.
    key(ImGuiKey_Z, true);
    add(3, [this] { check(std::abs(x_of("Campfire") - 3.5) < 0.02, "Cmd+Z undoes the move"); });
    key(ImGuiKey_Z, true, true);
    add(3, [this] { check(std::abs(x_of("Campfire") - 5.5) < 0.02, "Shift+Cmd+Z redoes it"); });

    // A box dragged over empty ground selects everything inside it.
    drag([this] { return world_point(-3.3, -3.3); }, {9.6F * 32, -8 * 32});
    add(2, [this] { check(scene_.selection().size() == 3, "a box drag selects all three entities"); });

    // Copy and paste, then delete the pasted copies.
    key(ImGuiKey_C, true);
    key(ImGuiKey_V, true);
    add(3, [this] {
        check(scene_.scene().entities.size() == 6 && scene_.selection() == std::vector<int>{3, 4, 5},
              "Cmd+C then Cmd+V pastes copies, which become the selection");
    });
    key(ImGuiKey_Delete);
    add(3, [this] { check(scene_.scene().entities.size() == 3, "Delete removes the selection"); });

    // The rotate handle: a quarter turn anticlockwise, dragging the ring's right side to its top.
    click([this] { return at("Campfire"); });
    key(ImGuiKey_E);
    drag([this] { return ImVec2{at("Campfire").x + 62, at("Campfire").y}; }, {-62, -62});
    add(2, [this] {
        const auto angle = scene_.scene().entities[1].angle;
        check(std::abs(angle - pi / 2) < 0.03F, "the rotate ring turns the entity");
    });

    // The scale handle: dragging the x handle twice as far from the centre doubles the width.
    key(ImGuiKey_R);
    drag([this] { return ImVec2{at("Campfire").x + 72, at("Campfire").y}; }, {72, 0});
    add(2, [this] {
        const auto& v = scene_.scene().entities[1].visual;
        check(v && std::abs(v->size.x - 1.6F) < 0.03F && std::abs(v->size.y - 0.8F) < 0.001F,
              "the x scale handle doubles the width only");
    });

    // Shift+click adds to the selection; Escape clears it.
    key(ImGuiKey_W);
    click([this] { return at("Player"); }, true);
    add(2,
        [this] { check(scene_.selection() == std::vector<int>{0, 1}, "Shift+click adds to the selection"); });
    key(ImGuiKey_Escape);
    add(2, [this] { check(scene_.selection().empty(), "Escape clears the selection"); });

    // The Hierarchy. Rows are Player, Campfire, Crate.
    click([this] { return row(2, 0); });
    add(2,
        [this] { check(scene_.selection() == std::vector<int>{2}, "clicking a Hierarchy row selects it"); });
    key(ImGuiKey_Enter); // Rename in place.
    add(3, [] {});
    type("Box");
    key(ImGuiKey_Enter);
    add(3, [this] { check(scene_.scene().entities[2].name == "Box", "Return renames in place"); });

    click([this] { return row(2, 1); }); // Hide Box.
    add(2, [this] { check(scene_.scene().entities[2].hidden, "the Show checkbox hides an entity"); });
    click([this] { return world_point(-2, -2); });
    add(2, [this] { check(scene_.selection().empty(), "a hidden entity cannot be clicked in the view"); });

    click([this] { return row(0, 2); }); // Lock Player.
    click([this] { return at("Player"); });
    add(2, [this] {
        check(scene_.scene().entities[0].locked && scene_.selection().empty(),
              "a locked entity cannot be picked");
    });

    // Drag Campfire's row onto Player's: Campfire moves to the top.
    drag([this] { return row(1, 0); }, [this] { return row(0, 0); });
    add(3, [this] {
        const auto& e = scene_.scene().entities;
        check(e[0].name == "Campfire" && e[1].name == "Player" && e[2].name == "Box",
              "dragging a row reorders the scene");
    });

    // Search narrows the list.
    click([this] { return scene_.search_box(); });
    type("bo");
    add(3, [this] {
        const auto& rows = scene_.rows();
        check(rows.size() == 3 && rows[2].name.x >= 0 && rows[0].name.x < 0 && rows[1].name.x < 0,
              "search shows only matching entities");
    });

    // Prefabs. Clear the search first; Campfire (now first) becomes a prefab.
    click([this] { return scene_.search_box(); });
    key(ImGuiKey_A, true);
    key(ImGuiKey_Backspace);
    click([this] { return at("Campfire"); });
    click([this] { return scene_.prefab_controls().new_from_selection; });
    add(3, [] {});
    key(ImGuiKey_Enter); // Accept the suggested name, "Campfire".
    add(3, [this] {
        check(scene_.prefabs().count("Campfire") && scene_.scene().entities[0].prefab == "Campfire",
              "New from Selection makes a prefab and links the entity");
    });
    // Drag it from the Prefabs panel into the view.
    drag(
        [this] {
            const auto& rows = scene_.prefab_controls().rows;
            const auto found = rows.find("Campfire");
            return found == rows.end() ? ImVec2{-1, -1} : found->second;
        },
        [this] { return world_point(8, -3); });
    add(3, [this] {
        const auto& e = scene_.scene().entities;
        check(e.size() == 4 && e[3].prefab == "Campfire" && e[3].name == "Campfire 2" &&
                  std::abs(x_of("Campfire 2") - 8) < 0.05,
              "dragging a prefab into the view places a linked copy");
    });
    // Widen the copy, apply it to the prefab: the original copy follows.
    click([this] { return at("Campfire 2"); });
    key(ImGuiKey_R);
    drag([this] { return ImVec2{at("Campfire 2").x + 72, at("Campfire 2").y}; }, {36, 0});
    click([this] { return scene_.prefab_controls().apply; });
    add(3, [this] {
        const auto& e = scene_.scene().entities;
        check(e[3].visual && e[0].visual && std::abs(e[3].visual->size.x - 2.4F) < 0.05F &&
                  e[0].visual->size == e[3].visual->size &&
                  scene_.prefabs().at("Campfire").visual->size == e[3].visual->size,
              "Apply updates the prefab and every copy");
    });
    key(ImGuiKey_Z, true);
    add(3, [this] {
        const auto& e = scene_.scene().entities;
        check(e[0].visual && std::abs(e[0].visual->size.x - 1.6F) < 0.05F, "Cmd+Z undoes Apply");
    });

    // Characters: the Create menu makes a player and an NPC; the checkbox moves the player role.
    const auto players = [this] {
        int count = 0;
        for (const auto& e : scene_.scene().entities)
            count += e.character && e.character->player;
        return count;
    };
    click([this] { return scene_.control("create"); });
    click([this] { return scene_.control("create player"); });
    add(3, [this, players] {
        const auto& e = scene_.scene().entities.back();
        check(players() == 1 && e.character && e.character->player && e.visual,
              "Create > Player adds the player");
    });
    click([this] { return scene_.control("create"); });
    click([this] { return scene_.control("create npc"); });
    add(3, [this, players] {
        const auto& e = scene_.scene().entities.back();
        check(players() == 1 && e.name == "NPC" && e.character && !e.character->player,
              "Create > NPC adds an NPC");
    });
    click([this] { return scene_.control("is player"); });
    add(3, [this, players] {
        const auto& e = scene_.scene().entities.back();
        check(players() == 1 && e.character && e.character->player,
              "the player checkbox moves the role to the NPC");
    });

    // The Terrain object: selecting it, then New > Starter Island makes a terrain the scene uses.
    click([this] { return scene_.control("terrain row"); });
    add(2, [this] {
        check(scene_.terrain_selected() && scene_.selection().empty(), "the Terrain row selects the terrain");
    });
    click([this] { return scene_.control("terrain new"); });
    click([this] { return scene_.control("terrain new starter"); });
    add(3, [] {});
    key(ImGuiKey_Enter); // Accept the suggested name, "island".
    add(3, [this] {
        check(scene_.scene().terrain == "island", "New > Starter Island makes a terrain the scene uses");
    });

    // Painting: the Paint tool, the ground brush, one stroke across the view; undo takes it back.
    click([this] { return scene_.control("paint tool"); });
    click([this] { return scene_.control("brush Ground"); });
    drag([this] { return world_point(2.5, 6.5); }, [this] { return world_point(8.5, 6.5); });
    add(3, [this] {
        const auto& paint = scene_.scene().paint;
        const auto* tile = paint.find(5, 6);
        check(tile && (tile->mask & paint_ground) && paint.find(8, 6) && !paint.find(5, 9) &&
                  paint.tiles() >= 15,
              "a stroke of the ground brush paints the tiles it passes over");
    });
    key(ImGuiKey_Z, true);
    add(3, [this] { check(scene_.scene().paint.empty(), "Cmd+Z takes back the whole stroke"); });
    click([this] { return scene_.control("brush Raise"); });
    drag([this] { return world_point(3.5, 3.5); }, ImVec2{0, 0});
    add(3, [this] {
        const auto* tile = scene_.scene().paint.find(3, 3);
        check(tile && tile->mask == paint_height, "the raise brush paints height alone");
    });
    // Shapes: a rectangle paints the tiles it covers when let go, a line paints along itself, and
    // fill paints the connected tiles under the click; each is one undo step.
    click([this] { return scene_.control("brush Ground"); });
    click([this] { return scene_.control("shape Rectangle"); });
    drag([this] { return world_point(6.5, 5.5); }, [this] { return world_point(9.5, 7.5); });
    add(3, [this] {
        const auto& paint = scene_.scene().paint;
        const auto* corner = paint.find(9, 7);
        check(paint.find(6, 5) && corner && (corner->mask & paint_ground) && paint.find(8, 6) &&
                  !paint.find(10, 6) && !paint.find(7, 8) && !paint.find(5, 6),
              "a rectangle paints exactly the tiles it covers");
    });
    key(ImGuiKey_Z, true);
    add(3, [this] { check(!scene_.scene().paint.find(8, 6), "Cmd+Z takes back the rectangle"); });
    click([this] { return scene_.control("shape Line"); });
    drag([this] { return world_point(5.5, 5.5); }, [this] { return world_point(9.5, 5.5); });
    add(3, [this] {
        const auto& paint = scene_.scene().paint;
        const auto* tile = paint.find(7, 6);
        check(tile && (tile->mask & paint_ground) && paint.find(5, 5) && paint.find(10, 5) &&
                  !paint.find(11, 5) && !paint.find(7, 7),
              "a line paints the brush's width along it");
    });
    key(ImGuiKey_Z, true);
    click([this] { return scene_.control("shape Fill"); });
    click([this] { return world_point(6.5, 6.5); });
    add(3, [this] {
        const auto& paint = scene_.scene().paint;
        check(paint.find(6, 6) && paint.find(7, 6) && paint.tiles() > 50, "fill paints the connected area");
    });
    key(ImGuiKey_Z, true);
    add(3, [this] { check(!scene_.scene().paint.find(6, 6), "Cmd+Z takes back the fill"); });
    click([this] { return scene_.control("shape Freehand"); });

    // A shared material made unique from the Inspector changes only the selected entity.
    click([this] { return scene_.control("done painting"); });
    click([this] { return row(index_of("Player"), 0); });
    add(2, [] {});
    click([this] { return scene_.control("make unique"); });
    add(3, [this] {
        const auto& e = scene_.scene().entities[static_cast<std::size_t>(index_of("Player"))];
        check(e.visual && e.visual->material.size() > 2 &&
                  e.visual->material.compare(e.visual->material.size() - 2, 2, "_2") == 0,
              "Make Unique gives the entity its own copy of the material");
    });

    // The Atmosphere row selects the scene's atmosphere; its checkbox turns on day and night.
    click([this] { return scene_.control("atmosphere row"); });
    add(2, [this] {
        check(scene_.atmosphere_selected() && scene_.selection().empty(), "the Atmosphere row selects it");
    });
    click([this] { return scene_.control("day cycle"); });
    add(3, [this] {
        check(scene_.scene().atmosphere.day_length > 0, "the day-and-night checkbox starts a day cycle");
    });
    key(ImGuiKey_Z, true);
    add(3, [this] { check(scene_.scene().atmosphere.day_length == 0, "Cmd+Z takes it back"); });
    // Bulk editing: relative movement preserves spacing, absolute X preserves every Y, and both undo.
    auto originals = std::make_shared<SceneFile>();
    click([this] { return row(0, 0); });
    click([this] { return row(1, 0); }, true);
    add(3, [this, originals] {
        *originals = scene_.scene();
        check(scene_.selection().size() == 2, "multiple selection opens the multi-entity Inspector");
    });
    drag([this] { return scene_.control("multi position x"); }, {30, 0});
    add(3, [this, originals] {
        const auto delta = relative(scene_.scene().entities[0].position, originals->entities[0].position);
        check(std::abs(delta.x) > 0.01F && std::abs(delta.y) < 0.001F,
              "relative Inspector X moves the selection");
        for (std::size_t i = 1; i < 2; ++i) {
            const auto moved = relative(scene_.scene().entities[i].position, originals->entities[i].position);
            check(std::abs(moved.x - delta.x) < 0.001F && std::abs(moved.y) < 0.001F,
                  "relative Inspector movement preserves spacing");
        }
    });
    key(ImGuiKey_Z, true);
    add(3,
        [this, originals] { check(scene_.scene() == *originals, "one undo restores a multi-entity drag"); });
    click([this] { return scene_.control("absolute positions"); });
    click([this] { return scene_.control("set position x"); });
    add(3, [this, originals] {
        const auto& all = scene_.scene().entities;
        for (std::size_t i = 0; i < 2; ++i) {
            check(all[i].position.chunk.x == all[0].position.chunk.x &&
                      all[i].position.local.x == all[0].position.local.x,
                  "Set resolves mixed X even when the displayed value is unchanged");
            check(all[i].position.chunk.y == originals->entities[i].position.chunk.y &&
                      all[i].position.local.y == originals->entities[i].position.local.y,
                  "absolute X preserves each Y coordinate");
        }
    });
    key(ImGuiKey_Z, true);
    add(3, [this, originals] {
        check(scene_.scene() == *originals, "one undo restores an absolute multi-entity edit");
    });
    key(ImGuiKey_Space, true, true);
    add(4, [this] { check(scene_.maximized, "the workspace shortcut maximizes the Scene view"); });
    key(ImGuiKey_Space, true, true);
    add(4, [this] { check(!scene_.maximized, "the workspace shortcut restores the docked Scene view"); });
}

void UiTest::type(const char* text) {
    add(1, [=] { ImGui::GetIO().AddInputCharactersUTF8(text); });
}

ImVec2 UiTest::row(int index, int column) const {
    const auto& rows = scene_.rows();
    if (index < 0 || index >= static_cast<int>(rows.size())) return {-1, -1};
    const auto& r = rows[static_cast<std::size_t>(index)];
    return column == 0 ? r.name : column == 1 ? r.show : r.lock;
}

int UiTest::index_of(const std::string& entity) const {
    const auto& e = scene_.scene().entities;
    for (int i = 0; i < static_cast<int>(e.size()); ++i)
        if (e[static_cast<std::size_t>(i)].name == entity) return i;
    return -1;
}

void UiTest::check(bool condition, const std::string& what) {
    if (condition)
        log_(false, "UI test: " + what + ".");
    else {
        log_(true, "UI test failed: " + what + ".");
        failed_ = true;
    }
}

ImVec2 UiTest::at(const std::string& entity) const {
    for (const auto& e : scene_.scene().entities)
        if (e.name == entity) return scene_.screen_of(e.position);
    return {};
}

ImVec2 UiTest::world_point(double x, double y) const {
    return scene_.screen_of(from_global(x, y));
}

double UiTest::x_of(const std::string& entity) const {
    for (const auto& e : scene_.scene().entities)
        if (e.name == entity) return global_coordinate(e.position.chunk.x, e.position.local.x);
    return 0;
}

void UiTest::click(std::function<ImVec2()> where, bool shift) {
    auto point = std::make_shared<ImVec2>();
    add(1, [=, this] {
        *point = where();
        pointer_ = *point;
    });
    // The target is found again as the button goes down, and the pointer is placed first, so a
    // layout that moved in between (a new window settling) cannot misdirect the press.
    add(1, [=, this] {
        *point = where();
        pointer_ = *point;
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(point->x, point->y);
        if (shift) io.AddKeyEvent(ImGuiMod_Shift, true);
        io.AddMouseButtonEvent(0, true);
    });
    add(1, [=] {
        ImGui::GetIO().AddMouseButtonEvent(0, false);
        if (shift) ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, false);
    });
}

void UiTest::drag(std::function<ImVec2()> from, ImVec2 by, bool shift) {
    drag(
        from,
        [from, by] {
            const auto start = from();
            return ImVec2{start.x + by.x, start.y + by.y};
        },
        shift);
}

void UiTest::drag(std::function<ImVec2()> from, std::function<ImVec2()> to, bool shift) {
    // Down, then several moves over separate frames so it reads as a drag, then up.
    auto start = std::make_shared<ImVec2>(), end = std::make_shared<ImVec2>();
    add(1, [=, this] {
        *start = from();
        *end = to();
        pointer_ = *start;
    });
    add(1, [=, this] {
        *start = from();
        *end = to();
        pointer_ = *start;
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(start->x, start->y);
        if (shift) io.AddKeyEvent(ImGuiMod_Shift, true);
        io.AddMouseButtonEvent(0, true);
    });
    constexpr int moves = 6;
    for (int i = 1; i <= moves; ++i)
        add(1, [=, this] {
            const float t = static_cast<float>(i) / moves;
            pointer_ = {start->x + (end->x - start->x) * t, start->y + (end->y - start->y) * t};
        });
    add(1, [=] {
        ImGui::GetIO().AddMouseButtonEvent(0, false);
        if (shift) ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, false);
    });
}

void UiTest::key(ImGuiKey key, bool command, bool shift) {
    // Shortcuts go to the focused window: hovering the Scene view and pressing no button there
    // focuses nothing, so the key follows a click on the view when it needs one.
    add(1, [=] {
        auto& io = ImGui::GetIO();
        if (command) io.AddKeyEvent(command_key, true);
        if (shift) io.AddKeyEvent(ImGuiMod_Shift, true);
        io.AddKeyEvent(key, true);
    });
    add(1, [=] {
        auto& io = ImGui::GetIO();
        io.AddKeyEvent(key, false);
        if (shift) io.AddKeyEvent(ImGuiMod_Shift, false);
        if (command) io.AddKeyEvent(command_key, false);
    });
}

void UiTest::frame() {
    if (finished()) return;
    // ImGui's key repeat and double-click detection run on time. A stalled frame (macOS checks a
    // freshly built app on first launch) would otherwise repeat a held key, so every test frame
    // counts as exactly 1/60 s and the result depends only on frame counts.
    ImGui::GetIO().DeltaTime = 1.0F / 60;
    if (waited_ < steps_[next_].wait)
        ++waited_;
    else {
        waited_ = 0;
        steps_[next_++].act();
    }
    // Last in the queue each frame, so this is where the pointer is, whatever else happened.
    if (pointer_.x >= 0) ImGui::GetIO().AddMousePosEvent(pointer_.x, pointer_.y);
}
} // namespace seed::editor
