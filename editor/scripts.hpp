#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace seed::editor {
// The project's Lua scripts, as the Inspector offers them.
class Scripts final {
public:
    // The .lua files in `folder`, sorted.
    static std::vector<std::string> list(const std::filesystem::path& folder);
    // Creates `folder/name` from the starter template. Throws if it exists or the name is invalid.
    static void create(const std::filesystem::path& folder, const std::string& name);
    // The file's syntax error ("player.lua:3: unexpected symbol near '='"), or empty. Results are
    // cached until the file changes, so calling it every frame is cheap.
    std::string syntax_error(const std::filesystem::path& file);

private:
    struct Checked {
        std::filesystem::file_time_type modified;
        std::string error;
    };
    std::map<std::filesystem::path, Checked> cache_;
};
} // namespace seed::editor
