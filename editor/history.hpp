#pragma once
#include <cstddef>
#include <deque>

namespace seed::editor {
// Undo and redo by snapshots. After each finished edit the editor offers the current state; a
// state equal to the present one is ignored, so any panel's edits become undoable without
// per-widget undo code. The oldest states are dropped beyond `limit`.
template<class State>
class History final {
public:
    explicit History(std::size_t limit = 100) : limit_(limit) {}
    // Forgets everything and starts from `state`.
    void reset(State state) {
        states_.clear();
        states_.push_back(std::move(state));
        at_ = 0;
    }
    // Records `state` if it differs from the present one, discarding anything that was undone.
    void offer(const State& state) {
        if (!states_.empty() && states_[at_] == state) return;
        states_.erase(states_.begin() + static_cast<std::ptrdiff_t>(at_ + 1), states_.end());
        states_.push_back(state);
        if (states_.size() > limit_) states_.pop_front();
        at_ = states_.size() - 1;
    }
    bool can_undo() const { return at_ > 0; }
    bool can_redo() const { return at_ + 1 < states_.size(); }
    const State& undo() { return states_[can_undo() ? --at_ : at_]; }
    const State& redo() { return states_[can_redo() ? ++at_ : at_]; }

private:
    std::deque<State> states_;
    std::size_t at_{};
    std::size_t limit_;
};
} // namespace seed::editor
