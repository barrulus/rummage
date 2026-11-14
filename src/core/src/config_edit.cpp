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
                    if (begin >= text.size() || text[begin] != '[') {
                        if (edit.table.find_first_of(".[]") != std::string::npos)
                            throw std::runtime_error("Cannot insert into an implicit action table");
                        text.insert(0, quote_config_string(edit.table) + "." +
                                           quote_config_string(edit.key) + " = " + edit.value +
                                           "\n");
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
                // Only new top-level sections may be created this way.
                if (edit.table.find_first_of(".[]") != std::string::npos ||
                    doc.contains(edit.table))
                    throw std::runtime_error("Cannot create this settings section");
                text += "\n[" + quote_config_string(edit.table) + "]\n" +
                        quote_config_string(edit.key) + " = " + edit.value + "\n";
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
