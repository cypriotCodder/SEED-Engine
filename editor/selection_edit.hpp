#pragma once
#include "project/scene_file.hpp"
#include <span>
#include <vector>

namespace seed::editor {
// Only the supplied field is touched. Missing components are deliberately skipped.
template<class Getter, class Value>
void set_selection_field(std::span<SceneEntity*> entities, Getter get, const Value& value) {
    for (auto* entity : entities)
        if (auto* field = get(*entity)) *field = value;
}
template<class Getter>
bool selection_mixed(std::span<SceneEntity*> entities, Getter get) {
    if (entities.empty()) return false;
    const auto* first = get(*entities.front());
    for (auto* entity : entities) {
        const auto* value = get(*entity);
        if (first && value && *first != *value) return true;
        if (!first && value) first = value;
    }
    return false;
}
inline void set_selection_position(std::span<SceneEntity*> entities, int axis, double value, bool absolute) {
    if (entities.empty()) return;
    const auto target = axis == 0 ? from_global(value, 0) : from_global(0, value);
    const auto& primary = entities.front()->position;
    auto desired = primary;
    if (axis == 0) {
        desired.chunk.x = target.chunk.x;
        desired.local.x = target.local.x;
    } else {
        desired.chunk.y = target.chunk.y;
        desired.local.y = target.local.y;
    }
    const auto offset = absolute ? Vec2{} : relative(desired, primary);
    std::vector<WorldPosition> positions;
    positions.reserve(entities.size());
    for (auto* entity : entities) {
        auto position = entity->position;
        if (!absolute)
            position.move(offset);
        else if (axis == 0) {
            position.chunk.x = target.chunk.x;
            position.local.x = target.local.x;
        } else {
            position.chunk.y = target.chunk.y;
            position.local.y = target.local.y;
        }
        positions.push_back(position);
    }
    for (std::size_t i = 0; i < entities.size(); ++i)
        entities[i]->position = positions[i];
}
} // namespace seed::editor
