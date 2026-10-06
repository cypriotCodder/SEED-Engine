#include "platform/actions.hpp"
#include <iostream>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Exception, class F>
void throws(F&& f, const char* message) {
    try {
        f();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(message);
}
} // namespace

int main() {
    try {
        using seed::Binding;
        seed::Actions actions;
        const auto jump =
            actions.add("jump", {Binding::key(SDL_SCANCODE_SPACE), Binding::key(SDL_SCANCODE_W)});
        const auto fire = actions.add("fire", {Binding::mouse(SDL_BUTTON_LEFT)});
        const auto left = actions.add("left", {Binding::key(SDL_SCANCODE_A)});
        const auto right = actions.add("right", {Binding::key(SDL_SCANCODE_D)});
        check(actions.find("fire") == fire, "Find by name");
        throws<std::invalid_argument>([&] { actions.add("jump", {}); }, "Duplicate action accepted");
        throws<std::invalid_argument>([&] { actions.bind(jump, {Binding::mouse(9)}); },
                                      "Invalid binding accepted");
        throws<std::out_of_range>([&] { (void)actions.held(99); }, "Unregistered action accepted");

        // A key press is pressed and held in its first frame, held afterwards, released on key-up.
        seed::Input input;
        input.held[SDL_SCANCODE_W] = input.pressed[SDL_SCANCODE_W] = true;
        input.held[SDL_SCANCODE_D] = true;
        actions.update(input);
        check(actions.pressed(jump) && actions.held(jump) && !actions.released(jump),
              "Key press through a second binding");
        check(actions.axis(left, right) == 1, "Axis from opposing actions");
        input.pressed.fill(false);
        actions.update(input);
        check(!actions.pressed(jump) && actions.held(jump), "Held after the first frame");
        input.held.fill(false);
        input.released[SDL_SCANCODE_W] = true;
        actions.update(input);
        check(actions.released(jump) && !actions.held(jump), "Released on key-up");

        // Mouse buttons produce edges from frame to frame.
        seed::Input mouse;
        mouse.mouse_buttons = SDL_BUTTON(SDL_BUTTON_LEFT);
        actions.update(mouse);
        check(actions.pressed(fire) && actions.held(fire), "Mouse press edge");
        actions.update(mouse);
        check(!actions.pressed(fire) && actions.held(fire), "Mouse held without a new edge");
        mouse.mouse_buttons = 0;
        actions.update(mouse);
        check(actions.released(fire) && !actions.held(fire), "Mouse release edge");

        // Rebinding changes which key drives the action.
        actions.bind(jump, {Binding::key(SDL_SCANCODE_K)});
        seed::Input rebound;
        rebound.held[SDL_SCANCODE_SPACE] = true;
        actions.update(rebound);
        check(!actions.held(jump), "Old binding still active after rebinding");
        rebound.held[SDL_SCANCODE_K] = true;
        actions.update(rebound);
        check(actions.held(jump), "New binding inactive");
        // Replays hold actions as a key would, alongside real input.
        seed::Input idle;
        actions.set_scripted(jump, true);
        actions.update(idle);
        check(actions.pressed(jump) && actions.held(jump), "Scripted press edge");
        actions.update(idle);
        check(!actions.pressed(jump) && actions.held(jump), "Scripted hold");
        actions.set_scripted(jump, false);
        actions.update(idle);
        check(actions.released(jump) && !actions.held(jump), "Scripted release edge");
        actions.update(rebound);
        check(actions.held(jump) && !actions.released(jump), "Keys still work after a replay lets go");
        std::cout << "Input action checks passed.\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
