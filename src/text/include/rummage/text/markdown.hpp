#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace rummage::text {

// A bounded presentation document, produced only in the text helper. No URLs, file names,
// HTML nodes or executable content cross this interface: links are styled text only.
enum class MarkdownKind : std::uint8_t {
    paragraph,
    heading,
    quote,
    list,
    code,
    table_header,
    table_row,
    rule,
    gap
};

enum MarkdownStyle : std::uint16_t {
    md_plain = 0,
    md_bold = 1,
    md_italic = 2,
    md_code = 4,
    md_link = 8,
    // Syntax colour + 1 in the high byte, zero for ordinary text.
};

struct MarkdownRun {
    std::string text;
    std::uint16_t style{0};
    std::uint8_t cell{0};
    bool operator==(const MarkdownRun&) const = default;
};

struct MarkdownBlock {
    MarkdownKind kind{MarkdownKind::paragraph};
    std::uint8_t level{0};  // heading 1–6; list/quote indentation 0–8
    std::string marker;
    std::vector<MarkdownRun> runs;
    bool operator==(const MarkdownBlock&) const = default;
};

struct Markdown {
    std::vector<MarkdownBlock> blocks;
    bool truncated{false};
    bool operator==(const Markdown&) const = default;
};

inline constexpr std::size_t kMarkdownMaxBlocks = 4096;
inline constexpr std::size_t kMarkdownMaxRuns = 32768;
inline constexpr std::size_t kMarkdownMaxText = 256 * 1024;
inline constexpr std::size_t kMarkdownMaxPayload = 1024 * 1024;

// Wire representation is bounded and checked independently from the file parser. An empty
// payload means no rendered document. Malformed values never reach the UI layout.
[[nodiscard]] std::vector<std::byte> encode_markdown(const Markdown& document);
[[nodiscard]] bool decode_markdown(std::span<const std::byte> bytes, Markdown& document);

}  // namespace rummage::text
