#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/ui/geometry.hpp"
#include "rummage/ui/icons.hpp"
#include "rummage/ui/renderer.hpp"
#include "rummage/ui/seat.hpp"
#include "rummage/views/folder.hpp"

namespace rummage::views {

// The space on the file system that holds a folder.
struct FreeSpace {
    std::uint64_t free{0};  // what an unprivileged user may still write
    std::uint64_t total{0};

    bool operator==(const FreeSpace&) const = default;
};

// statvfs(path); nullopt when it fails.
[[nodiscard]] std::optional<FreeSpace> free_space_of(const std::filesystem::path& path);

// A copy or a move in progress, as the bar shows it.
struct TransferStatus {
    std::string text;       // "Copying 163 files · 2.1 of 3.4 GB · 180 MB/s · 8 s left"
    float fraction{-1.0F};  // done, 0 to 1; negative: not known yet
};

// "12 items", "12 items, 3 hidden", "3 of 12 selected", "12 items, loading…", or the folder's
// error. A single selection shows its full filename, including its extension.
[[nodiscard]] std::string status_of(const Folder& folder);

// The bar under the view: at the left the folder's status and the free space of its file system
// (read on a worker when the folder or its listing changes, so it follows copies into it), or for
// a few seconds a message ("Opening photo.orf with darktable…"); at the right buttons for
// Settings and "?" for the shortcuts.
//
// Loop thread only. Must not outlive the loop or the pool.
class StatusBar {
public:
    static constexpr float kHeight = 30.0F;
    static constexpr std::chrono::seconds kMessageTime{4};

    // settings: the cog was clicked; help: the "?" button was.
    StatusBar(core::EventLoop& loop, core::WorkerPool& pool, std::function<void()> redraw,
              std::function<void()> settings, std::function<void()> help);
    ~StatusBar();

    StatusBar(const StatusBar&) = delete;
    StatusBar& operator=(const StatusBar&) = delete;
    StatusBar(StatusBar&&) = delete;
    StatusBar& operator=(StatusBar&&) = delete;

    // Logical pixels (8-32). The bar grows to fit the selected font's line height.
    void set_text_size(float size);

    [[nodiscard]] float text_size() const noexcept { return text_size_; }

    [[nodiscard]] float height(ui::Renderer& renderer) const;

    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
              ui::IconSet& icons, const Folder& folder, bool settings_enabled = true);

    // Reads the free space of folder unless it is known for its current listing (draw() does
    // this; tests call it without drawing).
    void update(const Folder& folder);

    [[nodiscard]] bool claims(float x, float y) const;

    // A button is there (Settings, "?", a transfer's ×), as last drawn: the pointer is a hand.
    [[nodiscard]] bool clickable_at(float x, float y) const;

    // Shows text in place of the status for kMessageTime; a new message replaces it. error
    // draws it in the error color.
    void show_message(std::string text, bool error);

    // Persistent status for a collection (for example, recursive search progress). Messages
    // and transfers take priority; an empty string restores the ordinary folder status.
    void set_context_status(std::string text);

    void set_type_jump(std::string prefix, bool matched = true) {
        type_jump_ = std::move(prefix);
        type_jump_matched_ = matched;
        redraw_();
    }

    void set_collection_status(std::string text) { collection_status_ = std::move(text); }

    // The message shown; empty when there is none.
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

    // A transfer in progress (nullopt: none): its line in place of the status, a thin bar along
    // the top edge, and a button to stop it (cancel). A message still comes first.
    void set_transfer(std::optional<TransferStatus> transfer);

    [[nodiscard]] const std::optional<TransferStatus>& transfer() const noexcept {
        return transfer_;
    }

    void set_cancel_handler(std::function<void()> cancel) { cancel_ = std::move(cancel); }

    void pointer(const ui::PointerEvent& event);

    [[nodiscard]] ui::Rect settings_bounds() const { return buttons_[0]; }

    [[nodiscard]] ui::Rect help_bounds() const { return buttons_[1]; }

    [[nodiscard]] ui::Rect cancel_bounds() const { return cancel_button_; }

    // The free space as last read; nullopt before it arrives or when it cannot be read.
    [[nodiscard]] const std::optional<FreeSpace>& free_space() const noexcept { return free_; }

    // The free space is being read (snapshots, tests).
    [[nodiscard]] bool loading() const noexcept { return job_.has_value(); }

private:
    std::string collection_status_;
    std::string type_jump_;
    bool type_jump_matched_{true};
    // Settings (0) or help (1).
    [[nodiscard]] std::optional<std::size_t> button_at(float x, float y) const;

    core::EventLoop& loop_;
    core::WorkerPool& pool_;
    std::function<void()> redraw_;
    std::function<void()> settings_;
    std::function<void()> help_;

    // What free_ is about: the folder and its listing's revision.
    std::filesystem::path path_;
    std::uint64_t revision_{0};
    bool known_{false};
    std::optional<FreeSpace> free_;
    std::optional<core::CancelToken> job_;

    std::optional<TransferStatus> transfer_;
    std::function<void()> cancel_;
    ui::Rect cancel_button_{};  // as last drawn; empty without a transfer
    bool cancel_hovered_{false};
    std::string message_;
    std::string context_status_;
    bool message_error_{false};
    core::EventLoop::SourceId message_timer_{0};

    ui::Rect bounds_{};
    float text_size_{12.0F};
    std::array<ui::Rect, 2> buttons_{};  // as last drawn
    std::optional<std::size_t> hovered_;
    // False once destroyed: results still on their way find out here.
    std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
};

}  // namespace rummage::views
