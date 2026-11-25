#include "rummage/platform/browser_instance.hpp"

#include <cstdint>
#include <format>
#include <sdbus-c++/sdbus-c++.h>
#include <utility>

#include "bus_loop.hpp"
#include "rummage/core/log.hpp"

namespace rummage::platform {
namespace {
constexpr auto kInterface = "org.rummage.Browser1";
constexpr auto kPath = "/org/rummage/Browser";
}  // namespace

bool valid_browser_request(const BrowserRequest& request) {
    if (request.show != Show::folders && request.show != Show::items &&
        request.show != Show::properties)
        return false;
    if (request.paths.empty() || request.paths.size() > 256 || request.token.size() > 4096 ||
        request.token.find('\0') != std::string::npos)
        return false;
    std::size_t bytes = request.token.size();
    for (const auto& path : request.paths) {
        if (path.empty() || path.size() > 4096 || path.find('\0') != std::string::npos ||
            (path != "trash:///" && !std::filesystem::path{path}.is_absolute()))
            return false;
        bytes += path.size();
    }
    return bytes <= 1024U * 1024U;
}

std::string browser_instance_name(std::string_view config, std::string_view display) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto part : {config, std::string_view{"\0", 1}, display})
        for (const char c : part) {
            hash ^= static_cast<unsigned char>(c);
            hash *= 1099511628211ULL;
        }
    return std::format("org.rummage.Browser.p{:016x}", hash);
}

BrowserForward forward_browser_request(const std::string& name, const BrowserRequest& request) {
    if (!valid_browser_request(request))
        return BrowserForward::rejected;
    std::unique_ptr<sdbus::IConnection> connection;
    try {
        connection = sdbus::createSessionBusConnection();
    } catch (const sdbus::Error&) {
        return BrowserForward::unavailable;
    }
    try {
        const auto proxy =
            sdbus::createProxy(*connection, sdbus::ServiceName{name}, sdbus::ObjectPath{kPath});
        bool accepted = false;
        proxy->callMethod("Open")
            .onInterface(kInterface)
            .withTimeout(std::chrono::seconds{2})
            .withArguments(static_cast<std::uint32_t>(request.show), request.paths, request.token,
                           request.new_window)
            .storeResultsTo(accepted);
        return accepted ? BrowserForward::accepted : BrowserForward::rejected;
    } catch (const sdbus::Error& error) {
        if (error.getName() == "org.freedesktop.DBus.Error.ServiceUnknown" ||
            error.getName() == "org.freedesktop.DBus.Error.NameHasNoOwner")
            return BrowserForward::unavailable;
        // An uncertain reply must not start another process and duplicate a late request.
        log::warn("browser launch: {}", error.what());
        return BrowserForward::rejected;
    }
}

struct BrowserInstance::Impl {
    struct Pending {
        std::function<void(BrowserRequest)> open;
        std::size_t count{};
    };

    std::shared_ptr<Pending> pending;
    std::unique_ptr<sdbus::IConnection> connection;
    std::unique_ptr<sdbus::IObject> object;
    std::unique_ptr<BusLoop> bus;

    Impl(core::EventLoop& loop, const std::string& name, std::function<void(BrowserRequest)> open)
        : pending{std::make_shared<Pending>(Pending{std::move(open), 0})},
          connection{sdbus::createSessionBusConnection()},
          object{sdbus::createObject(*connection, sdbus::ObjectPath{kPath})} {
        object
            ->addVTable(sdbus::registerMethod("Open").implementedAs(
                [this, &loop](std::uint32_t show, const std::vector<std::string>& paths,
                              const std::string& token, bool new_window) {
                    // Validate the wire value before narrowing the enum.
                    if (show > static_cast<std::uint32_t>(Show::properties))
                        return false;
                    BrowserRequest request{static_cast<Show>(show), paths, token, new_window};
                    if (!valid_browser_request(request) || pending->count >= 32)
                        return false;
                    ++pending->count;
                    loop.post(
                        [weak = std::weak_ptr{pending}, request = std::move(request)]() mutable {
                            if (const auto state = weak.lock()) {
                                --state->count;
                                state->open(std::move(request));
                            }
                        });
                    return true;
                }))
            .forInterface(kInterface);
        connection->requestName(sdbus::ServiceName{name});
        bus = std::make_unique<BusLoop>(loop, *connection, "browser launch");
        bus->pump();
    }
};

BrowserInstance::BrowserInstance(core::EventLoop& loop, const std::string& name,
                                 std::function<void(BrowserRequest)> open)
    : impl_{std::make_unique<Impl>(loop, name, std::move(open))} {}

BrowserInstance::~BrowserInstance() = default;

}  // namespace rummage::platform
