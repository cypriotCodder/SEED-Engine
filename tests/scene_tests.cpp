#include "project/scene_file.hpp"
#include <cmath>
#include <filesystem>
#include <iostream>

namespace {
namespace fs = std::filesystem;
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F>
void rejects(F&& f, const char* message) {
    try {
        f();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

seed::Assets assets() {
    seed::Assets result;
    seed::MaterialAsset grass;
    grass.name = "grass";
    result.materials = {grass};
    return result;
}

seed::SceneFile sample() {
    seed::SceneFile scene;
    seed::SceneEntity ground;
    ground.name = "Ground";
    ground.position = {{-3, 1000000}, {31.5F, 0.25F}};
    ground.angle = 0.5F;
    ground.visual = seed::SceneVisual{"grass", {4, 2}};
    seed::SceneEntity lamp;
    lamp.name = "Lamp";
    lamp.light = seed::SceneLight{};
    seed::SceneEntity spawn;
    spawn.name = "Spawn";
    seed::SceneEntity cave;
    cave.name = "Cave mouth";
    cave.area = seed::SceneArea{{3, 1.5F}};
    seed::SceneEntity patrol;
    patrol.name = "Patrol";
    patrol.path = seed::ScenePath{{{0, 0}, {4, -2.5F}, {4, 3}}, true};
    scene.entities = {ground, lamp, spawn, cave, patrol};
    scene.decorations = {{"grass", {{-2, 5}, {3.5F, 30.25F}}, {0.5F, 0.4F}, 1.25F, true},
                         {"grass", {{0, 0}, {1, 2}}, {1, 1}, 0, false}};
    return scene;
}

void round_trip(const fs::path& root) {
    const auto scene = sample();
    check(scene.problems(assets()).empty(), "Sample scene is valid");
    seed::save_scene(root / "scenes" / "main.json", scene);
    const auto loaded = seed::load_scene(root / "scenes" / "main.json");
    check(loaded == scene, "Scene reads back unchanged, including far chunk coordinates");
    check(!loaded.entities[2].visual && !loaded.entities[2].light && !loaded.entities[2].area,
          "Component-free entities stay empty");
    check(loaded.entities[3].area && loaded.entities[3].area->size.x == 3, "Areas read back");
    check(loaded.entities[4].path && loaded.entities[4].path->points.size() == 3 &&
              loaded.entities[4].path->points[1].y == -2.5F && loaded.entities[4].path->loop,
          "Paths read back");
    check(loaded.decorations == scene.decorations, "Decorations read back");
    const auto text = seed::read_text(root / "scenes" / "main.json");
    check(text.find("\"standing\": true") != std::string::npos, "Standing decorations are marked");
    check(seed::read_text(root / "scenes" / "main.json").find("atmosphere") == std::string::npos,
          "A default atmosphere is left out of the file");
    auto lit = scene;
    lit.atmosphere.hour = 21.5F;
    lit.atmosphere.day_length = 90;
    lit.atmosphere.night = {0.1F, 0.1F, 0.3F};
    lit.entities[1].light->flicker = 0.25F;
    lit.entities[1].light->night_only = true;
    seed::save_scene(root / "scenes" / "lit.json", lit);
    check(seed::load_scene(root / "scenes" / "lit.json") == lit, "Atmosphere and light settings read back");
    lit.atmosphere.day_length = 5;
    lit.entities[1].light->flicker = 2;
    const auto lit_problems = lit.problems(assets());
    check(lit_problems.find("A day lasts") != std::string::npos &&
              lit_problems.find("Light values out of range") != std::string::npos,
          "Atmosphere and flicker limits are checked");
    const auto paint_file = root / "scenes" / "main.paint";
    check(!fs::exists(paint_file), "A scene without paint has no paint file");

    // Painted terrain is kept beside the scene, and removed with the last painted tile.
    auto painted = scene;
    const auto name = assets().materials.front().name;
    painted.paint.set(-40, 7, {seed::paint_ground, painted.paint.material(name), 0, false, 0});
    check(painted.problems(assets()).empty(), "Painting a known material is valid");
    seed::save_scene(root / "scenes" / "main.json", painted);
    check(fs::exists(paint_file) && seed::load_scene(root / "scenes" / "main.json") == painted,
          "Paint reads back with its scene");
    seed::save_scene(root / "scenes" / "main.json", scene);
    check(!fs::exists(paint_file), "Clearing the paint removes its file");
    painted.paint.set(1, 1, {seed::paint_ground, painted.paint.material("lava"), 0, false, 0});
    check(painted.problems(assets()).find("\"lava\"") != std::string::npos,
          "Unknown painted materials reported");
}

// Editor-only flags round trip, and are left out of the file when unset.
void editor_flags() {
    seed::SceneEntity plain;
    plain.name = "Plain";
    check(seed::to_json(seed::entity_json(plain)).find("editor") == std::string::npos,
          "No editor block when unset");
    auto flagged = plain;
    flagged.hidden = true;
    flagged.locked = true;
    const auto back = seed::parse_entity(seed::entity_json(flagged));
    check(back.hidden && back.locked && back == flagged, "Hidden and locked round trip");
}

// Prefab files hold one entity at the origin; placing one keeps the copy's name and placement.
void prefabs() {
    for (const char* good : {"torch", "Tree_2", "big-rock"})
        check(seed::valid_prefab_name(good), good);
    for (const char* bad : {"", "has space", "a/b", "dot.json", "\u00e9"})
        check(!seed::valid_prefab_name(bad), bad);

    seed::SceneEntity torch;
    torch.name = "torch";
    torch.position = {{3, 4}, {5, 6}};
    torch.angle = 1;
    torch.prefab = "ignored";
    torch.visual = seed::SceneVisual{"grass", {0.5F, 1}};
    torch.light = seed::SceneLight{};
    torch.script = "flicker.lua";
    torch.area = seed::SceneArea{{2, 2}};
    const auto stored = seed::parse_prefab(seed::parse_json(seed::to_json(seed::prefab_json(torch))));
    check(stored.position == seed::WorldPosition{} && stored.angle == 0 && stored.prefab.empty() &&
              stored.visual == torch.visual && stored.light == torch.light && stored.script == torch.script,
          "A prefab stores the components at the origin, without a link");

    seed::SceneEntity placed;
    placed.name = "Torch by the door";
    placed.position = {{0, 0}, {7, 8}};
    placed.angle = 0.5F;
    placed.prefab = "torch";
    seed::apply_prefab(stored, placed);
    check(placed.name == "Torch by the door" && placed.position.local.x == 7 && placed.angle == 0.5F &&
              placed.visual == torch.visual && placed.area == torch.area && placed.script == "flicker.lua" &&
              placed.prefab == "torch",
          "Applying a prefab keeps the copy's name, placement and link");
    check(seed::parse_entity(seed::entity_json(placed)).prefab == "torch",
          "The link is saved with the scene");
}

void canonical() {
    // Offsets outside [0, 32) move into the neighbouring chunk.
    const auto scene = seed::parse_scene(seed::parse_json(
        R"({"format": 1, "entities": [{"name": "A", "chunk": [0, 0], "position": [-1, 40], "angle": 0}]})"));
    const auto& p = scene.entities[0].position;
    check(p.chunk == seed::ChunkCoord{-1, 1} && p.local.x == 31 && p.local.y == 8, "Positions canonicalized");
}

void problems() {
    auto scene = sample();
    scene.entities[0].visual->material = "stone";
    scene.entities[1].light->radius = 0;
    scene.entities[2].name.clear();
    scene.entities[3].area->size.y = 0;
    scene.entities[4].path->points.resize(1);
    scene.decorations[0].material = "lava";
    scene.decorations[1].size.x = 0;
    const auto report = scene.problems(assets());
    check(report.find("Unknown material \"stone\"") != std::string::npos, "Unknown material reported");
    check(report.find("Light values out of range") != std::string::npos, "Bad light reported");
    check(report.find("Names need") != std::string::npos, "Empty name reported");
    check(report.find("Area size") != std::string::npos, "Bad area reported");
    check(report.find("A path has 2 to 256 points") != std::string::npos, "Short path reported");
    check(report.find("Decoration 1: unknown material \"lava\"") != std::string::npos &&
              report.find("Decoration 2: size") != std::string::npos,
          "Bad decorations reported");
    check(std::count(report.begin(), report.end(), '\n') == 7, "One line per problem");
}

void malformed() {
    const char* bad[] = {
        R"({"format": 2, "entities": []})",
        R"({"format": 1})",
        R"({"format": 1, "entities": [{"name": "A", "chunk": [0], "position": [0, 0]}]})",
        R"({"format": 1, "entities": [{"name": "A", "chunk": [0.5, 0], "position": [0, 0]}]})",
        R"({"format": 1, "entities": [{"name": "A", "chunk": [0, 0], "position": [0, 0], "visual": {"size": [1, 1]}}]})",
        R"({"format": 1, "entities": [{"name": "A", "chunk": [0, 0], "position": [1e30, 0]}]})",
    };
    for (const char* text : bad)
        rejects([&] { seed::parse_scene(seed::parse_json(text)); }, text);
}
} // namespace

// Daylight and the lighting it gives; full daylight is exactly the scene's own colours.
void atmosphere() {
    check(seed::daylight(12) == 1 && seed::daylight(9) == 1 && seed::daylight(0) == 0 &&
              seed::daylight(21) == 0,
          "Day and night hours");
    check(std::abs(seed::daylight(6) - 0.5F) < 1e-6F && std::abs(seed::daylight(18) - 0.5F) < 1e-6F,
          "Dawn and dusk are halfway");
    check(seed::daylight(36) == 1 && seed::daylight(-24) == 0 && seed::daylight(-12) == 1,
          "Hours wrap around the day");
    const seed::SceneAtmosphere a;
    const auto noon = seed::lighting_at(a, 12);
    const seed::Lighting renderer_defaults;
    check(noon.ambient == renderer_defaults.ambient && noon.clear == renderer_defaults.clear &&
              noon.haze == renderer_defaults.haze && noon.haze_amount == renderer_defaults.haze_amount,
          "A default atmosphere at noon is the renderer's default lighting");
    check(seed::lighting_at(a, 0).ambient == a.night, "Midnight is the night colour");
    seed::LightComponent lamp{{1, 1, 1}, 5, 2, 2, 0, true};
    check(seed::light_intensity(lamp, 1, 0) == 0 && seed::light_intensity(lamp, 0, 0) == 2,
          "Lamps light at night");
    lamp.flicker = 1;
    float low = 9, high = 0;
    for (int i = 0; i < 600; ++i) {
        const float v = seed::light_intensity(lamp, 0, i / 60.0);
        low = std::min(low, v), high = std::max(high, v);
    }
    check(low >= 2 * 0.4F - 1e-4F && high <= 2 && high - low > 0.5F, "Flicker wavers within its range");
    seed::DayClock clock;
    seed::SceneAtmosphere title, field, cave;
    title.hour = 18;                         // No day: the clock stands still.
    field.hour = 23, field.day_length = 240; // Ten seconds an hour.
    cave.hour = 2;
    clock.enter(title);
    clock.advance(100);
    check(clock.hour == 18 && clock.shown() == 18, "Before any day, a scene keeps its own hour");
    clock.enter(field);
    check(clock.hour == 23 && clock.day_length == 240, "The first scene with a day starts the clock");
    clock.advance(20);
    check(std::abs(clock.hour - 1) < 1e-9, "The clock wraps past midnight");
    clock.enter(cave);
    clock.advance(10);
    check(std::abs(clock.hour - 2) < 1e-9 && clock.shown() == 2,
          "The clock runs on in a scene without a day");
    clock.set_hour(5);
    check(clock.shown() == 2, "A scene without a day keeps its own light");
    field.hour = 12, field.day_length = 480;
    clock.enter(field);
    check(clock.hour == 5 && clock.shown() == 5 && clock.day_length == 480,
          "A later scene with a day keeps the hour and sets the pace");
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::invalid_argument("Usage: seed_scene_tests SCRATCH_DIRECTORY");
        fs::remove_all(argv[1]);
        round_trip(argv[1]);
        canonical();
        editor_flags();
        prefabs();
        problems();
        malformed();
        atmosphere();
        std::cout << "Scene file checks passed.\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
