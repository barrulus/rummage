#pragma once

#include "archive_text.hpp"
#include "rummage/views/media_details.hpp"

namespace rummage::views {
// Non-image metadata shared by Properties and preview details.
inline std::vector<std::pair<std::string, std::string>> content_details(
    const preview::Summary& summary) {
    std::vector<std::pair<std::string, std::string>> rows;
    if (summary.text) {
        const auto& text = *summary.text;
        if (!text.language.empty())
            rows.emplace_back(i18n::tr("Language"), text.language);
        rows.emplace_back(i18n::tr("Encoding"), text::encoding_name(text.encoding));
        rows.emplace_back(i18n::tr("Line endings"),
                          text.crlf ? i18n::tr("CRLF present") : i18n::tr("LF / no line endings"));
        rows.emplace_back(text.truncated ? i18n::tr("Preview lines") : i18n::tr("Lines"),
                          std::to_string(text.lines.size()));
        if (text.truncated)
            rows.emplace_back(i18n::tr("Text preview"),
                              text.tail ? i18n::tr("Truncated (end of file)")
                                        : i18n::tr("Truncated (start of file)"));
    }
    if (summary.archive) {
        const auto& archive = *summary.archive;
        const auto details = archive_rows(archive);
        rows.emplace_back(i18n::tr("Contains"), details.contains);
        rows.emplace_back(i18n::tr("Unpacked"), details.unpacked);
        rows.emplace_back(i18n::tr("Encrypted"), details.encrypted);
        rows.emplace_back(i18n::tr("Not readable yet"), details.unsupported);
        rows.emplace_back(i18n::tr("Archive information"), archive_problem(archive));
    }
    std::erase_if(rows, [](const auto& row) { return row.second.empty(); });
    return rows;
}
}  // namespace rummage::views
