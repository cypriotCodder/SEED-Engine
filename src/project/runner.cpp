#include "project/runner.hpp"
#include "app/app.hpp"
#include "io/storage.hpp"
#include "physics/character.hpp"
#include "project/scene_file.hpp"
#include "script/host.hpp"
#include <cmath>
#include <cstdio>
#include <memory>

namespace seed {
namespace {
constexpr float player_speed = 5;       // World units per second.
constexpr Vec2 player_half{0.3F, 0.3F}; // Collision box half size.
constexpr const char* move_actions[] = {"move_up", "move_down", "move_left", "move_right"};

// A light standing on a scene entity; drawn each frame at the entity's position.
struct SceneLightComponent {
    std::array<float, 3> color{};
    float radius{}, intensity{}, height{};
};

struct Runner {
    ProjectFiles files;
    std::string id, name, title, save;
    Assets data; // For the scene's material names and the default seed.
    SceneFile scene;
    std::array<ActionId, 4> moves{};
    std::unique_ptr<ScriptHost> scripts;
    Entity player{};
    bool scripted_player{}; // A player with its own script moves itself.
};

void add_moves(void* context, Actions& actions) {
    auto& runner = *static_cast<Runner*>(context);
    const SDL_Scancode keys[4][2] = {{SDL_SCANCODE_W, SDL_SCANCODE_UP},
                                     {SDL_SCANCODE_S, SDL_SCANCODE_DOWN},
                                     {SDL_SCANCODE_A, SDL_SCANCODE_LEFT},
                                     {SDL_SCANCODE_D, SDL_SCANCODE_RIGHT}};
    for (std::size_t i = 0; i < 4; ++i)
        try {
            runner.moves[i] = actions.find(move_actions[i]); // The project's own binding wins.
        } catch (const std::out_of_range&) {
            runner.moves[i] =
                actions.add(move_actions[i], {Binding::key(keys[i][0]), Binding::key(keys[i][1])});
        }
}

Entity setup(void* context, Engine& engine, WorldPosition spawn) {
    auto& runner = *static_cast<Runner*>(context);
    auto& lights = engine.scene.add_component<SceneLightComponent>();
    runner.scripts = std::make_unique<ScriptHost>(engine, subfolder(runner.files, "scripts/"));
    std::vector<std::pair<Entity, const SceneEntity*>> scripted;
    const SceneEntity* player = nullptr;
    const SceneEntity* marker = nullptr;
    for (const auto& e : runner.scene.entities) {
        if (e.name == "Player" && !player) {
            player = &e;
            continue;
        }
        if (e.name == "Spawn" && !marker) marker = &e;
        const Transform transform{e.position, e.position, e.angle};
        const auto entity =
            e.visual
                ? engine.scene.create(transform, {engine.materials.find(e.visual->material), e.visual->size})
                : engine.scene.create(transform);
        if (e.light)
            lights.add(entity, {e.light->color, e.light->radius, e.light->intensity, e.light->height});
        runner.scripts->name(entity, e.name);
        if (!e.script.empty()) scripted.emplace_back(entity, &e);
    }
    // A saved game resumes where the player was; a new one starts at the scene's player or spawn.
    const bool resumed = std::filesystem::exists(engine.checkpoint.read_path("player.delta"));
    auto at = resumed ? spawn : player ? player->position : marker ? marker->position : spawn;
    const Visual look = player && player->visual
                            ? Visual{engine.materials.find(player->visual->material), player->visual->size}
                            : Visual{0, {0.6F, 0.6F}};
    const auto entity = engine.scene.create({at, at, player ? player->angle : 0}, look);
    if (player && player->light)
        lights.add(entity, {player->light->color, player->light->radius, player->light->intensity,
                            player->light->height});
    runner.scripts->name(entity, "Player");
    runner.player = entity;
    runner.scripted_player = player && !player->script.empty();
    if (runner.scripted_player) scripted.emplace_back(entity, player);
    // Scripts load once every entity exists, so their top-level code can already find the others.
    for (const auto& [e, source] : scripted)
        runner.scripts->attach(e, source->script);
    return entity;
}

void step(void* context, Engine& engine, float dt) {
    auto& runner = *static_cast<Runner*>(context);
    runner.scripts->update(dt);
    if (runner.scripted_player || !engine.scene.alive(runner.player)) return;
    Vec2 direction{engine.actions.axis(runner.moves[2], runner.moves[3]),
                   engine.actions.axis(runner.moves[1], runner.moves[0])};
    if (direction.x != 0 && direction.y != 0) direction = direction * 0.70710678F;
    auto& transform = *engine.scene.transforms.find(runner.player);
    transform.previous = transform.position;
    transform.position =
        move_character(engine.world, engine.physics, transform.position, direction * (player_speed * dt),
                       player_half, {nullptr, blocks_walking});
}

void render(void*, Engine& engine, const View& view) {
    const float half_w = static_cast<float>(view.width) / view.zoom / 2 + 1;
    const float half_h = static_cast<float>(view.height) / view.zoom / 2 + 1;
    engine.world.each([&](ChunkCoord coord, const Chunk& chunk) {
        const auto offset = relative({coord, {}}, view.camera);
        if (offset.x > half_w || offset.y > half_h || offset.x + chunk_side < -half_w ||
            offset.y + chunk_side < -half_h)
            return;
        for (int y = 0; y < chunk_side; ++y)
            for (int x = 0; x < chunk_side; ++x) {
                const auto& tile = chunk.tiles[static_cast<std::size_t>(y * chunk_side + x)];
                const float px = offset.x + static_cast<float>(x) + 0.5F,
                            py = offset.y + static_cast<float>(y) + 0.5F;
                if (std::abs(px) > half_w || std::abs(py) > half_h) continue;
                engine.renderer.sprite(tile.material, px, py);
                if (tile.object != no_object)
                    engine.renderer.sprite(static_cast<MaterialId>(tile.object - 1), px, py);
            }
    });
    engine.draw_entities(view);
    engine.particles.draw(engine.renderer, view.camera);
    // Lights near the view, up to the renderer's 32.
    auto& lights = engine.scene.components<SceneLightComponent>();
    const auto owners = lights.owners();
    const auto values = lights.values();
    for (std::size_t i = 0, drawn = 0; i < owners.size() && drawn < 32; ++i) {
        const auto& t = *engine.scene.transforms.find(owners[i]);
        if (!nearby(t.position.chunk, view.camera.chunk, 8)) continue;
        const auto p = relative(t.previous, view.camera) + relative(t.position, t.previous) * view.alpha;
        const auto& l = values[i];
        if (std::abs(p.x) > half_w + l.radius || std::abs(p.y) > half_h + l.radius) continue;
        engine.renderer.light(p.x, p.y, l.radius, l.color[0], l.color[1], l.color[2], l.intensity, l.height);
        ++drawn;
    }
}

// Reads a project through `files`. `save` is where its games are saved unless --save says otherwise.
void load(Runner& runner, ProjectFiles files, std::filesystem::path save) {
    runner.files = std::move(files);
    const auto project = runner.files("project.seed.json");
    if (!project) throw std::runtime_error("Not a Seed project: no project.seed.json");
    const auto json = parse_json(*project);
    if (json.at("seed_project").as_int(1, 1000000) > 1)
        throw std::runtime_error("This project was made by a newer editor");
    runner.name = json.at("name").as_string();
    runner.id = json.at("game_id").as_string();
    runner.title = runner.name;
    runner.save = save.string();
    runner.data = load_assets(subfolder(runner.files, "assets/"));
    if (const auto scene = runner.files("scenes/main.json")) try {
            runner.scene = parse_scene(parse_json(*scene));
        } catch (const std::exception& error) {
            throw std::runtime_error(std::string("scenes/main.json: ") + error.what());
        }
    if (const auto problems = runner.scene.problems(runner.data); !problems.empty())
        throw std::runtime_error("The main scene has problems:\n" + problems);
    if (runner.data.materials.empty()) throw std::runtime_error("The project has no materials yet");
}

int play(Runner& runner, int argc, char** argv) {
    Game game;
    game.context = &runner;
    game.id = runner.id.c_str();
    game.name = "seed_player";
    game.title = runner.title.c_str();
    game.default_save = runner.save.c_str();
    game.default_seed = runner.data.terrain.default_seed;
    game.assets = &runner.data;
    game.actions = add_moves;
    game.setup = setup;
    game.step = step;
    game.render = render;
    // Automated runs (tests, the editor's checks) fail on any script error; players just see it
    // reported and the game carries on.
    game.shutdown = [](void* context, Engine& engine) {
        auto& runner = *static_cast<Runner*>(context);
        const auto errors = runner.scripts->errors();
        runner.scripts.reset();
        if (errors && engine.options.smoke)
            throw std::runtime_error(std::to_string(errors) + " script error(s); see above");
    };
    return run(game, argc, argv);
}
} // namespace

int run_project(const std::filesystem::path& project, int argc, char** argv) {
    Runner runner;
    try {
        const auto root = std::filesystem::weakly_canonical(std::filesystem::absolute(project));
        if (!std::filesystem::is_directory(root))
            throw std::runtime_error("Not a Seed project: " + root.string());
        load(runner, folder_files(root), root / ".seed" / "save");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Engine error: %s\n", error.what());
        return 1;
    }
    return play(runner, argc, argv);
}

int run_archive(const std::filesystem::path& archive, int argc, char** argv) {
    Runner runner;
    try {
        const auto files = ProjectArchive::read(archive).reader();
        // An exported game saves in the player's Application Support folder, under its game ID.
        const auto project = files("project.seed.json");
        if (!project) throw std::runtime_error("The game archive has no project.seed.json");
        const auto id = parse_json(*project).at("game_id").as_string();
        if (!valid_project_path(id) || id.find('/') != std::string::npos)
            throw std::runtime_error("Invalid game ID");
        char* base = SDL_GetPrefPath("Seed", id.c_str());
        if (!base) throw std::runtime_error(std::string("Cannot find a folder for saves: ") + SDL_GetError());
        const std::filesystem::path saves = std::filesystem::path(base) / "save";
        SDL_free(base);
        load(runner, files, saves);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Engine error: %s: %s\n", archive.string().c_str(), error.what());
        return 1;
    }
    return play(runner, argc, argv);
}
} // namespace seed
