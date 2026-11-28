#pragma once
#include <memory>

#include "rummage/core/accessibility.hpp"
#include "rummage/core/event_loop.hpp"

namespace rummage::platform {
// Native AT-SPI bridge. Bus discovery and registration are asynchronous. All methods and
// immutable frame snapshots are handled on the UI event loop; no toolkit dependency.
class Accessibility {
public:
    explicit Accessibility(core::EventLoop& loop);
    ~Accessibility();
    Accessibility(const Accessibility&) = delete;
    Accessibility& operator=(const Accessibility&) = delete;
    [[nodiscard]] bool active() const;
    // Called immediately when registration has already completed, otherwise when it completes.
    void set_ready_handler(std::function<void()> handler);
    void update_window(std::string id, std::string title, std::vector<core::AccessibleNode> nodes,
                       bool focused = false);
    void remove_window(const std::string& id);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace rummage::platform
