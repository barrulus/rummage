#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <system_error>

#include "rummage/fs/permissions.hpp"

namespace rummage::fs {

// Portable-to-xattr-aware-tools metadata. Rating is Rummage's 0..5 value, never a photo sidecar.
struct Annotations {
    std::string tags, rating, comment;
    std::string error;
    bool operator==(const Annotations&) const = default;
};

inline constexpr unsigned kAnnotationTags = 1, kAnnotationRating = 2, kAnnotationComment = 4;
// Blocking, bounded reads. fd may be O_PATH; links/special files are rejected.
[[nodiscard]] Annotations read_annotations(int fd);
[[nodiscard]] Annotations read_annotations(const std::filesystem::path& path);
// Directory workers read just the rating for browsing; absent ratings are zero, unavailable
// or malformed ratings are null. No filesystem access is needed while rendering or sorting.
[[nodiscard]] std::optional<unsigned> read_rating(int fd);
// Only selected fields change. Refuse replacement roots and changed selected annotations.
// No links, elevated privileges, sidecar files, or hidden fallback database.
[[nodiscard]] std::string write_annotations(const PermissionTarget& target,
                                            const Annotations& before, Annotations values,
                                            unsigned fields);
// Copy only the three annotation xattrs between already pinned descriptors. An annotated source
// copied to a filesystem without xattrs fails explicitly, so a move cannot silently lose them.
[[nodiscard]] std::error_code copy_annotations(int source, int destination);

}  // namespace rummage::fs
