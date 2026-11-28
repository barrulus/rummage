#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace rummage::photos::detail {

// A bounded reducer: only counters, timestamps, an allowlisted operation, and one sanitized
// error reason (at most 240 bytes) are retained, never photo IDs or request/history payloads.
// Its owner serializes begin/finish; supplied timestamps allow tests without sleeps.
std::string bounded_diagnostic_reason(std::string_view text);

class WorkDiagnostics {
public:
    using Clock = std::chrono::steady_clock;
    enum class Kind { start, stop, checkpoint, error };

    struct Event {
        Kind kind;
        std::string message;
    };

    std::vector<Event> shutdown(Clock::time_point now);
    std::uint64_t begin(const nlohmann::json& request);
    std::vector<Event> finish(const nlohmann::json& request, const nlohmann::json& response,
                              std::uint64_t generation, Clock::time_point now,
                              bool cancelled = false);

private:
    bool running_{};
    std::size_t in_flight_{};
    std::uint64_t generation_{};
    std::uint64_t processed_{}, metadata_{}, request_errors_{}, metadata_errors_{}, changed_{},
        source_errors_{}, work_errors_{};
    Clock::time_point checkpoint_{};
    Clock::time_point last_error_{};
    bool error_reported_{};
    std::string last_operation_;
    std::string last_reason_;
    void errors(std::vector<Event>& events, Clock::time_point now, bool force);
    void stop(std::vector<Event>& events, Clock::time_point now, std::string_view reason);
    [[nodiscard]] std::string totals() const;
};

struct SharedWorkDiagnostics {
    ~SharedWorkDiagnostics();
    std::mutex mutex;
    WorkDiagnostics state;
    // Weak registry entries are reclaimed on acquisition. One fixed-size state per live catalog,
    // shared by independent windows as well as Client copies. No filesystem access is performed.
    static std::shared_ptr<SharedWorkDiagnostics> for_catalog(const std::filesystem::path& path);
};

}  // namespace rummage::photos::detail
