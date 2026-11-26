#include "rummage/isolate/helpers.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <ios>
#include <memory>
#include <optional>
#include <poll.h>
#include <span>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <zlib.h>

#include <doctest/doctest.h>

#include "protocol.hpp"
#include "rummage/archive/archive.hpp"
#include "rummage/formats/archive/archive.hpp"
#include "rummage/formats/image/load.hpp"
#include "rummage/formats/image/photo.hpp"
#include "rummage/formats/text/read.hpp"
#include "rummage/picture/load.hpp"
#include "rummage/text/text.hpp"
#include "temp_dir.hpp"

namespace fs = std::filesystem;
namespace image = rummage::formats::image;
namespace picture = rummage::picture;
namespace isolate = rummage::isolate;
namespace text = rummage::text;
using picture::Failure;
using rummage::test::TempDir;

namespace {

const fs::path kData = fs::path{RUMMAGE_TEST_DATA} / "image";

isolate::HelperOptions real(std::size_t max_helpers = 4) {
    isolate::HelperOptions options;
    options.program = RUMMAGE_PREVIEW_HELPER;
    options.max_helpers = max_helpers;
    return options;
}

isolate::HelperOptions fake(const char* mode) {
    isolate::HelperOptions options;
    options.program = RUMMAGE_FAKE_HELPER;
    options.args = {mode};
    options.max_helpers = 1;
    options.timeout = std::chrono::milliseconds{300};
    return options;
}

picture::LoadRequest request(const fs::path& path, std::uint32_t side = 128) {
    return {.path = path, .max_width = side, .max_height = side};
}

}  // namespace

TEST_CASE("helpers: the helper makes the same pictures as this process") {
    isolate::Helpers helpers{real()};
    for (const char* name : {"blocks.png", "blocks.jpg", "blocks.gif", "blocks.webp", "blocks.jxl",
                             "blocks-orient6.jpg", "shot.jpg", "with-thumbnail.jpg",
                             "tiff-preview.tif", "truncated.jpg"}) {
        INFO(name);
        const picture::Loaded there = helpers.load(request(kData / name));
        const picture::Loaded here = image::load(request(kData / name));
        CHECK(there.failure == here.failure);
        CHECK(there.detail == here.detail);
        CHECK(there.from_preview == here.from_preview);
        CHECK(there.full_width == here.full_width);
        CHECK(there.full_height == here.full_height);
        CHECK(there.image.width == here.image.width);
        CHECK(there.image.rgba == here.image.rgba);
    }
    CHECK(helpers.running() == 1);  // one after the other: one helper did it all
}

TEST_CASE("helpers: a photo's information from the helper") {
    isolate::Helpers helpers{real(1)};
    for (const char* name : {"shot.jpg", "with-thumbnail.jpg", "blocks.png", "truncated.jpg"}) {
        INFO(name);
        const picture::PhotoInfo there = helpers.photo_info(kData / name);
        const picture::PhotoInfo here = image::read_photo_info(kData / name);
        CHECK(there.width == here.width);
        CHECK(there.height == here.height);
        CHECK(there.camera == here.camera);
        CHECK(there.lens == here.lens);
        CHECK(there.exposure_time == here.exposure_time);
        CHECK(there.aperture == here.aperture);
        CHECK(there.iso == here.iso);
        CHECK(there.taken == here.taken);
        CHECK(there.software == here.software);
        CHECK(there.rating == here.rating);
        CHECK(there.details == here.details);
        CHECK(there.details_truncated == here.details_truncated);
    }
    const picture::PhotoInfo shot = helpers.photo_info(kData / "shot.jpg");
    CHECK(shot.camera == "OM-1");
    CHECK_FALSE(shot.rating);  // shot.jpg.xmp is not read (2e-4)
    CHECK(helpers.photo_info(kData / "missing.jpg").camera.empty());
}

TEST_CASE("helpers: a photo's information travels whole, and nonsense is refused") {
    picture::PhotoInfo info;
    info.width = 5220;
    info.height = 3912;
    info.camera = "OM-1";
    info.duration = "0:00:12.500";
    info.duration_ms = 12500;
    info.bitrate_bps = 3000000;
    info.video_codec = "h264";
    info.audio_codec = "aac";
    info.sample_rate = "48000 Hz";
    info.lens = std::string(400, 'L');  // cut at 255 bytes
    info.rating = -1;
    const std::vector<std::byte> bytes = isolate::protocol::encode_info(info);
    const std::optional<picture::PhotoInfo> decoded = isolate::protocol::decode_info(bytes);
    REQUIRE(decoded.has_value());
    const picture::PhotoInfo back = decoded.value_or(picture::PhotoInfo{});
    CHECK(back.width == 5220);
    CHECK(back.camera == "OM-1");
    CHECK(back.duration == info.duration);
    CHECK(back.duration_ms == info.duration_ms);
    CHECK(back.bitrate_bps == info.bitrate_bps);
    CHECK(back.video_codec == info.video_codec);
    CHECK(back.audio_codec == info.audio_codec);
    CHECK(back.sample_rate == info.sample_rate);
    CHECK(back.lens == std::string(255, 'L'));
    CHECK(back.rating == -1);
    CHECK_FALSE(isolate::protocol::decode_info(std::span{bytes}.first(bytes.size() - 1)));
    std::vector<std::byte> longer = bytes;
    longer.push_back(std::byte{0});
    CHECK_FALSE(isolate::protocol::decode_info(longer));
    CHECK_FALSE(isolate::protocol::decode_info({}));
}

TEST_CASE("helpers: never more helpers than allowed, every request answered") {
    isolate::Helpers helpers{real(2)};
    std::vector<std::future<picture::Loaded>> results;
    results.reserve(12);
    for (int i = 0; i < 12; ++i) {
        results.push_back(std::async(std::launch::async, [&helpers] {
            return helpers.load(request(kData / "shot.jpg", 256));
        }));
    }
    for (std::future<picture::Loaded>& result : results) {
        const picture::Loaded got = result.get();
        INFO(got.detail);
        CHECK(got.failure == Failure::none);
    }
    CHECK(helpers.running() <= 2);
}

TEST_CASE("helpers: an idle helper is closed and waited for, then replaced unseen") {
    isolate::HelperOptions options = real(1);
    options.idle = std::chrono::milliseconds{50};
    isolate::Helpers helpers{options};
    CHECK(helpers.load(request(kData / "blocks.png")).failure == Failure::none);
    CHECK(helpers.running() == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    CHECK(helpers.running() == 0);  // closed by rummage and waited for: no zombie
    CHECK(helpers.load(request(kData / "blocks.png")).failure == Failure::none);
    // A goodbye racing with a request: sent again to a new one (which says goodbye too here).
    isolate::Helpers leaving{fake("goodbye")};
    const picture::Loaded left = leaving.load(request(kData / "blocks.png"));
    CHECK(left.failure == Failure::unavailable);
    CHECK(leaving.running() == 0);
}

TEST_CASE("helpers: a helper that crashes, hangs or talks nonsense is let go of") {
    for (const char* mode : {"crash", "hang", "garbage", "big-lie"}) {
        INFO(mode);
        isolate::Helpers helpers{fake(mode)};
        const auto start = std::chrono::steady_clock::now();
        const picture::Loaded loaded = helpers.load(request(kData / "blocks.png"));
        CHECK(loaded.failure == Failure::unavailable);
        CHECK(loaded.error() == "Preview unavailable");
        CHECK_FALSE(loaded.detail.empty());
        CHECK(loaded.image.empty());
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds{2});
        CHECK(helpers.running() == 0);  // killed and waited for
    }
}

TEST_CASE("helpers: a helper without its cage never works, and rummage says why") {
    isolate::Helpers helpers{fake("no-sandbox")};
    const picture::Loaded loaded = helpers.load(request(kData / "blocks.png"));
    CHECK(loaded.failure == Failure::unavailable);
    CHECK(loaded.detail == "the preview helper could not set up its sandbox");
    CHECK(helpers.running() == 0);
}

TEST_CASE("helpers: no helper, no picture, and nothing parsed here instead") {
    isolate::HelperOptions missing;
    missing.program = "/nonexistent/rummage-preview";
    isolate::Helpers helpers{missing};
    const picture::Loaded loaded = helpers.load(request(kData / "blocks.png"));
    CHECK(loaded.failure == Failure::unavailable);
    CHECK(loaded.image.empty());
    CHECK(helpers.running() == 0);
}

TEST_CASE("helpers: only regular files are sent") {
    const TempDir dir;
    isolate::Helpers helpers{real(1)};
    const fs::path fifo = dir.path / "pipe.jpg";
    REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);
    CHECK(helpers.load(request(fifo)).failure == Failure::unreadable);  // and returns at once
    CHECK(helpers.load(request(dir.path)).failure == Failure::unreadable);
    CHECK(helpers.load(request(dir.path / "missing.png")).failure == Failure::unreadable);
    CHECK(helpers.running() == 0);
}

TEST_CASE("helpers: load_picture goes where the loader says") {
    auto helpers = std::make_shared<isolate::Helpers>(fake("crash"));
    picture::set_loader([helpers](const picture::LoadRequest& r) { return helpers->load(r); });
    CHECK(picture::load_picture(request(kData / "blocks.png")).failure == Failure::unavailable);
    // No loader: no picture, never made here instead.
    picture::set_loader({});
    CHECK(picture::load_picture(request(kData / "blocks.png")).failure == Failure::unavailable);
    // Back to what the other tests use: the image family in this process.
    picture::set_loader([](const picture::LoadRequest& r) { return image::load(r); });
    CHECK(picture::load_picture(request(kData / "blocks.png")).failure == Failure::none);
}

TEST_CASE("helpers: the helper reads the same text as this process") {
    const TempDir dir;
    const auto write = [&](const char* name, std::string_view bytes) {
        const fs::path path = dir.path / name;
        std::ofstream{path, std::ios::binary} << bytes;
        return path;
    };
    std::string many;
    for (int i = 0; i < 3000; ++i) {
        many += "line\t" + std::to_string(i) + (i % 2 == 0 ? "\r\n" : "\n");
    }
    const std::vector<fs::path> files{
        write("utf8.txt", "caffè ☕\n\tok\n"),
        write("latin1.txt", "caff\xE8 3\x80\n"),
        write("utf16.txt", std::string_view{"\xFF\xFEh\0i\0\n\0\xE8\0", 10}),
        write("binary.txt", std::string_view{"a\0b", 3}),
        write("empty.txt", ""),
        write("newline.txt", "\n"),
        write("many.txt", many),
        fs::path{RUMMAGE_TEST_DATA} / ".." / "text_test.cpp",
    };
    isolate::Helpers helpers{real(1)};
    for (const text::Limits limits :
         {text::Limits{}, text::Limits{.max_bytes = 8192, .max_line_bytes = 256, .max_lines = 40},
          text::Limits{.max_bytes = 100, .max_line_bytes = 3, .max_lines = 0, .tab_width = 8},
          text::Limits{.max_bytes = 8192, .tail = true}}) {
        for (const fs::path& file : files) {
            INFO(file.filename().string(), " ", limits.max_bytes);
            const bool code = file.extension() == ".cpp";
            const text::LoadRequest request{
                .path = file, .mime = code ? "text/x-c++src" : "text/plain", .limits = limits};
            const text::Loaded there = helpers.load_text(request);
            const text::Loaded here = rummage::formats::text::load(request);
            CHECK(there.failure == here.failure);
            CHECK(there.detail == here.detail);
            CHECK(there.text.lines == here.text.lines);
            CHECK(there.text.truncated == here.text.truncated);
            CHECK(there.text.encoding == here.text.encoding);
            CHECK(there.text.crlf == here.text.crlf);
            CHECK(there.text.tail == here.text.tail);
            CHECK(there.text.language == here.text.language);
            CHECK(there.text.spans == here.text.spans);
            CHECK(there.text.language == (code ? "C++" : ""));
            CHECK(there.text.spans.empty() != code);
        }
    }
    CHECK(helpers.load_text({.path = files[2], .mime = {}, .limits = {}}).text.encoding ==
          text::Encoding::utf16le);
    CHECK(helpers.load_text({.path = files[3], .mime = {}, .limits = {}}).failure ==
          text::Failure::binary);
    CHECK(helpers.running() == 1);
    // Only regular files are sent.
    const fs::path fifo = dir.path / "pipe.txt";
    REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);
    CHECK(helpers.load_text({.path = fifo, .mime = {}, .limits = {}}).failure ==
          text::Failure::unreadable);
    CHECK(helpers.load_text({.path = dir.path / "missing.txt", .mime = {}, .limits = {}}).failure ==
          text::Failure::unreadable);
}

TEST_CASE("helpers: a text request travels whole, and too much is refused") {
    namespace protocol = isolate::protocol;
    const std::vector<std::byte> bytes = protocol::encode_text_request(
        {.max_bytes = 8192, .max_line_bytes = 256, .max_lines = 40, .tab_width = 8}, "notes.md",
        "text/markdown");
    const std::optional<protocol::TextAsk> ask = protocol::decode_text_request(bytes);
    REQUIRE(ask.has_value());
    const protocol::TextAsk back = ask.value_or(protocol::TextAsk{});
    CHECK(back.limits.max_bytes == 8192);
    CHECK(back.limits.max_line_bytes == 256);
    CHECK(back.limits.max_lines == 40);
    CHECK(back.limits.tab_width == 8);
    CHECK(back.name == "notes.md");
    CHECK(back.mime == "text/markdown");
    CHECK_FALSE(back.limits.tail);
    const std::optional<protocol::TextAsk> tail = protocol::decode_text_request(
        protocol::encode_text_request({.tail = true}, "app.log", "text/x-log"));
    CHECK(tail.value_or(protocol::TextAsk{}).limits.tail);
    // A tail that is neither 0 nor 1 is nonsense.
    std::vector<std::byte> two = bytes;
    two[offsetof(protocol::TextRequest, tail)] = std::byte{2};
    CHECK_FALSE(protocol::decode_text_request(two));
    // Names are cut at 255 bytes; limits past the helper's are lowered to them.
    const std::optional<protocol::TextAsk> long_name = protocol::decode_text_request(
        protocol::encode_text_request({.max_bytes = std::size_t{1} << 30U,
                                       .max_line_bytes = 1U << 20U,
                                       .max_lines = 0,
                                       .tab_width = 100},
                                      std::string(400, 'n'), "text/plain"));
    REQUIRE(long_name.has_value());
    CHECK(long_name.value_or(protocol::TextAsk{}).name.size() == 255);
    CHECK(long_name.value_or(protocol::TextAsk{}).limits.max_bytes == protocol::kMaxTextBytes);
    CHECK(long_name.value_or(protocol::TextAsk{}).limits.tab_width == protocol::kMaxTabWidth);
    // Nonsense: cut short, longer, zero limits.
    CHECK_FALSE(protocol::decode_text_request(std::span{bytes}.first(bytes.size() - 1)));
    std::vector<std::byte> longer = bytes;
    longer.push_back(std::byte{0});
    CHECK_FALSE(protocol::decode_text_request(longer));
    CHECK_FALSE(
        protocol::decode_text_request(protocol::encode_text_request({.max_bytes = 0}, "a", "b")));
    CHECK_FALSE(protocol::decode_text_request(
        protocol::encode_text_request({.max_line_bytes = 0}, "a", "b")));
    CHECK_FALSE(
        protocol::decode_text_request(protocol::encode_text_request({.tab_width = 0}, "a", "b")));
    CHECK_FALSE(protocol::decode_text_request({}));
}

TEST_CASE("helpers: empty text fields and payloads travel without a backing buffer") {
    namespace protocol = isolate::protocol;
    // Empty optional fields can be default-constructed views whose data() is null. UBSan
    // rejects passing these to memcpy, even with a count of zero.
    const std::vector<std::byte> request = protocol::encode_text_request({}, {}, {});
    CHECK(request.size() == sizeof(protocol::TextRequest));
    const std::optional<protocol::TextAsk> ask = protocol::decode_text_request(request);
    REQUIRE(ask.has_value());
    CHECK(ask->name.empty());
    CHECK(ask->mime.empty());

    for (const std::uint32_t lines : {0U, 1U}) {
        text::Text sent;
        sent.lines.resize(lines);
        std::uint32_t bytes = 1;
        std::uint32_t spans = 1;
        const std::vector<std::byte> payload = protocol::encode_text_payload(sent, bytes, spans);
        CHECK(payload.empty());
        CHECK(bytes == 0);
        CHECK(spans == 0);
        text::Text back;
        REQUIRE(protocol::decode_text_payload(payload, lines, bytes, spans, false, {}, back));
        CHECK(back.lines == sent.lines);
    }
}

TEST_CASE("helpers: a helper that lies about a text is let go of") {
    const TempDir dir;
    const fs::path file = dir.path / "a.txt";
    std::ofstream{file} << "a\nb\nc\n";
    for (const char* mode : {"crash", "garbage", "text-lie"}) {
        INFO(mode);
        isolate::Helpers helpers{fake(mode)};
        const text::Loaded loaded = helpers.load_text({.path = file, .mime = {}, .limits = {}});
        CHECK(loaded.failure == text::Failure::unavailable);
        CHECK_FALSE(loaded.detail.empty());
        CHECK(loaded.text.lines.empty());
        CHECK(helpers.running() == 0);
    }
}

TEST_CASE("helpers: a text's payload travels whole, and spans out of place are refused") {
    namespace protocol = isolate::protocol;
    text::Text sent;
    sent.lines = {"int x;", "", "// é"};
    sent.spans = {{{.start = 0, .length = 3, .style = text::Style::type}},
                  {},
                  {{.start = 0, .length = 5, .style = text::Style::comment}}};
    std::uint32_t bytes = 0;
    std::uint32_t spans = 0;
    const std::vector<std::byte> payload = protocol::encode_text_payload(sent, bytes, spans);
    CHECK(bytes == 13);
    CHECK(spans == 2);
    CHECK(payload.size() == 16 + (2 * 12));
    text::Text back;
    REQUIRE(protocol::decode_text_payload(payload, 3, bytes, spans, true, {}, back));
    CHECK(back.lines == sent.lines);
    CHECK(back.spans == sent.spans);
    // Without colours: no spans at all, not one empty list per line.
    text::Text plain;
    plain.lines = sent.lines;
    const std::vector<std::byte> plain_payload = protocol::encode_text_payload(plain, bytes, spans);
    REQUIRE(protocol::decode_text_payload(plain_payload, 3, bytes, 0, false, {}, back));
    CHECK(back.spans.empty());

    // A span changed in the payload: past its line, overlapping, out of order, an unknown style.
    const auto refused = [&](std::size_t number, std::uint32_t value) {
        std::vector<std::byte> changed = payload;
        std::memcpy(changed.data() + 16 + (number * 4), &value, sizeof(value));
        text::Text out;
        return !protocol::decode_text_payload(changed, 3, 13, 2, true, {}, out);
    };
    CHECK(refused(0, 3));                // the first span on a line that is not there
    CHECK(refused(1, 4));                // starts at 4, 3 long: past "int x;"
    CHECK(refused(2, (7U << 8U) | 1U));  // 7 long
    CHECK(refused(2, (3U << 8U) | 8U));  // style 8
    CHECK(refused(2, 1U));               // 0 long
    CHECK(refused(3, 0));                // the second span over the first
    CHECK_FALSE(refused(1, 0));          // unchanged
    text::Text out;
    CHECK_FALSE(protocol::decode_text_payload(std::span{payload}.first(payload.size() - 1), 3, 13,
                                              2, true, {}, out));
    CHECK_FALSE(protocol::decode_text_payload(payload, 2, 13, 2, true, {}, out));  // lines
    CHECK_FALSE(protocol::decode_text_payload(payload, 3, 13, 2, true, {.max_line_bytes = 5}, out));
}

TEST_CASE("helpers: the helper lists and reads archives in its cage as this process does") {
    isolate::Helpers helpers{real(1)};
    const fs::path archives = fs::path{RUMMAGE_TEST_DATA} / "archive";
    for (const char* name :
         {"basic.zip", "aes.zip", "symlink.zip", "gnu.tar", "pax.tar.gz", "notes.txt.gz"}) {
        CAPTURE(name);
        const rummage::archive::Listing there = helpers.list_archive(archives / name, {});
        const int fd = ::open((archives / name).c_str(), O_RDONLY | O_CLOEXEC);
        REQUIRE(fd >= 0);
        const rummage::archive::Listing here = rummage::formats::archive::list(fd, {});
        CHECK(there.failure == here.failure);
        CHECK(there.format == here.format);
        CHECK(there.entries == here.entries);
        for (const rummage::archive::Entry& entry : there.entries) {
            CAPTURE(entry.path);
            const int out_there = ::memfd_create("there", MFD_CLOEXEC);
            const int out_here = ::memfd_create("here", MFD_CLOEXEC);
            const rummage::archive::Read read_there = helpers.read_archive_entry(
                archives / name, there.format, entry, out_there, 1 << 20);
            const rummage::archive::Read read_here =
                rummage::formats::archive::read_entry(fd, here.format, entry, out_here, 1 << 20);
            CHECK(read_there.failure == read_here.failure);
            CHECK(read_there.detail == read_here.detail);
            CHECK(read_there.bytes == read_here.bytes);
            struct stat a{};
            struct stat b{};
            REQUIRE(::fstat(out_there, &a) == 0);
            REQUIRE(::fstat(out_here, &b) == 0);
            CHECK(a.st_size == b.st_size);
            ::close(out_there);
            ::close(out_here);
        }
        ::close(fd);
    }
    // Not an archive: said, nothing else.
    const rummage::archive::Listing text = helpers.list_archive(kData / "blocks.png", {});
    CHECK(text.failure == rummage::archive::Failure::not_archive);
    CHECK(helpers.running() == 1);
}

TEST_CASE("helpers: a sequence reads a tar.gz's entries in one helper, as this process does") {
    auto helpers = std::make_shared<isolate::Helpers>(real(2));
    const fs::path archive = fs::path{RUMMAGE_TEST_DATA} / "archive" / "pax.tar.gz";
    const rummage::archive::Listing listing = helpers->list_archive(archive, {});
    REQUIRE(listing.failure == rummage::archive::Failure::none);
    const int fd = ::open(archive.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    const auto bytes_of = [](int out) {
        std::string bytes;
        std::array<char, 4096> buffer{};
        off_t at = 0;
        ssize_t got = 0;
        while ((got = ::pread(out, buffer.data(), buffer.size(), at)) > 0) {
            bytes.append(buffer.data(), static_cast<std::size_t>(got));
            at += got;
        }
        return bytes;
    };
    {
        const std::unique_ptr<rummage::archive::EntrySequence> sequence =
            isolate::Helpers::open_sequence(helpers, archive, listing.format);
        std::size_t files = 0;
        for (const rummage::archive::Entry& entry : listing.entries) {
            if (entry.type != rummage::archive::Type::file) {
                continue;
            }
            CAPTURE(entry.path);
            ++files;
            const int out_there = ::memfd_create("there", MFD_CLOEXEC);
            const int out_here = ::memfd_create("here", MFD_CLOEXEC);
            const rummage::archive::Read there = sequence->read(entry, out_there, 1 << 20);
            const rummage::archive::Read here =
                rummage::formats::archive::read_entry(fd, listing.format, entry, out_here, 1 << 20);
            CHECK(there.failure == rummage::archive::Failure::none);
            CHECK(there.bytes == here.bytes);
            CHECK(bytes_of(out_there) == bytes_of(out_here));
            ::close(out_there);
            ::close(out_here);
        }
        CHECK(files >= 4);
        CHECK(helpers->running() == 1);  // one helper, taken for the whole sequence
    }
    ::close(fd);
    // Given back: the next request finds it idle.
    CHECK(helpers->list_archive(archive, {}).failure == rummage::archive::Failure::none);
    CHECK(helpers->running() == 1);
}

TEST_CASE("helpers: a sequence's read is stopped mid-entry, the helper with it") {
    // 4 GiB of zeros once inflated: one 64 MiB gzip member, 64 times over (gzip allows that).
    std::string member;
    {
        z_stream z{};
        REQUIRE(deflateInit2(&z, 1, Z_DEFLATED, MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY) == Z_OK);
        const std::string zeros(std::size_t{64} << 20, '\0');
        member.resize(deflateBound(&z, static_cast<uLong>(zeros.size())));
        z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(zeros.data()));  // NOLINT: zlib
        z.avail_in = static_cast<uInt>(zeros.size());
        z.next_out = reinterpret_cast<Bytef*>(member.data());  // NOLINT: zlib's API
        z.avail_out = static_cast<uInt>(member.size());
        REQUIRE(deflate(&z, Z_FINISH) == Z_STREAM_END);
        member.resize(z.total_out);
        deflateEnd(&z);
    }
    const TempDir dir;
    {
        std::ofstream out{dir.path / "zeros.gz", std::ios::binary};
        for (int i = 0; i < 64; ++i) {
            out.write(member.data(), static_cast<std::streamsize>(member.size()));
        }
    }
    auto helpers = std::make_shared<isolate::Helpers>(real(1));
    const rummage::archive::Listing listing = helpers->list_archive(dir.path / "zeros.gz", {});
    REQUIRE(listing.format == rummage::archive::Format::gzip);
    REQUIRE(listing.entries.size() == 1);
    // Into a pipe nobody reads: the helper fills it and waits, so the entry cannot end before
    // the stop, however fast zlib is. Stopped once the first bytes are there.
    std::array<int, 2> ends{};
    REQUIRE(::pipe2(ends.data(), O_CLOEXEC) == 0);
    const std::unique_ptr<rummage::archive::EntrySequence> sequence =
        isolate::Helpers::open_sequence(helpers, dir.path / "zeros.gz", listing.format, [&ends] {
            pollfd written{.fd = ends[0], .events = POLLIN, .revents = 0};
            return ::poll(&written, 1, 0) == 1;
        });
    const auto started = std::chrono::steady_clock::now();
    const rummage::archive::Read read =
        sequence->read(listing.entries.front(), ends[1], UINT64_MAX);
    const auto took = std::chrono::steady_clock::now() - started;
    ::close(ends[0]);
    ::close(ends[1]);
    CHECK(read.failure == rummage::archive::Failure::unreadable);
    CHECK(read.detail == "preview cancelled");
    CHECK(took < std::chrono::seconds{2});  // seen at the next look at stopped
    CHECK(helpers->running() == 0);         // stopped: let go of, not given back
}

TEST_CASE("helpers: an archive's entries travel whole, and nonsense is refused") {
    namespace protocol = rummage::isolate::protocol;
    std::vector<rummage::archive::Entry> entries(3);
    entries[0].path = "docs/";
    entries[0].type = rummage::archive::Type::directory;
    entries[1].path = "docs/a.txt";
    entries[1].size = 13;
    entries[1].packed = 9;
    entries[1].mode = 0644;
    entries[1].mtime = 1794744000;
    entries[1].locator = 1234;
    entries[2].path = "link";
    entries[2].link = "docs/a.txt";
    entries[2].type = rummage::archive::Type::symlink;
    entries[2].unreadable = rummage::archive::Unreadable::encrypted;
    const std::vector<std::byte> payload = protocol::encode_archive_payload(entries);
    CHECK(payload.size() % 8 == 0);
    const auto back = protocol::decode_archive_payload(payload, 3, {});
    REQUIRE(back.has_value());
    CHECK(back.value_or(std::vector<rummage::archive::Entry>{}) == entries);

    CHECK_FALSE(protocol::decode_archive_payload(payload, 2, {}).has_value());  // bytes left over
    CHECK_FALSE(protocol::decode_archive_payload(payload, 4, {}).has_value());  // too few
    CHECK_FALSE(
        protocol::decode_archive_payload(std::span{payload}.first(payload.size() - 8), 3, {})
            .has_value());
    CHECK_FALSE(
        protocol::decode_archive_payload(payload, 3, {.max_entries = 2, .max_path_bytes = 4096})
            .has_value());
    CHECK_FALSE(
        protocol::decode_archive_payload(payload, 3, {.max_entries = 10, .max_path_bytes = 5})
            .has_value());
    std::vector<std::byte> wrong = payload;
    wrong[0] = std::byte{9};  // no such type
    CHECK_FALSE(protocol::decode_archive_payload(wrong, 3, {}).has_value());

    rummage::archive::Format format{};
    const protocol::ArchiveReadRequest request =
        protocol::encode_archive_read(rummage::archive::Format::tar_gzip, entries[1], 4096);
    const auto entry = protocol::decode_archive_read(request, format);
    REQUIRE(entry.has_value());
    CHECK(format == rummage::archive::Format::tar_gzip);
    CHECK(entry.value_or(rummage::archive::Entry{}).locator == 1234);
    CHECK(entry.value_or(rummage::archive::Entry{}).size == 13);
    protocol::ArchiveReadRequest bad = request;
    bad.format = 7;
    CHECK_FALSE(protocol::decode_archive_read(bad, format).has_value());
}

TEST_CASE("helpers: rendered Markdown crosses the sandbox boundary with its source intact") {
    const TempDir dir;
    const auto path = dir.path / "README.md";
    std::ofstream{path} << "# Heading\n\nA **bold** [link](https://example.com).\n\n"
                           "| Name | Value |\n| --- | --- |\n| first | 42 |\n\n"
                           "```cpp\nint answer = 42;\n```\n";
    isolate::Helpers helpers{real(1)};
    const text::LoadRequest request{.path = path, .mime = "text/markdown", .limits = {}};
    const auto there = helpers.load_text(request);
    const auto here = rummage::formats::text::load(request);
    REQUIRE(there.failure == text::Failure::none);
    REQUIRE_FALSE(there.text.markdown.blocks.empty());
    CHECK(there.text.markdown == here.text.markdown);
    CHECK(there.text.lines == here.text.lines);
    CHECK(there.text.spans == here.text.spans);
    CHECK(helpers.running() == 1);
}

TEST_CASE("helpers: reject overflow and dimensions beyond the requested preview") {
    for (const char* mode : {"pixel-overflow", "wide", "tall", "zero-width", "zero-height"}) {
        CAPTURE(mode);
        isolate::Helpers helpers{fake(mode)};
        auto ask = request(kData / "blocks.png");
        if (std::string_view{mode} == "pixel-overflow") {
            // Exercise the size check even when the dimensions fit the request.
            ask.max_width = UINT32_MAX;
            ask.max_height = UINT32_MAX;
        }
        const auto loaded = helpers.load(ask);
        CHECK(loaded.failure == Failure::unavailable);
        CHECK(loaded.image.empty());
        CHECK(helpers.running() == 0);
    }
    isolate::Helpers helpers{fake("fits")};
    auto ask = request(kData / "blocks.png");
    ask.max_width = 64;
    ask.max_height = 32;
    const auto loaded = helpers.load(ask);
    REQUIRE(loaded.failure == Failure::none);
    CHECK(loaded.image.width == 64);
    CHECK(loaded.image.height == 32);
    CHECK(loaded.image.rgba.size() == 64 * 32 * 4);
    CHECK(helpers.running() == 1);
}

TEST_CASE("helpers: rotated previews fit rectangular requests") {
    isolate::Helpers helpers{real(1)};
    for (const auto dimensions : {std::array{16U, 64U}, std::array{64U, 16U}}) {
        auto ask = request(kData / "blocks-orient6.jpg");
        ask.max_width = dimensions[0];
        ask.max_height = dimensions[1];
        const auto loaded = helpers.load(ask);
        REQUIRE(loaded.failure == Failure::none);
        CHECK(loaded.image.width <= ask.max_width);
        CHECK(loaded.image.height <= ask.max_height);
    }
}

TEST_CASE("helpers: extended metadata is bounded, round trips, and rejects truncated packets") {
    picture::PhotoInfo info;
    info.add_detail("Custom", std::string(3000, 'x'));
    info.add_detail("Custom", "second value");
    const auto bytes = isolate::protocol::encode_info(info);
    const auto back = isolate::protocol::decode_info(bytes);
    REQUIRE(back);
    CHECK(back->details == info.details);
    CHECK_FALSE(back->details_truncated);
    for (std::size_t size = 0; size < bytes.size(); ++size)
        CHECK_FALSE(isolate::protocol::decode_info(std::span{bytes}.first(size)));
    for (unsigned i = 0; i < 300; ++i)
        info.add_detail("Field " + std::to_string(i), std::string(4096, 'v'));
    const auto large = isolate::protocol::encode_info(info);
    CHECK(large.size() <= isolate::protocol::kMaxPacket);
    const auto bounded = isolate::protocol::decode_info(large);
    REQUIRE(bounded);
    CHECK(bounded->details_truncated);
    CHECK_FALSE(bounded->details.empty());
}

TEST_CASE(
    "helpers: metadata bounds preserve UTF-8 scalar boundaries and reject malformed packets") {
    for (const std::string& scalar : {std::string{"é"}, std::string{"旅"}, std::string{"🌷"}}) {
        for (std::size_t prefix = 251; prefix <= 255; ++prefix) {
            picture::PhotoInfo info;
            info.camera = std::string(prefix, 'x') + scalar;
            const auto decoded =
                isolate::protocol::decode_info(isolate::protocol::encode_info(info));
            REQUIRE(decoded);
            CHECK(decoded->camera == std::string(prefix, 'x') +
                                         (prefix + scalar.size() <= 255 ? scalar : std::string{}));
        }
    }
    picture::PhotoInfo info;
    info.camera = "before" + std::string{"\xed\xa0\x80"} + "after";  // UTF-8 surrogate
    info.details.emplace_back(std::string(4095, 'x') + "旅", std::string(4094, 'v') + "🌷");
    const auto decoded = isolate::protocol::decode_info(isolate::protocol::encode_info(info));
    REQUIRE(decoded);
    CHECK(decoded->camera == "before���after");
    REQUIRE(decoded->details.size() == 1);
    CHECK(decoded->details[0].first == std::string(4095, 'x'));
    CHECK(decoded->details[0].second == std::string(4094, 'v'));
    CHECK(decoded->details_truncated);
    info = {};
    info.camera = "é";
    auto damaged = isolate::protocol::encode_info(info);
    damaged.back() = std::byte{0xff};
    CHECK_FALSE(isolate::protocol::decode_info(damaged));
}

TEST_CASE("helpers: unused bundled engines create no housekeeping threads") {
    const auto threads = [] {
        return std::distance(fs::directory_iterator{"/proc/self/task"}, fs::directory_iterator{});
    };
    const auto before = threads();
    {
        isolate::Helpers base{real()}, media{real()}, pdf{real()};
        CHECK(base.running() == 0);
        CHECK(media.running() == 0);
        CHECK(pdf.running() == 0);
        CHECK(threads() == before);
    }
    CHECK(threads() == before);
}
