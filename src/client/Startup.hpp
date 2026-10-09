#ifndef CLIENT_STARTUP_HPP
#define CLIENT_STARTUP_HPP

#include <cstdint>

// The stages CommonMain runs, in order. None of these is identified beyond its place in
// that sequence yet, so they carry their reference address as their name.

// Startup progress, DAT_142ddcfa8: CommonMain sets a bit as each stage completes, and the
// shutdown path (0x140216880) reads it to know what to tear down.
enum STARTUP_FLAGS : uint32_t {
    STARTUP_PRE_INIT = 0x1,
    STARTUP_STAGE_1 = 0x8,
    STARTUP_STAGE_2 = 0x10,
    STARTUP_STAGE_3 = 0x20,
    STARTUP_STAGE_4 = 0x46,
};

extern uint32_t g_startupFlags;  // DAT_142ddcfa8
extern uint32_t g_startupValue;  // DAT_142ddcf4c: the clock when the main loop starts
extern uint8_t g_running;        // DAT_142ddcf98

void Sub140215370();
void Sub140217F20();
void Sub140217E00();
void Sub1402178D0();

extern uint32_t g_clientInitFlags;  // DAT_142ddd0d8
extern uint32_t g_clientTeardownFlags;  // DAT_142ddd0dc

struct CVar;
extern CVar* g_agentUid;     // DAT_142ddfa40
extern CVar* g_launchAgent;  // DAT_142ddfa48
extern CVar* g_overrideArchive;  // DAT_142ddfa78: "overrideArchive"

// Adds a listener for the initial hotfix events (3 applied, 4 failed).
void HotfixAddInitialListener(void (*listener)(int32_t event));

void StartupErrorCallback(uint32_t code, const char* text);
void Sub1405F64E0();
void Sub14165C0E0();
void Sub1415002E0(int32_t a1, int32_t a2, int32_t a3, int32_t a4, int32_t a5);
void Sub140226240();
void LogOpenDefaultLogs();

bool Sub1402262E0();
void Sub1402265E0();
// StartupService (client/StartupService.cpp).
bool StartupServiceInitialize();
bool Sub1402289B0();
void Sub1402295B0();
bool Sub14022A260();  // StartupClient.cpp
bool Sub1402225D0();
void Sub140225AB0();
void Sub140227810();
void Sub140226550();
void Sub140226260();
void Sub140216880();

#endif
