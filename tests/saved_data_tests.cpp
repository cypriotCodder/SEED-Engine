// game.data in a save: what round-trips, what is left out, and saved bytes that must be refused
// without touching the game.data already there.
#include "script/saved_data.hpp"
// Lua is compiled as C++ (see CMakeLists.txt).
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F>
void refused(F&& f, const std::string& message) {
    try {
        f();
    } catch (const std::runtime_error&) {
        return;
    }
    throw std::runtime_error(message);
}

using State = std::unique_ptr<lua_State, decltype(&lua_close)>;

// A Lua state with the globals scripts share: `game`, with an empty game.data.
State fresh() {
    State lua(luaL_newstate(), &lua_close);
    luaL_openlibs(lua.get());
    luaL_dostring(lua.get(), "game = {data = {}}");
    return lua;
}
void run(lua_State* lua, const char* code) {
    if (luaL_dostring(lua, code) != LUA_OK) throw std::runtime_error(lua_tostring(lua, -1));
}
bool truth(lua_State* lua, const char* expression) {
    run(lua, (std::string("return ") + expression).c_str());
    const bool result = lua_toboolean(lua, -1);
    lua_pop(lua, 1);
    return result;
}
std::vector<std::uint8_t> save(lua_State* lua) {
    std::string skipped;
    auto bytes = seed::save_lua_data(lua, skipped);
    check(skipped.empty(), "Nothing was expected to be left out: " + skipped);
    return bytes;
}
// The table { [key] = value } as saved bytes, for hand-made broken saves.
std::vector<std::uint8_t> table(std::vector<std::uint8_t> entries, std::uint32_t count = 1) {
    std::vector<std::uint8_t> out{6, std::uint8_t(count), std::uint8_t(count >> 8), std::uint8_t(count >> 16),
                                  std::uint8_t(count >> 24)};
    out.insert(out.end(), entries.begin(), entries.end());
    return out;
}
const std::vector<std::uint8_t> key_a{5, 1, 0, 0, 0, 'a'}; // The string "a".
} // namespace

int main() {
    try {
        // Everything game.data may hold comes back as it was.
        auto a = fresh();
        run(a.get(), R"(
            game.data.coins = 42
            game.data.speed = 2.5
            game.data.tiny = -0.0
            game.data.big = math.maxinteger
            game.data.name = "Ayla\0with a zero"
            game.data.done = true
            game.data.lost = false
            game.data.list = {"a", "b", {deep = {deeper = 7}}}
            game.data[3] = "three"
            game.data[1.5] = "one and a half"
            game.data[true] = "yes"
            game.data.empty = {}
        )");
        const auto bytes = save(a.get());
        check(bytes == save(a.get()), "The same data saves the same bytes");
        auto b = fresh();
        run(b.get(), "game.data.stale = 1");
        seed::load_lua_data(b.get(), bytes);
        check(truth(b.get(), R"(
            game.data.coins == 42 and math.type(game.data.coins) == "integer" and
            game.data.speed == 2.5 and 1 / game.data.tiny < 0 and game.data.big == math.maxinteger and
            game.data.name == "Ayla\0with a zero" and game.data.done == true and game.data.lost == false and
            game.data.list[2] == "b" and game.data.list[3].deep.deeper == 7 and
            game.data[3] == "three" and game.data[1.5] == "one and a half" and game.data[true] == "yes" and
            next(game.data.empty) == nil and game.data.stale == nil)"),
              "Saved data round-trips and replaces game.data");
        check(save(b.get()) == bytes, "Loaded data saves the same bytes again");

        // What cannot be saved is left out and named; the rest is saved.
        auto c = fresh();
        run(c.get(), R"(
            game.data.kept = 1
            game.data.hello = print
            game.data.inner = {fn = function() end, ok = "yes"}
            game.data[{}] = "a table key"
        )");
        std::string skipped;
        const auto partial = seed::save_lua_data(c.get(), skipped);
        check(skipped.find("game.data.hello holds a function") != std::string::npos &&
                  skipped.find("game.data.inner.fn holds a function") != std::string::npos &&
                  skipped.find("game.data[table]") != std::string::npos,
              "Left-out values are named: " + skipped);
        auto d = fresh();
        seed::load_lua_data(d.get(), partial);
        check(
            truth(d.get(), "game.data.kept == 1 and game.data.inner.ok == 'yes' and game.data.hello == nil"),
            "The rest of game.data is saved");

        // Data that cannot be saved at all is an error, not a partial save.
        auto e = fresh();
        run(e.get(), "game.data.me = game.data");
        refused([&] { save(e.get()); }, "A table holding itself was saved");
        run(e.get(), "local t = game.data; game.data = {}; t = game.data; "
                     "for i = 1, 32 do t.next = {}; t = t.next end");
        refused([&] { save(e.get()); }, "Tables 33 deep were saved");
        run(e.get(), "game.data = {}; local t = game.data; for i = 1, 31 do t.next = {}; t = t.next end");
        save(e.get()); // 32 deep is the limit.
        run(e.get(), "game.data = string.rep('x', 10)");
        refused([&] { save(e.get()); }, "game.data that is not a table was saved");
        run(e.get(), "game.data = {big = string.rep('x', 5 * 1024 * 1024)}");
        refused([&] { save(e.get()); }, "game.data over 4 MiB was saved");

        // Broken saves are refused, and game.data stays as it was.
        auto f = fresh();
        run(f.get(), "game.data.mine = 'untouched'");
        const std::vector<std::vector<std::uint8_t>> broken{
            {},                          // Nothing.
            {5, 0, 0, 0, 0},             // Not a table.
            {6, 1, 0, 0},                // Truncated count.
            table({}, 1),                // Missing entry.
            table(key_a),                // Missing value.
            table({5, 9, 0, 0, 0, 'a'}), // String past the end.
            table({}, 0x7fffffff),       // Count beyond the bytes.
            [] {
                auto t = table({}, 0);
                t.push_back(0);
                return t;
            }(),                                         // Trailing byte.
            table({7, 1}),                               // Unknown tag.
            table({6, 0, 0, 0, 0, 1}),                   // A table as a key.
            table({4, 0, 0, 0, 0, 0, 0, 0xf8, 0x7f, 1}), // A NaN key.
            [] {
                auto entry = key_a;
                entry.push_back(1);
                entry.insert(entry.end(), key_a.begin(), key_a.end());
                entry.push_back(2);
                return table(entry, 2); // The key "a" twice.
            }(),
            [] {
                std::vector<std::uint8_t> t{6, 0, 0, 0, 0};
                for (int i = 0; i < 32; ++i) { // 33 tables deep.
                    auto outer = table(key_a);
                    outer.insert(outer.end(), t.begin(), t.end());
                    t = outer;
                }
                return t;
            }(),
        };
        for (std::size_t i = 0; i < broken.size(); ++i) {
            refused([&] { seed::load_lua_data(f.get(), broken[i]); },
                    "Broken saved game.data " + std::to_string(i) + " was accepted");
            check(truth(f.get(), "game.data.mine == 'untouched'"),
                  "A refused save changed game.data (" + std::to_string(i) + ")");
        }
        check(lua_gettop(f.get()) == 0, "Refused saves leave the Lua stack as it was");
        std::cout << "Saved game.data checks passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
