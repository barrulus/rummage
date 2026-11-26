#pragma once

#include <array>
#include <cerrno>
#include <filesystem>
#include <string_view>
#include <sys/xattr.h>

#include <doctest/doctest.h>

namespace rummage::test {

// Probe an owned, writable fixture independently of the code under test. Some
// filesystems and build sandboxes return ENOTSUP for every user-xattr operation.
// Unexpected errors remain test failures, rather than hiding a broken fixture.
inline bool user_xattrs_available(const std::filesystem::path& path) {
    constexpr const char* key = "user.rummage.test-capability";
    constexpr std::string_view value = "probe";
    if (::setxattr(path.c_str(), key, value.data(), value.size(), XATTR_CREATE) != 0) {
        const int failed = errno;
        CAPTURE(path.string());
        REQUIRE(failed == ENOTSUP);
        REQUIRE(::getxattr(path.c_str(), key, nullptr, 0) == -1);
        REQUIRE(errno == ENOTSUP);
        MESSAGE("User xattrs are unsupported; checking the unsupported-storage behavior");
        return false;
    }
    std::array<char, value.size()> read{};
    REQUIRE(::getxattr(path.c_str(), key, read.data(), read.size()) ==
            static_cast<ssize_t>(value.size()));
    REQUIRE(std::string_view{read.data(), read.size()} == value);
    REQUIRE(::removexattr(path.c_str(), key) == 0);
    return true;
}

}  // namespace rummage::test
