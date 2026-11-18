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
#include "rummage/text/text.hpp"
#include "rummage/views/context_menu.hpp"
#include "rummage/views/folder.hpp"

namespace rummage::views {

// A short mark at the end of an entry's row or under its grid cell ("M", "U").
struct Badge {
    // What it is drawn in: a file state's colour (theme::status_colors), or subdued.
    enum class Tone : std::uint8_t {
        muted,
        added,
        modified,
        deleted,
        renamed,
        untracked,
        conflict,
    };

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

// A section a decorator adds at the end of the information pane: a heading that folds it, then
// optionally a header line, rows of label and value, a text field, a split button, buttons and
// foldable groups of entries. Texts are shown as given (translated by the decorator).
struct DetailSection {
    struct Row {
        std::string label;
        std::string value;
        std::function<void()> action;  // set: the value is a link, run on a click
    };

    struct Button {
        std::string id;  // stable, for menus and tests ("commit")
        std::string label;
        bool enabled{true};
        std::function<void()> action;  // runs on a click, the section may change after
        std::string hint;              // what it does, shown while the pointer is on it
        // Set: a click opens these as a menu under the button instead.
        std::function<std::vector<MenuItem>()> menu;
    };

    // A line under the heading: a name, a quieter subtitle, small buttons at its right.
    struct Header {
        std::string title;
        std::string subtitle;
        std::vector<Button> buttons;
        // Set: the subtitle opens these as a menu (a branch to switch to, say).
        std::function<std::vector<MenuItem>()> subtitle_menu;
    };

    // One line of text to send (Ctrl+Enter). The pane keeps what is typed; it takes text instead
    // whenever revision changes (cleared after a commit, say).
    struct Input {
        std::string placeholder;
        std::uint64_t revision{0};
        std::string text;
        std::function<void(const std::string& typed)> submit;
    };

    // A wide button and, at its right, an arrow with more ways to do it; both get what the input
    // holds.
    struct Split {
        std::string label;
        bool enabled{true};
        std::function<void(const std::string& typed)> action;
        std::function<std::vector<MenuItem>(const std::string& typed)> menu;
    };

    struct Entry {
        std::string name;
        std::string detail;  // quieter, after the name (where it is)
        Badge badge;
        std::function<void()> open;                   // a click
        std::function<std::vector<MenuItem>()> menu;  // a right click
        std::vector<Button> actions;  // small buttons shown while the pointer is on the line
    };

    // Folded until opened; entries is asked only then, so a long list costs nothing folded.
    struct Group {
        std::string id;  // stable: the pane remembers which are open by it
        std::string title;
        std::size_t count{0};
        std::function<std::vector<Entry>()> entries;
    };

    std::string title;
    std::optional<Header> header;
    std::vector<Row> rows;
    std::optional<Input> input;
    std::optional<Split> split;
    std::vector<Button> buttons;
    std::vector<Group> groups;
};

// What a decorator may ask of the window it decorates. Set by the browser that uses it.
struct DecorationHost {
    // A message in the status bar (error: in the error colour).
    std::function<void(std::string text, bool error)> message;
    // One line of text on a card: accept returns what is wrong with it (the card stays), or empty;
    // action names the button that accepts it.
    std::function<void(std::string title, std::string detail, std::string initial,
                       std::string action, std::function<std::string(const std::string&)> accept)>
        ask_text;
    // A question before something that cannot be undone; action names its button, warning is
    // the line under the question.
    std::function<void(std::string title, std::string detail, std::string action,
                       std::string warning, std::function<void(bool confirmed)> answer)>
        confirm;
    // Moves files to the trash, as Delete does (undoable).
    std::function<void(std::vector<std::filesystem::path> paths)> trash;
    // Shows text in Quick Look (a file's changes), titled title; step(+1/-1) runs for the arrow
    // keys there, to show the next or previous one.
    std::function<void(std::string title, text::Text text, std::function<void(int)> step)>
        show_text;
    // Opens a file in its default application, as Enter does.
    std::function<void(const std::filesystem::path& file)> open_file;
    // Shows target in the browser, its folders opened from root and it selected.
    std::function<void(const std::filesystem::path& root, const std::filesystem::path& target)>
        reveal;
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

    // Where entry of folder sorts by status (SortKey::mark), lower first; asked while sorting.
    [[nodiscard]] virtual int rank(const std::filesystem::path& /*folder*/,
                                   const fs::Entry& /*entry*/) {
        return 0;
    }

    // Whether entries may be left out of listings at all; hides() is asked only then.
    [[nodiscard]] virtual bool hides_any() const { return false; }

    // Whether entry of folder is left out of its listing. Asked while listings are filtered: the
    // decorator's owner filters them again when the answers change.
    [[nodiscard]] virtual bool hides(const std::filesystem::path& /*folder*/,
                                     const fs::Entry& /*entry*/) {
        return false;
    }
};

// The ranks decorations give the entries of folder; empty without decorations.
[[nodiscard]] inline std::function<int(const fs::Entry&)> ranking(Decorations* decorations,
                                                                  std::filesystem::path folder) {
    if (decorations == nullptr) {
        return {};
    }
    return [decorations, folder = std::move(folder)](const fs::Entry& entry) {
        return decorations->rank(folder, entry);
    };
}

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
