#include "rummage/fs/batch_rename.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <format>
#include <limits>
#include <map>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

#include "rummage/core/i18n.hpp"
#include "rummage/fs/operations.hpp"

namespace rummage::fs {
namespace {

std::string system_error() { return std::error_code{errno, std::generic_category()}.message(); }

std::string new_name(std::string_view original, bool folder, const RenameOptions& options,
                     std::uint64_t number, std::string& error) {
    const auto dot = original.rfind('.');
    const bool extension = !folder && dot != std::string_view::npos && dot != 0;
    std::string name{extension ? original.substr(0, dot) : original};
    const std::string_view ext = extension ? original.substr(dot) : std::string_view{};
    if (!options.find.empty()) {
        std::size_t pos = 0;
        while ((pos = name.find(options.find, pos)) != std::string::npos) {
            name.replace(pos, options.find.size(), options.replace);
            pos += options.replace.size();
            if (name.size() > 255) {
                error = std::string{i18n::tr("The replacement makes a name longer than 255 bytes")};
                return {};
            }
        }
    }
    std::string result;
    for (std::size_t pos = 0; pos < options.pattern.size();) {
        if (options.pattern[pos] != '{') {
            result += options.pattern[pos++];
        } else {
            const auto end = options.pattern.find('}', pos);
            if (end == std::string::npos) {
                error =
                    std::string{i18n::tr("Close each template token; use {name}, {ext} and {n}")};
                return {};
            }
            const auto token = std::string_view{options.pattern}.substr(pos, end - pos + 1);
            if (token == "{name}") {
                result += name;
            } else if (token == "{ext}") {
                result += ext;
            } else if (token == "{n}") {
                result += std::format("{:0{}}", number, options.padding);
            } else {
                error = std::string{i18n::tr("Use only {name}, {ext} and {n} in the template")};
                return {};
            }
            pos = end + 1;
        }
        if (result.size() > 255) {
            error = std::string{i18n::tr("The template makes a name longer than 255 bytes")};
            return {};
        }
    }
    return result;
}

}  // namespace

RenameDirectory rename_directory(const std::filesystem::path& directory) {
    RenameDirectory result;
    std::error_code error;
    std::filesystem::directory_iterator it{directory, error};
    if (error) {
        result.error = error.message();
        return result;
    }
    for (; it != std::filesystem::directory_iterator{}; it.increment(error)) {
        if (error) {
            break;
        }
        struct stat info{};
        if (lstat(it->path().c_str(), &info) != 0) {
            if (errno == ENOENT) {
                continue;
            }
            result.error = system_error();
            return result;
        }
        result.entries.push_back({.name = it->path().filename().string(),
                                  .regular = S_ISREG(info.st_mode),
                                  .directory = S_ISDIR(info.st_mode),
                                  .device = static_cast<std::uint64_t>(info.st_dev),
                                  .inode = static_cast<std::uint64_t>(info.st_ino)});
    }
    if (error) {
        result.error = error.message();
    }
    return result;
}

bool RenamePlan::changes_anything() const {
    return std::ranges::any_of(changes, [](const auto& c) { return c.from != c.to; });
}

std::string check_rename(std::span<const RenameSource> directory,
                         std::span<const RenameChange> changes) {
    std::map<std::string, const RenameSource*, std::less<>> existing;
    for (const auto& entry : directory) {
        existing.emplace(entry.name, &entry);
    }
    std::set<std::string, std::less<>> sources;
    std::set<std::string, std::less<>> targets;
    for (const auto& change : changes) {
        if (const auto problem = name_problem(change.from)) {
            return *problem;
        }
        if (const auto problem = name_problem(change.to)) {
            return std::format("{}: {}", change.from, *problem);
        }
        if (!sources.insert(change.from).second) {
            return i18n::trf("{} is selected more than once", change.from);
        }
        if (!targets.insert(change.to).second) {
            return i18n::trf("More than one file would be named {}", change.to);
        }
        const auto it = existing.find(change.from);
        if (it == existing.end()) {
            return i18n::trf("{} no longer exists", change.from);
        }
        if (it->second->device != change.device || it->second->inode != change.inode) {
            return i18n::trf("{} was replaced since the preview; reopen Batch Rename", change.from);
        }
    }
    for (const auto& change : changes) {
        if (existing.contains(change.to) && !sources.contains(change.to)) {
            return i18n::trf("{} already exists", change.to);
        }
    }
    return {};
}

RenamePlan plan_rename(std::span<const RenameSource> directory,
                       std::span<const std::string> selection, const RenameOptions& options) {
    RenamePlan plan;
    if (options.padding > 20 || options.step == 0 || options.pattern.size() > 1024 ||
        options.replace.size() > 255) {
        plan.error = std::string{i18n::tr(
            "Use a positive step, padding from 0 to 20, and a shorter template/replacement")};
        return plan;
    }
    std::map<std::string, const RenameSource*, std::less<>> entries;
    for (const auto& entry : directory) {
        entries.emplace(entry.name, &entry);
    }
    // Index every sidecar once. Looking up its possible owner avoids repeated full-directory
    // scans and preserves the rule that a duplicate with its own photo belongs to that photo.
    std::map<std::string, std::vector<std::string>, std::less<>> sidecars;
    for (const auto& candidate : directory) {
        if (!candidate.name.ends_with(".xmp")) {
            continue;
        }
        std::string owner = candidate.name.substr(0, candidate.name.size() - 4);
        if (!entries.contains(owner)) {
            const auto dot = owner.rfind('.');
            if (dot == std::string::npos || dot == 0) {
                continue;
            }
            const auto underscore = owner.rfind('_', dot);
            if (underscore == std::string::npos || underscore == 0 || dot - underscore < 3) {
                continue;
            }
            const auto number_part =
                std::string_view{owner}.substr(underscore + 1, dot - underscore - 1);
            if (!std::ranges::all_of(number_part, [](char c) { return c >= '0' && c <= '9'; })) {
                continue;
            }
            owner.erase(underscore, dot - underscore);
        }
        const auto it = entries.find(owner);
        if (it != entries.end() && it->second->regular && !owner.ends_with(".xmp")) {
            sidecars[owner].push_back(candidate.name);
        }
    }
    for (auto& [owner, followers] : sidecars) {
        std::ranges::sort(followers);
    }
    std::set<std::string, std::less<>> followers;
    for (const auto& name : selection) {
        if (const auto it = sidecars.find(name); it != sidecars.end()) {
            followers.insert(it->second.begin(), it->second.end());
        }
    }
    std::uint64_t number = options.start;
    bool overflow = false;
    const auto add = [&](const std::string& from, std::string to, bool sidecar) {
        const auto it = entries.find(from);
        if (it == entries.end()) {
            plan.error = i18n::trf("{} no longer exists", from);
            return;
        }
        plan.changes.push_back({.from = from,
                                .to = std::move(to),
                                .device = it->second->device,
                                .inode = it->second->inode,
                                .sidecar = sidecar});
    };
    for (const auto& from : selection) {
        if (followers.contains(from)) {
            continue;
        }
        if (overflow && options.pattern.find("{n}") != std::string::npos) {
            plan.error =
                std::string{i18n::tr("The numbering exceeds the largest supported number")};
            return plan;
        }
        const auto entry = entries.find(from);
        const bool folder = entry != entries.end() && entry->second->directory;
        std::string to = new_name(from, folder, options, number, plan.error);
        if (!plan.error.empty()) {
            return plan;
        }
        add(from, to, false);
        if (const auto it = sidecars.find(from); it != sidecars.end()) {
            for (const auto& sidecar : it->second) {
                add(sidecar, sidecar_name(sidecar, from, to), true);
            }
        }
        if (!plan.error.empty()) {
            return plan;
        }
        overflow = number > std::numeric_limits<std::uint64_t>::max() - options.step;
        if (!overflow) {
            number += options.step;
        }
    }
    plan.error = check_rename(directory, plan.changes);
    return plan;
}

BatchRenamed rename_batch(const std::filesystem::path& directory,
                          std::span<const RenameChange> changes) {
    BatchRenamed result;
    const RenameDirectory snapshot = rename_directory(directory);
    result.error =
        snapshot.error.empty() ? check_rename(snapshot.entries, changes) : snapshot.error;
    if (!result.error.empty()) {
        return result;
    }
    std::vector<RenameChange> work;
    for (const auto& c : changes) {
        if (c.from != c.to) {
            work.push_back(c);
        }
    }
    if (work.empty()) {
        return result;
    }
    const int parent = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parent < 0) {
        result.error = system_error();
        return result;
    }
    // mkdtemp is atomic and makes a mode-0700 directory, on the same filesystem as the sources.
    std::string temporary = (directory / ".rummage-rename-XXXXXX").string();
    if (mkdtemp(temporary.data()) == nullptr) {
        result.error = system_error();
        close(parent);
        return result;
    }
    const int stage = open(temporary.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (stage < 0) {
        result.error = system_error();
        rmdir(temporary.c_str());
        close(parent);
        return result;
    }
    // 0: original, 1: staged, 2: final. Keep the validated original basenames in staging,
    // so even an interrupted process leaves recoverable names rather than opaque numbers.
    std::vector<unsigned char> where(work.size(), 0);
    const auto move = [](int a, const std::string& from, int b, const std::string& to) {
        return renameat2(a, from.c_str(), b, to.c_str(), RENAME_NOREPLACE) == 0;
    };
    for (std::size_t i = 0; i < work.size(); ++i) {
        struct stat info{};
        if (fstatat(parent, work[i].from.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0 ||
            static_cast<std::uint64_t>(info.st_dev) != work[i].device ||
            static_cast<std::uint64_t>(info.st_ino) != work[i].inode) {
            result.error = i18n::trf("{} changed while renaming", work[i].from);
            break;
        }
        if (!move(parent, work[i].from, stage, work[i].from)) {
            result.error = i18n::trf("Cannot rename {}: {}", work[i].from, system_error());
            break;
        }
        where[i] = 1;
        // A source could be replaced between fstatat and renameat2. Keep the moved entry safe,
        // but never install it under a user-requested name when its identity differs.
        if (fstatat(stage, work[i].from.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0 ||
            static_cast<std::uint64_t>(info.st_dev) != work[i].device ||
            static_cast<std::uint64_t>(info.st_ino) != work[i].inode) {
            result.error = i18n::trf("{} changed while renaming", work[i].from);
            break;
        }
    }
    if (result.error.empty()) {
        for (std::size_t i = 0; i < work.size(); ++i) {
            if (!move(stage, work[i].from, parent, work[i].to)) {
                result.error = i18n::trf("Cannot rename {} to {}: {}", work[i].from, work[i].to,
                                         system_error());
                break;
            }
            where[i] = 2;
        }
    }
    if (result.error.empty()) {
        result.changed = work;
    } else {
        // Vacate final names before restoring originals, so cycles roll back too.
        for (std::size_t i = 0; i < work.size(); ++i) {
            if (where[i] == 2) {
                struct stat info{};
                if (fstatat(parent, work[i].to.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0 &&
                    static_cast<std::uint64_t>(info.st_dev) == work[i].device &&
                    static_cast<std::uint64_t>(info.st_ino) == work[i].inode &&
                    move(parent, work[i].to, stage, work[i].from)) {
                    where[i] = 1;
                }
            }
        }
        for (std::size_t i = 0; i < work.size(); ++i) {
            if (where[i] == 1 && move(stage, work[i].from, parent, work[i].from)) {
                where[i] = 0;
            }
            if (where[i] != 0) {
                const auto path = where[i] == 1 ? std::filesystem::path{temporary} / work[i].from
                                                : directory / work[i].to;
                result.recovery.push_back(std::format("{} → {}", work[i].from, path.string()));
            }
        }
        result.error += result.recovery.empty()
                            ? i18n::tr(". No names were changed.")
                            : i18n::tr(". Some names could not be restored; see recovery paths.");
    }
    close(stage);
    close(parent);
    rmdir(
        temporary.c_str());  // removes only an empty staging directory; never erase stranded files
    return result;
}

}  // namespace rummage::fs
