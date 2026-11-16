#pragma once
#include "rummage/text/text.hpp"

namespace rummage::formats::text {
// Parse already-decoded lines inside the helper. Bounded work, depth, output and no I/O.
[[nodiscard]] rummage::text::Markdown markdown(const rummage::text::Text& source);
}  // namespace rummage::formats::text
