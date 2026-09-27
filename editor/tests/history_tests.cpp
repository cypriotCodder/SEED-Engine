#include "history.hpp"
#include <iostream>
#include <stdexcept>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
} // namespace

int main() {
    try {
        seed::editor::History<int> history(3);
        history.reset(0);
        check(!history.can_undo() && !history.can_redo(), "Fresh history has nothing to step");
        history.offer(0);
        check(!history.can_undo(), "An unchanged state is not recorded");
        history.offer(1);
        history.offer(2);
        check(history.undo() == 1 && history.undo() == 0 && !history.can_undo(), "Undo walks back");
        check(history.undo() == 0, "Undo at the start stays put");
        check(history.redo() == 1, "Redo walks forward");
        history.offer(5);
        check(!history.can_redo(), "A new edit discards the undone states");
        check(history.undo() == 1 && history.undo() == 0, "History kept up to the new edit");
        history.redo();
        history.redo();
        history.offer(6);
        history.offer(7); // Limit 3: the oldest states go.
        check(history.undo() == 6 && history.undo() == 5 && !history.can_undo(), "Oldest states dropped");
        std::cout << "Undo history checks passed.\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
