#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include "rummage/core/worker_pool.hpp"

namespace rummage::fs {
inline constexpr char kArchiveOrigin[] = "user.rummage.archive-origin";
// Bounded provenance. Failure to persist provenance removes all execute bits; failure to do
// either is fatal. Call AFTER restoring the archived/copied mode, before publishing a file.
[[nodiscard]] std::error_code mark_archive_origin(int fd, std::string_view origin);
[[nodiscard]] std::error_code copy_archive_origin(int source, int destination);
[[nodiscard]] bool archive_confirmation_required(const std::filesystem::path& file);
[[nodiscard]] bool archive_confirmation_required(int fd);
[[nodiscard]] std::string archive_origin(const std::filesystem::path& archive);
[[nodiscard]] std::error_code mark_archive_tree(const std::filesystem::path& path,
                                                std::string_view origin,
                                                const core::CancelToken& token);
// Explicit approval affects only the already-open regular file. Does not follow a path.
[[nodiscard]] std::error_code approve_archive_program(int fd);
}  // namespace rummage::fs
