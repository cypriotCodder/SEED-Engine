#pragma once
#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace seed::editor {
enum class LogLevel { info, warning, error };
struct LogLine {
    LogLevel level;
    std::string time, text;
};
struct ConsoleRow {
    std::size_t index, count;
};
inline bool contains_text(std::string_view text, std::string_view query) {
    return std::search(text.begin(), text.end(), query.begin(), query.end(),
                       [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); }) !=
               text.end() ||
           query.empty();
}
// Filter before grouping so the displayed count and copied output describe the same messages.
inline std::vector<ConsoleRow> console_rows(const std::vector<LogLine>& lines,
                                            const std::array<bool, 3>& levels, std::string_view query,
                                            bool group) {
    std::vector<ConsoleRow> rows;
    std::map<std::pair<LogLevel, std::string>, std::size_t> groups;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto& line = lines[i];
        if (!levels[static_cast<std::size_t>(line.level)] || !contains_text(line.text, query)) continue;
        if (group) {
            const auto [it, added] = groups.emplace(std::make_pair(line.level, line.text), rows.size());
            if (!added) {
                ++rows[it->second].count;
                continue;
            }
        }
        rows.push_back({i, 1});
    }
    return rows;
}
inline const char* level_name(LogLevel level) {
    return level == LogLevel::error ? "Error" : level == LogLevel::warning ? "Warning" : "Info";
}
inline std::string console_text(const LogLine& line, std::size_t count = 1) {
    return line.time + " [" + level_name(line.level) + "] " + line.text +
           (count > 1 ? " (x" + std::to_string(count) + ")" : "");
}
} // namespace seed::editor
