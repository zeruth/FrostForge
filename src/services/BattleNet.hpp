#ifndef SERVICES_BATTLE_NET_HPP
#define SERVICES_BATTLE_NET_HPP

#include <cstdint>

// No disconnect is deferred: the login may go ahead.
bool BattleNetIsReady();

#endif
