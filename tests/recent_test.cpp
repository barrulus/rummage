#include "rummage/fs/recent.hpp"

#include <atomic>
#include <sys/stat.h>
#include <thread>

#include <doctest/doctest.h>

#include "rummage/core/file.hpp"
#include "rummage/platform/gvfs.hpp"
#include "rummage/platform/network.hpp"
#include "temp_dir.hpp"
using rummage::fs::RecentFiles;
using rummage::test::TempDir;

TEST_CASE("recent files: bounded deduplicated private history clears without deleting files") {
    TempDir dir;
    RecentFiles history{dir.path / "history"};
    const auto first = dir.write("first\nwith spaces.txt", "one");
    const auto second = dir.write("second.txt", "two");
    REQUIRE(history.record({first, second, dir.path}).empty());
    auto files = history.load();
    REQUIRE(files.size() == 2);
    CHECK(files[0].path == second);
    CHECK(files[1].path == first);
    CHECK(files[0].opened > 0);
    REQUIRE(history.record({first}).empty());
    CHECK(history.load()[0].path == first);
    struct stat info{};
    REQUIRE(::stat((dir.path / "history/recent-files").c_str(), &info) == 0);
    CHECK((info.st_mode & 0777) == 0600);
    REQUIRE(history.set_enabled(false).empty());
    CHECK_FALSE(history.enabled());
    REQUIRE(history.record({second}).empty());
    CHECK(history.load().empty());
    REQUIRE(history.set_enabled(true).empty());
    CHECK(history.load().empty());
    REQUIRE(history.record({first}).empty());
    REQUIRE(history.clear().empty());
    CHECK(history.load().empty());
    CHECK(std::filesystem::exists(first));
    CHECK(std::filesystem::exists(second));
    std::vector<std::filesystem::path> many;
    for (int n = 0; n < 210; ++n)
        many.push_back(dir.write(std::to_string(n), "x"));
    REQUIRE(history.record(many).empty());
    CHECK(history.load().size() == 200);
}

TEST_CASE("recent files: stale inaccessible malformed and remote entries are omitted") {
    TempDir dir;
    RecentFiles history{dir.path / "history"};
    const auto first = dir.write("first", "one");
    REQUIRE(history.record({first}).empty());
    std::filesystem::remove(first);
    CHECK(history.load().empty());
    REQUIRE(rummage::core::write_file_atomically(
                dir.path / "history/recent-files",
                "bogus\tfile:///tmp/x\n23\tsftp://user:password@host/file\n12\tfile:///tmp/%00\n")
                .value() == 0);
    CHECK(history.load().empty());
}

TEST_CASE("network discovery: Avahi escaped service names ports paths and duplicates") {
    const auto places = rummage::platform::parse_network_services(
        "=;eth0;IPv4;Office\\059Files;_sftp-ssh._tcp;local;files.local;192.0.2.1;2222;\n"
        "=;eth0;IPv6;Office\\059Files;_sftp-ssh._tcp;local;files.local;::1;2222;\n"
        "=;eth0;IPv4;Export;_nfs._tcp;local;nfs.local;192.0.2.2;2049;\"path=/data/project\"\n"
        "=;eth0;IPv4;Printer;_ipp._tcp;local;printer.local;192.0.2.3;631;\n"
        "=;eth0;IPv4;Bad;_ftp._tcp;local;user@host;192.0.2.4;21;\n"
        "=;eth0;IPv4;Bad;_ftp._tcp;local;host;192.0.2.4;99999;\n");
    REQUIRE(places.size() == 2);
    CHECK(places[0].label.starts_with("Office;Files"));
    CHECK(places[0].uri == "sftp://files.local:2222/");
    CHECK(places[1].uri == "nfs://nfs.local/data/project");
}

TEST_CASE("remote backends: NFS and attached devices map without retaining credentials") {
    for (const auto uri :
         {"nfs://files.local/export/folder", "mtp://[usb:001,002]/", "gphoto2://[usb:001,003]/"}) {
        const auto parsed = rummage::fs::parse_remote_uri(uri);
        REQUIRE(parsed);
        const auto location = rummage::platform::gvfs_location(*parsed);
        REQUIRE(location);
        CHECK(location->spec.type == parsed->scheme);
        CHECK(location->path == parsed->path);
    }
    CHECK_FALSE(rummage::fs::parse_remote_uri("sftp://user:secret@server/a"));
    CHECK_FALSE(rummage::fs::parse_remote_uri("sftp://user%3asecret@server/a"));
    const auto nfs = rummage::fs::parse_remote_uri("nfs://[2001:db8::1]/export/a");
    REQUIRE(nfs);
    const auto location = rummage::platform::gvfs_location(*nfs);
    REQUIRE(location);
    CHECK(location->spec.items.at("host") == "[2001:db8::1]");
    rummage::platform::GvfsMount mount;
    mount.fuse = "/run/user/1000/gvfs/nfs";
    mount.spec = location->spec;
    mount.spec.prefix = "/export";
    CHECK(rummage::platform::local_path(mount, *location) == "/run/user/1000/gvfs/nfs/a");
    mount.fuse.clear();
    CHECK(rummage::platform::local_path(mount, *location).empty());
}

TEST_CASE("recent files: disabling serializes with queued writers and stale instances") {
    TempDir dir;
    const auto file = dir.write("opened.txt", "fixture");
    const auto location = dir.path / "history";
    RecentFiles writer{location}, settings{location};
    REQUIRE(writer.enabled());
    std::atomic<bool> start{false};
    std::vector<std::thread> threads;
    for (int n = 0; n < 4; ++n)
        threads.emplace_back([&] {
            while (!start.load())
                std::this_thread::yield();
            for (int i = 0; i < 8; ++i)
                static_cast<void>(writer.record({file}));
        });
    start = true;
    const auto error = settings.set_enabled(false);
    for (auto& thread : threads)
        thread.join();
    REQUIRE(error.empty());
    CHECK_FALSE(writer.enabled());
    CHECK(rummage::core::read_text_file(location / "recent-files").text.empty());
    REQUIRE(writer.record({file}).empty());
    CHECK(rummage::core::read_text_file(location / "recent-files").text.empty());
    REQUIRE(settings.set_enabled(true).empty());
    CHECK(writer.load().empty());
    REQUIRE(writer.record({file}).empty());
    REQUIRE(writer.load().size() == 1);
    CHECK(writer.load()[0].path == file);
}

TEST_CASE("recent files: unexpected FIFO state is rejected without waiting for a writer") {
    TempDir dir;
    const auto location = dir.path / "history";
    std::filesystem::create_directory(location);
    REQUIRE(::mkfifo((location / "recent-files").c_str(), 0600) == 0);
    CHECK(RecentFiles{location}.load().empty());
}
