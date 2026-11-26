#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
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

// Snapshot used to refuse roots replaced or edited since the dialog was opened.
struct PermissionTarget {
    std::filesystem::path path;
    std::uint64_t device{}, inode{};
    std::uint32_t owner{}, group{};
    std::uint16_t mode{};
    bool directory{false};
    std::string error;
};

[[nodiscard]] PermissionTarget permission_target(const std::filesystem::path& path);

struct PermissionEdit {
    std::uint16_t mask{}, values{};  // only explicitly chosen rwx bits; others stay unchanged
    std::string owner, group;        // empty keeps existing; numeric ID or account/group name
    bool recursive{false};
};

struct PermissionResult {
    std::size_t changed{};
    std::vector<PermissionTarget> refreshed;  // validated original roots after attempted changes
    std::vector<std::string> errors;
};

// No privilege elevation. Roots must match snapshots; descendants are traversed through
// pinned descriptors without following symlinks. Directories change after their children.
// Explicit recursive scope includes selected roots and their descendants, but no mount crossing.
// Changes are immediate and not recorded in file-operation Undo; errors can be partial.
[[nodiscard]] PermissionResult apply_permissions(const std::vector<PermissionTarget>& roots,
                                                 const PermissionEdit& edit,
                                                 const std::function<bool()>& cancelled = {});

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
