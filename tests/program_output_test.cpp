#include "rummage/platform/program_output.hpp"

#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>
#include <sys/wait.h>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/platform/desktop_entry.hpp"

using rummage::core::EventLoop;
using rummage::platform::ProgramOutput;
using namespace std::chrono_literals;

namespace {

// What sh -c script printed, with options; nullopt when it did not end in 5 s.
std::optional<std::string> run_shell(const std::string& script, ProgramOutput::Options options) {
    const char* const path = std::getenv("PATH");
    const auto shell = rummage::platform::find_program("sh", path != nullptr ? path : "/bin");
    REQUIRE(shell.has_value());
    EventLoop loop;
    std::optional<std::string> result;
    ProgramOutput program{loop, [&](const std::string& output, int status) {
                              CHECK(WIFEXITED(status));
                              result = output;
                              loop.quit(0);
                          }};
    REQUIRE(program.start(*shell, {"-c", script}, options));
    loop.add_timer(5s, 0ms, [&loop] { loop.quit(1); });
    loop.run();
    return result;
}

}  // namespace

TEST_CASE("program output: standard error only when asked, and added environment") {
    const std::string script = "echo out; echo err >&2; echo \"$RUMMAGE_TEST_VALUE\"";
    CHECK(run_shell(script, {}) == "out\n\n");
    CHECK(run_shell(script, {.merge_stderr = true, .environment = {"RUMMAGE_TEST_VALUE=x y"}}) ==
          "out\nerr\nx y\n");
    // A variable rummage has is replaced, not doubled.
    setenv("RUMMAGE_TEST_VALUE", "old", 1);
    CHECK(run_shell("echo \"$RUMMAGE_TEST_VALUE\"", {.environment = {"RUMMAGE_TEST_VALUE=new"}}) ==
          "new\n");
    CHECK(run_shell("echo \"$RUMMAGE_TEST_VALUE\"", {}) == "old\n");
    unsetenv("RUMMAGE_TEST_VALUE");
}
