#include "db/DynamicLifeTimeDB.hpp"
#include "cas/CAS.hpp"
#include "db/DBClient.hpp"
#include "db/WowClientDB2.hpp"
#include "fileservices/AsyncFile.hpp"
#include "util/Unimplemented.hpp"

namespace {

// The file landed: load the table and tell the waiters. One copy per table in the reference
// (GlobalColor's is FUN_141672ec0).
// ref: FUN_141970210
void RequestDone(void* userData, void* buffer, uint32_t size) {
    (void)buffer;
    (void)size;
    auto db = static_cast<DynamicLifeTimeDB*>(userData);
    db->m_request = nullptr;
    if (!db->m_table->m_loaded) {
        WowClientDB2LoadWithFlags(db->m_table, 4);
    }
    for (uint64_t i = 0; i < db->m_waiterCount; i++) {
        db->m_waiters[i].m_callback(1, db->m_waiters[i].m_param);
    }
    db->m_waiterCount = 0;
}

// The file failed: tell the waiters.
// ref: FUN_141672e40
void RequestFailed(void* userData, void* buffer, uint32_t size, const char* error) {
    (void)buffer;
    (void)size;
    (void)error;
    auto db = static_cast<DynamicLifeTimeDB*>(userData);
    db->m_request = nullptr;
    for (uint64_t i = 0; i < db->m_waiterCount; i++) {
        db->m_waiters[i].m_callback(0, db->m_waiters[i].m_param);
    }
    db->m_waiterCount = 0;
}

}

// ref: FUN_140574300
void WowClientDB2ReloadWithHotfixes(WowClientDB2_Base* table) {
    FROST_UNIMPLEMENTED();  // FUN_14056f5d0 over FUN_1405752b0: the hotfix cache replayed for the table's hash
    WowClientDB2LoadWithFlags(table, 4);
}

void DynamicLifeTimeDB::Load() {
    int32_t fileDataId = this->m_table->m_meta->m_fileDataId;
    if (cas::CASFileGetResidency(fileDataId, 0, 0) == 2) {
        if (!this->m_loadedOnce) {
            WowClientDB2LoadWithFlags(this->m_table, 4);
        } else {
            WowClientDB2ReloadWithHotfixes(this->m_table);
        }
        this->m_loadedOnce = 1;
    } else if (this->m_request == nullptr) {
        this->m_request = AsyncFileReadById(fileDataId, 2, this, nullptr, nullptr, nullptr, RequestDone,
            RequestFailed, nullptr);
    }
}

void DynamicLifeTimeDB::AcquireIfUnloaded() {
    if (this->m_table->m_loaded) {
        return;
    }
    this->m_refs++;
    if (this->m_request == nullptr && !this->m_table->m_loaded) {
        this->Load();
    }
}

void DynamicLifeTimeDB::Acquire() {
    this->m_refs++;
    if (this->m_request == nullptr && !this->m_table->m_loaded) {
        this->Load();
    }
}

void DynamicLifeTimeDB::Wait() {
    if (this->m_request != nullptr) {
        AsyncFileWait(this->m_request);
    }
}

void DynamicLifeTimeDB::ReleaseIfLoaded() {
    if (!this->m_table->m_loaded) {
        return;
    }
    this->Release();
}

void DynamicLifeTimeDB::Release() {
    if (this->m_refs == 0) {
        return;
    }
    this->m_refs--;
    if (this->m_refs >= 1) {
        return;
    }
    this->m_waiterCount = 0;
    if (this->m_request != nullptr) {
        AsyncFileCancel(this->m_request);
    }
    this->m_request = nullptr;
    if (this->m_table != nullptr && this->m_table->m_loaded) {
        this->m_table->Unload(false);
    }
}
