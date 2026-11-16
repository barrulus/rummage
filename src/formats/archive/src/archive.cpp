#include "rummage/formats/archive/archive.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fcntl.h>
#include <new>
#include <span>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "readers.hpp"
#include "stream.hpp"

namespace rummage::formats::archive {

using rummage::archive::Entry;
using rummage::archive::Failure;
using rummage::archive::Format;
using rummage::archive::Limits;
using rummage::archive::Listing;
using rummage::archive::Type;
using rummage::archive::Unreadable;
using namespace detail;

namespace {

constexpr std::size_t kBlock = 512;
constexpr std::size_t kMaxGzipName = 4096;

bool is_zip(std::span<const std::uint8_t> head) {
    return head.size() >= 4 && head[0] == 'P' && head[1] == 'K' &&
           ((head[2] == 3 && head[3] == 4) || (head[2] == 5 && head[3] == 6) ||
            (head[2] == 7 && head[3] == 8));  // spanned marker before the first entry
}

bool is_gzip(std::span<const std::uint8_t> head) {
    return head.size() >= 3 && head[0] == 0x1F && head[1] == 0x8B && head[2] == 8;
}

// A single .gz file's one entry: its name and time from the header, its size from the trailer.
Entry gzip_entry(const File& file) {
    std::array<std::uint8_t, 10> header{};
    file.read_exactly(0, header, "its header");
    const std::uint8_t flags = header[3];
    if ((flags & 0xE0U) != 0) {
        damaged("its header is broken");
    }
    std::uint64_t at = header.size();
    if ((flags & 0x04U) != 0) {  // FEXTRA
        std::array<std::uint8_t, 2> length{};
        file.read_exactly(at, length, "its header");
        at += 2U + le16(length, 0);
    }
    std::string name;
    if ((flags & 0x08U) != 0) {  // FNAME: ISO 8859-1, ending with a NUL
        std::vector<std::uint8_t> bytes(kMaxGzipName);
        const std::size_t got = file.read_at(at, bytes);
        const auto end = std::find(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(got),
                                   std::uint8_t{0});
        if (end == bytes.begin() + static_cast<std::ptrdiff_t>(got)) {
            damaged("its header is broken");
        }
        for (auto it = bytes.begin(); it != end; ++it) {
            if (*it < 0x80) {
                name.push_back(static_cast<char>(*it));
            } else {  // Latin-1 to UTF-8
                name.push_back(static_cast<char>(0xC0U | (*it >> 6U)));
                name.push_back(static_cast<char>(0x80U | (*it & 0x3FU)));
            }
        }
        // Only the name: a path in it is not followed.
        if (const std::size_t slash = name.find_last_of('/'); slash != std::string::npos) {
            name.erase(0, slash + 1);
        }
        if (name == "." || name == "..") {
            name.clear();
        }
    }
    Entry entry;
    entry.path = name;  // empty: the archive's own name without .gz
    entry.mtime = le32(header, 4);
    entry.packed = file.size();
    if (file.size() >= 18) {
        std::array<std::uint8_t, 4> trailer{};
        file.read_exactly(file.size() - 4, trailer, "its end");
        entry.size = le32(trailer, 0);
    }
    return entry;
}

const char* why_unreadable(Unreadable unreadable) {
    switch (unreadable) {
        case Unreadable::encrypted:
            return "it is encrypted";
        case Unreadable::method:
            return "its compression is not read yet";
        case Unreadable::kind:
            return "it is not a file";
        case Unreadable::none:
            break;
    }
    return "";
}

}  // namespace

Listing list(int fd, const Limits& limits) {
    Listing listing;
    try {
        const File file{fd};
        std::array<std::uint8_t, kBlock> head{};
        const std::size_t got = file.read_at(0, head);
        const std::span<const std::uint8_t> start = std::span{head}.first(got);
        if (is_zip(start)) {
            list_zip(file, limits, listing);
        } else if (is_gzip(start)) {
            bool tar = false;
            {
                Inflater probe{file, 0, file.size(), Inflater::Wrapping::gzip};
                std::array<std::uint8_t, kBlock> block{};
                try {
                    tar = probe.read_exactly(block) && looks_like_tar(block);
                } catch (const Problem&) {
                    tar = false;  // too broken to tell: listed as one file, read fails later
                }
            }
            if (tar) {
                listing.format = Format::tar_gzip;
                Inflater stream{file, 0, file.size(), Inflater::Wrapping::gzip};
                list_tar(stream, limits, listing);
            } else {
                listing.format = Format::gzip;
                listing.entries.push_back(gzip_entry(file));
            }
        } else if (got == kBlock && looks_like_tar(start)) {
            listing.format = Format::tar;
            FileStream stream{file, 0, file.size()};
            list_tar(stream, limits, listing);
        } else {
            listing.failure = Failure::not_archive;
            listing.detail = "not an archive rummage reads";
        }
    } catch (const Problem& problem) {
        listing.failure = problem.failure;
        listing.detail = problem.detail;
    } catch (const std::bad_alloc&) {
        listing.failure = Failure::too_large;
        listing.detail = "too large to list";
    }
    return listing;
}

rummage::archive::Read read_entry(int fd, Format format, const Entry& entry, int out,
                                  std::uint64_t max_bytes) {
    rummage::archive::Read result;
    if (entry.unreadable != Unreadable::none) {
        result.failure = Failure::unreadable;
        result.detail = why_unreadable(entry.unreadable);
        return result;
    }
    if (entry.type != Type::file && entry.type != Type::symlink) {
        result.failure = Failure::unreadable;
        result.detail = "it is not a file";
        return result;
    }
    FdSink sink{out, max_bytes};
    try {
        const File file{fd};
        switch (format) {
            case Format::zip:
                read_zip(file, entry, sink);
                break;
            case Format::tar: {
                if (entry.locator > file.size() || entry.size > file.size() - entry.locator) {
                    damaged("an entry is cut short");
                }
                FileStream stream{file, entry.locator, entry.locator + entry.size};
                copy(stream, entry.size, sink);
                break;
            }
            case Format::tar_gzip: {
                Inflater stream{file, 0, file.size(), Inflater::Wrapping::gzip};
                if (!stream.skip(entry.locator)) {
                    damaged("an entry is cut short");
                }
                copy(stream, entry.size, sink);
                break;
            }
            case Format::gzip: {
                Inflater stream{file, 0, file.size(), Inflater::Wrapping::gzip};
                copy_all(stream, sink);
                break;
            }
            case Format::unknown:
                result.failure = Failure::not_archive;
                result.detail = "not an archive rummage reads";
                break;
        }
    } catch (const Problem& problem) {
        result.failure = problem.failure;
        result.detail = problem.detail;
    } catch (const std::bad_alloc&) {
        result.failure = Failure::too_large;
        result.detail = "too large to read";
    }
    result.bytes = sink.written();
    return result;
}

struct Sequence::State {
    int fd{-1};
    dev_t device{0};
    ino_t inode{0};
    off_t size{0};
    timespec mtime{};
    std::unique_ptr<File> file;
    std::unique_ptr<Inflater> stream;

    State() = default;
    State(const State&) = delete;
    State& operator=(const State&) = delete;
    State(State&&) = delete;
    State& operator=(State&&) = delete;

    ~State() {
        stream.reset();
        file.reset();
        if (fd >= 0) {
            ::close(fd);
        }
    }

    [[nodiscard]] bool same(const struct stat& info) const {
        return fd >= 0 && info.st_dev == device && info.st_ino == inode && info.st_size == size &&
               info.st_mtim.tv_sec == mtime.tv_sec && info.st_mtim.tv_nsec == mtime.tv_nsec;
    }
};

Sequence::Sequence() = default;

Sequence::~Sequence() = default;

void Sequence::forget() { state_.reset(); }

rummage::archive::Read Sequence::read(int fd, Format format, const Entry& entry, int out,
                                      std::uint64_t max_bytes) {
    struct stat info{};
    if (format != Format::tar_gzip || entry.type != Type::file ||
        entry.unreadable != Unreadable::none || ::fstat(fd, &info) != 0) {
        return read_entry(fd, format, entry, out, max_bytes);  // read where it is: no stream
    }
    rummage::archive::Read result;
    FdSink sink{out, max_bytes};
    try {
        if (!state_ || !state_->same(info) || !state_->stream ||
            state_->stream->offset() > entry.locator) {
            state_ = std::make_unique<State>();
            state_->fd = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
            if (state_->fd < 0) {
                state_.reset();
                return read_entry(fd, format, entry, out, max_bytes);
            }
            state_->device = info.st_dev;
            state_->inode = info.st_ino;
            state_->size = info.st_size;
            state_->mtime = info.st_mtim;
            state_->file = std::make_unique<File>(state_->fd);
            state_->stream = std::make_unique<Inflater>(*state_->file, 0, state_->file->size(),
                                                        Inflater::Wrapping::gzip);
        }
        Inflater& stream = *state_->stream;
        if (!stream.skip(entry.locator - stream.offset())) {
            damaged("an entry is cut short");
        }
        copy(stream, entry.size, sink);
    } catch (const Problem& problem) {
        state_.reset();  // where the stream is now is not known: from the start next time
        result.failure = problem.failure;
        result.detail = problem.detail;
    } catch (const std::bad_alloc&) {
        state_.reset();
        result.failure = Failure::too_large;
        result.detail = "too large to read";
    }
    result.bytes = sink.written();
    return result;
}

}  // namespace rummage::formats::archive
