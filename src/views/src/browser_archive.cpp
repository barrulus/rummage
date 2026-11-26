#include <algorithm>
#include <charconv>
#include <cstring>
#include <format>

#include "rummage/core/i18n.hpp"
#include "rummage/fs/advanced_archive.hpp"
#include "rummage/fs/archive_folder.hpp"
#include "rummage/fs/compress.hpp"
#include "rummage/fs/extract.hpp"
#include "rummage/fs/operations.hpp"
#include "rummage/views/browser.hpp"

namespace rummage::views {
void Browser::extract_dragged_members(const std::filesystem::path& destination,
                                      std::vector<std::filesystem::path> paths) {
    ++operations_;
    pool_.submit(
        core::Priority::visible,
        [destination, paths](const core::CancelToken& token) {
            if (fs::find_archive(destination))
                return false;
            return !paths.empty() && std::ranges::all_of(paths, [&](const auto& path) {
                return !token.cancelled() && fs::find_archive(path.parent_path()).has_value();
            });
        },
        [this, alive = alive_, destination, paths](bool valid) {
            if (!*alive)
                return;
            --operations_;
            if (!valid) {
                show_message(std::string{i18n::tr("Nothing could be read from the drop")}, true);
                return;
            }
            transfer_paths(destination, paths, false, std::nullopt);
        });
}

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

MenuItem Browser::advanced_archive_menu() {
    MenuItem menu;
    menu.id = "advanced_archive";
    menu.label = i18n::tr("Advanced archive options");
    menu.submenu = true;
    const auto add = [&](std::string id, std::string_view label,
                         std::function<void()> action) -> MenuItem& {
        MenuItem item;
        item.id = std::move(id);
        item.label = i18n::tr(label);
        item.action = std::move(action);
        menu.children.push_back(std::move(item));
        return menu.children.back();
    };
    add("create_7z", i18n::mark("Create 7z…"), [this] { advanced_archive(false); });
    add("test_archive", i18n::mark("Test archive integrity…"), [this] { advanced_archive(true); });
    for (const auto& [level, label] :
         std::vector<std::pair<int, std::string_view>>{{0, i18n::mark("Store without compression")},
                                                       {1, i18n::mark("Fast compression")},
                                                       {5, i18n::mark("Normal compression")},
                                                       {9, i18n::mark("Maximum compression")}}) {
        auto& item = add("archive_level_" + std::to_string(level), label,
                         [this, level] { archive_level_ = level; });
        item.radio = true;
        item.checked = archive_level_ == level;
    }
    auto& encrypt = add("archive_encrypt", i18n::mark("Password protection"),
                        [this] { archive_encrypt_ = !archive_encrypt_; });
    encrypt.checkable = true;
    encrypt.checked = archive_encrypt_;
    auto& names = add("archive_encrypt_names", i18n::mark("Encrypt filenames"),
                      [this] { archive_encrypt_names_ = !archive_encrypt_names_; });
    names.checkable = true;
    names.checked = archive_encrypt_names_;
    names.enabled = archive_encrypt_;
    auto& verify = add("archive_verify", i18n::mark("Verify after creation"),
                       [this] { archive_verify_ = !archive_verify_; });
    verify.checkable = true;
    verify.checked = archive_verify_;
    add("archive_volume", i18n::mark("Volume size…"), [this] {
        confirm_.open_name(
            std::string{i18n::tr("Volume size…")},
            std::string{i18n::tr("MiB per volume; 0 makes one archive")},
            std::to_string(archive_volume_ >> 20U),
            [this](const std::string& text) {
                std::uint64_t value = 0;
                const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
                if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
                    value > (1U << 20U))
                    return std::string{i18n::tr("Invalid archive options")};
                archive_volume_ = value << 20U;
                return std::string{};
            },
            std::string{i18n::tr("Apply")});
    });
    return menu;
}

void Browser::advanced_archive(bool test_only) {
    const auto folder = current_folder();
    if (!folder || folder->is_archive() || folder->is_trash() || picker_mode_)
        return;
    auto names = targets(*folder);
    if (names.empty() || (test_only && names.size() != 1)) {
        show_message(std::string{i18n::tr("Select one archive to test")}, true);
        return;
    }
    auto options = std::make_shared<fs::ArchiveOptions>();
    options->program = fs::seven_zip_program();
    if (options->program.empty()) {
        show_message(
            std::string{i18n::tr(
                "Install 7zz (7-Zip) or 7z and try again; these also support archive passwords.")},
            true);
        return;
    }
    options->level = archive_level_;
    options->volume_bytes = archive_volume_;
    options->encrypt_names = !test_only && archive_encrypt_ && archive_encrypt_names_;
    options->verify = archive_verify_;
    options->test_only = test_only;
    const auto launch = [this, folder, names, options](const std::string& name) {
        submit({.kind = fs::TransferKind::compress,
                .source = folder->path(),
                .names = names,
                .destination = folder->path(),
                .archive_name = name,
                .archive_options = options},
               open_batch(Purpose::user, FileOperation::Kind::copy));
    };
    const auto named = [this, options, launch](std::string name) {
        if (!archive_encrypt_) {
            launch(name);
            return;
        }
        password_.open_password(
            name, false,
            [this, options, launch, name](std::optional<std::string> secret) {
                if (!secret)
                    return;
                if (secret->empty() || secret->size() > 4096 ||
                    secret->find_first_of(std::string_view{"\r\n\0", 3}) != std::string::npos) {
                    if (!secret->empty())
                        explicit_bzero(secret->data(), secret->size());
                    show_message(std::string{i18n::tr("Invalid archive options")}, true);
                    return;
                }
                options->password = std::make_shared<fs::ArchivePassword>(*secret);
                explicit_bzero(secret->data(), secret->size());
                launch(name);
            },
            std::string{options->test_only ? i18n::tr("Test archive integrity…")
                                           : i18n::tr("Compress")});
    };
    if (test_only) {
        named(names.front());
        return;
    }
    auto proposed = fs::archive_name_for(names, false, false);
    proposed.replace(proposed.size() - 4, 4, ".7z");
    confirm_.open_name(std::string{i18n::tr("Create 7z…")},
                       std::string{i18n::tr("Archive name · existing files are kept")}, proposed,
                       [this, named](const std::string& name) {
                           if (const auto error = fs::name_problem(name))
                               return *error;
                           // ConfirmDialog finishes its own state before the password dialog is
                           // opened.
                           loop_.post([alive = alive_, named, name] {
                               if (*alive)
                                   named(name);
                           });
                           return std::string{};
                       });
}

void Browser::compress_done(const fs::TransferRequest& request, const fs::TransferResult& result) {
    if (!result.refused.empty()) {
        show_message(result.refused, true);
        return;
    }
    if (request.archive_options && request.archive_options->test_only && !result.cancelled &&
        result.errors.empty()) {
        show_message(std::string{i18n::tr("Archive integrity test passed")}, false);
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
