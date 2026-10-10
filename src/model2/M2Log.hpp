#ifndef MODEL2_M2_LOG_HPP
#define MODEL2_M2_LOG_HPP

// The Model2 log (Logs\Model.log, DAT_1430a12c8) and the last model error
// (FUN_1411b7cc0, FUN_1411b7d70).

#include "storm/Log.hpp"

extern HSLOG g_m2Log;

// Where the next model error was raised (FUN_1411b7d70).
void M2SetErrorLocation(const char* file, int32_t line);
// The first model error, kept as text (FUN_1411b7cc0).
void M2SetError(const char* format, ...);
// The kept error (DAT_1430a1030).
const char* M2GetError();

#endif
