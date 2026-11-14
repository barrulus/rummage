#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "rummage/engines/engines.hpp"
#include "rummage/isolate/helpers.hpp"
#include "rummage/picture/load.hpp"
#include "rummage/preview/preview.hpp"
#include "rummage/thumbs/cache.hpp"
#include "temp_dir.hpp"
namespace fs = std::filesystem;
namespace isolate = rummage::isolate;
namespace picture = rummage::picture;
namespace engines = rummage::engines;
using rummage::test::TempDir;
static const fs::path fixtures{RUMMAGE_PDF_DATA};

TEST_CASE("pdf: actual sandbox draws the first page, rotated pages and built-in fonts") {
    isolate::Helpers helpers{{.program = RUMMAGE_PDF_HELPER, .args = {}}};
    auto red = helpers.load({.path = fixtures / "red.pdf", .max_width = 128, .max_height = 128});
    INFO(red.detail);
    REQUIRE(red.failure == picture::Failure::none);
    CHECK(red.image.width == 128);
    CHECK(red.image.height == 64);
    CHECK(red.image.rgba[0] > 220);
    CHECK(red.image.rgba[1] < 30);
    CHECK(red.image.rgba[3] == 255);

    auto rotated =
        helpers.load({.path = fixtures / "rotated.pdf", .max_width = 128, .max_height = 128});
    REQUIRE(rotated.failure == picture::Failure::none);
    CHECK(rotated.image.width == 64);
    CHECK(rotated.image.height == 128);

    // Helvetica is not embedded: PDFium's own font draws it, without opening a file in the cage.
    auto text = helpers.load({.path = fixtures / "text.pdf", .max_width = 200, .max_height = 200});
    REQUIRE(text.failure == picture::Failure::none);
    std::size_t ink = 0;
    for (std::size_t i = 0; i < text.image.rgba.size(); i += 4) {
        ink += text.image.rgba[i] < 100 ? 1U : 0U;
    }
    CHECK(text.image.rgba[0] == 255);  // white paper
    CHECK(ink > 200);

    auto large =
        helpers.load({.path = fixtures / "red.pdf", .max_width = 8000, .max_height = 8000});
    REQUIRE(large.failure == picture::Failure::none);
    CHECK(large.image.width <= 2048);
}

TEST_CASE("pdf: damaged files fail without killing the helper") {
    TempDir dir;
    isolate::Helpers helpers{{.program = RUMMAGE_PDF_HELPER, .args = {}}};
    for (const auto& path :
         {dir.write("broken.pdf", "not a pdf"),
          dir.write("cut.pdf", "%PDF-1.4\n1 0 obj<</Type/Catalog"), dir.write("empty.pdf", "")}) {
        INFO(path);
        CHECK(helpers.load({.path = path, .max_width = 128, .max_height = 128}).failure !=
              picture::Failure::none);
    }
    CHECK(
        helpers.load({.path = fixtures / "red.pdf", .max_width = 128, .max_height = 128}).failure ==
        picture::Failure::none);
}

TEST_CASE("pdf: saved text fields and selected checkbox appearances are drawn") {
    isolate::Helpers helpers{{.program = RUMMAGE_PDF_HELPER, .args = {}}};
    for (const char* name : {"form.pdf", "form-rotated.pdf"}) {
        INFO(name);
        const auto loaded =
            helpers.load({.path = fixtures / name, .max_width = 200, .max_height = 200});
        INFO(loaded.detail);
        REQUIRE(loaded.failure == picture::Failure::none);
        const auto& image = loaded.image;
        CHECK(image.width * image.height == 200 * 120);
        std::size_t red = 0;
        std::size_t black = 0;
        std::size_t blue = 0;
        std::size_t green = 0;
        for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
            const auto r = image.rgba[i];
            const auto g = image.rgba[i + 1];
            const auto b = image.rgba[i + 2];
            red += r > 220 && g < 30 && b < 30 ? 1U : 0U;
            black += r < 30 && g < 30 && b < 30 ? 1U : 0U;
            blue += r < 30 && g < 30 && b > 220 ? 1U : 0U;
            green += r < 30 && g > 220 && b < 30 ? 1U : 0U;
        }
        // No page contents: all ink comes from the widgets. The text field has
        // a red background and black APPROVED; the checked state is blue, off green.
        CHECK(red > 4000);
        CHECK(black > 200);
        CHECK(blue > 300);
        CHECK(green == 0);
        // The fixtures also contain an endless document-open JS action: it must
        // never run when initializing the form environment or drawing appearances.
    }
}

TEST_CASE("pdf: MIME routes to its own helper, thumbnails and Quick Look use it") {
    TempDir dir;
    const auto source = dir.path / "a 'paper'; $test.pdf";
    fs::copy_file(fixtures / "red.pdf", source);
    const auto registry = engines::Registry::load(RUMMAGE_ENGINE_DATA, [](std::string_view name) {
        return fs::path{name == "rummage-pdf" ? RUMMAGE_PDF_HELPER : RUMMAGE_BASE_HELPER};
    });
    isolate::use_engines(registry);
    // What rummage does at start-up: Quick Look asks the installed registry who shows a type.
    engines::set_installed(std::make_shared<const engines::Registry>(registry));
    const rummage::thumbs::Cache cache{dir.path / "cache", "test"};
    const auto generated = cache.get(source, rummage::thumbs::Size::normal, "application/pdf");
    INFO(generated.error);
    REQUIRE_FALSE(generated.image.empty());
    CHECK(generated.image.rgba[0] > 220);
    const auto mime = rummage::fs::MimeDatabase::load();
    const auto& provider = rummage::preview::provider_for(*mime, mime->find("application/pdf"));
    CHECK(provider.kind() == rummage::preview::Kind::image);
    const auto content =
        provider.load({.path = source, .mime = "application/pdf", .width = 128, .height = 128}, {});
    REQUIRE(std::holds_alternative<picture::Loaded>(content));
    CHECK(std::get<picture::Loaded>(content).failure == picture::Failure::none);
}

TEST_CASE("pdf: password protected files say so, an empty user password just opens") {
    isolate::Helpers helpers{{.program = RUMMAGE_PDF_HELPER, .args = {}}};
    auto locked =
        helpers.load({.path = fixtures / "locked.pdf", .max_width = 128, .max_height = 128});
    CHECK(locked.failure == picture::Failure::locked);
    CHECK(locked.error() == "This file is password protected");
    auto open =
        helpers.load({.path = fixtures / "owner-only.pdf", .max_width = 128, .max_height = 128});
    INFO(open.detail);
    REQUIRE(open.failure == picture::Failure::none);
    CHECK(open.image.rgba[0] > 220);
}

TEST_CASE("pdf: the cache remembers why a PDF failed, for the icon's mark") {
    TempDir dir;
    const auto registry = engines::Registry::load(RUMMAGE_ENGINE_DATA, [](std::string_view name) {
        return fs::path{name == "rummage-pdf" ? RUMMAGE_PDF_HELPER : RUMMAGE_BASE_HELPER};
    });
    isolate::use_engines(registry);
    const rummage::thumbs::Cache cache{dir.path / "cache", "test"};
    const auto locked = dir.path / "locked.pdf";
    fs::copy_file(fixtures / "locked.pdf", locked);
    const auto broken = dir.write("broken.pdf", "%PDF-1.4\nnothing else");
    for (int round = 0; round < 2; ++round) {
        INFO("round " << round);  // the second time from fail/, without the helper
        const auto first = cache.get(locked, rummage::thumbs::Size::normal, "application/pdf");
        CHECK(first.image.empty());
        CHECK(first.failure == picture::Failure::locked);
        const auto second = cache.get(broken, rummage::thumbs::Size::normal, "application/pdf");
        CHECK(second.image.empty());
        CHECK(second.failure == picture::Failure::damaged);
    }
    CHECK(fs::exists(dir.path / "cache/fail/test-document-v1"));
    // The reason is kept per file: the file changed, the mark goes.
    fs::copy_file(fixtures / "red.pdf", locked, fs::copy_options::overwrite_existing);
    fs::last_write_time(locked, fs::last_write_time(locked) + std::chrono::seconds{5});
    const auto fixed = cache.get(locked, rummage::thumbs::Size::normal, "application/pdf");
    CHECK_FALSE(fixed.image.empty());
    CHECK(fixed.failure == picture::Failure::none);
}

// Security review: hostile files. Each must end in bounded time, never take the app down, and
// leave the helper pool working for the next file.
namespace {
const fs::path kAttacks = fixtures / "attack";

// Seconds a check may take: sanitizer builds run decoders, and Helpers' timeouts, five times
// slower (Helpers::Helpers).
double seconds(double plain) {
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    return plain * 5.0;
#else
    return plain;
#endif
}

struct Timed {
    picture::Loaded loaded;
    double seconds{0.0};
};

Timed timed_load(isolate::Helpers& helpers, const fs::path& path) {
    const auto start = std::chrono::steady_clock::now();
    Timed result{.loaded = helpers.load({.path = path, .max_width = 256, .max_height = 256})};
    result.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return result;
}

void still_works(isolate::Helpers& helpers) {
    const auto red =
        helpers.load({.path = fixtures / "red.pdf", .max_width = 64, .max_height = 64});
    CHECK(red.failure == picture::Failure::none);
}

// A sparse file exercises the real 512 MiB limit without storing/reading half a
// gigabyte in the fixture corpus. Cover both page contents and image data: PDFium
// versions may read streams eagerly or defer them to rendering. Neither may
// return partial success.
fs::path budget_pdf(const TempDir& dir, bool image) {
    const auto path = dir.path / (image ? "large-image.pdf" : "large-contents.pdf");
    std::ofstream file{path, std::ios::binary};
    file << "%PDF-1.4\n";
    std::vector<std::streamoff> offsets{0};
    const auto object = [&](std::string_view body) {
        offsets.push_back(file.tellp());
        file << offsets.size() - 1 << " 0 obj\n" << body << "\nendobj\n";
    };
    object("<</Type /Catalog /Pages 2 0 R>>");
    object("<</Type /Pages /Count 1 /Kids [3 0 R]>>");
    object(image ? "<</Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Contents 5 0 R "
                   "/Resources <</XObject <</Im 4 0 R>>>>>>"
                 : "<</Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Contents 4 0 R "
                   "/Resources <<>>>>");
    offsets.push_back(file.tellp());
    constexpr std::streamoff length = 513LL * 1024 * 1024;
    file << "4 0 obj\n<</Length " << length;
    if (image) {
        file << " /Type /XObject /Subtype /Image /Width 1 /Height 1 "
                "/ColorSpace /DeviceRGB /BitsPerComponent 8";
    }
    file << ">>\nstream\n";
    const auto start = file.tellp();
    // In page contents the sparse bytes belong to a comment. In an image the
    // first three bytes are one red pixel and the trailing bytes are ignored.
    file.put(image ? static_cast<char>(255) : '%');
    const std::string tail = image ? "\n" : "\n1 0 0 rg 0 0 200 100 re f\n";
    file.seekp(start + length - static_cast<std::streamoff>(tail.size()));
    file << tail << "endstream\nendobj\n";
    if (image) {
        const std::string contents = "q 200 0 0 100 0 0 cm /Im Do Q\n";
        object("<</Length " + std::to_string(contents.size()) + ">>\nstream\n" + contents +
               "endstream");
    }
    const auto xref = file.tellp();
    file << "xref\n0 " << offsets.size() << "\n0000000000 65535 f \n";
    for (std::size_t i = 1; i < offsets.size(); ++i) {
        file << std::setfill('0') << std::setw(10) << offsets[i] << " 00000 n \n";
    }
    file << "trailer\n<</Size " << offsets.size() << " /Root 1 0 R>>\nstartxref\n"
         << xref << "\n%%EOF\n";
    file.close();
    REQUIRE(file.good());
    return path;
}
}  // namespace

TEST_CASE("pdf security: exhausted reads never return a successful partial preview") {
    TempDir dir;
    isolate::Helpers helpers{
        {.program = RUMMAGE_PDF_HELPER, .args = {}, .timeout = std::chrono::seconds{8}}};
    for (const bool image : {false, true}) {
        INFO((image ? "image stream" : "page contents"));
        const auto run = timed_load(helpers, budget_pdf(dir, image));
        INFO(run.loaded.detail);
        CHECK(run.loaded.failure == picture::Failure::unavailable);
        CHECK(run.loaded.detail == "pdf reading budget exhausted");
        CHECK(run.loaded.image.empty());
        CHECK(run.seconds < seconds(7.0));
        still_works(helpers);
    }
}

TEST_CASE("pdf security: decompression bombs hit the memory budget, quickly") {
    isolate::Helpers helpers{
        {.program = RUMMAGE_PDF_HELPER, .args = {}, .timeout = std::chrono::seconds{8}}};
    for (const char* name : {"bomb-font.pdf", "bomb-image.pdf"}) {
        INFO(name);
        const Timed run = timed_load(helpers, kAttacks / name);
        INFO(run.loaded.detail << ", " << run.seconds << " s");
        // Not killed by the timeout: the helper itself stopped the bomb.
        CHECK(run.loaded.failure != picture::Failure::unavailable);
        CHECK(run.seconds < seconds(7.0));
        still_works(helpers);
    }
}

TEST_CASE("pdf security: endless drawing is cut off by the helper timeout") {
    isolate::Helpers helpers{
        {.program = RUMMAGE_PDF_HELPER, .args = {}, .timeout = std::chrono::seconds{8}}};
    const Timed run = timed_load(helpers, kAttacks / "bomb-cpu.pdf");
    INFO(run.loaded.detail << ", " << run.seconds << " s");
    CHECK(run.seconds < seconds(10.0));
    still_works(helpers);
}

TEST_CASE("pdf security: JavaScript never runs, external stream files are never read") {
    isolate::Helpers helpers{
        {.program = RUMMAGE_PDF_HELPER, .args = {}, .timeout = std::chrono::seconds{8}}};
    const Timed script = timed_load(helpers, kAttacks / "javascript.pdf");
    INFO(script.loaded.detail);
    REQUIRE(script.loaded.failure == picture::Failure::none);  // while(true){} did not run
    CHECK(script.seconds < seconds(2.0));
    CHECK(script.loaded.image.rgba[0] > 220);

    const Timed external = timed_load(helpers, kAttacks / "external.pdf");
    INFO(external.loaded.detail);
    if (external.loaded.failure == picture::Failure::none) {
        // The page's only content is /F (/etc/passwd): nothing of it is drawn.
        for (std::size_t i = 0; i < external.loaded.image.rgba.size(); i += 4) {
            REQUIRE(external.loaded.image.rgba[i] == 255);
            REQUIRE(external.loaded.image.rgba[i + 1] == 255);
        }
    }
    still_works(helpers);
}

TEST_CASE("pdf security: recursion, deep nesting and absurd pages end safely") {
    isolate::Helpers helpers{
        {.program = RUMMAGE_PDF_HELPER, .args = {}, .timeout = std::chrono::seconds{8}}};
    for (const char* name :
         {"recursive-form.pdf", "recursive-pages.pdf", "deep.pdf", "huge-page.pdf", "tiny-page.pdf",
          "nan-page.pdf", "no-pages.pdf", "lying-count.pdf"}) {
        INFO(name);
        const Timed run = timed_load(helpers, kAttacks / name);
        INFO(run.loaded.detail << ", " << run.seconds << " s");
        CHECK(run.seconds < seconds(7.0));
        CHECK(run.loaded.failure != picture::Failure::unavailable);  // the helper did not die
        if (run.loaded.failure == picture::Failure::none) {
            CHECK(run.loaded.image.width <= 256);
            CHECK(run.loaded.image.height <= 256);
            CHECK(run.loaded.image.rgba.size() ==
                  std::size_t{run.loaded.image.width} * run.loaded.image.height * 4);
        }
        still_works(helpers);
    }
    CHECK(timed_load(helpers, kAttacks / "no-pages.pdf").loaded.failure ==
          picture::Failure::damaged);
    // A page tree that claims two billion pages is not walked: only page one is drawn.
    const Timed lying = timed_load(helpers, kAttacks / "lying-count.pdf");
    CHECK(lying.seconds < seconds(2.0));
}

TEST_CASE("pdf security: only PDF is parsed, other formats named .pdf are refused") {
    isolate::Helpers helpers{{.program = RUMMAGE_PDF_HELPER, .args = {}}};
    for (const char* name : {"cbz-named.pdf", "svg-named.pdf", "html-named.pdf"}) {
        INFO(name);
        const Timed run = timed_load(helpers, kAttacks / name);
        INFO(run.loaded.detail);
        CHECK(run.loaded.failure == picture::Failure::damaged);
    }
    still_works(helpers);
}
