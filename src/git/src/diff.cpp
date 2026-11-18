#include "rummage/git/diff.hpp"

#include <array>
#include <cstdint>

namespace rummage::git {
namespace {

constexpr std::size_t kMaxLineBytes = 4000;
constexpr std::size_t kTabWidth = 4;

// The length of the UTF-8 sequence at text[at], or 0 when it is not a valid one.
std::size_t sequence_at(std::string_view text, std::size_t at) {
    const auto byte = [&](std::size_t i) { return static_cast<std::uint8_t>(text[i]); };
    const std::uint8_t lead = byte(at);
    std::size_t length = 0;
    if (lead < 0x80U) {
        return 1;
    }
    if (lead >= 0xC2U && lead <= 0xDFU) {
        length = 2;
    } else if (lead >= 0xE0U && lead <= 0xEFU) {
        length = 3;
    } else if (lead >= 0xF0U && lead <= 0xF4U) {
        length = 4;
    } else {
        return 0;
    }
    if (at + length > text.size()) {
        return 0;
    }
    for (std::size_t i = 1; i < length; ++i) {
        if ((byte(at + i) & 0xC0U) != 0x80U) {
            return 0;
        }
    }
    return length;
}

// line with tabs written out, broken UTF-8 replaced (U+FFFD), cut after kMaxLineBytes.
std::string clean(std::string_view line) {
    std::string out;
    out.reserve(line.size());
    std::size_t column = 0;
    for (std::size_t at = 0; at < line.size();) {
        if (out.size() >= kMaxLineBytes) {
            out += "…";
            break;
        }
        if (line[at] == '\t') {
            const std::size_t spaces = kTabWidth - (column % kTabWidth);
            out.append(spaces, ' ');
            column += spaces;
            ++at;
            continue;
        }
        if (line[at] == '\r' && at + 1 == line.size()) {
            break;  // a CRLF file's line end
        }
        const std::size_t length = sequence_at(line, at);
        if (length == 0) {
            out += "\xEF\xBF\xBD";
            ++at;
        } else {
            out.append(line.substr(at, length));
            at += length;
        }
        ++column;
    }
    return out;
}

bool is_header(std::string_view line) {
    constexpr std::array<std::string_view, 12> kHeaders{
        "diff ",    "index ",   "--- ",        "+++ ",    "new file",      "deleted file",
        "old mode", "new mode", "similarity ", "rename ", "Binary files ", "\\ No newline"};
    for (const std::string_view header : kHeaders) {
        if (line.starts_with(header)) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::vector<std::string> diff_arguments(const std::filesystem::path& worktree,
                                        const std::string& path, DiffKind kind) {
    std::vector<std::string> arguments{"--no-optional-locks", "-C",   worktree.string(),
                                       "--literal-pathspecs", "diff", "--no-color",
                                       "--no-ext-diff"};
    switch (kind) {
        case DiffKind::staged:
            arguments.emplace_back("--cached");
            break;
        case DiffKind::untracked:
            // A new file against nothing: all of it added (exits 1 when they differ).
            arguments.emplace_back("--no-index");
            arguments.emplace_back("--");
            arguments.emplace_back("/dev/null");
            arguments.push_back(path);
            return arguments;
        case DiffKind::changes:
            break;
    }
    arguments.emplace_back("--");
    arguments.push_back(path);
    return arguments;
}

text::Text diff_text(std::string_view output, std::size_t max_lines) {
    text::Text text;
    text.language = "Diff";
    std::size_t start = 0;
    while (start < output.size()) {
        if (text.lines.size() == max_lines) {
            text.truncated = true;
            break;
        }
        std::size_t end = output.find('\n', start);
        if (end == std::string_view::npos) {
            end = output.size();
        }
        std::string line = clean(output.substr(start, end - start));
        start = end + 1;
        std::vector<text::Span> spans;
        const auto whole = [&](text::Style style) {
            if (!line.empty()) {
                spans.push_back({.start = 0,
                                 .length = static_cast<std::uint32_t>(line.size()),
                                 .style = style});
            }
        };
        if (is_header(line)) {
            whole(text::Style::comment);
        } else if (line.starts_with("@@")) {
            whole(text::Style::function);
        } else if (line.starts_with('+')) {
            whole(text::Style::added);
        } else if (line.starts_with('-')) {
            whole(text::Style::removed);
        }
        text.lines.push_back(std::move(line));
        text.spans.push_back(std::move(spans));
    }
    return text;
}

}  // namespace rummage::git
