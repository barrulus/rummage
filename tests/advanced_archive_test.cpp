#include "rummage/fs/advanced_archive.hpp"

#include <chrono>
#include <fstream>
#include <thread>

#include <doctest/doctest.h>

#include "rummage/fs/compress.hpp"
#include "temp_dir.hpp"

using namespace rummage::fs;
namespace fs = std::filesystem;

namespace {
std::shared_ptr<ArchiveOptions> options() {
    auto out = std::make_shared<ArchiveOptions>();
    out->program = seven_zip_program();
    return out;
}

TransferRequest request(const fs::path& root, const std::shared_ptr<ArchiveOptions>& opts) {
    return {.kind = TransferKind::compress,
            .source = root,
            .names = {"naïve file.txt"},
            .destination = root,
            .archive_name = "test archive.7z",
            .archive_options = opts};
}
}  // namespace

TEST_CASE("advanced archive: encrypted 7z verifies with stdin password, wrong secret fails") {
    rummage::test::TempDir dir;
    dir.write("naïve file.txt", "interoperable data");
    auto opts = options();
    if (opts->program.empty()) {
        MESSAGE("7z unavailable");
        return;
    }
    opts->encrypt_names = true;
    opts->password = std::make_shared<ArchivePassword>("secret not in argv");
    auto made = run_compress(request(dir.path, opts), {}, {});
    CHECK(made.refused.empty());
    CHECK(made.errors.empty());
    REQUIRE(made.placed == std::vector<std::string>{"test archive.7z"});
    CHECK(opts->password->value.empty());
    CHECK_FALSE(run_compress(request(dir.path, opts), {}, {}).refused.empty());
    auto test = options();
    test->test_only = true;
    auto req = request(dir.path, test);
    req.names = made.placed;
    test->password = std::make_shared<ArchivePassword>("wrong secret");
    auto wrong = run_compress(req, {}, {});
    REQUIRE_FALSE(wrong.errors.empty());
    CHECK(wrong.errors.front().find("wrong secret") == std::string::npos);
    test->password = std::make_shared<ArchivePassword>("secret not in argv");
    auto correct = run_compress(req, {}, {});
    CHECK(correct.errors.empty());
    CHECK(correct.files_done == 1);
    CHECK(correct.placed.empty());
}

TEST_CASE(
    "advanced archive: multipart publication preserves existing archives and validates all "
    "volumes") {
    rummage::test::TempDir dir;
    dir.write("naïve file.txt", std::string(8192, 'x'));
    auto opts = options();
    if (opts->program.empty()) {
        MESSAGE("7z unavailable");
        return;
    }
    opts->volume_bytes = 1024;
    opts->level = 0;
    auto req = request(dir.path, opts);
    auto made = run_compress(req, {}, {});
    CHECK(made.refused.empty());
    CHECK(made.errors.empty());
    REQUIRE(made.placed.size() == 1);
    const auto published = dir.path / made.placed.front();
    REQUIRE(fs::is_directory(published));
    CHECK(fs::exists(published / "test archive.7z.001"));
    CHECK(fs::exists(published / "test archive.7z.002"));
    auto again = run_compress(req, {}, {});
    REQUIRE(again.placed.size() == 1);
    CHECK(again.placed != made.placed);
    auto test = options();
    test->test_only = true;
    auto test_request = request(published, test);
    test_request.names = {"test archive.7z.001"};
    CHECK(run_compress(test_request, {}, {}).errors.empty());
    fs::remove(published / "test archive.7z.002");
    CHECK_FALSE(run_compress(test_request, {}, {}).errors.empty());
}

TEST_CASE(
    "advanced archive: cancellation and missing input publish nothing; options reject unsafe "
    "names") {
    rummage::test::TempDir dir;
    auto opts = options();
    auto req = request(dir.path, opts);
    rummage::core::CancelToken token;
    token.cancel();
    if (!opts->program.empty())
        CHECK(run_compress(req, token, {}).cancelled);
    opts->program.clear();
    CHECK_FALSE(run_compress(req, {}, {}).refused.empty());
    opts = options();
    req.archive_options = opts;
    if (opts->program.empty())
        return;
    CHECK_FALSE(run_compress(req, {}, {}).errors.empty());
    CHECK(fs::is_empty(dir.path));
    req.names = {"../outside"};
    CHECK_FALSE(run_compress(req, {}, {}).refused.empty());
    CHECK(fs::is_empty(dir.path));
}

TEST_CASE("advanced archive: active cancellation reaps backend and removes staging") {
    rummage::test::TempDir dir;
    auto opts = options();
    if (opts->program.empty())
        return;
    dir.write("naïve file.txt", std::string(32U << 20U, 'x'));
    opts->level = 9;
    rummage::core::CancelToken token;
    std::jthread cancel{[token] {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        token.cancel();
    }};
    const auto result = run_compress(request(dir.path, opts), token, {});
    CHECK(result.cancelled);
    CHECK(result.placed.empty());
    for (const auto& file : fs::directory_iterator(dir.path))
        CHECK(file.path().filename() == "naïve file.txt");
}
