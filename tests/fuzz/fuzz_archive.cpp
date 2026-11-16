// Fuzzes the archive family (ROADMAP 0.9.0) the way the base engine runs it: the input is the
// archive, listed from a memfd with small limits, then every entry read into another memfd (at
// most 1 MiB each), then every entry again through a Sequence in the archive's order, as
// extracting reads them (0.9.0 step 4). The format is found from the first bytes, so one target
// reaches zip, tar, tar.gz and .gz; seeds: tests/data/archive.
#include <cstddef>
#include <cstdint>
#include <sys/mman.h>
#include <unistd.h>

#include "rummage/formats/archive/archive.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const int archive = ::memfd_create("fuzz-archive", MFD_CLOEXEC);
    const int out = ::memfd_create("fuzz-entry", MFD_CLOEXEC);
    if (archive < 0 || out < 0 || ::write(archive, data, size) != static_cast<ssize_t>(size)) {
        ::close(archive);
        ::close(out);
        return 0;
    }
    const rummage::archive::Listing listing =
        rummage::formats::archive::list(archive, {.max_entries = 4096, .max_path_bytes = 4096});
    for (const rummage::archive::Entry& entry : listing.entries) {
        static_cast<void>(::ftruncate(out, 0));
        static_cast<void>(::lseek(out, 0, SEEK_SET));
        static_cast<void>(rummage::formats::archive::read_entry(archive, listing.format, entry, out,
                                                                std::uint64_t{1} << 20U));
    }
    rummage::formats::archive::Sequence sequence;
    for (const rummage::archive::Entry& entry : listing.entries) {
        static_cast<void>(::ftruncate(out, 0));
        static_cast<void>(::lseek(out, 0, SEEK_SET));
        static_cast<void>(
            sequence.read(archive, listing.format, entry, out, std::uint64_t{1} << 20U));
    }
    sequence.forget();
    ::close(archive);
    ::close(out);
    return 0;
}
