#include "rummage/core/i18n.hpp"
#include "rummage/views/browser.hpp"

namespace rummage::views {
void Browser::links_done(const fs::TransferRequest& request, const fs::TransferResult& result) {
    const std::size_t made = result.placed.size();
    const std::string what = i18n::trn("{} link", "{} links", made);
    if (!result.refused.empty()) {
        show_message(result.refused, true);
    } else if (result.cancelled) {
        show_message(i18n::trf("Stopped: {} made so far", what), false);
    } else if (!result.errors.empty()) {
        show_message(i18n::trf("Made {}; {} could not be: {}", what, result.errors.size(),
                               result.errors.front()),
                     true);
    } else if (result.placed.empty()) {
        show_message(std::string{i18n::tr("Nothing to do: every name was skipped")}, false);
    } else {
        show_message(i18n::trf("Made {}", what), false);
    }
    // Selected, ready to rename or drag away, when their folder is the one shown.
    if (const std::shared_ptr<Folder> folder = current_folder();
        folder && folder->path() == request.destination && !result.placed.empty()) {
        folder->expect_names(result.placed);
    }
}

void Browser::create_links() {
    const std::shared_ptr<Folder> folder = current_folder();
    if (!folder || picker_mode_ || folder->is_trash() || folder->is_archive()) {
        return;
    }
    std::vector<std::string> names = targets(*folder);
    if (names.empty()) {
        return;
    }
    const std::size_t count = names.size();
    // Recorded as a copy, as Compress is: Ctrl+Z moves the new links to the trash, never what
    // they lead to.
    submit({.kind = fs::TransferKind::link,
            .source = folder->path(),
            .names = std::move(names),
            .destination = folder->path()},
           open_batch(Purpose::user, FileOperation::Kind::copy));
    if (!status_bar_.transfer()) {
        status_bar_.set_transfer(
            TransferStatus{.text = i18n::trf("Making {}…", i18n::trn("{} link", "{} links", count)),
                           .fraction = -1.0F});
    }
    redraw_();
}

}  // namespace rummage::views
