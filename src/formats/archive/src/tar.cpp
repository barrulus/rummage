// tar (POSIX ustar and pax, GNU's long names): 512-byte headers, each followed by its entry's data
// rounded up to 512 bytes, until a block of zeros. Read in order, so the same code lists a plain
// tar (from the file) and one inside gzip (from the inflater).

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "readers.hpp"

namespace rummage::formats::archive::detail {

using rummage::archive::Entry;
using rummage::archive::Failure;
using rummage::archive::Limits;
using rummage::archive::Listing;
using rummage::archive::Type;
using rummage::archive::Unreadable;

namespace {

constexpr std::size_t kBlock = 512;
// A pax header's or a GNU long name's data, at most: real ones are a few hundred bytes.
constexpr std::uint64_t kMaxMeta = std::uint64_t{1} * 1024 * 1024;

std::string_view text_of(std::span<const std::uint8_t> field) {
    const auto end = std::ranges::find(field, std::uint8_t{0});
    return {reinterpret_cast<const char*>(field.data()),  // NOLINT: bytes as text
            static_cast<std::size_t>(end - field.begin())};
}

// An octal number (spaces and NULs around it), or GNU's base-256 (first bit set); nullopt when
// it is neither, negative or too large.
std::optional<std::uint64_t> number(std::span<const std::uint8_t> field) {
    if (field.empty()) {
        return 0;
    }
    if ((field[0] & 0x80U) != 0) {
        if ((field[0] & 0x40U) != 0) {
            return std::nullopt;  // negative
        }
        std::uint64_t value = field[0] & 0x3FU;
        for (const std::uint8_t byte : field.subspan(1)) {
            if (value > (UINT64_MAX >> 8U)) {
                return std::nullopt;
            }
            value = (value << 8U) | byte;
        }
        return value;
    }
    std::size_t at = 0;
    while (at < field.size() && (field[at] == ' ' || field[at] == 0)) {
        ++at;
    }
    std::uint64_t value = 0;
    for (; at < field.size() && field[at] >= '0' && field[at] <= '7'; ++at) {
        if (value > (UINT64_MAX >> 3U)) {
            return std::nullopt;
        }
        value = (value << 3U) | static_cast<std::uint64_t>(field[at] - '0');
    }
    for (; at < field.size(); ++at) {
        if (field[at] != ' ' && field[at] != 0) {
            return std::nullopt;
        }
    }
    return value;
}

// A decimal number of a pax record ("123", "-4", "1700000000.25": the whole seconds).
std::optional<std::int64_t> decimal(std::string_view text, bool fraction_allowed) {
    bool negative = false;
    if (!text.empty() && text.front() == '-') {
        negative = true;
        text.remove_prefix(1);
    }
    if (fraction_allowed) {
        text = text.substr(0, text.find('.'));
    }
    if (text.empty() || text.size() > 18) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        value = (value * 10) + (c - '0');
    }
    return negative ? -value : value;
}

// What pax headers and GNU long names say about the next entry.
struct Pending {
    std::optional<std::string> path;
    std::optional<std::string> link;
    std::optional<std::uint64_t> size;
    std::optional<std::int64_t> mtime;
    bool sparse{false};
};

void parse_pax(std::string_view data, Pending& pending) {
    while (!data.empty()) {
        const std::size_t space = data.find(' ');
        if (space == std::string_view::npos) {
            damaged("an extended header is broken");
        }
        const std::optional<std::int64_t> length = decimal(data.substr(0, space), false);
        if (!length || *length <= static_cast<std::int64_t>(space) + 1 ||
            static_cast<std::uint64_t>(*length) > data.size()) {
            damaged("an extended header is broken");
        }
        const std::string_view record = data.substr(0, static_cast<std::size_t>(*length));
        if (record.back() != '\n') {
            damaged("an extended header is broken");
        }
        const std::string_view body = record.substr(space + 1, record.size() - space - 2);
        const std::size_t equals = body.find('=');
        if (equals == std::string_view::npos) {
            damaged("an extended header is broken");
        }
        const std::string_view key = body.substr(0, equals);
        const std::string_view value = body.substr(equals + 1);
        if (key == "path") {
            pending.path = std::string{value};
        } else if (key == "linkpath") {
            pending.link = std::string{value};
        } else if (key == "size") {
            const std::optional<std::int64_t> size = decimal(value, false);
            if (!size || *size < 0) {
                damaged("an extended header is broken");
            }
            pending.size = static_cast<std::uint64_t>(*size);
        } else if (key == "mtime") {
            pending.mtime = decimal(value, true);
        } else if (key.starts_with("GNU.sparse.")) {
            pending.sparse = true;
        }
        data.remove_prefix(record.size());
    }
}

std::vector<std::uint8_t> read_meta(Stream& stream, std::uint64_t size) {
    if (size > kMaxMeta) {
        damaged("an extended header is too long");
    }
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    if (!stream.read_exactly(data)) {
        damaged("an extended header is cut short");
    }
    return data;
}

std::uint64_t padding(std::uint64_t size) { return (kBlock - (size % kBlock)) % kBlock; }

void skip(Stream& stream, std::uint64_t count) {
    if (!stream.skip(count)) {
        damaged("an entry is cut short");
    }
}

}  // namespace

bool looks_like_tar(std::span<const std::uint8_t> block) {
    if (block.size() < kBlock ||
        std::ranges::all_of(block.first(kBlock), [](std::uint8_t b) { return b == 0; })) {
        return false;
    }
    const std::optional<std::uint64_t> stored = number(block.subspan(148, 8));
    if (!stored) {
        return false;
    }
    std::uint64_t unsigned_sum = 0;
    std::int64_t signed_sum = 0;
    for (std::size_t i = 0; i < kBlock; ++i) {
        const std::uint8_t byte = (i >= 148 && i < 156) ? std::uint8_t{' '} : block[i];
        unsigned_sum += byte;
        signed_sum += static_cast<std::int8_t>(byte);
    }
    return *stored == unsigned_sum || std::cmp_equal(*stored, signed_sum);
}

void list_tar(Stream& stream, const Limits& limits, Listing& listing) {
    std::array<std::uint8_t, kBlock> header{};
    Pending pending;
    while (true) {
        if (!stream.read_exactly(header)) {
            return;  // no end blocks: GNU tar takes it too
        }
        if (std::ranges::all_of(header, [](std::uint8_t b) { return b == 0; })) {
            return;  // the end
        }
        if (!looks_like_tar(header)) {
            damaged("a header is broken");
        }
        const std::span<const std::uint8_t> h{header};
        const char flag = static_cast<char>(header[156]);
        const std::optional<std::uint64_t> stated_size = number(h.subspan(124, 12));
        if (!stated_size) {
            damaged("a header is broken");
        }
        std::uint64_t size = *stated_size;
        if (flag == 'x' || flag == 'g' || flag == 'L' || flag == 'K') {
            const std::vector<std::uint8_t> data = read_meta(stream, size);
            skip(stream, padding(size));
            if (flag == 'x') {
                parse_pax({reinterpret_cast<const char*>(data.data()),  // NOLINT: bytes as text
                           data.size()},
                          pending);
            } else if (flag == 'L') {
                pending.path = std::string{text_of(data)};
            } else if (flag == 'K') {
                pending.link = std::string{text_of(data)};
            }
            continue;  // 'g': global settings, nothing rummage uses
        }
        if (pending.size) {
            size = *pending.size;
        }
        if (flag == 'V') {  // a volume's label: not an entry
            skip(stream, size + padding(size));
            pending = {};
            continue;
        }
        Entry entry;
        const bool ustar = std::string_view{reinterpret_cast<const char*>(&header[257]),  // NOLINT
                                            6} == std::string_view{"ustar\0", 6};
        if (pending.path) {
            entry.path = *pending.path;
        } else {
            const std::string_view prefix = ustar ? text_of(h.subspan(345, 155)) : "";
            entry.path = prefix.empty()
                             ? std::string{text_of(h.subspan(0, 100))}
                             : std::string{prefix} + "/" + std::string{text_of(h.subspan(0, 100))};
        }
        entry.link = pending.link ? *pending.link : std::string{text_of(h.subspan(157, 100))};
        const std::optional<std::uint64_t> mode = number(h.subspan(100, 8));
        entry.mode = static_cast<std::uint32_t>(mode.value_or(0) & 07777U);
        const std::optional<std::uint64_t> mtime = number(h.subspan(136, 12));
        entry.mtime =
            pending.mtime.value_or(mtime && *mtime <= static_cast<std::uint64_t>(INT64_MAX)
                                       ? static_cast<std::int64_t>(*mtime)
                                       : 0);
        bool data = false;  // whether size bytes of data follow
        switch (flag) {
            case '0':
            case '\0':
            case '7':
                entry.type = entry.path.ends_with('/') ? Type::directory : Type::file;
                data = true;
                break;
            case '1':
                entry.type = Type::hardlink;
                break;
            case '2':
                entry.type = Type::symlink;
                break;
            case '5':
                entry.type = Type::directory;
                break;
            case 'D':  // GNU's directory with a list of its names: the list is skipped
                entry.type = Type::directory;
                data = true;
                break;
            case '3':
            case '4':
            case '6':
                entry.type = Type::other;
                break;
            default:  // 'S' (sparse), 'M' (continued from another volume), unknown ones
                entry.type = Type::other;
                data = true;
                break;
        }
        if (pending.sparse) {
            entry.type = Type::other;
        }
        if (entry.type == Type::other) {
            entry.unreadable = Unreadable::kind;
        }
        pending = {};
        if (entry.path.empty()) {
            damaged("an entry has no name");
        }
        if (entry.path.size() > limits.max_path_bytes ||
            entry.link.size() > limits.max_path_bytes) {
            throw Problem{Failure::too_large, "an entry's name is too long"};
        }
        if (listing.entries.size() >= limits.max_entries) {
            throw Problem{Failure::too_large, "it has more entries than rummage lists"};
        }
        entry.locator = stream.offset();
        if (data) {
            if (entry.type == Type::file) {
                entry.size = size;
                entry.packed = size;
            }
            if (size > UINT64_MAX - kBlock) {
                damaged("a header is broken");
            }
            skip(stream, size + padding(size));
        }
        listing.entries.push_back(std::move(entry));
    }
}

}  // namespace rummage::formats::archive::detail
