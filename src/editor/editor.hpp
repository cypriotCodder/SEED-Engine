#pragma once
#include "platform/window.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace seed {
class Engine;
struct Game;

// The in-engine editor, drawn with Dear ImGui over the game. It starts closed unless --editor is
// given, and the engine's "editor" action (F1) toggles it. While it is open, input it uses (a
// click on a panel, typing in a field) is hidden from the game.
class Editor final {
public:
    Editor(Window& window, bool open);
    ~Editor();
    Editor(const Editor&) = delete;
    Editor& operator=(const Editor&) = delete;

    bool is_open() const { return open_; }
    void toggle() { open_ = !open_; }
    // Starts an editor frame when open. Call once per frame after polling input.
    void begin();
    // Whether the frame begun last wants the mouse or keyboard for itself.
    bool captures_mouse() const;
    bool captures_keyboard() const;
    // Draws the engine's panels and the game's, then renders them over the current framebuffer.
    void finish(Engine& engine, const Game& game);
    // Ends a begun frame without drawing, e.g. while the window is minimised.
    void cancel();
    // Figures sampled by the engine while physics was idle; physics may be stepping on a worker
    // while the editor draws, so the editor never reads it directly.
    struct Sample {
        float seconds{};
        std::size_t bodies{};
        std::uint64_t physics_nanoseconds{};
    };
    void record(const Sample& sample);

private:
    static void observe(void* context, const SDL_Event& event);
    void stats(Engine& engine);
    bool open_;
    bool started_{};
    std::array<float, 240> frame_ms_{};
    Sample last_{};
    std::size_t frame_next_{}, frame_count_{};
};
} // namespace seed
