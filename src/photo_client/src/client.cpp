#include "rummage/photos/client.hpp"

#include <algorithm>
#include <cstdlib>
#include <dlfcn.h>
#include <exception>
#include <unistd.h>
#include <utility>

#include "rummage/core/worker_pool.hpp"
#include "rummage/engines/engines.hpp"
#include "rummage/fs/entry.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/fs/trash.hpp"
#include "rummage/picture/load.hpp"
#include "rummage/thumbs/cache.hpp"

namespace rummage::photos {
namespace {

std::filesystem::path absolute_environment(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0')
        return {};
    const std::filesystem::path path{value};
    return path.is_absolute() ? path : std::filesystem::path{};
}

bool regular(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

}  // namespace

struct Client::Module {
    using Version = int (*)();
    using Request = char* (*)(const char*, const char*);
    using Free = void (*)(char*);
    void* handle{};
    Request request{};
    Free release{};
    std::string error;

    explicit Module(const std::filesystem::path& path) {
        if (path.empty()) {
            error = "Photo Library is not installed. Install the rummage-photos add-on.";
            return;
        }
        if (!path.is_absolute()) {
            error = "The Photo Library component path must be absolute.";
            return;
        }
        handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr) {
            const char* detail = dlerror();
            error = "Cannot load Photo Library: " + std::string{detail ? detail : "unknown error"};
            return;
        }
        const auto version = reinterpret_cast<Version>(dlsym(handle, "rummage_photos_version"));
        request = reinterpret_cast<Request>(dlsym(handle, "rummage_photos_request"));
        release = reinterpret_cast<Free>(dlsym(handle, "rummage_photos_free"));
        if (version == nullptr || request == nullptr || release == nullptr || version() != 1)
            error =
                "Photo Library is incompatible. Install the add-on matching this Rummage version.";
    }

    ~Module() {
        if (handle != nullptr)
            dlclose(handle);
    }

    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;
};

std::filesystem::path Client::installed_path() {
    // An explicit override is authoritative, including a missing/invalid path. Do not silently
    // load a different version when a user's configured component has disappeared.
    if (const char* value = std::getenv("RUMMAGE_PHOTOS_LIBRARY"); value && *value)
        return absolute_environment("RUMMAGE_PHOTOS_LIBRARY");
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error)
        return {};
    const auto directory = executable.parent_path();
    const auto build = directory / "librummage-photos.so";
    if (regular(build))
        return build;
    const auto installed = directory.parent_path() / "libexec/rummage/librummage-photos.so";
    return regular(installed) ? installed : std::filesystem::path{};
}

std::filesystem::path Client::default_catalog_path() {
    if (const auto data = absolute_environment("XDG_DATA_HOME"); !data.empty())
        return data / "rummage/photos/catalog.sqlite3";
    if (const auto home = absolute_environment("HOME"); !home.empty())
        return home / ".local/share/rummage/photos/catalog.sqlite3";
    return {};
}

Client::Client() : Client(installed_path(), default_catalog_path()) {}

Client::Client(std::filesystem::path module, std::filesystem::path catalog)
    : module_{std::make_shared<Module>(module)}, catalog_{std::move(catalog)} {}

bool Client::available() const { return module_->error.empty(); }

std::string Client::error() const { return module_->error; }

nlohmann::json Client::request(const nlohmann::json& request,
                               const core::CancelToken* cancel) const {
    if (cancel && cancel->cancelled())
        return {{"ok", false}, {"error", "Photo Library request cancelled"}};
    if (!available())
        return {{"ok", false}, {"error", error()}};
    if (catalog_.empty() || !catalog_.is_absolute())
        return {{"ok", false},
                {"error", "Set an absolute XDG_DATA_HOME or HOME for Photo Library."}};
    try {
        if (request.value("op", "") == "recover_operations") {
            const auto journal = this->request({{"op", "operations"}}, cancel);
            if (!journal.value("ok", false))
                return journal;
            std::optional<std::vector<fs::TrashedItem>> trash;
            const auto matches = [](const nlohmann::json& photo,
                                    const std::filesystem::path& path) {
                const auto actual = fs::capture_source_identity(path);
                return actual && actual->device == photo.at("device").get<std::uint64_t>() &&
                       actual->inode == photo.at("inode").get<std::uint64_t>() &&
                       actual->size == photo.at("size").get<std::uint64_t>() &&
                       actual->mtime_ns == photo.at("mtime_ns").get<std::int64_t>();
            };
            for (const auto& operation : journal.at("operations")) {
                if (cancel && cancel->cancelled())
                    break;
                const auto state = operation.value("state", "");
                if (state != "pending" && state != "needs_review")
                    continue;
                const auto kind = operation.value("kind", "");
                if (kind != "trash" && kind != "restore")
                    continue;
                nlohmann::json recovered = nlohmann::json::array();
                for (const auto& photo : operation.at("photos")) {
                    const std::filesystem::path original{photo.at("path").get<std::string>()};
                    std::error_code error;
                    // Missing alone proves nothing. Only an unambiguous existing receipt and
                    // the captured inode/size/time can establish that this operation happened.
                    if (std::filesystem::exists(std::filesystem::symlink_status(original, error)) ||
                        (error && error != std::errc::no_such_file_or_directory))
                        continue;
                    std::filesystem::path destination;
                    if (operation.at("kind") == "trash") {
                        if (!trash)
                            trash = fs::list_trash(fs::TrashSources::from_environment());
                        bool ambiguous = false;
                        for (const auto& candidate : *trash) {
                            if (candidate.info.original == original &&
                                matches(photo, candidate.path)) {
                                if (!destination.empty())
                                    ambiguous = true;
                                destination = candidate.path;
                            }
                        }
                        if (ambiguous)
                            destination.clear();
                    } else if (operation.at("kind") == "restore" &&
                               photo.contains("original_path")) {
                        const std::filesystem::path target{
                            photo.at("original_path").get<std::string>()};
                        if (matches(photo, target))
                            destination = target;
                    }
                    if (!destination.empty())
                        recovered.push_back({{"id", photo.at("id")},
                                             {"success", true},
                                             {"destination", destination.string()}});
                }
                if (!recovered.empty()) {
                    const auto saved = this->request({{"op", "operation_complete"},
                                                      {"operation_id", operation.at("id")},
                                                      {"results", recovered}});
                    if (!saved.value("ok", false))
                        return saved;
                }
            }
            return this->request({{"op", "reconcile_operations"}});
        }
        auto envelope = request;
        const auto operation = request.value("op", "");
        if (operation == "add_source" || operation == "work" || operation == "watch_paths")
            envelope["internal_exclusions"] = {catalog_.parent_path().string(),
                                               thumbs::default_root().string()};
        const auto encoded = envelope.dump();
        std::unique_ptr<char, Module::Free> response{
            module_->request(catalog_.c_str(), encoded.c_str()), module_->release};
        if (!response)
            return {{"ok", false}, {"error", "Photo Library could not allocate its response."}};
        auto result = nlohmann::json::parse(response.get());
        if (!result.is_object() || !result.contains("ok") || !result["ok"].is_boolean())
            return {{"ok", false}, {"error", "Photo Library returned an invalid response."}};
        // The optional catalog never links a decoder. Reuse the application's isolated image
        // loader and shared thumbnail cache, after the bounded catalog transaction has ended.
        // Limit each prefetch task so visible thumbnails can take priority between batches.
        if (result.value("ok", false) && request.value("op", "") == "work" &&
            !engines::installed()->engines().empty()) {
            result["metadata_processed"] = 0;
            const auto metadata_limit = std::clamp(result.value("metadata_limit", 4), 0, 4);
            if (result.value("paused", false) || metadata_limit == 0)
                return result;
            const auto pending =
                this->request({{"op", "metadata_pending"}, {"limit", metadata_limit}});
            if (!pending.value("ok", false))
                return pending;
            static const auto mime = fs::MimeDatabase::load();
            const thumbs::Cache cache{thumbs::default_root(), "rummage-photos"};
            for (const auto& photo : pending.value("metadata", nlohmann::json::array())) {
                if (cancel && cancel->cancelled())
                    break;
                const auto claim = this->request({{"op", "metadata_claim"},
                                                  {"id", photo.at("id")},
                                                  {"signature", photo.at("signature")},
                                                  {"owner", std::to_string(::getpid())}});
                if (!claim.value("ok", false))
                    return claim;
                if (!claim.value("allowed", false))
                    continue;
                const std::filesystem::path path{photo.at("path").get<std::string>()};
                const fs::Entry entry{.name = path.filename().string(),
                                      .kind = fs::EntryKind::file,
                                      .size = photo.value("size", std::uint64_t{0})};
                const auto type = mime->detect(path.parent_path(), entry);
                const auto type_name = mime->name(type);
                nlohmann::json update{{"op", "metadata"},
                                      {"id", photo.at("id")},
                                      {"signature", photo.at("signature")},
                                      {"claim_token", claim.at("claim_token")}};
                const auto* family = engines::installed()->find(*mime, type);
                if (!family || family->shows != engines::Shows::picture ||
                    !type_name.starts_with("image/")) {
                    update["error"] = "No installed still-image engine supports this file.";
                } else {
                    const auto info = picture::load_photo_info(path, type_name);
                    const auto thumbnail = cache.get(path, thumbs::Size::large, type_name);
                    update["width"] = info.width ? info.width : thumbnail.full_width;
                    update["height"] = info.height ? info.height : thumbnail.full_height;
                    update["camera"] = info.camera;
                    // Shared EXIF display dates preserve wall time, without a known offset.
                    // Never manufacture UTC or use modification time as a capture date.
                    auto taken = info.taken;
                    if (taken.size() >= 16 && taken[4] == '-' && taken[7] == '-' &&
                        taken[10] == ' ') {
                        taken[10] = 'T';
                        update["capture_date"] = taken;
                    }
                    if (thumbnail.image.empty())
                        update["error"] = thumbnail.error;
                }
                const auto saved = this->request(update);
                if (!saved.value("ok", false))
                    return saved;
                result["metadata_processed"] = result["metadata_processed"].get<int>() + 1;
                result["worked"] = true;
            }
        }
        return result;
    } catch (const std::exception& failure) {
        return {{"ok", false}, {"error", std::string{"Photo Library: "} + failure.what()}};
    }
}

}  // namespace rummage::photos
