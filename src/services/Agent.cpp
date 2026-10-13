#include "services/Agent.hpp"
#include "fileservices/AsyncFile.hpp"
#include "ui/FrameTime.hpp"
#include "storm/Log.hpp"
#include "storm/Memory.hpp"
#include "util/Unimplemented.hpp"

namespace {

void* s_agent;                          // DAT_1430d2468: the communicator
HSLOG s_agentLog;                       // DAT_1430d24b0: Logs\agent.log
AGENTINSTALLCONFIG* s_installConfig;    // DAT_1430d24b8
uint32_t s_programCode;                 // DAT_1430d2538 ('WoW')
uint32_t s_textLocaleCode;              // DAT_1430d253c
uint32_t s_audioLocaleCode;             // DAT_1430d2540
void (*s_callback)(void* param);        // DAT_1430d2548

// Queues a command for the communicator's thread (with an argument for some commands).
// Offline: only reachable with a communicator.
// ref: FUN_141564430
bool AgentSend(void* agent, uint32_t command, uint32_t argument = 0) {
    (void)agent;
    (void)command;
    (void)argument;
    FROST_UNIMPLEMENTED();
    return false;
}

bool AgentNotify(uint32_t command, uint32_t value, uint32_t limit) {
    if (value < limit && s_agent) {
        if (*reinterpret_cast<int32_t*>(static_cast<uint8_t*>(s_agent) + 0x290) != 0) {
            AgentSend(s_agent, 9);
        }
        AgentSend(s_agent, command, value);
        return true;
    }
    return false;
}

}

// Reads the install config (product uid, expansion level, data and update directories) from
// .agent.db. Offline: not ported (docs/stubs.md), so there is no install config.
// ref: FUN_1415644d0
void AgentReadInstallConfig() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141563810
AGENTINSTALLCONFIG* AgentGetInstallConfig() {
    return s_installConfig;
}

// ref: FUN_141563730
void AgentFreeInstallConfig() {
    if (AGENTINSTALLCONFIG* config = s_installConfig) {
        config->~AGENTINSTALLCONFIG();
        ::operator delete(config, sizeof(AGENTINSTALLCONFIG));
    }
    s_installConfig = nullptr;
}

// Opens Logs\agent.log and starts the communicator for a product uid. Offline: not ported
// (docs/stubs.md), so there is no communicator.
// ref: FUN_141563eb0
void AgentInitialize(const char* uid) {
    (void)uid;
    FROST_UNIMPLEMENTED();
}

extern "C" unsigned long long FUN_141563170(unsigned long long);
// translated: FUN_141563170
// Once a frame: the communicator's connection is polled, and the Agent is told when the game
// goes idle or busy (commands 2, 8, 9), kept alive while busy (7, every 5 seconds), and told
// when file reads stall and recover (3, then 4 at least 10 seconds later). Offline there is no
// communicator, and it returns at once.
// ref: FUN_1415642a0
bool AgentUpdate() {
    auto agent = static_cast<uint8_t*>(s_agent);
    if (agent == nullptr) {
        return false;
    }
    auto field = [agent](size_t offset) -> int32_t& { return *reinterpret_cast<int32_t*>(agent + offset); };
    uint32_t now = FrameTime_GetTime();
    if (FUN_141563170(reinterpret_cast<uintptr_t>(agent)) == 0 && *reinterpret_cast<void**>(agent + 0xa08) != nullptr
        && field(0x28c) == 0 && field(0x290) == 0) {
        AgentSend(agent, 2, 0);
    }
    if (field(0x28c) != 0) {
        if (field(0x290) == 0) {
            AgentSend(agent, 8, 0);
            field(0x290) = 1;
            field(0x2a0) = static_cast<int32_t>(now + 5000);
        }
    } else {
        if (field(0x290) == 0) {
            return true;
        }
        AgentSend(agent, 9, 0);
        field(0x290) = 0;
    }
    if (field(0x290) == 0 || *reinterpret_cast<void**>(agent + 0xa08) == nullptr) {
        return true;
    }
    if (static_cast<int32_t>(now - static_cast<uint32_t>(field(0x2a0))) >= 0) {
        AgentSend(agent, 7, 0);
        field(0x2a0) = static_cast<int32_t>(now + 5000);
    }
    if (AsyncGetStalled() != 0) {
        if (field(0x288) == 0) {
            AgentSend(agent, 3, 0);
            field(0x288) = 1;
            field(0x2a4) = static_cast<int32_t>(now + 10000);
        }
    } else if (field(0x288) != 0 && static_cast<int32_t>(now - static_cast<uint32_t>(field(0x2a4))) >= 0) {
        AgentSend(agent, 4, 0);
        field(0x288) = 0;
    }
    if (field(0xa44) == 0x3f1) {
        field(0x28c) = 0;
    }
    return true;
}

// ref: FUN_1415649d0
bool AgentStart() {
    if (s_agent) {
        return AgentSend(s_agent, 0x10);
    }
    return false;
}

// Stops the communicator (waiting up to a second for it to finish) and closes the log.
// ref: FUN_141565530
void AgentShutdown() {
    if (s_agent) {
        // the stop handshake, connection teardown and delete; unreachable offline
        FROST_UNIMPLEMENTED();
    }
    SLogClose(s_agentLog);
    s_agentLog = nullptr;
}

// ref: FUN_141565470
bool AgentSetTextLocale(uint32_t locale) {
    return AgentNotify(0xC, locale, 0xC);
}

// ref: FUN_141565410
bool AgentSetAudioLocale(uint32_t locale) {
    return AgentNotify(0xF, locale, 0xC);
}

// ref: FUN_1415654d0
bool AgentSetRegion(uint32_t region) {
    return AgentNotify(0xE, region, 0x100);
}

// ref: FUN_141563110
bool AgentSetGameLocale(uint32_t locale) {
    return AgentNotify(0xA, locale, 0xC);
}

// ref: FUN_141567a20
void AgentSetTextLocaleCode(uint32_t code) {
    s_textLocaleCode = code;
}

// ref: FUN_1415679f0
void AgentSetAudioLocaleCode(uint32_t code) {
    s_audioLocaleCode = code;
}

// ref: FUN_141567a10
void AgentSetProgramCode(uint32_t code) {
    s_programCode = code;
}

// ref: FUN_141567a00
void AgentSetCallback(void (*callback)(void* param)) {
    s_callback = callback;
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_1430d2538 = (void*)&s_programCode;
extern "C" void* const gen_p_1430d253c = (void*)&s_textLocaleCode;
extern "C" void* const gen_p_1430d2540 = (void*)&s_audioLocaleCode;
extern "C" void* const gen_p_1430d2548 = (void*)&s_callback;
// ---- end of generated globals ----
