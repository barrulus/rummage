#include <string>
#include <string_view>
#include <utility>

#include "rummage/picture/load.hpp"
#include "rummage/preview/preview.hpp"

namespace rummage::preview {

Summary summarize(const std::filesystem::path& path, std::string_view mime, Kind kind,
                  const core::CancelToken& cancel,
                  const std::shared_ptr<const fs::MimeDatabase>& types, bool thumbnail) {
    Summary summary;
    summary.info = read_info(path, cancel);
    if (!cancel.cancelled() && (kind == Kind::image || mime.starts_with("video/"))) {
        summary.photo = picture::load_photo_info(path, mime);
    } else if (kind == Kind::text) {
        const text::Limits limits =
            thumbnail ? text::Limits{.max_bytes = 16 * 1024, .max_line_bytes = 256, .max_lines = 12}
                      : text::Limits{.tail = mime == "text/x-log"};
        text::Loaded loaded =
            text::load_text({.path = path, .mime = std::string{mime}, .limits = limits});
        if (loaded.failure == text::Failure::none) {
            summary.text = std::move(loaded.text);
        }
    } else if (kind == Kind::archive) {
        summary.archive = read_archive(path, types, 0);
    }
    return summary;
}

}  // namespace rummage::preview
