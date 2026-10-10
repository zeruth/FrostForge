// SE3Emitter (Engine/Source/SoundEngine/SE3Emitter.cpp): the world's sound emitters, kept in a
// grid around the listener. Emitters come from the map; nothing makes one before the world, so
// the manager is not ported yet.

#include "sound/SE3Kit.hpp"
#include "sound/SE3Internal.hpp"

#include "util/Unimplemented.hpp"

// ref: FUN_1412dd330
void SE3EmitterManagerInit(blz::function_storage* gridCell, int32_t param88, int32_t param8c) {
    (void)gridCell;
    (void)param88;
    (void)param8c;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1412dcfb0
void SE3EmitterUpdate(int32_t elapsed) {
    (void)elapsed;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1412deac0
void SE3EmitterManagerShutdown() {
    FROST_UNIMPLEMENTED();
}
