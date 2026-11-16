#include "rummage/views/undo.hpp"

#include <format>
#include <sys/stat.h>

#include "rummage/core/i18n.hpp"

namespace rummage::views {

std::string describe(const FileOperation& operation) {
    const std::size_t count = operation.items.size();
    std::string what = count == 1
                           ? std::format("“{}”", operation.items.front().to.filename().string())
                           : i18n::trf("{} items", count);
    switch (operation.kind) {
        case FileOperation::Kind::rename:
            if (count == 1) {
                return i18n::trf("“{}” is “{}” again",
                                 operation.items.front().from.filename().string(),
                                 operation.items.front().to.filename().string());
            }
            break;
        case FileOperation::Kind::trash:
            if (count == 1) {
                return i18n::trf("“{}” moved to the trash",
                                 operation.items.front().from.filename().string());
            }
            return i18n::trf("{} moved to the trash", what);
        case FileOperation::Kind::restore:
            return i18n::trf("{} restored", what);
        case FileOperation::Kind::copy:
        case FileOperation::Kind::move:
            if (count > 0) {
                return i18n::trf("{} back in {}", what,
                                 operation.items.front().to.parent_path().filename().string());
            }
            break;
        case FileOperation::Kind::create:
            return i18n::trf("{} made", what);
        case FileOperation::Kind::erase:
            break;
        case FileOperation::Kind::permissions: {
            const bool runs =
                !operation.modes.empty() && (operation.modes.front().after & S_IXUSR) != 0;
            if (count == 1) {
                return runs ? i18n::trf("{} can run as a program", what)
                            : i18n::trf("{} can no longer run as a program", what);
            }
            return runs ? i18n::trf("{} can run as programs", what)
                        : i18n::trf("{} can no longer run as programs", what);
        }
    }
    return what;
}

}  // namespace rummage::views
