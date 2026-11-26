#include "rummage/fs/compress.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <functional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <vector>
#include <zlib.h>

#include "rummage/fs/operations.hpp"

namespace rummage::fs {
namespace {

constexpr std::size_t kPiece = std::size_t{128} * 1024;  // read from a file at a time
constexpr std::size_t kBuffer = std::size_t{1} << 20;    // written to the archive at a time
constexpr int kLevel = 6;                                // zlib's default: size and speed
constexpr std::uint32_t kMax32 = 0xFFFFFFFF;
constexpr std::uint64_t kZip64Above = 0xFFFF0000;  // a file this large may deflate past 4 GiB
constexpr std::string_view kTemporary = ".rummage-part-";

std::string errno_text(int error) {
    return std::error_code{error, std::generic_category()}.message();
}

// Something that stops the archive: its words.
struct Stop {
    std::string why;
};

// What goes in: its path in the archive ('/' between parts), and what it is on disk.
struct Item {
    std::string name;  // "Trip/day 1/a.jpg"
    std::filesystem::path disk;
    struct stat info{};
    std::string link;  // a symbolic link's target
};

[[nodiscard]] bool valid_utf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::size_t extra = 0;
        if (c < 0x80) {
            extra = 0;
        } else if ((c & 0xE0U) == 0xC0 && c >= 0xC2) {
            extra = 1;
        } else if ((c & 0xF0U) == 0xE0) {
            extra = 2;
        } else if ((c & 0xF8U) == 0xF0 && c <= 0xF4) {
            extra = 3;
        } else {
            return false;
        }
        if (extra >= text.size() - i) {
            return extra == 0;
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            if ((static_cast<unsigned char>(text[i + k]) & 0xC0U) != 0x80) {
                return false;
            }
        }
        i += extra + 1;
    }
    return true;
}

// The archive's bytes, through a buffer, counted.
class Out {
public:
    explicit Out(int fd) : fd_{fd} { buffer_.reserve(kBuffer); }

    void put(std::span<const std::uint8_t> bytes) {
        offset_ += bytes.size();
        while (!bytes.empty()) {
            const std::size_t room = kBuffer - buffer_.size();
            const std::size_t take = std::min(room, bytes.size());
            buffer_.insert(buffer_.end(), bytes.begin(),
                           bytes.begin() + static_cast<std::ptrdiff_t>(take));
            bytes = bytes.subspan(take);
            if (buffer_.size() == kBuffer) {
                flush();
            }
        }
    }

    void put(std::string_view text) {
        put(std::span{reinterpret_cast<const std::uint8_t*>(text.data()),  // NOLINT: bytes
                      text.size()});
    }

    void le16(std::uint32_t value) {
        const std::array<std::uint8_t, 2> bytes{static_cast<std::uint8_t>(value & 0xFFU),
                                                static_cast<std::uint8_t>((value >> 8U) & 0xFFU)};
        put(bytes);
    }

    void le32(std::uint32_t value) {
        le16(value & 0xFFFFU);
        le16(value >> 16U);
    }

    void le64(std::uint64_t value) {
        le32(static_cast<std::uint32_t>(value & kMax32));
        le32(static_cast<std::uint32_t>(value >> 32U));
    }

    void flush() {
        std::size_t done = 0;
        while (done < buffer_.size()) {
            const ssize_t wrote = ::write(fd_, buffer_.data() + done, buffer_.size() - done);
            if (wrote < 0 && errno == EINTR) {
                continue;
            }
            if (wrote <= 0) {
                throw Stop{std::format("the archive could not be written: {}",
                                       errno_text(wrote < 0 ? errno : ENOSPC))};
            }
            done += static_cast<std::size_t>(wrote);
        }
        buffer_.clear();
    }

    [[nodiscard]] std::uint64_t offset() const { return offset_; }

private:
    int fd_;
    std::vector<std::uint8_t> buffer_;
    std::uint64_t offset_{0};
};

// Bytes deflated into out: raw (zip) or gzip-wrapped (tar.gz).
class Deflater {
public:
    Deflater(Out& out, bool gzip) : out_{out}, chunk_(kPiece) {
        if (deflateInit2(&z_, kLevel, Z_DEFLATED, gzip ? MAX_WBITS + 16 : -MAX_WBITS, 8,
                         Z_DEFAULT_STRATEGY) != Z_OK) {
            throw Stop{"out of memory"};
        }
    }

    ~Deflater() { deflateEnd(&z_); }

    Deflater(const Deflater&) = delete;
    Deflater& operator=(const Deflater&) = delete;
    Deflater(Deflater&&) = delete;
    Deflater& operator=(Deflater&&) = delete;

    void put(std::span<const std::uint8_t> bytes) { run(bytes, Z_NO_FLUSH); }

    void put(std::string_view text) {
        put(std::span{reinterpret_cast<const std::uint8_t*>(text.data()),  // NOLINT: bytes
                      text.size()});
    }

    // The end of the stream; the deflater starts again for the next (zip: one per entry).
    void finish() {
        run({}, Z_FINISH);
        deflateReset(&z_);
    }

    // Bytes given out since the last reset.
    [[nodiscard]] std::uint64_t written() const { return written_; }

    void reset_count() { written_ = 0; }

private:
    void run(std::span<const std::uint8_t> bytes, int flush) {
        z_.next_in = const_cast<Bytef*>(bytes.data());  // NOLINT: zlib's API, not changed
        z_.avail_in = static_cast<uInt>(bytes.size());
        for (;;) {
            z_.next_out = chunk_.data();
            z_.avail_out = static_cast<uInt>(chunk_.size());
            const int result = deflate(&z_, flush);
            if (result == Z_STREAM_ERROR) {
                throw Stop{"compressing failed"};
            }
            const std::size_t made = chunk_.size() - z_.avail_out;
            out_.put(std::span{chunk_}.first(made));
            written_ += made;
            if (flush == Z_FINISH ? result == Z_STREAM_END : z_.avail_out != 0) {
                return;
            }
        }
    }

    Out& out_;
    z_stream z_{};
    std::vector<std::uint8_t> chunk_;
    std::uint64_t written_{0};
};

// Reads a regular file's bytes, piece by piece, checked against what the walk saw.
class Reader {
public:
    Reader(const Item& item, const core::CancelToken& token)
        : item_{item},
          token_{token},
          fd_{::open(item.disk.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)} {
        struct stat now{};
        if (fd_ < 0 || ::fstat(fd_, &now) != 0 || !S_ISREG(now.st_mode) ||
            now.st_dev != item.info.st_dev || now.st_ino != item.info.st_ino) {
            const int saved_error = errno;
            const bool not_opened = fd_ < 0;
            if (fd_ >= 0) {
                ::close(fd_);
                fd_ = -1;
            }
            if (not_opened && saved_error != ELOOP) {
                throw Stop{std::format("{}: {}", item.name, errno_text(saved_error))};
            }
            throw Stop{std::format("{}: changed while being compressed", item.name)};
        }
    }

    ~Reader() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    Reader(Reader&&) = delete;
    Reader& operator=(Reader&&) = delete;

    // Each piece to use, then: exactly the size the walk saw, or Stop.
    template <typename Use>
    void each(std::vector<std::uint8_t>& piece, const Use& use) {
        const auto size = static_cast<std::uint64_t>(item_.info.st_size);
        std::uint64_t done = 0;
        for (;;) {
            if (token_.cancelled()) {
                throw Stop{};
            }
            const ssize_t got = ::read(fd_, piece.data(), piece.size());
            if (got < 0 && errno == EINTR) {
                continue;
            }
            if (got < 0) {
                throw Stop{std::format("{}: {}", item_.name, errno_text(errno))};
            }
            if (got == 0) {
                break;
            }
            done += static_cast<std::uint64_t>(got);
            if (done > size) {
                break;
            }
            use(std::span<const std::uint8_t>{piece.data(), static_cast<std::size_t>(got)});
        }
        if (done != size) {
            throw Stop{std::format("{}: changed while being compressed", item_.name)};
        }
    }

private:
    const Item& item_;
    const core::CancelToken& token_;
    int fd_;
};

// The MS-DOS date and time zip headers carry (local time, two-second steps; 1980 at the least).
std::pair<std::uint16_t, std::uint16_t> dos_time(std::int64_t seconds) {
    const auto when = static_cast<time_t>(seconds);
    std::tm local{};
    if (::localtime_r(&when, &local) == nullptr || local.tm_year < 80) {
        return {static_cast<std::uint16_t>((1U << 5U) | 1U), 0};  // 1980-01-01 00:00
    }
    const auto date = static_cast<std::uint16_t>(((local.tm_year - 80) << 9) |
                                                 ((local.tm_mon + 1) << 5) | local.tm_mday);
    const auto time = static_cast<std::uint16_t>((local.tm_hour << 11) | (local.tm_min << 5) |
                                                 (local.tm_sec / 2));
    return {date, time};
}

class Compressor {
public:
    Compressor(const TransferRequest& request, const core::CancelToken& token,
               const TransferCallbacks& callbacks)
        : request_{request}, token_{token}, callbacks_{callbacks}, piece_(kPiece) {}

    TransferResult run() {
        if (const auto problem = name_problem(request_.archive_name)) {
            result_.refused = *problem;
            return std::move(result_);
        }
        for (const auto& name : request_.names) {
            if (name_problem(name)) {
                result_.refused = "Invalid archive selection name";
                return std::move(result_);
            }
        }
        progress_.preparing = true;
        report();
        for (const std::string& name : request_.names) {
            walk(request_.source / name, name);
            if (token_.cancelled()) {
                result_.cancelled = true;
                return std::move(result_);
            }
        }
        if (items_.empty()) {
            result_.refused = "Nothing to put in the archive";
            return std::move(result_);
        }
        progress_.preparing = false;
        if (!make_temporary()) {
            return std::move(result_);
        }
        try {
            Out out{fd_};
            if (request_.tar_gz) {
                write_tar_gz(out);
            } else {
                write_zip(out);
            }
            out.flush();
            if (token_.cancelled())
                throw Stop{};
            const int closed = ::close(fd_);
            fd_ = -1;
            if (closed != 0)
                throw Stop{std::format("the archive could not be closed: {}", errno_text(errno))};
        } catch (const Stop& stop) {
            ::close(fd_);
            ::unlink(temporary_.c_str());
            if (stop.why.empty()) {
                result_.cancelled = true;
            } else {
                result_.refused = stop.why;
            }
            return std::move(result_);
        }
        if (token_.cancelled()) {
            ::unlink(temporary_.c_str());
            result_.cancelled = true;
        } else {
            publish();
        }
        return std::move(result_);
    }

private:
    void report() {
        if (callbacks_.progress) {
            callbacks_.progress(progress_);
        }
    }

    void error(const std::string& name, std::string_view why) {
        result_.errors.push_back(std::format("{}: {}", name, why));
    }

    // Everything at path, in name order, without following links (a stack, not recursion: a
    // tree may be deeper than the stack would allow).
    void walk(const std::filesystem::path& top, const std::string& top_name) {
        std::vector<std::pair<std::filesystem::path, std::string>> waiting{{top, top_name}};
        while (!waiting.empty() && !token_.cancelled()) {
            auto [path, name] = std::move(waiting.back());
            waiting.pop_back();
            Item item{.name = name, .disk = path, .info = {}, .link = {}};
            if (::lstat(path.c_str(), &item.info) != 0) {
                error(name, errno_text(errno));
                continue;
            }
            if (S_ISLNK(item.info.st_mode)) {
                std::error_code failed;
                item.link = std::filesystem::read_symlink(path, failed).string();
                if (failed) {
                    error(name, failed.message());
                    continue;
                }
                items_.push_back(std::move(item));
                continue;
            }
            if (S_ISREG(item.info.st_mode)) {
                progress_.bytes_total += static_cast<std::uint64_t>(item.info.st_size);
                ++progress_.files_total;
                items_.push_back(std::move(item));
                continue;
            }
            if (!S_ISDIR(item.info.st_mode)) {
                error(name, "not a file, left out");
                continue;
            }
            items_.push_back(std::move(item));
            std::vector<std::string> children;
            std::error_code failed;
            for (std::filesystem::directory_iterator it{path, failed}, end; !failed && it != end;
                 it.increment(failed)) {
                children.push_back(it->path().filename().string());
            }
            if (failed) {
                error(name, failed.message());
            }
            // Last pushed, first taken: in name order.
            std::ranges::sort(children, std::greater{});
            for (const std::string& child : children) {
                waiting.emplace_back(path / child, std::format("{}/{}", name, child));
            }
        }
    }

    bool make_temporary() {
        std::random_device random;
        for (int attempt = 0; attempt < 16; ++attempt) {
            temporary_ = request_.destination / std::format("{}{:08x}", kTemporary, random());
            fd_ = ::open(temporary_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                         0644);
            if (fd_ >= 0) {
                return true;
            }
            if (errno != EEXIST) {
                break;
            }
        }
        result_.refused =
            std::format("{}: {}", request_.destination.filename().string(), errno_text(errno));
        return false;
    }

    // A file's bytes through use, counted in the progress.
    template <typename Use>
    void read_file(const Item& item, const Use& use) {
        progress_.current = item.name;
        report();
        Reader reader{item, token_};
        reader.each(piece_, [&](std::span<const std::uint8_t> bytes) {
            use(bytes);
            progress_.bytes_done += bytes.size();
            report();
        });
        ++progress_.files_done;
        ++result_.files_done;
        result_.bytes_done += static_cast<std::uint64_t>(item.info.st_size);
    }

    // --- zip ---

    struct Central {
        std::string name;
        std::uint16_t flags{0};
        std::uint16_t method{0};
        std::uint16_t date{0};
        std::uint16_t time{0};
        std::uint32_t crc{0};
        std::uint64_t packed{0};
        std::uint64_t size{0};
        std::uint64_t local{0};
        std::uint32_t external{0};
        std::int64_t mtime{0};
        bool zip64{false};
    };

    static void put_unix_time(Out& out, std::int64_t mtime) {
        out.le16(0x5455);  // extended timestamp: the modification time, UTC
        out.le16(5);
        const std::array<std::uint8_t, 1> flags{1};
        out.put(flags);
        out.le32(static_cast<std::uint32_t>(
            static_cast<std::int32_t>(std::clamp<std::int64_t>(mtime, INT32_MIN, INT32_MAX))));
    }

    void write_zip(Out& out) {
        std::vector<Central> centrals;
        centrals.reserve(items_.size());
        Deflater deflater{out, false};
        for (const Item& item : items_) {
            if (token_.cancelled())
                throw Stop{};
            Central central;
            const bool folder = S_ISDIR(item.info.st_mode);
            const bool link = S_ISLNK(item.info.st_mode);
            central.name = folder ? item.name + "/" : item.name;
            central.mtime = item.info.st_mtim.tv_sec;
            std::tie(central.date, central.time) = dos_time(central.mtime);
            central.external =
                (static_cast<std::uint32_t>(item.info.st_mode) << 16U) | (folder ? 0x10U : 0U);
            central.flags = valid_utf8(central.name) ? 0x0800 : 0;
            central.local = out.offset();
            const bool file = !folder && !link;
            central.zip64 = file && std::cmp_greater_equal(item.info.st_size, kZip64Above);
            if (!file) {
                // Stored: its bytes (a link's target) known before the header.
                central.method = 0;
                central.size = link ? item.link.size() : 0;
                central.packed = central.size;
                central.crc = static_cast<std::uint32_t>(
                    crc32(0, reinterpret_cast<const Bytef*>(item.link.data()),  // NOLINT: bytes
                          static_cast<uInt>(item.link.size())));
                put_local(out, central);
                out.put(item.link);
                centrals.push_back(std::move(central));
                continue;
            }
            central.method = 8;
            central.flags |= 0x0008;  // sizes and checksum after the data
            put_local(out, central);
            auto crc = static_cast<std::uint32_t>(crc32(0, nullptr, 0));
            deflater.reset_count();
            read_file(item, [&](std::span<const std::uint8_t> bytes) {
                crc = static_cast<std::uint32_t>(
                    crc32(crc, bytes.data(), static_cast<uInt>(bytes.size())));
                deflater.put(bytes);
            });
            deflater.finish();
            central.crc = crc;
            central.size = static_cast<std::uint64_t>(item.info.st_size);
            central.packed = deflater.written();
            if (!central.zip64 && central.packed >= kMax32) {
                throw Stop{std::format("{}: grew too large while being compressed", item.name)};
            }
            out.le32(0x08074b50);  // the data descriptor
            out.le32(central.crc);
            if (central.zip64) {
                out.le64(central.packed);
                out.le64(central.size);
            } else {
                out.le32(static_cast<std::uint32_t>(central.packed));
                out.le32(static_cast<std::uint32_t>(central.size));
            }
            centrals.push_back(std::move(central));
        }
        const std::uint64_t directory = out.offset();
        for (const Central& central : centrals) {
            if (token_.cancelled())
                throw Stop{};
            put_central(out, central);
        }
        const std::uint64_t directory_size = out.offset() - directory;
        const std::uint64_t count = centrals.size();
        if (count >= 0xFFFF || directory >= kMax32 || directory_size >= kMax32) {
            const std::uint64_t record = out.offset();
            out.le32(0x06064b50);  // ZIP64 end of central directory
            out.le64(44);
            out.le16(0x033F);
            out.le16(45);
            out.le32(0);
            out.le32(0);
            out.le64(count);
            out.le64(count);
            out.le64(directory_size);
            out.le64(directory);
            out.le32(0x07064b50);  // its locator
            out.le32(0);
            out.le64(record);
            out.le32(1);
        }
        out.le32(0x06054b50);
        out.le16(0);
        out.le16(0);
        out.le16(static_cast<std::uint32_t>(std::min<std::uint64_t>(count, 0xFFFF)));
        out.le16(static_cast<std::uint32_t>(std::min<std::uint64_t>(count, 0xFFFF)));
        out.le32(static_cast<std::uint32_t>(std::min<std::uint64_t>(directory_size, kMax32)));
        out.le32(static_cast<std::uint32_t>(std::min<std::uint64_t>(directory, kMax32)));
        out.le16(0);
    }

    static void put_local(Out& out, const Central& central) {
        out.le32(0x04034b50);
        out.le16(central.zip64 ? 45 : 20);
        out.le16(central.flags);
        out.le16(central.method);
        out.le16(central.time);
        out.le16(central.date);
        // Sizes and checksum after the data: zeros here (ZIP64: all ones, the field below).
        const bool later = (central.flags & 0x0008U) != 0;
        std::uint32_t packed = later ? 0 : static_cast<std::uint32_t>(central.packed);
        std::uint32_t size = later ? 0 : static_cast<std::uint32_t>(central.size);
        if (central.zip64) {
            packed = kMax32;
            size = kMax32;
        }
        out.le32(later ? 0 : central.crc);
        out.le32(packed);
        out.le32(size);
        out.le16(static_cast<std::uint32_t>(central.name.size()));
        out.le16(9 + (central.zip64 ? 20 : 0));
        out.put(central.name);
        if (central.zip64) {
            out.le16(0x0001);  // ZIP64: the sizes come in the data descriptor
            out.le16(16);
            out.le64(0);
            out.le64(0);
        }
        put_unix_time(out, central.mtime);
    }

    static void put_central(Out& out, const Central& central) {
        const bool wide_size = central.size >= kMax32 || central.zip64;
        const bool wide_packed = central.packed >= kMax32 || central.zip64;
        const bool wide_local = central.local >= kMax32;
        const std::uint32_t wide =
            (wide_size ? 8U : 0U) + (wide_packed ? 8U : 0U) + (wide_local ? 8U : 0U);
        out.le32(0x02014b50);
        out.le16(0x033F);  // made by Unix, spec 6.3
        out.le16(wide > 0 ? 45 : 20);
        out.le16(central.flags);
        out.le16(central.method);
        out.le16(central.time);
        out.le16(central.date);
        out.le32(central.crc);
        out.le32(wide_packed ? kMax32 : static_cast<std::uint32_t>(central.packed));
        out.le32(wide_size ? kMax32 : static_cast<std::uint32_t>(central.size));
        out.le16(static_cast<std::uint32_t>(central.name.size()));
        out.le16(9 + (wide > 0 ? 4 + wide : 0));
        out.le16(0);  // comment
        out.le16(0);  // disk
        out.le16(0);  // internal attributes
        out.le32(central.external);
        out.le32(wide_local ? kMax32 : static_cast<std::uint32_t>(central.local));
        out.put(central.name);
        if (wide > 0) {
            out.le16(0x0001);
            out.le16(wide);
            if (wide_size) {
                out.le64(central.size);
            }
            if (wide_packed) {
                out.le64(central.packed);
            }
            if (wide_local) {
                out.le64(central.local);
            }
        }
        put_unix_time(out, central.mtime);
    }

    // --- tar.gz (pax) ---

    // A ustar header: what fits; the rest is said in a pax header before it.
    static std::array<char, 512> ustar(std::string_view name, const struct stat& info, char type,
                                       std::uint64_t size, std::string_view link) {
        std::array<char, 512> block{};
        const auto text = [&block](std::size_t at, std::size_t width, std::string_view value) {
            std::ranges::copy(value.substr(0, width),
                              block.begin() + static_cast<std::ptrdiff_t>(at));
        };
        const auto octal = [&block](std::size_t at, std::size_t width, std::uint64_t value) {
            std::string digits(width - 1, '0');
            for (std::size_t i = width - 1; i-- > 0 && value > 0; value >>= 3U) {
                digits[i] = static_cast<char>('0' + (value & 7U));
            }
            std::ranges::copy(digits, block.begin() + static_cast<std::ptrdiff_t>(at));
        };
        text(0, 100, name);
        octal(100, 8, info.st_mode & 07777U);
        octal(108, 8, std::min<std::uint64_t>(info.st_uid, 07777777));
        octal(116, 8, std::min<std::uint64_t>(info.st_gid, 07777777));
        octal(124, 12, std::min<std::uint64_t>(size, 077777777777));
        octal(136, 12,
              static_cast<std::uint64_t>(
                  std::clamp<std::int64_t>(info.st_mtim.tv_sec, 0, 077777777777)));
        block[156] = type;
        text(157, 100, link);
        text(257, 6, std::string_view{"ustar\0", 6});
        text(263, 2, "00");
        std::fill(block.begin() + 148, block.begin() + 156, ' ');
        unsigned sum = 0;
        for (const char c : block) {
            sum += static_cast<unsigned char>(c);
        }
        octal(148, 7, sum);
        return block;
    }

    // "30 path=…\n": the length counts itself.
    static std::string pax_record(std::string_view key, std::string_view value) {
        const std::size_t body = key.size() + value.size() + 3;  // ' ', '=', '\n'
        std::size_t length = body + 1;
        while (std::to_string(length).size() + body != length) {
            ++length;
        }
        return std::format("{} {}={}\n", length, key, value);
    }

    static void pad(Deflater& out, std::uint64_t size) {
        static const std::array<char, 512> kZeros{};
        if (const std::uint64_t rest = size % 512; rest != 0) {
            out.put(std::string_view{kZeros.data(), 512 - rest});
        }
    }

    void write_tar_gz(Out& out) {
        Deflater gzip{out, true};
        for (const Item& item : items_) {
            if (token_.cancelled())
                throw Stop{};
            const bool folder = S_ISDIR(item.info.st_mode);
            const bool link = S_ISLNK(item.info.st_mode);
            const std::string name = folder ? item.name + "/" : item.name;
            const auto size = link || folder ? 0 : static_cast<std::uint64_t>(item.info.st_size);
            std::string pax;
            if (name.size() > 100) {
                pax += pax_record("path", name);
            }
            if (item.link.size() > 100) {
                pax += pax_record("linkpath", item.link);
            }
            if (size > 077777777777) {
                pax += pax_record("size", std::to_string(size));
            }
            if (item.info.st_mtim.tv_sec < 0 || item.info.st_mtim.tv_sec > 077777777777) {
                pax += pax_record("mtime", std::to_string(item.info.st_mtim.tv_sec));
            }
            if (!pax.empty()) {
                const std::array<char, 512> header =
                    ustar("././@PaxHeader", item.info, 'x', pax.size(), {});
                gzip.put(std::string_view{header.data(), header.size()});
                gzip.put(pax);
                pad(gzip, pax.size());
            }
            char type = '0';
            if (folder) {
                type = '5';
            } else if (link) {
                type = '2';
            }
            const std::array<char, 512> header = ustar(name, item.info, type, size, item.link);
            gzip.put(std::string_view{header.data(), header.size()});
            if (!folder && !link) {
                read_file(item, [&](std::span<const std::uint8_t> bytes) { gzip.put(bytes); });
                pad(gzip, size);
            }
        }
        static const std::array<char, 1024> kEnd{};
        gzip.put(std::string_view{kEnd.data(), kEnd.size()});
        gzip.finish();
    }

    // The archive under its name, or the first free "x 2"; never over anything.
    void publish() {
        // "Trip 2.tar.gz", "Trip 2.zip": the number before the whole ending.
        const std::string& wanted = request_.archive_name;
        std::size_t dot = wanted.ends_with(".tar.gz") ? wanted.size() - 7 : wanted.rfind('.');
        if (dot == std::string::npos || dot == 0) {
            dot = wanted.size();
        }
        std::string name = wanted;
        for (int attempt = 0; attempt < 1000; ++attempt) {
            if (attempt > 0) {
                name =
                    std::format("{} {}{}", wanted.substr(0, dot), attempt + 1, wanted.substr(dot));
            }
            const auto destination = request_.destination / name;
            bool published = ::renameat2(AT_FDCWD, temporary_.c_str(), AT_FDCWD,
                                         destination.c_str(), RENAME_NOREPLACE) == 0;
            if (!published && (errno == EINVAL || errno == ENOSYS || errno == EOPNOTSUPP)) {
                // link publishes a complete regular file atomically without replacing a name.
                published = ::link(temporary_.c_str(), destination.c_str()) == 0;
                if (published)
                    ::unlink(temporary_.c_str());
            }
            if (published) {
                result_.placed.push_back(name);
                result_.items.push_back({.from = {}, .to = request_.destination / name});
                return;
            }
            if (errno != EEXIST) {
                break;
            }
        }
        result_.refused = std::format("{}: {}", request_.archive_name, errno_text(errno));
        ::unlink(temporary_.c_str());
    }

    const TransferRequest& request_;
    const core::CancelToken& token_;
    const TransferCallbacks& callbacks_;
    std::vector<Item> items_;
    std::vector<std::uint8_t> piece_;
    TransferProgress progress_;
    TransferResult result_;
    std::filesystem::path temporary_;
    int fd_{-1};
};

}  // namespace

TransferResult run_compress(const TransferRequest& request, const core::CancelToken& token,
                            const TransferCallbacks& callbacks) {
    return Compressor{request, token, callbacks}.run();
}

std::string archive_name_for(const std::vector<std::string>& names, bool first_is_folder,
                             bool tar_gz) {
    std::string stem = "Archive";
    if (names.size() == 1) {
        stem = names.front();
        if (!first_is_folder) {
            if (const std::size_t dot = stem.rfind('.'); dot != std::string::npos && dot > 0) {
                stem.resize(dot);
            }
        }
    }
    return stem + (tar_gz ? ".tar.gz" : ".zip");
}

}  // namespace rummage::fs
