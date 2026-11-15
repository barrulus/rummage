#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "rummage/core/event_loop.hpp"

namespace rummage::platform {

// org.freedesktop.FileManager1: browsers' and other applications' "Show in Folder".
enum class Show : std::uint8_t { folders, items, properties };

// Only syntactically valid local file URIs are passed to rummage. Filesystem checks run
// in the child, so a stalled mount cannot block this service. Empty means no child.
[[nodiscard]] std::vector<std::string> show_arguments(Show show,
                                                      const std::vector<std::string>& uris);

struct FileManagerOptions {
    // Empty resolves /proc/self/exe, preserving installed wrappers.
    std::filesystem::path program;
    std::chrono::milliseconds idle_timeout{30'000};
};

// A separate headless session-bus service. Each method replies before starting a window,
// passes startup_id as its activation token, and leaves the FileChooser backend untouched.
int run_file_manager(core::EventLoop& loop, FileManagerOptions options = {});

}  // namespace rummage::platform
