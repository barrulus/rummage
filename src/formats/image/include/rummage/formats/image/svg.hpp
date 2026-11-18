#pragma once

namespace rummage::formats::image {
// Prepare the calling thread's SVG font map before entering the helper sandbox.
// Only the selected generic font faces remain available after startup.
void prepare_svg_fonts();
}  // namespace rummage::formats::image
