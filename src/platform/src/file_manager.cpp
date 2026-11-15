#include "rummage/platform/file_manager.hpp"

#include <memory>
#include <sdbus-c++/sdbus-c++.h>
#include <stdexcept>
#include <utility>

#include "bus_loop.hpp"
#include "rummage/core/file.hpp"
#include "rummage/core/log.hpp"
#include "rummage/platform/file_manager_config.hpp"
#include "rummage/platform/launcher.hpp"

namespace rummage::platform {

std::vector<std::string> show_arguments(Show show, const std::vector<std::string>& uris) {
    std::vector<std::string> paths;
    for (const auto& uri : uris) {
        if (const auto path = core::path_from_uri(uri)) {
            paths.push_back(path->string());
        }
    }
    if (paths.empty())
        return {};
    std::vector<std::string> arguments;
    switch (show) {
        case Show::folders:
            arguments.emplace_back("--open-folders");
            break;
        case Show::items:
            arguments.emplace_back("--reveal");
            break;
        case Show::properties:
            arguments.emplace_back("--properties");
            break;
    }
    arguments.emplace_back("--");
    arguments.insert(arguments.end(), paths.begin(), paths.end());
    return arguments;
}

namespace {
class FileManager {
public:
    FileManager(core::EventLoop& loop, FileManagerOptions options)
        : loop_{loop}, options_{std::move(options)}, launcher_{loop} {
        if (options_.program.empty()) {
            std::error_code error;
            const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
            const auto program = file_manager_program(executable);
            if (error || !program)
                throw std::runtime_error{"cannot locate rummage executable"};
            options_.program = *program;
        }
        connection_ = sdbus::createSessionBusConnection();
        object_ =
            sdbus::createObject(*connection_, sdbus::ObjectPath{"/org/freedesktop/FileManager1"});
        const auto method = [this](const char* name, Show what) {
            return sdbus::registerMethod(name)
                .withInputParamNames("uris", "startup_id")
                .implementedAs(
                    [this, what](const std::vector<std::string>& uris, const std::string& token) {
                        rest();
                        // The D-Bus handler does no filesystem I/O or process startup. Send the
                        // method's reply first, then parse URIs and start the requested window.
                        // Filesystem checks belong to that child: an unresponsive mount must
                        // never prevent this service from replying to other applications.
                        loop_.post([this, what, uris, token] { show(what, uris, token); });
                    });
        };
        object_
            ->addVTable(method("ShowFolders", Show::folders), method("ShowItems", Show::items),
                        method("ShowItemProperties", Show::properties))
            .forInterface(std::string{kFileManagerBusName});
        connection_->requestName(sdbus::ServiceName{std::string{kFileManagerBusName}});
        bus_ = std::make_unique<BusLoop>(loop_, *connection_, "file manager");
        bus_->set_lost_handler([this] { loop_.quit(1); });
        bus_->pump();
        rest();
    }

    ~FileManager() { loop_.remove(idle_); }

    FileManager(const FileManager&) = delete;
    FileManager& operator=(const FileManager&) = delete;
    FileManager(FileManager&&) = delete;
    FileManager& operator=(FileManager&&) = delete;

private:
    void rest() {
        loop_.remove(idle_);
        idle_ = 0;
        if (options_.idle_timeout.count() > 0) {
            idle_ = loop_.add_timer(options_.idle_timeout, std::chrono::milliseconds{0}, [this] {
                idle_ = 0;
                loop_.quit(0);
            });
        }
    }

    void show(Show what, const std::vector<std::string>& uris, const std::string& token) {
        auto argv = show_arguments(what, uris);
        if (argv.empty())
            return;
        argv.insert(argv.begin(), options_.program.string());
        // Override even empty tokens so the long-lived service cannot reuse a launcher's token.
        const std::vector<std::string> env{"XDG_ACTIVATION_TOKEN=" + token,
                                           "DESKTOP_STARTUP_ID=" + token};
        if (const auto error = launcher_.spawn(argv, {}, env, [](Launcher::Exit) {})) {
            log::error("file manager: cannot start {}: {}", options_.program.string(),
                       error.message());
        }
    }

    core::EventLoop& loop_;
    FileManagerOptions options_;
    Launcher launcher_;
    core::EventLoop::SourceId idle_{};
    std::unique_ptr<sdbus::IConnection> connection_;
    std::unique_ptr<sdbus::IObject> object_;
    std::unique_ptr<BusLoop> bus_;
};
}  // namespace

int run_file_manager(core::EventLoop& loop, FileManagerOptions options) {
    FileManager manager{loop, std::move(options)};
    return loop.run();
}

}  // namespace rummage::platform
