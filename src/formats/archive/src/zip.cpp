// zip (PKWARE's APPNOTE 6.3): the central directory at the end is the archive's list; each entry's
// local header is checked against it, and no two entries may share bytes. Stored and deflated
// entries are read; others are listed as a method rummage does not read yet, encrypted ones as
// encrypted.

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <zlib.h>

#include "readers.hpp"

namespace rummage::formats::archive::detail {

using rummage::archive::Entry;
using rummage::archive::Failure;
using rummage::archive::Limits;
using rummage::archive::Listing;
using rummage::archive::Type;
using rummage::archive::Unreadable;

namespace {

constexpr std::uint32_t kLocalMagic = 0x04034B50;
constexpr std::uint32_t kCentralMagic = 0x02014B50;
constexpr std::uint32_t kEndMagic = 0x06054B50;
constexpr std::uint32_t kEnd64Magic = 0x06064B50;
constexpr std::uint32_t kLocator64Magic = 0x07064B50;
constexpr std::size_t kEndSize = 22;
constexpr std::size_t kEnd64Size = 56;
constexpr std::size_t kLocator64Size = 20;
constexpr std::size_t kCentralSize = 46;
constexpr std::size_t kLocalSize = 30;
constexpr std::size_t kMaxComment = 0xFFFF;
constexpr std::uint16_t kStored = 0;
constexpr std::uint16_t kDeflated = 8;
constexpr std::uint16_t kAesMethod = 99;  // WinZip's AES: the real method is inside, encrypted
constexpr std::uint16_t kEncryptedFlag = 0x0001;
constexpr std::uint16_t kUtf8Flag = 0x0800;
constexpr std::uint64_t kMaxLinkBytes = 4096;
constexpr std::uint64_t kNtfsTicks = 10'000'000;         // 100 ns in a second
constexpr std::uint64_t kNtfsFrom1601 = 11'644'473'600;  // seconds from 1601 to 1970
// The central directory is read whole: a million entries with long names fit well inside.
constexpr std::uint64_t kMaxDirectoryBytes = std::uint64_t{512} * 1024 * 1024;

constexpr std::uint32_t kTypeMask = 0170000;
constexpr std::uint32_t kSymlinkType = 0120000;
constexpr std::uint32_t kDirectoryType = 0040000;
constexpr std::uint32_t kFileType = 0100000;

// CP437's upper half, the code page old zip tools wrote names in when they did not say UTF-8.
constexpr std::array<char16_t, 128> kCp437{
    u'Ç', u'ü', u'é', u'â', u'ä', u'à', u'å', u'ç', u'ê', u'ë', u'è', u'ï', u'î', u'ì', u'Ä', u'Å',
    u'É', u'æ', u'Æ', u'ô', u'ö', u'ò', u'û', u'ù', u'ÿ', u'Ö', u'Ü', u'¢', u'£', u'¥', u'₧', u'ƒ',
    u'á', u'í', u'ó', u'ú', u'ñ', u'Ñ', u'ª', u'º', u'¿', u'⌐', u'¬', u'½', u'¼', u'¡', u'«', u'»',
    u'░', u'▒', u'▓', u'│', u'┤', u'╡', u'╢', u'╖', u'╕', u'╣', u'║', u'╗', u'╝', u'╜', u'╛', u'┐',
    u'└', u'┴', u'┬', u'├', u'─', u'┼', u'╞', u'╟', u'╚', u'╔', u'╩', u'╦', u'╠', u'═', u'╬', u'╧',
    u'╨', u'╤', u'╥', u'╙', u'╘', u'╒', u'╓', u'╫', u'╪', u'┘', u'┌', u'█', u'▄', u'▌', u'▐', u'▀',
    u'α', u'ß', u'Γ', u'π', u'Σ', u'σ', u'µ', u'τ', u'Φ', u'Θ', u'Ω', u'δ', u'∞', u'φ', u'ε', u'∩',
    u'≡', u'±', u'≥', u'≤', u'⌠', u'⌡', u'÷', u'≈', u'°', u'∙', u'·', u'√', u'ⁿ', u'²', u'■', u' ',
};

void append_utf8(std::string& out, char32_t c) {
    if (c < 0x80) {
        out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (c >> 6U)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xE0 | (c >> 12U)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3FU)));
    }
}

bool valid_utf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::size_t extra = 0;
        char32_t value = 0;
        if (c < 0x80) {
            ++i;
            continue;
        }
        if ((c & 0xE0U) == 0xC0) {
            extra = 1;
            value = c & 0x1FU;
        } else if ((c & 0xF0U) == 0xE0) {
            extra = 2;
            value = c & 0x0FU;
        } else if ((c & 0xF8U) == 0xF0) {
            extra = 3;
            value = c & 0x07U;
        } else {
            return false;
        }
        if (extra >= text.size() - i) {
            return false;  // cut short
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0U) != 0x80) {
                return false;
            }
            value = (value << 6U) | (next & 0x3FU);
        }
        static constexpr std::array<char32_t, 4> kSmallest{0, 0x80, 0x800, 0x10000};
        if (value < kSmallest.at(extra) || value > 0x10FFFF ||
            (value >= 0xD800 && value < 0xE000)) {
            return false;  // overlong, out of range or a surrogate
        }
        i += extra + 1;
    }
    return true;
}

std::string from_cp437(std::string_view bytes) {
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const char byte : bytes) {
        const auto c = static_cast<unsigned char>(byte);
        append_utf8(out, c < 0x80 ? char32_t{c} : char32_t{kCp437.at(c - 0x80U)});
    }
    return out;
}

// Where the central directory is: its first record's offset in the file, its size, how many
// records it says it has.
struct Directory {
    std::uint64_t offset{0};
    std::uint64_t size{0};
    std::uint64_t count{0};
    std::uint64_t shift{0};  // bytes before the archive (a self-extractor's program)
};

Directory find_directory(const File& file) {
    if (file.size() < kEndSize) {
        damaged("too short to be a zip");
    }
    const std::uint64_t tail_size = std::min<std::uint64_t>(file.size(), kEndSize + kMaxComment);
    const std::uint64_t tail_at = file.size() - tail_size;
    std::vector<std::uint8_t> tail(tail_size);
    file.read_exactly(tail_at, tail, "its end");
    // The last end record whose comment fits in the file.
    std::size_t end = tail.size();
    for (std::size_t at = tail.size() - kEndSize + 1; at-- > 0;) {
        if (le32(tail, at) == kEndMagic && at + kEndSize + le16(tail, at + 20) <= tail.size()) {
            end = at;
            break;
        }
    }
    if (end == tail.size()) {
        damaged("its directory is missing");
    }
    const std::span<const std::uint8_t> record = std::span{tail}.subspan(end, kEndSize);
    Directory directory{.offset = le32(record, 16),
                        .size = le32(record, 12),
                        .count = le16(record, 10),
                        .shift = 0};
    std::uint64_t directory_end = tail_at + end;  // where the directory must stop
    const bool wide =
        directory.offset == 0xFFFFFFFF || directory.size == 0xFFFFFFFF || directory.count == 0xFFFF;
    if (wide && end >= kLocator64Size && le32(tail, end - kLocator64Size) == kLocator64Magic) {
        const std::uint64_t record_at = le64(tail, end - kLocator64Size + 8);
        std::array<std::uint8_t, kEnd64Size> wide_record{};
        // The record sits right before the locator; its stated offset may be shifted.
        const std::uint64_t locator_at = tail_at + end - kLocator64Size;
        if (locator_at < kEnd64Size) {
            damaged("its ZIP64 directory is missing");
        }
        const std::uint64_t actual_at = locator_at - kEnd64Size;
        file.read_exactly(actual_at, wide_record, "its ZIP64 directory");
        if (le32(wide_record, 0) != kEnd64Magic || record_at > actual_at) {
            damaged("its ZIP64 directory is missing");
        }
        directory.count = le64(wide_record, 32);
        directory.size = le64(wide_record, 40);
        directory.offset = le64(wide_record, 48);
        directory_end = actual_at;
    } else if (le16(record, 4) != 0 || le16(record, 6) != 0) {
        damaged("it is split over several files");
    }
    if (directory.size > directory_end) {
        damaged("its directory is larger than the file");
    }
    const std::uint64_t actual = directory_end - directory.size;
    if (directory.offset > actual) {
        damaged("its directory is not where it says");
    }
    directory.shift = actual - directory.offset;
    directory.offset = actual;
    return directory;
}

// One central directory record, parsed.
struct Central {
    std::uint16_t made_by{0};
    std::uint16_t flags{0};
    std::uint16_t method{0};
    std::uint16_t dos_time{0};
    std::uint16_t dos_date{0};
    std::uint32_t crc{0};
    std::uint64_t packed{0};
    std::uint64_t size{0};
    std::uint64_t local{0};  // the local header's offset in the file (shift added)
    std::uint32_t external{0};
    std::string_view raw_name;
    std::string unicode_name;  // Info-ZIP's Unicode path extra field, when its CRC matches
    std::optional<std::int64_t> unix_time;
    std::size_t length{0};  // of the whole record
};

Central parse_central(std::span<const std::uint8_t> bytes, std::uint64_t shift) {
    if (bytes.size() < kCentralSize || le32(bytes, 0) != kCentralMagic) {
        damaged("its directory is broken");
    }
    Central central;
    central.made_by = le16(bytes, 4);
    central.flags = le16(bytes, 8);
    central.method = le16(bytes, 10);
    central.dos_time = le16(bytes, 12);
    central.dos_date = le16(bytes, 14);
    central.crc = le32(bytes, 16);
    central.packed = le32(bytes, 20);
    central.size = le32(bytes, 24);
    const std::size_t name_length = le16(bytes, 28);
    const std::size_t extra_length = le16(bytes, 30);
    const std::size_t comment_length = le16(bytes, 32);
    const std::uint32_t disk = le16(bytes, 34);
    central.external = le32(bytes, 38);
    std::uint64_t local = le32(bytes, 42);
    central.length = kCentralSize + name_length + extra_length + comment_length;
    if (central.length > bytes.size()) {
        damaged("its directory is broken");
    }
    const std::span<const std::uint8_t> name = bytes.subspan(kCentralSize, name_length);
    central.raw_name = {reinterpret_cast<const char*>(name.data()),  // NOLINT: bytes as text
                        name.size()};
    // Extra fields: ZIP64 sizes, Unix times, a Unicode name.
    const std::span<const std::uint8_t> extra =
        bytes.subspan(kCentralSize + name_length, extra_length);
    bool wide_disk = disk == 0xFFFF;
    for (std::size_t at = 0; at + 4 <= extra.size();) {
        const std::uint16_t id = le16(extra, at);
        const std::size_t length = le16(extra, at + 2);
        if (at + 4 + length > extra.size()) {
            break;  // a last field cut short: left alone, as unzip does
        }
        const std::span<const std::uint8_t> field = extra.subspan(at + 4, length);
        if (id == 0x0001) {
            std::size_t next = 0;
            const auto take = [&](std::uint64_t& value) {
                if (next + 8 > field.size()) {
                    damaged("its ZIP64 sizes are missing");
                }
                value = le64(field, next);
                next += 8;
            };
            if (central.size == 0xFFFFFFFF) {
                take(central.size);
            }
            if (central.packed == 0xFFFFFFFF) {
                take(central.packed);
            }
            if (local == 0xFFFFFFFF) {
                take(local);
            }
            if (wide_disk && next + 4 <= field.size()) {
                wide_disk = le32(field, next) != 0;
            }
        } else if (id == 0x5455 && length >= 5 && (field[0] & 1U) != 0) {
            central.unix_time = static_cast<std::int32_t>(le32(field, 1));
        } else if (id == 0x000A && length >= 32 && le16(field, 4) == 1 && le16(field, 6) >= 24 &&
                   !central.unix_time) {
            // NTFS times (7-Zip, Windows): 100 ns since 1601, in UTC.
            const std::uint64_t seconds = le64(field, 8) / kNtfsTicks;
            if (seconds >= kNtfsFrom1601) {
                central.unix_time = static_cast<std::int64_t>(seconds - kNtfsFrom1601);
            }
        } else if (id == 0x7075 && length >= 5 && field[0] == 1) {
            const auto name_crc =
                static_cast<std::uint32_t>(crc32(0, name.data(), static_cast<uInt>(name.size())));
            if (le32(field, 1) == name_crc) {
                const std::span<const std::uint8_t> text = field.subspan(5);
                central.unicode_name.assign(text.begin(), text.end());
            }
        }
        at += 4 + length;
    }
    if ((disk != 0 && disk != 0xFFFF) || (disk == 0xFFFF && wide_disk)) {
        damaged("it is split over several files");
    }
    if (local > UINT64_MAX - shift) {
        damaged("its directory is broken");
    }
    central.local = local + shift;
    return central;
}

// The offset of an entry's data, after checking its local header against its record.
std::uint64_t data_start(const File& file, const Central& central) {
    std::array<std::uint8_t, kLocalSize> local{};
    file.read_exactly(central.local, local, "an entry's header");
    if (le32(local, 0) != kLocalMagic) {
        damaged("an entry's header is missing");
    }
    const std::size_t name_length = le16(local, 26);
    const std::size_t extra_length = le16(local, 28);
    if (name_length != central.raw_name.size()) {
        damaged("an entry's names do not match");
    }
    std::vector<std::uint8_t> name(name_length);
    file.read_exactly(central.local + kLocalSize, name, "an entry's header");
    if (!std::ranges::equal(name, central.raw_name, {}, {},
                            [](char c) { return static_cast<std::uint8_t>(c); })) {
        damaged("an entry's names do not match");
    }
    return central.local + kLocalSize + name_length + extra_length;
}

bool is_encrypted(const Central& central) {
    return (central.flags & kEncryptedFlag) != 0 || central.method == kAesMethod;
}

// Writes the entry's bytes into sink, checked against its size and CRC-32.
void read_central(const File& file, const Central& central, Sink& sink) {
    if (is_encrypted(central)) {
        throw Problem{Failure::unreadable, "it is encrypted"};
    }
    const std::uint64_t start = data_start(file, central);
    if (start > file.size() || central.packed > file.size() - start) {
        damaged("an entry is cut short");
    }
    std::uint32_t crc = 0;
    if (central.method == kStored) {
        if (central.packed != central.size) {
            damaged("a stored entry's sizes do not match");
        }
        FileStream stream{file, start, start + central.packed};
        copy(stream, central.size, sink, &crc);
    } else if (central.method == kDeflated) {
        Inflater stream{file, start, start + central.packed, Inflater::Wrapping::raw};
        copy(stream, central.size, sink, &crc);
        std::array<std::uint8_t, 1> more{};
        if (stream.read(more) != 0) {
            damaged("an entry is larger than it says");
        }
    } else {
        throw Problem{Failure::unreadable, "its compression is not read yet"};
    }
    if (crc != central.crc) {
        damaged("an entry's bytes do not match its checksum");
    }
}

std::int64_t dos_seconds(std::uint16_t date, std::uint16_t time) {
    const auto field = [](unsigned value, unsigned low, unsigned high) {
        return static_cast<int>(std::clamp(value, low, high));
    };
    return epoch_seconds(1980 + static_cast<int>(date >> 9U), field((date >> 5U) & 0x0FU, 1, 12),
                         field(date & 0x1FU, 1, 31), field(time >> 11U, 0, 23),
                         field((time >> 5U) & 0x3FU, 0, 59), field((time & 0x1FU) * 2U, 0, 59));
}

Entry entry_of(const File& file, const Central& central, std::uint64_t locator) {
    Entry entry;
    entry.locator = locator;
    entry.size = central.size;
    entry.packed = central.packed;
    const unsigned host = central.made_by >> 8U;
    if ((central.flags & kUtf8Flag) != 0 || valid_utf8(central.raw_name)) {
        entry.path = central.raw_name;
    } else if (!central.unicode_name.empty()) {
        entry.path = central.unicode_name;
    } else {
        entry.path = from_cp437(central.raw_name);
    }
    if (host == 0 || host == 10 || host == 11 || host == 14) {  // DOS, NTFS, VFAT
        std::ranges::replace(entry.path, '\\', '/');
    }
    const bool unix_host = host == 3 || host == 19;  // Unix, OS X
    const std::uint32_t unix_mode = central.external >> 16U;
    const std::uint32_t kind = unix_mode & kTypeMask;
    if (unix_host && kind == kSymlinkType) {
        entry.type = Type::symlink;
    } else if (entry.path.ends_with('/') || (unix_host && kind == kDirectoryType) ||
               (!unix_host && (central.external & 0x10U) != 0)) {
        entry.type = Type::directory;
    } else if (unix_host && kind != 0 && kind != kFileType) {
        entry.type = Type::other;
    }
    if (unix_host) {
        entry.mode = unix_mode & 07777U;
    }
    entry.mtime = central.unix_time.value_or(dos_seconds(central.dos_date, central.dos_time));
    if (entry.type == Type::other) {
        entry.unreadable = Unreadable::kind;
    } else if (is_encrypted(central)) {
        entry.unreadable = Unreadable::encrypted;
    } else if (central.method != kStored && central.method != kDeflated) {
        entry.unreadable = Unreadable::method;
    }
    if (entry.type == Type::symlink && entry.unreadable == Unreadable::none) {
        if (central.size > kMaxLinkBytes) {
            damaged("a link's target is too long");
        }
        MemorySink target{kMaxLinkBytes};
        read_central(file, central, target);
        entry.link.assign(target.bytes().begin(), target.bytes().end());
    }
    return entry;
}

}  // namespace

void list_zip(const File& file, const Limits& limits, Listing& listing) {
    listing.format = rummage::archive::Format::zip;
    const Directory directory = find_directory(file);
    if (directory.count > limits.max_entries) {
        throw Problem{Failure::too_large, "it has more entries than rummage lists"};
    }
    if (directory.size > kMaxDirectoryBytes) {
        throw Problem{Failure::too_large, "its directory is larger than rummage reads"};
    }
    std::vector<std::uint8_t> records(directory.size);
    file.read_exactly(directory.offset, records, "its directory");

    struct Span {
        std::uint64_t begin;
        std::uint64_t end;
    };

    std::vector<Span> spans;
    spans.reserve(directory.count);
    std::size_t at = 0;
    while (at < records.size()) {
        if (listing.entries.size() >= limits.max_entries) {
            throw Problem{Failure::too_large, "it has more entries than rummage lists"};
        }
        const Central central = parse_central(std::span{records}.subspan(at), directory.shift);
        if (central.raw_name.size() > limits.max_path_bytes) {
            throw Problem{Failure::too_large, "an entry's name is too long"};
        }
        const std::uint64_t start = data_start(file, central);
        if (start > directory.offset || central.packed > directory.offset - start) {
            damaged("an entry reaches into the directory");
        }
        spans.push_back({central.local, start + central.packed});
        listing.entries.push_back(entry_of(file, central, directory.offset + at));
        at += central.length;
    }
    if (listing.entries.size() != directory.count) {
        damaged("its directory does not hold the entries it says");
    }
    std::ranges::sort(spans, {}, &Span::begin);
    for (std::size_t i = 1; i < spans.size(); ++i) {
        if (spans[i].begin < spans[i - 1].end) {
            damaged("two entries share their bytes");
        }
    }
}

void read_zip(const File& file, const Entry& entry, Sink& sink) {
    const Directory directory = find_directory(file);
    if (entry.locator < directory.offset || entry.locator - directory.offset >= directory.size) {
        damaged("no such entry");
    }
    const std::uint64_t at = entry.locator - directory.offset;
    std::vector<std::uint8_t> record(static_cast<std::size_t>(
        std::min<std::uint64_t>(directory.size - at, kCentralSize + (std::size_t{3} * 0xFFFFU))));
    file.read_exactly(entry.locator, record, "its directory");
    const Central central = parse_central(record, directory.shift);
    read_central(file, central, sink);
}

}  // namespace rummage::formats::archive::detail
