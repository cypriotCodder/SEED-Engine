#include "script/saved_data.hpp"
#include "io/binary.hpp"
// Lua is compiled as C++ (see CMakeLists.txt), so its errors are C++ exceptions.
#include "lauxlib.h"
#include "lua.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace seed {
namespace {
enum : std::uint8_t { saved_false = 1, saved_true, saved_integer, saved_number, saved_string, saved_table };
static_assert(sizeof(lua_Number) == 8 && sizeof(lua_Integer) == 8);

// Writes the boolean, number or string at `index`; false for any other value.
bool save_plain(lua_State* lua, int index, Bytes& out) {
    switch (lua_type(lua, index)) {
    case LUA_TBOOLEAN:
        out.u8(lua_toboolean(lua, index) ? saved_true : saved_false);
        return true;
    case LUA_TNUMBER:
        if (lua_isinteger(lua, index)) {
            out.u8(saved_integer);
            out.u64(static_cast<std::uint64_t>(lua_tointeger(lua, index)));
        } else {
            out.u8(saved_number);
            out.u64(std::bit_cast<std::uint64_t>(lua_tonumber(lua, index)));
        }
        return true;
    case LUA_TSTRING: {
        std::size_t size{};
        const char* text = lua_tolstring(lua, index, &size);
        if (size > saved_data_capacity) luaL_error(lua, "a string in game.data is too long to save");
        out.u8(saved_string);
        out.u32(static_cast<std::uint32_t>(size));
        out.data.insert(out.data.end(), text, text + size);
        return true;
    }
    default:
        return false;
    }
}

// How a key reads in a message: .name, [3] or [table].
std::string key_text(lua_State* lua, int index) {
    if (lua_type(lua, index) == LUA_TSTRING) return std::string(".") + lua_tostring(lua, index);
    if (lua_isinteger(lua, index)) return "[" + std::to_string(lua_tointeger(lua, index)) + "]";
    if (lua_type(lua, index) == LUA_TNUMBER) return "[" + std::to_string(lua_tonumber(lua, index)) + "]";
    return std::string("[") + luaL_typename(lua, index) + "]";
}

// Writes the table at `index`, named `path` in messages.
void save_table(lua_State* lua, int index, Bytes& out, int depth, const std::string& path,
                std::string& skipped) {
    if (depth > saved_data_depth)
        luaL_error(lua, "%s nests tables more than %d deep, or holds itself", path.c_str(), saved_data_depth);
    luaL_checkstack(lua, 4, "game.data nests too deep");
    index = lua_absindex(lua, index);
    std::vector<std::pair<Bytes, Bytes>> entries;
    std::size_t size = 0;
    lua_pushnil(lua);
    while (lua_next(lua, index)) { // Raw: metatables play no part.
        Bytes key, value;
        bool saved = save_plain(lua, -2, key);
        if (saved && lua_istable(lua, -1))
            save_table(lua, -1, value, depth + 1, path + key_text(lua, -2), skipped);
        else if (saved)
            saved = save_plain(lua, -1, value);
        if (saved) {
            size += key.data.size() + value.data.size();
            if (size > saved_data_capacity) luaL_error(lua, "game.data is larger than 4 MiB");
            entries.emplace_back(std::move(key), std::move(value));
        } else
            skipped += path + key_text(lua, -2) + " holds a " + luaL_typename(lua, -1) + "\n";
        lua_pop(lua, 1);
    }
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.first.data < b.first.data; });
    out.u8(saved_table);
    out.u32(static_cast<std::uint32_t>(entries.size()));
    for (const auto& [key, value] : entries) {
        out.data.insert(out.data.end(), key.data.begin(), key.data.end());
        out.data.insert(out.data.end(), value.data.begin(), value.data.end());
    }
}

// Pushes one saved value; a key may not be a table or NaN.
void load_value(lua_State* lua, Reader& in, int depth, bool key) {
    luaL_checkstack(lua, 4, "game.data nests too deep");
    const auto tag = in.u8();
    switch (tag) {
    case saved_false:
    case saved_true:
        lua_pushboolean(lua, tag == saved_true);
        return;
    case saved_integer:
        lua_pushinteger(lua, std::bit_cast<lua_Integer>(in.u64()));
        return;
    case saved_number: {
        const auto value = std::bit_cast<lua_Number>(in.u64());
        if (key && std::isnan(value)) throw std::runtime_error("Saved game.data has a NaN key");
        lua_pushnumber(lua, value);
        return;
    }
    case saved_string: {
        const auto text = in.take(in.u32());
        lua_pushlstring(lua, reinterpret_cast<const char*>(text.data()), text.size());
        return;
    }
    case saved_table:
        if (!key && depth <= saved_data_depth) break;
        [[fallthrough]];
    default:
        throw std::runtime_error("Saved game.data has an invalid value");
    }
    // Every entry takes at least two bytes, which bounds the count before anything is made.
    const auto count = in.u32();
    if (count > in.remaining() / 2) throw std::runtime_error("Saved game.data is truncated");
    lua_createtable(lua, 0, static_cast<int>(std::min<std::uint32_t>(count, 1024)));
    for (std::uint32_t i = 0; i < count; ++i) {
        load_value(lua, in, depth + 1, true);
        lua_pushvalue(lua, -1);
        if (lua_rawget(lua, -3) != LUA_TNIL) throw std::runtime_error("Saved game.data repeats a key");
        lua_pop(lua, 1);
        load_value(lua, in, depth + 1, false);
        lua_rawset(lua, -3);
    }
}

struct Transfer {
    Bytes out;
    std::string* skipped{};
    std::span<const std::uint8_t> in;
};

// Engine errors become Lua errors, so both leave the protected call the same way.
template<int (*F)(lua_State*)>
int guarded(lua_State* lua) {
    try {
        return F(lua);
    } catch (const std::exception& error) {
        return luaL_error(lua, "%s", error.what());
    }
}
int save(lua_State* lua) {
    auto& transfer = *static_cast<Transfer*>(lua_touserdata(lua, 1));
    lua_getglobal(lua, "game");
    if (!lua_istable(lua, -1) || lua_getfield(lua, -1, "data") != LUA_TTABLE)
        luaL_error(lua, "game.data must be a table to be saved");
    save_table(lua, -1, transfer.out, 1, "game.data", *transfer.skipped);
    return 0;
}
int load(lua_State* lua) {
    auto& transfer = *static_cast<Transfer*>(lua_touserdata(lua, 1));
    if (transfer.in.empty() || transfer.in[0] != saved_table)
        throw std::runtime_error("Saved game.data is not a table");
    Reader in(transfer.in);
    load_value(lua, in, 1, false);
    if (!in.done()) throw std::runtime_error("Trailing saved game.data");
    // Only now, with every value read, does the saved table replace game.data.
    if (lua_getglobal(lua, "game") != LUA_TTABLE) luaL_error(lua, "there is no game table to load into");
    lua_insert(lua, -2);
    lua_setfield(lua, -2, "data");
    return 0;
}
void run(lua_State* lua, lua_CFunction function, Transfer& transfer, const char* fallback) {
    const int top = lua_gettop(lua);
    lua_pushcfunction(lua, function);
    lua_pushlightuserdata(lua, &transfer);
    if (lua_pcall(lua, 1, 0, 0) != LUA_OK) {
        const char* text = lua_tostring(lua, -1);
        std::string message = text ? text : fallback;
        lua_settop(lua, top);
        throw std::runtime_error(message);
    }
}
} // namespace

std::vector<std::uint8_t> save_lua_data(lua_State* lua, std::string& skipped) {
    Transfer transfer;
    transfer.skipped = &skipped;
    run(lua, guarded<save>, transfer, "game.data could not be saved");
    return std::move(transfer.out.data);
}

void load_lua_data(lua_State* lua, std::span<const std::uint8_t> bytes) {
    Transfer transfer;
    transfer.in = bytes;
    run(lua, guarded<load>, transfer, "game.data could not be loaded");
}
} // namespace seed
