#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "rummage/core/worker_pool.hpp"

namespace rummage::fs {
struct PathMatches {
    std::vector<std::string> paths;
    std::string error;
};

// Blocking; local paths and archive folders only. Does not mount remote locations.
PathMatches complete_path(const std::string& text, const std::filesystem::path& current,
                          const std::filesystem::path& home, const core::CancelToken& token);

struct PathTarget {
    std::filesystem::path folder;
    std::string file;
    std::string error;
};

PathTarget resolve_path(const std::filesystem::path& path);
}  // namespace rummage::fs
