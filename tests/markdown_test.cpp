#include "rummage/formats/text/markdown.hpp"

#include <algorithm>
#include <cstring>

#include <doctest/doctest.h>

#include "rummage/formats/text/syntax.hpp"
#include "rummage/text/markdown.hpp"

namespace {
using rummage::text::MarkdownKind;

rummage::text::Text parse(std::vector<std::string> lines) {
    rummage::text::Text value;
    value.lines = std::move(lines);
    rummage::formats::text::colour(value, "README.md", "text/markdown");
    return value;
}

std::string plain(const rummage::text::MarkdownBlock& block) {
    std::string result;
    for (const auto& run : block.runs)
        result += run.text;
    return result;
}
}  // namespace

TEST_CASE("markdown: blocks, emphasis, tables and fenced syntax preserve useful content") {
    const auto text =
        parse({"# A heading", "", "A **bold** and *italic* [link](https://example.com).",
               "Second line with `code`.", "", "- [x] Done", "  - [ ] Next", "> A quote", "",
               "| Name | State |", "| :--- | ---: |", "| alpha | **ready** |", "", "```cpp",
               "int main() { return 42; }", "```", "---", "![Picture](file:///private/image.png)"});
    REQUIRE(text.language == "Markdown");
    const auto& blocks = text.markdown.blocks;
    REQUIRE(blocks.size() == 14);
    CHECK(blocks[0].kind == MarkdownKind::heading);
    CHECK(blocks[0].level == 1);
    CHECK(plain(blocks[0]) == "A heading");
    CHECK(plain(blocks[2]) == "A bold and italic link. Second line with code.");
    CHECK(std::ranges::any_of(blocks[2].runs, [](const auto& run) {
        return run.text == "bold" && (run.style & rummage::text::md_bold) != 0;
    }));
    CHECK(std::ranges::any_of(blocks[2].runs, [](const auto& run) {
        return run.text == "italic" && (run.style & rummage::text::md_italic) != 0;
    }));
    CHECK(blocks[4].kind == MarkdownKind::list);
    CHECK(blocks[4].marker == "☑");
    CHECK(blocks[5].level == 1);
    CHECK(blocks[6].kind == MarkdownKind::quote);
    CHECK(blocks[8].kind == MarkdownKind::table_header);
    CHECK(blocks[9].kind == MarkdownKind::table_row);
    CHECK(blocks[9].runs.back().cell == 1);
    CHECK(blocks[11].kind == MarkdownKind::code);
    CHECK(plain(blocks[11]) == "int main() { return 42; }");
    CHECK(std::ranges::any_of(blocks[11].runs,
                              [](const auto& run) { return (run.style >> 8U) != 0; }));
    CHECK(plain(blocks.back()) == "[Image: Picture]");
    CHECK(text.lines[2].find("https://") != std::string::npos);  // source remains intact
}

TEST_CASE("markdown: unclosed syntax and HTML stay text and links never carry destinations") {
    const auto text =
        parse({"<script>alert('x')</script>", "a_b_c **unfinished \\*literal*",
               "[open](javascript:alert) ![secret](https://private/)", "```", "*code*"});
    CHECK(plain(text.markdown.blocks.front()).find("<script>") != std::string::npos);
    CHECK(plain(text.markdown.blocks.front()).find("a_b_c **unfinished *literal*") !=
          std::string::npos);
    CHECK(plain(text.markdown.blocks.front()).find("javascript") == std::string::npos);
    CHECK(text.markdown.blocks.back().kind == MarkdownKind::code);
    CHECK(plain(text.markdown.blocks.back()) == "*code*");
}

TEST_CASE("markdown: parser bounds pathological delimiters, lines and total output") {
    rummage::text::Text text;
    text.lines.assign(6000, "# **a***_[](``\\| >");
    auto result = rummage::formats::text::markdown(text);
    CHECK(result.truncated);
    CHECK(result.blocks.size() <= rummage::text::kMarkdownMaxBlocks);
    text.lines = {std::string(rummage::text::kMarkdownMaxText + 1, '[')};
    result = rummage::formats::text::markdown(text);
    CHECK(result.truncated);
    CHECK(result.blocks.empty());
    text.lines = {std::string(64000, '*') + "x", std::string(64000, '[')};
    result = rummage::formats::text::markdown(text);
    CHECK(result.blocks.size() <= 2);
    auto wire = rummage::text::encode_markdown(result);
    CHECK(wire.size() <= rummage::text::kMarkdownMaxPayload);
}

TEST_CASE("markdown: wire round trip rejects malformed, oversized and unsorted content") {
    const auto document =
        parse({"# Title", "| A | B |", "| --- | --- |", "| one | two |"}).markdown;
    const auto wire = rummage::text::encode_markdown(document);
    rummage::text::Markdown output;
    REQUIRE(rummage::text::decode_markdown(wire, output));
    CHECK(output == document);
    for (std::size_t cut = 1; cut < wire.size(); ++cut)
        CHECK_FALSE(rummage::text::decode_markdown(std::span{wire}.first(cut), output));
    auto invalid = wire;
    invalid[0] = std::byte{255};
    invalid[1] = std::byte{255};
    CHECK_FALSE(rummage::text::decode_markdown(invalid, output));
    invalid = wire;
    invalid[8] = std::byte{255};  // block kind
    CHECK_FALSE(rummage::text::decode_markdown(invalid, output));
    invalid = wire;
    invalid.push_back(std::byte{0});
    CHECK_FALSE(rummage::text::decode_markdown(invalid, output));
    auto malformed = document;
    malformed.blocks[0].runs[0].style = 0xFFFF;
    CHECK_FALSE(rummage::text::decode_markdown(rummage::text::encode_markdown(malformed), output));
    malformed = document;
    malformed.blocks[0].runs[0].text = std::string("x\0y", 3);
    CHECK_FALSE(rummage::text::decode_markdown(rummage::text::encode_markdown(malformed), output));
}

TEST_CASE("markdown: escaped delimiters, Unicode and ragged tables keep their content") {
    const auto value = parse({"*one \\* two* and ***both*** café ☕", "", "| A | B | C |",
                              "| --- | --- | --- |", "| only one |", "| x | y | |"});
    REQUIRE_FALSE(value.markdown.blocks.empty());
    CHECK(plain(value.markdown.blocks[0]) == "one * two and both café ☕");
    CHECK((value.markdown.blocks[0].runs[0].style & rummage::text::md_italic) != 0);
    const auto& last = value.markdown.blocks.back();
    REQUIRE_FALSE(last.runs.empty());
    CHECK(last.runs.back().cell == 2);
    const auto& short_row = value.markdown.blocks[value.markdown.blocks.size() - 2];
    REQUIRE_FALSE(short_row.runs.empty());
    CHECK(short_row.runs.back().cell == 2);
}
