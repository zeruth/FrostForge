#ifndef DB_DB_CLIENT_HPP
#define DB_DB_CLIENT_HPP

// WoW/Source/DB/DBClient: the client's tables, loaded in three steps. ClientDBInitialize loads
// the first two lists (the glue's tables) at startup and prefetches the rest; the next two
// load when the glue first asks for character data, the last two on entering the world.

#include <cstdint>

class WowClientDB2_Base;

constexpr int32_t DB2_LOAD_GROUP_COUNT = 6;

// FUN_14057d1b0
bool WowClientDB2LoadWithFlags(WowClientDB2_Base* table, uint32_t flags);

// FUN_140351bf0
void ClientDBInitialize();
// FUN_140351d40
void ClientDBLoadGroup1();
// FUN_140351db0
void ClientDBLoadGroup2();
// FUN_140351f70
void ClientDBReload();
// FUN_140352060
void ClientDBShutdown();

// DAT_142e30491
bool ClientDBGroup1Loaded();

#endif
