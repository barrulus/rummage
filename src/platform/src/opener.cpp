#include "rummage/platform/opener.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
#include <optional>
#include <string_view>
#include <utility>

#include "rummage/core/i18n.hpp"
#include "rummage/core/log.hpp"
#include "rummage/fs/permissions.hpp"
#include "rummage/platform/exec.hpp"
#include "rummage/platform/program.hpp"

namespace rummage::platform {
namespace {

// The files that go to one application.
struct Group {
    const DesktopEntry* app{nullptr};
    std::vector<std::filesystem::path> files;
};

void add_problem(std::vector<std::string>& problems, std::string problem) {
    if (std::ranges::find(problems, problem) == problems.end()) {
        problems.push_back(std::move(problem));
    }
}

// "Opening photo.orf with darktable…", "Opening 5 files with darktable…", "Starting tool…".
std::string opening_message(const OpenPlan& plan) {
    if (plan.launches.size() == 1 && plan.launches.front().program) {
        return i18n::trf("Starting {}…", plan.launches.front().app_name);
    }
    std::vector<std::string_view> apps;
    for (const OpenPlan::Launch& launch : plan.launches) {
        if (std::ranges::find(apps, launch.app_name) == apps.end()) {
            apps.push_back(launch.app_name);
        }
    }
    if (apps.size() > 1) {
        return i18n::trf("Opening {} files with {} applications…", plan.files, apps.size());
    }
    if (plan.files == 1) {
        return i18n::trf("Opening {} with {}…", plan.first_name, apps.front());
    }
    return i18n::trf("Opening {} files with {}…", plan.files, apps.front());
}

// The types may_be_program takes, their kin (subclasses) included.
constexpr std::array<std::string_view, 2> kProgramTypes{"application/x-executable",
                                                        "application/x-sharedlib"};

// The launch that starts program by itself, in its folder.
OpenPlan::Launch program_launch(const std::filesystem::path& program) {
    return {.app_id = "program",
            .app_name = program.filename().string(),
            .argv = {program.string()},
            .directory = program.parent_path(),
            .files = 1,
            .program = true};
}

}  // namespace

bool may_be_program(const fs::MimeDatabase& database, fs::MimeId mime) {
    return std::ranges::any_of(kProgramTypes, [&](std::string_view type) {
        const fs::MimeId parent = database.find(type);
        return parent != fs::kUnknownMime && database.is_a(mime, parent);
    });
}

OpenPlan plan_open(std::span<const OpenFile> files, const Applications& apps,
                   const fs::MimeDatabase& database, const Environment& environment,
                   std::string_view app_id) {
    OpenPlan plan;
    std::vector<Group> groups;
    for (const OpenFile& file : files) {
        const DesktopEntry* const app =
            app_id.empty() ? apps.default_for(file.mime, database) : apps.find(app_id);
        if (app == nullptr) {
            add_problem(plan.problems, app_id.empty()
                                           ? i18n::trf("No application opens {}", file.mime)
                                           : i18n::trf("{} is not installed", app_id));
            continue;
        }
        auto group = std::ranges::find(groups, app, &Group::app);
        if (group == groups.end()) {
            group = groups.insert(groups.end(), Group{.app = app, .files = {}});
        }
        group->files.push_back(file.path);
    }

    for (const Group& group : groups) {
        const DesktopEntry& app = *group.app;
        const std::string name = app.name.empty() ? app.id : app.name;
        std::vector<std::vector<std::string>> lines = expand_exec(app, group.files);
        if (lines.empty()) {
            add_problem(plan.problems, i18n::trf("{} has an Exec line rummage cannot read", name));
            continue;
        }
        const std::size_t per_line = lines.size() == 1 ? group.files.size() : 1;
        bool started = false;
        for (std::vector<std::string>& line : lines) {
            if (app.terminal) {
                std::optional<std::vector<std::string>> wrapped = in_terminal(line, environment);
                if (!wrapped) {
                    add_problem(
                        plan.problems,
                        i18n::trf("No terminal to run {} in: set terminal.program in config.toml",
                                  name));
                    break;
                }
                line = std::move(*wrapped);
            } else if (const std::optional<std::filesystem::path> program =
                           find_program(line.front(), environment.path)) {
                line.front() = program->string();  // the one find() checked, from the same PATH
            }
            plan.launches.push_back({.app_id = app.id,
                                     .app_name = name,
                                     .argv = std::move(line),
                                     .directory = app.working_directory.empty()
                                                      ? group.files.front().parent_path()
                                                      : app.working_directory,
                                     .files = per_line});
            started = true;
        }
        if (started) {
            if (plan.files == 0) {
                plan.first_name = group.files.front().filename().string();
            }
            plan.files += group.files.size();
        }
    }
    return plan;
}

std::vector<AppChoice> choices_for(std::string_view mime, const Applications& apps,
                                   const fs::MimeDatabase& database) {
    std::vector<AppChoice> choices;
    const auto add = [&choices](const DesktopEntry& app, bool is_default) {
        if (std::ranges::find(choices, app.id, &AppChoice::id) == choices.end()) {
            choices.push_back({.id = app.id,
                               .name = app.name.empty() ? app.id : app.name,
                               .is_default = is_default});
        }
    };
    if (const DesktopEntry* const app = apps.default_for(mime, database)) {
        add(*app, true);
    }
    for (const DesktopEntry* const app : apps.all_for(mime, database)) {
        add(*app, false);
    }
    return choices;
}

Opener::Opener(core::EventLoop& loop, core::WorkerPool& pool,
               std::shared_ptr<const fs::MimeDatabase> database, Environment environment,
               Report report, TokenRequest token)
    : pool_{pool},
      database_{std::move(database)},
      environment_{std::make_shared<const Environment>(std::move(environment))},
      report_{std::move(report)},
      token_{std::move(token)},
      launcher_{loop} {}

Opener::~Opener() { *alive_ = false; }

void Opener::open(const std::filesystem::path& directory, std::vector<fs::Entry> entries,
                  std::string app_id) {
    if (app_id.empty()) {  // folders open in the views, or with a chosen application
        std::erase_if(entries, [](const fs::Entry& entry) { return entry.is_directory(); });
    }
    if (entries.empty()) {
        return;
    }
    ++planning_;
    pool_.submit(
        core::Priority::visible,
        [directory, entries = std::move(entries), database = database_, environment = environment_,
         app_id = std::move(app_id), programs = programs_](const core::CancelToken& /*token*/) {
            std::vector<OpenFile> files;
            std::vector<OpenPlan::Launch> started;
            std::vector<std::filesystem::path> not_allowed;
            std::vector<std::string> problems;
            for (const fs::Entry& entry : entries) {
                const fs::MimeId found = entry.mime != fs::kUnknownMime
                                             ? entry.mime
                                             : database->detect(directory, entry);
                const std::filesystem::path path = directory / entry.name;
                if (programs && app_id.empty() && entry.kind == fs::EntryKind::file &&
                    may_be_program(*database, found)) {
                    const ProgramCheck check = check_program(path);
                    if (check.program) {
                        if (check.noexec) {
                            problems.push_back(i18n::trf(
                                "“{}” is on a disk that does not let programs run", entry.name));
                        } else if (check.executable) {
                            started.push_back(program_launch(path));
                        } else {
                            not_allowed.push_back(path);
                        }
                        continue;
                    }
                }
                const fs::MimeId mime = database->to_open(found, entry.name);
                files.push_back({.path = path, .mime = std::string{database->name(mime)}});
            }
            OpenPlan plan;
            if (!files.empty()) {
                const Applications apps = Applications::load(*environment, *database);
                plan = plan_open(files, apps, *database, *environment, app_id);
            }
            if (!started.empty() && plan.files == 0) {
                plan.first_name = started.front().app_name;
            }
            plan.files += started.size();
            plan.launches.insert(plan.launches.begin(), std::make_move_iterator(started.begin()),
                                 std::make_move_iterator(started.end()));
            plan.problems.insert(plan.problems.begin(), problems.begin(), problems.end());
            plan.not_allowed = std::move(not_allowed);
            return plan;
        },
        [this, alive = alive_](OpenPlan plan) {
            if (*alive) {
                --planning_;
                start(std::move(plan));
            }
        });
}

void Opener::choices(const std::filesystem::path& directory, fs::Entry entry,
                     std::function<void(std::vector<AppChoice>)> done) {
    pool_.submit(
        core::Priority::visible,
        [directory, entry = std::move(entry), database = database_,
         environment = environment_](const core::CancelToken& /*token*/) {
            const fs::MimeId mime = database->to_open(
                entry.mime != fs::kUnknownMime ? entry.mime : database->detect(directory, entry),
                entry.name);
            const Applications apps = Applications::load(*environment, *database);
            return choices_for(database->name(mime), apps, *database);
        },
        [alive = alive_, done = std::move(done)](std::vector<AppChoice> choices) {
            if (*alive) {
                done(std::move(choices));
            }
        });
}

void Opener::allow_and_run(const std::filesystem::path& program) {
    if (!programs_)
        return;
    ++planning_;
    pool_.submit(
        core::Priority::visible,
        [program](const core::CancelToken& /*token*/) {
            OpenPlan plan;
            const fs::ModeResult result = fs::set_running({program}, true);
            if (!result.errors.empty()) {
                plan.problems.push_back(
                    i18n::trf("Could not allow it to run: {}", result.errors.front()));
                return plan;
            }
            const ProgramCheck check = check_program(program);
            if (!check.program || !check.executable || check.noexec) {
                plan.problems.push_back(
                    i18n::trf("“{}” still cannot run", program.filename().string()));
                return plan;
            }
            plan.launches.push_back(program_launch(program));
            plan.files = 1;
            plan.first_name = program.filename().string();
            return plan;
        },
        [this, alive = alive_](OpenPlan plan) {
            if (*alive) {
                --planning_;
                start(std::move(plan));
            }
        });
}

void Opener::set_terminal(std::vector<std::string> configured) {
    // A new copy: the workers planning now keep the one they took.
    auto fresh = std::make_shared<Environment>(*environment_);
    fresh->configured_terminal = std::move(configured);
    environment_ = std::move(fresh);
}

void Opener::open_terminal(const std::filesystem::path& directory) {
    ++planning_;
    pool_.submit(
        core::Priority::visible,
        [directory, environment = environment_](const core::CancelToken& /*token*/) {
            OpenPlan plan;
            std::optional<std::vector<std::string>> line = terminal_alone(*environment);
            if (!line) {
                plan.problems.emplace_back(
                    i18n::tr("No terminal found: set terminal.program in config.toml"));
                return plan;
            }
            const std::string name = std::filesystem::path{line->front()}.filename().string();
            plan.launches.push_back({.app_id = name,
                                     .app_name = name,
                                     .argv = std::move(*line),
                                     .directory = directory,
                                     .files = 1});
            plan.files = 1;
            plan.first_name =
                directory.has_filename() ? directory.filename().string() : directory.string();
            return plan;
        },
        [this, alive = alive_](OpenPlan plan) {
            if (*alive) {
                --planning_;
                start(std::move(plan));
            }
        });
}

void Opener::start(OpenPlan plan) {
    if (!programs_) {
        std::erase_if(plan.launches, [](const auto& launch) { return launch.program; });
        plan.not_allowed.clear();
    }
    for (const std::string& problem : plan.problems) {
        log::info("open: {}", problem);
    }
    if (!plan.not_allowed.empty()) {
        // One question at a time: the first; the others are named.
        if (plan.not_allowed.size() > 1) {
            plan.problems.insert(
                plan.problems.begin(),
                i18n::trf("{} more programs are not allowed to run", plan.not_allowed.size() - 1));
        }
        const std::filesystem::path first = plan.not_allowed.front();
        if (ask_) {
            ask_(first, [this, alive = alive_, first] {
                if (*alive) {
                    allow_and_run(first);
                }
            });
        } else {
            plan.problems.insert(plan.problems.begin(),
                                 i18n::trf("“{}” is a program but is not allowed to run",
                                           first.filename().string()));
        }
        if (plan.launches.empty() && plan.problems.empty()) {
            return;  // the question is the answer
        }
    }
    if (plan.launches.empty()) {
        report_(plan.problems.empty() ? std::string{i18n::tr("Nothing to open")}
                                      : plan.problems.front(),
                true);
        return;
    }
    // A problem matters more than what went well, which shows on screen anyway.
    if (plan.problems.empty()) {
        report_(opening_message(plan), false);
    } else {
        report_(plan.problems.front(), true);
    }
    for (OpenPlan::Launch& launch : plan.launches) {
        if (!token_) {
            start_one(launch, {});
            continue;
        }
        token_([this, alive = alive_, launch = std::move(launch)](const std::string& token) {
            if (*alive) {
                start_one(launch, token);
            }
        });
    }
}

void Opener::start_one(const OpenPlan::Launch& launch, const std::string& token) {
    if (launch.program && !programs_)
        return;
    std::vector<std::string> env;
    if (!token.empty()) {
        // XDG_ACTIVATION_TOKEN for Wayland apps; DESKTOP_STARTUP_ID for the ones that only know
        // startup notification (GLib sets both).
        env.push_back("XDG_ACTIVATION_TOKEN=" + token);
        env.push_back("DESKTOP_STARTUP_ID=" + token);
    }
    log::info("open: {} with {} ({} file(s))", launch.argv.front(), launch.app_id, launch.files);
    const std::error_code error = launcher_.spawn(
        launch.argv, launch.directory, env,
        [this, alive = alive_, name = launch.app_name](const Launcher::Exit& exit) {
            if (!*alive || exit.lifetime >= kQuickExit || (exit.status == 0 && exit.signal == 0)) {
                return;
            }
            report_(exit.signal != 0
                        ? i18n::trf("{} was stopped by signal {}", name, exit.signal)
                        : i18n::trf("{} quit at once with status {}", name, exit.status),
                    true);
        });
    if (error) {
        report_(i18n::trf("Could not start {}: {}", launch.app_name, error.message()), true);
    }
}

}  // namespace rummage::platform
