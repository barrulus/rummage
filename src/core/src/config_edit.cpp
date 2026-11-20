#include "rummage/core/config_edit.hpp"

#include <sstream>
#include <stdexcept>
#include <toml++/toml.hpp>

namespace rummage::core {
namespace {
std::size_t offset(std::string_view text, toml::source_position pos) {
    std::size_t at = 0;
    for (std::uint32_t line = 1; line < pos.line; ++line) {
        const auto next = text.find('\n', at);
        if (next == std::string_view::npos)
            throw std::runtime_error("Cannot locate setting in configuration");
        at = next + 1;
    }
    for (std::uint32_t column = 1; column < pos.column && at < text.size(); ++column) {
        ++at;
        while (at < text.size() && (static_cast<unsigned char>(text[at]) & 0xc0U) == 0x80U)
            ++at;
    }
    return at;
}
}  // namespace

std::string quote_config_string(std::string_view text) {
    std::ostringstream out;
    out << toml::toml_formatter{toml::value<std::string>{std::string{text}}};
    return out.str();
}

ConfigText edit_config_text(std::string text, const std::vector<ConfigEdit>& edits,
                            std::string_view append) {
    try {
        for (const auto& edit : edits) {
            auto doc = toml::parse(text);
            if (edit.remove) {
                auto* table = doc.at_path(edit.table).as_table();
                if (table && table->contains(edit.key)) {
                    if (table->is_inline()) {
                        const auto begin = offset(text, table->source().begin);
                        const auto end = offset(text, table->source().end);
                        table->erase(edit.key);
                        std::ostringstream out;
                        out << toml::toml_formatter{*table};
                        text.replace(begin, end - begin, out.str());
                    } else {
                        const auto key = table->find(edit.key);
                        const auto start = offset(text, key->first.source().begin);
                        const auto newline =
                            start == 0 ? std::string::npos : text.rfind('\n', start - 1);
                        const auto begin = newline == std::string::npos ? 0 : newline + 1;
                        const auto end = offset(text, key->second.source().end);
                        text.erase(begin, end - begin);
                    }
                }
                continue;
            }
            const auto value = toml::parse("value = " + edit.value);
            if (value.size() != 1 || !value.contains("value"))
                throw std::runtime_error("Enter one value for each setting");
            auto* table = doc.at_path(edit.table).as_table();
            const bool motion_table = edit.table == "animations.events" ||
                                      edit.table == "animations.durations" ||
                                      edit.table == "animations.ambient";
            const auto table_key = motion_table ? quote_config_string("animations") + "." +
                                                      quote_config_string(edit.table.substr(11))
                                                : quote_config_string(edit.table);
            // Inline parents cannot be extended by a later [table] declaration. Reformat
            // only that parent when adding a missing nested motion setting.
            if (motion_table && (!table || !table->contains(edit.key))) {
                auto* parent = doc.get_as<toml::table>("animations");
                if (parent && parent->is_inline()) {
                    const auto begin = offset(text, parent->source().begin);
                    const auto end = offset(text, parent->source().end);
                    const auto child_key = edit.table.substr(11);
                    if (!parent->contains(child_key))
                        parent->insert(child_key, toml::table{});
                    auto* child = parent->get_as<toml::table>(child_key);
                    if (!child)
                        throw std::runtime_error("Motion settings section must be a table");
                    child->insert(edit.key, *value.get("value"));
                    std::ostringstream out;
                    out << toml::toml_formatter{*parent};
                    text.replace(begin, end - begin, out.str());
                    continue;
                }
            }
            if (table) {
                if (const auto* node = table->get(edit.key)) {
                    const auto begin = offset(text, node->source().begin);
                    const auto end = offset(text, node->source().end);
                    text.replace(begin, end - begin, edit.value);
                } else if (table->is_inline()) {
                    const auto begin = offset(text, table->source().begin);
                    const auto end = offset(text, table->source().end);
                    table->insert(edit.key, *value.get("value"));
                    std::ostringstream out;
                    out << toml::toml_formatter{*table};
                    text.replace(begin, end - begin, out.str());
                } else {
                    const auto begin = offset(text, table->source().begin);
                    bool explicit_header = begin < text.size() && text[begin] == '[';
                    if (explicit_header && edit.table.find_first_of("[]") == std::string::npos) {
                        // An implicit parent inherits its child's source position. Inserting
                        // after [animations.events] would put enabled inside events instead.
                        const auto newline = text.find('\n', begin);
                        const auto header = toml::parse(text.substr(begin, newline - begin));
                        const auto* declared = header.at_path(edit.table).as_table();
                        explicit_header = declared && declared->empty();
                    }
                    if (!explicit_header) {
                        if (!motion_table && edit.table.find_first_of(".[]") != std::string::npos)
                            throw std::runtime_error("Cannot insert into an implicit action table");
                        text.insert(0, table_key + "." + quote_config_string(edit.key) + " = " +
                                           edit.value + "\n");
                        continue;
                    }
                    const auto newline = text.find('\n', begin);
                    const auto insertion =
                        quote_config_string(edit.key) + " = " + edit.value + "\n";
                    if (newline == std::string::npos)
                        text += "\n" + insertion;
                    else
                        text.insert(newline + 1, insertion);
                }
            } else {
                // New top-level sections and the supported nested motion sections.
                if ((!motion_table && edit.table.find_first_of(".[]") != std::string::npos) ||
                    doc.contains(edit.table))
                    throw std::runtime_error("Cannot create this settings section");
                text += "\n[" + table_key + "]\n" + quote_config_string(edit.key) + " = " +
                        edit.value + "\n";
            }
        }
        text += append;
        static_cast<void>(toml::parse(text));
        return {.text = std::move(text), .error = {}};
    } catch (const std::exception& error) {
        return {.text = {}, .error = error.what()};
    }
}
}  // namespace rummage::core
