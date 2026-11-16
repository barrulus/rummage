#pragma once
#include <string_view>

#include "rummage/archive/archive.hpp"
#include "rummage/picture/load.hpp"
#include "rummage/text/text.hpp"

namespace rummage::isolate::backend {
picture::Loaded make_picture(int fd, std::uint32_t width, std::uint32_t height);
picture::PhotoInfo photo_info(int fd);
text::Loaded read_text(int fd, const text::Limits& limits);
void colour(text::Text& value, std::string_view name, std::string_view mime);
void warm_up();
#ifdef RUMMAGE_ARCHIVES
archive::Listing list_archive(int fd, const archive::Limits& limits);
archive::Read read_archive_entry(int fd, archive::Format format, const archive::Entry& entry,
                                 int out, std::uint64_t max_bytes);
#else
inline archive::Listing list_archive(int, const archive::Limits&) {
    return {.format = archive::Format::unknown,
            .failure = archive::Failure::unreadable,
            .detail = "this engine does not read archives",
            .entries = {}};
}

inline archive::Read read_archive_entry(int, archive::Format, const archive::Entry&, int,
                                        std::uint64_t) {
    return {.failure = archive::Failure::unreadable,
            .detail = "this engine does not read archives",
            .bytes = 0};
}
#endif
}  // namespace rummage::isolate::backend
