#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "rummage/fs/transfer.hpp"

namespace rummage::fs {
// A job consumes this secret once. Not serialized, placed in argv/environment, or reported.
struct ArchivePassword {
    explicit ArchivePassword(std::string_view value);
    ~ArchivePassword();
    ArchivePassword(const ArchivePassword&) = delete;
    ArchivePassword& operator=(const ArchivePassword&) = delete;
    std::string value;
};

struct ArchiveOptions {
    std::filesystem::path program;
    int level{5};  // 0 store, 1 fast, 5 normal, 9 maximum
    std::uint64_t volume_bytes{0};
    bool encrypt_names{false};
    bool verify{true};
    bool test_only{false};
    std::shared_ptr<ArchivePassword> password{};
};

[[nodiscard]] std::filesystem::path seven_zip_program();
[[nodiscard]] TransferResult run_advanced_archive(const TransferRequest& request,
                                                  const core::CancelToken& token,
                                                  const TransferCallbacks& callbacks);
}  // namespace rummage::fs
