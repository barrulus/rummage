#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "rummage/core/event_loop.hpp"
#include "rummage/platform/file_manager.hpp"

namespace rummage::platform {

struct BrowserRequest {
    Show show{Show::folders};
    std::vector<std::string> paths;
    std::string token;
    bool new_window{};
};

// Syntactic checks only: never touches mounts or resolves symlinks on the bus thread.
[[nodiscard]] bool valid_browser_request(const BrowserRequest& request);
[[nodiscard]] std::string browser_instance_name(std::string_view config, std::string_view display);
enum class BrowserForward { unavailable, accepted, rejected };
[[nodiscard]] BrowserForward forward_browser_request(const std::string& name,
                                                     const BrowserRequest& request);

// Private per-display, per-config endpoint. D-Bus releases ownership when the process exits.
// All accepted requests are delivered on a later loop iteration. Destroy before the loop.
class BrowserInstance {
public:
    BrowserInstance(core::EventLoop& loop, const std::string& name,
                    std::function<void(BrowserRequest)> open);
    ~BrowserInstance();
    BrowserInstance(const BrowserInstance&) = delete;
    BrowserInstance& operator=(const BrowserInstance&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rummage::platform
