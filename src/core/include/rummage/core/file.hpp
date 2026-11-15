#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace rummage::core {

struct TextFile {
    bool exists{false};  // false: no such file, which callers usually treat as "use defaults"
    std::string text;
    std::string error;  // set when the file exists but cannot be read; text is then empty
};

// Reads a whole regular file. Never throws for I/O problems: they land in error.
[[nodiscard]] TextFile read_text_file(const std::filesystem::path& path);

// Copy a settings file from the sibling nordend directory if the rummage destination is
// absent. Never overwrites a destination (including symlinks) or changes the original.
// Returns an error message, or an empty string for success/no legacy file.
[[nodiscard]] std::string migrate_nordend_file(const std::filesystem::path& destination);

// Replaces path's content with text, all or nothing: written to a hidden file beside it, synced,
// then renamed over it. Its folder is made when missing. Returns what failed.
// With replace=false, publish without overwriting any existing entry (including symlinks);
// an existing entry is success.
[[nodiscard]] std::error_code write_file_atomically(const std::filesystem::path& path,
                                                    std::string_view text, bool replace = true);

// The file:// URI of an absolute path, escaped exactly as GLib's g_filename_to_uri does (other
// programs compare these strings: thumbnails by their MD5, bookmarks, the clipboard).
[[nodiscard]] std::string file_uri(const std::filesystem::path& absolute);

// Decode an absolute local file:// URI. Remote hosts, query/fragment components, malformed
// escapes and NUL bytes are refused; escaped filename characters are preserved.
[[nodiscard]] std::optional<std::filesystem::path> path_from_uri(std::string_view uri);

}  // namespace rummage::core
