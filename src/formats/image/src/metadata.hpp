#pragma once

#include "fd_file.hpp"
#include "rummage/picture/photo_info.hpp"

namespace rummage::formats::image::detail {
// Bounded, descriptor-only metadata reads; never decodes image pixels.
void read_metadata(const FdFile& file, picture::PhotoInfo& info);
}  // namespace rummage::formats::image::detail
