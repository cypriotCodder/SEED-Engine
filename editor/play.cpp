#include "play.hpp"
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace seed::editor {
PlaySession::~PlaySession() {
    if (!running()) return;
    stop();
    // The editor is closing: give the game a moment to save, then make sure it is gone.
    for (int i = 0; i < 30 && running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        poll();
    }
    if (running()) {
        kill(pid_, SIGKILL);
        waitpid(pid_, nullptr, 0);
    }
    if (output_ >= 0) close(output_);
}

void PlaySession::start(const std::filesystem::path& player, const std::vector<std::string>& arguments) {
    if (running()) throw std::logic_error("A game is already running");
    int pipe_ends[2];
    if (pipe(pipe_ends) != 0)
        throw std::runtime_error(std::string("Cannot create a pipe: ") + std::strerror(errno));
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    // The game's stdout and stderr both go to the pipe; the editor keeps only the read end.
    posix_spawn_file_actions_adddup2(&actions, pipe_ends[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipe_ends[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipe_ends[0]);
    posix_spawn_file_actions_addclose(&actions, pipe_ends[1]);
    const auto program = player.string();
    std::vector<char*> argv{const_cast<char*>(program.c_str())};
    for (const auto& argument : arguments)
        argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    pid_t pid{};
    const int result = posix_spawn(&pid, program.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipe_ends[1]);
    if (result != 0) {
        close(pipe_ends[0]);
        throw std::runtime_error("Cannot start " + program + ": " + std::strerror(result));
    }
    fcntl(pipe_ends[0], F_SETFL, fcntl(pipe_ends[0], F_GETFL) | O_NONBLOCK);
    pid_ = pid;
    output_ = pipe_ends[0];
    stopping_ = false;
    partial_.clear();
}

void PlaySession::stop() {
    if (!running() || stopping_) return;
    kill(pid_, SIGTERM); // SDL turns this into a quit event, so the game checkpoints and exits.
    stopping_ = true;
    stop_requested_ = std::chrono::steady_clock::now();
}

void PlaySession::drain(bool flush) {
    if (output_ < 0) return;
    char buffer[4096];
    ssize_t count;
    while ((count = read(output_, buffer, sizeof(buffer))) > 0)
        partial_.append(buffer, static_cast<std::size_t>(count));
    for (std::size_t end; (end = partial_.find('\n')) != std::string::npos; partial_.erase(0, end + 1)) {
        const auto line = partial_.substr(0, end);
        // SDL's own informational lines ("INFO: OpenGL context 4.1") are noise in the Console.
        if (line.find("INFO: ") != std::string::npos) continue;
        log_(line.find("error") != std::string::npos || line.find("Error") != std::string::npos,
             "Game: " + line);
    }
    if (flush && !partial_.empty()) {
        log_(false, "Game: " + partial_);
        partial_.clear();
    }
}

void PlaySession::poll() {
    if (!running()) return;
    drain(false);
    int status = 0;
    const pid_t done = waitpid(pid_, &status, WNOHANG);
    if (done == 0) {
        if (stopping_ && std::chrono::steady_clock::now() - stop_requested_ > std::chrono::seconds(3)) {
            log_(true, "The game did not stop within 3 seconds; ending it.");
            kill(pid_, SIGKILL);
        }
        return;
    }
    drain(true);
    close(output_);
    output_ = -1;
    pid_ = -1;
    last_exit_ = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    if (last_exit_ == 0 || stopping_)
        log_(false, "Game stopped.");
    else
        log_(true, "The game exited with code " + std::to_string(last_exit_) + ".");
    if (stopping_ && last_exit_ != 0 && WIFSIGNALED(status)) last_exit_ = 0; // Stopped on request.
}
} // namespace seed::editor
