#include "client/Main.hpp"
#include <cstdlib>
#include <malloc.h>
#include <windows.h>

// The reference ignores its own arguments: it takes the CRT's wide argv, converts every
// argument to UTF-8 on the stack, and hands (argc, argv) to the platform-independent main.
// ref: FUN_1402611e0
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int32_t argc = __argc;
    wchar_t** wargv = __wargv;

    char** argv = static_cast<char**>(_alloca((argc + 1) * sizeof(char*)));
    for (int32_t i = 0; i < argc; i++) {
        int32_t size = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        argv[i] = static_cast<char*>(_alloca(size));
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, argv[i], size, nullptr, nullptr);
    }
    argv[argc] = nullptr;

    ClientMain(argc, argv);
}
