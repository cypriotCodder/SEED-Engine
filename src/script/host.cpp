#include "script/host.hpp"
#include "app/app.hpp"
#include "io/storage.hpp"
#include "physics/character.hpp"
// Lua is compiled as C++ (see CMakeLists.txt), so its errors are C++ exceptions and unwind engine
// code correctly. Its headers are therefore included without extern "C".
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#include <cstdio>
#include <cstdlib>
#include <new>

namespace seed {
bool blocks_walking(void*, const Tile* tile) {
    return !tile || (tile->flags & tile_solid) || tile->elevation < 0;
}

// The Lua-facing half of ScriptHost: every function Lua calls, with access to its internals.
struct ScriptApi {
    static constexpr const char* entity_type = "seed.entity";

    static ScriptHost& host(lua_State* lua) { return **static_cast<ScriptHost**>(lua_getextraspace(lua)); }
    static Engine& engine(lua_State* lua) { return host(lua).engine_; }

    // Engine errors (unknown action, out-of-range position, ...) become Lua errors at the call.
    template<int (*F)(lua_State*)>
    static int guarded(lua_State* lua) {
        try {
            return F(lua);
        } catch (const std::exception& error) {
            return luaL_error(lua, "%s", error.what());
        }
    }

    static void* allocate(void* context, void* block, std::size_t old_size, std::size_t new_size) {
        auto& self = *static_cast<ScriptHost*>(context);
        if (!block) old_size = 0; // Lua passes a type tag in old_size for new blocks.
        if (new_size == 0) {
            std::free(block);
            self.memory_ -= old_size;
            return nullptr;
        }
        if (new_size > old_size && self.memory_ + (new_size - old_size) > ScriptHost::memory_limit)
            return nullptr; // Lua reports "not enough memory" to the script.
        void* moved = std::realloc(block, new_size);
        if (moved) self.memory_ = self.memory_ - old_size + new_size;
        return moved;
    }

    // Stops a call that has run too long, such as an endless loop.
    static void watchdog(lua_State* lua, lua_Debug*) {
        const auto& self = host(lua);
        const std::chrono::duration<double> spent = std::chrono::steady_clock::now() - self.call_start_;
        if (spent.count() > ScriptHost::call_budget_seconds) {
            // Lua's error formatting has no precision flags, so the budget is formatted here.
            char message[96];
            std::snprintf(message, sizeof(message),
                          "the script ran for over %.2f s in one call; is there an endless loop?",
                          ScriptHost::call_budget_seconds);
            luaL_error(lua, "%s", message);
        }
    }

    static int traceback(lua_State* lua) {
        const char* message = lua_tostring(lua, 1);
        luaL_traceback(lua, lua, message ? message : "(error value is not a string)", 1);
        return 1;
    }

    static void push_entity(lua_State* lua, Entity entity) {
        new (lua_newuserdatauv(lua, sizeof(Entity), 0)) Entity(entity);
        luaL_setmetatable(lua, entity_type);
    }
    static Entity check_entity(lua_State* lua, int index) {
        const auto entity = *static_cast<Entity*>(luaL_checkudata(lua, index, entity_type));
        if (!engine(lua).scene.alive(entity)) luaL_error(lua, "that entity no longer exists");
        return entity;
    }
    static Transform& transform(lua_State* lua, Entity entity) {
        auto* t = engine(lua).scene.transforms.find(entity);
        if (!t) luaL_error(lua, "that entity has no position");
        return *t;
    }
    static WorldPosition position_at(lua_State* lua, int x) {
        return from_global(luaL_checknumber(lua, x), luaL_checknumber(lua, x + 1));
    }
    static void push_position(lua_State* lua, WorldPosition p) {
        lua_pushnumber(lua, global_coordinate(p.chunk.x, p.local.x));
        lua_pushnumber(lua, global_coordinate(p.chunk.y, p.local.y));
    }
    static ActionId action(lua_State* lua, int index) {
        return engine(lua).actions.find(luaL_checkstring(lua, index));
    }

    // input
    static int held(lua_State* lua) {
        lua_pushboolean(lua, engine(lua).actions.held(action(lua, 1)));
        return 1;
    }
    static int pressed(lua_State* lua) {
        lua_pushboolean(lua, engine(lua).actions.pressed(action(lua, 1)));
        return 1;
    }
    static int released(lua_State* lua) {
        lua_pushboolean(lua, engine(lua).actions.released(action(lua, 1)));
        return 1;
    }
    static int axis(lua_State* lua) {
        lua_pushnumber(lua, engine(lua).actions.axis(action(lua, 1), action(lua, 2)));
        return 1;
    }

    // world
    static int find(lua_State* lua) {
        const std::string_view name = luaL_checkstring(lua, 1);
        auto& self = host(lua);
        for (const auto owner : self.engine_.scene.transforms.owners())
            if (owner.index < self.names_.size() && self.names_[owner.index] == name) {
                push_entity(lua, owner);
                return 1;
            }
        lua_pushnil(lua);
        return 1;
    }
    static int spawn(lua_State* lua) {
        luaL_checktype(lua, 1, LUA_TTABLE);
        auto& e = engine(lua);
        const auto number = [&](const char* key, double fallback) {
            lua_getfield(lua, 1, key);
            const double value = lua_isnil(lua, -1) ? fallback : luaL_checknumber(lua, -1);
            lua_pop(lua, 1);
            return value;
        };
        const auto text = [&](const char* key) {
            lua_getfield(lua, 1, key);
            std::string value = lua_isnil(lua, -1) ? std::string() : std::string(luaL_checkstring(lua, -1));
            lua_pop(lua, 1);
            return value;
        };
        lua_getfield(lua, 1, "x");
        lua_getfield(lua, 1, "y");
        const auto at = position_at(lua, -2);
        lua_pop(lua, 2);
        const Transform t{at, at, static_cast<float>(number("angle", 0))};
        const auto material = text("material");
        Vec2 size{1, 1};
        lua_getfield(lua, 1, "size");
        if (lua_istable(lua, -1)) {
            lua_rawgeti(lua, -1, 1);
            lua_rawgeti(lua, -2, 2);
            size = {static_cast<float>(luaL_checknumber(lua, -2)),
                    static_cast<float>(luaL_checknumber(lua, -1))};
            lua_pop(lua, 2);
        } else if (!lua_isnil(lua, -1)) {
            const auto side = static_cast<float>(luaL_checknumber(lua, -1));
            size = {side, side};
        }
        lua_pop(lua, 1);
        if (!(size.x > 0 && size.y > 0 && size.x <= 64 && size.y <= 64))
            luaL_error(lua, "size must be above 0 and at most 64");
        const auto entity =
            material.empty() ? e.scene.create(t) : e.scene.create(t, {e.materials.find(material), size});
        auto& self = host(lua);
        const auto name = text("name");
        self.name(entity, name.empty() ? "spawned" : name);
        if (const auto script = text("script"); !script.empty()) self.attach(entity, script);
        push_entity(lua, entity);
        return 1;
    }
    static int tile(lua_State* lua) {
        auto& e = engine(lua);
        const auto* t = e.world.tile(position_at(lua, 1));
        if (!t) {
            lua_pushnil(lua); // Not loaded: too far from the camera.
            return 1;
        }
        lua_createtable(lua, 0, 4);
        lua_pushstring(lua, e.materials[t->material].name);
        lua_setfield(lua, -2, "material");
        if (t->object != no_object) {
            lua_pushstring(lua, e.materials[static_cast<MaterialId>(t->object - 1)].name);
            lua_setfield(lua, -2, "object");
        }
        lua_pushboolean(lua, (t->flags & tile_solid) != 0);
        lua_setfield(lua, -2, "solid");
        lua_pushnumber(lua, t->elevation);
        lua_setfield(lua, -2, "elevation");
        return 1;
    }

    // sound, particles, camera, game
    static int play(lua_State* lua) {
        auto& e = engine(lua);
        e.audio.play(e.sounds.find(luaL_checkstring(lua, 1)));
        return 0;
    }
    static int burst(lua_State* lua) {
        auto& e = engine(lua);
        e.particles.burst(position_at(lua, 2), e.particles.find_style(luaL_checkstring(lua, 1)));
        return 0;
    }
    static int follow(lua_State* lua) {
        engine(lua).focus = check_entity(lua, 1);
        return 0;
    }
    static int time(lua_State* lua) {
        lua_pushnumber(lua, host(lua).time_);
        return 1;
    }

    // Entity methods.
    static int position(lua_State* lua) {
        push_position(lua, transform(lua, check_entity(lua, 1)).position);
        return 2;
    }
    static int set_position(lua_State* lua) {
        auto& t = transform(lua, check_entity(lua, 1));
        t.position = t.previous = position_at(lua, 2); // A jump, not a slide: no interpolation.
        return 0;
    }
    static int move(lua_State* lua) {
        auto& e = engine(lua);
        const auto entity = check_entity(lua, 1);
        auto& t = transform(lua, entity);
        const Vec2 delta{static_cast<float>(luaL_checknumber(lua, 2)),
                         static_cast<float>(luaL_checknumber(lua, 3))};
        const auto* visual = e.scene.visuals.find(entity);
        const Vec2 half = visual ? visual->size * 0.45F : Vec2{0.3F, 0.3F};
        const auto target = [&] {
            auto p = t.position;
            p.move(delta);
            return p;
        }();
        t.position = move_character(e.world, e.physics, t.position, delta, half, {nullptr, blocks_walking});
        lua_pushboolean(lua, t.position == target); // False when something blocked part of the move.
        return 1;
    }
    static int angle(lua_State* lua) {
        lua_pushnumber(lua, transform(lua, check_entity(lua, 1)).angle);
        return 1;
    }
    static int set_angle(lua_State* lua) {
        transform(lua, check_entity(lua, 1)).angle = static_cast<float>(luaL_checknumber(lua, 2));
        return 0;
    }
    static int name_of(lua_State* lua) {
        const auto entity = check_entity(lua, 1);
        const auto& names = host(lua).names_;
        lua_pushstring(lua, entity.index < names.size() ? names[entity.index].c_str() : "");
        return 1;
    }
    static Visual& visual(lua_State* lua, Entity entity) {
        auto* v = engine(lua).scene.visuals.find(entity);
        if (!v) luaL_error(lua, "that entity has no visual; spawn it with a material");
        return *v;
    }
    static int size(lua_State* lua) {
        const auto& v = visual(lua, check_entity(lua, 1));
        lua_pushnumber(lua, v.size.x);
        lua_pushnumber(lua, v.size.y);
        return 2;
    }
    static int set_size(lua_State* lua) {
        auto& v = visual(lua, check_entity(lua, 1));
        const Vec2 size{static_cast<float>(luaL_checknumber(lua, 2)),
                        static_cast<float>(luaL_optnumber(lua, 3, luaL_checknumber(lua, 2)))};
        if (!(size.x > 0 && size.y > 0 && size.x <= 64 && size.y <= 64))
            luaL_error(lua, "size must be above 0 and at most 64");
        v.size = size;
        return 0;
    }
    static int set_material(lua_State* lua) {
        visual(lua, check_entity(lua, 1)).material = engine(lua).materials.find(luaL_checkstring(lua, 2));
        return 0;
    }
    static int alive(lua_State* lua) {
        const auto entity = *static_cast<Entity*>(luaL_checkudata(lua, 1, entity_type));
        lua_pushboolean(lua, engine(lua).scene.alive(entity));
        return 1;
    }
    static int destroy(lua_State* lua) {
        auto& e = engine(lua);
        const auto entity = check_entity(lua, 1);
        if (entity == e.focus)
            luaL_error(lua, "the camera follows this entity; follow another before destroying it");
        host(lua).forget(entity);
        e.scene.destroy(entity);
        return 0;
    }
    static int equal(lua_State* lua) {
        const auto* a = static_cast<Entity*>(luaL_testudata(lua, 1, entity_type));
        const auto* b = static_cast<Entity*>(luaL_testudata(lua, 2, entity_type));
        lua_pushboolean(lua, a && b && *a == *b);
        return 1;
    }
    static int describe(lua_State* lua) {
        const auto entity = *static_cast<Entity*>(luaL_checkudata(lua, 1, entity_type));
        const auto& names = host(lua).names_;
        const char* name = entity.index < names.size() ? names[entity.index].c_str() : "";
        lua_pushfstring(lua, "entity \"%s\"", name);
        return 1;
    }

    static void module(lua_State* lua, const char* name, const luaL_Reg* functions) {
        lua_newtable(lua);
        luaL_setfuncs(lua, functions, 0);
        lua_setglobal(lua, name);
    }
    static void install(lua_State* lua) {
        luaL_requiref(lua, LUA_GNAME, luaopen_base, 1);
        luaL_requiref(lua, LUA_TABLIBNAME, luaopen_table, 1);
        luaL_requiref(lua, LUA_STRLIBNAME, luaopen_string, 1);
        luaL_requiref(lua, LUA_MATHLIBNAME, luaopen_math, 1);
        luaL_requiref(lua, LUA_UTF8LIBNAME, luaopen_utf8, 1);
        lua_pop(lua, 5);
        // No reaching outside the sandbox: no files, and no loading of code or bytecode at run time.
        for (const char* name : {"dofile", "loadfile", "load", "collectgarbage"}) {
            lua_pushnil(lua);
            lua_setglobal(lua, name);
        }
        static const luaL_Reg input[] = {{"held", guarded<held>},
                                         {"pressed", guarded<pressed>},
                                         {"released", guarded<released>},
                                         {"axis", guarded<axis>},
                                         {nullptr, nullptr}};
        static const luaL_Reg world[] = {
            {"find", guarded<find>}, {"spawn", guarded<spawn>}, {"tile", guarded<tile>}, {nullptr, nullptr}};
        static const luaL_Reg sound[] = {{"play", guarded<play>}, {nullptr, nullptr}};
        static const luaL_Reg particles[] = {{"burst", guarded<burst>}, {nullptr, nullptr}};
        static const luaL_Reg camera[] = {{"follow", guarded<follow>}, {nullptr, nullptr}};
        static const luaL_Reg game[] = {{"time", guarded<time>}, {nullptr, nullptr}};
        module(lua, "input", input);
        module(lua, "world", world);
        module(lua, "sound", sound);
        module(lua, "particles", particles);
        module(lua, "camera", camera);
        module(lua, "game", game);
        static const luaL_Reg methods[] = {{"position", guarded<position>},
                                           {"set_position", guarded<set_position>},
                                           {"move", guarded<move>},
                                           {"angle", guarded<angle>},
                                           {"set_angle", guarded<set_angle>},
                                           {"name", guarded<name_of>},
                                           {"size", guarded<size>},
                                           {"set_size", guarded<set_size>},
                                           {"set_material", guarded<set_material>},
                                           {"alive", guarded<alive>},
                                           {"destroy", guarded<destroy>},
                                           {nullptr, nullptr}};
        luaL_newmetatable(lua, entity_type);
        luaL_newlib(lua, methods);
        lua_setfield(lua, -2, "__index");
        lua_pushcfunction(lua, equal);
        lua_setfield(lua, -2, "__eq");
        lua_pushcfunction(lua, describe);
        lua_setfield(lua, -2, "__tostring");
        lua_pop(lua, 1);
        lua_sethook(lua, watchdog, LUA_MASKCOUNT, 10000);
    }
};

ScriptHost::ScriptHost(Engine& engine, ProjectFiles scripts)
    : engine_(engine), scripts_(std::move(scripts)), names_(entity_capacity) {
    lua_ = lua_newstate(ScriptApi::allocate, this);
    if (!lua_) throw std::bad_alloc();
    *static_cast<ScriptHost**>(lua_getextraspace(lua_)) = this;
    ScriptApi::install(lua_);
}

ScriptHost::~ScriptHost() {
    lua_close(lua_);
}

void ScriptHost::name(Entity entity, const std::string& name) {
    names_.at(entity.index) = name;
}

void ScriptHost::forget(Entity entity) {
    names_.at(entity.index).clear();
    for (auto& instance : instances_)
        if (instance.entity == entity) instance.failed = true; // Purged after the current update.
}

void ScriptHost::fail(Instance& instance, const std::string& message) {
    instance.failed = true;
    ++errors_;
    std::fprintf(stderr, "Script error: %s\n", message.c_str());
    std::fflush(stderr);
}

void ScriptHost::attach(Entity entity, const std::string& file) {
    Instance instance{entity, LUA_NOREF, file};
    std::string source;
    try {
        auto text = scripts_(file);
        if (!text) throw std::runtime_error("not found");
        source = std::move(*text);
    } catch (const std::exception& error) {
        instances_.push_back(instance);
        return fail(instances_.back(), "scripts/" + file + ": " + error.what());
    }
    lua_State* lua = lua_;
    lua_pushcfunction(lua, ScriptApi::traceback);
    const int handler = lua_gettop(lua);
    const auto chunk_name = "@scripts/" + file;
    if (luaL_loadbufferx(lua, source.data(), source.size(), chunk_name.c_str(), "t") != LUA_OK) {
        instances_.push_back(instance);
        fail(instances_.back(), lua_tostring(lua, -1));
        lua_settop(lua, handler - 1);
        return;
    }
    // The script's own globals: a table that falls back to the shared API for anything it lacks.
    lua_newtable(lua);
    lua_newtable(lua);
    lua_pushglobaltable(lua);
    lua_setfield(lua, -2, "__index");
    lua_setmetatable(lua, -2);
    ScriptApi::push_entity(lua, entity);
    lua_setfield(lua, -2, "self");
    lua_pushvalue(lua, -1);
    instance.environment = luaL_ref(lua, LUA_REGISTRYINDEX);
    lua_setupvalue(lua, -2, 1); // The chunk's _ENV.
    instances_.push_back(instance);
    const auto index = instances_.size() - 1;
    call_start_ = std::chrono::steady_clock::now();
    if (lua_pcall(lua, 0, 0, handler) != LUA_OK) fail(instances_[index], lua_tostring(lua, -1));
    lua_settop(lua, handler - 1);
}

void ScriptHost::call(Instance& instance, const char* function, int arguments) {
    // Arguments are on the stack already; they are consumed either way.
    lua_State* lua = lua_;
    const int base = lua_gettop(lua) - arguments;
    lua_rawgeti(lua, LUA_REGISTRYINDEX, instance.environment);
    lua_pushstring(lua, function);
    lua_rawget(lua, -2); // Only the script's own functions, never the shared globals.
    if (!lua_isfunction(lua, -1)) {
        lua_settop(lua, base);
        return;
    }
    lua_replace(lua, -2); // Function where the environment was.
    lua_insert(lua, base + 1);
    lua_pushcfunction(lua, ScriptApi::traceback);
    lua_insert(lua, base + 1);
    const auto file = instance.file;
    const auto entity = instance.entity;
    call_start_ = std::chrono::steady_clock::now();
    if (lua_pcall(lua, arguments, 0, base + 1) != LUA_OK) {
        // The call may have spawned scripted entities, moving `instance`; find it again.
        for (auto& i : instances_)
            if (i.entity == entity && i.file == file) {
                fail(i, lua_tostring(lua, -1));
                break;
            }
    }
    lua_settop(lua, base);
}

void ScriptHost::update(float dt) {
    time_ += dt;
    auto& scene = engine_.scene;
    // Scripted entities start each step where they are, so their moves interpolate smoothly.
    for (const auto& instance : instances_)
        if (!instance.failed && scene.alive(instance.entity))
            if (auto* t = scene.transforms.find(instance.entity)) t->previous = t->position;
    // By index: scripts may spawn scripted entities, growing the list while it is walked.
    for (std::size_t i = 0; i < instances_.size(); ++i) {
        if (instances_[i].failed || !scene.alive(instances_[i].entity)) continue;
        if (!instances_[i].started) {
            instances_[i].started = true;
            call(instances_[i], "start", 0);
            if (instances_[i].failed || !scene.alive(instances_[i].entity)) continue;
        }
        lua_pushnumber(lua_, dt);
        call(instances_[i], "update", 1);
    }
    // Drop the scripts of destroyed entities.
    std::erase_if(instances_, [&](const Instance& instance) {
        if (scene.alive(instance.entity)) return false; // A failed script stays, switched off.
        luaL_unref(lua_, LUA_REGISTRYINDEX, instance.environment);
        return true;
    });
}
} // namespace seed
