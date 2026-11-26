#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rummage::fs {
struct RecentFile {
    std::filesystem::path path;
    std::int64_t opened{0};
};

// Rummage-only history. All disk I/O belongs on a worker. Paths are local/FUSE paths;
// the collection is capped at 200 entries and does not scan unrelated folders.
class RecentFiles {
public:
    explicit RecentFiles(std::filesystem::path directory = default_directory());
    static std::filesystem::path default_directory();
    [[nodiscard]] std::vector<RecentFile> load() const;
    [[nodiscard]] bool enabled() const;
    [[nodiscard]] std::string record(const std::vector<std::filesystem::path>& paths) const;
    [[nodiscard]] std::string clear() const;
    [[nodiscard]] std::string set_enabled(bool enabled) const;

private:
    std::filesystem::path directory_;
};
}  // namespace rummage::fs
