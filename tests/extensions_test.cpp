#include "rummage/platform/extensions.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <thread>

#include <doctest/doctest.h>

#include "rummage/core/file.hpp"
#include "temp_dir.hpp"

using namespace std::chrono_literals;
namespace ext = rummage::platform::extensions;
using rummage::core::CancelToken;
using rummage::test::TempDir;

namespace {
struct Environment {
    std::string name;
    std::optional<std::string> before;

    Environment(std::string key, const std::string& value) : name{std::move(key)} {
        if (const auto* old = std::getenv(name.c_str()))
            before = old;
        setenv(name.c_str(), value.c_str(), 1);
    }

    ~Environment() {
        if (before)
            setenv(name.c_str(), before->c_str(), 1);
        else
            unsetenv(name.c_str());
    }
};

void git(const std::filesystem::path& directory, std::vector<std::string> args) {
    args.insert(args.begin(), {"git", "-C", directory.string()});
    const auto result = ext::run(args, directory, {}, CancelToken{});
    INFO(result.error, result.output);
    REQUIRE(result.code == 0);
}

void init(const std::filesystem::path& path) {
    git(path, {"init", "-q", "-b", "main"});
    git(path, {"config", "user.name", "Fixture"});
    git(path, {"config", "user.email", "fixture@example.invalid"});
    git(path, {"config", "commit.gpgsign", "false"});
}

void helper(const std::filesystem::path& path, std::string_view body) {
    std::ofstream{path} << "#!/usr/bin/env python3\n" << body;
    std::filesystem::permissions(path, std::filesystem::perms::owner_all);
}

struct Reset {
    ~Reset() { ext::configure({}); }
};
}  // namespace

TEST_CASE("extensions: bounded process lifetime, cancellation and descendant stdout") {
    TempDir temporary;
    const CancelToken token;
    auto result = ext::run({"python3", "-c", "import sys; print(sys.stdin.read())"}, temporary.path,
                           "literal $();\ninput", token);
    REQUIRE(result.code == 0);
    CHECK(result.output == "literal $();\ninput\n");
    CHECK_FALSE(ext::run({"python3", "-c", "print('x'*2000)"}, temporary.path, {}, token, 1s, 100)
                    .error.empty());
    const auto start = std::chrono::steady_clock::now();
    result = ext::run({"python3", "-c",
                       "import os,time; p=os.fork(); print(p,flush=True) if p else time.sleep(20)"},
                      temporary.path, {}, token, 120ms);
    CHECK(result.error == "Extension timed out");
    REQUIRE_FALSE(result.output.empty());
    const auto child = std::stoi(result.output);
    REQUIRE(child > 0);
    // The orphan may briefly remain a zombie under the test container's init, but
    // must no longer execute or hold stdout after the group's deadline.
    bool terminated = false;
    for (unsigned attempt = 0; attempt < 50; ++attempt) {
        std::ifstream state{"/proc/" + std::to_string(child) + "/stat"};
        std::string process_id, name;
        char activity = 'Z';
        if (!(state >> process_id >> name >> activity) || activity == 'Z') {
            terminated = true;
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    CHECK(terminated);
    CHECK(std::chrono::steady_clock::now() - start < 2s);
    const CancelToken cancelled;
    std::jthread cancel{[cancelled] {
        std::this_thread::sleep_for(80ms);
        cancelled.cancel();
    }};
    result =
        ext::run({"python3", "-c", "import time;time.sleep(20)"}, temporary.path, {}, cancelled);
    CHECK(result.error == "Extension cancelled");
    CHECK_FALSE(ext::run({"rummage-does-not-exist"}, temporary.path, {}, token).error.empty());
}

TEST_CASE("extensions: opt-in manifests, protocol compatibility and request boundaries") {
    TempDir temporary;
    const Reset reset;
    Environment data{"XDG_DATA_HOME", temporary.path.string()};
    const auto root = temporary.path / "rummage/extensions";
    const auto folder = root / "example";
    std::filesystem::create_directories(folder);
    std::ofstream{folder / "extension.toml"} << R"([extension]
api=1
id="example"
label="Example"
program="helper"
status=true
[[command]]
id="count"
label="Count"
selection="many"
)";
    helper(folder / "helper", R"(import json,sys
r=json.load(sys.stdin)
assert r['version']==1
if r['operation']=='command':
 print(json.dumps({'version':1,'message':str(len(r['files']))+' selected'}))
else:
 print(json.dumps({'version':1,'decorations':{p:{'badge':'T','label':'Test status','severity':1} for p in r['files']}}))
)");
    CHECK(ext::discover({}, {root})->extensions.empty());
    rummage::core::Config::Extensions config;
    config.enabled = {"example"};
    const auto discovered = ext::discover(config, {root});
    REQUIRE(discovered->problems.empty());
    REQUIRE(discovered->extensions.size() == 1);
    const auto& extension = discovered->extensions.front();
    REQUIRE(extension.commands.size() == 1);
    CHECK(extension.commands.front().selection == rummage::core::Action::Selection::many);
    const std::string odd = std::string{"a\n$();' ü.todo"} + char(0xff);
    temporary.write(odd, "hello");
    rummage::core::ActionContext context{.directory = temporary.path,
                                         .targets = {{temporary.path / odd, "text/plain"}}};
    CHECK(ext::invoke(extension, extension.commands.front(), context, CancelToken{}).message ==
          "1 selected");
    ext::configure(config);
    auto status = ext::status(temporary.path, {odd}, CancelToken{});
    REQUIRE(status.problem.empty());
    REQUIRE(status.entries.contains(odd));
    CHECK(status.entries.at(odd)->label == "Test status");
    helper(folder / "helper", R"(import json,sys
r=json.load(sys.stdin)
print(json.dumps({'version':1,'decorations':{r['files'][0]:{'badge':'T','label':'Test'},'file:///outside':{'badge':'T','label':'Bad'}}}))
)");
    status = ext::status(temporary.path, {odd}, CancelToken{});
    CHECK_FALSE(status.problem.empty());
    CHECK(status.entries.empty());
    helper(folder / "helper", "print('{\"version\":2,\"message\":\"wrong\"}')\n");
    CHECK(ext::invoke(extension, extension.commands.front(), context, CancelToken{}).failed);
    for (const auto& reply : std::vector<std::string>{
             "{", "log before JSON\n{\"version\":1}", R"({"version":1,"message":123})",
             std::string(40, '[') + "0" + std::string(40, ']')}) {
        helper(folder / "helper",
               "import sys\nsys.stdout.write(" + nlohmann::json(reply).dump() + ")\n");
        CHECK(ext::invoke(extension, extension.commands.front(), context, CancelToken{}).failed);
    }
    std::ofstream{folder / "extension.toml"} << "[extension]\napi=2\nid='example'\n";
    ext::reload();
    CHECK(ext::registry()->extensions.empty());
    CHECK_FALSE(ext::registry()->problems.empty());
    ext::configure({});
    CHECK(ext::status(temporary.path, {odd}, CancelToken{}).entries.empty());
}

TEST_CASE("extensions: Git states, unusual filenames, ignored policy and external index updates") {
    TempDir temporary;
    init(temporary.path);
    for (const auto* name : {"modified", "staged", "both", "rename", "conflict"})
        temporary.write(name, "base\n");
    const std::string odd = std::string{"line\nü $();' .txt"} + char(0xff);
    temporary.write(odd, "base\n");
    temporary.write(".gitignore", "ignored\n");
    git(temporary.path, {"add", "--all"});
    git(temporary.path, {"commit", "-qm", "initial"});
    temporary.write("modified", "change\n");
    temporary.write("staged", "change\n");
    temporary.write("both", "stage\n");
    temporary.write(odd, "change\n");
    git(temporary.path, {"add", "--", "staged", "both"});
    temporary.write("both", "more\n");
    temporary.write("untracked", "new\n");
    temporary.write("ignored", "new\n");
    git(temporary.path, {"mv", "rename", "renamed\nfile"});
    auto status = ext::git_status(temporary.path, CancelToken{}, false);
    REQUIRE(status.problem.empty());
    CHECK(status.repository == temporary.path);
    for (const auto& [name, badge] :
         std::vector<std::pair<std::string, std::string>>{{"modified", "M"},
                                                          {"staged", "S"},
                                                          {"both", "SM"},
                                                          {"untracked", "?"},
                                                          {"ignored", "I"},
                                                          {odd, "M"},
                                                          {"renamed\nfile", "S"}}) {
        REQUIRE(status.entries.contains(name));
        CHECK(status.entries.at(name)->badge == badge);
    }
    rummage::core::ActionContext context{.directory = temporary.path,
                                         .targets = {{temporary.path / odd, "text/plain"}}};
    const auto diff = ext::git_diff(context, false, CancelToken{});
    CHECK(diff.code == 0);
    CHECK(diff.output.find("+change") != std::string::npos);
    git(temporary.path, {"add", "--", "modified"});
    std::this_thread::sleep_for(2100ms);
    status = ext::git_status(temporary.path, CancelToken{});
    CHECK(status.entries.at("modified")->badge == "S");
    TempDir aliases;
    std::filesystem::create_directory_symlink(temporary.path, aliases.path / "alias");
    status = ext::git_status(aliases.path / "alias", CancelToken{}, false);
    CHECK(status.entries.at(odd)->badge == "M");
    TempDir wrong;
    init(wrong.path);
    Environment dir{"GIT_DIR", (wrong.path / ".git").string()};
    Environment tree{"GIT_WORK_TREE", wrong.path.string()};
    Environment index{"GIT_INDEX_FILE", (wrong.path / "missing-index").string()};
    status = ext::git_status(temporary.path, CancelToken{}, false);
    CHECK(status.repository == temporary.path);
    CHECK(status.entries.at(odd)->badge == "M");
    CHECK_FALSE(std::filesystem::exists(wrong.path / "missing-index"));
}

TEST_CASE("extensions: Git nested repositories, worktrees, conflict and no repository") {
    TempDir temporary;
    CHECK(ext::git_status(temporary.path, CancelToken{}, false).repository.empty());
    init(temporary.path);
    temporary.write("conflict", "base\n");
    git(temporary.path, {"add", "."});
    git(temporary.path, {"commit", "-qm", "initial"});
    git(temporary.path, {"checkout", "-qb", "side"});
    temporary.write("conflict", "side\n");
    git(temporary.path, {"commit", "-qam", "side"});
    git(temporary.path, {"checkout", "-q", "main"});
    temporary.write("conflict", "main\n");
    git(temporary.path, {"commit", "-qam", "main"});
    const auto merge = ext::run({"git", "merge", "side"}, temporary.path, {}, CancelToken{});
    REQUIRE(merge.code != 0);
    auto status = ext::git_status(temporary.path, CancelToken{}, false);
    REQUIRE(status.entries.contains("conflict"));
    CHECK(status.entries.at("conflict")->badge == "!");
    CHECK(status.entries.at("conflict")->severity == 3);
    const auto nested = temporary.path / "nested";
    std::filesystem::create_directory(nested);
    init(nested);
    std::ofstream{nested / "new"} << "new";
    status = ext::git_status(nested, CancelToken{}, false);
    CHECK(status.repository == nested);
    CHECK(status.entries.at("new")->badge == "?");
    const auto loose = temporary.path / "loose";
    std::filesystem::create_directory(loose);
    std::ofstream{loose / "new"} << "new";
    status = ext::git_status(loose, CancelToken{}, false);
    REQUIRE(status.directory_decoration);
    CHECK(status.directory_decoration->badge == "?");
    TempDir worktree;
    const auto worktree_path = worktree.path / "linked\nworktree";
    git(temporary.path, {"worktree", "add", "--detach", worktree_path.string(), "HEAD"});
    std::ofstream{worktree_path / "conflict"} << "worktree change\n";
    status = ext::git_status(worktree_path, CancelToken{}, false);
    CHECK(status.repository == worktree_path);
    CHECK(status.entries.at("conflict")->badge == "M");
}

TEST_CASE("extensions: unavailable Git fails without decorating unrelated files") {
    TempDir temporary;
    Environment path{"PATH", temporary.path.string()};
    const auto status = ext::git_status(temporary.path, CancelToken{}, false);
    CHECK_FALSE(status.problem.empty());
    CHECK(status.entries.empty());
}
