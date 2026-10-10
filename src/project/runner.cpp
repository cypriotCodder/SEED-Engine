#include "project/runner.hpp"
#include "app/app.hpp"
#include "io/storage.hpp"
#include "physics/character.hpp"
#include "project/characters.hpp"
#include "project/game_settings.hpp"
#include "project/scene_file.hpp"
#include "script/host.hpp"
#include "script/saved_data.hpp"
#include "world/player_save.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <utility>

namespace seed {
namespace {
// The player's actions that have keys by default; a project may rebind them in its Input panel.
struct DefaultAction {
    const char* name;
    SDL_Scancode keys[2];
};
constexpr DefaultAction default_actions[] = {{"move_up", {SDL_SCANCODE_W, SDL_SCANCODE_UP}},
                                             {"move_down", {SDL_SCANCODE_S, SDL_SCANCODE_DOWN}},
                                             {"move_left", {SDL_SCANCODE_A, SDL_SCANCODE_LEFT}},
                                             {"move_right", {SDL_SCANCODE_D, SDL_SCANCODE_RIGHT}},
                                             {"run", {SDL_SCANCODE_LSHIFT, SDL_SCANCODE_RSHIFT}}};

struct Runner {
    ProjectFiles files;
    std::string id, name, title, save;
    std::string pack; // The project's texture pack, if it has one.
    GameSettings game;
    Assets data; // For the scene's material names and the default seed.
    SceneFile scene;
    std::size_t player_index{}; // The scene entity that is the player.
    ScenePlayer player_settings;
    std::array<ActionId, 5> moves{}; // up, down, left, right, run
    std::unique_ptr<ScriptHost> scripts;
    Entity player{};
    std::string scene_name, prefix; // The scene playing, and its save files' prefix.
    DayClock clock;                 // The game's time of day.
    double time{};                  // Seconds of play, for flickering lights.
    // From the save's game.state: the scene it was made in, and game.data until scripts exist.
    std::string saved_scene;
    std::optional<std::vector<std::uint8_t>> saved_data;
    std::vector<std::uint8_t> last_data; // game.data as last saved, kept if it cannot save again.
};

void read_scene(Runner& runner, const std::string& name, bool start);

void add_moves(void* context, Actions& actions) {
    auto& runner = *static_cast<Runner*>(context);
    const auto& p = runner.player_settings;
    const std::string* names[] = {&p.up, &p.down, &p.left, &p.right, &p.run};
    for (std::size_t i = 0; i < 5; ++i)
        try {
            runner.moves[i] = actions.find(*names[i]); // The project's own binding wins.
        } catch (const std::out_of_range&) {
            const auto found = std::find_if(std::begin(default_actions), std::end(default_actions),
                                            [&](const DefaultAction& d) { return *names[i] == d.name; });
            if (found == std::end(default_actions))
                throw std::runtime_error("The player uses the action \"" + *names[i] +
                                         "\", which is not in the project's Input panel");
            runner.moves[i] =
                actions.add(found->name, {Binding::key(found->keys[0]), Binding::key(found->keys[1])});
        }
}

// Makes the engine entity for a scene entity: position, look, light, character and name. Scripts are
// attached by the caller, once everything they might look for exists.
Entity instantiate(Runner& runner, Engine& engine, const SceneEntity& e, WorldPosition at) {
    const Transform transform{at, at, e.angle};
    const auto entity =
        e.visual ? engine.scene.create(transform, {engine.materials.find(e.visual->material), e.visual->size})
                 : engine.scene.create(transform);
    if (e.light)
        engine.scene.components<LightComponent>().add(entity, {e.light->color, e.light->radius,
                                                               e.light->intensity, e.light->height,
                                                               e.light->flicker, e.light->night_only});
    if (e.character) engine.scene.components<CharacterMotion>().add(entity, character_motion(*e.character));
    runner.scripts->name(entity, e.name);
    return entity;
}

// Makes the engine entities of the scene that plays, the player at `spawn` when `place` (else at
// its scene position), then loads their scripts.
void spawn_scene(Runner& runner, Engine& engine, WorldPosition spawn, bool place) {
    std::vector<std::pair<Entity, const SceneEntity*>> scripted;
    for (std::size_t i = 0; i < runner.scene.entities.size(); ++i) {
        const auto& e = runner.scene.entities[i];
        const bool player = i == runner.player_index;
        const auto at = player && place ? spawn : e.position;
        const auto entity = instantiate(runner, engine, e, at);
        if (player) runner.player = entity;
        if (!e.script.empty()) scripted.emplace_back(entity, &e);
    }
    engine.camera_smoothing = runner.player_settings.smoothing;
    engine.camera_dead_zone = runner.player_settings.dead_zone;
    engine.camera_zoom = runner.player_settings.zoom;
    runner.scripts->set_player(runner.player);
    engine.renderer.lighting = lighting_at(runner.clock.atmosphere, static_cast<float>(runner.clock.shown()));
    runner.scripts->set_scene(runner.scene_name);
    // Scripts load once every entity exists, so their top-level code can already find the others.
    for (const auto& [e, source] : scripted)
        runner.scripts->attach(e, source->script);
}

// Leaves the scene that plays for another (game.load_scene): its world is saved and the other
// scene's world, entities and scripts take its place. The player comes back where it left that
// scene, or at the entity named `spawn_name` there when one is given.
void change_scene(Runner& runner, Engine& engine, const std::string& name, const std::string& spawn_name) {
    try {
        read_scene(runner, name, false);
    } catch (const std::exception& error) {
        return runner.scripts->report("game.load_scene: " + std::string(error.what()));
    }
    // The start scene keeps the seed the game started with; others use their terrain's.
    const auto seed =
        name == runner.game.start_scene ? engine.options.seed : runner.data.terrain.default_seed;
    engine.change_world(runner.data.terrain, runner.data.paint, seed, runner.prefix);
    // Everything of the scene left behind goes, and its scripts with it.
    const auto owners = engine.scene.transforms.owners();
    const std::vector<Entity> leaving(owners.begin(), owners.end());
    for (const auto e : leaving)
        if (engine.scene.alive(e)) engine.scene.destroy(e);
    // The player starts at the spawn entity asked for, else where it left this scene before (if
    // its settings resume), else at its scene position.
    WorldPosition at{};
    bool place = false;
    if (!spawn_name.empty()) {
        const auto found = std::find_if(runner.scene.entities.begin(), runner.scene.entities.end(),
                                        [&](const SceneEntity& e) { return e.name == spawn_name; });
        if (found == runner.scene.entities.end())
            runner.scripts->report("game.load_scene: the scene \"" + name + "\" has no entity \"" +
                                   spawn_name + "\"");
        else
            at = found->position, place = true;
    }
    const auto saved = engine.checkpoint.read_path(runner.prefix + "player.delta");
    if (!place && runner.player_settings.resume && std::filesystem::exists(saved)) {
        at = load_player(saved.parent_path(), seed, engine.world.generator().version, runner.prefix);
        place = true;
    }
    spawn_scene(runner, engine, at, place);
    engine.focus = runner.player;
    engine.world.settle(engine.scene.transforms.find(runner.player)->position.chunk);
}

// game.state, version 1: the scene playing, the clock, and game.data (see ScriptHost::save_data).
constexpr std::uint32_t state_version = 1;
static_assert(saved_data_capacity + 1024 <= game_state_capacity, "game.state has room for the rest");

void save_state(void* context, Engine&, Bytes& out) {
    auto& runner = *static_cast<Runner*>(context);
    out.u32(state_version);
    out.u8(static_cast<std::uint8_t>(runner.scene_name.size()));
    out.data.insert(out.data.end(), runner.scene_name.begin(), runner.scene_name.end());
    out.u64(std::bit_cast<std::uint64_t>(runner.clock.hour));
    out.u32(std::bit_cast<std::uint32_t>(runner.clock.day_length));
    try {
        runner.last_data = runner.scripts->save_data();
    } catch (const std::exception& error) {
        runner.scripts->report(std::string(error.what()) + "; the save keeps game.data as it was last saved");
    }
    out.data.insert(out.data.end(), runner.last_data.begin(), runner.last_data.end());
}

// Everything is checked before any of it is used; game.data itself is checked as setup loads it.
void load_state(void* context, Engine&, Reader& in) {
    auto& runner = *static_cast<Runner*>(context);
    if (in.u32() != state_version) throw std::runtime_error("game.state is from a newer Seed");
    const auto name = in.take(in.u8());
    std::string scene(name.begin(), name.end());
    if (!valid_prefab_name(scene)) throw std::runtime_error("game.state names an invalid scene");
    const auto hour = std::bit_cast<double>(in.u64());
    const auto day_length = std::bit_cast<float>(in.u32());
    if (!(hour >= 0 && hour < 24) || !(day_length == 0 || (day_length >= 10 && day_length <= 86400)))
        throw std::runtime_error("game.state has an invalid time of day");
    const auto data = in.take(in.remaining());
    runner.saved_scene = std::move(scene);
    runner.clock.hour = hour;
    runner.clock.day_length = day_length;
    runner.saved_data.emplace(data.begin(), data.end());
}

Entity setup(void* context, Engine& engine, WorldPosition spawn) {
    auto& runner = *static_cast<Runner*>(context);
    engine.scene.add_component<LightComponent>();
    engine.scene.add_component<CharacterMotion>();
    runner.scripts = std::make_unique<ScriptHost>(engine, subfolder(runner.files, "scripts/"));
    // Scripts spawn prefabs through the same path as scene entities, lights and scripts included.
    runner.scripts->set_prefab_spawner(
        [&runner, &engine](const std::string& name, WorldPosition at, float angle) {
            if (!valid_prefab_name(name)) throw std::invalid_argument("Invalid prefab name \"" + name + "\"");
            const auto text = runner.files("prefabs/" + name + ".json");
            if (!text) throw std::runtime_error("Unknown prefab \"" + name + "\"");
            auto prefab = parse_prefab(parse_json(*text));
            prefab.angle = angle;
            if (prefab.character) prefab.character->player.reset(); // Spawned characters are NPCs.
            const auto entity = instantiate(runner, engine, prefab, at);
            if (!prefab.script.empty()) runner.scripts->attach(entity, prefab.script);
            return entity;
        });
    runner.scripts->set_clock(&runner.clock);
    // A saved game brings back game.data before any script runs; where to go on from there is
    // the game's choice (game.saved_scene()).
    if (runner.saved_data) runner.scripts->load_data(*std::exchange(runner.saved_data, {}));
    runner.scripts->set_saved_scene(runner.saved_scene);
    runner.last_data = runner.scripts->save_data();
    // A saved game can put the player back where it was instead of at its scene position.
    const bool resumed = std::filesystem::exists(engine.checkpoint.read_path(runner.prefix + "player.delta"));
    spawn_scene(runner, engine, spawn, resumed);
    return runner.player;
}

void step(void* context, Engine& engine, float dt) {
    auto& runner = *static_cast<Runner*>(context);
    // Characters start each step where they are, so their moves interpolate smoothly.
    const auto owners = engine.scene.components<CharacterMotion>().owners();
    for (const auto owner : owners)
        if (auto* t = engine.scene.transforms.find(owner)) t->previous = t->position;
    runner.scripts->update(dt);
    if (runner.player_settings.input && engine.scene.alive(runner.player))
        if (auto* motion = engine.scene.components<CharacterMotion>().find(runner.player)) {
            const auto& a = engine.actions;
            motion->walk = {a.axis(runner.moves[2], runner.moves[3]),
                            a.axis(runner.moves[1], runner.moves[0])};
            motion->running = a.held(runner.moves[4]);
            if (motion->walk.x != 0 || motion->walk.y != 0) motion->has_target = false; // Keys take over.
        }
    // A paused game (a title or pause screen) keeps running its scripts, but nothing else moves.
    if (!runner.scripts->paused()) {
        move_characters(engine, dt);
        animate_characters(engine);
        runner.scripts->surfaces();
        runner.scripts->touches();
        // The day moves on; the frame drawn next uses its light.
        runner.time += dt;
        runner.clock.advance(dt);
    }
    // A scene change asked for in this step happens once the step is over.
    if (auto request = runner.scripts->take_scene_request())
        change_scene(runner, engine, request->first, request->second);
    engine.renderer.lighting = lighting_at(runner.clock.atmosphere, static_cast<float>(runner.clock.shown()));
}

void render(void* context, Engine& engine, const View& view) {
    const auto& runner = *static_cast<const Runner*>(context);
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
                // Ground textures continue across tiles, placed by the tile's world position, and
                // blend into their neighbours'. Neighbours in chunks that are not active count as
                // this tile's own ground.
                const auto ground_at = [&](int dx, int dy) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx >= 0 && ny >= 0 && nx < chunk_side && ny < chunk_side)
                        return chunk.tiles[static_cast<std::size_t>(ny * chunk_side + nx)].material;
                    const auto* n = engine.world.tile(
                        {coord, {static_cast<float>(nx) + 0.5F, static_cast<float>(ny) + 0.5F}});
                    return n ? n->material : tile.material;
                };
                engine.renderer.ground_blended(tile.material,
                                               {ground_at(1, 0), ground_at(-1, 0), ground_at(0, 1),
                                                ground_at(0, -1), ground_at(1, 1), ground_at(-1, 1),
                                                ground_at(1, -1), ground_at(-1, -1)},
                                               px, py, global_coordinate(coord.x, static_cast<float>(x)),
                                               global_coordinate(coord.y, static_cast<float>(y)));
                if (tile.object != no_object)
                    engine.renderer.sprite(static_cast<MaterialId>(tile.object - 1), px, py);
            }
    });
    engine.draw_entities(view);
    engine.particles.draw(engine.renderer, view.camera);
    // Lights near the view, up to the renderer's 32; lights that are out (a lamp by day) are skipped.
    const float day = daylight(static_cast<float>(runner.clock.shown()));
    auto& lights = engine.scene.components<LightComponent>();
    const auto owners = lights.owners();
    const auto values = lights.values();
    for (std::size_t i = 0, drawn = 0; i < owners.size() && drawn < 32; ++i) {
        const auto& t = *engine.scene.transforms.find(owners[i]);
        if (!nearby(t.position.chunk, view.camera.chunk, 8)) continue;
        const auto p = relative(t.previous, view.camera) + relative(t.position, t.previous) * view.alpha;
        const auto& l = values[i];
        if (std::abs(p.x) > half_w + l.radius || std::abs(p.y) > half_h + l.radius) continue;
        const float intensity = light_intensity(l, day, runner.time, owners[i].index);
        if (intensity <= 0) continue;
        engine.renderer.light(p.x, p.y, l.radius, l.color[0], l.color[1], l.color[2], intensity, l.height);
        ++drawn;
    }
    runner.scripts->draw_ui(engine.renderer);
}

// Once a frame, before rendering: where the mouse is, for scripts' next step.
void act(void* context, Engine& engine, const View& view) {
    auto& runner = *static_cast<Runner*>(context);
    runner.scripts->set_pointer(view.pointer, static_cast<float>(engine.input.mouse_x),
                                static_cast<float>(engine.input.mouse_y), view.logical_width,
                                view.logical_height);
}

// Reads scenes/<name>.json, its paint and its terrain, and makes it the scene that plays. Nothing
// changes if it cannot: errors are thrown first. Only the start scene "main" may be missing (an
// empty scene, for a new project).
void read_scene(Runner& runner, const std::string& name, bool start) {
    const auto scene_path = "scenes/" + name + ".json";
    const auto text = runner.files(scene_path);
    if (!text && !(start && name == "main"))
        throw std::runtime_error("The scene " + scene_path + " does not exist");
    SceneFile scene;
    if (text) try {
            scene = parse_scene(parse_json(*text));
        } catch (const std::exception& error) {
            throw std::runtime_error(scene_path + ": " + error.what());
        }
    auto data = runner.data;
    data.terrain = load_terrain(subfolder(runner.files, "assets/"), scene.terrain);
    const auto paint_path = "scenes/" + name + ".paint";
    if (const auto paint = runner.files(paint_path)) try {
            scene.paint = decode_paint(*paint);
        } catch (const std::exception& error) {
            throw std::runtime_error(paint_path + ": " + error.what());
        }
    data.paint = scene.paint;
    if (const auto problems = scene.problems(data); !problems.empty())
        throw std::runtime_error("The scene \"" + name + "\" has problems:\n" + problems);
    // The player is the character marked as the player. Older projects mark it by name: an entity
    // named "Player" (moving itself if it has a script), else a new one at "Spawn" or the origin.
    auto& entities = scene.entities;
    auto found = std::find_if(entities.begin(), entities.end(),
                              [](const SceneEntity& e) { return e.character && e.character->player; });
    if (found == entities.end()) {
        found = std::find_if(entities.begin(), entities.end(),
                             [](const SceneEntity& e) { return e.name == "Player"; });
        if (found == entities.end()) {
            SceneEntity player;
            player.name = "Player";
            const auto spawn = std::find_if(entities.begin(), entities.end(),
                                            [](const SceneEntity& e) { return e.name == "Spawn"; });
            if (spawn != entities.end()) player.position = spawn->position;
            player.visual = SceneVisual{data.materials.front().name, {0.6F, 0.6F}};
            entities.push_back(player);
            found = entities.end() - 1;
        }
        if (!found->character) found->character = SceneCharacter{};
        found->character->player = ScenePlayer{};
        found->character->player->input = found->script.empty();
    }
    runner.player_index = static_cast<std::size_t>(found - entities.begin());
    runner.player_settings = *found->character->player;
    runner.scene = std::move(scene);
    runner.data.terrain = std::move(data.terrain);
    runner.data.paint = std::move(data.paint);
    runner.scene_name = name;
    runner.prefix = name + ".";
    runner.clock.enter(runner.scene.atmosphere);
}

// Reads a project through `files`. `save` is where its games are saved unless --save says otherwise.
void load(Runner& runner, ProjectFiles files, std::filesystem::path save, std::filesystem::path pack) {
    // Textures arrive already compressed: the editor packs them (see editor/textures.hpp).
    if (!pack.empty() && std::filesystem::exists(pack)) runner.pack = pack.string();
    runner.files = std::move(files);
    const auto project = runner.files("project.seed.json");
    if (!project) throw std::runtime_error("Not a Seed project: no project.seed.json");
    const auto json = parse_json(*project);
    if (json.at("seed_project").as_int(1, 1000000) > 1)
        throw std::runtime_error("This project was made by a newer editor");
    runner.name = json.at("name").as_string();
    runner.id = json.at("game_id").as_string();
    runner.game = parse_game_settings(json.find("game"));
    if (const auto problem = runner.game.problems(); !problem.empty()) throw std::runtime_error(problem);
    runner.title = runner.game.title.empty() ? runner.name : runner.game.title;
    runner.save = save.string();
    runner.data = load_assets(subfolder(runner.files, "assets/"));
    if (runner.data.materials.empty()) throw std::runtime_error("The project has no materials yet");
    read_scene(runner, runner.game.start_scene, true);
}

int play(Runner& runner, int argc, char** argv) {
    Game game;
    game.context = &runner;
    game.id = runner.id.c_str();
    game.name = "seed_player";
    game.title = runner.title.c_str();
    game.default_save = runner.save.c_str();
    game.default_seed = runner.data.terrain.default_seed;
    game.window_width = runner.game.width;
    game.window_height = runner.game.height;
    game.fullscreen = runner.game.fullscreen;
    game.assets = &runner.data;
    game.asset_files = subfolder(runner.files, "assets/"); // Recorded sounds and music.
    game.save_prefix = runner.prefix.c_str();              // Each scene keeps its own files in the save.
    // Room for the runner's own components (lights and characters, 8,192 of each) beyond what the
    // engine's scene needs.
    game.scene_memory = Scene::default_memory + 1024 * 1024;
    if (!runner.pack.empty()) game.asset_pack = runner.pack.c_str();
    game.actions = add_moves;
    game.setup = setup;
    game.step = step;
    game.render = render;
    game.act = act;
    game.save_state = save_state;
    game.load_state = load_state;
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
        load(runner, folder_files(root), root / ".seed" / "save", root / ".seed" / "textures.pak");
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
        load(runner, files, saves, archive.parent_path() / "game.pak");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Engine error: %s: %s\n", archive.string().c_str(), error.what());
        return 1;
    }
    return play(runner, argc, argv);
}
} // namespace seed
