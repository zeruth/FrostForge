#ifndef MODEL2_M2_MODEL_HPP
#define MODEL2_M2_MODEL_HPP

// CM2Model (Engine/Source/Model2/M2Model.cpp, in a 0x690 ObjectAlloc slot): one instance of a
// shared model file, with its own animation state, bones and draw data. A model made before its
// file is in waits on the file's list and is initialized when the file is.

#include <cstdint>

struct CM2Shared;

class CM2Model {
    public:
    uint8_t m_000[0x3c8];
    CM2Model** m_waitPrev;          // 0x3c8 the link pointing here on the file's waiting list
    CM2Model* m_waitNext;           // 0x3d0
    CM2Shared* m_shared;            // 0x3d8
    // The last 4 bytes of the slot hold its ObjectAlloc index.
    uint8_t m_3e0[0x690 - 0x3e0];
};
static_assert(sizeof(CM2Model) == 0x690, "CM2Model fills its 0x690 slot");

// The file's waiting list (FUN_1411c62e0 joins at the tail, FUN_1411cdb30 leaves).
void M2ModelLinkWaiting(CM2Model* model);
void M2ModelUnlinkWaiting(CM2Model* model);
// The file is in: the model sets itself up from it (FUN_1411c24f0).
bool M2ModelInitializeLoaded(CM2Model* model);
// CM2Model's pools (FUN_1411c5d90).
void M2ModelInitializePools();
// FUN_1411b7d60
void M2ModelSetFlag400(bool set);
// The module's own scene (DAT_1430a0ed0, FUN_141196e40(9, ...)).
void M2CreateModuleScene();

#endif
