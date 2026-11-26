#include <algorithm>
#include <format>

#include "rummage/core/i18n.hpp"
#include "rummage/fs/compress.hpp"
#include "rummage/fs/extract.hpp"
#include "rummage/fs/operations.hpp"
#include "rummage/views/browser.hpp"

namespace rummage::views {
void Browser::open_archive_members(const std::filesystem::path& source,
                                   std::vector<fs::Entry> files) {
    std::vector<std::string> names;
    for (const auto& file : files)
        names.push_back(file.name);
    ++operations_;
    pool_.submit(
        core::Priority::visible,
        [source, names = std::move(names)](const core::CancelToken& token) {
            return fs::extract_to_open(source, names, token);
        },
        [this, alive = alive_, files = std::move(files)](fs::ExtractedToOpen made) mutable {
            if (!*alive)
                return;
            --operations_;
            if (!made.result.refused.empty()) {
                show_message(made.result.refused, true);
                return;
            }
            std::erase_if(files, [&made](const auto& file) {
                return std::ranges::find(made.result.placed, file.name) == made.result.placed.end();
            });
            if (!made.result.errors.empty())
                show_message(made.result.errors.front(), true);
            if (!files.empty() && open_)
                open_(made.folder, std::move(files));
            redraw_();
        });
}

void Browser::compress(bool tar_gz) {
    const auto folder = current_folder();
    if (!folder || picker_mode_ || folder->is_trash() ||
        (folder->is_archive() || folder->is_collection()))
        return;
    auto names = targets(*folder);
    if (names.empty())
        return;
    bool first_folder = false;
    for (const auto& entry : folder->entries())
        if (entry.name == names.front())
            first_folder = entry.is_directory();
    const auto proposed = fs::archive_name_for(names, first_folder, tar_gz);
    confirm_.open_name(
        std::string{i18n::tr(tar_gz ? "Compress to tar.gz" : "Compress to ZIP")},
        std::string{i18n::tr("Archive name · existing files are kept")}, proposed,
        [this, folder, names = std::move(names), tar_gz](const std::string& name) {
            if (const auto error = fs::name_problem(name))
                return *error;
            submit({.kind = fs::TransferKind::compress,
                    .source = folder->path(),
                    .names = names,
                    .destination = folder->path(),
                    .force_copy = false,
                    .archive_name = name,
                    .tar_gz = tar_gz},
                   open_batch(Purpose::user, FileOperation::Kind::copy));
            status_bar_.set_transfer(TransferStatus{
                .text = std::string{i18n::tr("Getting ready to compress…")}, .fraction = -1.0F});
            redraw_();
            return std::string{};
        });
}

void Browser::compress_done(const fs::TransferRequest& request, const fs::TransferResult& result) {
    if (!result.refused.empty()) {
        show_message(result.refused, true);
        return;
    }
    if (result.cancelled) {
        show_message(std::string{i18n::tr("Stopped: no archive made")}, false);
        return;
    }
    if (result.placed.empty()) {
        if (!result.errors.empty())
            show_message(result.errors.front(), true);
        return;
    }
    const auto& name = result.placed.front();
    show_message(result.errors.empty() ? i18n::trf("Created “{}”", name)
                                       : i18n::trf("Created “{}”; {}", name, result.errors.front()),
                 !result.errors.empty());
    if (const auto folder = current_folder(); folder && folder->path() == request.destination)
        folder->expect_name(name);
}
}  // namespace rummage::views
