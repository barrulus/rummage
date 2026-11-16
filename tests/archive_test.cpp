// The archive family (ROADMAP 0.9.0): real archives made by GNU tar, gzip, 7-Zip and Python
// (tests/data/archive/make-fixtures.sh), and broken or hostile ones made here byte by byte.
#include "rummage/archive/archive.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <zlib.h>

#include <doctest/doctest.h>

#include "rummage/formats/archive/archive.hpp"

using rummage::archive::Entry;
using rummage::archive::Failure;
using rummage::archive::Format;
using rummage::archive::Limits;
using rummage::archive::Listing;
using rummage::archive::Type;
using rummage::archive::Unreadable;
namespace engine = rummage::formats::archive;
namespace fs = std::filesystem;

namespace {

const fs::path kData = fs::path{RUMMAGE_TEST_DATA} / "archive";

// A descriptor holding bytes, as the helper gets a file.
class Memfd {
public:
    explicit Memfd(std::string_view bytes) : fd_{::memfd_create("archive-test", MFD_CLOEXEC)} {
        REQUIRE(fd_ >= 0);
        REQUIRE(::write(fd_, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
    }

    ~Memfd() { ::close(fd_); }

    Memfd(const Memfd&) = delete;
    Memfd& operator=(const Memfd&) = delete;
    Memfd(Memfd&&) = delete;
    Memfd& operator=(Memfd&&) = delete;

    [[nodiscard]] int fd() const { return fd_; }

    // What was written to it (from the start).
    [[nodiscard]] std::string bytes() const {
        std::string out;
        std::array<char, 4096> buffer{};
        off_t at = 0;
        ssize_t got = 0;
        while ((got = ::pread(fd_, buffer.data(), buffer.size(), at)) > 0) {
            out.append(buffer.data(), static_cast<std::size_t>(got));
            at += got;
        }
        return out;
    }

private:
    int fd_;
};

std::string file_bytes(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    REQUIRE(in);
    std::string bytes(static_cast<std::size_t>(fs::file_size(path)), '\0');
    in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

Listing list_bytes(std::string_view bytes, const Limits& limits = {}) {
    const Memfd archive{bytes};
    return engine::list(archive.fd(), limits);
}

Listing list_file(const std::string& name) { return list_bytes(file_bytes(kData / name)); }

const Entry* find(const Listing& listing, std::string_view path) {
    const auto it = std::ranges::find(listing.entries, path, &Entry::path);
    return it == listing.entries.end() ? nullptr : &*it;
}

struct Got {
    rummage::archive::Read read;
    std::string bytes;
};

Got read_bytes(std::string_view archive_bytes, Format format, const Entry& entry,
               std::uint64_t max_bytes = UINT64_MAX) {
    const Memfd archive{archive_bytes};
    const Memfd out{""};
    Got got;
    got.read = engine::read_entry(archive.fd(), format, entry, out.fd(), max_bytes);
    got.bytes = out.bytes();
    return got;
}

// --- zip, made by hand ---

void put16(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>(value & 0xFFU));
    out.push_back(static_cast<char>((value >> 8U) & 0xFFU));
}

void put32(std::string& out, std::uint32_t value) {
    put16(out, value & 0xFFFFU);
    put16(out, value >> 16U);
}

std::string raw_deflate(std::string_view data) {
    z_stream z{};
    REQUIRE(deflateInit2(&z, 9, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    std::string out(deflateBound(&z, static_cast<uLong>(data.size())), '\0');
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));  // NOLINT: zlib's API
    z.avail_in = static_cast<uInt>(data.size());
    z.next_out = reinterpret_cast<Bytef*>(out.data());  // NOLINT: zlib's API
    z.avail_out = static_cast<uInt>(out.size());
    REQUIRE(deflate(&z, Z_FINISH) == Z_STREAM_END);
    out.resize(z.total_out);
    deflateEnd(&z);
    return out;
}

struct ZipItem {
    std::string name;
    std::string data;
    bool deflate{false};
    std::optional<std::uint32_t> said_size;  // a lie about its size
    std::optional<std::uint32_t> said_crc;
    std::optional<std::size_t> same_as;  // its record points at that item's local header
    std::uint16_t flags{0};
};

ZipItem zip_item(std::string name, std::string data, bool deflate = false) {
    ZipItem item;
    item.name = std::move(name);
    item.data = std::move(data);
    item.deflate = deflate;
    return item;
}

std::string make_zip(const std::vector<ZipItem>& items) {
    std::string out;
    std::vector<std::uint32_t> locals;
    std::vector<std::string> packed;
    for (const ZipItem& item : items) {
        const std::string data = item.deflate ? raw_deflate(item.data) : item.data;
        const auto crc = static_cast<std::uint32_t>(
            crc32(0, reinterpret_cast<const Bytef*>(item.data.data()),  // NOLINT: zlib's API
                  static_cast<uInt>(item.data.size())));
        locals.push_back(static_cast<std::uint32_t>(out.size()));
        put32(out, 0x04034B50);
        put16(out, 20);
        put16(out, item.flags);
        put16(out, item.deflate ? 8 : 0);
        put16(out, 0);
        put16(out, 0x5B6F);  // 2025-11-15
        put32(out, item.said_crc.value_or(crc));
        put32(out, static_cast<std::uint32_t>(data.size()));
        put32(out, item.said_size.value_or(static_cast<std::uint32_t>(item.data.size())));
        put16(out, static_cast<std::uint32_t>(item.name.size()));
        put16(out, 0);
        out += item.name;
        out += data;
        packed.push_back(data);
    }
    const auto directory = static_cast<std::uint32_t>(out.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        const ZipItem& item = items[i];
        const auto crc = static_cast<std::uint32_t>(
            crc32(0, reinterpret_cast<const Bytef*>(item.data.data()),  // NOLINT: zlib's API
                  static_cast<uInt>(item.data.size())));
        put32(out, 0x02014B50);
        put16(out, (3U << 8U) | 20U);  // made on Unix
        put16(out, 20);
        put16(out, item.flags);
        put16(out, item.deflate ? 8 : 0);
        put16(out, 0);
        put16(out, 0x5B6F);
        put32(out, item.said_crc.value_or(crc));
        put32(out, static_cast<std::uint32_t>(packed[i].size()));
        put32(out, item.said_size.value_or(static_cast<std::uint32_t>(item.data.size())));
        put16(out, static_cast<std::uint32_t>(item.name.size()));
        put16(out, 0);
        put16(out, 0);
        put16(out, 0);
        put16(out, 0);
        put32(out, 0100644U << 16U);
        put32(out, locals[item.same_as.value_or(i)]);
        out += item.name;
    }
    const auto directory_size = static_cast<std::uint32_t>(out.size()) - directory;
    put32(out, 0x06054B50);
    put16(out, 0);
    put16(out, 0);
    put16(out, static_cast<std::uint32_t>(items.size()));
    put16(out, static_cast<std::uint32_t>(items.size()));
    put32(out, directory_size);
    put32(out, directory);
    put16(out, 0);
    return out;
}

// --- tar, made by hand ---

std::string tar_header(std::string_view name, char flag, std::uint64_t size,
                       bool break_checksum = false) {
    std::string block(512, '\0');
    std::ranges::copy(name.substr(0, 100), block.begin());
    const auto octal = [&block](std::size_t at, std::size_t width, std::uint64_t value) {
        std::string text(width - 1, '0');
        for (std::size_t i = width - 1; i-- > 0 && value > 0; value >>= 3U) {
            text[i] = static_cast<char>('0' + (value & 7U));
        }
        std::ranges::copy(text, block.begin() + static_cast<std::ptrdiff_t>(at));
    };
    octal(100, 8, 0644);
    octal(108, 8, 0);
    octal(116, 8, 0);
    octal(124, 12, size);
    octal(136, 12, 1794744000);
    block[156] = flag;
    std::ranges::copy(std::string_view{"ustar\0"
                                       "00",
                                       8},
                      block.begin() + 257);
    std::ranges::fill(block.begin() + 148, block.begin() + 156, ' ');
    unsigned sum = 0;
    for (const char c : block) {
        sum += static_cast<unsigned char>(c);
    }
    if (break_checksum) {
        ++sum;
    }
    octal(148, 7, sum);
    return block;
}

std::string tar_file(std::string_view name, std::string_view data) {
    std::string out = tar_header(name, '0', data.size());
    out += data;
    out.append((512 - (data.size() % 512)) % 512, '\0');
    return out;
}

std::string tar_end() { return std::string(1024, '\0'); }

}  // namespace

TEST_CASE("archive: a zip from 7-Zip lists its entries and reads stored and deflated ones") {
    const std::string bytes = file_bytes(kData / "basic.zip");
    const Listing listing = list_bytes(bytes);
    REQUIRE(listing.failure == Failure::none);
    CHECK(listing.format == Format::zip);
    CHECK(listing.entries.size() == 6);
    const Entry* a = find(listing, "a.txt");
    REQUIRE(a != nullptr);
    CHECK(a->type == Type::file);
    CHECK(a->size == 13);
    CHECK(a->mtime == 1794744000);  // 2026-11-15 12:00 UTC, from 7-Zip's NTFS times
    CHECK(find(listing, "città.txt") != nullptr);
    const Entry* docs = find(listing, "docs/");
    REQUIRE(docs != nullptr);
    CHECK(docs->type == Type::directory);
    const std::string long_name = "docs/" + std::string(120, 'n') + ".txt";
    CHECK(find(listing, long_name) != nullptr);

    const Got got = read_bytes(bytes, listing.format, *a);
    CHECK(got.read.failure == Failure::none);
    CHECK(got.bytes == "hello from a\n");
    const Entry* repeat = find(listing, "docs/repeat.txt");
    REQUIRE(repeat != nullptr);
    CHECK(repeat->packed < repeat->size);  // deflated
    const Got deflated = read_bytes(bytes, listing.format, *repeat);
    CHECK(deflated.read.failure == Failure::none);
    CHECK(deflated.bytes.size() == 5120);
    CHECK(deflated.bytes.starts_with("the same line again\nthe same line again\n"));

    // Never more than allowed.
    const Got small = read_bytes(bytes, listing.format, *repeat, 100);
    CHECK(small.read.failure == Failure::too_large);
    CHECK(small.bytes.size() <= 100);
}

TEST_CASE("archive: encrypted and unknown methods are listed, said, never read") {
    for (const char* name : {"aes.zip", "zipcrypto.zip"}) {
        CAPTURE(name);
        const Listing listing = list_file(name);
        REQUIRE(listing.failure == Failure::none);
        REQUIRE(listing.entries.size() == 1);
        CHECK(listing.entries[0].unreadable == Unreadable::encrypted);
        const Got got = read_bytes(file_bytes(kData / name), listing.format, listing.entries[0]);
        CHECK(got.read.failure == Failure::unreadable);
        CHECK(got.read.detail == "it is encrypted");
        CHECK(got.bytes.empty());
    }
    const Listing bzip2 = list_file("bzip2.zip");
    REQUIRE(bzip2.entries.size() == 1);
    CHECK(bzip2.entries[0].unreadable == Unreadable::method);
}

TEST_CASE("archive: a zip's symlink keeps its target") {
    const Listing listing = list_file("symlink.zip");
    REQUIRE(listing.failure == Failure::none);
    const Entry* link = find(listing, "link-to-a");
    REQUIRE(link != nullptr);
    CHECK(link->type == Type::symlink);
    CHECK(link->link == "a.txt");
    const Entry* a = find(listing, "a.txt");
    REQUIRE(a != nullptr);
    CHECK(a->mode == 0644);
    CHECK(read_bytes(file_bytes(kData / "symlink.zip"), listing.format, *a).bytes ==
          "hello from a\n");
}

TEST_CASE("archive: tar from GNU tar, gnu and pax, plain and gzip'd, read alike") {
    for (const char* name : {"gnu.tar", "pax.tar", "pax.tar.gz"}) {
        CAPTURE(name);
        const std::string bytes = file_bytes(kData / name);
        const Listing listing = list_bytes(bytes);
        REQUIRE(listing.failure == Failure::none);
        CHECK(listing.format ==
              (std::string_view{name}.ends_with(".gz") ? Format::tar_gzip : Format::tar));
        const Entry* a = find(listing, "./a.txt");
        REQUIRE(a != nullptr);
        CHECK(a->size == 13);
        CHECK(a->mode == 0644);
        CHECK(a->mtime == 1794744000);
        CHECK(find(listing, "./città.txt") != nullptr);
        const Entry* docs = find(listing, "./docs/");
        REQUIRE(docs != nullptr);
        CHECK(docs->type == Type::directory);
        CHECK(find(listing, "./docs/" + std::string(120, 'n') + ".txt") != nullptr);
        const Entry* link = find(listing, "./link-to-a");
        REQUIRE(link != nullptr);
        CHECK(link->type == Type::symlink);
        CHECK(link->link == "a.txt");

        CHECK(read_bytes(bytes, listing.format, *a).bytes == "hello from a\n");
        const Entry* repeat = find(listing, "./docs/repeat.txt");
        REQUIRE(repeat != nullptr);
        const Got got = read_bytes(bytes, listing.format, *repeat);
        CHECK(got.read.failure == Failure::none);
        CHECK(got.bytes.size() == 5120);
    }
}

TEST_CASE("archive: reads in a row give what single reads give, in any order") {
    const std::string bytes = file_bytes(kData / "pax.tar.gz");
    const Listing listing = list_bytes(bytes);
    REQUIRE(listing.failure == Failure::none);
    std::vector<const Entry*> files;
    for (const Entry& entry : listing.entries) {
        if (entry.type == Type::file) {
            files.push_back(&entry);
        }
    }
    REQUIRE(files.size() >= 4);
    const Memfd archive{bytes};
    engine::Sequence sequence;
    const auto read_all = [&] {
        for (const Entry* entry : files) {
            CAPTURE(entry->path);
            const Memfd out{""};
            const rummage::archive::Read read =
                sequence.read(archive.fd(), listing.format, *entry, out.fd(), UINT64_MAX);
            CHECK(read.failure == Failure::none);
            CHECK(out.bytes() == read_bytes(bytes, listing.format, *entry).bytes);
        }
    };
    read_all();  // in the archive's order: inflated once
    std::ranges::reverse(files);
    read_all();  // backwards: from the start each time
    // Another archive through the same sequence: its own bytes.
    const std::string other = file_bytes(kData / "gnu.tar");
    const Listing other_listing = list_bytes(other);
    const Entry* a = find(other_listing, "./a.txt");
    REQUIRE(a != nullptr);
    const Memfd other_archive{other};
    const Memfd out{""};
    CHECK(
        sequence.read(other_archive.fd(), other_listing.format, *a, out.fd(), UINT64_MAX).failure ==
        Failure::none);
    CHECK(out.bytes() == "hello from a\n");
    // More than allowed is refused, as a single read refuses it.
    const Memfd small{""};
    CHECK(sequence.read(archive.fd(), listing.format, *files.front(), small.fd(), 1).failure ==
          Failure::too_large);
    sequence.forget();
}

TEST_CASE("archive: a single .gz is one entry, named by its header") {
    const std::string bytes = file_bytes(kData / "notes.txt.gz");
    const Listing listing = list_bytes(bytes);
    REQUIRE(listing.failure == Failure::none);
    CHECK(listing.format == Format::gzip);
    REQUIRE(listing.entries.size() == 1);
    CHECK(listing.entries[0].path == "notes.txt");
    CHECK(listing.entries[0].size == 14);
    CHECK(listing.entries[0].mtime == 1794744000);
    CHECK(read_bytes(bytes, listing.format, listing.entries[0]).bytes == "just one file\n");
}

TEST_CASE("archive: what is not an archive says so") {
    CHECK(list_bytes("").failure == Failure::not_archive);
    CHECK(list_bytes("just some text, nothing more\n").failure == Failure::not_archive);
    CHECK(list_bytes(std::string(4096, '\0')).failure == Failure::not_archive);
}

TEST_CASE("archive: a zip whose entries share their bytes is refused (a zip bomb's trick)") {
    ZipItem two = zip_item("two", std::string(1000, 'x'), true);
    two.same_as = 0;
    const std::string bytes = make_zip({zip_item("one", std::string(1000, 'x'), true), two});
    const Listing listing = list_bytes(bytes);
    CHECK(listing.failure == Failure::damaged);
}

TEST_CASE("archive: a zip entry larger than it says, or with a wrong checksum, is refused") {
    ZipItem big = zip_item("big", std::string(100'000, 'z'), true);
    big.said_size = 1000;
    const std::string lying = make_zip({big});
    const Listing listing = list_bytes(lying);
    REQUIRE(listing.failure == Failure::none);
    REQUIRE(listing.entries.size() == 1);
    CHECK(listing.entries[0].size == 1000);
    const Got got = read_bytes(lying, listing.format, listing.entries[0]);
    CHECK(got.read.failure == Failure::damaged);
    CHECK(got.bytes.size() <= 1000);

    const std::string wrong_crc = make_zip({[] {
        ZipItem item = zip_item("x", "data");
        item.said_crc = 1234;
        return item;
    }()});
    const Listing crc_listing = list_bytes(wrong_crc);
    REQUIRE(crc_listing.entries.size() == 1);
    const Got crc_got = read_bytes(wrong_crc, crc_listing.format, crc_listing.entries[0]);
    CHECK(crc_got.read.failure == Failure::damaged);
    CHECK(crc_got.read.detail == "an entry's bytes do not match its checksum");
}

TEST_CASE("archive: a zip cut short, or with its directory gone, is damaged") {
    const std::string bytes = make_zip({zip_item("a", "hello"), zip_item("b", "x")});
    for (std::size_t cut = 4; cut < bytes.size(); cut += 7) {
        CAPTURE(cut);
        const Listing listing = list_bytes(bytes.substr(0, cut));
        CHECK(listing.failure != Failure::none);
    }
}

TEST_CASE("archive: a tar's broken header or short data keeps what came before") {
    const std::string good = tar_file("one.txt", "first");
    const Listing broken = list_bytes(good + tar_header("two.txt", '0', 5, true) + tar_end());
    CHECK(broken.failure == Failure::damaged);
    REQUIRE(broken.entries.size() == 1);
    CHECK(broken.entries[0].path == "one.txt");

    // A header saying 4 GiB follow.
    const Listing short_data = list_bytes(good + tar_header("huge", '0', 1ULL << 32U) + "x");
    CHECK(short_data.failure == Failure::damaged);
    CHECK(short_data.entries.size() == 1);

    const Listing whole = list_bytes(good + tar_file("two.txt", "second") + tar_end());
    CHECK(whole.failure == Failure::none);
    CHECK(whole.entries.size() == 2);
}

TEST_CASE("archive: more entries or longer names than the limits are too large") {
    std::string bytes;
    for (int i = 0; i < 5; ++i) {
        bytes += tar_file("f" + std::to_string(i), "x");
    }
    bytes += tar_end();
    CHECK(list_bytes(bytes, {.max_entries = 5, .max_path_bytes = 4096}).failure == Failure::none);
    CHECK(list_bytes(bytes, {.max_entries = 4, .max_path_bytes = 4096}).failure ==
          Failure::too_large);
    CHECK(list_bytes(bytes, {.max_entries = 100, .max_path_bytes = 1}).failure ==
          Failure::too_large);
    const std::string zip = make_zip({zip_item("a", "1"), zip_item("b", "2")});
    CHECK(list_bytes(zip, {.max_entries = 1, .max_path_bytes = 4096}).failure ==
          Failure::too_large);
}
