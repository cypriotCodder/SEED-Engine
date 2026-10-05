#pragma once
#include <algorithm>
#include <imgui.h>
#include <string>
#include <vector>

// Small ImGui helpers shared by the editor's panels.
namespace seed::editor {
// Continue a toolbar only when the next control fits in the available content width.
inline void toolbar_next(float width) {
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width <= right) ImGui::SameLine();
}

inline float button_width(const char* label) {
    return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2;
}

template<class T>
inline std::string unique_name(const std::vector<T>& items, const char* base) {
    for (int n = 1;; ++n) {
        const auto name = n == 1 ? std::string(base) : std::string(base) + "_" + std::to_string(n);
        if (std::none_of(items.begin(), items.end(), [&](const T& item) { return item.name == name; }))
            return name;
    }
}

// The list half of a panel: add, delete and reorder buttons above selectable names. Returns true
// when the list changed. `order_note` explains what reordering changes, when it matters.
template<class T>
inline bool list(std::vector<T>& items, int& selected, const char* base, std::size_t capacity,
                 const char* order_note, bool compact = false) {
    bool changed = false;
    if (selected >= static_cast<int>(items.size())) selected = static_cast<int>(items.size()) - 1;
    ImGui::BeginDisabled(items.size() >= capacity);
    if (ImGui::SmallButton("Add")) {
        T item;
        item.name = unique_name(items, base);
        items.insert(items.begin() + (selected + 1), item);
        ++selected;
        changed = true;
    }
    ImGui::EndDisabled();
    toolbar_next(button_width("Delete"));
    ImGui::BeginDisabled(selected < 0);
    if (ImGui::SmallButton("Delete")) {
        items.erase(items.begin() + selected);
        selected = std::min(selected, static_cast<int>(items.size()) - 1);
        changed = true;
    }
    toolbar_next(ImGui::GetFrameHeight());
    ImGui::BeginDisabled(selected <= 0);
    if (ImGui::ArrowButton("up", ImGuiDir_Up)) {
        std::swap(items[selected], items[selected - 1]);
        --selected;
        changed = true;
    }
    ImGui::EndDisabled();
    toolbar_next(ImGui::GetFrameHeight());
    ImGui::BeginDisabled(selected < 0 || selected + 1 >= static_cast<int>(items.size()));
    if (ImGui::ArrowButton("down", ImGuiDir_Down)) {
        std::swap(items[selected], items[selected + 1]);
        ++selected;
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (order_note) ImGui::SetItemTooltip("%s", order_note);
    if (compact) {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##items", selected >= 0
                                             ? items[static_cast<std::size_t>(selected)].name.c_str()
                                             : "Select an item")) {
            for (int i = 0; i < static_cast<int>(items.size()); ++i) {
                ImGui::PushID(i);
                if (ImGui::Selectable(items[static_cast<std::size_t>(i)].name.c_str(), selected == i))
                    selected = i;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        return changed;
    }
    ImGui::BeginChild("items", {0, 0}, ImGuiChildFlags_Borders);
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        ImGui::PushID(i);
        if (ImGui::Selectable(items[i].name.empty() ? "(unnamed)" : items[i].name.c_str(), selected == i))
            selected = i;
        ImGui::PopID();
    }
    if (items.empty()) ImGui::TextDisabled("None yet. Press Add.");
    ImGui::EndChild();
    return changed;
}

} // namespace seed::editor
