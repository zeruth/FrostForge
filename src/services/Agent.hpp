#ifndef SERVICES_AGENT_HPP
#define SERVICES_AGENT_HPP

#include "blz/String.hpp"
#include <cstdint>

// WoW/Source/WowServices/AgentCommunicator.cpp: the link to the Battle.net Agent process.
// Offline for now (docs/stubs.md): reading .agent.db and starting the communicator are
// stubs, so there is no install config and no communicator, and every notification below
// does nothing, as the reference's own null checks make it.

// The install config read from .agent.db (0xb0 bytes): its uid comes first.
struct AGENTINSTALLCONFIG {
    blz::string m_uid;          // 0x00
    uint8_t m_unknown28[0x10];  // 0x28
    blz::string m_dataDir;      // 0x38
    blz::string m_updateDir;    // 0x60
    blz::string m_product;      // 0x88
};
static_assert(sizeof(AGENTINSTALLCONFIG) == 0xB0, "AGENTINSTALLCONFIG is 0xb0 bytes");

void AgentReadInstallConfig();
AGENTINSTALLCONFIG* AgentGetInstallConfig();
void AgentFreeInstallConfig();

void AgentInitialize(const char* uid);
bool AgentStart();
bool AgentUpdate();
void AgentShutdown();

bool AgentSetTextLocale(uint32_t locale);
bool AgentSetAudioLocale(uint32_t locale);
bool AgentSetRegion(uint32_t region);
bool AgentSetGameLocale(uint32_t locale);

void AgentSetTextLocaleCode(uint32_t code);
void AgentSetAudioLocaleCode(uint32_t code);
void AgentSetProgramCode(uint32_t code);
void AgentSetCallback(void (*callback)(void* param));
uint32_t AgentGetAudioLocaleCode();
void AgentRunCallback(void* param);
uint32_t AgentGetProgramCode();
uint32_t AgentGetTextLocaleCode();

#endif
