#ifndef CONSOLE_COMMAND_HPP
#define CONSOLE_COMMAND_HPP

#include <cstdint>

typedef int32_t (*CONSOLECOMMANDHANDLER)(const char* command, const char* arguments);

bool ConsoleCommandRegister(const char* command, CONSOLECOMMANDHANDLER handler, uint32_t category, const char* help,
    uint64_t a5);

bool ConsoleCommandExists(const char* command);

// Removes and frees a command's entry; nothing happens if it is not registered.
void ConsoleCommandUnregister(const char* command);

void ConsoleWrite(uint32_t color, const char* text);

void ConsoleWriteText(const char* text);

// ConsoleWriteFormat that also passes the format to a hook.
void ConsoleWriteFormatNotify(uint32_t color, const char* format, ...);

// A line from the command history: 0 is the newest.
const char* ConsoleGetHistory(int32_t age);

// help, ver and quit.
void ConsoleRegisterHelpCommand();
void ConsoleRegisterVersionCommand();
void ConsoleRegisterBasicCommands();

// The client's quit request (client/Main.cpp), which the quit command calls.
void ClientRequestQuit();

// Executes console input, one command per line; true when a handler accepted a line.
bool ConsoleCommandExecute(const char* text, int32_t addToHistory);

void ConsoleWriteFormat(uint32_t color, const char* format, ...);

// The key that toggles the console (the ConsoleKey CVar), and its getter.
void ConsoleSetHotKey(int32_t key);
int32_t ConsoleGetHotKey();

#endif
