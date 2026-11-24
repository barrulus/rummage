#include <cstddef>
#include <cstdint>

#include "rummage/svg/svg.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::uint32_t width = size ? 1U + data[0] : 256U;
    const std::uint32_t height = size > 1 ? 1U + data[1] : 256U;
    static_cast<void>(rummage::svg::render({data, size}, width, height));
    return 0;
}
