#include "rummage/text/markdown.hpp"

#include <algorithm>
#include <string_view>

namespace rummage::text {
namespace {
void number(std::vector<std::byte>& out, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<std::byte>((value >> shift) & 255U));
}

void string(std::vector<std::byte>& out, std::string_view value) {
    number(out, static_cast<std::uint32_t>(value.size()));
    for (const char c : value)
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
}

struct Reader {
    std::span<const std::byte> bytes;
    std::size_t at{0};
    bool valid{true};

    std::uint32_t number() {
        if (at > bytes.size() || bytes.size() - at < 4) {
            valid = false;
            return 0;
        }
        std::uint32_t result = 0;
        for (unsigned shift = 0; shift < 32; shift += 8)
            result |= std::to_integer<std::uint32_t>(bytes[at++]) << shift;
        return result;
    }

    std::string string(std::size_t limit) {
        const auto size = number();
        if (!valid || size > limit || size > bytes.size() - at) {
            valid = false;
            return {};
        }
        std::string result;
        result.reserve(size);
        for (std::size_t i = 0; i < size; ++i) {
            const char c = static_cast<char>(bytes[at++]);
            if (c == '\0' || c == '\n' || c == '\r')
                valid = false;
            result.push_back(c);
        }
        return result;
    }
};
}  // namespace

std::vector<std::byte> encode_markdown(const Markdown& document) {
    if (document.blocks.empty())
        return {};
    std::vector<std::byte> out;
    number(out, static_cast<std::uint32_t>(document.blocks.size()));
    number(out, document.truncated ? 1U : 0U);
    for (const auto& block : document.blocks) {
        number(out, static_cast<std::uint32_t>(block.kind));
        number(out, block.level);
        string(out, block.marker);
        number(out, static_cast<std::uint32_t>(block.runs.size()));
        for (const auto& run : block.runs) {
            number(out, run.style);
            number(out, run.cell);
            string(out, run.text);
        }
    }
    return out;
}

bool decode_markdown(std::span<const std::byte> bytes, Markdown& document) {
    document = {};
    if (bytes.empty())
        return true;
    if (bytes.size() > kMarkdownMaxPayload)
        return false;
    Reader read{bytes};
    const auto count = read.number();
    const auto truncated = read.number();
    if (!read.valid || count > kMarkdownMaxBlocks || truncated > 1)
        return false;
    Markdown result;
    result.truncated = truncated != 0;
    std::size_t runs = 0;
    std::size_t text = 0;
    for (std::uint32_t i = 0; i < count && read.valid; ++i) {
        const auto kind = read.number();
        const auto level = read.number();
        if (kind > static_cast<std::uint32_t>(MarkdownKind::gap) || level > 8)
            return false;
        MarkdownBlock block{.kind = static_cast<MarkdownKind>(kind),
                            .level = static_cast<std::uint8_t>(level),
                            .marker = read.string(32),
                            .runs = {}};
        const auto size = read.number();
        if (size > kMarkdownMaxRuns - runs)
            return false;
        runs += size;
        std::uint8_t last_cell = 0;
        for (std::uint32_t j = 0; j < size && read.valid; ++j) {
            const auto style = read.number();
            const auto cell = read.number();
            if ((style & 0xF0U) != 0 || (style >> 8U) > 8 || cell > 15 || cell < last_cell)
                return false;
            last_cell = static_cast<std::uint8_t>(cell);
            auto value = read.string(kMarkdownMaxText - text);
            text += value.size();
            block.runs.push_back({std::move(value), static_cast<std::uint16_t>(style), last_cell});
        }
        result.blocks.push_back(std::move(block));
    }
    if (!read.valid || read.at != bytes.size())
        return false;
    document = std::move(result);
    return true;
}
}  // namespace rummage::text
