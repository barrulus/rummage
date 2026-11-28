#include "rummage/platform/accessibility.hpp"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <sdbus-c++/sdbus-c++.h>
#include <set>

#include "bus_loop.hpp"
#include "rummage/core/log.hpp"

namespace rummage::platform {
namespace {
using Node = core::AccessibleNode;
using Edit = core::AccessibleEdit;
using Role = core::AccessibleRole;
using Ref = sdbus::Struct<std::string, sdbus::ObjectPath>;
using CacheItem =
    sdbus::Struct<Ref, Ref, Ref, std::int32_t, std::int32_t, std::vector<std::string>, std::string,
                  std::uint32_t, std::string, std::vector<std::uint32_t>>;
const std::string kRoot = "/org/a11y/atspi/accessible/root";
const std::string kNull = "/org/a11y/atspi/null";
const std::string kAccessible = "org.a11y.atspi.Accessible", kText = "org.a11y.atspi.Text",
                  kAction = "org.a11y.atspi.Action", kComponent = "org.a11y.atspi.Component",
                  kEditable = "org.a11y.atspi.EditableText",
                  kSelection = "org.a11y.atspi.Selection", kEvent = "org.a11y.atspi.Event.Object";

std::string role_name(Role role) {
    switch (role) {
        case Role::application:
            return "application";
        case Role::frame:
            return "frame";
        case Role::entry:
            return "entry";
        case Role::password:
            return "password text";
        case Role::list:
            return "list";
        case Role::list_item:
            return "list item";
        case Role::dialog:
            return "dialog";
        case Role::check_box:
            return "check box";
        case Role::radio_button:
            return "radio button";
        case Role::button:
            return "push button";
        case Role::menu:
            return "menu";
        case Role::menu_item:
            return "menu item";
        case Role::tab:
            return "page tab";
        case Role::tab_list:
            return "page tab list";
        case Role::status:
            return "status bar";
        case Role::alert:
            return "alert";
        case Role::panel:
            return "panel";
        case Role::text:
            return "text";
        case Role::label:
            return "label";
    }
    return "unknown";
}

int count(const Node& n) { return core::text_character_offset(n.text, n.text.size()); }

std::vector<std::uint32_t> states(const Node& n) {
    std::vector<std::uint32_t> bits(2);
    const auto bit = [&](unsigned i) { bits[i / 32] |= 1U << (i % 32); };
    bit(25);
    bit(30);  // showing, visible
    if (n.enabled) {
        bit(8);
        bit(24);
    }  // enabled, sensitive
    if (n.perform)
        bit(11);
    if (n.focused)
        bit(n.role == Role::frame ? 1U : 12U);
    if (n.selected)
        bit(23);
    if (n.checkable)
        bit(41);
    if (n.checked)
        bit(4);
    if (n.role == Role::list_item || n.role == Role::tab)
        bit(22);
    if (n.editable) {
        bit(7);
        bit(26);
        bit(38);
    }
    if (n.role == Role::dialog)
        bit(16);
    return bits;
}

std::vector<std::string> interfaces(const Node& n) {
    std::vector<std::string> list{kAccessible, kComponent, kText};
    if (n.perform)
        list.push_back(kAction);
    if (n.editable)
        list.push_back(kEditable);
    if (n.role == Role::frame || n.role == Role::list || n.role == Role::tab_list)
        list.push_back(kSelection);
    if (n.role == Role::application)
        list.push_back("org.a11y.atspi.Application");
    return list;
}

std::string substring(const Node& n, int first, int last) {
    first = std::clamp(first, 0, count(n));
    last = last < 0 ? count(n) : std::clamp(last, first, count(n));
    const auto begin = core::text_byte_offset(n.text, first),
               end = core::text_byte_offset(n.text, last);
    return n.text.substr(begin, end - begin);
}
}  // namespace

struct Accessibility::Impl {
    struct Held {
        Node node;
        std::string path, window;
        std::vector<std::string> children;
        std::size_t order{};
        std::shared_ptr<int> lifetime{std::make_shared<int>(0)};
        std::unique_ptr<sdbus::IObject> object;
    };

    core::EventLoop& loop;
    std::unique_ptr<sdbus::IConnection> session, connection;
    std::unique_ptr<sdbus::IProxy> discovery, registry;
    std::unique_ptr<BusLoop> session_loop, bus;
    std::map<std::string, std::unique_ptr<Held>> nodes;
    std::unique_ptr<sdbus::IObject> cache;
    std::map<std::string, std::vector<Node>> pending;
    std::map<std::string, std::string> titles;
    std::map<std::string, bool> active_windows;
    std::string unique;
    Ref root_parent{"", sdbus::ObjectPath{kNull}};
    std::uint64_t next_id{};
    int application_id{};
    bool ready{}, discovering{};
    core::EventLoop::SourceId retry{};
    std::function<void()> ready_handler;
    std::shared_ptr<int> lifetime{std::make_shared<int>(0)};

    explicit Impl(core::EventLoop& value) : loop{value} {
        const char* disabled = std::getenv("NO_AT_BRIDGE");  // NOLINT(concurrency-mt-unsafe)
        if (disabled && std::string_view{disabled} == "1")
            return;
        retry = loop.add_timer(std::chrono::seconds{5}, std::chrono::seconds{5}, [this] {
            if (!ready && !discovering && !connection)
                discover();
        });
        discover();
    }

    ~Impl() { loop.remove(retry); }

    void discover() {
        discovery.reset();
        session_loop.reset();
        session.reset();
        try {
            const char* override_address =
                std::getenv("AT_SPI_BUS_ADDRESS");  // NOLINT(concurrency-mt-unsafe)
            if (override_address && *override_address) {
                connect(override_address);
                return;
            }
            session = sdbus::createSessionBusConnection();
            discovery = sdbus::createProxy(*session, sdbus::ServiceName{"org.a11y.Bus"},
                                           sdbus::ObjectPath{"/org/a11y/bus"});
            session_loop = std::make_unique<BusLoop>(loop, *session, "accessibility discovery");
            discovering = true;
            discovery->callMethodAsync("GetAddress")
                .onInterface("org.a11y.Bus")
                .uponReplyInvoke(
                    [this](std::optional<sdbus::Error> error, const std::string& address) {
                        discovering = false;
                        if (error)
                            return;
                        try {
                            connect(address);
                        } catch (const sdbus::Error& failure) {
                            log::warn("accessibility: {}", failure.getMessage());
                        }
                    });
            session_loop->pump();
        } catch (const sdbus::Error& error) {
            discovering = false;
            log::debug("accessibility unavailable: {}", error.getMessage());
        }
    }

    Ref ref(const std::string& id) const {
        const auto it = nodes.find(id);
        return it == nodes.end() ? Ref{"", sdbus::ObjectPath{kNull}}
                                 : Ref{unique, sdbus::ObjectPath{it->second->path}};
    }

    int index(const Held& h) const {
        const auto it = nodes.find(h.node.parent);
        if (it == nodes.end())
            return -1;
        const auto& children = it->second->children;
        const auto at = std::ranges::find(children, h.node.id);
        return at == children.end() ? -1 : static_cast<int>(at - children.begin());
    }

    CacheItem item(const Held& h) const {
        return {ref(h.node.id),
                ref("root"),
                h.node.id == "root" ? root_parent : ref(h.node.parent),
                index(h),
                static_cast<int>(h.children.size()),
                interfaces(h.node),
                h.node.name,
                static_cast<std::uint32_t>(h.node.role),
                h.node.description,
                states(h.node)};
    }

    bool perform(Held* h, Edit edit) {
        if (!h->node.enabled || !h->node.perform || edit.text.size() > 65536 ||
            edit.text.find('\0') != std::string::npos ||
            (edit.kind == Edit::Kind::focus && !h->node.editable &&
             h->node.role != Role::list_item))
            return false;
        const auto id = h->node.id;
        loop.post([this, bridge = std::weak_ptr{lifetime}, target = std::weak_ptr{h->lifetime}, id,
                   edit = std::move(edit)] {
            const auto bridge_alive = bridge.lock(), target_alive = target.lock();
            if (!bridge_alive || !target_alive)
                return;
            const auto found = nodes.find(id);
            if (!ready || found == nodes.end() || found->second->lifetime != target_alive ||
                !found->second->node.enabled || !found->second->node.perform)
                return;
            // Resolve the current geometry callback only after checking identity. Never
            // replay a queued click into a replacement modal or a newly scrolled item.
            auto callback = found->second->node.perform;
            callback(edit);
        });
        return true;
    }

    void signal(Held& h, const std::string& name, std::string detail, int one = 0, int two = 0,
                sdbus::Variant data = sdbus::Variant{std::string{}}) {
        h.object->emitSignal(name).onInterface(kEvent).withArguments(
            detail, one, two, data, std::map<std::string, sdbus::Variant>{});
    }

    void add(Node node, const std::string& window) {
        auto holder = std::make_unique<Held>();
        holder->node = std::move(node);
        holder->window = window;
        holder->path = holder->node.id == "root"
                           ? kRoot
                           : "/org/a11y/atspi/accessible/n" + std::to_string(++next_id);
        auto* h = holder.get();
        const auto id = h->node.id;
        nodes[id] = std::move(holder);
        h->object = sdbus::createObject(*connection, sdbus::ObjectPath{h->path});
        auto& object = *h->object;
        object
            .addVTable(
                sdbus::registerProperty("version").withGetter([] { return 1U; }),
                sdbus::registerProperty("Name").withGetter([h] { return h->node.name; }),
                sdbus::registerProperty("Description").withGetter([h] {
                    return h->node.description;
                }),
                sdbus::registerProperty("Parent").withGetter(
                    [this, h] { return h->node.id == "root" ? root_parent : ref(h->node.parent); }),
                sdbus::registerProperty("ChildCount").withGetter([h] {
                    return static_cast<int>(h->children.size());
                }),
                sdbus::registerProperty("Locale").withGetter([] { return std::string{}; }),
                sdbus::registerProperty("AccessibleId").withGetter([h] { return h->node.id; }),
                sdbus::registerProperty("HelpText").withGetter([h] { return h->node.description; }),
                sdbus::registerMethod("GetChildAtIndex").implementedAs([this, h](int i) {
                    return i < 0 || static_cast<std::size_t>(i) >= h->children.size()
                               ? ref("")
                               : ref(h->children[static_cast<std::size_t>(i)]);
                }),
                sdbus::registerMethod("GetChildren").implementedAs([this, h] {
                    std::vector<Ref> out;
                    for (const auto& child : h->children)
                        out.push_back(ref(child));
                    return out;
                }),
                sdbus::registerMethod("GetIndexInParent").implementedAs([this, h] {
                    return index(*h);
                }),
                sdbus::registerMethod("GetRelationSet").implementedAs([] {
                    return std::vector<sdbus::Struct<std::uint32_t, std::vector<Ref>>>{};
                }),
                sdbus::registerMethod("GetRole").implementedAs(
                    [h] { return static_cast<std::uint32_t>(h->node.role); }),
                sdbus::registerMethod("GetRoleName").implementedAs([h] {
                    return role_name(h->node.role);
                }),
                sdbus::registerMethod("GetLocalizedRoleName").implementedAs([h] {
                    return role_name(h->node.role);
                }),
                sdbus::registerMethod("GetState").implementedAs([h] { return states(h->node); }),
                sdbus::registerMethod("GetAttributes").implementedAs([] {
                    return std::map<std::string, std::string>{{"toolkit", "rummage"}};
                }),
                sdbus::registerMethod("GetApplication").implementedAs([this] {
                    return ref("root");
                }),
                sdbus::registerMethod("GetInterfaces").implementedAs([h] {
                    return interfaces(h->node);
                }))
            .forInterface(kAccessible);
        const auto extents = [h](std::uint32_t coordinate) {
            const auto& r = h->node.bounds;
            // Wayland clients have no global window position. Never invent screen coordinates.
            return coordinate == 0 ? sdbus::Struct<int, int, int, int>{-1, -1, r.width, r.height}
                                   : sdbus::Struct<int, int, int, int>{r.x, r.y, r.width, r.height};
        };
        object
            .addVTable(
                sdbus::registerMethod("GetExtents").implementedAs(extents),
                sdbus::registerMethod("GetPosition").implementedAs([h](std::uint32_t coordinate) {
                    return std::tuple{coordinate == 0 ? -1 : h->node.bounds.x,
                                      coordinate == 0 ? -1 : h->node.bounds.y};
                }),
                sdbus::registerMethod("GetSize").implementedAs(
                    [h] { return std::tuple{h->node.bounds.width, h->node.bounds.height}; }),
                sdbus::registerMethod("Contains")
                    .implementedAs([h](int x, int y, std::uint32_t coordinate) {
                        const auto& r = h->node.bounds;
                        return coordinate != 0 && x >= r.x && y >= r.y && x < r.x + r.width &&
                               y < r.y + r.height;
                    }),
                sdbus::registerMethod("GetAccessibleAtPoint")
                    .implementedAs([this, h](int x, int y, std::uint32_t coordinate) {
                        for (const auto& child : h->children) {
                            const auto& r = nodes.at(child)->node.bounds;
                            if (coordinate != 0 && x >= r.x && y >= r.y && x < r.x + r.width &&
                                y < r.y + r.height)
                                return ref(child);
                        }
                        return ref("");
                    }),
                sdbus::registerMethod("GetLayer").implementedAs([] { return 3U; }),
                sdbus::registerMethod("GetMDIZOrder").implementedAs([] { return std::int16_t{0}; }),
                sdbus::registerMethod("GetAlpha").implementedAs([] { return 1.0; }),
                sdbus::registerMethod("GrabFocus").implementedAs([this, h] {
                    return perform(h, {.kind = Edit::Kind::focus, .text = {}});
                }))
            .forInterface(kComponent);
        object
            .addVTable(sdbus::registerProperty("NActions").withGetter([h] {
                return h->node.perform ? 1 : 0;
            }),
                       sdbus::registerMethod("GetName").implementedAs(
                           [](int i) { return i == 0 ? std::string{"activate"} : std::string{}; }),
                       sdbus::registerMethod("GetLocalizedName").implementedAs([](int i) {
                           return i == 0 ? std::string{"activate"} : std::string{};
                       }),
                       sdbus::registerMethod("GetDescription").implementedAs([h](int) {
                           return h->node.description;
                       }),
                       sdbus::registerMethod("GetKeyBinding").implementedAs([](int) {
                           return std::string{};
                       }),
                       sdbus::registerMethod("GetActions").implementedAs([h] {
                           std::vector<sdbus::Struct<std::string, std::string, std::string>> out;
                           if (h->node.perform)
                               out.emplace_back("activate", h->node.description, "");
                           return out;
                       }),
                       sdbus::registerMethod("DoAction").implementedAs([this, h](int i) {
                           return i == 0 && perform(h, {.kind = Edit::Kind::activate, .text = {}});
                       }))
            .forInterface(kAction);
        const auto select = [this, h](int start, int end) {
            return start >= 0 && end >= 0 && start <= count(h->node) && end <= count(h->node) &&
                   perform(h, {.kind = Edit::Kind::set_selection,
                               .text = {},
                               .first = start,
                               .last = end});
        };
        const auto range = [h](int offset, std::uint32_t granularity) {
            const int size = count(h->node);
            offset = std::clamp(offset, 0, size);
            int first = offset, last = std::min(offset + 1, size);
            if (granularity != 0) {
                const auto separator = [&](int at) {
                    const auto value = substring(h->node, at, at + 1);
                    return granularity == 1 ? value == " " || value == "\t" || value == "\n"
                           : granularity == 2
                               ? value == "." || value == "!" || value == "?" || value == "\n"
                               : value == "\n";
                };
                while (first > 0 && !separator(first - 1))
                    --first;
                while (last < size && !separator(last - 1))
                    ++last;
            }
            return std::tuple{substring(h->node, first, last), first, last};
        };
        object
            .addVTable(
                sdbus::registerProperty("CharacterCount").withGetter([h] {
                    return count(h->node);
                }),
                sdbus::registerProperty("CaretOffset").withGetter([h] { return h->node.caret; }),
                sdbus::registerMethod("GetText").implementedAs(
                    [h](int first, int last) { return substring(h->node, first, last); }),
                sdbus::registerMethod("GetCharacterAtOffset").implementedAs([h](int at) {
                    const auto value = substring(h->node, at, at + 1);
                    if (at < 0 || at >= count(h->node) || value.empty())
                        return 0;
                    const auto lead = static_cast<unsigned char>(value[0]);
                    std::uint32_t scalar = lead < 0x80U   ? lead
                                           : lead < 0xe0U ? lead & 0x1fU
                                           : lead < 0xf0U ? lead & 0x0fU
                                                          : lead & 0x07U;
                    for (std::size_t i = 1; i < value.size(); ++i)
                        scalar = (scalar << 6U) | (static_cast<unsigned char>(value[i]) & 0x3fU);
                    return static_cast<int>(scalar);
                }),
                sdbus::registerMethod("GetStringAtOffset").implementedAs(range),
                sdbus::registerMethod("GetTextAtOffset").implementedAs(range),
                sdbus::registerMethod("GetTextBeforeOffset")
                    .implementedAs([h](int offset, std::uint32_t) {
                        return std::tuple{substring(h->node, 0, offset), 0,
                                          std::clamp(offset, 0, count(h->node))};
                    }),
                sdbus::registerMethod("GetTextAfterOffset")
                    .implementedAs([h](int offset, std::uint32_t) {
                        return std::tuple{substring(h->node, offset, -1),
                                          std::clamp(offset, 0, count(h->node)), count(h->node)};
                    }),
                sdbus::registerMethod("SetCaretOffset").implementedAs([select](int at) {
                    return select(at, at);
                }),
                sdbus::registerMethod("GetNSelections").implementedAs([h] {
                    return h->node.anchor == h->node.caret ? 0 : 1;
                }),
                sdbus::registerMethod("GetSelection").implementedAs([h](int) {
                    return std::tuple{std::min(h->node.anchor, h->node.caret),
                                      std::max(h->node.anchor, h->node.caret)};
                }),
                sdbus::registerMethod("AddSelection").implementedAs(select),
                sdbus::registerMethod("SetSelection")
                    .implementedAs([select](int selection, int first, int last) {
                        return selection == 0 && select(first, last);
                    }),
                sdbus::registerMethod("RemoveSelection").implementedAs([select, h](int selection) {
                    return selection == 0 && select(h->node.caret, h->node.caret);
                }),
                sdbus::registerMethod("GetDefaultAttributes").implementedAs([] {
                    return std::map<std::string, std::string>{};
                }),
                sdbus::registerMethod("GetDefaultAttributeSet").implementedAs([] {
                    return std::map<std::string, std::string>{};
                }),
                sdbus::registerMethod("GetAttributeRun").implementedAs([h](int, bool) {
                    return std::tuple{std::map<std::string, std::string>{}, 0, count(h->node)};
                }),
                sdbus::registerMethod("GetAttributes").implementedAs([h](int) {
                    return std::tuple{std::map<std::string, std::string>{}, 0, count(h->node)};
                }))
            .forInterface(kText);
        object
            .addVTable(
                sdbus::registerMethod("SetTextContents")
                    .implementedAs([this, h](const std::string& text) {
                        return h->node.editable && perform(h, {.kind = Edit::Kind::set_text,
                                                               .text = text,
                                                               .first = 0,
                                                               .last = count(h->node)});
                    }),
                sdbus::registerMethod("InsertText")
                    .implementedAs([this, h](int position, const std::string& text, int length) {
                        return h->node.editable && position >= 0 && position <= count(h->node) &&
                               length >= 0 &&
                               perform(
                                   h, {.kind = Edit::Kind::set_text,
                                       .text = text.substr(0, core::text_byte_offset(text, length)),
                                       .first = position,
                                       .last = position});
                    }),
                sdbus::registerMethod("DeleteText").implementedAs([this, h](int first, int last) {
                    return h->node.editable && first >= 0 && last >= first &&
                           last <= count(h->node) &&
                           perform(h, {.kind = Edit::Kind::set_text,
                                       .text = {},
                                       .first = first,
                                       .last = last});
                }),
                sdbus::registerMethod("CopyText").implementedAs([this, h](int first, int last) {
                    if (!h->node.password)
                        perform(
                            h,
                            {.kind = Edit::Kind::copy, .text = {}, .first = first, .last = last});
                }),
                sdbus::registerMethod("CutText").implementedAs([this, h](int first, int last) {
                    return h->node.editable && !h->node.password &&
                           perform(
                               h,
                               {.kind = Edit::Kind::cut, .text = {}, .first = first, .last = last});
                }),
                sdbus::registerMethod("PasteText").implementedAs([this, h](int position) {
                    return h->node.editable && perform(h, {.kind = Edit::Kind::paste,
                                                           .text = {},
                                                           .first = position,
                                                           .last = position});
                }))
            .forInterface(kEditable);
        const auto selected = [this, h] {
            std::vector<std::string> out;
            for (const auto& child : h->children)
                if (nodes.at(child)->node.selected)
                    out.push_back(child);
            return out;
        };
        object
            .addVTable(
                sdbus::registerProperty("NSelectedChildren").withGetter([selected] {
                    return static_cast<int>(selected().size());
                }),
                sdbus::registerMethod("GetSelectedChild").implementedAs([this, selected](int i) {
                    auto list = selected();
                    return i >= 0 && static_cast<std::size_t>(i) < list.size()
                               ? ref(list[static_cast<std::size_t>(i)])
                               : ref("");
                }),
                sdbus::registerMethod("SelectChild").implementedAs([this, h](int i) {
                    return i >= 0 && static_cast<std::size_t>(i) < h->children.size() &&
                           perform(nodes.at(h->children[static_cast<std::size_t>(i)]).get(),
                                   {.kind = Edit::Kind::select, .text = {}, .first = 1});
                }),
                sdbus::registerMethod("DeselectChild").implementedAs([this, h](int i) {
                    return i >= 0 && static_cast<std::size_t>(i) < h->children.size() &&
                           perform(nodes.at(h->children[static_cast<std::size_t>(i)]).get(),
                                   {.kind = Edit::Kind::select, .text = {}, .first = 0});
                }),
                sdbus::registerMethod("DeselectSelectedChild")
                    .implementedAs([this, selected](int i) {
                        const auto list = selected();
                        return i >= 0 && static_cast<std::size_t>(i) < list.size() &&
                               perform(nodes.at(list[static_cast<std::size_t>(i)]).get(),
                                       {.kind = Edit::Kind::select, .text = {}, .first = 0});
                    }),
                sdbus::registerMethod("ClearSelection").implementedAs([this, selected] {
                    bool good = true;
                    for (const auto& child_id : selected())
                        good &= perform(nodes.at(child_id).get(),
                                        {.kind = Edit::Kind::select, .text = {}, .first = 0});
                    return good;
                }),
                sdbus::registerMethod("SelectAll").implementedAs([this, h] {
                    bool good = true;
                    for (const auto& child_id : h->children) {
                        auto* child = nodes.at(child_id).get();
                        if (child->node.role == Role::list_item)
                            good &= perform(child,
                                            {.kind = Edit::Kind::select, .text = {}, .first = 1});
                    }
                    return good;
                }),
                sdbus::registerMethod("IsChildSelected").implementedAs([this, h](int i) {
                    return i >= 0 && static_cast<std::size_t>(i) < h->children.size() &&
                           nodes.at(h->children[static_cast<std::size_t>(i)])->node.selected;
                }))
            .forInterface(kSelection);
        for (const char* name : {"StateChanged", "ChildrenChanged", "PropertyChange",
                                 "SelectionChanged", "TextChanged", "TextSelectionChanged",
                                 "TextCaretMoved", "Announcement", "VisibleDataChanged"})
            object
                .addVTable(sdbus::registerSignal(name)
                               .withParameters<std::string, int, int, sdbus::Variant,
                                               std::map<std::string, sdbus::Variant>>())
                .forInterface(kEvent);
    }

    void connect(const std::string& address) {
        connection = sdbus::createSessionBusConnectionWithAddress(address);
        unique = connection->getUniqueName();
        bus = std::make_unique<BusLoop>(loop, *connection, "accessibility");
        bus->set_lost_handler([this] { ready = false; });
        Node root;
        root.id = "root";
        root.name = "Rummage";
        root.role = Role::application;
        add(std::move(root), "");
        nodes.at("root")
            ->object
            ->addVTable(
                sdbus::registerProperty("ToolkitName").withGetter([] {
                    return std::string{"rummage"};
                }),
                sdbus::registerProperty("Version").withGetter([] { return std::string{"0.6"}; }),
                sdbus::registerProperty("ToolkitVersion").withGetter([] {
                    return std::string{"0.6"};
                }),
                sdbus::registerProperty("AtspiVersion").withGetter([] {
                    return std::string{"2.1"};
                }),
                sdbus::registerProperty("Id")
                    .withGetter([this] { return application_id; })
                    .withSetter([this](int id) { application_id = id; }),
                sdbus::registerMethod("GetLocale").implementedAs([](std::uint32_t) {
                    return std::string{};
                }),
                sdbus::registerMethod("GetApplicationBusAddress").implementedAs([] {
                    return std::string{};
                }))
            .forInterface("org.a11y.atspi.Application");
        cache = sdbus::createObject(*connection, sdbus::ObjectPath{"/org/a11y/atspi/cache"});
        cache
            ->addVTable(sdbus::registerMethod("GetItems").implementedAs([this] {
                std::vector<CacheItem> out;
                for (const auto& [id, h] : nodes) {
                    (void)id;
                    out.push_back(item(*h));
                }
                return out;
            }),
                        sdbus::registerSignal("AddAccessible").withParameters<CacheItem>(),
                        sdbus::registerSignal("RemoveAccessible").withParameters<Ref>())
            .forInterface("org.a11y.atspi.Cache");
        registry = sdbus::createProxy(*connection, sdbus::ServiceName{"org.a11y.atspi.Registry"},
                                      sdbus::ObjectPath{kRoot});
        registry->callMethodAsync("Embed")
            .onInterface("org.a11y.atspi.Socket")
            .withArguments(ref("root"))
            .uponReplyInvoke([this](std::optional<sdbus::Error> error, const Ref& parent) {
                if (error) {
                    log::warn("accessibility registration: {}", error->getMessage());
                    return;
                }
                root_parent = parent;
                ready = true;
                for (auto& [id, values] : pending)
                    update(id, titles[id], values, active_windows[id]);
                pending.clear();
                if (ready_handler)
                    ready_handler();
            });
        bus->pump();
    }

    void rebuild() {
        for (auto& [id, h] : nodes) {
            (void)id;
            h->children.clear();
        }
        for (const auto& [id, h] : nodes)
            if (auto parent = nodes.find(h->node.parent); parent != nodes.end())
                parent->second->children.push_back(id);
        for (auto& [id, h] : nodes) {
            (void)id;
            std::ranges::stable_sort(h->children, [&](const auto& a, const auto& b) {
                return nodes.at(a)->order < nodes.at(b)->order;
            });
        }
    }

    void update(const std::string& window, const std::string& title,
                const std::vector<Node>& values, bool focused) {
        active_windows[window] = focused;
        if (!ready) {
            pending[window] = values;
            titles[window] = title;
            return;
        }
        const std::string frame = "window:" + window;
        std::vector<Node> next;
        Node root;
        root.id = frame;
        root.parent = "root";
        root.role = Role::frame;
        root.name = title;
        root.focused = focused;
        next.push_back(std::move(root));
        for (auto node : values) {
            node.id = frame + ":" + node.id;
            node.parent = node.parent.empty() ? frame : frame + ":" + node.parent;
            next.push_back(std::move(node));
        }
        std::set<std::string> keep;
        std::vector<std::string> added;
        std::size_t order = 0;
        for (auto& node : next) {
            if (!keep.insert(node.id).second)
                continue;
            auto found = nodes.find(node.id);
            if (found == nodes.end()) {
                added.push_back(node.id);
                const auto id = node.id;
                add(std::move(node), window);
                nodes.at(id)->order = order++;
                continue;
            }
            auto& h = *found->second;
            h.order = order++;
            const auto old = h.node;
            h.node = std::move(node);
            if (old.name != h.node.name || old.role != h.node.role || old.parent != h.node.parent)
                h.lifetime = std::make_shared<int>(0);
            if (old.description != h.node.description)
                signal(h, "PropertyChange", "accessible-description", 0, 0,
                       sdbus::Variant{h.node.description});
            if (old.focused != h.node.focused)
                signal(h, "StateChanged", h.node.role == Role::frame ? "active" : "focused",
                       h.node.focused ? 1 : 0);
            if (old.selected != h.node.selected) {
                signal(h, "StateChanged", "selected", h.node.selected ? 1 : 0);
                if (auto parent = nodes.find(h.node.parent); parent != nodes.end())
                    signal(*parent->second, "SelectionChanged", "");
            }
            if (old.checked != h.node.checked)
                signal(h, "StateChanged", "checked", h.node.checked ? 1 : 0);
            if (old.enabled != h.node.enabled)
                signal(h, "StateChanged", "enabled", h.node.enabled ? 1 : 0);
            if (old.name != h.node.name)
                signal(h, "PropertyChange", "accessible-name", 0, 0, sdbus::Variant{h.node.name});
            if (old.text != h.node.text) {
                if (!old.text.empty())
                    signal(h, "TextChanged", "delete", 0, count(old), sdbus::Variant{old.text});
                if (!h.node.text.empty())
                    signal(h, "TextChanged", "insert", 0, count(h.node),
                           sdbus::Variant{h.node.text});
            }
            if (old.caret != h.node.caret)
                signal(h, "TextCaretMoved", "", h.node.caret);
            if (old.anchor != h.node.anchor)
                signal(h, "TextSelectionChanged", "");
            if ((h.node.role == Role::status || h.node.role == Role::alert) &&
                old.name != h.node.name)
                signal(h, "Announcement", "", h.node.role == Role::alert ? 2 : 1, 0,
                       sdbus::Variant{h.node.name});
        }
        for (auto it = nodes.begin(); it != nodes.end();) {
            if (it->second->window == window && !keep.contains(it->first)) {
                const auto reference = ref(it->first);
                if (auto parent = nodes.find(it->second->node.parent); parent != nodes.end())
                    signal(*parent->second, "ChildrenChanged", "remove", index(*it->second), 0,
                           sdbus::Variant{reference});
                signal(*it->second, "StateChanged", "defunct", 1);
                cache->emitSignal("RemoveAccessible")
                    .onInterface("org.a11y.atspi.Cache")
                    .withArguments(reference);
                it = nodes.erase(it);
            } else
                ++it;
        }
        rebuild();
        for (const auto& id : added) {
            auto& h = *nodes.at(id);
            cache->emitSignal("AddAccessible")
                .onInterface("org.a11y.atspi.Cache")
                .withArguments(item(h));
            if (auto parent = nodes.find(h.node.parent); parent != nodes.end())
                signal(*parent->second, "ChildrenChanged", "add", index(h), 0,
                       sdbus::Variant{ref(id)});
            if (h.node.role == Role::alert)
                signal(h, "Announcement", "", 2, 0, sdbus::Variant{h.node.name});
            if (h.node.focused)
                signal(h, "StateChanged", h.node.role == Role::frame ? "active" : "focused", 1);
        }
        bus->pump();
    }

    void remove(const std::string& window) {
        pending.erase(window);
        titles.erase(window);
        active_windows.erase(window);
        if (!ready)
            return;
        for (auto it = nodes.begin(); it != nodes.end();) {
            if (it->second->window == window) {
                if (auto parent = nodes.find(it->second->node.parent); parent != nodes.end())
                    signal(*parent->second, "ChildrenChanged", "remove", index(*it->second), 0,
                           sdbus::Variant{ref(it->first)});
                signal(*it->second, "StateChanged", "defunct", 1);
                cache->emitSignal("RemoveAccessible")
                    .onInterface("org.a11y.atspi.Cache")
                    .withArguments(ref(it->first));
                it = nodes.erase(it);
            } else
                ++it;
        }
        rebuild();
        bus->pump();
    }
};

Accessibility::Accessibility(core::EventLoop& loop) : impl_{std::make_unique<Impl>(loop)} {}

Accessibility::~Accessibility() = default;

void Accessibility::set_ready_handler(std::function<void()> handler) {
    impl_->ready_handler = std::move(handler);
    // The constructor's bus pump can complete registration before a caller installs this.
    if (impl_->ready && impl_->ready_handler)
        impl_->ready_handler();
}

bool Accessibility::active() const { return impl_->ready; }

void Accessibility::update_window(std::string id, std::string title, std::vector<Node> nodes,
                                  bool focused) {
    try {
        impl_->update(id, title, nodes, focused);
    } catch (const sdbus::Error& error) {
        impl_->ready = false;
        log::warn("accessibility: {}", error.getMessage());
    }
}

void Accessibility::remove_window(const std::string& id) {
    try {
        impl_->remove(id);
    } catch (const sdbus::Error& error) {
        impl_->ready = false;
        log::warn("accessibility: {}", error.getMessage());
    }
}
}  // namespace rummage::platform
