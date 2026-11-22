#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace rummage::fs {
struct FilesystemFacts {
    std::filesystem::path mount_point;
    std::string type, device;
    std::uint64_t capacity{0}, free{0}, available{0};
    bool read_only{false};
    std::error_code error;
};

// Finds the most specific mount containing an absolute, resolved path. No filesystem access.
[[nodiscard]] FilesystemFacts filesystem_mount(std::string_view mountinfo,
                                               const std::filesystem::path& path);
// Live capacity and mount information, without walking directory contents. Blocking: workers only.
[[nodiscard]] FilesystemFacts filesystem_facts(const std::filesystem::path& path);
}  // namespace rummage::fs
