#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/entry.hpp"
#include "rummage/fs/filesystem.hpp"
#include "rummage/preview/preview.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/ui/icons.hpp"
#include "rummage/ui/scrollbar.hpp"
#include "rummage/ui/text_field.hpp"
#include "rummage/views/photo_properties.hpp"
#include "rummage/views/selectable_text.hpp"

namespace rummage::views {

class Thumbnails;

// Single-item properties and a staged folder-icon chooser. All disk work runs on workers.
class PropertiesDialog {
public:
    PropertiesDialog(core::WorkerPool& pool, std::shared_ptr<const fs::MimeDatabase> mime,
                     std::function<void()> redraw);
    ~PropertiesDialog();
    void open(std::filesystem::path path, std::function<void()> changed, bool editable = true,
              bool drive = false, std::string label = {});
    void open_photo(PhotoProperties properties, PhotoSave save, std::function<void()> changed = {});
    // The first three fields have explicit apply switches, including clearing mixed values.
    void set_photo_field(std::size_t index, std::string value, bool apply = true);

    void open_drive(const std::filesystem::path& path, std::string label) {
        open(path, {}, false, true, std::move(label));
    }

    [[nodiscard]] const fs::FilesystemFacts& filesystem() const { return data_.filesystem; }

    void close();

    void set_copy_handler(std::function<void(std::string)> handler) {
        copy_ = std::move(handler);
        text_.set_copy_handler(copy_);
    }

    // Drawn selectable field geometry: name, type, or a metadata row's label.
    [[nodiscard]] ui::Rect text_bounds(std::string_view id) const { return text_.bounds(id); }

    [[nodiscard]] ui::Rect control_bounds(std::string_view id) const {
        if (photo_ && id.size() == 7 && id.starts_with("photo:") && id.back() >= '0' &&
            id.back() <= '4')
            return photo_fields_[static_cast<std::size_t>(id.back() - '0')].bounds();
        for (const auto& control : controls_)
            if (control.id == id)
                return control.rect;
        return {};
    }

    [[nodiscard]] bool is_open() const { return open_; }

    [[nodiscard]] bool loading() const { return loading_ || (picking_ && icons_loading_); }

    [[nodiscard]] bool saving() const { return saving_; }

    [[nodiscard]] bool picking() const { return picking_; }

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

    [[nodiscard]] const std::string& icon() const { return draft_; }

    [[nodiscard]] const std::string& error() const { return error_; }

    [[nodiscard]] std::uint64_t generation() const { return generation_; }

    [[nodiscard]] const ui::Rect& card_bounds() const { return card_; }

    [[nodiscard]] float scroll_offset() const { return scroller_.offset(); }

    [[nodiscard]] ui::Rect scrollbar_bounds() const { return scrollbar_.thumb(scroller_); }

    [[nodiscard]] bool can_edit_icon() const;

    [[nodiscard]] bool can_edit_permissions() const;

    [[nodiscard]] std::uint16_t permissions() const { return permissions_; }

    void set_permissions(std::uint16_t permissions);

    [[nodiscard]] bool dirty() const {
        if (photo_)
            return photo_apply_[0] || photo_apply_[1] || photo_apply_[2] ||
                   !photo_fields_[3].text().empty() || !photo_fields_[4].text().empty();
        return draft_ != original_ || permissions_ != (data_.mode & 0777);
    }

    // Also used by keyboard interaction/tests; choosing only changes the draft.
    void choose_icon(std::string icon);
    void pick_icon();
    void apply();
    bool key(const ui::KeyEvent& event);
    void pointer(const ui::PointerEvent& event);
    void paste(std::string_view text);
    [[nodiscard]] ui::CursorShape cursor_at(float x, float y) const;
    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
              ui::IconSet& icons, Thumbnails* thumbnails = nullptr);

private:
    struct Data {
        fs::Entry entry;
        fs::FilesystemFacts filesystem;
        preview::Info info;
        preview::Summary summary;
        std::string icon;
        std::string icon_error;
        std::string default_icon;
        std::uint64_t device{0}, inode{0};
        std::uint16_t mode{0};
        bool owned{false};
    };

    struct Control {
        std::string id;
        ui::Rect rect;
        bool enabled{true};
    };

    [[nodiscard]] std::vector<std::pair<std::string, std::string>> property_rows() const;
    void activate(std::string_view id);
    void filter();
    void load_catalogue(std::string theme);
    void accept_custom();
    [[nodiscard]] std::vector<std::string> focus_order() const;
    void apply_photo();
    [[nodiscard]] int photo_field_index() const;
    core::WorkerPool& pool_;
    std::shared_ptr<const fs::MimeDatabase> mime_;
    std::function<void()> redraw_, changed_;
    std::function<void(std::string)> copy_;
    SelectableText text_;
    core::CancelToken load_job_, save_job_, icons_job_;
    bool open_{false}, loading_{false}, saving_{false}, picking_{false}, editable_{true};
    bool icons_loading_{false};
    bool drive_{false};
    std::string drive_label_;
    bool details_expanded_{false}, copy_hovered_{false}, copied_{false};
    std::filesystem::path path_;
    Data data_;
    std::uint16_t permissions_{0};
    std::string original_, draft_, error_, focus_{"close"};
    std::optional<std::string> catalogue_theme_;
    std::string resolved_theme_{"hicolor"};
    std::vector<std::string> catalogue_, filtered_;
    std::size_t selected_{0}, page_{0};
    ui::TextField search_, custom_;
    std::optional<PhotoProperties> photo_;
    PhotoSave photo_save_;
    std::array<ui::TextField, 5> photo_fields_;
    std::array<bool, 3> photo_apply_{};
    std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
    std::vector<Control> controls_;
    ui::Rect grid_{};
    ui::Rect content_{};
    ui::Rect card_{};
    ui::Scroller scroller_;
    ui::Scrollbar scrollbar_;
    std::uint64_t generation_{0};
    std::unique_ptr<ui::IconSet> preview_icons_;
    static constexpr std::size_t kPageSize = 15;
};

}  // namespace rummage::views
