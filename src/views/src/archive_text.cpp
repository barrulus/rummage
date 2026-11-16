#include "archive_text.hpp"

#include <format>
#include <string_view>

#include "rummage/core/i18n.hpp"
#include "rummage/views/format.hpp"

namespace rummage::views {
namespace {

// "1 file", "3 files"; empty for none.
std::string count_of(std::uint64_t count, std::string_view noun) {
    if (count == 0)
        return {};
    if (noun == "file")
        return i18n::trn("{} file", "{} files", count);
    if (noun == "folder")
        return i18n::trn("{} folder", "{} folders", count);
    if (noun == "link")
        return i18n::trn("{} link", "{} links", count);
    return i18n::trn("{} other", "{} others", count);
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
        rows.contains = i18n::tr("Nothing");
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
        return partly ? i18n::trf("damaged: {}", archive.detail) : archive.detail;
    }
    if (archive.left_out > 0) {
        return i18n::trn("{} entry with an unsafe name left out",
                         "{} entries with unsafe names left out", archive.left_out);
    }
    return {};
}

}  // namespace rummage::views
