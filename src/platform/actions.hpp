#pragma once
#include "platform/window.hpp"
#include <array>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>

namespace seed {
// One physical input an action can be bound to: a keyboard key or a mouse button (1 = left,
// 2 = middle, 3 = right, as SDL numbers them).
struct Binding {
    enum class Kind : std::uint8_t { none, key, mouse };
    Kind kind{Kind::none};
    int code{};
    static Binding key(SDL_Scancode scancode) { return {Kind::key, static_cast<int>(scancode)}; }
    static Binding mouse(int button) { return {Kind::mouse, button}; }
};

using ActionId = std::uint8_t;

// Named game actions ("jump", "build") mapped to physical inputs. Games read actions, never raw
// keys, so bindings can change without touching game code. Updated once per frame after polling.
class Actions final {
public:
    static constexpr std::size_t capacity = 64, bindings_per_action = 4;

    ActionId add(const char* name, std::initializer_list<Binding> bindings) {
        if (!name || !*name) throw std::invalid_argument("An action needs a name");
        for (std::size_t i = 0; i < size_; ++i)
            if (std::string_view(actions_[i].name) == name)
                throw std::invalid_argument(std::string("Duplicate action: ") + name);
        if (size_ == capacity) throw std::length_error("Too many actions");
        actions_[size_].name = name;
        const auto id = static_cast<ActionId>(size_++);
        bind(id, bindings);
        return id;
    }
    // Replaces every binding of an action. Invalid bindings are rejected before anything changes.
    void bind(ActionId id, std::initializer_list<Binding> bindings) {
        auto& action = at(id);
        if (bindings.size() > bindings_per_action)
            throw std::invalid_argument("Too many bindings for one action");
        for (const auto& binding : bindings) {
            const bool valid =
                (binding.kind == Binding::Kind::key && binding.code > 0 &&
                 binding.code < SDL_NUM_SCANCODES) ||
                (binding.kind == Binding::Kind::mouse && binding.code >= 1 && binding.code <= 5);
            if (!valid) throw std::invalid_argument("Invalid input binding");
        }
        action.bindings = {};
        std::size_t i = 0;
        for (const auto& binding : bindings)
            action.bindings[i++] = binding;
    }
    ActionId find(std::string_view name) const {
        for (std::size_t i = 0; i < size_; ++i)
            if (actions_[i].name == name) return static_cast<ActionId>(i);
        throw std::out_of_range("Unknown action: " + std::string(name));
    }

    // Recomputes every action from this frame's raw input.
    void update(const Input& input) {
        for (std::size_t i = 0; i < size_; ++i) {
            auto& action = actions_[i];
            bool held = false, pressed = false, released = false;
            for (const auto& binding : action.bindings) {
                if (binding.kind == Binding::Kind::key) {
                    const auto key = static_cast<std::size_t>(binding.code);
                    held |= input.held[key];
                    pressed |= input.pressed[key];
                    released |= input.released[key];
                } else if (binding.kind == Binding::Kind::mouse) {
                    const auto mask = SDL_BUTTON(binding.code);
                    const bool down = input.mouse_buttons & mask, was = previous_mouse_ & mask;
                    held |= down;
                    pressed |= down && !was;
                    released |= !down && was;
                }
            }
            action.held = held;
            action.pressed = pressed;
            action.released = released;
        }
        previous_mouse_ = input.mouse_buttons;
    }
    bool held(ActionId id) const { return at(id).held; }
    bool pressed(ActionId id) const { return at(id).pressed; }
    bool released(ActionId id) const { return at(id).released; }
    // -1, 0 or 1 from a pair of opposing actions, e.g. left/right.
    float axis(ActionId negative, ActionId positive) const {
        return static_cast<float>(held(positive)) - static_cast<float>(held(negative));
    }
    std::size_t size() const { return size_; }

private:
    struct Action {
        const char* name{};
        std::array<Binding, bindings_per_action> bindings{};
        bool held{}, pressed{}, released{};
    };
    Action& at(ActionId id) {
        if (id >= size_) throw std::out_of_range("Unregistered action");
        return actions_[id];
    }
    const Action& at(ActionId id) const {
        if (id >= size_) throw std::out_of_range("Unregistered action");
        return actions_[id];
    }
    std::array<Action, capacity> actions_{};
    std::size_t size_{};
    Uint32 previous_mouse_{};
};

// Actions the engine itself responds to, registered before the game's. Games may rebind them.
namespace engine_action {
constexpr ActionId quit = 0;       // Escape: checkpoint and quit.
constexpr ActionId checkpoint = 1; // F5: start a background checkpoint.
constexpr ActionId screenshot = 2; // F12: write a frame when --screenshot was given.
constexpr ActionId editor = 3;     // F1: open or close the editor.
} // namespace engine_action
} // namespace seed
