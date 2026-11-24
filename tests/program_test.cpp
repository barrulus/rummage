#include "rummage/platform/program.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/entry.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/platform/opener.hpp"
#include "temp_dir.hpp"

using rummage::core::EventLoop;
using rummage::core::WorkerPool;
using rummage::fs::EntryKind;
using rummage::fs::MimeDatabase;
using rummage::platform::Environment;
using rummage::platform::Opener;
using rummage::test::TempDir;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

using Bytes = std::vector<unsigned char>;

void put(Bytes& bytes, std::size_t at, std::uint64_t value, std::size_t size, bool little) {
    if (bytes.size() < at + size) {
        bytes.resize(at + size);
    }
    for (std::size_t i = 0; i < size; ++i) {
        const std::size_t shift = 8 * (little ? i : size - 1 - i);
        bytes[at + i] = static_cast<unsigned char>((value >> shift) & 0xFFU);
    }
}

// An ELF header of type, with count program headers at 64 (wide) or 52, the one at interp (if
// any) of type PT_INTERP.
Bytes elf(bool wide, bool little, std::uint16_t type, std::uint16_t count = 0, int interp = -1) {
    Bytes bytes(wide ? 64 : 52, 0);
    bytes[0] = 0x7F;
    bytes[1] = 'E';
    bytes[2] = 'L';
    bytes[3] = 'F';
    bytes[4] = wide ? 2 : 1;
    bytes[5] = little ? 1 : 2;
    bytes[6] = 1;
    put(bytes, 16, type, 2, little);
    const std::size_t offset = wide ? 64 : 52;
    const std::size_t size = wide ? 56 : 32;
    if (wide) {
        put(bytes, 32, count > 0 ? offset : 0, 8, little);
        put(bytes, 54, size, 2, little);
        put(bytes, 56, count, 2, little);
    } else {
        put(bytes, 28, count > 0 ? offset : 0, 4, little);
        put(bytes, 42, size, 2, little);
        put(bytes, 44, count, 2, little);
    }
    for (std::uint16_t i = 0; i < count; ++i) {
        put(bytes, offset + (i * size), std::cmp_equal(i, interp) ? 3 : 1, 4,
            little);  // PT_INTERP, PT_LOAD
    }
    return bytes;
}

bool is_program(const Bytes& bytes) {
    return rummage::platform::is_elf_program([&bytes](std::uint64_t offset,
                                                      std::span<unsigned char> out) {
        if (offset > bytes.size() || bytes.size() - offset < out.size()) {
            return false;
        }
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), out.size(), out.begin());
        return true;
    });
}

bool run_until(EventLoop& loop, const std::function<bool()>& done) {
    bool ok = false;
    const auto poll = loop.add_timer(1ms, 1ms, [&] {
        if (done()) {
            ok = true;
            loop.quit(0);
        }
    });
    const auto watchdog = loop.add_timer(5s, 0ms, [&loop] { loop.quit(1); });
    loop.run();
    loop.remove(poll);
    loop.remove(watchdog);
    return ok;
}

fs::path harmless_program() { return RUMMAGE_TEST_PROGRAM; }

}  // namespace

TEST_CASE("is_elf_program: executables and PIEs run; libraries, objects and the rest do not") {
    CHECK(is_program(elf(true, true, 2)));           // ET_EXEC, 64-bit
    CHECK(is_program(elf(false, false, 2)));         // ET_EXEC, 32-bit big-endian
    CHECK(is_program(elf(true, true, 3, 4, 1)));     // a PIE: ET_DYN with PT_INTERP
    CHECK(is_program(elf(false, true, 3, 3, 2)));    // the same, 32-bit
    CHECK_FALSE(is_program(elf(true, true, 3, 4)));  // a shared library: no PT_INTERP
    CHECK_FALSE(is_program(elf(true, true, 3)));     // no program headers at all
    CHECK_FALSE(is_program(elf(true, true, 1)));     // ET_REL, an object file
    CHECK_FALSE(is_program(elf(true, true, 4)));     // ET_CORE
    CHECK_FALSE(is_program(Bytes{'#', '!', '/', 'b', 'i', 'n', '/', 's', 'h', '\n'}));

    // An AppImage (issue #23): its runtime is a static PIE, ET_DYN with no PT_INTERP, and the
    // AppImage magic "AI" and its type at byte 8.
    const auto appimage = [](Bytes bytes, unsigned char kind) {
        bytes[8] = 'A';
        bytes[9] = 'I';
        bytes[10] = kind;
        return bytes;
    };
    CHECK(is_program(appimage(elf(true, true, 3, 4), 2)));        // type 2
    CHECK(is_program(appimage(elf(true, true, 3, 4), 1)));        // the old type 1
    CHECK(is_program(appimage(elf(false, true, 3, 3), 2)));       // 32-bit
    CHECK_FALSE(is_program(appimage(elf(true, true, 3, 4), 3)));  // no such type
    CHECK_FALSE(is_program(appimage(elf(true, true, 1), 2)));     // the magic on an object file
    CHECK_FALSE(is_program(appimage(elf(true, true, 4), 2)));     // or a core dump

    Bytes truncated = elf(true, true, 3, 4, 3);
    truncated.resize(64 + 56);  // the PT_INTERP header is past the end
    CHECK_FALSE(is_program(truncated));
    Bytes short_header = elf(true, true, 2);
    short_header.resize(40);
    CHECK_FALSE(is_program(short_header));
    Bytes bad_class = elf(true, true, 2);
    bad_class[4] = 7;
    CHECK_FALSE(is_program(bad_class));
    Bytes too_many = elf(true, true, 3, 1, 0);
    put(too_many, 56, 60000, 2, true);  // far more headers than any file has
    CHECK_FALSE(is_program(too_many));
}

TEST_CASE("check_program: a real program, executable or not; a script is not one") {
    const fs::path real = harmless_program();
    if (real.empty()) {
        FAIL("missing executable fixture");
        return;
    }
    const TempDir dir;
    const fs::path tool = dir.path / "tool";
    fs::copy_file(real, tool);
    fs::permissions(tool, fs::perms::owner_read | fs::perms::owner_write);
    auto check = rummage::platform::check_program(tool);
    CHECK(check.program);
    CHECK_FALSE(check.executable);
    fs::permissions(tool, fs::perms::owner_exec, fs::perm_options::add);
    check = rummage::platform::check_program(tool);
    CHECK(check.executable);

    const fs::path script = dir.write("run.sh", "#!/bin/sh\nexit 0\n");
    fs::permissions(script, fs::perms::owner_all);
    CHECK_FALSE(rummage::platform::check_program(script).program);
    CHECK_FALSE(rummage::platform::check_program(dir.path / "gone").program);
}

TEST_CASE("may_be_program: executables and AppImages by type, never documents or scripts") {
    const auto mime = MimeDatabase::load();
    const auto is = [&](const char* type) {
        return rummage::platform::may_be_program(*mime, mime->find(type));
    };
    CHECK(is("application/x-executable"));
    CHECK(is("application/x-sharedlib"));
    CHECK(is("application/vnd.appimage"));
    CHECK_FALSE(is("application/pdf"));
    CHECK_FALSE(is("text/plain"));
    // A script is a kind of executable to shared-mime-info, and the ELF check turns it away.
    CHECK(is("application/x-shellscript"));
}

TEST_CASE(
    "opener: a program starts itself, asks first without its bit, never by a document's "
    "name") {
    const fs::path real = harmless_program();
    if (real.empty()) {
        FAIL("missing executable fixture");
        return;
    }
    const TempDir dir;
    Environment environment;
    environment.data_dirs = {dir.path / "share"};
    environment.config_dirs = {dir.path / "config"};
    environment.path = (dir.path / "bin").string();
    const fs::path apps = dir.path / "apps";
    fs::create_directories(apps);
    const fs::path tool = apps / "tool";
    fs::copy_file(real, tool);
    fs::permissions(tool, fs::perms::owner_all);

    EventLoop loop;
    WorkerPool pool{loop, 2};
    std::vector<std::pair<std::string, bool>> reports;
    std::vector<fs::path> asked;
    std::function<void()> yes;
    Opener opener{
        loop, pool, MimeDatabase::load(), environment,
        [&](const std::string& message, bool error) { reports.emplace_back(message, error); }};
    opener.set_ask_to_run([&](fs::path program, std::function<void()> allow_and_run) {
        asked.push_back(std::move(program));
        yes = std::move(allow_and_run);
    });
    const auto open = [&](const std::string& name) {
        reports.clear();
        // The real size: detection reads no more than the entry's size.
        opener.open(apps,
                    {{.name = name, .kind = EntryKind::file, .size = fs::file_size(apps / name)}});
        REQUIRE(run_until(loop, [&] {
            return !opener.planning() && opener.launcher().running() == 0 &&
                   (!reports.empty() || !asked.empty());
        }));
    };

    // Allowed to run: it starts, in its folder, and exits 0 (nothing more said).
    open("tool");
    CHECK(reports == std::vector<std::pair<std::string, bool>>{{"Starting tool…", false}});

    // Not allowed: asked, nothing started; yes gives it u+x and starts it.
    fs::permissions(tool, fs::perms::owner_read | fs::perms::owner_write);
    open("tool");
    CHECK(reports.empty());
    REQUIRE(asked == std::vector<fs::path>{tool});
    opener.set_programs(false);
    yes();
    CHECK_FALSE(opener.planning());
    CHECK((fs::status(tool).permissions() & fs::perms::owner_exec) == fs::perms::none);
    opener.set_programs(true);
    yes();
    REQUIRE(run_until(loop, [&] {
        return !reports.empty() && !opener.planning() && opener.launcher().running() == 0;
    }));
    CHECK(reports == std::vector<std::pair<std::string, bool>>{{"Starting tool…", false}});
    CHECK((fs::status(tool).permissions() & fs::perms::owner_exec) != fs::perms::none);

    // A program named like a document is a document: never started.
    fs::copy_file(real, apps / "invoice.pdf");
    fs::permissions(apps / "invoice.pdf", fs::perms::owner_all);
    asked.clear();
    open("invoice.pdf");
    CHECK(asked.empty());
    REQUIRE(reports.size() == 1);
    CHECK(reports[0] == std::pair<std::string, bool>{"No application opens application/pdf", true});

    // A script with its bit set opens as text, never runs.
    const fs::path script = apps / "run.sh";
    static_cast<void>(
        dir.write("apps/run.sh", "#!/bin/sh\ntouch " + (dir.path / "ran").string() + "\n"));
    fs::permissions(script, fs::perms::owner_all);
    open("run.sh");
    CHECK(asked.empty());
    REQUIRE(reports.size() == 1);
    CHECK(reports[0].first.starts_with("No application opens"));
    CHECK_FALSE(fs::exists(dir.path / "ran"));

    // [open] programs = false: not even an allowed program starts.
    opener.set_programs(false);
    open("tool");
    CHECK(asked.empty());
    REQUIRE(reports.size() == 1);
    CHECK(reports[0].first.starts_with("No application opens"));
}
