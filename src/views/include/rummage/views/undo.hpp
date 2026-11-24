#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "rummage/fs/batch_rename.hpp"
#include "rummage/fs/permissions.hpp"
#include "rummage/fs/transfer.hpp"

// What Ctrl+Z undoes (ROADMAP 6.7): the last operation on files, as it was done. Undoing one
// does its inverse, which is itself an operation: that one is what Ctrl+Shift+Z undoes again.
namespace rummage::views {

struct FileOperation {
    enum class Kind : std::uint8_t {
        group,         // move into/out of a directory owned by this operation
        batch_rename,  // all explicit names, including sidecars, in one transaction
        rename,        // one item: from the old name to the new (its sidecars follow)
        create,        // one item: to = the new folder or file; from = its template, or empty
        copy,          // undone by trashing the copies
        move,          // undone by moving them back
        trash,         // to = where each went in the trash's files/: undone by restoring
        restore,       // undone by trashing again
        permissions,   // undone by restoring the previous mode
        erase,         // cannot be undone
    };

    struct CreatedFile {
        std::uintmax_t size;
        std::filesystem::file_time_type modified;
    };

    Kind kind{Kind::copy};
    std::vector<fs::PlacedItem> items;    // sidecars left out: they follow their photo
    std::vector<fs::ModeChange> modes{};  // permissions only
    // Groups that cannot be safely undone (replacements or preexisting sidecars).
    std::size_t undo_excluded{0};
    std::optional<CreatedFile> created_file{};  // clipboard content, checked before undo
    fs::GroupFolder group_folder{};
    bool grouped{false};
    std::vector<fs::RenameChange> renames{};  // batch rename identities and exact sidecar set
};

// What the operation did, for "Undone: ..." and "Redone: ...": "“P1.ORF” is “Lake.ORF” again",
// "3 items moved to the trash", "“a.txt” restored", "2 items back in photos".
[[nodiscard]] std::string describe(const FileOperation& operation);

}  // namespace rummage::views
