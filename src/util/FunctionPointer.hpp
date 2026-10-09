#ifndef UTIL_FUNCTIONPOINTER_HPP
#define UTIL_FUNCTIONPOINTER_HPP

#include <cstdint>

// Raises a fatal error unless `function` lies inside this executable's .text section. Run on
// a CVar's callback target before every call.
void CheckFunctionPointer(uint64_t function);

#endif
