#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

struct lua_State;

namespace seed {
// game.data in a save, version 1. Each value is a u8 tag and its bytes: false (1), true (2), a
// 64-bit integer (3), a binary64 number (4), a string (5: u32 length, bytes) or a table (6: u32
// entry count, then each key and value). Keys are booleans, numbers (never NaN) and strings; a
// table's entries are sorted by their key bytes, so the same data always saves the same bytes.
inline constexpr std::size_t saved_data_capacity = 4 * 1024 * 1024 - 1024;
inline constexpr int saved_data_depth = 32; // Tables within tables, game.data itself the first.

// Encodes the table at `game.data` in `lua`'s globals. Values that cannot be saved (functions,
// entities, tables as keys) are left out, each named on its own line in `skipped`. Throws if
// game.data is not a table, nests deeper than saved_data_depth (or holds itself) or encodes to
// more than saved_data_capacity bytes. Runs protected, so running out of script memory throws too.
std::vector<std::uint8_t> save_lua_data(lua_State* lua, std::string& skipped);
// Replaces game.data with what save_lua_data wrote. Throws, changing nothing, for invalid bytes.
void load_lua_data(lua_State* lua, std::span<const std::uint8_t> bytes);
} // namespace seed
