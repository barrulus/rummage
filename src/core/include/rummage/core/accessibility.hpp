#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace rummage::core {
// AT-SPI roles are wire constants, not a toolkit ABI. This snapshot contains no toolkit objects.
enum class AccessibleRole : std::uint32_t {
    alert = 2,
    check_box = 7,
    dialog = 16,
    frame = 23,
    label = 29,
    list = 31,
    list_item = 32,
    menu = 33,
    menu_item = 35,
    tab = 37,
    tab_list = 38,
    panel = 39,
    password = 40,
    button = 43,
    radio_button = 44,
    status = 54,
    text = 61,
    application = 75,
    entry = 79
};

struct AccessibleRect {
    int x{}, y{}, width{}, height{};
};

struct AccessibleEdit {
    enum class Kind {
        focus,
        activate,
        select,
        set_text,
        set_selection,
        copy,
        cut,
        paste
    } kind{Kind::activate};
    std::string text;
    int first{}, last{};  // Unicode character offsets, never UTF-8 bytes
};

struct AccessibleNode {
    std::string id, parent, name, description;
    AccessibleRole role{AccessibleRole::label};
    AccessibleRect bounds;
    bool focused{}, selected{}, enabled{true}, editable{}, password{}, checkable{}, checked{};
    std::string text;
    int caret{}, anchor{};
    std::function<bool(const AccessibleEdit&)> perform;
};

inline std::size_t text_byte_offset(std::string_view text, int characters) {
    std::size_t at = 0;
    for (int n = 0; at < text.size() && n < characters; ++n) {
        ++at;
        while (at < text.size() && (static_cast<unsigned char>(text[at]) & 0xc0U) == 0x80U)
            ++at;
    }
    return at;
}

inline int text_character_offset(std::string_view text, std::size_t bytes) {
    int count = 0;
    for (std::size_t at = 0; at < bytes && at < text.size(); ++at)
        if ((static_cast<unsigned char>(text[at]) & 0xc0U) != 0x80U)
            ++count;
    return count;
}
}  // namespace rummage::core
