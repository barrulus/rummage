#include <algorithm>
#include <cstddef>
#include <iterator>
#include <sys/stat.h>

#include "rummage/fs/sort.hpp"
#include "rummage/preview/preview.hpp"

namespace rummage::preview {

Archive read_archive(const std::filesystem::path& path,
                     const std::shared_ptr<const fs::MimeDatabase>& types, std::size_t top) {
    const std::shared_ptr<const fs::ArchiveTree> tree = fs::archive_tree(path, types.get());
    Archive out;
    out.format = tree->format();
    out.failure = tree->failure();
    out.detail = tree->detail();
    out.totals = tree->totals();
    out.left_out = tree->left_out();
    if (const std::vector<fs::Entry>* const entries = tree->folder(""); entries != nullptr) {
        out.top_count = entries->size();
        // Folders first, then by name, as a folder shows them.
        std::vector<const fs::Entry*> order;
        order.reserve(entries->size());
        for (const fs::Entry& entry : *entries) {
            order.push_back(&entry);
        }
        const std::size_t count = std::min(top, order.size());
        std::ranges::partial_sort(order, order.begin() + static_cast<std::ptrdiff_t>(count),
                                  [](const fs::Entry* a, const fs::Entry* b) {
                                      if (a->is_directory() != b->is_directory()) {
                                          return a->is_directory();
                                      }
                                      return fs::natural_compare(a->name, b->name) < 0;
                                  });
        out.top.reserve(count);
        std::transform(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(count),
                       std::back_inserter(out.top), [](const fs::Entry* entry) { return *entry; });
    }
    return out;
}

}  // namespace rummage::preview
