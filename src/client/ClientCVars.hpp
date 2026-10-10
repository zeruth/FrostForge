#ifndef CLIENT_CLIENTCVARS_HPP
#define CLIENT_CLIENTCVARS_HPP

#include <cstdint>

struct CVar;

// The saved account name and account list (hidden from script, read by the glue directly).
extern CVar* g_accountNameCVar;     // DAT_142de29a0
extern CVar* g_accountListCVar;     // DAT_142de29b0

// The client's own CVars (FUN_14024c220), the full-screen effect switches among them.
void ClientRegisterCVars();
void FullScreenEffectRegisterCVars();

// The screenshot size override, {width, height} with 0 for the window's (FUN_14023a3f0).
void ClientSetScreenshotSize(const uint16_t* size);
// The hardware cursor on or off (FUN_1414fca60).
void ClientSetHardwareCursor(bool software);

#endif
