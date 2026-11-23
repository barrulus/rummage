#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rummage::fs {

struct RenameSource {
    std::string name;
    bool regular{false};
    bool directory{false};
    std::uint64_t device{0};
    std::uint64_t inode{0};
    std::optional<std::uint64_t> size{};
    std::optional<std::int64_t> modified{};  // seconds since the Unix epoch
};

struct RenameDirectory {
    std::vector<RenameSource> entries;
    std::string error;
};

// Blocking; includes hidden entries and dangling links, without following links.
[[nodiscard]] RenameDirectory rename_directory(const std::filesystem::path& directory);

struct RenameOptions {
    enum class Target { basename, extension, whole };
    enum class Case { unchanged, lower, upper, title };
    std::string find{};
    std::string replace{};
    // {name}: the stem after literal replacement, {ext}: extension including its dot, {n}: number.
    std::string pattern{"{name}{ext}"};
    std::uint64_t start{1};
    std::uint64_t step{1};
    unsigned padding{2};
    Target target{Target::basename};
    Case letter_case{Case::unchanged};  // ASCII case; other UTF-8 characters are preserved
    bool regex{false};
    std::uint64_t position{0};  // zero-based Unicode scalar position; beyond end appends
    std::uint64_t remove{0};
    std::string insert{};
    std::optional<std::int64_t> timestamp{};  // fixed when the dialog opens, for {date}/{time}
};

struct RenameChange {
    std::string from;
    std::string to;
    std::uint64_t device{0};
    std::uint64_t inode{0};
    bool sidecar{false};
};

struct RenamePlan {
    std::vector<RenameChange>
        changes;  // includes unchanged rows and automatically following sidecars
    std::string error;
    [[nodiscard]] bool changes_anything() const;
};

// Pure preview in selection order. Selected sidecars follow their selected photo and do not
// consume a number. Other entries are numbered exactly in the order supplied by the view.
[[nodiscard]] RenamePlan plan_rename(std::span<const RenameSource> directory,
                                     std::span<const std::string> selection,
                                     const RenameOptions& options);

// Checks names, duplicate targets, occupied destinations, and missing/replaced sources.
[[nodiscard]] std::string check_rename(std::span<const RenameSource> directory,
                                       std::span<const RenameChange> changes);

struct BatchRenamed {
    std::vector<RenameChange>
        changed;  // actual moves, including sidecars; only populated on success
    std::string error;
    // If another process prevents rollback, identifies each original name and its current path.
    std::vector<std::string> recovery;
};

// Blocking. Validates again, stages in a private directory, then installs all names. Every move,
// including rollback, uses RENAME_NOREPLACE; unsupported file systems fail without a racy fallback.
// A failure rolls back what moved, or lists every stranded entry in recovery. Never deletes data.
[[nodiscard]] BatchRenamed rename_batch(const std::filesystem::path& directory,
                                        std::span<const RenameChange> changes);

}  // namespace rummage::fs
