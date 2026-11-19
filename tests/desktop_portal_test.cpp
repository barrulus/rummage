#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <sdbus-c++/sdbus-c++.h>
#include <stdexcept>
#include <string>
#include <vector>

#include "rummage/platform/portal.hpp"

namespace {
using namespace std::chrono_literals;
using Values = std::map<std::string, sdbus::Variant>;
using Namespaces = std::map<std::string, Values>;
constexpr auto kNamespace = "org.freedesktop.appearance";
constexpr auto kInterface = "org.freedesktop.portal.Settings";

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error{message};
}

void check(const Values& values, bool initial_contrast) {
    auto connection = sdbus::createSessionBusConnection();
    auto object =
        sdbus::createObject(*connection, sdbus::ObjectPath{"/org/freedesktop/portal/desktop"});
    object
        ->addVTable(sdbus::registerMethod("ReadAll").implementedAs(
            [&](const std::vector<std::string>&) { return Namespaces{{kNamespace, values}}; }))
        .forInterface(kInterface);
    connection->requestName(sdbus::ServiceName{"org.freedesktop.portal.Desktop"});
    connection->enterEventLoopAsync();

    rummage::core::EventLoop loop;
    int changes = 0;
    bool motion = false;
    rummage::platform::DesktopPortal* current = nullptr;
    rummage::platform::DesktopPortal portal{
        loop, [&] {
            ++changes;
            if (current && current->appearance().reduced_motion == motion)
                loop.quit(0);
        }};
    current = &portal;
    require(portal.available(), "ReadAll did not supply appearance settings");
    require(portal.appearance().high_contrast == initial_contrast, "Wrong startup contrast");
    const auto original = portal.appearance();

    const auto emit = [&](const char* space, const char* key, const sdbus::Variant& value) {
        object->emitSignal("SettingChanged")
            .onInterface(kInterface)
            .withArguments(std::string{space}, std::string{key}, value);
    };
    const auto update = [&](const sdbus::Variant& value, bool expected,
                            const char* space = kNamespace, const char* key = "contrast") {
        const bool previous = portal.appearance().high_contrast;
        const int before = changes;
        motion = !motion;
        emit(space, key, value);
        // A second setting is an ordered barrier: it must be delivered even when a duplicate
        // or irrelevant contrast signal correctly produces no change callback.
        emit(kNamespace, "reduced-motion", sdbus::Variant{std::uint32_t{motion ? 1U : 0U}});
        const auto timeout = loop.add_timer(2s, 0ms, [&] { loop.quit(1); });
        require(loop.run() == 0, "SettingChanged delivery timed out");
        loop.remove(timeout);
        require(portal.appearance().high_contrast == expected, "Wrong live contrast");
        require(changes == before + 1 + (previous != expected ? 1 : 0),
                "Unexpected number of appearance callbacks");
        require(portal.appearance().dark == original.dark, "Contrast changed the colour mode");
        require(portal.appearance().accent == original.accent, "Contrast changed the accent");
    };
    update(sdbus::Variant{1U}, true);
    update(sdbus::Variant{1U}, true);
    update(sdbus::Variant{0U}, true, "org.example.unrelated");
    update(sdbus::Variant{0U}, true, kNamespace, "unknown-setting");
    update(sdbus::Variant{0U}, false);
    update(sdbus::Variant{1U}, true);
    update(sdbus::Variant{2U}, false);
    update(sdbus::Variant{1U}, true);
    update(sdbus::Variant{std::string{"1"}}, false);
    update(sdbus::Variant{1U}, true);
    update(sdbus::Variant{true}, false);
    connection->leaveEventLoop();
}
}  // namespace

int main() {
    try {
        const Values normal{{"color-scheme", sdbus::Variant{1U}},
                            {"accent-color", sdbus::Variant{sdbus::Struct{0.2, 0.5, 0.9}}}};
        check(normal, false);  // An older backend need not expose contrast at all.
        for (const auto& value : {sdbus::Variant{0U}, sdbus::Variant{1U}, sdbus::Variant{7U},
                                  sdbus::Variant{std::string{"invalid"}}}) {
            Values values = normal;
            values.emplace("contrast", value);
            check(values,
                  value.containsValueOfType<std::uint32_t>() && value.get<std::uint32_t>() == 1U);
        }
        check({{"contrast", sdbus::Variant{1U}}}, true);
        std::cout << "Appearance startup and live contrast checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
