#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "rummage/core/log.hpp"

#include <array>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <syslog.h>

#include <doctest/doctest.h>

namespace {
struct Transport {
    std::string identity;
    int options{};
    int facility{};
    int priority{};
    int calls{};
    std::array<char, 4096> message{};
} transport;

struct LogLevel {
    rummage::log::Level previous = rummage::log::level();

    LogLevel() {
        rummage::log::set_level(rummage::log::Level::trace);
        transport.calls = 0;
    }

    ~LogLevel() { rummage::log::set_level(previous); }
};

void capture_message(int priority, const char* format, va_list arguments) {
    transport.priority = priority;
    ++transport.calls;
    std::vsnprintf(transport.message.data(), transport.message.size(), format, arguments);
    errno = ECONNREFUSED;
}
}  // namespace

// Linker wrapping confines the fake transport to this executable; production code
// calls the real libc syslog API without a test configuration or injectable sink.
extern "C" void __wrap_openlog(const char* identity, int options, int facility) {
    transport.identity = identity;
    transport.options = options;
    transport.facility = facility;
    errno = ENOENT;
}

extern "C" void __wrap_syslog(int priority, const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    capture_message(priority, format, arguments);
    va_end(arguments);
}

// Optimized glibc builds route syslog through this fortified entry point. Keep
// hardening enabled in the tested production code and capture both libc paths.
extern "C" void __wrap___syslog_chk(int priority, int /*flag*/, const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    capture_message(priority, format, arguments);
    va_end(arguments);
}

TEST_CASE("journal: explicit transport identifies rummage and preserves errno on failure") {
    const LogLevel reset;
    errno = EBUSY;
    rummage::log::journal(rummage::log::Level::info, "photos: checkpoint 50% %s");
    CHECK(errno == EBUSY);
    CHECK(transport.calls == 1);
    CHECK(transport.identity == "rummage");
    CHECK(transport.facility == LOG_USER);
    CHECK(transport.options == (LOG_PID | LOG_NDELAY));
    CHECK(std::string{transport.message.data()} == "photos: checkpoint 50% %s");
}

TEST_CASE("journal: configured levels filter and map to syslog priorities") {
    const LogLevel reset;
    using rummage::log::Level;
    for (const auto& [level, priority] :
         std::array{std::pair{Level::trace, LOG_DEBUG}, std::pair{Level::debug, LOG_DEBUG},
                    std::pair{Level::info, LOG_INFO}, std::pair{Level::warn, LOG_WARNING},
                    std::pair{Level::error, LOG_ERR}}) {
        rummage::log::journal(level, "photos: test priority");
        CHECK(transport.priority == (LOG_USER | priority));
    }
    CHECK(transport.calls == 5);
    rummage::log::set_level(Level::warn);
    rummage::log::journal(Level::info, "filtered");
    rummage::log::journal(Level::off, "filtered");
    CHECK(transport.calls == 5);
    rummage::log::set_level(Level::off);
    rummage::log::journal(Level::error, "filtered");
    CHECK(transport.calls == 5);
}

TEST_CASE("journal: messages are bounded single records with intact UTF-8") {
    const LogLevel reset;
    using rummage::log::Level;
    rummage::log::journal(Level::info, std::string{"one\nline\rwith\0controls", 22});
    CHECK(std::string{transport.message.data()} == "one line with controls");
    rummage::log::journal(Level::info, std::string(2048, 'x'));
    CHECK(std::string{transport.message.data()} == std::string(2048, 'x'));
    rummage::log::journal(Level::info, std::string(4096, 'x'));
    CHECK(std::string{transport.message.data()} == std::string(2045, 'x') + "...");
    rummage::log::journal(Level::info, std::string(2044, 'x') + "\xc3\xa9" + "tail");
    CHECK(std::string{transport.message.data()} == std::string(2044, 'x') + "...");
    CHECK(transport.calls == 4);
}
