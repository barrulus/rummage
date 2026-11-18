#pragma once

#include <cstdint>
#include <span>

#include "rummage/formats/image/load.hpp"

namespace rummage::formats::image::detail {
// The caller bounds the source to 8 MiB and each output side to 1..1024.
Loaded decode_svg(std::span<const std::uint8_t> bytes, std::uint32_t width, std::uint32_t height);
}  // namespace rummage::formats::image::detail
