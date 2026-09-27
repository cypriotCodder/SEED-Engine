#include "scripts.hpp"
#include "io/storage.hpp"
#include "project/scene_file.hpp"
// Lua is built as C++, so its headers are included without extern "C".
#include "lauxlib.h"
#include "lua.h"
#include <algorithm>

namespace seed::editor {
namespace fs = std::filesystem;

std::vector<std::string> Scripts::list(const fs::path& folder) {
    std::vector<std::string> names;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(folder, error))
        if (entry.path().extension() == ".lua") names.push_back(entry.path().filename().string());
    std::sort(names.begin(), names.end());
    return names;
}

void Scripts::create(const fs::path& folder, const std::string& name) {
    if (!valid_script_name(name))
        throw std::invalid_argument("Script names use letters, digits, '_', '-' and '.', and end in .lua");
    const auto file = folder / name;
    if (fs::exists(file)) throw std::runtime_error(name + " already exists");
    fs::create_directories(folder);
    write_text(file, "-- " + name + R"(: attached to an entity, which is `self` here.
-- start() runs once before the first update; update(dt) runs every step, dt in seconds.
-- The API (input, world, sound, particles, camera, game) is described in the engine's
-- docs/scripting.md.

function start()
end

function update(dt)
end
)");
}

std::string Scripts::syntax_error(const fs::path& file) {
    std::error_code error;
    const auto modified = fs::last_write_time(file, error);
    if (error) return "File not found in scripts/";
    if (const auto found = cache_.find(file); found != cache_.end() && found->second.modified == modified)
        return found->second.error;
    std::string message;
    try {
        const auto source = read_text(file, 1024 * 1024);
        lua_State* lua = luaL_newstate();
        const auto chunk = "@" + file.filename().string();
        if (luaL_loadbufferx(lua, source.data(), source.size(), chunk.c_str(), "t") != LUA_OK)
            message = lua_tostring(lua, -1);
        lua_close(lua);
    } catch (const std::exception& failure) {
        message = failure.what();
    }
    cache_[file] = {modified, message};
    return message;
}
} // namespace seed::editor
