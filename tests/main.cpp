#define DOCTEST_CONFIG_IMPLEMENT
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "rummage/archive/archive.hpp"
#include "rummage/core/i18n.hpp"
#include "rummage/engines/engines.hpp"
#include "rummage/formats/archive/archive.hpp"
#include "rummage/formats/image/load.hpp"
#include "rummage/formats/image/photo.hpp"
#include "rummage/formats/text/read.hpp"
#include "rummage/fs/trash.hpp"
#include "rummage/picture/load.hpp"
#include "rummage/text/text.hpp"

int main(int argc, char** argv) {
    // The tests read the English interface, whatever language the machine speaks.
    rummage::i18n::set_language(rummage::i18n::kEnglish);
    // No test reads or makes the user's trash: the trash folder lists one of its own, and no
    // disk's.
    std::string root = (std::filesystem::temp_directory_path() / "rummage-tests-XXXXXX").string();
    if (mkdtemp(root.data()) == nullptr) {
        return 1;
    }
    rummage::fs::set_trash_sources({.home = std::filesystem::path{root} / "Trash",
                                    .tops = std::vector<std::filesystem::path>{},
                                    .uid = geteuid()});
    // Nor the user's configuration, data, cache or state: a key that writes there (Ctrl+D adds a
    // bookmark to gtk-3.0/bookmarks) writes into the test root. Set before any thread starts.
    for (const auto& [name, folder] :
         {std::pair{"XDG_CONFIG_HOME", "config"}, std::pair{"XDG_DATA_HOME", "data"},
          std::pair{"XDG_CACHE_HOME", "cache"}, std::pair{"XDG_STATE_HOME", "state"}}) {
        // NOLINTNEXTLINE(concurrency-mt-unsafe): no other thread yet
        setenv(name, (std::filesystem::path{root} / folder).c_str(), 1);
    }
    // The MIME database found at build time (tests/CMakeLists.txt), the same on every system,
    // whatever $XDG_DATA_DIRS the tests run with.
    // NOLINTNEXTLINE(concurrency-mt-unsafe): no other thread yet
    setenv("XDG_DATA_DIRS", RUMMAGE_MIME_DATA, 1);
    // The engines rummage ships, from their manifests in the source tree (their programs are not
    // started here).
    rummage::engines::set_installed(std::make_shared<const rummage::engines::Registry>(
        rummage::engines::Registry::load(RUMMAGE_ENGINES, [](std::string_view program) {
            return program == "rummage-preview" ? std::filesystem::path{program}
                                                : std::filesystem::path{};
        })));
    // Pictures made in this process, by the image family itself (rummage sends them to its
    // isolated helpers instead; tests/helpers_test.cpp covers that path).
    rummage::picture::set_loader([](const rummage::picture::LoadRequest& request) {
        return rummage::formats::image::load(request);
    });
    rummage::picture::set_info_loader([](const std::filesystem::path& path, std::string_view) {
        return rummage::formats::image::read_photo_info(path);
    });
    rummage::text::set_loader([](const rummage::text::LoadRequest& request) {
        return rummage::formats::text::load(request);
    });
    rummage::archive::set_lister(
        [](const std::filesystem::path& path, const rummage::archive::Limits& limits) {
            const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) {
                return rummage::archive::Listing{.format = rummage::archive::Format::unknown,
                                                 .failure = rummage::archive::Failure::unreadable,
                                                 .detail = "it could not be opened",
                                                 .entries = {}};
            }
            rummage::archive::Listing listing = rummage::formats::archive::list(fd, limits);
            ::close(fd);
            return listing;
        });
    rummage::archive::set_entry_reader(
        [](const std::filesystem::path& path, rummage::archive::Format format,
           const rummage::archive::Entry& entry, int out, std::uint64_t max_bytes) {
            const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) {
                return rummage::archive::Read{.failure = rummage::archive::Failure::unreadable,
                                              .detail = "it could not be opened",
                                              .bytes = 0};
            }
            rummage::archive::Read read =
                rummage::formats::archive::read_entry(fd, format, entry, out, max_bytes);
            ::close(fd);
            return read;
        });
    doctest::Context context;
    context.applyCommandLine(argc, argv);
    const int result = context.run();
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return result;
}
