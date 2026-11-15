#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <dirent.h>
#include <filesystem>
#include <functional>
#include <set>
#include <string_view>
#include <sys/types.h>
#include <utility>

#include "rummage/core/worker_pool.hpp"

// How big a tree is: what the info pane shows for a folder, what a permanent delete counts before
// asking. Blocking: call it from a worker.
namespace rummage::fs {

struct MeasureOptions {
    // Folders on another file system than the one measured (a disk mounted inside, /proc under /)
    // are counted but not entered.
    bool one_file_system{true};
    // Folders deeper than this are counted but not entered.
    std::size_t max_depth{64};
    // A tree on a network or FUSE file system (NFS, SMB, sshfs) is not walked at all: a stuck
    // server would hold a worker for as long as it is stuck. fuseblk (NTFS, exFAT disks) is local.
    bool skip_remote{true};
    // A symbolic link given to add() is measured as what it points to; links inside the tree are
    // never followed either way.
    bool follow_root{true};
    // How often progress is told, at most.
    std::chrono::milliseconds progress_interval{150};
};

struct Tally {
    std::uint64_t files{0};        // regular files
    std::uint64_t folders{0};      // the roots included
    std::uint64_t others{0};       // symbolic links, sockets, devices, pipes
    std::uint64_t bytes{0};        // the regular files' sizes (st_size); a hard link counted once
    std::uint64_t unreadable{0};   // entries that could not be examined or listed
    std::uint64_t not_entered{0};  // folders left out: on another file system, or too deep
    bool remote{false};            // a root was on a network file system: not walked
    bool cancelled{false};

    [[nodiscard]] std::uint64_t entries() const noexcept { return files + folders + others; }

    // Nothing was left out: bytes is the whole tree.
    [[nodiscard]] bool complete() const noexcept {
        return unreadable == 0 && not_entered == 0 && !remote && !cancelled;
    }
};

// Adds up trees; hard-linked file bytes count once. Already visited directories are skipped,
// including a root that aliases another root or a descendant of one.
class Measurer {
public:
    using Progress = std::function<void(const Tally&)>;

    Measurer(const MeasureOptions& options, const core::CancelToken& token, Progress progress = {});

    // Counts path and, for a folder, everything under it. Something that cannot be examined
    // counts nothing. Polls the token between entries.
    void add(const std::filesystem::path& path);

    [[nodiscard]] const Tally& tally() const noexcept { return tally_; }

private:
    // Counts name in the folder parent (follow: through a symbolic link); the folder opened when
    // it is one to enter, else nullptr. device: the file system measured (nullptr for a root).
    DIR* visit(int parent, const char* name, bool follow, std::size_t depth, const dev_t* device);
    void tell();

    MeasureOptions options_;
    const core::CancelToken& token_;
    Progress progress_;
    Tally tally_;
    std::set<std::pair<dev_t, ino_t>> linked_;
    std::set<std::pair<dev_t, ino_t>> directories_;
    std::chrono::steady_clock::time_point told_;
};

// One tree, measured with options.
[[nodiscard]] Tally measure(const std::filesystem::path& path, const MeasureOptions& options,
                            const core::CancelToken& token,
                            const Measurer::Progress& progress = {});

// The file system path is on is a network or FUSE one (MeasureOptions::skip_remote); for FUSE,
// /proc/self/mountinfo tells fuseblk apart.
[[nodiscard]] bool is_remote(const std::filesystem::path& path);

// For tests: the decision is_remote makes from statfs's f_type and the mount's type in mountinfo.
[[nodiscard]] bool is_remote_type(std::int64_t magic, std::string_view mount_type);

}  // namespace rummage::fs
