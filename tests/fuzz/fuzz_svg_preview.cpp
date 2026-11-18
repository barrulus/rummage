#include <cstddef>
#include <cstdint>

#include "svg_decoder.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > 8U * 1024U * 1024U) {
        return 0;
    }
    const std::uint32_t width = size ? 1U + data[0] : 256U;
    const std::uint32_t height = size > 1 ? 1U + data[1] : 256U;
    static_cast<void>(rummage::formats::image::detail::decode_svg({data, size}, width, height));
    return 0;
}
