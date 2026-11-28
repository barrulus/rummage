#include "rummage/photos/work_diagnostics.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>

#include "rummage/core/log.hpp"

namespace rummage::photos::detail {
namespace {
constexpr auto interval = std::chrono::seconds{30};

std::string operation(const nlohmann::json& request) {
    const auto it = request.find("op");
    return it != request.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

std::string safe_operation(std::string_view op) {
    // Never echo arbitrary request contents into the journal.
    constexpr std::array known{"work",
                               "background_control",
                               "add_source",
                               "refresh_source",
                               "watch_paths",
                               "query",
                               "sources",
                               "jobs",
                               "progress",
                               "create_album",
                               "rename_album",
                               "delete_album",
                               "album_add",
                               "album_remove",
                               "metadata_claim",
                               "metadata_batch",
                               "operation_complete",
                               "recover_operations",
                               "catalog_identity"};
    return std::find(known.begin(), known.end(), op) != known.end() ? std::string{op}
                                                                    : "catalog request";
}

std::uint64_t count(const nlohmann::json& value, const char* key) {
    const auto it = value.find(key);
    if (it == value.end() || !it->is_number_integer())
        return 0;
    if (it->is_number_unsigned())
        return it->get<std::uint64_t>();
    return static_cast<std::uint64_t>(std::max(std::int64_t{0}, it->get<std::int64_t>()));
}

std::string reason(const nlohmann::json& response, const char* key) {
    const auto it = response.find(key);
    if (it == response.end() || !it->is_string())
        return "request failed without a reason";
    return bounded_diagnostic_reason(it->get_ref<const std::string&>());
}

void add(std::uint64_t& total, std::uint64_t amount) {
    total += std::min(amount, std::numeric_limits<std::uint64_t>::max() - total);
}
}  // namespace

std::string bounded_diagnostic_reason(std::string_view input) {
    auto length = std::min(input.size(), std::size_t{240});
    // Preserve UTF-8 when a bounded sample ends inside a multibyte character.
    if (length < input.size())
        while (length > 0 && (static_cast<unsigned char>(input[length]) & 0xc0U) == 0x80U)
            --length;
    std::string text{input.substr(0, length)};
    for (auto& byte : text)
        if (static_cast<unsigned char>(byte) < 32 || byte == 127)
            byte = ' ';
    return text;
}

std::uint64_t WorkDiagnostics::begin(const nlohmann::json& request) {
    if (operation(request) == "work")
        ++in_flight_;
    return generation_;
}

std::string WorkDiagnostics::totals() const {
    return "entries processed=" + std::to_string(processed_) +
           ", metadata processed=" + std::to_string(metadata_);
}

void WorkDiagnostics::errors(std::vector<Event>& events, Clock::time_point now, bool force) {
    if (request_errors_ == 0 && metadata_errors_ == 0 && changed_ == 0 && source_errors_ == 0 &&
        work_errors_ == 0)
        return;
    if (!force && error_reported_ && now - last_error_ < interval)
        return;
    events.push_back(
        {Kind::error, "Photo Library error: request failures=" + std::to_string(request_errors_) +
                          " (last operation=" + last_operation_ +
                          "), metadata failures=" + std::to_string(metadata_errors_) +
                          ", changed or unavailable originals=" + std::to_string(changed_) +
                          ", unavailable sources=" + std::to_string(source_errors_) +
                          ", work failures=" + std::to_string(work_errors_) +
                          "; reason=" + last_reason_});
    request_errors_ = metadata_errors_ = changed_ = source_errors_ = work_errors_ = 0;
    last_error_ = now;
    error_reported_ = true;
}

void WorkDiagnostics::stop(std::vector<Event>& events, Clock::time_point now,
                           std::string_view reason) {
    if (!running_)
        return;
    errors(events, now, true);
    events.push_back({Kind::stop, "Photo Library stop: " + std::string{reason} + "; " + totals()});
    running_ = false;
}

std::vector<WorkDiagnostics::Event> WorkDiagnostics::finish(const nlohmann::json& request,
                                                            const nlohmann::json& response,
                                                            std::uint64_t generation,
                                                            Clock::time_point now, bool cancelled) {
    std::vector<Event> events;
    const auto op = operation(request);
    const bool work = op == "work";
    if (work && in_flight_ > 0)
        --in_flight_;
    if (cancelled) {
        if (work && in_flight_ == 0 && generation == generation_)
            stop(events, now, "cancelled");
        return events;
    }
    if (!response.value("ok", false)) {
        add(request_errors_, 1);
        last_operation_ = safe_operation(op);
        last_reason_ = reason(response, "error");
        errors(events, now, false);
        if (work && in_flight_ == 0 && generation == generation_)
            stop(events, now, "request failed");
        return events;
    }
    if (op == "background_control") {
        // In-flight work started before an explicit pause/resume must not restart a stopped run.
        if (request.contains("paused")) {
            ++generation_;
            if (response.value("paused", false)) {
                errors(events, now, true);
                stop(events, now, "paused");
            }
        }
        return events;
    }
    if (!work)
        return events;
    const auto work_failures = count(response, "work_errors");
    add(work_errors_, work_failures);
    if (work_failures > 0)
        last_reason_ = reason(response, "work_error");
    if (response.value("waiting", false) && work_failures == 0) {
        add(source_errors_, 1);
        last_reason_ = "Photo source is unavailable; reconnect it and retry";
    }
    add(metadata_errors_, count(response, "metadata_failed"));
    if (count(response, "metadata_failed") > 0)
        last_reason_ = response.contains("metadata_failure_reason")
                           ? reason(response, "metadata_failure_reason")
                           : "Image decoding or metadata extraction failed";
    if (response.contains("metadata_error")) {
        add(metadata_errors_, 1);
        last_reason_ = reason(response, "metadata_error");
    }
    if (const auto results = response.find("metadata_results");
        results != response.end() && results->is_array()) {
        for (const auto& result : *results) {
            if (!result.value("ok", true)) {
                add(metadata_errors_, 1);
                last_reason_ = reason(result, "error");
            }
            if (result.value("stale", false)) {
                add(changed_, 1);
                if (metadata_errors_ == 0)
                    last_reason_ = "Original changed or became unavailable during metadata work";
            }
        }
    }
    if (metadata_errors_ || changed_ || source_errors_ || work_errors_)
        last_operation_ = "work";
    if (generation != generation_) {
        errors(events, now, false);
        return events;
    }
    if (response.value("worked", false)) {
        if (!running_) {
            running_ = true;
            processed_ = metadata_ = 0;
            checkpoint_ = now;
            events.push_back({Kind::start, "Photo Library start: background work"});
        }
        add(processed_, count(response, "processed"));
        add(metadata_, count(response, "metadata_processed"));
    }
    errors(events, now, false);
    if (response.value("paused", false))
        stop(events, now, "paused");
    else if (!response.value("worked", false) && !response.value("busy", false) && in_flight_ == 0)
        stop(events, now, "idle");
    else if (running_ && response.value("worked", false) && now - checkpoint_ >= interval) {
        events.push_back({Kind::checkpoint, "Photo Library checkpoint: " + totals()});
        checkpoint_ = now;
    }
    return events;
}

std::vector<WorkDiagnostics::Event> WorkDiagnostics::shutdown(Clock::time_point now) {
    std::vector<Event> events;
    errors(events, now, true);
    stop(events, now, "client closed");
    return events;
}

SharedWorkDiagnostics::~SharedWorkDiagnostics() {
    try {
        for (const auto& event : state.shutdown(WorkDiagnostics::Clock::now()))
            log::journal(
                event.kind == WorkDiagnostics::Kind::error ? log::Level::warn : log::Level::info,
                event.message);
    } catch (...) {
        // Destruction of the final client must remain noexcept.
    }
}

std::shared_ptr<SharedWorkDiagnostics> SharedWorkDiagnostics::for_catalog(
    const std::filesystem::path& path) {
    static std::mutex mutex;
    static std::map<std::filesystem::path, std::weak_ptr<SharedWorkDiagnostics>> states;
    const std::lock_guard lock{mutex};
    std::erase_if(states, [](const auto& entry) { return entry.second.expired(); });
    auto& slot = states[path.lexically_normal()];
    auto state = slot.lock();
    if (!state) {
        state = std::make_shared<SharedWorkDiagnostics>();
        slot = state;
    }
    return state;
}

}  // namespace rummage::photos::detail
