#include "rummage/formats/text/markdown.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace rummage::formats::text {
namespace {
using namespace rummage::text;
using Kind = MarkdownKind;

std::string_view trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string_view::npos)
        return {};
    const auto last = value.find_last_not_of(" \t");
    return value.substr(first, last - first + 1);
}

struct Inline {
    std::vector<MarkdownRun>& runs;
    std::size_t budget;
    std::uint8_t cell{0};

    void append(std::string_view text, std::uint16_t style) {
        if (text.empty())
            return;
        if (!runs.empty() && runs.back().style == style && runs.back().cell == cell)
            runs.back().text += text;
        else
            runs.push_back({std::string{text}, style, cell});
    }

    std::size_t find(std::string_view value, std::string_view needle, std::size_t start) {
        const auto work = value.size() - std::min(start, value.size());
        if (work > budget)
            return std::string_view::npos;
        budget -= work;
        auto at = value.find(needle, start);
        while (at != std::string_view::npos) {
            std::size_t escaped = 0;
            for (std::size_t before = at; before > 0 && value[before - 1] == '\\'; --before)
                ++escaped;
            if ((escaped % 2) == 0)
                return at;
            at = value.find(needle, at + needle.size());
        }
        return std::string_view::npos;
    }

    void parse(std::string_view value, std::uint16_t style = 0, unsigned depth = 0) {
        if (depth >= 8 || budget == 0) {
            append(value, style);
            return;
        }
        std::size_t plain = 0;
        for (std::size_t i = 0; i < value.size();) {
            if (value[i] == '\\' && i + 1 < value.size() &&
                std::ispunct(static_cast<unsigned char>(value[i + 1])) != 0) {
                append(value.substr(plain, i - plain), style);
                append(value.substr(i + 1, 1), style);
                i += 2;
                plain = i;
                continue;
            }
            const bool image = value[i] == '!' && i + 1 < value.size() && value[i + 1] == '[';
            if (value[i] == '[' || image) {
                const auto begin = i + (image ? 2U : 1U);
                const auto label_end = find(value, "](", begin);
                const auto end = label_end == std::string_view::npos
                                     ? std::string_view::npos
                                     : find(value, ")", label_end + 2);
                if (end != std::string_view::npos) {
                    append(value.substr(plain, i - plain), style);
                    if (image)
                        append("[Image: ", style);
                    parse(value.substr(begin, label_end - begin),
                          static_cast<std::uint16_t>(style | (image ? 0 : md_link)), depth + 1);
                    if (image)
                        append("]", style);
                    i = end + 1;
                    plain = i;
                    continue;
                }
            }
            const char c = value[i];
            if (c == '`' || c == '*' || c == '_') {
                std::size_t length = 1;
                while (i + length < value.size() && value[i + length] == c && length < 3)
                    ++length;
                // Underscores in identifiers are ordinary text.
                const bool intraword = c == '_' && i > 0 &&
                                       std::isalnum(static_cast<unsigned char>(value[i - 1])) != 0;
                const auto end = intraword ? std::string_view::npos
                                           : find(value, value.substr(i, length), i + length);
                if (end != std::string_view::npos && end > i + length) {
                    append(value.substr(plain, i - plain), style);
                    const auto inside = value.substr(i + length, end - i - length);
                    if (c == '`')
                        append(inside, static_cast<std::uint16_t>(style | md_code));
                    else {
                        const unsigned extra = length == 1   ? md_italic
                                               : length == 2 ? md_bold
                                                             : md_bold | md_italic;
                        parse(inside, static_cast<std::uint16_t>(style | extra), depth + 1);
                    }
                    i = end + length;
                    plain = i;
                    continue;
                }
                // An unmatched delimiter run stays whole; its second character must not
                // become a new opener (e.g. **unfinished followed by an escaped star).
                i += length;
                continue;
            }
            ++i;
        }
        append(value.substr(plain), style);
    }
};

std::vector<std::string_view> cells(std::string_view line) {
    line = trim(line);
    if (line.starts_with('|'))
        line.remove_prefix(1);
    if (line.ends_with('|') && !line.ends_with("\\|"))
        line.remove_suffix(1);
    std::vector<std::string_view> result;
    std::size_t begin = 0;
    bool code = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\\') {
            ++i;
        } else if (line[i] == '`') {
            code = !code;
        } else if (line[i] == '|' && !code && result.size() < 15) {
            result.push_back(trim(line.substr(begin, i - begin)));
            begin = i + 1;
        }
    }
    result.push_back(trim(line.substr(begin)));
    return result;
}

bool table_separator(std::string_view line) {
    if (line.find('|') == std::string_view::npos)
        return false;
    const auto parts = cells(line);
    return std::ranges::all_of(parts, [](std::string_view part) {
        if (part.starts_with(':'))
            part.remove_prefix(1);
        if (part.ends_with(':'))
            part.remove_suffix(1);
        return part.size() >= 3 && part.find_first_not_of('-') == std::string_view::npos;
    });
}

bool rule(std::string_view line) {
    if (line.empty() || (line.front() != '-' && line.front() != '*' && line.front() != '_'))
        return false;
    std::size_t count = 0;
    for (const char c : line) {
        if (c == line.front())
            ++count;
        else if (c != ' ')
            return false;
    }
    return count >= 3;
}

void code_runs(MarkdownBlock& block, const Text& source, std::size_t line) {
    const auto& value = source.lines[line];
    std::size_t at = 0;
    if (line < source.spans.size())
        for (const auto& span : source.spans[line]) {
            if (span.start < at || std::size_t{span.start} + span.length > value.size())
                continue;
            if (span.start > at)
                block.runs.push_back({value.substr(at, span.start - at), md_code, 0});
            block.runs.push_back({value.substr(span.start, span.length),
                                  static_cast<std::uint16_t>(
                                      md_code | ((static_cast<unsigned>(span.style) + 1) << 8U)),
                                  0});
            at = std::size_t{span.start} + span.length;
        }
    if (at < value.size() || block.runs.empty())
        block.runs.push_back({value.substr(at), md_code, 0});
}
}  // namespace

Markdown markdown(const Text& source) {
    Markdown result;
    result.truncated = source.truncated;
    std::size_t bytes = 0;
    std::size_t runs = 0;
    std::size_t output_bytes = 0;
    char fence = 0;
    std::size_t fence_size = 0;
    bool table = false;
    std::size_t table_columns = 0;
    for (std::size_t i = 0; i < source.lines.size(); ++i) {
        const std::string_view original = source.lines[i];
        if (i >= kMarkdownMaxBlocks || original.size() > kMarkdownMaxText - bytes ||
            result.blocks.size() >= kMarkdownMaxBlocks) {
            result.truncated = true;
            break;
        }
        bytes += original.size();
        std::string_view line = trim(original);
        MarkdownBlock block;
        if (fence != 0) {
            const auto count = line.find_first_not_of(fence);
            if (line.starts_with(fence) &&
                (count == std::string_view::npos || count >= fence_size) &&
                (count == std::string_view::npos || trim(line.substr(count)).empty())) {
                fence = 0;
                continue;
            }
            block.kind = Kind::code;
            code_runs(block, source, i);
        } else if (line.starts_with("```") || line.starts_with("~~~")) {
            fence = line.front();
            fence_size = line.find_first_not_of(fence);
            if (fence_size == std::string_view::npos)
                fence_size = line.size();
            table = false;
            continue;
        } else if (line.empty()) {
            table = false;
            if (result.blocks.empty() || result.blocks.back().kind == Kind::gap)
                continue;
            block.kind = Kind::gap;
        } else if (i + 1 < source.lines.size() && line.find('|') != std::string_view::npos &&
                   table_separator(source.lines[i + 1])) {
            block.kind = Kind::table_header;
            table = true;
            ++i;
        } else if (table && line.find('|') != std::string_view::npos) {
            block.kind = Kind::table_row;
        } else {
            table = false;
            if (rule(line)) {
                block.kind = Kind::rule;
                line = {};
            } else if (line.starts_with('#')) {
                const auto hashes = line.find_first_not_of('#');
                if (hashes != std::string_view::npos && hashes <= 6 && line[hashes] == ' ') {
                    block.kind = Kind::heading;
                    block.level = static_cast<std::uint8_t>(hashes);
                    line = trim(line.substr(hashes));
                    const auto last = line.find_last_not_of('#');
                    if (last != std::string_view::npos && last + 1 < line.size() &&
                        line[last] == ' ')
                        line = trim(line.substr(0, last));
                }
            } else if (line.starts_with('>')) {
                block.kind = Kind::quote;
                while (line.starts_with('>') && block.level < 8) {
                    ++block.level;
                    line = trim(line.substr(1));
                }
            } else {
                std::size_t prefix = 0;
                if (line.size() >= 2 && (line[0] == '-' || line[0] == '*' || line[0] == '+') &&
                    line[1] == ' ')
                    prefix = 2;
                else {
                    const auto end = line.find_first_not_of("0123456789");
                    if (end > 0 && end <= 9 && end + 1 < line.size() &&
                        (line[end] == '.' || line[end] == ')') && line[end + 1] == ' ')
                        prefix = end + 2;
                }
                if (prefix != 0) {
                    block.kind = Kind::list;
                    block.level = static_cast<std::uint8_t>(
                        std::min<std::size_t>(8, original.find_first_not_of(" \t") / 2));
                    block.marker = prefix == 2 ? "•" : std::string{line.substr(0, prefix - 1)};
                    line = trim(line.substr(prefix));
                    if (line.starts_with("[ ] ") || line.starts_with("[x] ") ||
                        line.starts_with("[X] ")) {
                        block.marker = line[1] == ' ' ? "☐" : "☑";
                        line = line.substr(4);
                    }
                } else if (i + 1 < source.lines.size()) {
                    const auto next = trim(source.lines[i + 1]);
                    if (!next.empty() && (next.front() == '=' || next.front() == '-') &&
                        next.find_first_not_of(next.front()) == std::string_view::npos) {
                        block.kind = Kind::heading;
                        block.level = next.front() == '=' ? 1 : 2;
                        ++i;
                    }
                }
            }
        }
        if (block.kind == Kind::table_header || block.kind == Kind::table_row) {
            auto parts = cells(line);
            if (block.kind == Kind::table_header)
                table_columns = parts.size();
            parts.resize(table_columns);
            for (std::size_t cell = 0; cell < parts.size(); ++cell) {
                Inline parser{block.runs, parts[cell].size() * 16, static_cast<std::uint8_t>(cell)};
                parser.parse(parts[cell], block.kind == Kind::table_header ? md_bold : md_plain);
                if (parts[cell].empty())
                    block.runs.push_back({{}, 0, static_cast<std::uint8_t>(cell)});
            }
        } else if (block.kind != Kind::code && block.kind != Kind::gap &&
                   block.kind != Kind::rule) {
            Inline{block.runs, line.size() * 16}.parse(line);
        }
        std::size_t block_bytes = 1;  // possible paragraph joining space
        for (const auto& run : block.runs)
            block_bytes += run.text.size();
        if (block_bytes > kMarkdownMaxText - output_bytes ||
            block.runs.size() > kMarkdownMaxRuns - runs) {
            result.truncated = true;
            break;
        }
        output_bytes += block_bytes;
        runs += block.runs.size();
        if (block.kind == Kind::paragraph && !result.blocks.empty() &&
            result.blocks.back().kind == Kind::paragraph) {
            auto& previous = result.blocks.back().runs;
            if (!previous.empty())
                previous.back().text += ' ';
            previous.insert(previous.end(), std::make_move_iterator(block.runs.begin()),
                            std::make_move_iterator(block.runs.end()));
        } else
            result.blocks.push_back(std::move(block));
    }
    return result;
}
}  // namespace rummage::formats::text
