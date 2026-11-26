#include "rummage/views/file_finder.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <ctime>
#include <fcntl.h>
#include <format>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#include "rummage/core/i18n.hpp"
#include "rummage/fs/annotations.hpp"
#include "rummage/fs/name_pattern.hpp"
#include "rummage/fs/recent.hpp"
#include "rummage/views/fuzzy.hpp"

namespace rummage::views {
namespace {

// The order of results: the rank (tier, depth, score), then the shorter path, then the path.
bool better(const FoundFile& a, const FoundFile& b) {
    if (a.rank != b.rank) {
        return better_rank(a.rank, b.rank);
    }
    if (a.path.size() != b.path.size()) {
        return a.path.size() < b.path.size();
    }
    return a.path < b.path;
}

std::string count(std::size_t n) {
    // "12 345": groups of three, as the status bar writes them.
    std::string digits = std::to_string(n);
    for (std::ptrdiff_t at = static_cast<std::ptrdiff_t>(digits.size()) - 3; at > 0; at -= 3) {
        digits.insert(static_cast<std::size_t>(at), " ");
    }
    return digits;
}

}  // namespace

namespace {
std::string ascii_lower(std::string value) {
    for (char& c : value)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + ('a' - 'A'));
    return value;
}

std::optional<std::int64_t> search_date(std::string_view text) {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-')
        return {};
    for (std::size_t i = 0; i < text.size(); ++i)
        if (i != 4 && i != 7 && (text[i] < '0' || text[i] > '9'))
            return {};
    const auto number = [&](std::size_t start, std::size_t length) {
        unsigned value = 0;
        for (auto i = start; i < start + length; ++i)
            value = value * 10 + static_cast<unsigned>(text[i] - '0');
        return value;
    };
    const std::chrono::year_month_day day{std::chrono::year{static_cast<int>(number(0, 4))},
                                          std::chrono::month{number(5, 2)},
                                          std::chrono::day{number(8, 2)}};
    if (!day.ok())
        return {};
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::sys_days{day}.time_since_epoch())
        .count();
}

bool search_text(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        if (lead < 128) {
            if (lead == 0 || (lead < 32 && lead != 9 && lead != 10 && lead != 13))
                return false;
            ++i;
            continue;
        }
        const std::size_t n = lead >= 0xc2 && lead <= 0xdf   ? 2
                              : lead >= 0xe0 && lead <= 0xef ? 3
                              : lead >= 0xf0 && lead <= 0xf4 ? 4
                                                             : 0;
        if (n == 0 || i + n > text.size())
            return false;
        std::uint32_t code = lead & (n == 2 ? 0x1fU : n == 3 ? 0x0fU : 0x07U);
        for (std::size_t j = 1; j < n; ++j) {
            const auto c = static_cast<unsigned char>(text[i + j]);
            if ((c & 0xc0U) != 0x80U)
                return false;
            code = (code << 6U) | (c & 0x3fU);
        }
        if ((n == 3 && code < 0x800) || (n == 4 && code < 0x10000) || code > 0x10ffff ||
            (code >= 0xd800 && code <= 0xdfff))
            return false;
        i += n;
    }
    return true;
}

// Plain search text keeps fuzzy path ranking. A wildcard expression instead matches
// a whole basename at any depth, or a root-relative path when it contains '/'.
// Every component uses the existing bounded NamePattern engine; ** consumes zero or
// more complete path components, while ordinary * and ? cannot cross a separator.
class FilenameQuery {
public:
    explicit FilenameQuery(const std::string& text) : fuzzy_{text} {
        if (text.find_first_of("*?[") == std::string::npos)
            return;
        pattern_.emplace("glob:" + text);
        error_ = pattern_->error();
        path_ = text.find('/') != std::string::npos;
        if (!path_ || !error_.empty())
            return;
        std::string_view remaining{text};
        if (remaining.starts_with("./"))
            remaining.remove_prefix(2);
        while (!remaining.empty()) {
            const auto slash = remaining.find('/');
            const auto component = remaining.substr(0, slash);
            if (component.empty()) {
                error_ = i18n::trf("Invalid search filter: {}", text);
                return;
            }
            if (component == "**")
                components_.emplace_back();
            else {
                components_.emplace_back("glob:" + std::string{component});
                if (!components_.back()->error().empty())
                    error_ = components_.back()->error();
            }
            if (slash == std::string_view::npos)
                break;
            remaining.remove_prefix(slash + 1);
            if (remaining.empty()) {
                error_ = i18n::trf("Invalid search filter: {}", text);
                return;
            }
        }
    }

    [[nodiscard]] const std::string& error() const { return error_; }

    std::optional<PathRank> rank(std::string_view relative) {
        if (!error_.empty())
            return {};
        if (!pattern_)
            return fuzzy_.path_rank(relative);
        if (!path_) {
            const auto slash = relative.rfind('/');
            const auto name = relative.substr(slash == std::string_view::npos ? 0 : slash + 1);
            if (!pattern_->matches(std::string{name}))
                return {};
        } else {
            std::vector<std::string> parts;
            auto remaining = relative;
            while (!remaining.empty()) {
                const auto slash = remaining.find('/');
                parts.emplace_back(remaining.substr(0, slash));
                if (slash == std::string_view::npos)
                    break;
                remaining.remove_prefix(slash + 1);
            }
            std::vector<bool> matches(parts.size() + 1, false);
            matches[0] = true;
            for (const auto& component : components_) {
                if (!component) {
                    for (std::size_t i = 1; i < matches.size(); ++i)
                        matches[i] = matches[i] || matches[i - 1];
                } else {
                    for (std::size_t i = parts.size(); i > 0; --i)
                        matches[i] = matches[i - 1] && component->matches(parts[i - 1]);
                    matches[0] = false;
                }
            }
            if (!matches.back())
                return {};
        }
        return PathRank{.tier = PathRank::Tier::equal,
                        .depth = static_cast<std::size_t>(std::ranges::count(relative, '/')),
                        .score = 0};
    }

private:
    FuzzyQuery fuzzy_;
    std::optional<fs::NamePattern> pattern_;
    std::vector<std::optional<fs::NamePattern>> components_;
    std::string error_;
    bool path_{false};
};

struct SearchFd {
    int fd{-1};

    ~SearchFd() {
        if (fd >= 0)
            ::close(fd);
    }
};

bool same_version(const struct stat& a, const struct stat& b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
           a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
           a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
}  // namespace

SearchQuery parse_search_query(std::string_view input, bool hidden) {
    SearchQuery result;
    result.hidden = hidden;
    while (!input.empty()) {
        const auto first = input.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos)
            break;
        input.remove_prefix(first);
        const bool literal = input.front() == '"';
        std::string token;
        bool quoted = false;
        std::size_t i = 0;
        for (; i < input.size(); ++i) {
            const char c = input[i];
            if (c == '"') {
                quoted = !quoted;
                continue;
            }
            if (!quoted && (c == ' ' || c == '\t' || c == '\n' || c == '\r'))
                break;
            if (c == '\\' && i + 1 < input.size() && (input[i + 1] == '"' || input[i + 1] == '\\'))
                token += input[++i];
            else
                token += c;
        }
        input.remove_prefix(i);
        if (quoted) {
            result.error = i18n::tr("Close the quoted search text");
            break;
        }
        const auto colon = literal ? std::string::npos : token.find(':');
        const auto key = token.substr(0, colon);
        const auto value = colon == std::string::npos ? std::string{} : token.substr(colon + 1);
        const auto bad = [&] { result.error = i18n::trf("Invalid search filter: {}", token); };
        if (colon != std::string::npos && (key == "content" || key == "name")) {
            result.content = key == "content";
            if (!value.empty()) {
                if (!result.text.empty())
                    result.text += ' ';
                result.text += value;
            }
        } else if (colon != std::string::npos && (key == "tag" || key == "comment")) {
            if (value.empty())
                bad();
            else if (key == "tag")
                result.tag = value;
            else
                result.comment = value;
        } else if (colon != std::string::npos && key == "rating") {
            if (value.size() != 1 || value.front() < '0' || value.front() > '5')
                bad();
            else
                result.rating = static_cast<unsigned>(value.front() - '0');
        } else if (colon != std::string::npos && key == "type") {
            if (value != "file" && value != "folder" && value != "link" && value != "any")
                bad();
            else
                result.type = value == "any" ? "" : value;
        } else if (colon != std::string::npos && key == "ext") {
            result.extension = ascii_lower(value);
            if (result.extension.starts_with('.'))
                result.extension.erase(0, 1);
            if (result.extension.empty() ||
                result.extension.find_first_of("/\\") != std::string::npos)
                bad();
        } else if (colon != std::string::npos && (key == "after" || key == "before")) {
            auto date = search_date(value);
            if (!date)
                bad();
            else if (key == "after")
                result.after = date;
            else
                result.before = date;
        } else if (colon != std::string::npos && key == "hidden") {
            if (value != "yes" && value != "no")
                bad();
            else
                result.hidden = value == "yes";
        } else if (colon != std::string::npos && key == "case") {
            if (value != "fold" && value != "exact")
                bad();
            else
                result.fold_case = value == "fold";
        } else if (colon != std::string::npos && key == "exclude") {
            const std::filesystem::path path{value};
            if (value.empty() || path.is_absolute() ||
                std::ranges::any_of(path,
                                    [](const auto& part) { return part == ".." || part == "."; }))
                bad();
            else {
                std::string normalized = path.lexically_normal().string();
                while (normalized.ends_with('/'))
                    normalized.pop_back();
                result.excluded.push_back(std::move(normalized));
            }
        } else {
            if (!result.text.empty())
                result.text += ' ';
            result.text += token;
        }
        if (!result.error.empty())
            break;
    }
    if (result.after && result.before && *result.after >= *result.before)
        result.error = i18n::tr("The after date must precede the before date");
    if (result.error.empty() && !result.content)
        result.error = FilenameQuery{result.text}.error();
    return result;
}

SearchMatches match_files(const std::filesystem::path& root,
                          const std::vector<std::shared_ptr<const fs::PathBatch>>& batches,
                          const SearchQuery& query, std::size_t limit,
                          const core::CancelToken& token, SearchBudget& budget) {
    SearchMatches result;
    if (limit == 0 || !query.error.empty() || (query.content && query.text.empty()))
        return result;
    FilenameQuery names{query.text};
    const std::string needle = query.fold_case ? ascii_lower(query.text) : query.text;
    const SearchFd root_fd{
        query.metadata() ? ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC) : -1};
    struct stat root_info{};
    if (query.metadata() && (root_fd.fd < 0 || ::fstat(root_fd.fd, &root_info) != 0)) {
        ++result.stats.unreadable;
        return result;
    }
    const auto keep = [&](FoundFile file) {
        ++result.stats.matches;
        if (result.files.size() == limit) {
            if (!better(file, result.files.front()))
                return;
            std::ranges::pop_heap(result.files, better);
            result.files.pop_back();
        }
        result.files.push_back(std::move(file));
        std::ranges::push_heap(result.files, better);
    };
    for (const auto& batch : batches)
        for (std::size_t i = 0; i < batch->size(); ++i) {
            if (token.cancelled())
                return {};
            const auto relative = batch->path(i);
            const auto rank =
                query.content ? std::optional<PathRank>{PathRank{}} : names.rank(relative);
            if (!rank)
                continue;
            if (!query.extension.empty()) {
                auto extension = std::filesystem::path{relative}.extension().string();
                if (extension.empty() || ascii_lower(extension.substr(1)) != query.extension)
                    continue;
            }
            FoundFile found{
                .path = std::string{relative}, .directory = batch->directory(i), .rank = *rank};
            if (!query.metadata()) {
                keep(std::move(found));
                continue;
            }
            const SearchFd metadata{fs::open_walk_path(root_fd.fd, relative, O_PATH)};
            struct stat info{};
            if (metadata.fd < 0 || ::fstat(metadata.fd, &info) != 0) {
                ++result.stats.unreadable;
                continue;
            }
            if (info.st_dev != root_info.st_dev)
                continue;
            if ((query.type == "file" && !S_ISREG(info.st_mode)) ||
                (query.type == "folder" && !S_ISDIR(info.st_mode)) ||
                (query.type == "link" && !S_ISLNK(info.st_mode)) ||
                (query.after && info.st_mtim.tv_sec < *query.after) ||
                (query.before && info.st_mtim.tv_sec >= *query.before))
                continue;
            if (!query.tag.empty() || !query.comment.empty() || query.rating) {
                const auto annotations = fs::read_annotations(metadata.fd);
                if (!annotations.error.empty()) {
                    ++result.stats.unreadable;
                    continue;
                }
                const auto fold = [&](std::string text) {
                    return query.fold_case ? ascii_lower(text) : text;
                };
                bool tag_match = query.tag.empty();
                std::string_view tags = annotations.tags;
                while (!tags.empty()) {
                    const auto comma = tags.find(',');
                    auto tag = tags.substr(0, comma);
                    while (!tag.empty() && tag.front() == ' ')
                        tag.remove_prefix(1);
                    while (!tag.empty() && tag.back() == ' ')
                        tag.remove_suffix(1);
                    if (fold(std::string{tag}) == fold(query.tag))
                        tag_match = true;
                    tags.remove_prefix(comma == std::string_view::npos ? tags.size() : comma + 1);
                }
                const auto rating = annotations.rating.empty() ? "0" : annotations.rating;
                if (!tag_match ||
                    (!query.comment.empty() &&
                     fold(annotations.comment).find(fold(query.comment)) == std::string::npos) ||
                    (query.rating && rating != std::to_string(*query.rating)))
                    continue;
            }
            found.directory = S_ISDIR(info.st_mode);
            if (!query.content) {
                keep(std::move(found));
                continue;
            }
            if (!S_ISREG(info.st_mode))
                continue;
            if (info.st_size < 0 ||
                static_cast<std::uint64_t>(info.st_size) > SearchBudget::kFileBytes) {
                ++result.stats.oversized;
                continue;
            }
            const auto size = static_cast<std::uint64_t>(info.st_size);
            auto used = budget.bytes.load();
            bool reserved = false;
            while (used <= SearchBudget::kTotalBytes && size <= SearchBudget::kTotalBytes - used) {
                if (budget.bytes.compare_exchange_weak(used, used + size)) {
                    reserved = true;
                    break;
                }
            }
            if (!reserved) {
                result.stats.byte_limit = true;
                continue;
            }
            const SearchFd data{fs::open_walk_path(root_fd.fd, relative, O_RDONLY | O_NONBLOCK)};
            struct stat opened{};
            if (data.fd < 0 || ::fstat(data.fd, &opened) != 0) {
                ++result.stats.unreadable;
                continue;
            }
            if (!same_version(info, opened) || !S_ISREG(opened.st_mode)) {
                ++result.stats.changed;
                continue;
            }
            std::string contents(static_cast<std::size_t>(size), '\0');
            std::size_t read = 0;
            bool failed = false;
            while (read < contents.size()) {
                if (token.cancelled())
                    return {};
                const auto n = ::pread(data.fd, contents.data() + read,
                                       std::min<std::size_t>(64 * 1024, contents.size() - read),
                                       static_cast<off_t>(read));
                if (n < 0 && errno == EINTR)
                    continue;
                if (n <= 0) {
                    failed = true;
                    break;
                }
                read += static_cast<std::size_t>(n);
            }
            if (failed) {
                ++result.stats.unreadable;
                continue;
            }
            struct stat after{};
            if (::fstat(data.fd, &after) != 0 || !same_version(info, after)) {
                ++result.stats.changed;
                continue;
            }
            if (!search_text(contents)) {
                ++result.stats.binary;
                continue;
            }
            const auto match =
                query.fold_case ? ascii_lower(contents).find(needle) : contents.find(needle);
            if (match == std::string::npos)
                continue;
            found.line = 1 + static_cast<std::size_t>(std::count(
                                 contents.begin(),
                                 contents.begin() + static_cast<std::ptrdiff_t>(match), '\n'));
            auto start = match > 60 ? match - 60 : 0;
            while (start < match && (static_cast<unsigned char>(contents[start]) & 0xc0U) == 0x80U)
                ++start;
            auto end = std::min(start + 180, contents.size());
            while (end < contents.size() &&
                   (static_cast<unsigned char>(contents[end]) & 0xc0U) == 0x80U)
                ++end;
            found.context = contents.substr(start, end - start);
            for (char& c : found.context)
                if (static_cast<unsigned char>(c) < 32 || c == 127)
                    c = ' ';
            keep(std::move(found));
        }
    std::ranges::sort(result.files, better);
    return result;
}

std::vector<FoundFile> match_paths(const std::vector<std::shared_ptr<const fs::PathBatch>>& batches,
                                   const std::string& query, std::size_t limit,
                                   const core::CancelToken& token) {
    if (limit == 0)
        return {};
    FilenameQuery names{query};
    std::vector<FoundFile> best;  // a heap: the worst kept on top
    const auto worse = [](const FoundFile& a, const FoundFile& b) { return better(a, b); };
    for (const std::shared_ptr<const fs::PathBatch>& batch : batches) {
        if (token.cancelled()) {
            return {};
        }
        for (std::size_t i = 0; i < batch->size(); ++i) {
            const std::string_view path = batch->path(i);
            const std::optional<PathRank> rank = names.rank(path);
            if (!rank) {
                continue;
            }
            if (best.size() == limit) {
                const FoundFile& last = best.front();
                if (*rank != last.rank ? !better_rank(*rank, last.rank)
                                       : path.size() >= last.path.size()) {
                    continue;
                }
                std::ranges::pop_heap(best, worse);
                best.pop_back();
            }
            best.push_back(
                {.path = std::string{path}, .directory = batch->directory(i), .rank = *rank});
            std::ranges::push_heap(best, worse);
        }
    }
    std::ranges::sort(best, better);
    return best;
}

FileFinder::FileFinder(core::WorkerPool& pool, std::function<void()> changed,
                       fs::WalkOptions options)
    : pool_{pool}, changed_{std::move(changed)}, options_{std::move(options)} {}

FileFinder::~FileFinder() {
    *alive_ = false;
    scan_token_.cancel();
    cancel_matches();
}

void FileFinder::cancel_matches() {
    ++generation_;
    for (const auto& token : match_tokens_)
        token.cancel();
    match_tokens_.clear();
    pending_content_.clear();
    matching_ = 0;
}

void FileFinder::stop() {
    scan_token_.cancel();
    cancel_matches();
    scanning_ = false;
    stopped_ = true;
    walked_.cancelled = true;
    changed_();
}

void FileFinder::refresh() {
    if (recent_)
        start_recent();
    else
        start(root_, hidden_);
}

void FileFinder::filter_recent() {
    results_.clear();
    auto lower = [](std::string value) {
        for (auto& c : value)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return value;
    };
    const auto wanted = lower(query_);
    for (const auto& row : recent_rows_)
        if (wanted.empty() || lower(row.path).find(wanted) != std::string::npos)
            results_.push_back(row);
    entries_ = recent_rows_.size();
    stats_.matches = results_.size();
}

void FileFinder::start_recent() {
    scan_token_.cancel();
    cancel_matches();
    recent_ = true;
    root_ = "/";
    scanning_ = true;
    stopped_ = false;
    recent_rows_.clear();
    results_.clear();
    spec_ = {};
    stats_ = {};
    walked_ = {};
    const auto generation = generation_;
    scan_token_ = pool_.submit(
        core::Priority::visible,
        [](const core::CancelToken& token) {
            fs::RecentFiles history;
            std::vector<FoundFile> rows;
            for (const auto& file : history.load()) {
                if (token.cancelled())
                    break;
                std::array<char, 32> date{};
                const std::time_t time = file.opened;
                std::tm utc{};
                ::gmtime_r(&time, &utc);
                std::strftime(date.data(), date.size(), "%Y-%m-%d %H:%M UTC", &utc);
                rows.push_back({.path = file.path.relative_path().string(),
                                .directory = false,
                                .rank = {},
                                .context = date.data()});
            }
            return std::pair{history.enabled(), rows};
        },
        [this, alive = alive_, generation](std::pair<bool, std::vector<FoundFile>> result) {
            if (!*alive || generation != generation_)
                return;
            scanning_ = false;
            recent_enabled_ = result.first;
            recent_rows_ = std::move(result.second);
            filter_recent();
            changed_();
        });
    changed_();
}

void FileFinder::start(const std::filesystem::path& root, bool hidden) {
    scan_token_.cancel();
    cancel_matches();
    recent_ = false;
    recent_rows_.clear();
    root_ = root;
    hidden_ = hidden;
    spec_ = parse_search_query(query_, hidden);
    batches_.clear();
    entries_ = 0;
    walked_ = {};
    stats_ = {};
    budget_ = std::make_shared<SearchBudget>();
    results_.clear();
    stopped_ = false;
    scanning_ = spec_.error.empty();
    if (!scanning_) {
        changed_();
        return;
    }
    fs::WalkOptions options = options_;
    options.hidden = spec_.hidden;
    options.excluded.insert(options.excluded.end(), spec_.excluded.begin(), spec_.excluded.end());
    scan_token_ = pool_.submit_streaming(
        core::Priority::visible,
        [root = root_, options](const core::CancelToken& token, auto emit) {
            return fs::walk(root, options, token, [&emit](fs::PathBatch batch) {
                emit(std::make_shared<const fs::PathBatch>(std::move(batch)));
            });
        },
        [this, alive = alive_](Batch batch) {
            if (!*alive)
                return;
            entries_ += batch->size();
            batches_.push_back(batch);
            if (spec_.text.empty() && !spec_.metadata())
                fill_nearest();
            else
                match({std::move(batch)});
            changed_();
        },
        [this, alive = alive_](const fs::WalkResult& result) {
            if (!*alive)
                return;
            scanning_ = false;
            walked_ = result;
            changed_();
        });
}

void FileFinder::set_query(const std::string& query) {
    if (query == query_)
        return;
    query_ = query;
    if (recent_) {
        filter_recent();
        changed_();
        return;
    }
    const auto next = parse_search_query(query, hidden_);
    const bool rescan = stopped_ || !spec_.error.empty() || next.hidden != spec_.hidden ||
                        next.excluded != spec_.excluded;
    spec_ = next;
    cancel_matches();
    results_.clear();
    stats_ = {};
    budget_ = std::make_shared<SearchBudget>();
    if (root_.empty()) {
        changed_();
        return;
    }
    if (!spec_.error.empty()) {
        stop();
        return;
    }
    if (rescan) {
        start(root_, hidden_);
        changed_();
        return;
    }
    if (spec_.text.empty() && !spec_.metadata())
        fill_nearest();
    else if (!batches_.empty()) {
        const std::size_t shares =
            std::clamp<std::size_t>(pool_.thread_count(), 1, batches_.size());
        std::vector<std::vector<Batch>> parts(shares);
        for (std::size_t i = 0; i < batches_.size(); ++i)
            parts[i % shares].push_back(batches_[i]);
        for (auto& part : parts)
            match(std::move(part));
    }
    changed_();
}

void FileFinder::match(std::vector<Batch> batches) {
    // Content I/O gets one worker at a time, leaving the others free for previews.
    if (spec_.content && matching_ > 0) {
        pending_content_.insert(pending_content_.end(), batches.begin(), batches.end());
        return;
    }
    ++matching_;
    match_tokens_.push_back(pool_.submit(
        core::Priority::nearby,
        [batches = std::move(batches), root = root_, query = spec_,
         budget = budget_](const core::CancelToken& token) {
            return match_files(root, batches, query, kMaxResults, token, *budget);
        },
        [this, alive = alive_, generation = generation_](SearchMatches found) {
            if (!*alive || generation != generation_)
                return;
            --matching_;
            stats_.matches += found.stats.matches;
            stats_.unreadable += found.stats.unreadable;
            stats_.binary += found.stats.binary;
            stats_.oversized += found.stats.oversized;
            stats_.changed += found.stats.changed;
            stats_.byte_limit = stats_.byte_limit || found.stats.byte_limit;
            merge(std::move(found.files));
            if (!pending_content_.empty())
                match(std::exchange(pending_content_, {}));
            changed_();
        }));
}

void FileFinder::merge(std::vector<FoundFile> found) {
    if (found.empty())
        return;
    std::vector<FoundFile> merged;
    merged.reserve(std::min(results_.size() + found.size(), kMaxResults));
    std::ranges::merge(std::move(results_), std::move(found), std::back_inserter(merged), better);
    if (merged.size() > kMaxResults)
        merged.resize(kMaxResults);
    results_ = std::move(merged);
}

void FileFinder::fill_nearest() {
    results_.clear();
    stats_.matches = entries_;
    for (const Batch& batch : batches_) {
        for (std::size_t i = 0; i < batch->size() && results_.size() < kMaxResults; ++i) {
            results_.push_back({.path = std::string{batch->path(i)},
                                .directory = batch->directory(i),
                                .rank = {}});
        }
        if (results_.size() == kMaxResults)
            break;
    }
}

std::string FileFinder::status() const {
    if (recent_ && !recent_enabled_)
        return std::string{i18n::tr("Recent history is disabled")};
    if (!spec_.error.empty())
        return spec_.error;
    if (spec_.content && spec_.text.empty())
        return std::string{i18n::tr("Enter text after content:")};
    std::string text =
        scanning_
            ? i18n::trn("Searching… {1} item", "Searching… {1} items", entries_, count(entries_))
        : walked_.truncated || stopped_
            ? i18n::trn("Stopped at {1} item", "Stopped at {1} items", entries_, count(entries_))
            : i18n::trn("{1} item", "{1} items", entries_, count(entries_));
    if (walked_.depth_limited)
        text += " · " + std::string{i18n::tr("Search depth limit reached")};
    if (matching())
        text += " · " + std::string{i18n::tr("Matching…")};
    if (stats_.matches > kMaxResults)
        text += " · " + i18n::trf("Showing {} of {} matches", results_.size(), stats_.matches);
    if (walked_.unreadable + stats_.unreadable > 0)
        text += " · " + i18n::trf("Unreadable: {}", walked_.unreadable + stats_.unreadable);
    if (stats_.binary > 0)
        text += " · " + i18n::trf("Non-text files skipped: {}", stats_.binary);
    if (stats_.oversized > 0)
        text += " · " + i18n::trf("Files over 8 MiB skipped: {}", stats_.oversized);
    if (stats_.changed > 0)
        text += " · " + i18n::trf("Changed files skipped: {}", stats_.changed);
    if (stats_.byte_limit)
        text += " · " + std::string{i18n::tr("Content limit reached (512 MiB)")};
    return text;
}

std::size_t FileFinder::bytes() const noexcept {
    std::size_t total = 0;
    for (const Batch& batch : batches_)
        total += batch->bytes();
    return total;
}

}  // namespace rummage::views
