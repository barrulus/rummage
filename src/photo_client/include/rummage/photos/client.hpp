#pragma once

#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

namespace rummage::core {
class CancelToken;
}

namespace rummage::photos {

// The optional catalog has a versioned C boundary. Merely discovering or loading it never
// opens a catalog, creates directories, starts workers or scans a source. Call request() on
// a worker: filesystem and catalog operations may block. Copies keep the module alive until
// their outstanding work finishes. The base application has no SQLite dependency.
class Client {
public:
    Client();
    explicit Client(std::filesystem::path module, std::filesystem::path catalog);

    [[nodiscard]] static std::filesystem::path installed_path();
    [[nodiscard]] static std::filesystem::path default_catalog_path();
    [[nodiscard]] bool available() const;
    [[nodiscard]] std::string error() const;
    [[nodiscard]] nlohmann::json request(const nlohmann::json& request,
                                         const core::CancelToken* cancel = nullptr) const;

private:
    struct Module;
    std::shared_ptr<Module> module_;
    std::filesystem::path catalog_;
};

}  // namespace rummage::photos
