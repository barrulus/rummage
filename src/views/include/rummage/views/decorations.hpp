#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

#include "rummage/fs/entry.hpp"
#include "rummage/fs/sort.hpp"
#include "rummage/views/folder.hpp"

namespace rummage::views {

// A short mark at the end of an entry's row or under its grid cell ("M", "U").
struct Badge {
    // The palette role it is drawn in.
    enum class Tone : std::uint8_t { muted, accent, added, renamed, removed };

    std::string text;  // one or two characters, the same in every language
    Tone tone{Tone::muted};
    bool strong{false};  // drawn bold
    bool faded{false};   // the entry is drawn faded, as a cut one is

    bool operator==(const Badge&) const = default;
};

// A part of the status bar, right of the folder's status: text, and what the bar shows in place
// of the status while the pointer is on it (empty: nothing more).
struct StatusPart {
    std::string text;
    std::string detail;

    bool operator==(const StatusPart&) const = default;
};

// What an optional part of rummage adds to the views (the git integration): marks beside
// entries and a part of the status bar. Views and the status bar ask while they draw, so a
// decorator learns which folders are on screen and, through Folder::model().revision(), when
// their listings change. Without one nothing is asked and nothing drawn.
//
// Loop thread only. Answers stay valid until the next call.
class Decorations {
public:
    Decorations() = default;
    virtual ~Decorations() = default;

    Decorations(const Decorations&) = delete;
    Decorations& operator=(const Decorations&) = delete;
    Decorations(Decorations&&) = delete;
    Decorations& operator=(Decorations&&) = delete;

    // entry's mark in folder; nullptr when it has none.
    [[nodiscard]] virtual const Badge* badge(const Folder& folder, const fs::Entry& entry) = 0;

    // The status bar's part for folder, the one it shows; nullptr when there is none.
    [[nodiscard]] virtual const StatusPart* status(const Folder& folder) = 0;

    // Whether entries may be left out of listings at all; hides() is asked only then.
    [[nodiscard]] virtual bool hides_any() const { return false; }

    // Whether entry of folder is left out of its listing. Asked while listings are filtered: the
    // decorator's owner filters them again when the answers change.
    [[nodiscard]] virtual bool hides(const std::filesystem::path& /*folder*/,
                                     const fs::Entry& /*entry*/) {
        return false;
    }
};

// filter, also leaving out what decorations hide in folder.
[[nodiscard]] inline fs::Listing::Predicate hiding(fs::Listing::Predicate filter,
                                                   Decorations* decorations,
                                                   std::filesystem::path folder) {
    if (decorations == nullptr || !decorations->hides_any()) {
        return filter;
    }
    return [filter = std::move(filter), decorations,
            folder = std::move(folder)](const fs::Entry& entry) {
        return (!filter || filter(entry)) && !decorations->hides(folder, entry);
    };
}

}  // namespace rummage::views
