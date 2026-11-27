#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory_model.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"

namespace rummage::views {

// How moving the cursor changes the selection.
enum class SelectMode : std::uint8_t {
    replace,  // select only the new entry (plain click, arrows)
    extend,   // select from the anchor to the new entry (Shift)
    keep,     // leave the selection as it is (Ctrl+arrows)
    toggle,   // add or remove the new entry, which becomes the anchor (Ctrl+click)
};

// What a selection holds (the info pane's summary).
struct SelectionSummary {
    std::size_t folders{0};
    std::size_t files{0};    // everything that is not a folder
    std::uint64_t bytes{0};  // sizes of the files; folders are not counted
};

// One directory as the views see it: its listing, a cursor and a selection, and the types of its
// entries sniffed so far. It does not draw: a column or the grid shows it, and switching between
// them hands the same Folder over, so cursor and selection stay.
//
// The cursor is the entry the keyboard is on; the selection is a set of entries, usually just the
// cursor's. Both are kept by stable key (the filename in ordinary directories), so they follow
// their entries when rows move or are renamed, and lose them when they are deleted. The cursor can
// name an entry that has not been loaded yet (pending): it lands when the entry arrives, or is
// dropped when the load ends without it.
//
// Where the cursor sits on screen is the view's business: a Folder only says, through
// take_reveal(), that the cursor moved and how the view should bring it into sight.
//
// Loop thread only. Must not outlive the loop, pool or watcher.
class Folder {
public:
    // How the view should bring the cursor into sight at its next draw.
    enum class Reveal : std::uint8_t {
        none,
        glide,  // moved by the user: just enough to show it, animated
        jump,   // landed or asked for by name: centered, at once
    };

    // redraw runs when anything shown changes. show_hidden: dotfiles are listed.
    Folder(core::EventLoop& loop, core::WorkerPool& pool, fs::DirectoryWatcher& watcher,
           std::shared_ptr<const fs::MimeDatabase> mime, std::filesystem::path directory,
           std::function<void()> redraw, bool show_hidden = true);

    // An externally supplied collection: no directory is opened or watched. The controller
    // owns paging, filtering and sorting; snapshots keep their supplied order.
    Folder(core::EventLoop& loop, core::WorkerPool& pool, fs::DirectoryWatcher& watcher,
           std::shared_ptr<const fs::MimeDatabase> mime, std::function<void()> redraw);

    void replace_collection(std::vector<fs::Entry> entries, std::string label, std::size_t total,
                            bool loading = false);

    // Preserve stable row keys/order while publishing metadata or a later query page.
    // An invalid patch is rejected atomically; it cannot move the cursor to another photo.
    bool update_collection_rows(std::vector<std::pair<std::size_t, fs::Entry>> rows);
    void append_collection(std::vector<fs::Entry> rows, std::size_t total, bool loading = false);

    using VisibleRangeHandler = std::function<void(std::size_t first, std::size_t last_exclusive)>;

    // Collection controllers may load more rows as the native view reaches them. Views report
    // after drawing, when they no longer hold entry references; ordinary directories stay silent.
    void set_visible_range_handler(VisibleRangeHandler handler) {
        visible_range_handler_ = std::move(handler);
    }

    void notify_visible_range(std::size_t first, std::size_t last_exclusive) const;

    [[nodiscard]] bool is_collection() const noexcept { return collection_; }

    [[nodiscard]] const std::string& collection_label() const noexcept { return collection_label_; }

    [[nodiscard]] std::size_t total_count() const noexcept {
        return collection_ ? collection_total_ : entries().size();
    }

    [[nodiscard]] bool loading() const noexcept {
        return collection_ ? collection_loading_ : model_.loading();
    }

    [[nodiscard]] std::uint64_t revision() const noexcept {
        return collection_ ? collection_revision_ : model_.revision();
    }

    ~Folder();

    Folder(const Folder&) = delete;
    Folder& operator=(const Folder&) = delete;
    Folder(Folder&&) = delete;
    Folder& operator=(Folder&&) = delete;

    // changed runs when the cursor or the selection changes, lands, loses its entry, or that
    // entry changes. One handler: the view that owns the Folder's place sets it.
    void set_changed_handler(std::function<void()> changed) { changed_ = std::move(changed); }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return model_.path(); }

    [[nodiscard]] const fs::DirectoryModel& model() const noexcept { return model_; }

    void refresh_entry(const std::string& name) {
        if (!collection_)
            model_.refresh_entry(name);
    }

    // Where entry really is: path() / name, or in the trash folder its place in a trash.
    [[nodiscard]] std::filesystem::path path_of(const fs::Entry& entry) const;

    // The trash folder (fs::trash_path()): nothing is made, pasted or renamed in it.
    [[nodiscard]] bool is_trash() const noexcept { return model_.is_trash(); }

    // A folder inside an archive (0.9.0): read-only.
    [[nodiscard]] bool is_archive() const noexcept { return model_.is_archive(); }

    [[nodiscard]] const std::vector<fs::Entry>& entries() const noexcept {
        return collection_ ? collection_entries_ : model_.listing().entries();
    }

    // The order of the entries, the same in every view that shows this folder.
    [[nodiscard]] const fs::SortOrder& order() const noexcept {
        return collection_ ? collection_order_ : model_.listing().order();
    }

    // Sorts the entries again; the cursor and the selection stay on theirs, and the view jumps
    // to the cursor.
    void set_order(const fs::SortOrder& order);

    void set_entry_filter(fs::Listing::Predicate filter) {
        if (!collection_)
            model_.set_predicate(std::move(filter));
    }

    // The entry under the cursor; nullptr when there is none or it is still pending.
    [[nodiscard]] const fs::Entry* cursor_entry() const;

    [[nodiscard]] std::optional<std::size_t> cursor() const noexcept { return cursor_index_; }

    // A cursor was asked for by name and its entry has not arrived yet.
    [[nodiscard]] bool cursor_pending() const noexcept {
        return !cursor_name_.empty() && !cursor_index_;
    }

    // Puts the cursor on an entry (clamped) and updates the selection as mode says. nullopt
    // removes the cursor and clears the selection.
    void set_cursor(std::optional<std::size_t> index, SelectMode mode = SelectMode::replace);

    // A click with mode at index: as set_cursor, except that a plain click on an entry selected
    // with others keeps them selected until release_press(), so the press can drag them all.
    void press(std::optional<std::size_t> index, SelectMode mode);

    // The button went up where it went down: the selection becomes the pressed entry alone, if
    // a press left that for later.
    void release_press();

    // A drag took the press: the selection stays as it is.
    void forget_press() noexcept { press_pending_ = false; }

    // A rubber band: the selection becomes base and the entries at rows (indices), the cursor
    // stays where it is.
    void select_band(const std::vector<std::string>& base, std::span<const std::size_t> rows);

    // Moves the cursor by delta entries (to the first entry when there is none).
    void move_cursor(std::ptrdiff_t delta, SelectMode mode = SelectMode::replace,
                     bool wrap = false);

    // Puts the cursor on the entry with this stable key and selects it, now or when it arrives.
    void select_name(std::string name);

    // The same for an entry rummage has just made: it is not listed yet, and the cursor waits for
    // the watcher to report it (instead of giving up at the next change), until it lands or the
    // cursor is moved.
    void expect_name(std::string name);

    // The same for several (pasted files): the cursor on the first, all of them selected.
    void expect_names(const std::vector<std::string>& names);

    void select_all();

    // The entries shown that are not selected become the selection (Ctrl+Shift+I). The cursor
    // stays; with none, or one waiting for its entry, it goes to the first entry selected.
    void invert_selection();

    // The entries shown whose names match a literal substring or wildcard pattern, case aside, as
    // indices in listing order.
    [[nodiscard]] std::vector<std::size_t> matching(const std::string& glob) const;

    // The entries at rows (indices in listing order) become the selection, or are added to it
    // with add. The cursor stays on its entry if that is one of them, else it goes to the first;
    // a Shift range starts there.
    void select_rows(std::span<const std::size_t> rows, bool add);

    // Lists the hidden entries (dotfiles) or leaves them out; left out, they leave the selection.
    // A cursor on one moves to the nearest entry shown (after it, else before), unless
    // keep_cursor: then that entry stays (the way to a folder opened further on). An entry the
    // cursor is put on by name (select_name, expect_name) is shown even when hidden.
    void set_show_hidden(bool show, bool keep_cursor = false);

    [[nodiscard]] bool shows_hidden() const noexcept {
        return collection_ || model_.listing().shows_hidden();
    }

    // The entries left out.
    [[nodiscard]] std::size_t hidden_count() const noexcept {
        return collection_ ? 0 : model_.listing().hidden().size();
    }

    // Back to the cursor's entry alone (Escape).
    void collapse_selection();

    [[nodiscard]] std::size_t selected_count() const noexcept { return selected_.size(); }

    [[nodiscard]] std::uint64_t selection_revision() const noexcept { return selection_revision_; }

    [[nodiscard]] bool is_selected(const std::string& name) const {
        return selected_.contains(name);
    }

    // The selected stable keys, in listing order (ordinary directory keys are names).
    [[nodiscard]] std::vector<std::string> selected_names() const;

    // Borrowed rows remain valid until the next collection snapshot or directory update.
    [[nodiscard]] std::vector<const fs::Entry*> selected_entries() const;
    // Logical rows with no original path are omitted.
    [[nodiscard]] std::vector<std::filesystem::path> selected_paths() const;

    // The entry under the cursor when it is the whole selection; nullptr otherwise.
    [[nodiscard]] const fs::Entry* single_selection() const;

    [[nodiscard]] const SelectionSummary& summary() const;

    // How the view should show the cursor, once: the request is cleared.
    [[nodiscard]] Reveal take_reveal() noexcept;

    // Asks the view to jump to the cursor at its next draw (a view that starts showing it).
    void request_jump() noexcept { reveal_ = Reveal::jump; }

    // The entry's type: from the name, else sniffed from the content, else unknown (yet).
    [[nodiscard]] fs::MimeId mime_of(const fs::Entry& entry) const;

    [[nodiscard]] const std::shared_ptr<const fs::MimeDatabase>& shared_mime() const {
        return mime_;
    }

    [[nodiscard]] const fs::MimeDatabase& mime() const noexcept { return *mime_; }

    // The entry is a regular file of a type that runs: a program, an AppImage, a script
    // (application/x-executable and its kin, application/x-sharedlib), or a type not known yet.
    [[nodiscard]] bool type_runs(const fs::Entry& entry) const;

    // The entry may run as a program: type_runs, and its owner's execute bit is set. Never in
    // the trash. The views mark it with a dot on its icon (issue #17).
    [[nodiscard]] bool runs_as_program(const fs::Entry& entry) const;

    // Sniffs the types of entries [first, last) that the name does not decide, on a worker (the
    // view asks for what it shows).
    void sniff(std::size_t first, std::size_t last);

private:
    void on_change(const fs::ModelChange& change);
    void selection_changed(bool feedback = true);

    core::WorkerPool& pool_;
    std::shared_ptr<const fs::MimeDatabase> mime_;
    std::function<void()> redraw_;
    std::function<void()> changed_;
    VisibleRangeHandler visible_range_handler_;
    fs::DirectoryModel model_;
    bool collection_{false};
    bool collection_loading_{false};
    std::string collection_label_;
    std::size_t collection_total_{0};
    std::uint64_t collection_revision_{0};
    std::vector<fs::Entry> collection_entries_;
    fs::SortOrder collection_order_;

    std::string cursor_name_;                  // empty: no cursor
    std::optional<std::size_t> cursor_index_;  // nullopt while pending
    bool cursor_expected_{false};              // pending on an entry rummage made
    bool press_pending_{false};                // release_press() selects the cursor's alone
    Reveal reveal_{Reveal::none};
    std::unordered_set<std::string> selected_;
    std::uint64_t selection_revision_{0};
    std::string anchor_;                               // where a Shift range starts
    mutable std::optional<SelectionSummary> summary_;  // computed on demand

    // Types sniffed from content, by name.
    std::unordered_map<std::string, fs::MimeId> sniffed_;
    std::unordered_set<std::string> sniffing_;
    // False once destroyed: sniff results still on their way find out here.
    std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
};

}  // namespace rummage::views
