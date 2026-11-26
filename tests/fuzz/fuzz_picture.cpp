// Fuzzes the whole image family the way the preview helper runs it (ROADMAP 2c): the bytes in a
// memfd, through the formats registry, a picture and a photo's information.
#include <cstddef>
#include <cstdint>
#include <fontconfig/fontconfig.h>
#include <sys/mman.h>
#include <unistd.h>

#include "rummage/formats/formats.hpp"
#include "rummage/formats/image/svg.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const int fd = ::memfd_create("fuzz", MFD_CLOEXEC);
    if (fd < 0) {
        return 0;
    }
    if (::write(fd, data, size) == static_cast<ssize_t>(size)) {
        static_cast<void>(rummage::formats::make_picture(fd, 256, 256));
        static_cast<void>(rummage::formats::photo_info(fd));
    }
    ::close(fd);
    // This standalone harness owns every font user. End the prepared-map lifecycle before
    // libFuzzer's per-input leak check, then release Fontconfig's process-global caches.
    // Production helpers retain their map across requests and must not finalize Fontconfig
    // while another thread or renderer may still use it.
    rummage::formats::image::release_svg_fonts();
    FcFini();
    return 0;
}
