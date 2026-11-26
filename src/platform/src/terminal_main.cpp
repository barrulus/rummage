// Executed before any UI threads. Owns the controlling PTY then becomes the user's shell.
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc != 4 || argv[1][0] != '/' || argv[2][0] != '/' || argv[3][0] != '/')
        return 2;
    if (::setsid() < 0 || ::ioctl(0, TIOCSCTTY, 0) < 0 || ::chdir(argv[1]) < 0)
        return 3;
    ::setenv("TERM", "xterm-256color", 1);
    ::setenv("COLORTERM", "truecolor", 1);
    const char* name = std::strrchr(argv[2], '/');
    name = name ? name + 1 : argv[2];
    if (std::strcmp(name, "bash") == 0) {
        const auto rc = std::string{argv[3]} + "/bashrc";
        ::execl(argv[2], argv[2], "--rcfile", rc.c_str(), "-i", nullptr);
    } else if (std::strcmp(name, "zsh") == 0) {
        const char* original = std::getenv("ZDOTDIR");
        ::setenv("RUMMAGE_ORIGINAL_ZDOTDIR",
                 original              ? original
                 : std::getenv("HOME") ? std::getenv("HOME")
                                       : "/",
                 1);
        ::setenv("ZDOTDIR", argv[3], 1);
        ::execl(argv[2], argv[2], "-i", nullptr);
    } else
        ::execl(argv[2], argv[2], "-i", nullptr);
    return 127;
}
