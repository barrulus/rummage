#include "rummage/core/accessibility.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <sdbus-c++/sdbus-c++.h>
#include <thread>

#include "rummage/platform/accessibility.hpp"

namespace {
using namespace std::chrono_literals;
using Ref = sdbus::Struct<std::string, sdbus::ObjectPath>;

void require(bool good, const char* message) {
    if (!good)
        throw std::runtime_error{message};
}
}  // namespace

int main() {
    try {
        const auto* address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
        require(address != nullptr, "Run under dbus-run-session");
        setenv("AT_SPI_BUS_ADDRESS", address, 1);
        unsetenv("NO_AT_BRIDGE");
        auto registry =
            sdbus::createSessionBusConnection(sdbus::ServiceName{"org.a11y.atspi.Registry"});
        auto socket =
            sdbus::createObject(*registry, sdbus::ObjectPath{"/org/a11y/atspi/accessible/root"});
        std::promise<Ref> embedded;
        socket
            ->addVTable(sdbus::registerMethod("Embed").implementedAs([&](const Ref& application) {
                embedded.set_value(application);
                return Ref{registry->getUniqueName(),
                           sdbus::ObjectPath{"/org/a11y/atspi/accessible/root"}};
            }))
            .forInterface("org.a11y.atspi.Socket");
        registry->enterEventLoopAsync();
        rummage::core::EventLoop loop;
        rummage::platform::Accessibility bridge{loop};
        std::atomic<bool> edited{}, invalidated{};
        std::atomic<int> replacement_actions{};
        bridge.set_ready_handler([&] {
            rummage::core::AccessibleNode entry;
            entry.id = "name";
            entry.name = "File name";
            entry.role = rummage::core::AccessibleRole::entry;
            entry.text = "日本語.txt";
            entry.caret = entry.anchor = 3;
            entry.focused = entry.editable = true;
            entry.perform = [&](const auto& edit) {
                if (edit.kind == rummage::core::AccessibleEdit::Kind::activate) {
                    bridge.remove_window("test");
                    rummage::core::AccessibleNode replacement;
                    replacement.id = "name";
                    replacement.name = "Replacement dialog action";
                    replacement.role = rummage::core::AccessibleRole::button;
                    replacement.perform = [&](const auto&) {
                        ++replacement_actions;
                        return true;
                    };
                    bridge.update_window("test", "Replacement", {replacement});
                    invalidated.store(true);
                    return true;
                }
                if (edit.kind == rummage::core::AccessibleEdit::Kind::set_text &&
                    edit.text == "新しい.txt" && edit.first == 0 && edit.last == 7)
                    edited.store(true);
                return true;
            };
            auto password = entry;
            password.id = "password";
            password.name = "Password";
            password.role = rummage::core::AccessibleRole::password;
            password.password = true;
            password.text = "••••";
            password.caret = password.anchor = 4;
            password.focused = false;
            bridge.update_window("test", "Files", {entry, password});
        });
        std::exception_ptr failure;
        std::jthread client{[&] {
            try {
                auto future = embedded.get_future();
                require(future.wait_for(3s) == std::future_status::ready, "Bridge did not Embed");
                const auto application = future.get();
                auto bus = sdbus::createSessionBusConnection();
                const auto proxy = [&](const Ref& ref) {
                    return sdbus::createProxy(*bus, sdbus::ServiceName{ref.get<0>()}, ref.get<1>());
                };
                auto app = proxy(application);
                std::vector<Ref> frames;
                for (int n = 0; n < 100 && frames.empty(); ++n) {
                    app->callMethod("GetChildren")
                        .onInterface("org.a11y.atspi.Accessible")
                        .storeResultsTo(frames);
                    if (frames.empty())
                        std::this_thread::sleep_for(10ms);
                }
                require(frames.size() == 1, "Application must expose window");
                auto frame = proxy(frames[0]);
                std::vector<Ref> controls;
                frame->callMethod("GetChildren")
                    .onInterface("org.a11y.atspi.Accessible")
                    .storeResultsTo(controls);
                require(controls.size() == 2, "Window children missing");
                auto field = proxy(controls[0]);
                require(field->getProperty("Name")
                                .onInterface("org.a11y.atspi.Accessible")
                                .get<std::string>() == "File name",
                        "Accessible label missing");
                std::string text;
                field->callMethod("GetText")
                    .onInterface("org.a11y.atspi.Text")
                    .withArguments(0, 3)
                    .storeResultsTo(text);
                require(text == "日本語", "Text offsets must count Unicode characters");
                require(field->getProperty("CharacterCount")
                                .onInterface("org.a11y.atspi.Text")
                                .get<int>() == 7,
                        "Wrong character count");
                bool accepted{};
                field->callMethod("SetTextContents")
                    .onInterface("org.a11y.atspi.EditableText")
                    .withArguments(std::string{"新しい.txt"})
                    .storeResultsTo(accepted);
                require(accepted, "EditableText rejected edit");
                for (int n = 0; n < 100 && !edited.load(); ++n)
                    std::this_thread::sleep_for(10ms);
                require(edited.load(), "Accessible edit did not reach event loop");
                auto secret = proxy(controls[1]);
                secret->callMethod("GetText")
                    .onInterface("org.a11y.atspi.Text")
                    .withArguments(0, -1)
                    .storeResultsTo(text);
                require(text == "••••", "Password accessibility exposed content");
                secret->callMethod("CutText")
                    .onInterface("org.a11y.atspi.EditableText")
                    .withArguments(0, 4)
                    .storeResultsTo(accepted);
                require(!accepted, "Password accessible cut must not expose clipboard content");
                // Hold the UI loop while both messages reach the bus. They are dispatched
                // in one pump and queued before the first action replaces the window.
                std::promise<void> waiting, release;
                auto proceed = release.get_future();
                loop.post([&] {
                    waiting.set_value();
                    proceed.wait();
                });
                waiting.get_future().wait();
                field->callMethod("DoAction")
                    .onInterface("org.a11y.atspi.Action")
                    .withArguments(0)
                    .dontExpectReply();
                field->callMethod("DoAction")
                    .onInterface("org.a11y.atspi.Action")
                    .withArguments(0)
                    .dontExpectReply();
                std::this_thread::sleep_for(30ms);
                release.set_value();
                for (int n = 0; n < 100 && !invalidated.load(); ++n)
                    std::this_thread::sleep_for(10ms);
                require(invalidated.load(), "First queued action did not replace its window");
                std::this_thread::sleep_for(30ms);
                require(replacement_actions.load() == 0,
                        "Stale queued action reached replacement content");
            } catch (...) {
                failure = std::current_exception();
            }
            loop.post([&] {
                bridge.remove_window("test");
                loop.quit();
            });
        }};
        loop.add_timer(8s, 0ms, [&] { loop.quit(2); });
        require(loop.run() == 0, "Accessibility bus test timed out");
        client.join();
        if (failure)
            std::rethrow_exception(failure);
        std::cout << "AT-SPI discovery, native tree, Unicode text, queued editing and password "
                     "privacy and stale-action cancellation passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
