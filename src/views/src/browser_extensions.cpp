#include <algorithm>

#include "rummage/core/i18n.hpp"
#include "rummage/views/browser.hpp"

namespace rummage::views {
namespace ext = platform::extensions;

void Browser::set_extensions(const core::Config::Extensions& config) {
    ext::configure(config);
    extension_job_.cancel();
    extension_running_ = false;
    extension_job_ = pool_.submit(
        core::Priority::prefetch, [](const core::CancelToken&) { return ext::registry(); },
        [this, alive = alive_](std::shared_ptr<const ext::Registry> value) {
            if (!*alive)
                return;
            menu_.close();
            palette_.close();
            if (!value->problems.empty())
                show_action_result(value->problems.front(), true);
            redraw_();
        });
}

std::vector<MenuItem> Browser::extension_items(const core::ActionContext& context) {
    std::vector<MenuItem> items;
    if (context.directory.empty() || context.trash || picker_mode_)
        return items;
    const auto registry = ext::snapshot();
    if (registry->config.git) {
        MenuItem menu;
        menu.id = "git";
        menu.label = i18n::tr("Git");
        menu.submenu = true;
        if (const auto folder = current_folder();
            folder && folder->single_selection() && folder->single_selection()->decoration) {
            MenuItem status;
            status.id = "git:status";
            status.label = folder->single_selection()->decoration->label;
            status.enabled = false;
            menu.children.push_back(std::move(status));
        }
        for (const bool staged : {false, true}) {
            MenuItem item;
            item.id = staged ? "git:staged" : "git:diff";
            item.label =
                i18n::tr(staged ? i18n::mark("View staged Git diff") : i18n::mark("View Git diff"));
            item.enabled = !extension_running_;
            item.action = [this, context, staged] { show_git_diff(context, staged); };
            menu.children.push_back(std::move(item));
        }
        MenuItem tool;
        tool.id = "git:tool";
        tool.label = i18n::tr("Open repository in Git tool");
        const auto folder = current_folder();
        const auto root = folder ? folder->repository() : std::filesystem::path{};
        tool.enabled = !registry->config.git_tool.empty() && !root.empty() && bool(action_handoff_);
        tool.action = [this, root, argv = registry->config.git_tool] {
            core::Action action;
            action.id = "git-tool";
            action.label = "Git";
            action.selection = core::Action::Selection::any;
            action.argv = argv;
            action.argv.push_back("{directory}");
            core::ActionContext target;
            target.directory = root;
            invoke_action(action, target);
        };
        menu.children.push_back(std::move(tool));
        items.push_back(std::move(menu));
    }
    if (!registry->config.enabled.empty() || registry->config.git) {
        MenuItem menu;
        menu.id = "extensions";
        menu.label = i18n::tr("Extensions");
        menu.submenu = true;
        for (const auto& extension : registry->extensions)
            for (const auto& command : extension.commands) {
                core::Action match;
                match.selection = command.selection;
                if (!core::action_matches(match, context))
                    continue;
                MenuItem item;
                item.id = "extension:" + extension.id + ":" + command.id;
                item.label = extension.label + ": " + command.label;
                item.enabled = !extension_running_;
                item.action = [this, extension, command, context] {
                    invoke_extension(extension, command, context);
                };
                menu.children.push_back(std::move(item));
            }
        MenuItem reload;
        reload.id = "extensions:reload";
        reload.label = i18n::tr("Reload extensions");
        reload.action = [this] {
            const auto config = ext::snapshot()->config;
            ext::reload();
            set_extensions(config);
        };
        menu.children.push_back(std::move(reload));
        MenuItem info;
        info.id = "extensions:status";
        info.label = i18n::tr("Extension status");
        info.action = [this] {
            const auto current = ext::snapshot();
            std::string text;
            for (const auto& problem : current->problems)
                text += problem + "\n";
            for (const auto& extension : current->extensions)
                text += extension.id + ": " + extension.label + "\n";
            if (const auto folder = current_folder();
                folder && !folder->decoration_problem().empty())
                text += folder->decoration_problem() + "\n";
            extension_output_.push(text.empty() ? std::string{i18n::tr("No extension errors")}
                                                : text);
        };
        menu.children.push_back(std::move(info));
        items.push_back(std::move(menu));
    }
    return items;
}

void Browser::invoke_extension(ext::Extension extension, ext::Command command,
                               core::ActionContext context) {
    if (extension_running_)
        return;
    const auto current = ext::snapshot();
    if (std::ranges::find(current->config.enabled, extension.id) == current->config.enabled.end())
        return;
    extension_running_ = true;
    const auto version = current->generation;
    extension_job_ = pool_.submit(
        core::Priority::visible,
        [extension = std::move(extension), command = std::move(command),
         context = std::move(context)](const core::CancelToken& token) {
            return ext::invoke(extension, command, context, token);
        },
        [this, alive = alive_, version](ext::Result result) {
            if (!*alive)
                return;
            extension_running_ = false;
            if (version != ext::snapshot()->generation)
                return;
            show_action_result(std::move(result.message), result.failed);
            redraw_();
        });
}

void Browser::show_git_diff(core::ActionContext context, bool staged) {
    if (extension_running_)
        return;
    extension_running_ = true;
    extension_job_ = pool_.submit(
        core::Priority::visible,
        [context = std::move(context), staged](const core::CancelToken& token) {
            return ext::git_diff(context, staged, token);
        },
        [this, alive = alive_](ext::ProcessResult result) {
            if (!*alive)
                return;
            extension_running_ = false;
            if (result.code != 0)
                show_action_result(result.error.empty()
                                       ? std::string{i18n::tr("Git diff unavailable")}
                                       : result.error,
                                   true);
            else
                extension_output_.push(
                    result.output.empty()
                        ? std::string{i18n::tr("No Git changes in this selection")}
                        : result.output);
            redraw_();
        });
}
}  // namespace rummage::views
