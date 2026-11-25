#include "rummage/views/insights_view.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <map>
#include <numeric>
#include <ranges>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "rummage/core/i18n.hpp"
#include "rummage/fs/icon.hpp"
#include "rummage/fs/name_pattern.hpp"
#include "rummage/fs/sort.hpp"
#include "rummage/views/format.hpp"

namespace rummage::views {
namespace {
std::string age_label(std::size_t age) {
    switch (static_cast<fs::InsightAge>(age)) {
        case fs::InsightAge::day:
            return std::string{i18n::tr("Last 24 hours")};
        case fs::InsightAge::week:
            return std::string{i18n::tr("1–7 days")};
        case fs::InsightAge::month:
            return std::string{i18n::tr("7–30 days")};
        case fs::InsightAge::year:
            return std::string{i18n::tr("30–365 days")};
        case fs::InsightAge::older:
            return std::string{i18n::tr("Over a year")};
        case fs::InsightAge::future:
            return std::string{i18n::tr("Future dates")};
        default:
            return {};
    }
}

std::string extension_label(const std::string& extension) {
    return extension.empty() ? std::string{i18n::tr("No extension")} : extension;
}

ui::Color mix(ui::Color a, ui::Color b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1};
}

}  // namespace

std::vector<ui::Rect> insight_treemap(const std::vector<double>& weights, ui::Rect bounds) {
    std::vector<ui::Rect> result(weights.size());
    std::vector<std::size_t> ids;
    for (std::size_t i = 0; i < weights.size(); ++i)
        if (std::isfinite(weights[i]) && weights[i] > 0)
            ids.push_back(i);
    if (bounds.empty() || !std::isfinite(bounds.width) || !std::isfinite(bounds.height))
        return result;
    const auto divide = [&](auto&& self, std::size_t first, std::size_t end,
                            ui::Rect rect) -> void {
        if (first == end)
            return;
        if (end - first == 1) {
            result[ids[first]] = rect;
            return;
        }
        long double total = 0;
        for (auto i = first; i < end; ++i)
            total += weights[ids[i]];
        auto middle = first + 1;
        long double sum = weights[ids[first]];
        while (middle + 1 < end && sum + weights[ids[middle]] / 2 < total / 2)
            sum += weights[ids[middle++]];
        const float share = static_cast<float>(sum / total);
        auto other = rect;
        if (rect.width >= rect.height) {
            rect.width *= share;
            other.x += rect.width;
            other.width -= rect.width;
        } else {
            rect.height *= share;
            other.y += rect.height;
            other.height -= rect.height;
        }
        self(self, first, middle, rect);
        self(self, middle, end, other);
    };
    divide(divide, 0, ids.size(), bounds);
    return result;
}

InsightsView::InsightsView(core::EventLoop& loop, core::WorkerPool& pool,
                           std::function<void()> redraw, fs::InsightOptions options)
    : pool_{pool}, source_{loop, pool}, redraw_{std::move(redraw)}, options_{options} {}

InsightsView::~InsightsView() {
    clear_selection();
    stop_.cancel();
    job_.cancel();
}

void InsightsView::set_browser_handlers(FolderFactory factory, std::function<void()> activate,
                                        std::function<void(float, float)> menu, EditSlot* edit) {
    folder_factory_ = std::move(factory);
    activate_file_ = std::move(activate);
    menu_ = std::move(menu);
    edit_ = edit;
}

void InsightsView::clear_selection() {
    if (selected_folder_)
        selected_folder_->set_changed_handler({});
    selected_folder_.reset();
    selected_row_.reset();
    selected_path_.clear();
    pending_open_ = false;
    pending_menu_.reset();
}

void InsightsView::sync_folder() {
    if (folder_factory_ && (!scope_folder_ || scope_folder_->path() != scope_path()))
        scope_folder_ = folder_factory_(scope_path());
}

bool InsightsView::file_row(std::size_t index) const {
    return index < rows_.size() && !rows_[index].directory &&
           (view_ == View::matches || view_ == View::largest || view_ == View::age_matches ||
            view_ == View::code_matches || view_ == View::space || view_ == View::iris);
}

std::filesystem::path InsightsView::row_path(std::size_t index) const {
    if (!file_row(index))
        return {};
    const auto& row = rows_[index];
    if (row.path.is_absolute())
        return row.path;
    if (view_ == View::code_matches && code_)
        return code_->root / row.path;
    return data_ ? data_->root / data_->relative_path(row.node) : std::filesystem::path{};
}

bool InsightsView::selected(std::size_t index) const {
    const auto path = row_path(index);
    return selected_folder_ && path.parent_path() == selected_folder_->path() &&
           selected_folder_->is_selected(path.filename().string());
}

void InsightsView::select_row(std::size_t index, ui::Modifiers modifiers) {
    if (!file_row(index) || !folder_factory_)
        return;
    const auto path = row_path(index);
    const auto previous = selected_row_;
    const bool same_parent = selected_folder_ && selected_folder_->path() == path.parent_path();
    if (!same_parent) {
        clear_selection();
        selected_folder_ = folder_factory_(path.parent_path());
        selected_folder_->set_changed_handler([this] { selection_changed(); });
    }
    selected_row_ = index;
    selected_path_ = path;
    pending_open_ = false;
    pending_menu_.reset();
    focus_ = std::format("row:{}", index);
    const auto& entries = selected_folder_->entries();
    const auto found = std::ranges::find(entries, path.filename().string(), &fs::Entry::name);
    if (same_parent && modifiers.shift && previous) {
        std::vector<std::size_t> matches;
        for (auto i = std::min(index, *previous); i <= std::max(index, *previous); ++i) {
            const auto candidate = row_path(i);
            if (candidate.parent_path() != path.parent_path())
                continue;
            const auto at =
                std::ranges::find(entries, candidate.filename().string(), &fs::Entry::name);
            if (at != entries.end())
                matches.push_back(static_cast<std::size_t>(at - entries.begin()));
        }
        selected_folder_->select_rows(matches, modifiers.ctrl);
        if (found != entries.end())
            selected_folder_->set_cursor(static_cast<std::size_t>(found - entries.begin()),
                                         SelectMode::keep);
    } else if (same_parent && modifiers.ctrl && found != entries.end()) {
        selected_folder_->set_cursor(static_cast<std::size_t>(found - entries.begin()),
                                     SelectMode::toggle);
    } else
        selected_folder_->select_name(path.filename().string());
    redraw_();
}

void InsightsView::selection_changed() {
    if (!selected_folder_)
        return;
    const auto* entry = selected_folder_->cursor_entry();
    if (entry && selected_row_ && *selected_row_ < rows_.size()) {
        const auto path = selected_folder_->path_of(*entry);
        if (path != selected_path_ &&
            std::ranges::find(selected_folder_->entries(), selected_path_.filename().string(),
                              &fs::Entry::name) == selected_folder_->entries().end()) {
            // Folder's watched model follows a rename; keep its result row in place.
            for (auto& [original, target] : renamed_paths_)
                if (target == selected_path_)
                    target = path;
            renamed_paths_[selected_path_] = path;
            auto& row = rows_[*selected_row_];
            row.path = path;
            row.label = view_ == View::code_matches ? path.lexically_relative(scope_path()).string()
                                                    : entry->name;
            row.bytes = entry->size;
            invalidate_iris();
            selected_path_ = path;
        }
        if (pending_open_) {
            pending_open_ = false;
            if (activate_file_)
                activate_file_();
            return;
        }
        if (pending_menu_) {
            const auto [x, y] = *pending_menu_;
            pending_menu_.reset();
            if (menu_)
                menu_(x, y);
            return;
        }
    } else if (!selected_folder_->model().loading()) {
        pending_open_ = false;
        pending_menu_.reset();
    }
    redraw_();
}

void InsightsView::select_all(bool invert) {
    if (!selected_folder_ || selected_folder_->model().loading())
        return;
    const auto& entries = selected_folder_->entries();
    std::vector<std::size_t> matches;
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        const auto path = row_path(i);
        if (path.parent_path() != selected_folder_->path())
            continue;
        const auto found = std::ranges::find(entries, path.filename().string(), &fs::Entry::name);
        if (found != entries.end() && (!invert || !selected_folder_->is_selected(found->name)))
            matches.push_back(static_cast<std::size_t>(found - entries.begin()));
    }
    if (matches.empty())
        selected_folder_->set_cursor(std::nullopt);
    else
        selected_folder_->select_rows(matches, false);
}

std::vector<InsightsView::SortColumn> InsightsView::sort_columns() const {
    std::vector<SortColumn> columns{
        {SortKey::name, "name",
         view_ == View::languages                               ? i18n::mark("Language")
         : (view_ == View::types || view_ == View::other_types) ? i18n::mark("Extension")
                                                                : i18n::mark("Name")},
        {SortKey::files, "files", i18n::mark("Files")}};
    if (source_view()) {
        columns.push_back({SortKey::lines, "lines", i18n::mark("Lines")});
        columns.push_back({SortKey::code, "code", i18n::mark("Code")});
        columns.push_back({SortKey::comments, "comments", i18n::mark("Comments")});
        columns.push_back({SortKey::blanks, "blanks", i18n::mark("Blanks")});
    } else {
        columns.push_back({SortKey::bytes, "bytes", i18n::mark("File size")});
        if (view_ == View::types || view_ == View::other_types || view_ == View::age ||
            view_ == View::largest)
            columns.push_back({SortKey::share, "share", i18n::mark("Share")});
    }
    return columns;
}

std::optional<InsightsView::SortKey> InsightsView::sort_key() const {
    if (sort_) {
        const auto columns = sort_columns();
        if (std::ranges::find(columns, *sort_, &SortColumn::key) != columns.end())
            return sort_;
    }
    if (view_ == View::age)
        return {};  // Initially show age buckets chronologically.
    if (view_ == View::languages)
        return SortKey::name;
    if (view_ == View::code_matches)
        return SortKey::lines;
    return bytes_ || view_ == View::largest ? SortKey::bytes : SortKey::files;
}

bool InsightsView::sort_descending() const {
    if (sort_ && sort_key() == sort_)
        return sort_descending_;
    return view_ != View::languages && view_ != View::age;
}

void InsightsView::set_sort(SortKey key, bool descending) {
    const auto columns = sort_columns();
    if (std::ranges::find(columns, key, &SortColumn::key) == columns.end())
        return;
    invalidate_iris();
    sort_ = key;
    sort_descending_ = descending;
    const auto selected = selected_row_ && *selected_row_ < rows_.size()
                              ? std::optional<Row>{rows_[*selected_row_]}
                              : std::nullopt;
    sort_rows();
    requested_scroll_ = 0;
    if (selected) {
        const auto found = std::ranges::find_if(rows_, [&](const Row& row) {
            return row.node == selected->node && row.extension == selected->extension &&
                   row.path == selected->path && row.label == selected->label;
        });
        if (found != rows_.end()) {
            selected_row_ = static_cast<std::size_t>(found - rows_.begin());
            requested_scroll_ = static_cast<float>(*selected_row_) * 44;
            if (focus_.starts_with("row:") || focus_.starts_with("tile:"))
                focus_ = std::format("row:{}", *selected_row_);
        }
    }
    controls_.clear();
    redraw_();
}

void InsightsView::sort_rows() {
    const auto key = sort_key();
    const bool descending = sort_descending();
    const auto value = [&](const Row& row) -> std::uint64_t {
        switch (key.value_or(SortKey::name)) {
            case SortKey::files:
                return row.files;
            case SortKey::bytes:
                return row.bytes;
            case SortKey::lines:
                return row.code.lines();
            case SortKey::code:
                return row.code.code;
            case SortKey::comments:
                return row.code.comments;
            case SortKey::blanks:
                return row.code.blanks;
            case SortKey::share:
                return bytes_ || view_ == View::largest ? row.bytes : row.files;
            case SortKey::name:
                return 0;
        }
        return 0;
    };
    const auto less = [&](const Row& a, const Row& b) {
        if (!key)
            return a.extension < b.extension;
        if (*key == SortKey::name)
            return descending ? fs::natural_compare(a.label, b.label) > 0
                              : fs::natural_compare(a.label, b.label) < 0;
        const auto left = value(a), right = value(b);
        return left != right ? (descending ? left > right : left < right)
                             : fs::natural_compare(a.label, b.label) < 0;
    };
    if (view_ != View::languages) {
        std::stable_sort(rows_.begin(), rows_.end(), less);
        return;
    }
    // Sort siblings while keeping embedded languages attached to their parent.
    const auto root = rows_.size();
    std::map<std::string, std::size_t> by_key;
    for (std::size_t i = 0; i < rows_.size(); ++i)
        by_key.emplace(rows_[i].extension, i);
    std::vector<std::vector<std::size_t>> children(root + 1);
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        const auto& language = rows_[i].extension;
        const auto split = language.find_last_of('\x1f');
        const auto parent =
            split == std::string::npos ? by_key.end() : by_key.find(language.substr(0, split));
        children[parent == by_key.end() ? root : parent->second].push_back(i);
    }
    for (auto& siblings : children)
        std::stable_sort(siblings.begin(), siblings.end(),
                         [&](auto a, auto b) { return less(rows_[a], rows_[b]); });
    std::vector<Row> ordered;
    ordered.reserve(rows_.size());
    const auto append = [&](auto&& self, std::size_t parent) -> void {
        for (const auto index : children[parent]) {
            ordered.push_back(std::move(rows_[index]));
            self(self, index);
        }
    };
    append(append, root);
    rows_ = std::move(ordered);
}

void InsightsView::finish_rows() {
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        const auto found = renamed_paths_.find(row_path(i));
        if (found != renamed_paths_.end()) {
            rows_[i].path = found->second;
            rows_[i].label = view_ == View::code_matches
                                 ? found->second.lexically_relative(scope_path()).string()
                                 : found->second.filename().string();
        }
    }
    sort_rows();
    if (!filter_.empty()) {
        const fs::NamePattern pattern{filter_};
        std::erase_if(rows_, [&pattern](const Row& row) { return !pattern.matches(row.label); });
    }
}

void InsightsView::set_filter(std::string query) {
    if (query == filter_)
        return;
    filter_ = std::move(query);
    if (open_)
        rebuild();
}

ui::Rect InsightsView::selected_bounds() const {
    return selected_row_ ? control_bounds(std::format("row:{}", *selected_row_)) : ui::Rect{};
}

std::optional<ViewHit> InsightsView::hit(float x, float y) const {
    if (!selected_folder_ || !selected_row_ || !selected_folder_->cursor())
        return {};
    const auto controls = std::views::reverse(controls_);
    const auto found = std::ranges::find_if(
        controls, [&](const Control& control) { return control.contains(x, y); });
    if (found == controls.end() || (found->id != std::format("row:{}", *selected_row_) &&
                                    found->id != std::format("tile:{}", *selected_row_)))
        return {};
    return ViewHit{selected_folder_, selected_folder_->cursor(), true};
}

bool InsightsView::edit_name(ui::Rect rect, std::size_t index, float size) {
    rect = rect.intersect(list_);
    if (!edit_ || !selected_folder_ || edit_->folder != selected_folder_.get() ||
        row_path(index) != selected_folder_->path() / edit_->name)
        return false;
    edit_->place(rect, size);
    return true;
}

void InsightsView::open(std::filesystem::path root, bool hidden, Reveal reveal_handler,
                        std::optional<View> initial) {
    const bool resume = !initial && root == entry_root_;
    const auto destination = resume ? scope_path() : root;
    entry_root_ = std::move(root);
    reveal_ = std::move(reveal_handler);
    if (!resume) {
        options_.hidden = hidden;
        history_.clear();
        extension_.clear();
        offset_ = 0;
        if (view_ == View::matches || view_ == View::other_types)
            view_ = View::types;
        if (view_ == View::age_matches)
            view_ = View::age;
        if (view_ == View::code_matches)
            view_ = View::languages;
    }
    if (initial)
        view_ = *initial;
    root_ = destination;
    data_.reset();
    scope_ = 0;
    focus_.clear();
    hover_.clear();
    open_ = true;
    refresh();
}

void InsightsView::close() {
    iris_animating_ = false;
    iris_target_.clear();
    iris_from_.clear();
    iris_painted_.clear();
    iris_snapshot_.reset();
    if (!open_)
        return;
    root_ = scope_path();
    clear_selection();
    scope_folder_.reset();
    if (loading())
        offset_ = pending_offset_;
    stop_.cancel();
    job_.cancel();
    source_.cancel();
    code_.reset();
    open_ = loading_ = false;
    scope_ = 0;
    data_.reset();
    rows_.clear();
    controls_.clear();
    hover_.clear();
    redraw_();
}

std::filesystem::path InsightsView::scope_path() const {
    return data_ ? (scope_ == 0 ? data_->root : data_->root / data_->relative_path(scope_)) : root_;
}

void InsightsView::refresh() {
    invalidate_iris();
    if (!open_)
        return;
    root_ = scope_path();
    renamed_paths_.clear();
    stop_.cancel();
    job_.cancel();
    stop_ = core::CancelToken{};
    data_.reset();
    progress_ = {};
    pending_offset_ = offset_;
    scope_ = offset_ = 0;
    rows_.clear();
    controls_.clear();
    clear_selection();
    sync_folder();
    source_.cancel();
    code_.reset();
    loading_ = false;
    if (source_view()) {
        source_.start(root_, options_.recursive, options_.hidden, [this](auto report) {
            code_ = std::move(report);
            const auto offset = pending_offset_;
            rebuild();
            offset_ = rows_.empty() ? 0 : std::min(offset, rows_.size() - 1);
            requested_scroll_ = static_cast<float>(offset_) * 44;
            if (code_done_ && code_->error == platform::CodeError::none)
                code_done_(code_);
        });
        redraw_();
        return;
    }
    loading_ = true;
    job_ = pool_.submit_streaming(
        core::Priority::nearby,
        [root = root_, options = options_, stop = stop_](const core::CancelToken&, auto emit) {
            return std::make_shared<const fs::InsightSnapshot>(
                fs::scan_insights(root, options, stop, emit));
        },
        [this](fs::InsightProgress progress) {
            progress_ = progress;
            redraw_();
        },
        [this](std::shared_ptr<const fs::InsightSnapshot> data) {
            data_ = std::move(data);
            loading_ = false;
            const auto offset = pending_offset_;
            rebuild();
            offset_ = rows_.empty() ? 0 : std::min(offset, rows_.size() - 1);
            requested_scroll_ = static_cast<float>(offset_) * 44;
        });
    redraw_();
}

void InsightsView::stop() {
    source_.stop();
    stop_.cancel();  // Keep delivery enabled: publish the partial snapshot when the walk stops.
    redraw_();
}

void InsightsView::set_view(View view) {
    if (view == view_)
        return;
    remember();
    view_ = view;
    offset_ = 0;
    if ((source_view() && !code_) || (!source_view() && !data_))
        refresh();
    else {
        if (!source_view())
            source_.cancel();
        rebuild();
    }
}

void InsightsView::set_size_measure(bool bytes) {
    sort_.reset();
    bytes_ = bytes;
    rebuild();
}

void InsightsView::set_recursive(bool recursive) {
    if (options_.recursive != recursive) {
        options_.recursive = recursive;
        refresh();
    }
}

void InsightsView::set_hidden(bool hidden) {
    if (options_.hidden != hidden) {
        options_.hidden = hidden;
        refresh();
    }
}

void InsightsView::rebuild() {
    invalidate_iris();
    clear_selection();
    pending_offset_ = 0;
    requested_scroll_ = 0.0F;
    hover_.clear();
    rows_.clear();
    controls_.clear();
    offset_ = 0;
    if (source_view()) {
        if (code_ && code_->error == platform::CodeError::none) {
            for (std::size_t i = 0; i < code_->languages.size(); ++i) {
                const auto& lang = code_->languages[i];
                if (view_ == View::languages)
                    rows_.push_back({.label = lang.name,
                                     .extension = lang.key,
                                     .node = i,
                                     .files = lang.files.size(),
                                     .code = lang.counts,
                                     .depth = lang.depth});
                else if (lang.key == extension_)
                    for (const auto& file : lang.files)
                        rows_.push_back({.label = file.relative.string(),
                                         .files = 1,
                                         .code = file.counts,
                                         .path = file.relative});
            }
            std::ranges::sort(rows_, [this](const Row& a, const Row& b) {
                return view_ == View::languages           ? a.extension < b.extension
                       : a.code.lines() != b.code.lines() ? a.code.lines() > b.code.lines()
                                                          : a.label < b.label;
            });
        }
        finish_rows();
        sync_folder();
        redraw_();
        return;
    }
    if (data_ && scope_ < data_->nodes.size()) {
        std::map<std::string, Row> groups;
        for (std::size_t i = scope_ + 1; i < data_->nodes[scope_].end; ++i) {
            const auto& node = data_->nodes[i];
            if (view_ == View::space || view_ == View::iris) {
                if (node.parent == scope_)
                    rows_.push_back({.label = node.name,
                                     .extension = {},
                                     .node = i,
                                     .files = node.files,
                                     .bytes = node.bytes,
                                     .directory = node.directory,
                                     .partial = node.partial});
                continue;
            }
            if (node.directory)
                continue;
            const auto age = static_cast<std::size_t>(
                fs::insight_age(node.modified_seconds, data_->captured_seconds));
            if (view_ == View::largest || view_ == View::age_matches) {
                if (view_ == View::largest || extension_ == std::to_string(age))
                    rows_.push_back(
                        {.label = node.name, .node = i, .files = 1, .bytes = node.bytes});
                continue;
            }
            if (view_ == View::age) {
                auto& row = groups[std::to_string(age)];
                row.label = age_label(age);
                row.extension = std::to_string(age);
                ++row.files;
                row.bytes += node.bytes;
                continue;
            }
            const auto extension = fs::insight_extension(node.name);
            if (view_ == View::matches) {
                if (extension == extension_)
                    // Keep each basename once; repeating a deep prefix for every match can
                    // multiply the scan's bounded name storage by the directory depth.
                    rows_.push_back({.label = node.name,
                                     .extension = extension,
                                     .node = i,
                                     .files = 1,
                                     .bytes = node.bytes});
            } else {
                auto& row = groups[extension];
                row.label = extension_label(extension);
                row.extension = extension;
                ++row.files;
                row.bytes += node.bytes;
            }
        }
        for (auto& [extension, row] : groups)
            rows_.push_back(std::move(row));
        std::ranges::sort(rows_, [this](const Row& a, const Row& b) {
            if (view_ == View::age)
                return a.extension < b.extension;
            const auto left = bytes_ || view_ == View::largest ? a.bytes : a.files;
            const auto right = bytes_ || view_ == View::largest ? b.bytes : b.files;
            return left != right ? left > right : a.label < b.label;
        });
    }
    // Keep the leading types readable; Other can be expanded into the complete tail.
    // Compute membership from the active measure before user sorting/filtering.
    if (((view_ == View::types && filter_.empty()) || view_ == View::other_types) &&
        rows_.size() > 12) {
        long double total = 0;
        for (const auto& row : rows_)
            total += bytes_ ? row.bytes : row.files;
        std::size_t keep = 0;
        while (keep < 12 && keep < rows_.size() &&
               (keep == 0 || static_cast<long double>(bytes_ ? rows_[keep].bytes
                                                             : rows_[keep].files) >= total * 0.01L))
            ++keep;
        if (view_ == View::other_types)
            rows_.erase(rows_.begin(), rows_.begin() + static_cast<std::ptrdiff_t>(keep));
        else {
            Row other{.label = std::string{i18n::tr("Other")}, .other = true};
            for (std::size_t i = keep; i < rows_.size(); ++i) {
                other.files += rows_[i].files;
                other.bytes += rows_[i].bytes;
            }
            rows_.resize(keep);
            rows_.push_back(std::move(other));
        }
    }
    finish_rows();
    // Creating a Folder can redraw synchronously. Publish the new rows first so that
    // repaint cannot consume chart invalidation while the previous rows are still present.
    sync_folder();
    redraw_();
}

void InsightsView::reveal(std::size_t node) {
    if (!data_ || node >= data_->nodes.size())
        return;
    const auto path = data_->root / data_->relative_path(node);
    const bool directory = data_->nodes[node].directory;
    const auto handler = reveal_;
    close();
    if (handler)
        handler(path, directory);
}

void InsightsView::activate_row(std::size_t index) {
    if (index >= rows_.size())
        return;
    const auto row = rows_[index];
    if (row.other) {
        remember();
        view_ = View::other_types;
        rebuild();
        return;
    }
    if (file_row(index) && folder_factory_) {
        select_row(index);
        pending_open_ = true;
        selection_changed();
        return;
    }
    if (view_ == View::code_matches) {
        const auto path = code_->root / row.path;
        const auto handler = reveal_;
        close();
        if (handler)
            handler(path, false);
        return;
    }
    if (view_ == View::types || view_ == View::other_types || view_ == View::age ||
        view_ == View::languages) {
        remember();
        extension_ = row.extension;
        view_ = (view_ == View::types || view_ == View::other_types) ? View::matches
                : view_ == View::age                                 ? View::age_matches
                                                                     : View::code_matches;
        rebuild();
    } else if (row.directory) {
        navigate(data_->root / data_->relative_path(row.node));
    } else {
        reveal(row.node);
    }
}

void InsightsView::remember() {
    if (history_.size() == 128)
        history_.erase(history_.begin());
    history_.push_back({scope_path(), view_, extension_, offset_});
}

void InsightsView::restore(Location location) {
    view_ = location.view;
    extension_ = std::move(location.extension);
    if (source_view()) {
        if (code_ && code_->root == location.path) {
            rebuild();
            offset_ = rows_.empty() ? 0 : std::min(location.offset, rows_.size() - 1);
            requested_scroll_ = static_cast<float>(offset_) * 44;
            return;
        }
        root_ = location.path;
        data_.reset();
        scope_ = 0;
        offset_ = location.offset;
        refresh();
        return;
    }
    source_.cancel();
    // Reuse complete folders in the current snapshot, including after breadcrumb jumps.
    if (data_) {
        const auto relative = location.path.lexically_relative(data_->root);
        std::size_t id = 0;
        bool found = !relative.empty();
        for (const auto& component : relative) {
            if (component == ".")
                continue;
            auto child = id + 1;
            while (child < data_->nodes[id].end &&
                   (!data_->nodes[child].directory || data_->nodes[child].name != component))
                child = data_->nodes[child].end;
            if (child >= data_->nodes[id].end) {
                found = false;
                break;
            }
            id = child;
        }
        if (found && (id == scope_ || (options_.recursive && !data_->nodes[id].partial))) {
            scope_ = id;
            rebuild();
            offset_ = rows_.empty() ? 0 : std::min(location.offset, rows_.size() - 1);
            requested_scroll_ = static_cast<float>(offset_) * 44;
            focus_ = "back";
            return;
        }
    }
    root_ = std::move(location.path);
    data_.reset();
    scope_ = 0;
    offset_ = location.offset;
    focus_ = "back";
    refresh();
}

void InsightsView::navigate(const std::filesystem::path& path) {
    if (path.empty() || path == scope_path())
        return;
    remember();
    restore({path,
             (view_ == View::matches || view_ == View::other_types) ? View::types
             : view_ == View::age_matches                           ? View::age
             : view_ == View::code_matches                          ? View::languages
                                                                    : view_,
             {},
             0});
}

void InsightsView::back() {
    if (history_.empty())
        return;
    auto previous = std::move(history_.back());
    history_.pop_back();
    restore(std::move(previous));
}

bool InsightsView::can_up() const {
    const auto path = scope_path();
    return !path.empty() && path.parent_path() != path;
}

void InsightsView::up() {
    if (can_up())
        navigate(scope_path().parent_path());
}

void InsightsView::page(int direction) {
    scroller_.jump_to(scroller_.offset() + static_cast<float>(direction) * list_.height);
    controls_.clear();
    redraw_();
}

std::string InsightsView::status() const {
    if (source_view()) {
        if (source_.running())
            return std::string{i18n::tr("Analyzing source…")};
        if (!code_)
            return {};
        using platform::CodeError;
        switch (code_->error) {
            case CodeError::missing_tool:
                return std::string{i18n::tr("Install Tokei to analyze source code")};
            case CodeError::invalid_root:
                return std::string{i18n::tr("Folder is unavailable")};
            case CodeError::remote:
                return std::string{i18n::tr("Network folders are not scanned")};
            case CodeError::stopped:
                return std::string{i18n::tr("Source analysis stopped")};
            case CodeError::timed_out:
                return std::string{i18n::tr("Source analysis timed out · Try a smaller folder")};
            case CodeError::output_limit:
                return std::string{i18n::tr("Source report is too large · Try a smaller folder")};
            case CodeError::failed:
            case CodeError::invalid_output:
                return std::string{
                    i18n::tr("Source analysis failed · Check your Tokei installation")};
            case CodeError::none:
                return std::string{code_->inaccurate
                                       ? i18n::tr("Some source files could not be read")
                                       : i18n::tr("Scan complete · Refresh to check for changes")};
        }
    }
    if (loading_)
        return stop_.cancelled()
                   ? std::string{i18n::tr("Stopping…")}
                   : i18n::trf("Counting… {}", i18n::trn("{} file", "{} files", progress_.files));
    if (!data_)
        return {};
    if (data_->remote)
        return std::string{i18n::tr("Network folders are not scanned")};
    if (!data_->complete())
        return i18n::trf("Partial result · Unreadable: {} · Skipped folders: {}{}",
                         data_->unreadable, data_->skipped_folders,
                         data_->cancelled ? i18n::tr(" · stopped")
                         : data_->limited ? i18n::tr(" · scan limit reached")
                                          : std::string_view{});
    return std::string{i18n::tr("Scan complete · Refresh to check for changes")};
}

void InsightsView::key(const ui::KeyEvent& event) {
    if (!open_)
        return;
    if (folder_factory_ && event.modifiers.ctrl &&
        (event.keysym == XKB_KEY_a || event.keysym == XKB_KEY_A ||
         ((event.keysym == XKB_KEY_i || event.keysym == XKB_KEY_I) && event.modifiers.shift))) {
        select_all(event.modifiers.shift);
        return;
    }
    if (folder_factory_ && !event.modifiers.alt && !rows_.empty() &&
        (event.keysym == XKB_KEY_Down || event.keysym == XKB_KEY_Up ||
         event.keysym == XKB_KEY_Home || event.keysym == XKB_KEY_End)) {
        std::size_t index = selected_row_.value_or(0);
        if (event.keysym == XKB_KEY_Home)
            index = 0;
        else if (event.keysym == XKB_KEY_End)
            index = rows_.size() - 1;
        else if (selected_row_)
            index = event.keysym == XKB_KEY_Down ? std::min(index + 1, rows_.size() - 1)
                    : index > 0                  ? index - 1
                                                 : 0;
        if (file_row(index))
            select_row(index, event.modifiers);
        else {
            selected_row_ = index;
            focus_ = std::format("row:{}", index);
        }
        const float top = static_cast<float>(index) * 44;
        if (top < scroller_.offset())
            scroller_.jump_to(top);
        else if (top + 44 > scroller_.offset() + list_.height)
            scroller_.jump_to(top + 44 - list_.height);
        redraw_();
        return;
    }
    if (folder_factory_ && selected_row_ &&
        (focus_ == std::format("row:{}", *selected_row_) ||
         focus_ == std::format("tile:{}", *selected_row_)) &&
        (event.keysym == XKB_KEY_Return || event.keysym == XKB_KEY_KP_Enter)) {
        activate_row(*selected_row_);
        return;
    }
    if (event.keysym == XKB_KEY_Escape) {
        if (view_ == View::matches || view_ == View::other_types || view_ == View::age_matches ||
            view_ == View::code_matches)
            back();
        return;
    }
    if (event.keysym == XKB_KEY_BackSpace ||
        (event.modifiers.alt && event.keysym == XKB_KEY_Left)) {
        back();
        return;
    }
    if (event.modifiers.alt && event.keysym == XKB_KEY_Up) {
        up();
        return;
    }
    if (event.keysym == XKB_KEY_Page_Down || event.keysym == XKB_KEY_Page_Up) {
        page(event.keysym == XKB_KEY_Page_Down ? 1 : -1);
        return;
    }
    if (controls_.empty())
        return;
    auto found = std::ranges::find(controls_, focus_, &Control::id);
    if (event.keysym == XKB_KEY_Tab || event.keysym == XKB_KEY_ISO_Left_Tab ||
        event.keysym == XKB_KEY_Down || event.keysym == XKB_KEY_Up) {
        const bool back = event.modifiers.shift || event.keysym == XKB_KEY_ISO_Left_Tab ||
                          event.keysym == XKB_KEY_Up;
        auto index =
            found == controls_.end() ? 0 : static_cast<std::size_t>(found - controls_.begin());
        if (found == controls_.end())
            index = back ? controls_.size() - 1 : 0;
        else
            index = back ? (index + controls_.size() - 1) % controls_.size()
                         : (index + 1) % controls_.size();
        focus_ = controls_[index].id;
        redraw_();
    } else if ((event.keysym == XKB_KEY_Return || event.keysym == XKB_KEY_KP_Enter ||
                event.keysym == XKB_KEY_space) &&
               found != controls_.end()) {
        auto action = found->action;
        action();
    }
}

void InsightsView::pointer(const ui::PointerEvent& event, ui::Scroller::Clock::time_point now) {
    if (scrollbar_.pointer(event, scroller_)) {
        controls_.clear();
        redraw_();
        return;
    }
    if (event.kind == ui::PointerEvent::Kind::motion) {
        std::string hovered;
        for (const auto& control : std::views::reverse(controls_))
            if (control.contains(event.x, event.y)) {
                hovered = control.id;
                break;
            }
        if (hover_ != hovered) {
            hover_ = std::move(hovered);
            redraw_();
        }
    }
    if (event.kind == ui::PointerEvent::Kind::scroll && list_.contains(event.x, event.y)) {
        if (event.wheel_y != 0)
            scroller_.wheel(event.wheel_y, now);
        else if (event.scroll_y != 0)
            scroller_.finger(event.scroll_y, now);
        if (event.scroll_finger && event.scroll_stop)
            scroller_.release(now);
        redraw_();
        return;
    }
    if (event.kind != ui::PointerEvent::Kind::press || !card_.contains(event.x, event.y))
        return;
    // The last painted control owns the point, including overlapping rings during a zoom.
    // Resolve once before deciding whether this is a file action or navigation.
    const auto controls = std::views::reverse(controls_);
    const auto found = std::ranges::find_if(
        controls, [&](const Control& control) { return control.contains(event.x, event.y); });
    if (found == controls.end())
        return;
    const auto control = *found;
    if (folder_factory_ &&
        (event.button == ui::PointerButton::left || event.button == ui::PointerButton::right) &&
        (control.id.starts_with("row:") || control.id.starts_with("tile:"))) {
        const auto i =
            static_cast<std::size_t>(std::stoull(control.id.substr(control.id.find(':') + 1)));
        if (file_row(i)) {
            if (event.button == ui::PointerButton::left || !selected(i))
                select_row(i, event.modifiers);
            if (view_ == View::iris && control.sector)
                requested_scroll_ = static_cast<float>(i) * 44;
            if (event.button == ui::PointerButton::right)
                pending_menu_ = {event.x, event.y};
            else if (event.click_count == 2)
                pending_open_ = true;
            selection_changed();
            return;
        }
    }
    if (event.button == ui::PointerButton::left) {
        focus_ = control.id;
        control.action();
    }
}

ui::CursorShape InsightsView::cursor_at(float x, float y) const {
    if (scrollbar_.hit(scroller_, x, y))
        return ui::CursorShape::standard;
    return std::ranges::any_of(controls_, [&](const Control& c) { return c.contains(x, y); })
               ? ui::CursorShape::pointer
               : ui::CursorShape::standard;
}

ui::Rect InsightsView::control_bounds(std::string_view id) const {
    const auto found = std::ranges::find(controls_, id, &Control::id);
    return found == controls_.end() ? ui::Rect{} : found->bounds;
}

void InsightsView::draw(ui::Renderer& renderer, const ui::Rect& bounds,
                        const theme::Palette& palette, ui::IconSet& icons,
                        const fs::MimeDatabase& mime, ui::Scroller::Clock::time_point now) {
    if (!open_)
        return;
    controls_.clear();
    const float footer_height = std::min(78.0F, bounds.height);

    struct Frame {
        ui::Rect body, footer;
    };

    const Frame frame{{bounds.x, bounds.y, bounds.width, bounds.height - footer_height},
                      {bounds.x, bounds.bottom() - footer_height, bounds.width, footer_height}};
    card_ = bounds;
    const auto surface = palette.surface;
    const auto muted = theme::readable_foreground(palette.on_surface_variant, surface);
    const float left = frame.body.x + 16, width = std::max(1.0F, frame.body.width - 32);
    const auto text_width = [&](std::string_view text) {
        return renderer.fonts().layout(text, 13, renderer.scale()).width / renderer.scale();
    };
    const auto control = [&](std::string id, std::string_view label, ui::Rect rect,
                             std::function<void()> action, std::string_view glyph = {},
                             bool selected = false, bool enabled = true) {
        const bool hot = enabled && hover_ == id;
        const auto fill = mix(surface, palette.primary, selected ? 0.17F : hot ? 0.09F : 0.035F);
        auto ink =
            theme::readable_foreground(selected ? palette.primary : palette.on_surface, fill);
        if (!enabled)
            ink = ink.with_alpha(0.35F);
        renderer.draw_box(rect, {.fill = fill,
                                 .radius = ui::Corners::all(3),
                                 .border_width = enabled && focus_ == id ? 2.0F
                                                 : selected              ? 1.0F
                                                                         : 0,
                                 .border = palette.primary.with_alpha(selected ? 0.55F : 1.0F)});
        const float label_x = glyph.empty()
                                  ? rect.x + std::max(8.0F, (rect.width - text_width(label)) / 2)
                                  : rect.x + 34;
        if (!glyph.empty())
            icons.draw(renderer, glyph, rect.x + 10, rect.y + 8, 16, ink);
        renderer.draw_text(label, label_x, rect.y + 8, 13, ink,
                           std::max(0.0F, rect.right() - label_x - 8));
        if (enabled && rect.bottom() <= frame.footer.bottom())
            controls_.push_back({std::move(id), rect, std::move(action)});
    };
    renderer.push_clip(frame.body);
    float x = left, y = frame.body.y + 12;
    const auto files = data_ ? data_->nodes[scope_].files : progress_.files;
    const auto bytes = data_ ? data_->nodes[scope_].bytes : progress_.bytes;
    const bool partial = loading_ || (data_ && data_->nodes[scope_].partial);
    std::string summary =
        std::format("{} · {}", i18n::trn("{} file", "{} files", files), format_size(bytes));
    if (partial)
        summary = i18n::trf("At least {}", summary);
    if (view_ == View::other_types)
        summary = std::format("{} · {}", i18n::tr("Other"), summary);
    if (view_ == View::matches || view_ == View::age_matches)
        summary = std::format("{} · {}",
                              view_ == View::age_matches
                                  ? age_label(static_cast<std::size_t>(std::stoul(extension_)))
                                  : extension_label(extension_),
                              i18n::trf("Matches: {}", rows_.size()));
    if (source_view())
        summary = source_.running() ? std::string{i18n::tr("Analyzing source…")}
                  : code_ && code_->error == platform::CodeError::none
                      ? std::format("{} · {}", i18n::trn("{} file", "{} files", code_->files),
                                    i18n::trn("{} line", "{} lines", code_->total.lines()))
                      : status();
    renderer.draw_text(summary, left, y, 12, muted, width);
    y += 30;
    if ((view_ == View::space || view_ == View::iris) && partial && !loading_) {
        renderer.draw_text(i18n::tr("Partial map · Only measured files have area"), left, y, 12,
                           muted, width);
        y += 24;
    }
    ui::Rect available{left, y, width, std::max(0.0F, frame.body.bottom() - y - 12)};
    list_ = available;
    const auto row_icon = [&](const Row& row, const ui::Rect& icon, ui::Color color) {
        if (row.other) {
            icons.draw(renderer, "chevron-right", icon.x, icon.y, icon.width, color);
            return;
        }
        if (view_ == View::age) {
            icons.draw(renderer, "calendar", icon.x, icon.y, icon.width, color);
            return;
        }
        const auto type = row.directory
                              ? fs::MimeDatabase::kDirectory
                              : mime.by_name((view_ == View::types || view_ == View::other_types)
                                                 ? "file" + row.extension
                                                 : row.label)
                                    .mime;
        icons.draw_file(renderer, mime.name(type), fs::icon_name(mime, type), icon.x, icon.y,
                        icon.width, color);
    };
    if (view_ == View::space && available.height >= 200) {
        const bool wide = width >= 760;
        const ui::Rect map{available.x, available.y, wide ? width * 0.55F : width,
                           wide ? available.height : available.height * 0.48F};
        if (wide) {
            list_.x = map.right() + 16;
            list_.width = available.right() - list_.x;
        } else {
            list_.y = map.bottom() + 12;
            list_.height = available.bottom() - list_.y;
        }
        draw_map(renderer, map, palette, icons, mime);
    }
    if (view_ == View::iris && available.height >= 300) {
        const bool wide = width >= 760;
        const ui::Rect chart{available.x, available.y, wide ? width * 0.55F : width,
                             wide ? available.height : std::min(360.0F, available.height * 0.55F)};
        if (wide) {
            list_.x = chart.right() + 16;
            list_.width = available.right() - list_.x;
        } else {
            list_.y = chart.bottom() + 12;
            list_.height = available.bottom() - list_.y;
        }
        draw_iris(renderer, chart, palette, icons, mime, now);
    } else {
        iris_animating_ = false;
        iris_painted_.clear();
        iris_target_.clear();
        iris_from_.clear();
        iris_snapshot_.reset();
        iris_dirty_ = true;
    }
    if (view_ == View::types || view_ == View::other_types || view_ == View::largest) {
        draw_legend(renderer, {list_.x + 6, list_.y, list_.width - 12, 40}, palette, mime);
        list_.y += 44;
        list_.height = std::max(0.0F, list_.height - 44);
    }
    const auto number = [&](std::string_view text, ui::Rect cell, float size = 12.0F) {
        if (cell.width <= 0)
            return;
        const auto& layout = renderer.fonts().layout(text, size, renderer.scale(), cell.width,
                                                     ui::Ellipsis::end, ui::Typeface::monospace);
        renderer.draw_layout(layout, cell.right() - layout.width / renderer.scale(), cell.y, muted);
    };
    const auto sortable = sort_columns();
    const auto column_header = [&](const SortColumn& column, float x0, float cell_width) {
        const std::string id = "sort:" + std::string{column.id};
        const ui::Rect rect{x0, list_.y - 4, std::max(0.0F, cell_width), 28};
        const bool sorted = sort_key() == column.key;
        if (hover_ == id || focus_ == id)
            renderer.draw_box(rect, {.fill = palette.primary.with_alpha(0.08F),
                                     .border_width = focus_ == id ? 1.0F : 0,
                                     .border = palette.primary});
        const auto label = column.key == SortKey::share && partial ? i18n::tr("Share so far")
                                                                   : i18n::tr(column.label);
        const float label_width =
            std::min(std::max(0.0F, rect.width - (sorted ? 28 : 12)),
                     renderer.fonts().layout(label, 12, renderer.scale()).width / renderer.scale());
        renderer.draw_text(label,
                           column.key == SortKey::name
                               ? rect.x + 6
                               : rect.right() - (sorted ? 22 : 6) - label_width,
                           list_.y, 12, sorted ? palette.on_surface : muted, label_width);
        if (sorted && rect.width >= 28)
            icons.draw(renderer, sort_descending() ? "chevron-down" : "chevron-up",
                       rect.right() - 18, list_.y, 14, palette.on_surface);
        controls_.push_back({id, rect.intersect(frame.body), [this, key = column.key] {
                                 set_sort(key, sort_key() == key ? !sort_descending()
                                                                 : key != SortKey::name);
                             }});
    };
    const auto begin_rows = [&] {
        list_.y += 28;
        list_.height = std::max(0.0F, list_.height - 28);
        scroller_.set_extent(static_cast<float>(rows_.size()) * 44.0F, list_.height);
        if (requested_scroll_) {
            scroller_.jump_to(*requested_scroll_);
            requested_scroll_.reset();
        }
        scroller_.advance(now);
        scrollbar_.layout(list_);
        offset_ = static_cast<std::size_t>(scroller_.offset() / 44.0F);
        page_size_ = static_cast<std::size_t>(std::ceil(list_.height / 44.0F)) + 1;
        renderer.push_clip(list_);
    };
    if (source_view()) {
        // Shared scale and legend: parent rows contain only their own language;
        // indented embedded-language rows contribute separately to the total.
        const std::array accents{ui::Color::rgb(0x7AA2F7), ui::Color::rgb(0x9ECE6A),
                                 ui::Color::rgb(0xE0AF68)};
        const std::array labels{i18n::tr("Code"), i18n::tr("Comments"), i18n::tr("Blanks")};
        const auto stack = [&](ui::Rect rect, const platform::CodeCounts& counts,
                               std::uint64_t total, float opacity = 1.0F) {
            const std::array values{counts.code, counts.comments, counts.blanks};
            float at = rect.x;
            for (std::size_t k = 0; k < values.size(); ++k) {
                const float w =
                    total ? rect.width * static_cast<float>(static_cast<double>(values[k]) /
                                                            static_cast<double>(total))
                          : 0;
                if (w > 0)
                    renderer.draw_box({at, rect.y, w, rect.height},
                                      {.fill = accents[k].with_alpha(opacity)});
                at += w;
            }
        };
        if (code_ && code_->error == platform::CodeError::none && list_.height >= 90) {
            const std::array values{code_->total.code, code_->total.comments, code_->total.blanks};
            for (std::size_t k = 0; k < labels.size(); ++k) {
                const float at = list_.x + list_.width * static_cast<float>(k) / 3;
                renderer.draw_box({at, list_.y + 4, 8, 8},
                                  {.fill = accents[k], .radius = ui::Corners::all(2)});
                renderer.draw_text(std::format("{} {}", values[k], labels[k]), at + 16, list_.y, 12,
                                   muted, list_.width / 3 - 24);
            }
            stack({list_.x, list_.y + 23, list_.width, 8}, code_->total, code_->total.lines());
            list_.y += 48;
            list_.height -= 48;
        }
        const std::array columns{0.0F, 0.38F, 0.50F, 0.62F, 0.74F, 0.88F};
        const float table_width = std::max(0.0F, list_.width - 20);
        for (std::size_t k = 0; k < columns.size(); ++k)
            column_header(
                sortable[k], list_.x + table_width * columns[k],
                table_width * ((k + 1 < columns.size() ? columns[k + 1] : 1) - columns[k]));
        begin_rows();
        for (std::size_t i = offset_; i < rows_.size() && i < offset_ + page_size_; ++i) {
            const auto& row = rows_[i];
            const ui::Rect rect{list_.x, list_.y + static_cast<float>(i) * 44 - scroller_.offset(),
                                list_.width - 20, 40};
            const auto id = std::format("row:{}", i);
            const auto accent = row_color(row, palette, mime);
            stack(rect, row.code, code_ ? code_->total.lines() : 0, 0.12F);
            renderer.draw_box(rect, {.fill = palette.primary.with_alpha(selected(i)    ? 0.18F
                                                                        : hover_ == id ? 0.08F
                                                                                       : 0.0F),
                                     .radius = {},
                                     .border_width = focus_ == id ? 1.0F : 0,
                                     .border = palette.primary});
            const float indent = view_ == View::code_matches
                                     ? 26.0F
                                     : static_cast<float>(std::min<std::size_t>(row.depth, 4)) * 12;
            if (view_ == View::code_matches)
                row_icon(row, {rect.x + 6, rect.y + 10, 18, 18}, accent);
            const std::array cells{
                row.depth ? "↳ " + row.label : row.label, std::to_string(row.files),
                std::to_string(row.code.lines()),         std::to_string(row.code.code),
                std::to_string(row.code.comments),        std::to_string(row.code.blanks)};
            for (std::size_t k = 0; k < columns.size(); ++k) {
                if (k == 0 && edit_name({rect.x + 6 + indent, rect.y + 4,
                                         rect.width * 0.38F - 12 - indent, 30},
                                        i, 12))
                    continue;
                const float start = rect.x + rect.width * columns[k] + 6 + (k == 0 ? indent : 0);
                const float cell_width =
                    rect.width * ((k + 1 < columns.size() ? columns[k + 1] : 1) - columns[k]) - 12 -
                    (k == 0 ? indent : 0);
                if (k == 0)
                    renderer.draw_text(cells[k], start, rect.y + 8, 12, palette.on_surface,
                                       cell_width, ui::Ellipsis::middle);
                else
                    number(cells[k], {start, rect.y + 8, cell_width, 16});
            }
            controls_.push_back({id, rect.intersect(list_), [this, i] { activate_row(i); }});
        }
        if (rows_.empty())
            renderer.draw_text(source_.running() ? i18n::tr("Analyzing source…")
                               : code_ && code_->error == platform::CodeError::none
                                   ? i18n::tr("No source files in this scope")
                                   : status(),
                               list_.x + 6, list_.y + 36, 13, muted, list_.width - 12);
    } else {
        const bool shares = (view_ == View::types || view_ == View::other_types ||
                             view_ == View::age || view_ == View::largest) &&
                            list_.width >= 520;
        const float count_x = shares ? 0.48F : 0.55F, size_x = shares ? 0.64F : 0.76F;
        const float table_width = std::max(0.0F, list_.width - 20);
        column_header(sortable[0], list_.x, table_width * count_x);
        column_header(sortable[1], list_.x + table_width * count_x,
                      table_width * (size_x - count_x));
        column_header(sortable[2], list_.x + table_width * size_x,
                      table_width * ((shares ? 0.84F : 1.0F) - size_x));
        if (shares)
            column_header(sortable[3], list_.x + table_width * 0.84F, table_width * 0.16F);
        begin_rows();
        for (std::size_t i = offset_; i < rows_.size() && i < offset_ + page_size_; ++i) {
            const auto& row = rows_[i];
            const ui::Rect rect{list_.x, list_.y + static_cast<float>(i) * 44 - scroller_.offset(),
                                list_.width - 20, 40};
            const auto id = std::format("row:{}", i);
            const auto accent = row_color(row, palette, mime);
            const auto total = bytes_ || view_ == View::largest ? bytes : files;
            const auto value = bytes_ || view_ == View::largest ? row.bytes : row.files;
            const float share =
                total ? static_cast<float>(static_cast<double>(value) / static_cast<double>(total))
                      : 0;
            renderer.fill_rect(
                {rect.x, rect.y, rect.width * std::clamp(share, 0.0F, 1.0F), rect.height},
                accent.with_alpha(0.12F));
            renderer.draw_box(
                rect, {.fill = palette.primary.with_alpha(
                           selected(i) ? 0.18F
                           : (hover_ == id ||
                              ((view_ == View::iris || view_ == View::space) && chart_hover(i)))
                               ? 0.08F
                               : 0.0F),
                       .radius = {},
                       .border_width = focus_ == id ? 1.0F : 0,
                       .border = palette.primary});
            const auto label =
                file_row(i) ? row_path(i).lexically_relative(scope_path()).string() : row.label;
            row_icon(row, {rect.x + 10, rect.y + 10, 20, 20},
                     theme::readable_foreground(accent, surface));
            if (!edit_name({rect.x + 40, rect.y + 4, rect.width * (count_x - 0.03F) - 40, 30}, i,
                           13))
                renderer.draw_text(label, rect.x + 40, rect.y + 10, 13, palette.on_surface,
                                   std::max(0.0F, rect.width * (count_x - 0.03F) - 40),
                                   ui::Ellipsis::middle);
            if (shares)
                number(std::format("{:.1f}%", share * 100),
                       {rect.x + rect.width * 0.84F, rect.y + 10, rect.width * 0.16F - 6, 18});
            const bool unscanned = row.directory && !options_.recursive;
            const bool unknown = row.partial && row.files == 0;
            number(unscanned     ? "—"
                   : unknown     ? std::string{i18n::tr("Unknown")}
                   : row.partial ? i18n::trf("≥ {}", row.files)
                                 : std::to_string(row.files),
                   {rect.x + rect.width * count_x, rect.y + 10, rect.width * (size_x - count_x) - 6,
                    18});
            const auto size = unscanned     ? std::string{"—"}
                              : unknown     ? std::string{i18n::tr("Unknown")}
                              : row.partial ? i18n::trf("≥ {}", format_size(row.bytes))
                                            : format_size(row.bytes);
            number(size, {rect.x + rect.width * size_x, rect.y + 10,
                          rect.width * ((shares ? 0.84F : 1.0F) - size_x) - 6, 18});
            controls_.push_back({id, rect.intersect(list_), [this, i] { activate_row(i); }});
        }
        if (rows_.empty())
            renderer.draw_text(loading_  ? i18n::tr("Counting…")
                               : partial ? i18n::tr("No files measured yet")
                                         : i18n::tr("No files in this scope"),
                               list_.x + 10, list_.y + 36, 13, muted, list_.width - 20);
    }
    renderer.pop_clip();  // rows
    if (const auto thumb = scrollbar_.thumb(scroller_); !thumb.empty())
        renderer.draw_box(
            thumb,
            {.fill = palette.on_surface_variant.with_alpha(scrollbar_.dragging() ? 0.75F : 0.45F),
             .radius = ui::Corners::all(3)});
    renderer.pop_clip();  // body
    renderer.push_clip(frame.footer);
    renderer.draw_text(status(), left, frame.footer.y + 8, 11, muted, width);
    renderer.draw_text(
        source_view()
            ? i18n::tr("Tokei · Respects ignore files · Embedded languages shown separately")
            : i18n::tr("File sizes per name, including hard links · Links are not followed"),
        left, frame.footer.y + 26, 11, muted, width);
    x = left;
    y = frame.footer.y + 42;
    const auto footer_button = [&](std::string id, std::string_view label,
                                   std::function<void()> action, bool enabled = true) {
        const float w = std::min(128.0F, width / 5 - 6);
        control(std::move(id), label, {x, y, w, 32}, std::move(action), {}, false, enabled);
        x += w + 6;
    };
    footer_button(
        "refresh", loading() ? i18n::tr("Stop") : i18n::tr("Refresh"),
        [this] {
            if (loading())
                stop();
            else
                refresh();
        },
        source_view() || !loading_ || !stop_.cancelled());
    if (!rows_.empty())
        renderer.draw_text(i18n::trf("{} items · Select a row to explore", rows_.size()), x + 8,
                           frame.footer.y + 52, 11, muted, std::max(0.0F, left + width - x - 8));
    renderer.pop_clip();
}

}  // namespace rummage::views
