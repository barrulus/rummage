#pragma once

namespace rummage::formats::image {
// Prepare the calling thread's SVG font map before entering the helper sandbox.
// Only the selected generic font faces remain available after startup.
void prepare_svg_fonts();

// Release this thread's prepared map when no SVG render is active. A later preparation
// needs filesystem access again; intended for repeated isolated lifecycles in fuzz harnesses.
void release_svg_fonts();
}  // namespace rummage::formats::image
