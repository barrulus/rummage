#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>

#include "rummage/platform/code_stats.hpp"

namespace rummage::platform {
namespace {
constexpr std::size_t kMaxJson = 16 * 1024 * 1024;
constexpr std::size_t kMaxReports = 200'000;
constexpr std::size_t kMaxLanguages = 1024;

void require(bool condition) {
    if (!condition)
        throw std::runtime_error{"Invalid source report"};
}

using Json = nlohmann::json;

const Json& field(const Json& value, const char* key, Json::value_t type) {
    require(value.is_object());
    const auto& result = value.at(key);
    require(result.type() == type);
    return result;
}

std::uint64_t count(const Json& value, const char* key) {
    require(value.is_object());
    const auto& number = value.at(key);
    require(number.is_number_unsigned() ||
            (number.is_number_integer() && number.get<std::int64_t>() >= 0));
    return number.get<std::uint64_t>();
}

void add(std::uint64_t& a, std::uint64_t b) {
    require(b <= std::numeric_limits<std::uint64_t>::max() - a);
    a += b;
}

void add(CodeCounts& a, const CodeCounts& b) {
    add(a.code, b.code);
    add(a.comments, b.comments);
    add(a.blanks, b.blanks);
    auto lines = a.code;
    add(lines, a.comments);
    add(lines, a.blanks);
}

CodeCounts counts(const Json& value) {
    CodeCounts result;
    add(result, {count(value, "code"), count(value, "comments"), count(value, "blanks")});
    return result;
}

std::filesystem::path report_path(const Json& report, const std::filesystem::path& root) {
    const auto& text = field(report, "name", Json::value_t::string).get_ref<const std::string&>();
    require(!text.empty() && text.size() <= 32 * 1024 && text.find('\0') == std::string::npos);
    auto path = std::filesystem::path{text}.lexically_normal();
    if (path.is_absolute())
        path = path.lexically_relative(root);
    require(!path.empty() && path != "." && !path.is_absolute());
    for (const auto& part : path)
        require(part != "..");
    return path;
}
}  // namespace

CodeReport parse_code_report(std::string_view json, const std::filesystem::path& root,
                             const core::CancelToken& token) {
    CodeReport result;
    result.root = root;
    if (json.size() > kMaxJson) {
        result.error = CodeError::output_limit;
        return result;
    }
    try {
        const auto object = Json::parse(json, [&](int depth, Json::parse_event_t, Json&) {
            require(depth <= 48 && !token.cancelled());
            return true;
        });
        require(object.is_object());
        std::map<std::string, std::size_t> groups;
        std::set<std::filesystem::path> physical_files;
        std::size_t reports = 0;
        const auto collect = [&](auto&& self, const std::string& name, const std::string& parent,
                                 std::size_t depth, const std::filesystem::path& path,
                                 const Json& stats) -> void {
            require(!token.cancelled() && depth <= 12 && ++reports <= kMaxReports &&
                    !name.empty() && name.size() <= 256);
            const auto key = parent + '\x1f' + name;
            auto [entry, inserted] = groups.try_emplace(key, result.languages.size());
            if (inserted) {
                require(groups.size() <= kMaxLanguages);
                result.languages.push_back(
                    {.name = name, .key = key, .depth = depth, .counts = {}, .files = {}});
            }
            const auto value = counts(stats);
            auto& language = result.languages[entry->second];
            add(language.counts, value);
            language.files.push_back({path, value});
            add(result.total, value);
            const auto& blobs = field(stats, "blobs", Json::value_t::object);
            for (const auto& [child_name, child] : blobs.items()) {
                self(self, child_name, key, depth + 1, path, child);
            }
        };
        for (const auto& [language_name, language] : object.items()) {
            if (std::string_view{language_name} == "Total")
                continue;
            require(!token.cancelled());
            const auto& inaccurate = field(language, "inaccurate", Json::value_t::boolean);
            result.inaccurate = result.inaccurate || inaccurate.get<bool>();
            const auto& files = field(language, "reports", Json::value_t::array);
            CodeCounts reported;
            for (const auto& file : files) {
                const auto path = report_path(file, root);
                require(physical_files.insert(path).second);
                const auto& stats = field(file, "stats", Json::value_t::object);
                add(reported, counts(stats));
                collect(collect, language_name, {}, 0, path, stats);
            }
            require(reported == counts(language));
        }
        require(result.total == counts(field(object, "Total", Json::value_t::object)));
        result.files = physical_files.size();
    } catch (const std::exception&) {
        result.languages.clear();
        result.total = {};
        result.files = 0;
        result.error = token.cancelled() ? CodeError::stopped : CodeError::invalid_output;
    }
    return result;
}

}  // namespace rummage::platform
