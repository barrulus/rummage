#include "rummage/core/i18n.hpp"
#include "rummage/fs/filesystem.hpp"
#include "rummage/views/browser.hpp"
#include "rummage/views/format.hpp"

namespace rummage::views {
void Browser::draw_drive_tooltip(ui::Renderer& renderer, const theme::Palette& palette,
                                 const ui::Rect& bounds) {
    const auto* hovered = sidebar_.hovered_place();
    const auto path = hovered && hovered->section == fs::Place::Section::devices &&
                              hovered->disk.mounted && !has_overlay() && !settings_.is_open() &&
                              !batch_rename_.is_open()
                          ? hovered->path
                          : std::filesystem::path{};
    if (path != drive_hover_path_) {
        drive_hover_job_.cancel();
        drive_hover_path_ = path;
        drive_hover_text_.clear();
        if (!path.empty())
            drive_hover_job_ = pool_.submit(
                core::Priority::nearby,
                [path](const core::CancelToken&) { return fs::filesystem_facts(path); },
                [this, alive = alive_, path](const fs::FilesystemFacts& facts) {
                    if (!*alive || path != drive_hover_path_)
                        return;
                    drive_hover_text_ =
                        facts.error ? std::string{i18n::tr("Space information unavailable")}
                                    : i18n::trf("{} available of {}", format_size(facts.available),
                                                format_size(facts.capacity));
                    redraw_();
                });
    }
    if (!path.empty())
        sidebar_.draw_drive_tooltip(renderer, palette, bounds, drive_hover_text_);
}
}  // namespace rummage::views
