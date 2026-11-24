#pragma once

#include <cstdlib>
#include <optional>
#include <string>
#include <utility>

namespace rummage::test {

// Restore the process environment after a test that exercises XDG discovery.
class ScopedEnv {
public:
    ScopedEnv(std::string name, const std::string& value) : name_{std::move(name)} {
        if (const char* previous = std::getenv(name_.c_str()))
            previous_ = previous;
        setenv(name_.c_str(), value.c_str(), 1);
    }

    ~ScopedEnv() {
        if (previous_)
            setenv(name_.c_str(), previous_->c_str(), 1);
        else
            unsetenv(name_.c_str());
    }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    std::string name_;
    std::optional<std::string> previous_;
};

}  // namespace rummage::test
