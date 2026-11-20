#include "rummage/views/shortcuts.hpp"

#include <algorithm>
#include <deque>
#include <format>
#include <optional>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "rummage/core/config.hpp"
#include "rummage/core/i18n.hpp"

namespace rummage::views {
namespace {

constexpr ui::Modifiers kCtrl{.ctrl = true};
constexpr ui::Modifiers kShift{.shift = true};
constexpr ui::Modifiers kAlt{.alt = true};
constexpr ui::Modifiers kCtrlShift{.ctrl = true, .shift = true};

// The keys that move the cursor; Left and Right move it only in the grid (in the columns and the
// details they go up and in), but Shift and Ctrl work with all of them.
constexpr std::array<xkb_keysym_t, 8> kMoves{XKB_KEY_Up,    XKB_KEY_Down,    XKB_KEY_Left,
                                             XKB_KEY_Right, XKB_KEY_Page_Up, XKB_KEY_Page_Down,
                                             XKB_KEY_Home,  XKB_KEY_End};

constexpr std::array kShortcuts{
    // General
    Shortcut{.group = i18n::mark("General"),
             .keys = "Ctrl+1",
             .action = i18n::mark("Columns"),
             .scope = scope::kViews | scope::kSidebar | scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_1, XKB_KEY_KP_1},
             .command = Command::columns_view},
    Shortcut{.group = i18n::mark("General"),
             .keys = "Ctrl+2",
             .action = i18n::mark("Grid"),
             .scope = scope::kViews | scope::kSidebar | scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_2, XKB_KEY_KP_2},
             .command = Command::grid_view},
    Shortcut{.group = i18n::mark("General"),
             .keys = "Ctrl+3",
             .action = i18n::mark("Details"),
             .scope = scope::kViews | scope::kSidebar | scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_3, XKB_KEY_KP_3},
             .command = Command::details_view},
    Shortcut{.group = i18n::mark("General"),
             .keys = "Space",
             .action = i18n::mark("Quick Look"),
             .scope = scope::kViews | scope::kQuickLook,
             .keysyms = {XKB_KEY_space},
             .command = Command::quick_look},
    Shortcut{.group = i18n::mark("General"),
             .keys = "Ctrl+I",
             .action = i18n::mark("Info pane"),
             .scope = scope::kViews | scope::kSidebar | scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_i, XKB_KEY_I},
             .command = Command::toggle_info},
    Shortcut{.group = i18n::mark("General"),
             .keys = "Alt+K",
             .action = i18n::mark("Scroll the info pane's text up"),
             .scope = scope::kViews,
             .modifiers = kAlt,
             .keysyms = {XKB_KEY_k, XKB_KEY_K},
             .command = Command::scroll_preview_up},
    Shortcut{.group = i18n::mark("General"),
             .keys = "Alt+J",
             .action = i18n::mark("Scroll the info pane's text down"),
             .scope = scope::kViews,
             .modifiers = kAlt,
             .keysyms = {XKB_KEY_j, XKB_KEY_J},
             .command = Command::scroll_preview_down},
    Shortcut{.group = i18n::mark("General"),
             .keys = "Alt+PgUp",
             .action = i18n::mark("Scroll the info pane's text a page up"),
             .scope = scope::kViews,
             .modifiers = kAlt,
             .keysyms = {XKB_KEY_Page_Up},
             .command = Command::scroll_preview_page_up},
    Shortcut{.group = i18n::mark("General"),
             .keys = "Alt+PgDn",
             .action = i18n::mark("Scroll the info pane's text a page down"),
             .scope = scope::kViews,
             .modifiers = kAlt,
             .keysyms = {XKB_KEY_Page_Down},
             .command = Command::scroll_preview_page_down},
    Shortcut{.group = i18n::mark("General"),
             .keys = "Ctrl+B",
             .action = i18n::mark("Sidebar"),
             .scope = scope::kViews | scope::kSidebar | scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_b, XKB_KEY_B},
             .command = Command::toggle_sidebar},
    Shortcut{.group = i18n::mark("General"),
             .keys = "Ctrl+H",
             .action = i18n::mark("Hidden files"),
             .scope = scope::kViews | scope::kSidebar | scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_h, XKB_KEY_H},
             .command = Command::toggle_hidden},
    Shortcut{.group = i18n::mark("General"),
             .keys = "",
             .action = i18n::mark("Thumbnails"),
             .scope = scope::kViews | scope::kSidebar | scope::kQuickLook,
             .modifiers = {},
             .keysyms = {XKB_KEY_NoSymbol},
             .command = Command::toggle_thumbnails},
    Shortcut{.group = i18n::mark("General"),
             .keys = "",
             .action = i18n::mark("File extensions"),
             .scope = scope::kViews | scope::kSidebar | scope::kQuickLook,
             .modifiers = {},
             .keysyms = {XKB_KEY_NoSymbol},
             .command = Command::toggle_extensions},
    Shortcut{.group = i18n::mark("General"),
             .keys = "Ctrl+D",
             .action = i18n::mark("Add the folder to the sidebar"),
             .scope = scope::kViews | scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_d, XKB_KEY_D},
             .command = Command::add_bookmark},
    Shortcut{.group = i18n::mark("General"),
             .keys = "Ctrl+L",
             .action = i18n::mark("Go to a folder or a server"),
             .scope = scope::kViews | scope::kSidebar | scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_l, XKB_KEY_L},
             .command = Command::go_to},
    Shortcut{.group = i18n::mark("General"),
             .keys = "F6",
             .action = i18n::mark("Into the sidebar and out"),
             .scope = scope::kViews | scope::kSidebar,
             .keysyms = {XKB_KEY_F6},
             .command = Command::focus_sidebar},
    Shortcut{.group = i18n::mark("General"),
             .keys = "Ctrl+K",
             .action = i18n::mark("Command palette"),
             .scope = scope::kViews | scope::kSidebar | scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_k, XKB_KEY_K},
             .command = Command::command_palette},
    Shortcut{.group = i18n::mark("General"),
             .keys = "Ctrl+Shift+F",
             .action = i18n::mark("Filter names in the view"),
             .scope = scope::kViews | scope::kSidebar,
             .modifiers = kCtrlShift,
             .keysyms = {XKB_KEY_f, XKB_KEY_F},
             .command = Command::filter_names},
    Shortcut{.group = i18n::mark("General"),
             .keys = "Ctrl+F",
             .action = i18n::mark("Find files in the folder"),
             .scope = scope::kViews | scope::kSidebar | scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_f, XKB_KEY_F},
             .command = Command::find_files},
    Shortcut{.group = i18n::mark("General"),
             .keys = "F1  ?",
             .action = i18n::mark("These shortcuts"),
             .scope = scope::kViews | scope::kSidebar | scope::kQuickLook,
             .any_shift = true,
             .keysyms = {XKB_KEY_F1, XKB_KEY_question},
             .command = Command::show_shortcuts},
    Shortcut{.group = i18n::mark("General"),
             .keys = "Menu  Shift+F10",
             .action = i18n::mark("Context menu"),
             .scope = scope::kViews,
             .any_shift = true,
             .keysyms = {XKB_KEY_Menu, XKB_KEY_F10},
             .command = Command::context_menu},
    Shortcut{.group = i18n::mark("General"),
             .keys = "Alt+Enter",
             .action = i18n::mark("Properties"),
             .scope = scope::kViews,
             .modifiers = {.alt = true},
             .keysyms = {XKB_KEY_Return, XKB_KEY_KP_Enter},
             .command = Command::properties},
    Shortcut{.group = i18n::mark("General"),
             .keys = "Ctrl+Q",
             .action = i18n::mark("Quit"),
             .scope = scope::kWindow,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_q, XKB_KEY_Q}},

    // Moving
    Shortcut{.group = i18n::mark("Moving"),
             .keys = "↑  ↓",
             .action = i18n::mark("Previous, next"),
             .scope = scope::kViews,
             .keysyms = {XKB_KEY_Up, XKB_KEY_Down}},
    Shortcut{.group = i18n::mark("Moving"),
             .keys = "←  →",
             .action = i18n::mark("Previous, next (grid)"),
             .scope = scope::kGrid,
             .keysyms = {XKB_KEY_Left, XKB_KEY_Right}},
    Shortcut{.group = i18n::mark("Moving"),
             .keys = "PgUp  PgDn",
             .action = i18n::mark("A screenful"),
             .scope = scope::kViews,
             .keysyms = {XKB_KEY_Page_Up, XKB_KEY_Page_Down}},
    Shortcut{.group = i18n::mark("Moving"),
             .keys = "Home  End",
             .action = i18n::mark("First, last"),
             .scope = scope::kViews,
             .keysyms = {XKB_KEY_Home, XKB_KEY_End}},
    Shortcut{.group = i18n::mark("Moving"),
             .keys = "Enter",
             .action = i18n::mark("Open (files in their app)"),
             .scope = scope::kViews,
             .keysyms = {XKB_KEY_Return, XKB_KEY_KP_Enter}},
    Shortcut{.group = i18n::mark("Moving"),
             .keys = "→",
             .action = i18n::mark("Open (columns), expand (details)"),
             .scope = scope::kColumns | scope::kDetails,
             .keysyms = {XKB_KEY_Right}},
    Shortcut{.group = i18n::mark("Moving"),
             .keys = "Alt+↓",
             .action = i18n::mark("Open (grid, details)"),
             .scope = scope::kGrid | scope::kDetails,
             .modifiers = kAlt,
             .keysyms = {XKB_KEY_Down}},
    Shortcut{.group = i18n::mark("Moving"),
             .keys = "F4",
             .action = i18n::mark("Open in a terminal"),
             .scope = scope::kViews | scope::kQuickLook,
             .keysyms = {XKB_KEY_F4},
             .command = Command::open_terminal},
    Shortcut{.group = i18n::mark("Moving"),
             .keys = "Backspace",
             .action = i18n::mark("Parent folder"),
             .scope = scope::kViews,
             .keysyms = {XKB_KEY_BackSpace}},
    Shortcut{.group = i18n::mark("Moving"),
             .keys = "←",
             .action = i18n::mark("Parent (columns), collapse / parent (details)"),
             .scope = scope::kColumns | scope::kDetails,
             .keysyms = {XKB_KEY_Left}},
    Shortcut{.group = i18n::mark("Moving"),
             .keys = "Alt+↑",
             .action = i18n::mark("Parent folder"),
             .scope = scope::kViews,
             .modifiers = kAlt,
             .keysyms = {XKB_KEY_Up}},
    Shortcut{.group = i18n::mark("Moving"),
             .keys = "Alt+←  Alt+→",
             .action = i18n::mark("Back, forward (mouse too)"),
             .scope = scope::kViews | scope::kSidebar,
             .modifiers = kAlt,
             .keysyms = {XKB_KEY_Left},
             .command = Command::back},
    Shortcut{.group = i18n::mark("Moving"),
             .keys = "",
             .action = i18n::mark("Forward"),
             .scope = scope::kViews | scope::kSidebar,
             .modifiers = kAlt,
             .keysyms = {XKB_KEY_Right},
             .command = Command::forward},

    // Selecting
    Shortcut{.group = i18n::mark("Selecting"),
             .keys = "Shift+moves",
             .action = i18n::mark("Select a range"),
             .scope = scope::kViews,
             .modifiers = kShift,
             .keysyms = kMoves},
    Shortcut{.group = i18n::mark("Selecting"),
             .keys = "Ctrl+moves",
             .action = i18n::mark("Move without selecting"),
             .scope = scope::kViews,
             .modifiers = kCtrl,
             .keysyms = kMoves},
    Shortcut{.group = i18n::mark("Selecting"),
             .keys = "Ctrl+Space",
             .action = i18n::mark("Select or unselect"),
             .scope = scope::kViews,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_space}},
    Shortcut{.group = i18n::mark("Selecting"),
             .keys = "Ctrl+A",
             .action = i18n::mark("Select all"),
             .scope = scope::kViews,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_a, XKB_KEY_A}},
    Shortcut{.group = i18n::mark("Selecting"),
             .keys = "Escape",
             .action = i18n::mark("Back to one entry"),
             .scope = scope::kViews,
             .keysyms = {XKB_KEY_Escape}},

    Shortcut{.group = i18n::mark("General"),
             .keys = "Ctrl+,",
             .action = i18n::mark("Settings"),
             .scope = scope::kViews | scope::kSidebar,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_comma},
             .command = Command::show_settings},
    // Panes
    Shortcut{.group = i18n::mark("Panes"),
             .keys = "F3",
             .action = i18n::mark("Show two panes / active pane only"),
             .scope = scope::kViews,
             .modifiers = {},
             .keysyms = {XKB_KEY_F3},
             .command = Command::toggle_split},
    Shortcut{.group = i18n::mark("Panes"),
             .keys = "Tab",
             .action = i18n::mark("Switch pane"),
             .scope = scope::kViews,
             .modifiers = {},
             .keysyms = {XKB_KEY_Tab},
             .command = Command::switch_pane},
    Shortcut{.group = i18n::mark("Panes"),
             .keys = "",
             .action = i18n::mark("Swap panes"),
             .scope = scope::kViews,
             .modifiers = {},
             .keysyms = {XKB_KEY_NoSymbol},
             .command = Command::swap_panes},
    Shortcut{.group = i18n::mark("Panes"),
             .keys = "",
             .action = i18n::mark("Change pane orientation"),
             .scope = scope::kViews,
             .modifiers = {},
             .keysyms = {XKB_KEY_NoSymbol},
             .command = Command::orient_panes},
    Shortcut{.group = i18n::mark("Panes"),
             .keys = "",
             .action = i18n::mark("Open folder in other pane"),
             .scope = scope::kViews,
             .modifiers = {},
             .keysyms = {XKB_KEY_NoSymbol},
             .command = Command::open_other_pane},
    Shortcut{.group = i18n::mark("Panes"),
             .keys = "",
             .action = i18n::mark("Show same folder in other pane"),
             .scope = scope::kViews,
             .modifiers = {},
             .keysyms = {XKB_KEY_NoSymbol},
             .command = Command::same_other_pane},
    Shortcut{.group = i18n::mark("Panes"),
             .keys = "Ctrl+F5",
             .action = i18n::mark("Copy to other pane"),
             .scope = scope::kViews,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_F5},
             .command = Command::copy_other_pane},
    Shortcut{.group = i18n::mark("Panes"),
             .keys = "Ctrl+F6",
             .action = i18n::mark("Move to other pane"),
             .scope = scope::kViews,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_F6},
             .command = Command::move_other_pane},

    // Tabs
    Shortcut{.group = i18n::mark("Tabs"),
             .keys = "Ctrl+T",
             .action = i18n::mark("New tab"),
             .scope = scope::kViews | scope::kSidebar | scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_t, XKB_KEY_T},
             .command = Command::new_tab},
    Shortcut{.group = i18n::mark("Tabs"),
             .keys = "Ctrl+W",
             .action = i18n::mark("Close the tab"),
             .scope = scope::kViews | scope::kSidebar | scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_w, XKB_KEY_W},
             .command = Command::close_tab},
    Shortcut{.group = i18n::mark("Tabs"),
             .keys = "Ctrl+Tab  Ctrl+Shift+Tab",
             .action = i18n::mark("Next tab, previous"),
             .scope = scope::kViews | scope::kSidebar | scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_Tab},
             .command = Command::next_tab},
    Shortcut{.group = i18n::mark("Tabs"),
             .keys = "",
             .action = i18n::mark("Previous tab"),
             .scope = scope::kViews | scope::kSidebar | scope::kQuickLook,
             .modifiers = kCtrlShift,
             .keysyms = {XKB_KEY_ISO_Left_Tab, XKB_KEY_Tab},
             .command = Command::previous_tab},
    Shortcut{.group = i18n::mark("Tabs"),
             .keys = "Alt+1…9",
             .action = i18n::mark("Tab 1 to 9"),
             .scope = scope::kViews | scope::kSidebar | scope::kQuickLook,
             .modifiers = kAlt,
             .keysyms = {XKB_KEY_1, XKB_KEY_2, XKB_KEY_3, XKB_KEY_4, XKB_KEY_5, XKB_KEY_6,
                         XKB_KEY_7, XKB_KEY_8},
             .command = Command::select_tab},
    Shortcut{.group = i18n::mark("Tabs"),
             .keys = "",
             .action = i18n::mark("Tab 9"),
             .scope = scope::kViews | scope::kSidebar | scope::kQuickLook,
             .modifiers = kAlt,
             .keysyms = {XKB_KEY_9},
             .command = Command::select_tab},

    // Files
    Shortcut{.group = i18n::mark("Files"),
             .keys = "F2",
             .action = i18n::mark("Rename"),
             .scope = scope::kViews | scope::kQuickLook,
             .keysyms = {XKB_KEY_F2},
             .command = Command::rename},
    Shortcut{.group = i18n::mark("Files"),
             .keys = "Ctrl+Shift+N",
             .action = i18n::mark("New folder"),
             .scope = scope::kViews | scope::kQuickLook,
             .modifiers = {.ctrl = true, .shift = true},
             .keysyms = {XKB_KEY_n, XKB_KEY_N},
             .command = Command::new_folder},
    Shortcut{.group = i18n::mark("Files"),
             .keys = "Ctrl+C",
             .action = i18n::mark("Copy"),
             .scope = scope::kViews | scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_c, XKB_KEY_C},
             .command = Command::copy},
    Shortcut{.group = i18n::mark("Files"),
             .keys = "Ctrl+Shift+C",
             .action = i18n::mark("Copy the folder's path"),
             .scope = scope::kViews | scope::kQuickLook,
             .modifiers = kCtrlShift,
             .keysyms = {XKB_KEY_c, XKB_KEY_C},
             .command = Command::copy_path},
    Shortcut{.group = i18n::mark("Files"),
             .keys = "Ctrl+Alt+C",
             .action = i18n::mark("Copy Name"),
             .scope = scope::kViews,
             .modifiers = {.ctrl = true, .alt = true},
             .keysyms = {XKB_KEY_c, XKB_KEY_C},
             .action_id = "copy_name"},
    Shortcut{.group = i18n::mark("Files"),
             .keys = "Ctrl+X",
             .action = i18n::mark("Cut (Escape: keep them)"),
             .scope = scope::kViews | scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_x, XKB_KEY_X},
             .command = Command::cut},
    Shortcut{.group = i18n::mark("Files"),
             .keys = "Ctrl+V",
             .action = i18n::mark("Paste: copy or move here"),
             .scope = scope::kViews | scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_v, XKB_KEY_V},
             .command = Command::paste},
    Shortcut{.group = i18n::mark("Files"),
             .keys = "Delete",
             .action = i18n::mark("Move to the trash"),
             .scope = scope::kViews | scope::kQuickLook,
             .keysyms = {XKB_KEY_Delete, XKB_KEY_KP_Delete},
             .command = Command::trash},
    Shortcut{.group = i18n::mark("Files"),
             .keys = "Shift+Delete",
             .action = i18n::mark("Delete permanently"),
             .scope = scope::kViews | scope::kQuickLook,
             .modifiers = kShift,
             .keysyms = {XKB_KEY_Delete, XKB_KEY_KP_Delete},
             .command = Command::erase},
    Shortcut{.group = i18n::mark("Files"),
             .keys = "Ctrl+Z",
             .action = i18n::mark("Undo the last operation"),
             .scope = scope::kViews | scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_z, XKB_KEY_Z},
             .command = Command::undo},
    Shortcut{.group = i18n::mark("Files"),
             .keys = "Ctrl+Shift+Z",
             .action = i18n::mark("Redo it"),
             .scope = scope::kViews | scope::kQuickLook,
             .modifiers = {.ctrl = true, .shift = true},
             .keysyms = {XKB_KEY_z, XKB_KEY_Z},
             .command = Command::redo},
    Shortcut{.group = i18n::mark("Files"),
             .keys = "Shift+Escape",
             .action = i18n::mark("Stop the copy, move or delete"),
             .scope = scope::kViews | scope::kQuickLook | scope::kSidebar,
             .modifiers = kShift,
             .keysyms = {XKB_KEY_Escape},
             .command = Command::stop_transfer},
    Shortcut{.group = i18n::mark("Files"),
             .keys = "Enter  Escape",
             .action = i18n::mark("Renaming: keep, give up"),
             .scope = scope::kEditor,
             .keysyms = {XKB_KEY_Return, XKB_KEY_KP_Enter, XKB_KEY_Escape}},
    Shortcut{.group = i18n::mark("Files"),
             .keys = "Ctrl+←  Ctrl+→",
             .action = i18n::mark("Renaming: by words"),
             .scope = scope::kEditor,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_Left, XKB_KEY_Right}},
    Shortcut{.group = i18n::mark("Files"),
             .keys = "Ctrl+Z",
             .action = i18n::mark("Renaming: undo (Shift: redo)"),
             .scope = scope::kEditor,
             .modifiers = kCtrl,
             .any_shift = true,  // with Shift it redoes
             .keysyms = {XKB_KEY_z, XKB_KEY_Z}},
    Shortcut{.group = i18n::mark("Files"),
             .keys = "Ctrl+V",
             .action = i18n::mark("Renaming: paste text"),
             .scope = scope::kEditor,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_v, XKB_KEY_V}},
    Shortcut{.group = i18n::mark("Files"),
             .keys = "R  S  Enter",
             .action = i18n::mark("Name taken: replace or merge, skip, both"),
             .scope = scope::kConflict,
             .keysyms = {XKB_KEY_r, XKB_KEY_s, XKB_KEY_Return, XKB_KEY_KP_Enter}},
    Shortcut{.group = i18n::mark("Files"),
             .keys = "A  Escape",
             .action = i18n::mark("Name taken: for the rest, stop"),
             .scope = scope::kConflict,
             .keysyms = {XKB_KEY_a, XKB_KEY_Escape}},
    Shortcut{.group = i18n::mark("Files"),
             .keys = "Enter  Escape",
             .action = i18n::mark("Delete permanently? Yes, no"),
             .scope = scope::kConfirm,
             .keysyms = {XKB_KEY_Return, XKB_KEY_KP_Enter, XKB_KEY_Escape}},

    // Grid
    Shortcut{.group = i18n::mark("Zoom"),
             .keys = "Ctrl++  Ctrl+−",
             .action = i18n::mark("Larger, smaller (wheel too)"),
             .scope = scope::kViews | scope::kSidebar,
             .modifiers = kCtrl,
             .any_shift = true,
             .keysyms = {XKB_KEY_plus, XKB_KEY_equal, XKB_KEY_KP_Add, XKB_KEY_minus,
                         XKB_KEY_KP_Subtract}},
    Shortcut{.group = i18n::mark("Zoom"),
             .keys = "Ctrl+0",
             .action = i18n::mark("Usual size"),
             .scope = scope::kViews | scope::kSidebar,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_0, XKB_KEY_KP_0}},

    // Sidebar
    Shortcut{.group = i18n::mark("Sidebar"),
             .keys = "↑  ↓  Home  End",
             .action = i18n::mark("Choose a place"),
             .scope = scope::kSidebar,
             .keysyms = {XKB_KEY_Up, XKB_KEY_Down, XKB_KEY_Home, XKB_KEY_End}},
    Shortcut{.group = i18n::mark("Sidebar"),
             .keys = "Enter",
             .action = i18n::mark("Open it"),
             .scope = scope::kSidebar,
             .keysyms = {XKB_KEY_Return, XKB_KEY_KP_Enter}},
    Shortcut{.group = i18n::mark("Sidebar"),
             .keys = "Escape",
             .action = i18n::mark("Back to the view"),
             .scope = scope::kSidebar,
             .keysyms = {XKB_KEY_Escape}},
    Shortcut{.group = i18n::mark("Sidebar"),
             .keys = "F2",
             .action = i18n::mark("Rename a bookmark"),
             .scope = scope::kSidebar,
             .keysyms = {XKB_KEY_F2}},
    Shortcut{.group = i18n::mark("Sidebar"),
             .keys = "Alt+↑  Alt+↓",
             .action = i18n::mark("Move a bookmark up, down"),
             .scope = scope::kSidebar,
             .modifiers = kAlt,
             .keysyms = {XKB_KEY_Up, XKB_KEY_Down}},

    // Quick Look
    Shortcut{.group = i18n::mark("Quick Look"),
             .keys = "M",
             .action = i18n::mark("Markdown: rendered text or source"),
             .scope = scope::kQuickLook,
             .keysyms = {XKB_KEY_m, XKB_KEY_M}},
    Shortcut{.group = i18n::mark("Quick Look"),
             .keys = "Space  Escape",
             .action = i18n::mark("Close"),
             .scope = scope::kQuickLook,
             .keysyms = {XKB_KEY_space, XKB_KEY_Escape}},
    Shortcut{.group = i18n::mark("Quick Look"),
             .keys = "Arrows",
             .action = i18n::mark("The next file, the previous"),
             .scope = scope::kQuickLook,
             .keysyms = {XKB_KEY_Up, XKB_KEY_Down, XKB_KEY_Left, XKB_KEY_Right}},
    Shortcut{.group = i18n::mark("Quick Look"),
             .keys = "Home  End",
             .action = i18n::mark("A text file's top, its bottom"),
             .scope = scope::kQuickLook,
             .keysyms = {XKB_KEY_Home, XKB_KEY_End}},
    Shortcut{.group = i18n::mark("Quick Look"),
             .keys = "PgUp  PgDn",
             .action = i18n::mark("A page up, down (Shift+wheel: sideways)"),
             .scope = scope::kQuickLook,
             .keysyms = {XKB_KEY_Page_Up, XKB_KEY_Page_Down}},
    Shortcut{.group = i18n::mark("Quick Look"),
             .keys = "Ctrl+A",
             .action = i18n::mark("Select a text file's text (or drag on it)"),
             .scope = scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_a, XKB_KEY_A}},
    Shortcut{.group = i18n::mark("Quick Look"),
             .keys = "Ctrl+C",
             .action = i18n::mark("Copy the text selected"),
             .scope = scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_c, XKB_KEY_C}},
};

struct Bindings {
    std::vector<Shortcut> entries{kShortcuts.begin(), kShortcuts.end()};
    std::deque<std::string> labels;
};

Bindings active;

std::optional<std::size_t> binding_index(std::string_view name) {
    Command command = Command::none;
    if (name == "move_other_pane")
        command = Command::move_other_pane;
    if (name == "copy_other_pane")
        command = Command::copy_other_pane;
    if (name == "same_other_pane")
        command = Command::same_other_pane;
    if (name == "open_other_pane")
        command = Command::open_other_pane;
    if (name == "orient_panes")
        command = Command::orient_panes;
    if (name == "swap_panes")
        command = Command::swap_panes;
    if (name == "switch_pane")
        command = Command::switch_pane;
    if (name == "show_settings")
        command = Command::show_settings;
    if (name == "toggle_split")
        command = Command::toggle_split;
    if (name == "columns_view")
        command = Command::columns_view;
    if (name == "grid_view")
        command = Command::grid_view;
    if (name == "details_view")
        command = Command::details_view;
    if (name == "toggle_info")
        command = Command::toggle_info;
    if (name == "scroll_preview_up")
        command = Command::scroll_preview_up;
    if (name == "scroll_preview_down")
        command = Command::scroll_preview_down;
    if (name == "scroll_preview_page_up")
        command = Command::scroll_preview_page_up;
    if (name == "scroll_preview_page_down")
        command = Command::scroll_preview_page_down;
    if (name == "toggle_sidebar")
        command = Command::toggle_sidebar;
    if (name == "focus_sidebar")
        command = Command::focus_sidebar;
    if (name == "quick_look")
        command = Command::quick_look;
    if (name == "show_shortcuts")
        command = Command::show_shortcuts;
    if (name == "rename")
        command = Command::rename;
    if (name == "new_folder")
        command = Command::new_folder;
    if (name == "copy_path")
        command = Command::copy_path;
    if (name == "copy")
        command = Command::copy;
    if (name == "cut")
        command = Command::cut;
    if (name == "paste")
        command = Command::paste;
    if (name == "trash")
        command = Command::trash;
    if (name == "erase")
        command = Command::erase;
    if (name == "undo")
        command = Command::undo;
    if (name == "redo")
        command = Command::redo;
    if (name == "stop_transfer")
        command = Command::stop_transfer;
    if (name == "context_menu")
        command = Command::context_menu;
    if (name == "properties")
        command = Command::properties;
    if (name == "back")
        command = Command::back;
    if (name == "forward")
        command = Command::forward;
    if (name == "toggle_thumbnails")
        command = Command::toggle_thumbnails;
    if (name == "toggle_extensions")
        command = Command::toggle_extensions;
    if (name == "toggle_hidden")
        command = Command::toggle_hidden;
    if (name == "add_bookmark")
        command = Command::add_bookmark;
    if (name == "open_terminal")
        command = Command::open_terminal;
    if (name == "new_tab")
        command = Command::new_tab;
    if (name == "close_tab")
        command = Command::close_tab;
    if (name == "next_tab")
        command = Command::next_tab;
    if (name == "previous_tab")
        command = Command::previous_tab;
    if (name == "command_palette")
        command = Command::command_palette;
    if (name == "filter_names")
        command = Command::filter_names;
    if (name == "find_files")
        command = Command::find_files;
    if (name == "go_to")
        command = Command::go_to;
    for (std::size_t i = 0; i < kShortcuts.size(); ++i) {
        const auto& entry = kShortcuts[i];
        if (!entry.action_id.empty() &&
            (name == entry.action_id || name == "action:" + std::string{entry.action_id}))
            return i;
        if (command != Command::none && entry.command == command)
            return i;
        if (name == "select_all" && entry.action == "Select all")
            return i;
        if (name == "toggle_selection" && entry.action == "Select or unselect")
            return i;
    }
    return std::nullopt;
}

bool parse_chord(std::string_view text, Shortcut& entry) {
    entry.modifiers = {};
    entry.any_shift = false;
    entry.keysyms = {};
    if (text.empty())
        return true;
    while (text.find('+') != std::string_view::npos) {
        const auto split = text.find('+');
        const auto part = text.substr(0, split);
        bool* flag = nullptr;
        if (part == "Ctrl")
            flag = &entry.modifiers.ctrl;
        if (part == "Shift")
            flag = &entry.modifiers.shift;
        if (part == "Alt")
            flag = &entry.modifiers.alt;
        if (part == "Super")
            flag = &entry.modifiers.super;
        if (flag == nullptr || *flag)
            return false;
        *flag = true;
        text.remove_prefix(split + 1);
    }
    const auto symbol =
        xkb_keysym_from_name(std::string{text}.c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
    if (symbol == XKB_KEY_NoSymbol)
        return false;
    entry.keysyms[0] = xkb_keysym_to_lower(symbol);
    entry.keysyms[1] = xkb_keysym_to_upper(symbol);
    if (symbol == XKB_KEY_Tab && entry.modifiers.shift) {
        entry.keysyms[2] = XKB_KEY_ISO_Left_Tab;
    }
    return true;
}

}  // namespace

bool Shortcut::matches(const ui::KeyEvent& event) const {
    ui::Modifiers pressed = event.modifiers;
    if (any_shift) {
        pressed.shift = false;
    }
    return pressed == modifiers && event.keysym != XKB_KEY_NoSymbol &&
           std::ranges::find(keysyms, event.keysym) != keysyms.end();
}

std::vector<std::string> configure_shortcuts(const std::map<std::string, std::string>& bindings,
                                             std::span<const core::Action> custom, bool apply) {
    Bindings next;
    for (const auto& action : core::action_registry(custom)) {
        if (action.id == "copy_name")
            continue;
        const bool bundled = action.id == "copy_location" || action.id == "extract_here" ||
                             action.id == "extract_to";
        if (!bindings.contains("action:" + action.id) &&
            !(bundled && bindings.contains(action.id))) {
            continue;
        }
        next.labels.push_back(action.id);
        const std::string_view id = next.labels.back();
        next.labels.push_back(action.label);
        Shortcut entry;
        entry.group = i18n::mark("Files");
        entry.action = next.labels.back();
        entry.scope = scope::kViews;
        entry.action_id = id;
        next.entries.push_back(entry);
    }
    std::vector<std::string> errors;
    std::vector<std::size_t> changed;
    for (const auto& [name, chord] : bindings) {
        if (!name.starts_with("go:"))
            continue;
        const auto folder = core::expand_folder_path(std::string_view{name}.substr(3));
        if (folder.empty()) {
            errors.push_back(
                std::format("keybindings.{}: expected an absolute path or ~/ path", name));
            continue;
        }
        next.labels.push_back(folder.string());
        Shortcut entry;
        entry.folder_path = next.labels.back();
        next.labels.push_back(i18n::trf("Go to {}", name.substr(3)));
        entry.action = next.labels.back();
        next.labels.push_back(name);
        entry.action_id = next.labels.back();
        entry.group = i18n::mark("Moving");
        entry.scope = scope::kViews | scope::kSidebar | scope::kQuickLook;
        next.entries.push_back(entry);
    }
    // Split combined help rows when either half has been customized.
    if (bindings.contains("back") || bindings.contains("forward")) {
        auto& back = next.entries[*binding_index("back")];
        back.keys = "Alt+Left";
        back.action = i18n::mark("Back");
        next.entries[*binding_index("forward")].keys = "Alt+Right";
    }
    if (bindings.contains("next_tab") || bindings.contains("previous_tab")) {
        auto& tab = next.entries[*binding_index("next_tab")];
        tab.keys = "Ctrl+Tab";
        tab.action = i18n::mark("Next tab");
        next.entries[*binding_index("previous_tab")].keys = "Ctrl+Shift+Tab";
    }
    for (const auto& [name, chord] : bindings) {
        auto index = binding_index(name);
        const std::string_view id = std::string_view{name}.starts_with("action:")
                                        ? std::string_view{name}.substr(7)
                                        : std::string_view{name};
        if (!index) {
            for (std::size_t i = kShortcuts.size(); i < next.entries.size(); ++i) {
                if (next.entries[i].action_id == id) {
                    index = i;
                    break;
                }
            }
        }
        if (!index) {
            errors.push_back(std::format("keybindings.{}: unknown action", name));
            continue;
        }
        if (std::ranges::find(changed, *index) != changed.end()) {
            errors.push_back(std::format("keybindings.{}: action bound twice", name));
            continue;
        }
        auto& entry = next.entries[*index];
        if (!parse_chord(chord, entry)) {
            errors.push_back(std::format("keybindings.{}: invalid chord '{}'", name, chord));
            continue;
        }
        next.labels.push_back(chord);
        entry.keys = next.labels.back();
        changed.push_back(*index);
    }
    // Preserve explicit user bindings when introducing a new default shortcut.
    // An explicitly configured copy_path still participates in normal conflict checks.
    if (!bindings.contains("copy_path")) {
        auto& folder_path = next.entries[*binding_index("copy_path")];
        for (const auto index : changed) {
            const auto& entry = next.entries[index];
            for (const auto symbol : entry.keysyms) {
                if (folder_path.matches(
                        {.keysym = symbol, .text = {}, .modifiers = entry.modifiers})) {
                    folder_path.keysyms = {};
                    folder_path.keys = {};
                }
            }
        }
    }
    for (const auto index : changed) {
        const auto& entry = next.entries[index];
        for (std::size_t other = 0; other < next.entries.size(); ++other) {
            const auto& existing = next.entries[other];
            // Editors and dialogs own their input, independently of file commands.
            constexpr auto context =
                scope::kViews | scope::kSidebar | scope::kQuickLook | scope::kWindow;
            if (other == index || ((entry.scope & existing.scope & context) == 0 &&
                                   (existing.scope & scope::kWindow) == 0))
                continue;
            bool collision = false;
            for (const auto symbol : entry.keysyms) {
                ui::KeyEvent event;
                event.keysym = symbol;
                event.modifiers = entry.modifiers;
                collision = collision ||
                            (existing.matches(event) &&
                             (index >= kShortcuts.size() || !kShortcuts[index].matches(event)));
            }
            // Existing text-preview bindings intentionally share file copy/select-all chords.
            if (collision)
                errors.push_back(std::format("keybindings: '{}' conflicts with '{}'", entry.keys,
                                             existing.action));
        }
    }
    if (errors.empty() && apply)
        active = std::move(next);
    return errors;
}

std::vector<std::pair<std::string, std::string>> binding_settings() {
    std::vector<std::pair<std::string, std::string>> result;
    for (const auto* name : {"show_settings",
                             "move_other_pane",
                             "copy_other_pane",
                             "same_other_pane",
                             "open_other_pane",
                             "orient_panes",
                             "swap_panes",
                             "switch_pane",
                             "toggle_split",
                             "columns_view",
                             "grid_view",
                             "details_view",
                             "toggle_info",
                             "scroll_preview_up",
                             "scroll_preview_down",
                             "scroll_preview_page_up",
                             "scroll_preview_page_down",
                             "toggle_sidebar",
                             "focus_sidebar",
                             "quick_look",
                             "show_shortcuts",
                             "rename",
                             "new_folder",
                             "copy",
                             "copy_name",
                             "copy_path",
                             "cut",
                             "paste",
                             "trash",
                             "erase",
                             "undo",
                             "redo",
                             "stop_transfer",
                             "context_menu",
                             "properties",
                             "back",
                             "forward",
                             "toggle_hidden",
                             "toggle_thumbnails",
                             "toggle_extensions",
                             "add_bookmark",
                             "open_terminal",
                             "new_tab",
                             "close_tab",
                             "next_tab",
                             "previous_tab",
                             "command_palette",
                             "filter_names",
                             "find_files",
                             "go_to",
                             "select_all",
                             "toggle_selection"}) {
        if (const auto index = binding_index(name)) {
            const auto& entry = kShortcuts[*index];
            std::string chord;
            if (entry.keysyms[0] != XKB_KEY_NoSymbol) {
                if (entry.modifiers.ctrl)
                    chord += "Ctrl+";
                if (entry.modifiers.alt)
                    chord += "Alt+";
                if (entry.modifiers.shift)
                    chord += "Shift+";
                if (entry.modifiers.super)
                    chord += "Super+";
                std::array<char, 64> key{};
                xkb_keysym_get_name(entry.keysyms[0], key.data(), key.size());
                chord += key.data();
            }
            result.emplace_back(
                name, std::format("{} · {}", i18n::tr(entry.action),
                                  chord.empty() ? std::string{i18n::tr("Unbound by default")}
                                                : i18n::trf("Default: {}", i18n::keys(chord))));
        }
    }
    std::ranges::sort(result, [](const auto& a, const auto& b) {
        return binding_index(a.first) < binding_index(b.first);
    });
    return result;
}

ui::KeyEvent view_key_event(const ui::KeyEvent& event) {
    for (std::size_t i = 0; i < kShortcuts.size(); ++i) {
        const auto& entry = active.entries[i];
        if (entry.command == Command::none && (entry.scope & scope::kViews) != 0 &&
            entry.matches(event) &&
            (entry.action == "Select all" || entry.action == "Select or unselect")) {
            auto translated = event;
            translated.keysym = kShortcuts[i].keysyms[0];
            translated.modifiers = kShortcuts[i].modifiers;
            return translated;
        }
    }
    return event;
}

std::span<const Shortcut> shortcuts() { return active.entries; }

std::string keys_for(Command command) {
    const auto it = std::ranges::find(active.entries, command, &Shortcut::command);
    return it == active.entries.end() ? std::string{} : std::string{it->keys};
}

Command command_for(const ui::KeyEvent& event) {
    for (const Shortcut& shortcut : active.entries) {
        if (shortcut.command != Command::none && shortcut.matches(event)) {
            return shortcut.command;
        }
    }
    return Command::none;
}

std::string action_for(const ui::KeyEvent& event) {
    for (const auto& entry : active.entries) {
        if (!entry.action_id.empty() && entry.folder_path.empty() && entry.matches(event)) {
            return std::string{entry.action_id};
        }
    }
    return {};
}

std::string keys_for_action(std::string_view id) {
    const auto found = std::ranges::find(active.entries, id, &Shortcut::action_id);
    return found == active.entries.end() ? std::string{} : std::string{found->keys};
}

std::string folder_for(const ui::KeyEvent& event) {
    for (const auto& entry : active.entries) {
        if (!entry.folder_path.empty() && entry.matches(event))
            return std::string{entry.folder_path};
    }
    return {};
}

bool listed(const ui::KeyEvent& event, std::uint16_t where) {
    return std::ranges::any_of(active.entries, [&](const Shortcut& shortcut) {
        return (shortcut.scope & where) != 0U && shortcut.matches(event);
    });
}

}  // namespace rummage::views
