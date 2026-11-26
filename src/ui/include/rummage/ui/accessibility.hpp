#pragma once
#include <algorithm>
#include <cmath>

#include "rummage/core/accessibility.hpp"
#include "rummage/ui/geometry.hpp"

namespace rummage::ui {
// A semantic snapshot of the visible frame, independent of the retained GPU display lists.
// Only the event-loop thread records; inactive/offscreen rendering need not install a scope.
class AccessibleFrame {
public:
    explicit AccessibleFrame(bool enabled = true) : previous_{current_} {
        if (enabled)
            current_ = this;
    }

    ~AccessibleFrame() { current_ = previous_; }

    AccessibleFrame(const AccessibleFrame&) = delete;
    AccessibleFrame& operator=(const AccessibleFrame&) = delete;

    static AccessibleFrame* current() { return current_; }

    void add(core::AccessibleNode node) {
        if (nodes.size() >= 4096)
            return;
        if (node.parent.empty())
            node.parent = group;
        if (node.name.empty())
            node.name = last_label;
        nodes.push_back(std::move(node));
    }

    void dialog(std::string title, Rect rect) {
        nodes.clear();
        group = "dialog";
        core::AccessibleNode node;
        node.id = group;
        node.name = std::move(title);
        node.role = core::AccessibleRole::dialog;
        node.bounds = bounds(rect);
        nodes.push_back(std::move(node));
    }

    static core::AccessibleRect bounds(Rect rect) {
        return {static_cast<int>(std::floor(rect.x)), static_cast<int>(std::floor(rect.y)),
                static_cast<int>(std::ceil(rect.width)), static_cast<int>(std::ceil(rect.height))};
    }

    std::vector<core::AccessibleNode> nodes;
    std::string group, last_label;

private:
    inline static thread_local AccessibleFrame* current_{};
    AccessibleFrame* previous_{};
};
}  // namespace rummage::ui
