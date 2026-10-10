#include "script/host.hpp"
#include "app/app.hpp"
#include "io/storage.hpp"
#include "physics/character.hpp"
#include "project/characters.hpp"
#include "project/scene_file.hpp"
#include "render/renderer.hpp"
#include "script/saved_data.hpp"
// Lua is compiled as C++ (see CMakeLists.txt), so its errors are C++ exceptions and unwind engine
// code correctly. Its headers are therefore included without extern "C".
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <new>
#include <optional>
#include <queue>
#include <string_view>

namespace seed {
bool blocks_walking(void*, const Tile* tile) {
    return !tile || (tile->flags & tile_solid) || tile->elevation < 0;
}

// The Lua-facing half of ScriptHost: every function Lua calls, with access to its internals.
namespace {
// A* over loaded tiles from tile (sx, sy) to (gx, gy), stepping to the eight neighbours; a diagonal
// step needs both tiles beside it open. Returns the tiles walked through after the start, with
// straight runs reduced to their ends; nothing if the goal is blocked, unloaded or not reached
// within `budget` tiles searched.
std::optional<std::vector<std::pair<std::int64_t, std::int64_t>>> find_route(const World& world,
                                                                             std::int64_t sx, std::int64_t sy,
                                                                             std::int64_t gx, std::int64_t gy,
                                                                             std::size_t budget) {
    using Cell = std::pair<std::int64_t, std::int64_t>;
    const auto open_at = [&](std::int64_t x, std::int64_t y) {
        return !blocks_walking(
            nullptr, world.tile(from_global(static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5)));
    };
    if (!open_at(gx, gy)) return std::nullopt;
    const auto estimate = [&](Cell c) {
        const auto dx = static_cast<double>(std::llabs(c.first - gx)),
                   dy = static_cast<double>(std::llabs(c.second - gy));
        return std::max(dx, dy) + (std::sqrt(2.0) - 1) * std::min(dx, dy);
    };
    struct Node {
        double cost;
        Cell from;
        bool closed;
    };
    std::map<Cell, Node> nodes;
    using Entry = std::pair<double, Cell>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> frontier;
    nodes[{sx, sy}] = {0, {sx, sy}, false};
    frontier.push({estimate({sx, sy}), {sx, sy}});
    std::size_t searched = 0;
    while (!frontier.empty()) {
        const auto cell = frontier.top().second;
        frontier.pop();
        auto& node = nodes[cell];
        if (node.closed) continue;
        node.closed = true;
        if (cell == Cell{gx, gy}) {
            std::vector<Cell> route;
            for (auto c = cell; c != Cell{sx, sy}; c = nodes[c].from)
                route.push_back(c);
            std::reverse(route.begin(), route.end());
            // Keep only the turns and the end.
            std::vector<Cell> turns;
            for (std::size_t i = 0; i < route.size(); ++i) {
                if (i + 1 < route.size()) {
                    const auto& a = i ? route[i - 1] : Cell{sx, sy};
                    const auto& b = route[i];
                    const auto& c = route[i + 1];
                    if (b.first - a.first == c.first - b.first && b.second - a.second == c.second - b.second)
                        continue;
                }
                turns.push_back(route[i]);
            }
            return turns;
        }
        if (++searched > budget) return std::nullopt;
        const double cost = node.cost;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dy) continue;
                const Cell next{cell.first + dx, cell.second + dy};
                if (!open_at(next.first, next.second)) continue;
                if (dx && dy &&
                    (!open_at(cell.first + dx, cell.second) || !open_at(cell.first, cell.second + dy)))
                    continue;
                const double step = cost + (dx && dy ? std::sqrt(2.0) : 1.0);
                auto found = nodes.find(next);
                if (found != nodes.end() && (found->second.closed || found->second.cost <= step)) continue;
                nodes[next] = {step, cell, false};
                frontier.push({step + estimate(next), next});
            }
    }
    return std::nullopt;
}
} // namespace

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
        if (const auto prefab = text("prefab"); !prefab.empty()) {
            // A prefab brings its own look, light and script; a name given here replaces its own.
            auto& self = host(lua);
            if (!self.spawn_prefab_) luaL_error(lua, "prefabs cannot be spawned here");
            const auto entity = self.spawn_prefab_(prefab, at, static_cast<float>(number("angle", 0)));
            if (const auto name = text("name"); !name.empty()) self.name(entity, name);
            push_entity(lua, entity);
            return 1;
        }
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

    // Replaces fields of a loaded tile; the change is saved with the world. Fields left out keep
    // their value; object = false (or "") removes the object.
    static int set_tile(lua_State* lua) {
        luaL_checktype(lua, 1, LUA_TTABLE);
        auto& e = engine(lua);
        lua_getfield(lua, 1, "x");
        lua_getfield(lua, 1, "y");
        const auto at = position_at(lua, -2);
        lua_pop(lua, 2);
        const auto* current = e.world.tile(at);
        if (!current) {
            lua_pushboolean(lua, false); // Not loaded: too far from the camera.
            return 1;
        }
        Tile tile = *current;
        lua_getfield(lua, 1, "ground");
        if (!lua_isnil(lua, -1)) tile.material = e.materials.find(luaL_checkstring(lua, -1));
        lua_pop(lua, 1);
        lua_getfield(lua, 1, "object");
        if (lua_isboolean(lua, -1) && !lua_toboolean(lua, -1))
            tile.object = no_object;
        else if (!lua_isnil(lua, -1)) {
            const std::string name = luaL_checkstring(lua, -1);
            tile.object = name.empty() ? no_object : tile_object(e.materials.find(name));
        }
        lua_pop(lua, 1);
        lua_getfield(lua, 1, "solid");
        if (!lua_isnil(lua, -1)) {
            luaL_checktype(lua, -1, LUA_TBOOLEAN);
            tile.flags = static_cast<std::uint8_t>(lua_toboolean(lua, -1) ? tile.flags | tile_solid
                                                                          : tile.flags & ~tile_solid);
        }
        lua_pop(lua, 1);
        lua_getfield(lua, 1, "elevation");
        if (!lua_isnil(lua, -1)) {
            const auto height = luaL_checknumber(lua, -1);
            if (!(height >= -1000 && height <= 1000)) luaL_error(lua, "elevation must be -1000 to 1000");
            tile.elevation = static_cast<float>(height);
        }
        lua_pop(lua, 1);
        lua_pushboolean(lua, e.world.set_tile(at, tile));
        return 1;
    }

    // What the ground at a point is for walking: its material, speed and tags.
    static int surface(lua_State* lua) {
        auto& e = engine(lua);
        const auto* t = e.world.tile(position_at(lua, 1));
        if (!t) {
            lua_pushnil(lua);
            return 1;
        }
        const auto& m = e.materials[t->material];
        lua_createtable(lua, 0, 3);
        lua_pushstring(lua, m.name);
        lua_setfield(lua, -2, "material");
        lua_pushnumber(lua, m.speed);
        lua_setfield(lua, -2, "speed");
        lua_newtable(lua);
        std::string_view tags = m.tags ? m.tags : "";
        for (int n = 1; !tags.empty(); ++n) {
            const auto end = std::min(tags.find(' '), tags.size());
            lua_pushlstring(lua, tags.data(), end);
            lua_rawseti(lua, -2, n);
            tags.remove_prefix(std::min(end + 1, tags.size()));
        }
        lua_setfield(lua, -2, "tags");
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
    static int player(lua_State* lua) {
        auto& self = host(lua);
        if (self.engine_.scene.alive(self.player_))
            push_entity(lua, self.player_);
        else
            lua_pushnil(lua);
        return 1;
    }
    static int zoom(lua_State* lua) {
        lua_pushnumber(lua, engine(lua).camera_zoom);
        return 1;
    }
    static int set_zoom(lua_State* lua) {
        const auto value = luaL_checknumber(lua, 1);
        if (!(value >= 2 && value <= 512)) luaL_error(lua, "zoom must be from 2 to 512 pixels per tile");
        engine(lua).camera_zoom = static_cast<float>(value);
        return 0;
    }
    // Characters.
    static CharacterMotion& motion(lua_State* lua, Entity entity) {
        auto* m = engine(lua).scene.components<CharacterMotion>().find(entity);
        if (!m) luaL_error(lua, "that entity is not a character; give it a Character component");
        return *m;
    }
    static int walk(lua_State* lua) {
        auto& m = motion(lua, check_entity(lua, 1));
        host(lua).cancel_route(check_entity(lua, 1));
        m.walk = {static_cast<float>(luaL_checknumber(lua, 2)), static_cast<float>(luaL_checknumber(lua, 3))};
        m.running = lua_toboolean(lua, 4);
        m.has_target = false;
        return 0;
    }
    static int walk_to(lua_State* lua) {
        const auto entity = check_entity(lua, 1);
        auto& m = motion(lua, entity);
        const auto target = position_at(lua, 2);
        const auto& position = transform(lua, entity).position;
        // True once there; otherwise it keeps walking there, step by step, until it arrives.
        const bool there = nearby(target.chunk, position.chunk, 64) && [&] {
            const auto gap = relative(target, position);
            return gap.x * gap.x + gap.y * gap.y < 1e-6F;
        }();
        host(lua).cancel_route(entity);
        if (!there) {
            m.target = target;
            m.has_target = true;
            m.walk = {};
            m.running = lua_toboolean(lua, 4);
        }
        lua_pushboolean(lua, there);
        return 1;
    }
    // Routes: a list of points to walk through in turn.
    static void push_points(lua_State* lua, const std::vector<WorldPosition>& points) {
        lua_createtable(lua, static_cast<int>(points.size()), 0);
        for (std::size_t i = 0; i < points.size(); ++i) {
            lua_createtable(lua, 2, 0);
            push_position(lua, points[i]);
            lua_rawseti(lua, -3, 2);
            lua_rawseti(lua, -2, 1);
            lua_rawseti(lua, -2, static_cast<lua_Integer>(i + 1));
        }
    }
    static std::vector<WorldPosition> read_points(lua_State* lua, int index) {
        luaL_checktype(lua, index, LUA_TTABLE);
        const auto count = luaL_len(lua, index);
        if (count < 1 || count > 4096) luaL_error(lua, "a route has 1 to 4096 points");
        std::vector<WorldPosition> points;
        for (lua_Integer i = 1; i <= count; ++i) {
            lua_rawgeti(lua, index, i);
            if (!lua_istable(lua, -1)) luaL_error(lua, "each point is {x, y}");
            lua_rawgeti(lua, -1, 1);
            lua_rawgeti(lua, -2, 2);
            points.push_back(position_at(lua, -2));
            lua_pop(lua, 3);
        }
        return points;
    }
    // e:follow(points or a path entity [, loop [, run]]).
    static int follow_route(lua_State* lua) {
        const auto entity = check_entity(lua, 1);
        auto& m = motion(lua, entity);
        auto& self = host(lua);
        ScriptHost::Route route{entity, {}, 0, false, false};
        if (lua_isuserdata(lua, 2)) {
            const auto path_entity = check_entity(lua, 2);
            const auto found =
                std::find_if(self.paths_.begin(), self.paths_.end(),
                             [&](const ScriptHost::Path& p) { return p.entity == path_entity; });
            if (found == self.paths_.end()) luaL_error(lua, "that entity has no path");
            route.points = found->points;
            route.loop = found->loop;
        } else
            route.points = read_points(lua, 2);
        if (!lua_isnoneornil(lua, 3)) route.loop = lua_toboolean(lua, 3);
        route.running = lua_toboolean(lua, 4);
        self.cancel_route(entity);
        m.target = route.points.front();
        m.has_target = true;
        m.walk = {};
        m.running = route.running;
        self.routes_.push_back(std::move(route));
        return 0;
    }
    // world.find_path(x0, y0, x1, y1): the points to walk through, or nil.
    static std::optional<std::vector<WorldPosition>> route_between(lua_State* lua, WorldPosition from,
                                                                   WorldPosition to) {
        const auto cell = [](double v) {
            return static_cast<std::int64_t>(std::floor(v));
        };
        const double fx = global_coordinate(from.chunk.x, from.local.x),
                     fy = global_coordinate(from.chunk.y, from.local.y);
        const double tx = global_coordinate(to.chunk.x, to.local.x),
                     ty = global_coordinate(to.chunk.y, to.local.y);
        if (std::abs(tx - fx) > 512 || std::abs(ty - fy) > 512)
            luaL_error(lua, "a path reaches at most 512 tiles");
        const auto cells = find_route(engine(lua).world, cell(fx), cell(fy), cell(tx), cell(ty), 16384);
        if (!cells) return std::nullopt;
        std::vector<WorldPosition> points;
        for (const auto& [x, y] : *cells)
            points.push_back(from_global(static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5));
        if (points.empty())
            points.push_back(to);
        else
            points.back() = to; // End on the point asked for, not its tile's centre.
        return points;
    }
    static int find_path(lua_State* lua) {
        const auto points = route_between(lua, position_at(lua, 1), position_at(lua, 3));
        if (points)
            push_points(lua, *points);
        else
            lua_pushnil(lua);
        return 1;
    }
    // e:go_to(x, y [, run]): finds a way there and follows it; false if there is none.
    static int go_to(lua_State* lua) {
        const auto entity = check_entity(lua, 1);
        motion(lua, entity);
        const auto points = route_between(lua, transform(lua, entity).position, position_at(lua, 2));
        if (!points) {
            lua_pushboolean(lua, false);
            return 1;
        }
        lua_settop(lua, 4);
        const bool running = lua_toboolean(lua, 4);
        lua_settop(lua, 1);
        push_points(lua, *points);
        lua_pushboolean(lua, false);
        lua_pushboolean(lua, running);
        follow_route(lua);
        lua_pushboolean(lua, true);
        return 1;
    }
    // world.path(entity): its points and whether it loops.
    static int path(lua_State* lua) {
        const auto entity = check_entity(lua, 1);
        auto& self = host(lua);
        const auto found = std::find_if(self.paths_.begin(), self.paths_.end(),
                                        [&](const ScriptHost::Path& p) { return p.entity == entity; });
        if (found == self.paths_.end()) {
            lua_pushnil(lua);
            return 1;
        }
        push_points(lua, found->points);
        lua_pushboolean(lua, found->loop);
        return 2;
    }
    static int stop(lua_State* lua) {
        host(lua).cancel_route(check_entity(lua, 1));
        auto& m = motion(lua, check_entity(lua, 1));
        m.walk = {};
        m.has_target = false;
        m.velocity = {};
        return 0;
    }
    static int speed(lua_State* lua) {
        const auto& m = motion(lua, check_entity(lua, 1));
        lua_pushnumber(lua, m.speed);
        lua_pushnumber(lua, m.run_speed);
        return 2;
    }
    static int set_speed(lua_State* lua) {
        auto& m = motion(lua, check_entity(lua, 1));
        const auto walking = luaL_checknumber(lua, 2), running = luaL_optnumber(lua, 3, m.run_speed);
        if (!(walking >= 0 && walking <= 100 && running >= 0 && running <= 100))
            luaL_error(lua, "speeds must be from 0 to 100 tiles per second");
        m.speed = static_cast<float>(walking);
        m.run_speed = static_cast<float>(running);
        return 0;
    }
    static int moving(lua_State* lua) {
        const auto& m = motion(lua, check_entity(lua, 1));
        const auto entity = check_entity(lua, 1);
        const auto& routes = host(lua).routes_;
        lua_pushboolean(lua, m.has_target || m.walk.x != 0 || m.walk.y != 0 ||
                                 std::any_of(routes.begin(), routes.end(),
                                             [&](const ScriptHost::Route& r) { return r.entity == entity; }));
        return 1;
    }
    static int time(lua_State* lua) {
        lua_pushnumber(lua, host(lua).time_);
        return 1;
    }

    // Entities within `radius` of a point, nearest first, optionally only those with a name.
    static int near(lua_State* lua) {
        auto& self = host(lua);
        auto& e = self.engine_;
        const auto at = position_at(lua, 1);
        const double radius = luaL_checknumber(lua, 3);
        if (!(radius >= 0 && radius <= 256)) luaL_error(lua, "radius must be from 0 to 256 tiles");
        const char* name = luaL_optstring(lua, 4, nullptr);
        std::vector<std::pair<float, Entity>> found;
        const auto owners = e.scene.transforms.owners();
        const auto values = e.scene.transforms.values();
        for (std::size_t i = 0; i < owners.size(); ++i) {
            if (!nearby(values[i].position.chunk, at.chunk, 9)) continue;
            if (name && !(owners[i].index < self.names_.size() && self.names_[owners[i].index] == name))
                continue;
            const auto r = relative(values[i].position, at);
            const float d = std::sqrt(r.x * r.x + r.y * r.y);
            if (d <= radius) found.emplace_back(d, owners[i]);
        }
        std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        lua_createtable(lua, static_cast<int>(found.size()), 0);
        for (std::size_t i = 0; i < found.size(); ++i) {
            push_entity(lua, found[i].second);
            lua_rawseti(lua, -2, static_cast<lua_Integer>(i + 1));
        }
        return 1;
    }

    // ui: drawn over the game in logical pixels from the top-left corner. What a step draws
    // stays on screen until the next step draws again.
    static void read_color(lua_State* lua, int index, float out[4]) {
        out[0] = out[1] = out[2] = out[3] = 1;
        if (lua_isnoneornil(lua, index)) return;
        luaL_checktype(lua, index, LUA_TTABLE);
        for (int i = 0; i < 4; ++i) {
            lua_rawgeti(lua, index, i + 1);
            if (!lua_isnil(lua, -1)) {
                const auto c = luaL_checknumber(lua, -1);
                if (!(c >= 0 && c <= 1)) luaL_error(lua, "colour values must be from 0 to 1");
                out[i] = static_cast<float>(c);
            }
            lua_pop(lua, 1);
        }
    }
    static void add_ui(lua_State* lua, ScriptHost::UiCommand command) {
        auto& ui = host(lua).ui_;
        if (ui.size() >= 4096) luaL_error(lua, "too much ui in one step (4,096 items)");
        ui.push_back(std::move(command));
    }
    static int ui_rect(lua_State* lua) {
        ScriptHost::UiCommand c{};
        c.x = static_cast<float>(luaL_checknumber(lua, 1));
        c.y = static_cast<float>(luaL_checknumber(lua, 2));
        c.width = static_cast<float>(luaL_checknumber(lua, 3));
        c.height = static_cast<float>(luaL_checknumber(lua, 4));
        read_color(lua, 5, c.color);
        add_ui(lua, std::move(c));
        return 0;
    }
    static int ui_text(lua_State* lua) {
        ScriptHost::UiCommand c{};
        c.x = static_cast<float>(luaL_checknumber(lua, 1));
        c.y = static_cast<float>(luaL_checknumber(lua, 2));
        c.text = luaL_checkstring(lua, 3);
        if (c.text.empty()) return 0;
        if (c.text.size() > 1024) luaL_error(lua, "ui text is at most 1,024 characters");
        c.scale = static_cast<float>(luaL_optnumber(lua, 4, 2));
        if (!(c.scale >= 1 && c.scale <= 16)) luaL_error(lua, "text scale must be from 1 to 16");
        read_color(lua, 5, c.color);
        add_ui(lua, std::move(c));
        return 0;
    }
    static int ui_text_width(lua_State* lua) {
        const auto scale = luaL_optnumber(lua, 2, 2);
        lua_pushnumber(lua, Renderer::text_width(luaL_checkstring(lua, 1), static_cast<float>(scale)));
        return 1;
    }
    static int ui_size(lua_State* lua) {
        lua_pushinteger(lua, host(lua).screen_width_);
        lua_pushinteger(lua, host(lua).screen_height_);
        return 2;
    }

    // The mouse.
    static int pointer(lua_State* lua) {
        const auto& p = host(lua).pointer_;
        lua_pushnumber(lua, global_coordinate(p.chunk.x, p.local.x));
        lua_pushnumber(lua, global_coordinate(p.chunk.y, p.local.y));
        return 2;
    }
    static int screen_pointer(lua_State* lua) {
        lua_pushnumber(lua, host(lua).pointer_x_);
        lua_pushnumber(lua, host(lua).pointer_y_);
        return 2;
    }

    static int load_scene(lua_State* lua) {
        const std::string name = luaL_checkstring(lua, 1), spawn = luaL_optstring(lua, 2, "");
        if (!valid_prefab_name(name)) luaL_error(lua, "scene names use letters, digits, '_' and '-'");
        host(lua).scene_request_ = std::make_pair(name, spawn);
        return 0;
    }
    static int scene(lua_State* lua) {
        lua_pushstring(lua, host(lua).scene_.c_str());
        return 1;
    }
    static int saved_scene(lua_State* lua) {
        const auto& name = host(lua).saved_scene_;
        if (name.empty())
            lua_pushnil(lua);
        else
            lua_pushstring(lua, name.c_str());
        return 1;
    }

    static int set_paused(lua_State* lua) {
        host(lua).paused_ = lua_toboolean(lua, 1);
        return 0;
    }
    static int paused(lua_State* lua) {
        lua_pushboolean(lua, host(lua).paused_);
        return 1;
    }

    // music: one song at a time, from assets/music.
    static int music_play(lua_State* lua) {
        const std::string name = luaL_checkstring(lua, 1);
        const bool loop = lua_isnoneornil(lua, 2) || lua_toboolean(lua, 2);
        engine(lua).play_music(name, loop);
        return 0;
    }
    static int music_stop(lua_State* lua) {
        engine(lua).stop_music();
        return 0;
    }
    static int music_volume(lua_State* lua) {
        const auto v = luaL_checknumber(lua, 1);
        if (!(v >= 0 && v <= 1)) luaL_error(lua, "music volume must be from 0 to 1");
        engine(lua).audio.set_music_volume(static_cast<float>(v));
        return 0;
    }
    static int music_playing(lua_State* lua) {
        lua_pushboolean(lua, engine(lua).music_playing());
        return 1;
    }

    // atmosphere: the game's time of day, and how light the scene playing is.
    static DayClock& clock(lua_State* lua) {
        auto* c = host(lua).clock_;
        if (!c) luaL_error(lua, "this game has no time of day");
        return *c;
    }
    static int hour(lua_State* lua) {
        lua_pushnumber(lua, clock(lua).hour);
        return 1;
    }
    static int set_hour(lua_State* lua) {
        const auto value = luaL_checknumber(lua, 1);
        if (!(value >= 0 && value <= 24)) luaL_error(lua, "the hour must be from 0 to 24");
        clock(lua).set_hour(value);
        return 0;
    }
    static int light_of_day(lua_State* lua) {
        lua_pushnumber(lua, daylight(static_cast<float>(clock(lua).shown())));
        return 1;
    }

    // Entity methods.
    static Visual& visual_of(lua_State* lua, Entity entity) {
        auto* v = engine(lua).scene.visuals.find(entity);
        if (!v) luaL_error(lua, "that entity has no visual");
        return *v;
    }
    // Shows one frame of an animated material and stops the character animating by itself.
    static int set_frame(lua_State* lua) {
        const auto entity = check_entity(lua, 1);
        const auto frame = luaL_checkinteger(lua, 2);
        auto& v = visual_of(lua, entity);
        const auto frames = engine(lua).materials[v.material].frames;
        if (frame < 0 || frame >= static_cast<lua_Integer>(frames))
            luaL_error(lua, "frame must be from 0 to %d for this material", static_cast<int>(frames) - 1);
        v.frame = static_cast<std::uint8_t>(frame);
        v.still = true;
        if (auto* m = engine(lua).scene.components<CharacterMotion>().find(entity)) m->animate = false;
        return 0;
    }
    // The frame showing, and whether the animation is playing.
    static int frame(lua_State* lua) {
        const auto& v = visual_of(lua, check_entity(lua, 1));
        const auto& m = engine(lua).materials[v.material];
        const bool playing = !v.still && m.frames > 1 && m.fps > 0;
        const auto shown = v.still ? v.frame
                           : playing
                               ? static_cast<unsigned>(std::fmod(engine(lua).time * m.fps, 1e9)) % m.frames
                               : 0U;
        lua_pushinteger(lua, static_cast<lua_Integer>(shown));
        lua_pushboolean(lua, playing);
        return 2;
    }
    // Lets the animation play again (for a character: while it walks).
    static int animate(lua_State* lua) {
        const auto entity = check_entity(lua, 1);
        auto& v = visual_of(lua, entity);
        v.still = false;
        if (auto* m = engine(lua).scene.components<CharacterMotion>().find(entity)) m->animate = true;
        return 0;
    }
    static int light(lua_State* lua) {
        const auto* l = engine(lua).scene.components<LightComponent>().find(check_entity(lua, 1));
        if (!l) {
            lua_pushnil(lua);
            return 1;
        }
        lua_createtable(lua, 0, 6);
        lua_createtable(lua, 3, 0);
        for (int i = 0; i < 3; ++i) {
            lua_pushnumber(lua, l->color[static_cast<std::size_t>(i)]);
            lua_rawseti(lua, -2, i + 1);
        }
        lua_setfield(lua, -2, "color");
        const std::pair<const char*, float> fields[] = {{"radius", l->radius},
                                                        {"intensity", l->intensity},
                                                        {"height", l->height},
                                                        {"flicker", l->flicker}};
        for (const auto& [key, value] : fields) {
            lua_pushnumber(lua, value);
            lua_setfield(lua, -2, key);
        }
        lua_pushboolean(lua, l->night_only);
        lua_setfield(lua, -2, "night_only");
        return 1;
    }
    // Changes the fields given; an entity without a light gets one, with the editor's defaults.
    static int set_light(lua_State* lua) {
        const auto entity = check_entity(lua, 1);
        luaL_checktype(lua, 2, LUA_TTABLE);
        auto& lights = engine(lua).scene.components<LightComponent>();
        LightComponent l;
        if (const auto* found = lights.find(entity))
            l = *found;
        else {
            const SceneLight defaults;
            l = {defaults.color, defaults.radius, defaults.intensity, defaults.height, 0, false};
        }
        const auto number = [&](const char* key, float& out, float low, float high) {
            lua_getfield(lua, 2, key);
            if (!lua_isnil(lua, -1)) {
                const auto value = luaL_checknumber(lua, -1);
                if (!(value >= low && value <= high))
                    luaL_error(lua, "%s must be from %f to %f", key, low, high);
                out = static_cast<float>(value);
            }
            lua_pop(lua, 1);
        };
        lua_getfield(lua, 2, "color");
        if (lua_istable(lua, -1))
            for (int i = 0; i < 3; ++i) {
                lua_rawgeti(lua, -1, i + 1);
                const auto c = luaL_checknumber(lua, -1);
                if (!(c >= 0 && c <= 16)) luaL_error(lua, "colour values must be from 0 to 16");
                l.color[static_cast<std::size_t>(i)] = static_cast<float>(c);
                lua_pop(lua, 1);
            }
        else if (!lua_isnil(lua, -1))
            luaL_error(lua, "color is a table of three numbers");
        lua_pop(lua, 1);
        number("radius", l.radius, 0.01F, 256);
        number("intensity", l.intensity, 0, 64);
        number("height", l.height, 0.01F, 64);
        number("flicker", l.flicker, 0, 1);
        lua_getfield(lua, 2, "night_only");
        if (!lua_isnil(lua, -1)) l.night_only = lua_toboolean(lua, -1);
        lua_pop(lua, 1);
        if (auto* found = lights.find(entity))
            *found = l;
        else
            lights.add(entity, l);
        return 0;
    }
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
                                         {"pointer", guarded<pointer>},
                                         {"screen_pointer", guarded<screen_pointer>},
                                         {nullptr, nullptr}};
        static const luaL_Reg world[] = {
            {"find", guarded<find>},           {"spawn", guarded<spawn>},     {"tile", guarded<tile>},
            {"set_tile", guarded<set_tile>},   {"surface", guarded<surface>}, {"near", guarded<near>},
            {"find_path", guarded<find_path>}, {"path", guarded<path>},       {nullptr, nullptr}};
        static const luaL_Reg sound[] = {{"play", guarded<play>}, {nullptr, nullptr}};
        static const luaL_Reg particles[] = {{"burst", guarded<burst>}, {nullptr, nullptr}};
        static const luaL_Reg camera[] = {{"follow", guarded<follow>},
                                          {"zoom", guarded<zoom>},
                                          {"set_zoom", guarded<set_zoom>},
                                          {nullptr, nullptr}};
        static const luaL_Reg game[] = {{"time", guarded<time>},
                                        {"player", guarded<player>},
                                        {"set_paused", guarded<set_paused>},
                                        {"paused", guarded<paused>},
                                        {"load_scene", guarded<load_scene>},
                                        {"scene", guarded<scene>},
                                        {"saved_scene", guarded<saved_scene>},
                                        {nullptr, nullptr}};
        module(lua, "input", input);
        module(lua, "world", world);
        module(lua, "sound", sound);
        module(lua, "particles", particles);
        module(lua, "camera", camera);
        module(lua, "game", game);
        // game.data: a table every script shares, which outlives scene changes (score, inventory).
        lua_getglobal(lua, "game");
        lua_newtable(lua);
        lua_setfield(lua, -2, "data");
        lua_pop(lua, 1);
        static const luaL_Reg atmosphere[] = {{"hour", guarded<hour>},
                                              {"set_hour", guarded<set_hour>},
                                              {"daylight", guarded<light_of_day>},
                                              {nullptr, nullptr}};
        module(lua, "atmosphere", atmosphere);
        static const luaL_Reg music[] = {{"play", guarded<music_play>},
                                         {"stop", guarded<music_stop>},
                                         {"set_volume", guarded<music_volume>},
                                         {"playing", guarded<music_playing>},
                                         {nullptr, nullptr}};
        module(lua, "music", music);
        static const luaL_Reg ui[] = {{"rect", guarded<ui_rect>},
                                      {"text", guarded<ui_text>},
                                      {"text_width", guarded<ui_text_width>},
                                      {"size", guarded<ui_size>},
                                      {nullptr, nullptr}};
        module(lua, "ui", ui);
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
                                           {"walk", guarded<walk>},
                                           {"walk_to", guarded<walk_to>},
                                           {"follow", guarded<follow_route>},
                                           {"go_to", guarded<go_to>},
                                           {"stop", guarded<stop>},
                                           {"speed", guarded<speed>},
                                           {"set_speed", guarded<set_speed>},
                                           {"moving", guarded<moving>},
                                           {"light", guarded<light>},
                                           {"set_frame", guarded<set_frame>},
                                           {"frame", guarded<frame>},
                                           {"animate", guarded<animate>},
                                           {"set_light", guarded<set_light>},
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

void ScriptHost::set_path(Entity entity, std::vector<WorldPosition> points, bool loop) {
    if (points.empty()) throw std::invalid_argument("A path needs points");
    std::erase_if(paths_, [&](const ScriptHost::Path& p) { return p.entity == entity; });
    paths_.push_back({entity, std::move(points), loop});
}

void ScriptHost::cancel_route(Entity entity) {
    std::erase_if(routes_, [&](const Route& r) { return r.entity == entity; });
}

void ScriptHost::advance_routes() {
    auto& motions = engine_.scene.components<CharacterMotion>();
    std::erase_if(routes_, [&](Route& route) {
        auto* m = engine_.scene.alive(route.entity) ? motions.find(route.entity) : nullptr;
        if (!m) return true;
        if (m->has_target) return false; // Still on its way to the current point.
        if (++route.next == route.points.size()) {
            if (!route.loop) return true;
            route.next = 0;
        }
        m->target = route.points[route.next];
        m->has_target = true;
        m->running = route.running;
        return false;
    });
}

void ScriptHost::forget(Entity entity) {
    names_.at(entity.index).clear();
    cancel_route(entity);
    std::erase_if(paths_, [&](const ScriptHost::Path& p) { return p.entity == entity; });
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
    Instance instance{entity, LUA_NOREF, file, false, false, {}, {}};
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

void ScriptHost::surfaces() {
    auto& motions = engine_.scene.components<CharacterMotion>();
    for (std::size_t i = 0; i < instances_.size(); ++i) {
        if (instances_[i].failed || !engine_.scene.alive(instances_[i].entity)) continue;
        const auto* motion = motions.find(instances_[i].entity);
        if (!motion || !motion->surface_changed) continue;
        lua_pushstring(lua_, engine_.materials[static_cast<MaterialId>(motion->surface)].name);
        call(instances_[i], "on_surface", 1);
    }
}

std::vector<std::uint8_t> ScriptHost::save_data() {
    std::string skipped;
    call_start_ = std::chrono::steady_clock::now();
    auto bytes = save_lua_data(lua_, skipped);
    // Reported each time the game saves, until the script stops storing them.
    for (std::size_t start = 0; start < skipped.size();) {
        const auto end = skipped.find('\n', start);
        report(skipped.substr(start, end - start) + ", which cannot be saved; the save leaves it out");
        start = end + 1;
    }
    return bytes;
}

void ScriptHost::load_data(std::span<const std::uint8_t> bytes) {
    call_start_ = std::chrono::steady_clock::now();
    load_lua_data(lua_, bytes);
}

void ScriptHost::report(const std::string& message) {
    std::fprintf(stderr, "Script error: %s\n", message.c_str());
    ++errors_;
}

void ScriptHost::set_pointer(WorldPosition world, float x, float y, int width, int height) {
    pointer_ = world;
    pointer_x_ = x, pointer_y_ = y;
    screen_width_ = width, screen_height_ = height;
}

void ScriptHost::draw_ui(Renderer& renderer) const {
    for (const auto& c : ui_) {
        const Color color{c.color[0], c.color[1], c.color[2], c.color[3]};
        if (c.text.empty())
            renderer.ui_rect(c.x, c.y, c.width, c.height, color);
        else
            renderer.text(c.x, c.y, c.text, c.scale, color);
    }
}

void ScriptHost::touches() {
    auto& scene = engine_.scene;
    auto& motions = scene.components<CharacterMotion>();
    auto& areas = scene.components<AreaComponent>();
    // An entity's box: a character's collision box, else its visual's size; none for markers.
    const auto half_of = [&](Entity e, Vec2& half) {
        if (const auto* m = motions.find(e)) return half = m->half, true;
        if (const auto* v = scene.visuals.find(e)) return half = v->size * 0.5F, true;
        return false;
    };
    const auto owners = scene.transforms.owners();
    const auto values = scene.transforms.values();
    std::vector<Entity> now;
    for (std::size_t i = 0; i < instances_.size(); ++i) {
        if (instances_[i].failed || !scene.alive(instances_[i].entity)) continue;
        const auto self = instances_[i].entity;
        Vec2 half{};
        const auto* at = scene.transforms.find(self);
        now.clear();
        if (at && half_of(self, half))
            for (std::size_t k = 0; k < owners.size(); ++k) {
                Vec2 other{};
                if (owners[k] == self || !nearby(values[k].position.chunk, at->position.chunk, 1) ||
                    !half_of(owners[k], other))
                    continue;
                const auto r = relative(values[k].position, at->position);
                if (std::abs(r.x) < half.x + other.x && std::abs(r.y) < half.y + other.y)
                    now.push_back(owners[k]);
            }
        // Calls may spawn or destroy entities, so the lists are copied before any call.
        const auto before = instances_[i].touching;
        instances_[i].touching = now;
        const auto contains = [](const std::vector<Entity>& list, Entity e) {
            return std::find(list.begin(), list.end(), e) != list.end();
        };
        // Calls may also attach scripts, so the instance is found again for each.
        const auto notify = [&](Entity other, const char* function) {
            for (auto& instance : instances_)
                if (instance.entity == self && !instance.failed) {
                    ScriptApi::push_entity(lua_, other);
                    call(instance, function, 1);
                    break;
                }
        };
        const auto started = now;
        for (const auto other : before)
            if (!contains(started, other) && scene.alive(other)) notify(other, "on_leave");
        for (const auto other : started)
            if (!contains(before, other) && scene.alive(other) && scene.alive(self))
                notify(other, "on_touch");

        // An area: the characters whose centres are in it.
        if (instances_[i].failed || !scene.alive(self)) continue;
        const auto* area = areas.find(self);
        at = scene.transforms.find(self);
        if (!area || !at) continue;
        const auto reach = static_cast<std::uint64_t>(std::max(area->half.x, area->half.y) / chunk_side) + 1;
        const auto centre = at->position;
        now.clear();
        const auto characters = motions.owners();
        for (const auto other : characters) {
            const auto* t = scene.transforms.find(other);
            if (other == self || !t || !nearby(t->position.chunk, centre.chunk, reach)) continue;
            const auto r = relative(t->position, centre);
            if (std::abs(r.x) < area->half.x && std::abs(r.y) < area->half.y) now.push_back(other);
        }
        const auto was_inside = instances_[i].inside;
        instances_[i].inside = now;
        const auto entered = now;
        for (const auto other : was_inside)
            if (!contains(entered, other) && scene.alive(other)) notify(other, "on_exit");
        for (const auto other : entered)
            if (!contains(was_inside, other) && scene.alive(other) && scene.alive(self))
                notify(other, "on_enter");
    }
}

void ScriptHost::update(float dt) {
    time_ += dt;
    ui_.clear(); // Each step draws its ui afresh.
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
    advance_routes();
    // Drop the scripts of destroyed entities.
    std::erase_if(instances_, [&](const Instance& instance) {
        if (scene.alive(instance.entity)) return false; // A failed script stays, switched off.
        luaL_unref(lua_, LUA_REGISTRYINDEX, instance.environment);
        return true;
    });
}
} // namespace seed
