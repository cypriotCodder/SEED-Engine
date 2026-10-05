#include "console.hpp"
#include "history.hpp"
#include "selection_edit.hpp"
#include <iostream>
#include <stdexcept>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
} // namespace
int main() {
    try {
        using namespace seed;
        using namespace seed::editor;
        std::vector<LogLine> lines{{LogLevel::error, "12:00", "Missing tree"},
                                   {LogLevel::info, "12:01", "Loaded scene"},
                                   {LogLevel::error, "12:02", "Missing tree"},
                                   {LogLevel::warning, "12:03", "Missing tree"}};
        auto rows = console_rows(lines, {true, true, true}, "TREE", true);
        check(rows.size() == 2 && rows[0].count == 2 && rows[1].count == 1,
              "Search ignores case; grouping retains separate severities");
        rows = console_rows(lines, {false, false, true}, "", false);
        check(rows.size() == 2 && rows[0].index == 0 && rows[1].index == 2,
              "Ungrouped errors preserve chronological order");
        check(console_rows(lines, {true, true, true}, "absent", true).empty(), "No matches");
        check(console_text(lines[0], 2) == "12:00 [Error] Missing tree (x2)",
              "Copy includes severity and repeat count");

        SceneFile scene;
        scene.entities.resize(3);
        scene.entities[0].position = from_global(-2, 4);
        scene.entities[1].position = from_global(5, -7);
        scene.entities[2].position = from_global(9, 12);
        scene.entities[0].visual = SceneVisual{"sand", {1, 2}};
        scene.entities[1].visual = SceneVisual{"stone", {3, 4}};
        scene.entities[0].character.emplace();
        scene.entities[1].character.emplace();
        scene.entities[0].character->player.emplace();
        scene.entities[1].character->collision = {2, 3};
        std::vector<SceneEntity*> selected;
        for (auto& entity : scene.entities)
            selected.push_back(&entity);
        const auto width = [](SceneEntity& e) {
            return e.visual ? &e.visual->size.x : nullptr;
        };
        check(selection_mixed(selected, width), "Different widths are mixed despite a missing component");
        History<SceneFile> history;
        history.reset(scene);
        set_selection_field(selected, width, 1.0F);
        check(!selection_mixed(selected, width), "Setting the primary value also resolves a mixed selection");
        check(scene.entities[1].visual->size == Vec2{1, 4} && scene.entities[1].visual->material == "stone" &&
                  !scene.entities[2].visual,
              "Setting width preserves other axes, materials and missing components");
        history.offer(scene);
        check(history.undo().entities[1].visual->size.x == 3, "Bulk field edit is one undo step");
        const auto speed = [](SceneEntity& e) {
            return e.character ? &e.character->speed : nullptr;
        };
        set_selection_field(selected, speed, 8.0F);
        check(scene.entities[0].character->player && !scene.entities[1].character->player &&
                  scene.entities[1].character->collision == Vec2{2, 3},
              "Character edit preserves roles and other settings");
        set_selection_position(selected, 0, 1, false);
        check(scene.entities[0].position == from_global(1, 4) &&
                  scene.entities[1].position == from_global(8, -7),
              "Relative movement preserves spacing and the unedited axis");
        set_selection_position(selected, 0, -35, true);
        check(scene.entities[0].position == from_global(-35, 4) &&
                  scene.entities[1].position == from_global(-35, -7),
              "Absolute movement crosses chunk boundaries and preserves the unedited axis");
        set_selection_position(selected, 0, 1e9, true);
        check(global_coordinate(scene.entities[0].position.chunk.x, scene.entities[0].position.local.x) ==
                  1e9,
              "Absolute coordinates need not be near the previous position");
        const auto before_invalid = scene;
        bool rejected = false;
        try {
            set_selection_position(selected, 0, 1e30, true);
        } catch (const std::out_of_range&) {
            rejected = true;
        }
        check(rejected && scene == before_invalid,
              "Invalid absolute coordinates leave all entities untouched");
        std::cout << "Console and selection checks passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
