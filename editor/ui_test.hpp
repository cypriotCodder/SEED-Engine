#pragma once
#include <functional>
#include <imgui.h>
#include <string>
#include <vector>

namespace seed::editor {
class SceneEditor;

// Drives the editor with mouse and keyboard input and checks what happens, so automated runs
// cover the interactive editing that unit tests cannot: clicks, drags, box selection, handles and
// shortcuts. Input goes into ImGui's event queue exactly as real input would, after the platform
// backend's own events; the pointer position is re-sent every frame and the editor stops passing
// real input to ImGui during the test, so neither the real mouse nor focus changes interfere.
class UiTest final {
public:
    using Log = std::function<void(bool error, const std::string& text)>;
    UiTest(SceneEditor& scene, Log log);
    // Queues this frame's input. Call between the platform backend's NewFrame and ImGui::NewFrame.
    void frame();
    bool finished() const { return next_ >= steps_.size() || failed_; }
    bool passed() const { return finished() && !failed_; }

private:
    struct Step {
        int wait;                  // Frames to let pass before this step.
        std::function<void()> act; // Queues input or checks the result of earlier input.
    };
    void add(int wait, std::function<void()> act) { steps_.push_back({wait, std::move(act)}); }
    void check(bool condition, const std::string& what);
    ImVec2 at(const std::string& entity) const; // Screen position of a named entity.
    ImVec2 world_point(double x, double y) const;
    void click(std::function<ImVec2()> where, bool shift = false);
    // Drags from one point to another; both are worked out when the drag starts.
    void drag(std::function<ImVec2()> from, std::function<ImVec2()> to, bool shift = false);
    void drag(std::function<ImVec2()> from, ImVec2 by, bool shift = false);
    void key(ImGuiKey key, bool command = false, bool shift = false);
    void type(const char* text);
    ImVec2 row(int index, int column) const; // 0: name, 1: Show, 2: Lock.
    int index_of(const std::string& entity) const;
    double x_of(const std::string& entity) const;

    SceneEditor& scene_;
    Log log_;
    std::vector<Step> steps_;
    std::size_t next_{};
    int waited_{};
    ImVec2 pointer_{-1, -1}; // Re-sent every frame, so the real mouse never takes over.
    bool failed_{};
};
} // namespace seed::editor
