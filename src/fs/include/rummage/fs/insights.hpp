#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "rummage/core/worker_pool.hpp"

namespace rummage::fs {

struct InsightOptions {
    bool recursive{true};
    bool hidden{false};
    std::size_t max_entries{200'000};
    std::size_t max_name_bytes{32 * 1024 * 1024};
    std::size_t max_depth{64};
    std::chrono::milliseconds time_limit{30'000};
};

// Depth-first order; [id + 1, end) contains a folder's descendants. Names are stored once.
// Bytes are logical file sizes PER NAME, including hard links, not allocated disk space.
struct InsightNode {
    std::string name;
    std::size_t parent{0}, end{0};
    std::uint64_t files{0}, bytes{0};
    bool directory{false}, partial{false};
};

struct InsightProgress {
    std::uint64_t files{0}, bytes{0};
};

struct InsightSnapshot {
    std::filesystem::path root;
    std::vector<InsightNode> nodes;
    std::uint64_t unreadable{0}, skipped_folders{0}, links{0};
    bool limited{false}, cancelled{false}, remote{false};
    [[nodiscard]] bool complete() const;
    [[nodiscard]] std::filesystem::path relative_path(std::size_t node) const;
};

// Filename extensions, ASCII case folded. Dotfiles without another suffix and trailing dots
// have no extension. Common compound archives keep their full suffix. No content is opened.
[[nodiscard]] std::string insight_extension(std::string_view name);

// Workers only. Never follows links (including the root), crosses devices or scans remote roots.
// Progress is bounded to about 7 updates/second; cancellation returns a usable partial snapshot.
[[nodiscard]] InsightSnapshot scan_insights(
    const std::filesystem::path& root, const InsightOptions& options,
    const core::CancelToken& token, const std::function<void(InsightProgress)>& progress = {});

}  // namespace rummage::fs
