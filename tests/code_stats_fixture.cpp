#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <thread>

// Deterministic subprocess fixture: all behavior is selected by files in its working directory.
int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    if (argc < 5 || std::string_view{argv[1]} != "--output" ||
        std::string_view{argv[2]} != "json" || std::string_view{argv[argc - 2]} != "--" ||
        std::string_view{argv[argc - 1]} != ".")
        return 2;
    bool hidden = false, shallow = false;
    for (int i = 3; i < argc - 2; ++i) {
        if (std::string_view{argv[i]} == "--hidden")
            hidden = true;
        else if (std::string_view{argv[i]} == "--exclude" && i + 1 < argc - 2 &&
                 std::string_view{argv[++i]} == "*/*")
            shallow = true;
        else
            return 3;
    }
    if (fs::exists("hidden") != hidden || fs::exists("shallow") != shallow)
        return 4;
    if (fs::exists("sleep"))
        std::this_thread::sleep_for(std::chrono::seconds{20});
    if (fs::exists("fail"))
        return 5;
    if (fs::exists("oversize")) {
        const std::string block(4096, 'x');
        for (int i = 0; i < 4097; ++i)
            std::cout << block;
    } else {
        std::ifstream report{"report.json"};
        if (!report)
            return 6;
        std::cout << report.rdbuf();
    }
    return 0;
}
