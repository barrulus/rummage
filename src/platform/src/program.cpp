#include "rummage/platform/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <functional>
#include <span>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

namespace rummage::platform {
namespace {

constexpr std::uint16_t kTypeExec = 2;      // ET_EXEC
constexpr std::uint16_t kTypeDyn = 3;       // ET_DYN
constexpr std::uint32_t kInterp = 3;        // PT_INTERP
constexpr std::uint16_t kMaxHeaders = 512;  // more program headers than any real file has

// A little- or big-endian unsigned number of size bytes at bytes.
std::uint64_t number(std::span<const unsigned char> bytes, bool little) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const std::size_t at = little ? bytes.size() - 1 - i : i;
        value = (value << 8U) | bytes[at];
    }
    return value;
}

}  // namespace

bool is_elf_program(
    const std::function<bool(std::uint64_t offset, std::span<unsigned char> out)>& read) {
    std::array<unsigned char, 64> header{};
    if (!read(0, std::span{header}.first(52))) {  // an ELF32 header is 52 bytes
        return false;
    }
    if (header[0] != 0x7F || header[1] != 'E' || header[2] != 'L' || header[3] != 'F') {
        return false;
    }
    const bool wide = header[4] == 2;  // ELFCLASS64
    if ((header[4] != 1 && !wide) || (header[5] != 1 && header[5] != 2)) {
        return false;
    }
    const bool little = header[5] == 1;
    if (wide && !read(0, std::span{header})) {
        return false;
    }
    const std::span<const unsigned char> bytes{header};
    const auto type = static_cast<std::uint16_t>(number(bytes.subspan(16, 2), little));
    if (type == kTypeExec) {
        return true;
    }
    if (type != kTypeDyn) {
        return false;  // an object file, a core dump
    }
    // An AppImage's runtime is a static PIE: no PT_INTERP, but the AppImage magic in the ELF
    // header's padding ("AI" and the type, 1 or 2, at byte 8), as the AppImage spec puts it.
    if (header[8] == 'A' && header[9] == 'I' && (header[10] == 1 || header[10] == 2)) {
        return true;
    }
    const std::uint64_t offset =
        wide ? number(bytes.subspan(32, 8), little) : number(bytes.subspan(28, 4), little);
    const auto size = static_cast<std::uint16_t>(number(bytes.subspan(wide ? 54 : 42, 2), little));
    const auto count = static_cast<std::uint16_t>(number(bytes.subspan(wide ? 56 : 44, 2), little));
    if (offset == 0 || size < 4 || count > kMaxHeaders) {
        return false;
    }
    for (std::uint16_t i = 0; i < count; ++i) {
        std::array<unsigned char, 4> kind{};
        const std::uint64_t at = offset + (static_cast<std::uint64_t>(i) * size);
        if (at < offset || !read(at, std::span{kind})) {
            return false;
        }
        if (number(std::span{kind}, little) == kInterp) {
            return true;
        }
    }
    return false;
}

ProgramCheck check_program(const std::filesystem::path& path) {
    ProgramCheck check;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return check;
    }
    check.program = is_elf_program([fd](std::uint64_t offset, std::span<unsigned char> out) {
        if (offset > static_cast<std::uint64_t>(INT64_MAX)) {
            return false;
        }
        return ::pread(fd, out.data(), out.size(), static_cast<off_t>(offset)) ==
               static_cast<ssize_t>(out.size());
    });
    struct statvfs volume{};
    check.noexec = ::fstatvfs(fd, &volume) == 0 && (volume.f_flag & ST_NOEXEC) != 0;
    ::close(fd);
    check.executable = ::access(path.c_str(), X_OK) == 0;
    return check;
}

}  // namespace rummage::platform
