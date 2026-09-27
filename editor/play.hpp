#pragma once
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace seed::editor {
// A running game launched by the editor's Play button: seed_player in its own process and window,
// so a crashing game never takes the editor down. Its output goes to the editor's log.
class PlaySession final {
public:
    using Log = std::function<void(bool error, const std::string& text)>;
    explicit PlaySession(Log log) : log_(std::move(log)) {}
    ~PlaySession();
    PlaySession(const PlaySession&) = delete;
    PlaySession& operator=(const PlaySession&) = delete;

    bool running() const { return pid_ > 0; }
    // Starts `player` with `arguments`. Throws if it cannot be started.
    void start(const std::filesystem::path& player, const std::vector<std::string>& arguments);
    // Asks the game to quit as if its window were closed; it is killed if still running 3 s later.
    void stop();
    // Forwards new output and notices when the game has exited. Call once per frame.
    void poll();
    // Exit code of the last finished game; nonzero also when it was killed by a signal.
    int last_exit() const { return last_exit_; }

private:
    void drain(bool flush);
    Log log_;
    int pid_{-1}, output_{-1};
    std::string partial_;
    int last_exit_{};
    bool stopping_{};
    std::chrono::steady_clock::time_point stop_requested_{};
};
} // namespace seed::editor
