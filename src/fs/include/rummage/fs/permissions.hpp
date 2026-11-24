#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Permission changes use the current user credentials. Blocking: call from a worker.
namespace rummage::fs {

// A file's permission bits before and after a change (undone by setting before again).
struct ModeChange {
    std::filesystem::path path;
    std::uint16_t before{0};
    std::uint16_t after{0};
};

struct ModeResult {
    std::vector<ModeChange> changed;  // the files whose bits changed, in the order given
    std::vector<std::string> errors;  // "app.AppImage: Read-only file system"
};

// Changes only rwx bits of an owned regular file or directory, refusing links, replacements,
// and modes changed since Properties loaded. Uses a pinned descriptor, never privilege elevation.
[[nodiscard]] std::string set_permissions(const std::filesystem::path& path, std::uint64_t device,
                                          std::uint64_t inode, std::uint16_t before,
                                          std::uint16_t permissions);

// The bits a regular file gets: allow adds the owner's execute bit only (u+x); otherwise every
// execute bit goes (a-x). Other bits are kept.
[[nodiscard]] std::uint16_t running_mode(std::uint16_t mode, bool allow) noexcept;

// Sets running_mode on each of files (a link: its target, as chmod does). Anything that is not a
// regular file is left alone, and so is a file whose bits would not change.
[[nodiscard]] ModeResult set_running(const std::vector<std::filesystem::path>& files, bool allow);

// Gives each changed file its bits before (an undo) or after (a redo) again, if they are still
// what the change left (otherwise the file is left alone and named in errors: "changed since").
[[nodiscard]] ModeResult restore_modes(const std::vector<ModeChange>& changes, bool before);

}  // namespace rummage::fs
