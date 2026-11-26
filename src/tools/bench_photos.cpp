#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <sys/resource.h>

#include "rummage/engines/engines.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/isolate/helpers.hpp"
#include "rummage/photos/client.hpp"

// Explicit, disposable-fixture benchmark of the same catalog/isolated-image path as the UI.
// This intentionally has no UI scheduling delay: throughput is pipeline capacity, not a
// promise about interactive import speed. It never runs on application startup or installs.
int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: rummage-bench-photos NEW-CATALOG ABSOLUTE-FIXTURE-FOLDER\n");
        return 2;
    }
    try {
        const std::filesystem::path catalog{argv[1]}, source{argv[2]};
        if (!catalog.is_absolute() || !source.is_absolute() || std::filesystem::exists(catalog))
            throw std::runtime_error(
                "Use an absent absolute catalog path and an absolute disposable source");
        const auto registry = std::make_shared<const rummage::engines::Registry>(
            rummage::engines::Registry::load_installed());
        const auto mime = rummage::fs::MimeDatabase::load();
        rummage::isolate::use_engines(*registry, {}, mime);
        rummage::engines::set_installed(registry);
        const rummage::photos::Client client{rummage::photos::Client::installed_path(), catalog};
        const auto call = [&](const nlohmann::json& input) {
            auto result = client.request(input);
            if (!result.value("ok", false))
                throw std::runtime_error(result.value("error", "Photo request failed"));
            return result;
        };
        using Clock = std::chrono::steady_clock;
        const auto start = Clock::now();
        const auto milliseconds = [&](Clock::time_point since) {
            return std::chrono::duration<double, std::milli>(Clock::now() - since).count();
        };
        call({{"op", "add_source"}, {"path", source.string()}});
        double first_page_ms = 0;
        std::size_t batches = 0;
        std::size_t metadata_items = 0;
        for (;;) {
            const auto work = call({{"op", "work"}, {"budget", 32}});
            ++batches;
            metadata_items += work.value("metadata_processed", std::size_t{0});
            if (first_page_ms == 0 && call({{"op", "query"}, {"limit", 1}}).at("total") != 0)
                first_page_ms = milliseconds(start);
            if (!work.value("worked", false))
                break;
            if (milliseconds(start) > 3600000)
                throw std::runtime_error("Import did not complete within one hour");
        }
        const auto cold_ms = milliseconds(start);
        const auto jobs = call({{"op", "jobs"}}).at("jobs");
        const auto count = call({{"op", "query"}, {"limit", 1}}).at("total");
        const auto rescan = Clock::now();
        call({{"op", "refresh"}});
        std::size_t repeated_metadata = 0;
        for (;;) {
            const auto work = call({{"op", "work"}, {"budget", 32}});
            repeated_metadata += work.value("metadata_processed", std::size_t{0});
            if (!work.value("worked", false))
                break;
            if (milliseconds(rescan) > 3600000)
                throw std::runtime_error("Rescan did not complete within one hour");
        }
        const auto rescan_ms = milliseconds(rescan);
        rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        const nlohmann::json report{{"records", count},
                                    {"first_result_page_ms", first_page_ms},
                                    {"cold_pipeline_ms", cold_ms},
                                    {"unchanged_rescan_ms", rescan_ms},
                                    {"metadata_items", metadata_items},
                                    {"repeated_metadata_items", repeated_metadata},
                                    {"batches", batches},
                                    {"parent_peak_rss_kib", usage.ru_maxrss},
                                    {"jobs", jobs},
                                    {"method",
                                     "Client work batches with real isolated image engines, no UI "
                                     "pacing; parent RSS excludes helpers"}};
        std::puts(report.dump(2).c_str());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "photo benchmark: %s\n", error.what());
        return 1;
    }
}
