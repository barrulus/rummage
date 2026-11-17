#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "rummage/fs/entry.hpp"
#include "rummage/fs/sort.hpp"
#include "rummage/views/context_menu.hpp"
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
    bool tint{false};    // the entry's name is drawn in the badge's tone too

    bool operator==(const Badge&) const = default;
};

// A part of the status bar, right of the folder's status: lead and text, and what the bar shows in
// place of the status while the pointer is on it (empty: nothing more).
struct StatusPart {
    std::string lead;  // drawn first, in the accent colour ("main ↑1")
    std::string text;
    std::string detail;

    bool operator==(const StatusPart&) const = default;
};

// A section a decorator adds at the end of the information pane: a heading that folds it, rows of
// label and value, and buttons. Texts are shown as given (translated by the decorator).
struct DetailSection {
    struct Row {
        std::string label;
        std::string value;
    };

    struct Button {
        std::string id;  // stable, for menus and tests ("commit")
        std::string label;
        bool enabled{true};
        std::function<void()> action;  // runs on a click, the section may change after
    };

    std::string title;
    std::vector<Row> rows;
    std::vector<Button> buttons;
};

// What a decorator may ask of the window it decorates. Set by the browser that uses it.
struct DecorationHost {
    // A message in the status bar (error: in the error colour).
    std::function<void(std::string text, bool error)> message;
    // One line of text on a card: accept returns what is wrong with it (the card stays), or empty.
    std::function<void(std::string title, std::string detail, std::string initial,
                       std::function<std::string(const std::string&)> accept)>
        ask_text;
    // A question before something that cannot be undone; action names its button, warning is
    // the line under the question.
    std::function<void(std::string title, std::string detail, std::string action,
                       std::string warning, std::function<void(bool confirmed)> answer)>
        confirm;
    // Moves files to the trash, as Delete does (undoable).
    std::function<void(std::vector<std::filesystem::path> paths)> trash;
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

    // The browser's services; called again when the decorator is set.
    virtual void set_host(DecorationHost /*host*/) {}

    // A submenu for the context menu of names in folder (empty: the folder's background);
    // nullopt for none. Its items' actions run once the menu has closed.
    [[nodiscard]] virtual std::optional<MenuItem> menu(const Folder& /*folder*/,
                                                       const std::vector<std::string>& /*names*/) {
        return std::nullopt;
    }

    // The information pane's section for folder (what it shows); nullptr for none.
    [[nodiscard]] virtual const DetailSection* details(const Folder& /*folder*/) { return nullptr; }

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
