#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "rummage/picture/load.hpp"

namespace rummage::svg {
inline constexpr std::size_t kMaxInputBytes = 8U * 1024U * 1024U;
inline constexpr std::uint32_t kMaxSide = 1024;

// Shared librsvg adapter for theme icons and isolated file previews. No base URI:
// only internal references and data URIs are available. Returns straight-alpha RGBA.
// Fit within both bounds, preserving aspect ratio. Zero means kMaxSide; larger
// bounds are capped. Raster dimensions are not intrinsic document dimensions.
// Malformed input and allocation failures return an empty image with a failure.
[[nodiscard]] picture::Loaded render(std::span<const std::uint8_t> bytes, std::uint32_t max_width,
                                     std::uint32_t max_height) noexcept;

// Square-bound convenience for icons/tools. Invalid sizes (outside 1..kMaxSide)
// and rendering failures return an empty image.
[[nodiscard]] picture::Image render(std::span<const std::uint8_t> bytes, int pixels) noexcept;
}  // namespace rummage::svg
