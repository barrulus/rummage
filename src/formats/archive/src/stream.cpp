#include "stream.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <zlib.h>

namespace rummage::formats::archive::detail {

using rummage::archive::Failure;

namespace {

constexpr std::size_t kChunk = std::size_t{64} * 1024;

}  // namespace

void damaged(std::string detail) { throw Problem{Failure::damaged, std::move(detail)}; }

File::File(int fd) : fd_{fd} {
    struct stat info{};
    if (fd < 0 || ::fstat(fd, &info) != 0 || info.st_size < 0) {
        throw Problem{Failure::unreadable, "the file could not be read"};
    }
    size_ = static_cast<std::uint64_t>(info.st_size);
}

std::size_t File::read_at(std::uint64_t offset, std::span<std::uint8_t> to) const {
    if (offset >= size_) {
        return 0;
    }
    const auto length =
        static_cast<std::size_t>(std::min<std::uint64_t>(to.size(), size_ - offset));
    std::size_t done = 0;
    while (done < length) {
        const ssize_t got =
            ::pread(fd_, to.data() + done, length - done, static_cast<off_t>(offset + done));
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got < 0) {
            throw Problem{Failure::unreadable, std::strerror(errno)};
        }
        if (got == 0) {
            break;  // shorter than fstat said: it shrank meanwhile
        }
        done += static_cast<std::size_t>(got);
    }
    return done;
}

void File::read_exactly(std::uint64_t offset, std::span<std::uint8_t> to, const char* what) const {
    if (read_at(offset, to) != to.size()) {
        damaged(std::string{"cut short in "} + what);
    }
}

bool Stream::skip(std::uint64_t count) {
    std::array<std::uint8_t, kChunk> scratch{};
    while (count > 0) {
        const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(count, scratch.size()));
        const std::size_t got = read(std::span{scratch}.first(want));
        if (got == 0) {
            return false;
        }
        count -= got;
    }
    return true;
}

bool Stream::read_exactly(std::span<std::uint8_t> to) {
    std::size_t done = 0;
    while (done < to.size()) {
        const std::size_t got = read(to.subspan(done));
        if (got == 0) {
            return false;
        }
        done += got;
    }
    return true;
}

FileStream::FileStream(const File& file, std::uint64_t begin, std::uint64_t end)
    : file_{file}, at_{begin}, end_{std::min(end, file.size())} {}

std::size_t FileStream::read(std::span<std::uint8_t> to) {
    if (at_ >= end_) {
        return 0;
    }
    const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(to.size(), end_ - at_));
    const std::size_t got = file_.read_at(at_, to.first(want));
    at_ += got;
    offset_ += got;
    return got;
}

bool FileStream::skip(std::uint64_t count) {
    if (count > end_ - std::min(at_, end_)) {
        offset_ += end_ - std::min(at_, end_);
        at_ = end_;
        return false;
    }
    at_ += count;
    offset_ += count;
    return true;
}

struct Inflater::State {
    const File& file;
    std::uint64_t at;
    std::uint64_t end;
    Wrapping wrapping;
    z_stream z{};
    bool ready{false};
    bool finished{false};
    std::vector<std::uint8_t> input = std::vector<std::uint8_t>(kChunk);

    State(const File& f, std::uint64_t begin, std::uint64_t stop, Wrapping w)
        : file{f}, at{begin}, end{std::min(stop, f.size())}, wrapping{w} {}

    // More compressed bytes into the input, after what zlib has not taken yet; false at the end.
    bool refill() {
        if (at >= end) {
            return false;
        }
        const std::size_t kept = z.avail_in;
        if (kept > 0 && z.next_in != input.data()) {
            std::memmove(input.data(), z.next_in, kept);
        }
        const auto want =
            static_cast<std::size_t>(std::min<std::uint64_t>(input.size() - kept, end - at));
        const std::size_t got = file.read_at(at, std::span{input}.subspan(kept, want));
        at += got;
        z.next_in = input.data();
        z.avail_in = static_cast<uInt>(kept + got);
        return got > 0;
    }

    // After a gzip member: another one follows (its magic next), or the stream is over.
    bool another_member() {
        if (z.avail_in < 2) {
            refill();
        }
        if (z.avail_in < 2 || z.next_in[0] != 0x1F || z.next_in[1] != 0x8B) {
            return false;
        }
        return inflateReset(&z) == Z_OK;
    }
};

Inflater::Inflater(const File& file, std::uint64_t begin, std::uint64_t end, Wrapping wrapping)
    : state_{std::make_unique<State>(file, begin, end, wrapping)} {
    const int bits = wrapping == Wrapping::raw ? -MAX_WBITS : MAX_WBITS + 16;
    if (inflateInit2(&state_->z, bits) != Z_OK) {
        throw Problem{Failure::unreadable, "out of memory"};
    }
    state_->ready = true;
}

Inflater::~Inflater() {
    if (state_->ready) {
        inflateEnd(&state_->z);
    }
}

std::size_t Inflater::read(std::span<std::uint8_t> to) {
    State& s = *state_;
    if (s.finished || to.empty()) {
        return 0;
    }
    s.z.next_out = to.data();
    s.z.avail_out = static_cast<uInt>(std::min<std::size_t>(to.size(), kChunk));
    const uInt wanted = s.z.avail_out;
    while (s.z.avail_out == wanted) {
        if (s.z.avail_in == 0 && !s.refill()) {
            damaged("the compressed data is cut short");
        }
        const int status = inflate(&s.z, Z_NO_FLUSH);
        if (status == Z_STREAM_END) {
            if (s.wrapping == Wrapping::raw || !s.another_member()) {
                s.finished = true;
                break;
            }
            continue;
        }
        if (status == Z_BUF_ERROR && s.z.avail_in == 0) {
            continue;  // wants more input: refilled above
        }
        if (status != Z_OK) {
            damaged(s.z.msg != nullptr ? std::string{"the compressed data is broken: "} + s.z.msg
                                       : std::string{"the compressed data is broken"});
        }
    }
    const std::size_t got = wanted - s.z.avail_out;
    offset_ += got;
    return got;
}

void Sink::write(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > max_ - written_) {
        throw Problem{Failure::too_large, "larger than allowed"};
    }
    put(bytes);
    written_ += bytes.size();
}

void FdSink::put(std::span<const std::uint8_t> bytes) {
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t wrote = ::write(fd_, bytes.data() + done, bytes.size() - done);
        if (wrote < 0 && errno == EINTR) {
            continue;
        }
        if (wrote <= 0) {
            throw Problem{Failure::unreadable,
                          std::string{"could not be written: "} + std::strerror(errno)};
        }
        done += static_cast<std::size_t>(wrote);
    }
}

void MemorySink::put(std::span<const std::uint8_t> bytes) {
    bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
}

void copy(Stream& from, std::uint64_t count, Sink& to, std::uint32_t* crc) {
    std::vector<std::uint8_t> buffer(kChunk);
    while (count > 0) {
        const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(count, buffer.size()));
        const std::size_t got = from.read(std::span{buffer}.first(want));
        if (got == 0) {
            damaged("an entry is cut short");
        }
        const std::span<const std::uint8_t> piece = std::span{buffer}.first(got);
        to.write(piece);
        if (crc != nullptr) {
            *crc = static_cast<std::uint32_t>(
                crc32(*crc, piece.data(), static_cast<uInt>(piece.size())));
        }
        count -= got;
    }
}

void copy_all(Stream& from, Sink& to, std::uint32_t* crc) {
    std::vector<std::uint8_t> buffer(kChunk);
    while (true) {
        const std::size_t got = from.read(buffer);
        if (got == 0) {
            return;
        }
        const std::span<const std::uint8_t> piece = std::span{buffer}.first(got);
        to.write(piece);
        if (crc != nullptr) {
            *crc = static_cast<std::uint32_t>(
                crc32(*crc, piece.data(), static_cast<uInt>(piece.size())));
        }
    }
}

std::uint16_t le16(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::uint16_t>(bytes[at] | (bytes[at + 1] << 8U));
}

std::uint32_t le32(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::uint32_t>(le16(bytes, at)) |
           (static_cast<std::uint32_t>(le16(bytes, at + 2)) << 16U);
}

std::uint64_t le64(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::uint64_t>(le32(bytes, at)) |
           (static_cast<std::uint64_t>(le32(bytes, at + 4)) << 32U);
}

std::int64_t epoch_seconds(int year, int month, int day, int hour, int minute, int second) {
    // Howard Hinnant's days_from_civil.
    const std::int64_t y = year - (month <= 2 ? 1 : 0);
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const std::int64_t yoe = y - (era * 400);
    const std::int64_t doy = (((153 * (month + (month > 2 ? -3 : 9))) + 2) / 5) + day - 1;
    const std::int64_t doe = (yoe * 365) + (yoe / 4) - (yoe / 100) + doy;
    const std::int64_t days = (era * 146097) + doe - 719468;
    return (days * 86400) + (std::int64_t{hour} * 3600) + (std::int64_t{minute} * 60) + second;
}

}  // namespace rummage::formats::archive::detail
