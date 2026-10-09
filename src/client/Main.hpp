#ifndef CLIENT_MAIN_HPP
#define CLIENT_MAIN_HPP

#include <cstdint>

[[noreturn]] void ClientMain(int32_t argc, char** argv);

[[noreturn]] void ClientRun(int32_t argc, char** argv);

void CommonMain();

#endif
