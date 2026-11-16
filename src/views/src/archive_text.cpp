#include "archive_text.hpp"

#include <format>
#include <string_view>

#include "rummage/views/format.hpp"

namespace rummage::views {
namespace {

// "1 file", "3 files"; empty for none.
std::string count_of(std::uint64_t count, std::string_view noun) {
    return count == 0 ? std::string{} : std::format("{} {}{}", count, noun, count == 1 ? "" : "s");
}

}  // namespace

ArchiveRows archive_rows(const preview::Archive& archive) {
    const fs::ArchiveTotals& totals = archive.totals;
    ArchiveRows rows;
    const auto add = [&](std::uint64_t count, std::string_view noun) {
        if (count > 0) {
            rows.contains += (rows.contains.empty() ? "" : ", ") + count_of(count, noun);
        }
    };
    add(totals.files, "file");
    add(totals.folders, "folder");
    add(totals.links, "link");
    add(totals.others, "other");
    if (rows.contains.empty() && archive.failure == archive::Failure::none) {
        rows.contains = "Nothing";
    }
    if (totals.files > 0) {
        rows.unpacked = format_size(totals.bytes);
    }
    rows.encrypted = count_of(totals.encrypted, "file");
    rows.unsupported = count_of(totals.unsupported, "file");
    return rows;
}

std::string archive_problem(const preview::Archive& archive) {
    if (archive.failure != archive::Failure::none) {
        const bool partly = archive.top_count > 0 || archive.totals.files > 0;
        return partly ? "damaged: " + archive.detail : archive.detail;
    }
    if (archive.left_out > 0) {
        return std::format("{} {} with unsafe names left out", archive.left_out,
                           archive.left_out == 1 ? "entry" : "entries");
    }
    return {};
}

}  // namespace rummage::views
