#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "rummage/fs/mime.hpp"

namespace rummage::fs {

enum class EntryKind : std::uint8_t { file, directory, other, unknown };

// One directory entry, as the views need it. A symbolic link takes the kind of its target
// (a link to a directory is a directory) and keeps is_link; a dangling link is kind other.
struct Entry {
    std::string name;  // bytes as on disk: usually UTF-8, not guaranteed
    EntryKind kind{EntryKind::unknown};
    bool is_link{false};
    std::uint64_t size{0};        // bytes; for a link, its target's
    std::int64_t modified_ns{0};  // mtime, nanoseconds since the epoch
    // Set by the loader when the kind or the name alone decides it (no file read); otherwise
    // kUnknownMime, and MimeDatabase::detect() looks at the content when the type is needed.
    MimeId mime{kUnknownMime};
    // When the name gives several types (shared-mime-info 2.5 also gives *.md to Sega ROMs), the
    // one of highest weight: sorting by type uses it, so the entry sorts with its likely kind
    // whether or not its content has been sniffed. kUnknownMime otherwise.
    MimeId name_guess{kUnknownMime};
    // Resolved on the directory worker: theme name or absolute local icon path.
    std::string folder_icon{};

    // Collected on directory workers, never during drawing. Missing stat data stays empty.
    std::optional<std::int64_t> created_ns{};
    std::optional<std::int64_t> accessed_ns{};
    std::string owner{};
    std::string group{};
    std::string permissions{};
    std::string link_target{};

    [[nodiscard]] bool hidden() const noexcept { return !name.empty() && name.front() == '.'; }

    [[nodiscard]] bool is_directory() const noexcept { return kind == EntryKind::directory; }
};

// File metadata presentation shared by directory listings and Properties. Identity lookups
// may consult NSS: call on a worker and cache by ID when reading a whole directory.
[[nodiscard]] std::string file_permissions(std::uint32_t mode);
[[nodiscard]] std::string owner_name(std::uint32_t uid);
[[nodiscard]] std::string group_name(std::uint32_t gid);

}  // namespace rummage::fs
