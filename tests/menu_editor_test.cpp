#include "rummage/views/menu_editor.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/core/config_edit.hpp"
#include "rummage/core/i18n.hpp"
#include "rummage/ui/offscreen.hpp"

using namespace rummage;

namespace {
struct Editor {
    std::string text;
    views::MenuEditor editor{[] {},
                             [this](const auto& edits, std::string_view append) {
                                 const auto next = core::edit_config_text(text, edits, append);
                                 if (!next.error.empty())
                                     return next.error;
                                 const auto parsed = core::parse_config(next.text, "test.toml");
                                 if (!parsed.context_menu_valid)
                                     return std::string{"Invalid menu"};
                                 text = next.text;
                                 return std::string{};
                             }};

    Editor() { editor.sync(core::parse_config(text, "test.toml").config, true); }
};
}  // namespace

TEST_CASE("menu editor: edits nested layouts and preserves unrelated configuration") {
    Editor e;
    e.text = "# keep me\n[theme]\nmode = 'dark'\n";
    e.editor.context(1);
    REQUIRE(e.editor.add({.id = "open"}));
    e.editor.select(1);
    REQUIRE(e.editor.group("My tools"));
    REQUIRE(e.editor.add({.id = "copy"}));
    e.editor.select(3);
    REQUIRE(e.editor.nest());
    auto parsed = core::parse_config(e.text, "test.toml");
    REQUIRE(parsed.context_menu_valid);
    REQUIRE(parsed.config.context_menu.entry);
    const auto& nodes = *parsed.config.context_menu.entry;
    REQUIRE(nodes.size() == 2);
    REQUIRE(nodes[1].items.size() == 2);
    CHECK(nodes[1].items[1].id == "copy");
    CHECK(e.text.find("# keep me") != std::string::npos);
    CHECK(e.text.find("mode = 'dark'") != std::string::npos);
    e.editor.select(3);
    REQUIRE(e.editor.unnest());
    REQUIRE(e.editor.move(-1));
    REQUIRE(e.editor.remove());
    REQUIRE(e.editor.reset());
    CHECK_FALSE(core::parse_config(e.text, "test.toml").config.context_menu.entry);
}

TEST_CASE("menu editor: validation failure keeps the accepted layout") {
    Editor e;
    REQUIRE(e.editor.add({.id = "open"}));
    const auto text = e.text;
    CHECK_FALSE(e.editor.add({.id = "open"}));
    CHECK(e.text == text);
    CHECK(e.editor.nodes().size() == 2);
    CHECK_FALSE(e.editor.add({.remaining = true}));
    CHECK(e.text == text);
}

TEST_CASE("menu editor: explicit empty differs from inheritance and read-only prevents mutation") {
    Editor e;
    e.editor.context(2);
    REQUIRE(e.editor.remove());
    auto parsed = core::parse_config(e.text, "test.toml");
    REQUIRE(parsed.config.context_menu.background);
    CHECK(parsed.config.context_menu.background->empty());
    e.editor.sync(parsed.config, false);
    const auto text = e.text;
    CHECK_FALSE(e.editor.add({.id = "copy"}));
    CHECK_FALSE(e.editor.reset());
    CHECK(e.text == text);
    e.editor.sync(parsed.config, true);
    REQUIRE(e.editor.reset());
    CHECK_FALSE(core::parse_config(e.text, "test.toml").config.context_menu.background);
}

TEST_CASE("menu editor: serialization retains exact labels and nested dynamic references") {
    const std::vector<core::MenuNode> nodes{
        {.label = "A \"group\"",
         .items = {
             {.id = "open_with"}, {.id = "new_file", .separator = true}, {.remaining = true}}}};
    const auto parsed = core::parse_config(
        "[context_menu]\nitems = " + views::MenuEditor::serialize(nodes), "test.toml");
    REQUIRE(parsed.context_menu_valid);
    REQUIRE(parsed.config.context_menu.items);
    CHECK(views::MenuEditor::serialize(*parsed.config.context_menu.items) ==
          views::MenuEditor::serialize(nodes));
}

TEST_CASE("menu editor: empty layouts do not redraw on every status sync") {
    auto config = core::parse_config("[context_menu]\nitems=[]\n", "test.toml").config;
    int redraws = 0;
    views::MenuEditor editor{[&] { ++redraws; },
                             [](const auto&, std::string_view) { return std::string{}; }};
    editor.sync(config, true);
    const auto initial = redraws;
    for (int i = 0; i < 20; ++i)
        editor.sync(config, true);
    CHECK(redraws == initial);
}

TEST_CASE("menu editor: rejected labels remain drafts and readonly labels can be copied") {
    Editor e;
    REQUIRE(e.editor.add({.id = "open"}));
    e.editor.select(1);
    REQUIRE(e.editor.group("Tools"));
    const auto saved = e.text;
    CHECK_FALSE(e.editor.rename(""));
    CHECK(e.editor.has_draft());
    CHECK_FALSE(e.editor.error().empty());
    CHECK_FALSE(e.editor.commit());
    CHECK(e.text == saved);
    e.editor.discard();
    e.editor.sync(core::parse_config(e.text, "test.toml").config, false);
    e.editor.select(1);
    e.editor.enter_focus(true);
    e.editor.key({.keysym = XKB_KEY_a, .text = {}, .modifiers = {.ctrl = true}});
    CHECK(e.editor.copy_selection(true) == "Tools");
    e.editor.paste("Changed");
    e.editor.key({.keysym = XKB_KEY_BackSpace, .text = {}});
    CHECK_FALSE(e.editor.has_draft());
    CHECK(e.text == saved);
    e.editor.select(0);  // Remaining commands have no editable label.
    e.editor.enter_focus(true);
    e.editor.paste("Invalid label");
    CHECK_FALSE(e.editor.has_draft());
}

TEST_CASE("menu editor: nesting and unnesting preserve the moved command selection") {
    Editor e;
    REQUIRE(e.editor.add({.id = "open"}));
    e.editor.select(1);
    REQUIRE(e.editor.group("Tools"));
    REQUIRE(e.editor.add({.id = "copy"}));
    e.editor.select(3);
    REQUIRE(e.editor.nest());
    REQUIRE(e.editor.rename("Nested copy"));
    REQUIRE(e.editor.unnest());
    REQUIRE(e.editor.rename("Moved copy"));
    const auto parsed = core::parse_config(e.text, "test.toml");
    REQUIRE(parsed.config.context_menu.items);
    const auto& nodes = *parsed.config.context_menu.items;
    REQUIRE(nodes.size() == 3);
    CHECK(nodes[2].id == "copy");
    CHECK(nodes[2].label == "Moved copy");
    CHECK(nodes[1].label == "Tools");
}

TEST_CASE("menu editor: compact page scrolls to labels and keeps popup dismissal local") {
    Editor e;
    REQUIRE(e.editor.add({.id = "open", .label = "Open item"}));
    e.editor.select(1);
    ui::Offscreen gl{400, 360};
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    const auto draw = [&](const std::string& name) {
        renderer.begin_frame(400, 360, 1, {});
        e.editor.draw(renderer, {12, 164, 376, 132}, theme::Palette{});
        renderer.end_frame();
        if (const char* output = std::getenv("RUMMAGE_SETTINGS_FRAMES")) {
            std::vector<unsigned char> pixels(400 * 360 * 4);
            glReadPixels(0, 0, 400, 360, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            std::ofstream file{std::filesystem::path{output} / (name + ".ppm"), std::ios::binary};
            file << "P6\n400 360\n255\n";
            for (int y = 359; y >= 0; --y)
                for (int x = 0; x < 400; ++x)
                    file.write(reinterpret_cast<const char*>(pixels.data() + (y * 400 + x) * 4), 3);
        }
    };
    e.editor.enter_focus();
    draw("compact-menu-top");
    e.editor.enter_focus(true);
    draw("compact-menu-label");
    e.editor.key({.keysym = XKB_KEY_a, .text = {}, .modifiers = {.ctrl = true}});
    e.editor.paste("Edited label");
    REQUIRE(e.editor.commit());
    CHECK(e.editor.nodes()[1].label == "Edited label");
    CHECK_FALSE(e.editor.key({.keysym = XKB_KEY_Tab, .text = {}}));  // Return to host footer.
    e.editor.enter_focus();
    draw("compact-menu-returned");
    e.editor.key({.keysym = XKB_KEY_Return, .text = {}});
    REQUIRE(e.editor.popup_open());
    draw("compact-menu-popup");
    e.editor.pointer({.kind = ui::PointerEvent::Kind::press,
                      .x = 390,
                      .y = 330,
                      .button = ui::PointerButton::left});
    CHECK_FALSE(e.editor.popup_open());
}

TEST_CASE("menu editor: rejected persistence preserves label draft and accepted tree") {
    const auto config =
        core::parse_config("[context_menu]\nitems=[{id='open',label='Original'}]\n", "test.toml")
            .config;
    views::MenuEditor editor{
        [] {}, [](const auto&, std::string_view) { return std::string{"Cannot save"}; }};
    editor.sync(config, true);
    editor.enter_focus(true);
    editor.key({.keysym = XKB_KEY_a, .text = {}, .modifiers = {.ctrl = true}});
    editor.paste("Rejected draft");
    CHECK_FALSE(editor.commit());
    CHECK(editor.has_draft());
    CHECK(editor.error() == "Cannot save");
    CHECK(editor.nodes().front().label == "Original");
    editor.key({.keysym = XKB_KEY_a, .text = {}, .modifiers = {.ctrl = true}});
    CHECK(editor.copy_selection(false) == "Rejected draft");
}

TEST_CASE("menu editor: complete picker catalog uses names and valid command references") {
    Editor e;
    const auto catalog = e.editor.command_catalog();
    std::vector<core::MenuNode> nodes;
    std::size_t builtins = 0;
    for (const auto& command : catalog) {
        REQUIRE_FALSE(command.id.empty());
        CHECK_FALSE(command.action);
        CHECK_FALSE(command.submenu);
        CHECK(command.children.empty());
        CHECK(command.label != command.id);
        nodes.push_back({.id = command.id});
        if (!command.id.starts_with("action:"))
            ++builtins;
    }
    CHECK(builtins == 53);
    const auto parsed = core::parse_config(
        "[context_menu]\nitems=" + views::MenuEditor::serialize(nodes), "test.toml");
    CHECK(parsed.context_menu_valid);
    for (const auto id : {"running", "compress_zip", "compress_tar", "sort:accessed",
                          "sort:link_target", "sort:descending", "copy_other_pane", "orient_panes"})
        CHECK(std::ranges::find(catalog, id, &views::MenuItem::id) != catalog.end());
}

TEST_CASE("menu editor: representative previews retain dynamic submenus and never run commands") {
    Editor e;
    const auto preview = e.editor.preview_items();
    for (const auto id : {"open_with", "new_file", "sort", "panes", "actions"}) {
        const auto item = std::ranges::find(preview, id, &views::MenuItem::id);
        REQUIRE(item != preview.end());
        CHECK(item->submenu);
        CHECK_FALSE(item->children.empty());
    }
    const auto safe = [&](const auto& self, const auto& items) -> void {
        for (const auto& item : items) {
            CHECK_FALSE(item.action);
            self(self, item.children);
        }
    };
    safe(safe, preview);
    e.editor.context(2);
    const auto background = e.editor.preview_items();
    CHECK(std::ranges::find(background, "new_file", &views::MenuItem::id) != background.end());
    CHECK(std::ranges::find(background, "open_with", &views::MenuItem::id) == background.end());
    e.editor.context(3);
    const auto trash = e.editor.preview_items();
    CHECK(std::ranges::find(trash, "restore", &views::MenuItem::id) != trash.end());
    CHECK(std::ranges::find(trash, "paste", &views::MenuItem::id) == trash.end());
}

TEST_CASE("menu editor: translated catalog preserves literal action and layout labels") {
    struct RestoreLanguage {
        ~RestoreLanguage() { i18n::set_language("en"); }
    } restore;

    i18n::set_language("de");
    Editor e;
    e.text =
        "[[actions]]\nid='custom'\nlabel='Open'\nargv=['echo']\n"
        "[context_menu]\nitems=[{id='copy',label='Copy'},{remaining=true}]\n";
    e.editor.sync(core::parse_config(e.text, "test.toml").config, true);
    const auto catalog = e.editor.command_catalog();
    const auto open = std::ranges::find(catalog, "open", &views::MenuItem::id);
    REQUIRE(open != catalog.end());
    CHECK(open->label == "Öffnen");
    const auto action = std::ranges::find(catalog, "action:custom", &views::MenuItem::id);
    REQUIRE(action != catalog.end());
    CHECK(action->label == "Aktionen › Open");
    const auto preview = e.editor.preview_items();
    REQUIRE_FALSE(preview.empty());
    CHECK(preview.front().id == "copy");
    CHECK(preview.front().label == "Copy");
}
