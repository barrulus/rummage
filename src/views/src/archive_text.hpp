#pragma once

// An archive's summary (preview::Archive) in words, the same in Quick Look and the info pane.

#include <string>

#include "rummage/preview/preview.hpp"

namespace rummage::views {

// Each empty when there is nothing to say.
struct ArchiveRows {
    std::string contains;     // "12 files, 3 folders"; "Nothing" for an empty archive
    std::string unpacked;     // the files' bytes once read
    std::string encrypted;    // "2 files"
    std::string unsupported;  // "1 file": compressed in a way not read yet
};

[[nodiscard]] ArchiveRows archive_rows(const preview::Archive& archive);

// What is wrong with it, as a folder inside it says (DirectoryModel::problem); empty when fine.
[[nodiscard]] std::string archive_problem(const preview::Archive& archive);

}  // namespace rummage::views
