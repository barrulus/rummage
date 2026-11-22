#include "rummage/fs/group.hpp"

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rummage/core/i18n.hpp"
#include "rummage/fs/operations.hpp"

namespace rummage::fs {
namespace {
bool matches(const struct stat& info, const GroupFolder& folder) {
    return S_ISDIR(info.st_mode) && info.st_dev == folder.device && info.st_ino == folder.inode;
}
}  // namespace

TransferResult run_group(const TransferRequest& request, const core::CancelToken& token,
                         const TransferCallbacks& callbacks) {
    TransferResult result;
    if (token.cancelled()) {
        result.cancelled = true;
        return result;
    }
    const auto path = request.ungroup ? request.source : request.destination;
    if (auto error = name_problem(path.filename().string())) {
        result.refused = *error;
        return result;
    }
    bool created = false;
    struct stat info{};
    int fd = -1;
    if (!request.ungroup && ::lstat(path.c_str(), &info) != 0 && errno == ENOENT) {
        std::string staging = (path.parent_path() / ".rummage-group-XXXXXX").string();
        if (::mkdtemp(staging.data())) {
            fd = ::open(staging.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (fd >= 0 && ::fstat(fd, &info) == 0 &&
                ::renameat2(AT_FDCWD, staging.c_str(), AT_FDCWD, path.c_str(), RENAME_NOREPLACE) ==
                    0)
                created = true;
            else {
                if (fd >= 0)
                    ::close(fd);
                fd = -1;
                (void)::rmdir(staging.c_str());
            }
        }
    }
    if (!created) {
        if (::lstat(path.c_str(), &info) != 0 || !matches(info, request.group_folder)) {
            result.refused = std::string{
                i18n::tr("The folder already exists or has changed; nothing was moved")};
            return result;
        }
        fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0 || ::fstat(fd, &info) != 0 || !matches(info, request.group_folder)) {
            if (fd >= 0)
                ::close(fd);
            result.refused = std::string{i18n::tr("Cannot open the new folder safely")};
            return result;
        }
    }
    const GroupFolder identity{path, info.st_dev, info.st_ino};
    auto transfer = request;
    transfer.kind = TransferKind::move;
    transfer.make_destination = false;
    const std::filesystem::path pinned = "/proc/self/fd/" + std::to_string(fd);
    if (request.ungroup)
        transfer.source = pinned;
    else
        transfer.destination = pinned;
    result = run_transfer(transfer, token, callbacks);
    result.group_folder = identity;
    for (auto& item : result.items) {
        if (request.ungroup)
            item.from = path / item.from.lexically_relative(pinned);
        else
            item.to = path / item.to.lexically_relative(pinned);
    }
    struct stat current{};
    if (::lstat(path.c_str(), &current) == 0 && matches(current, identity)) {
        // rmdir refuses any remaining or newly added contents. Never recursively delete it.
        if (request.ungroup || result.items.empty())
            (void)::rmdir(path.c_str());
    } else if (!result.items.empty()) {
        result.errors.push_back(
            std::string{i18n::tr("The folder moved during the operation; undo is unavailable")});
        result.undo_excluded += result.items.size();
        result.items.clear();
    }
    ::close(fd);
    return result;
}
}  // namespace rummage::fs
