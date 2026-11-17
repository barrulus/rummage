// Fuzzes the git integration's parsers with what `git status --porcelain=v2 -z --branch` or
// `git log -1` might print, and what rummage makes of a parsed status: badges, meanings and the
// status bar's summary. The first byte picks the parser.
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "rummage/git/decorator.hpp"
#include "rummage/git/status.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    namespace git = rummage::git;
    const std::string_view input{reinterpret_cast<const char*>(data) + 1, size - 1};
    if ((data[0] & 1U) != 0) {
        if (const auto commit = git::parse_commit(input)) {
            (void)git::commit_line(*commit, commit->time + data[0]);
        }
        return 0;
    }
    if (const auto status = git::parse_status(input)) {
        for (const git::FileStatus& file : status->files) {
            (void)git::badge_of(file);
            (void)git::meaning_of(file);
        }
        (void)git::summary_of(*status, static_cast<git::Operation>(data[0] % 6U), "abc1234");
    }
    return 0;
}
