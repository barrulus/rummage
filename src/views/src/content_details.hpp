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
            rows.emplace_back("Language", text.language);
        rows.emplace_back("Encoding", text::encoding_name(text.encoding));
        rows.emplace_back("Line endings", text.crlf ? "CRLF present" : "LF / no line endings");
        rows.emplace_back(text.truncated ? "Preview lines" : "Lines",
                          std::to_string(text.lines.size()));
        if (text.truncated)
            rows.emplace_back("Text preview",
                              text.tail ? "Truncated (end of file)" : "Truncated (start of file)");
    }
    if (summary.archive) {
        const auto& archive = *summary.archive;
        const auto details = archive_rows(archive);
        rows.emplace_back("Contains", details.contains);
        rows.emplace_back("Unpacked", details.unpacked);
        rows.emplace_back("Encrypted", details.encrypted);
        rows.emplace_back("Not readable yet", details.unsupported);
        rows.emplace_back("Archive information", archive_problem(archive));
    }
    std::erase_if(rows, [](const auto& row) { return row.second.empty(); });
    return rows;
}
}  // namespace rummage::views
