#include "model2/M2Model.hpp"

#include "model2/M2Shared.hpp"
#include "util/Unimplemented.hpp"

// ref: FUN_1411c62e0
void M2ModelLinkWaiting(CM2Model* model) {
    CM2Shared* shared = model->m_shared;
    model->m_waitPrev = shared->m_waitingTail;
    model->m_waitNext = nullptr;
    *shared->m_waitingTail = model;
    shared->m_waitingTail = &model->m_waitNext;
}

// ref: FUN_1411cdb30
void M2ModelUnlinkWaiting(CM2Model* model) {
    if (model->m_waitPrev == nullptr) {
        return;
    }
    *model->m_waitPrev = model->m_waitNext;
    if (model->m_waitNext != nullptr) {
        model->m_waitNext->m_waitPrev = model->m_waitPrev;
    } else {
        model->m_shared->m_waitingTail = model->m_waitPrev;
    }
    model->m_waitPrev = nullptr;
    model->m_waitNext = nullptr;
}

// ref: FUN_1411c24f0
bool M2ModelInitializeLoaded(CM2Model* model) {
    (void)model;
    FROST_UNIMPLEMENTED();
    return false;
}

// ref: FUN_1411c5d90
void M2ModelInitializePools() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1411b7d60
void M2ModelSetFlag400(bool set) {
    (void)set;
    FROST_UNIMPLEMENTED();
}

void M2CreateModuleScene() {
    FROST_UNIMPLEMENTED();  // FUN_141196e40: CM2Scene
}
