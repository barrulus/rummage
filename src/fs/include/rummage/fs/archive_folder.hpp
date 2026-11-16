#pragma once

// Archives seen as folders (ROADMAP 0.9.0): "/home/me/photos.zip/docs" is the folder docs inside
// photos.zip. The entries come from the engine that reads archives (archive::list_archive);
// here they only become a tree of folders. Names that would lead outside the archive ("..") are
// left out, and counted.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "rummage/archive/archive.hpp"
#include "rummage/fs/entry.hpp"
#include "rummage/fs/mime.hpp"

namespace rummage::fs {

// A path inside an archive: the archive file and the folder in it ("" for its top).
struct ArchiveLocation {
    std::filesystem::path archive;
    std::string inner;  // "docs/2026", no leading or trailing '/'

    bool operator==(const ArchiveLocation&) const = default;
};

// Where path goes into an archive: its longest leading part that is a regular file, and the rest
// (blocking: stats each part; workers only). nullopt when no part of it is a regular file.
[[nodiscard]] std::optional<ArchiveLocation> find_archive(const std::filesystem::path& path);

// An inner path made safe: "./a//b/" -> "a/b", absolute paths refused; nullopt when a
// part is "..", holds a NUL, exceeds 255 bytes, or the path exceeds 1024 bytes/64 levels.
[[nodiscard]] std::optional<std::string> clean_inner_path(std::string_view path);

// What an archive holds, counted over the entries its tree shows (folders made up from names
// included, entries left out not).
struct ArchiveTotals {
    std::uint64_t files{0};    // regular files, hard links included
    std::uint64_t folders{0};  // below the top
    std::uint64_t links{0};    // symbolic links
    std::uint64_t others{0};   // devices, fifos...: listed, never extracted
    std::uint64_t bytes{0};    // the files' bytes once read
    std::uint64_t encrypted{0};
    std::uint64_t unsupported{0};  // compressed in a way rummage does not read yet

    bool operator==(const ArchiveTotals&) const = default;
};

class ArchiveTree {
public:
    // The tree of listing's entries; own_name names a .gz's one entry when its header does not
    // (the archive's name without ".gz"). mime, when set, gives the entries their types by name.
    ArchiveTree(archive::Listing listing, std::string_view own_name, const MimeDatabase* mime);

    // Binds the tree to the file identity it was listed from; members must match it when read.
    void identify(const std::filesystem::path& path);
    [[nodiscard]] bool unchanged(const std::filesystem::path& path) const;

    [[nodiscard]] archive::Format format() const noexcept { return format_; }

    [[nodiscard]] archive::Failure failure() const noexcept { return failure_; }

    [[nodiscard]] const std::string& detail() const noexcept { return detail_; }

    // Entries left out: names leading outside the archive, or a file where a folder is.
    [[nodiscard]] std::size_t left_out() const noexcept { return left_out_; }

    [[nodiscard]] const ArchiveTotals& totals() const noexcept { return totals_; }

    // The entries of the folder at inner ("" for the top); nullptr when there is no such folder.
    [[nodiscard]] const std::vector<Entry>* folder(std::string_view inner) const;

    // The archive's entry at inner path ("docs/a.txt"); nullptr for a folder made up from the
    // names under it, or no such entry.
    [[nodiscard]] const archive::Entry* entry(std::string_view path) const;

    // Each entry the tree shows at path or under it ("" for all), with its clean path, in no
    // particular order; folders made up from the names under them come with a null entry.
    void each_under(std::string_view path,
                    const std::function<void(const std::string& path, const archive::Entry* entry)>&
                        visit) const;

private:
    std::optional<std::array<std::uint64_t, 7>> identity_;
    archive::Format format_;
    archive::Failure failure_;
    std::string detail_;
    std::size_t left_out_{0};
    ArchiveTotals totals_;
    std::vector<archive::Entry> entries_;
    std::unordered_map<std::string, std::size_t> by_path_;  // clean path -> entries_ index
    std::unordered_map<std::string, std::vector<Entry>> folders_;
};

// The tree of the archive file, listed by the engine; the last few are kept while their files are
// unchanged, each for the mime database it was typed with (asked without one, any kept tree will
// do: extracting needs no types, and a tar.gz is not inflated again to list it) (blocking: workers
// only). Never null: a failure is in the tree's failure().
[[nodiscard]] std::shared_ptr<const ArchiveTree> archive_tree(const std::filesystem::path& archive,
                                                              const MimeDatabase* mime);

// Forgets the kept trees (tests).
void forget_archive_trees();

}  // namespace rummage::fs
