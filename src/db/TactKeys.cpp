#include "db/TactKeys.hpp"

#include <cstring>

#include "blz/ChainedHash.hpp"
#include "cas/CAS.hpp"
#include "db/DB2Tables.hpp"
#include "db/WowClientDB2.hpp"
#include "db/WowDB2Lookup.hpp"
#include "util/Unimplemented.hpp"

namespace {

const char s_payloadNodeTag[] = ".?AU?$chained_hash_node@U?$pair@$$CBVTactID@@UEncryptionPayload@@@blz@@@blz@@";
const char s_payloadBucketTag[] = ".PEAU?$chained_hash_node@U?$pair@$$CBVTactID@@UEncryptionPayload@@@blz@@@blz@@";
const char s_knownNodeTag[] = ".?AU?$chained_hash_node@VTactID@@@blz@@";
const char s_knownBucketTag[] = ".PEAU?$chained_hash_node@VTactID@@@blz@@";

// TactID -> EncryptionPayload (DAT_142cfdc00). The reference buckets by the low bits of the
// TactID itself; the shared blz helpers mix them first, which only changes the bucket order.
struct PAYLOADNODE {
    PAYLOADNODE* m_next;
    uint64_t m_key;
    ENCRYPTIONPAYLOAD m_payload;
};
blz::chained_hash_header s_payloads;

// The TactIDs whose keys have arrived (DAT_1430ab6f8, FUN_14135ae10 inserts).
struct KNOWNNODE {
    KNOWNNODE* m_next;
    uint64_t m_key;
};
blz::chained_hash_header s_known;

// Reads waiting for keys, by request (DAT_142cfdc40). Only the async missing-key hook
// (FUN_141358160, installed through FUN_1405d8e10) adds to it, and that hook is not ported, so
// it stays empty.
blz::chained_hash_header s_waitingReads;

void (*s_handler)(int32_t);     // DAT_1430ab768
bool s_initialized;             // DAT_1430ab6f5

// The closure the record callbacks carry (the reference's blz::function storage).
struct RECORDREQUEST {
    void (*m_callback)(uint32_t id, void* record, int32_t status, uint64_t keyName);
    uint64_t m_keyName;
};

// A record now, or the callback when it comes: 0 the record was there (the callback has run
// with it), 1 the table has no such record (run with null), 2 asked for as a hotfix.
// ref: FUN_141358a40
int32_t GetRecordWithCallback(WowClientDB2_Base* table, uint32_t id, const RECORDREQUEST* request) {
    bool local = false;
    void* record = table->GetRecord(id, true, &local);
    if (!local) {
        FROST_UNIMPLEMENTED();  // the pending hotfix request list at +0x1f0 (FUN_141357630)
        return 2;
    }
    if (record == nullptr) {
        request->m_callback(id, nullptr, 1, request->m_keyName);
        return 1;
    }
    request->m_callback(id, record, 0, request->m_keyName);
    return 0;
}

// A key's TactKey record arrived (status 0), was not there (1), or could not be had (3).
// ref: FUN_1413593d0
void KeyArrived(uint64_t keyName, uint32_t id, const void* record, int32_t status) {
    (void)id;
    PAYLOADNODE* node = blz::chained_find<PAYLOADNODE>(&s_payloads, keyName);
    if (node == nullptr || node->m_payload.m_state == ENCRYPTIONPAYLOAD::KNOWN) {
        return;
    }
    uint32_t previous = node->m_payload.m_state;
    if (status != 0) {
        if (status == 1 || status == 3) {
            node->m_payload.m_state = ENCRYPTIONPAYLOAD::FAILED;
        }
        return;
    }
    node->m_payload.m_state = ENCRYPTIONPAYLOAD::KNOWN;
    std::memcpy(node->m_payload.m_key, record, 16);
    bool inserted = false;
    blz::chained_insert<KNOWNNODE>(&s_known, keyName, &inserted, s_knownNodeTag, s_knownBucketTag);
    if (previous == ENCRYPTIONPAYLOAD::PENDING && s_waitingReads.m_size != 0) {
        FROST_UNIMPLEMENTED();  // requeueing the reads that waited for this key (DAT_1430ab778)
    }
}

// ref: FUN_14135acd0
void RecordCallback(uint32_t id, void* record, int32_t status, uint64_t keyName) {
    if (status == 2 && s_handler != nullptr) {
        s_handler(static_cast<int32_t>(id));
    }
    KeyArrived(keyName, id, record, status);
}

// Names key `keyName` as TactKey row `id`, and asks for the key unless it is already known or
// asked for.
// ref: FUN_141359c30
void RegisterKey(uint32_t id, uint64_t keyName) {
    ENCRYPTIONPAYLOAD payload = {};
    payload.m_keyId = id;
    if (PAYLOADNODE* node = blz::chained_find<PAYLOADNODE>(&s_payloads, keyName)) {
        payload = node->m_payload;
        if (payload.m_state - 1u < 2) {
            return;
        }
    }
    bool inserted = false;
    PAYLOADNODE* node = blz::chained_insert<PAYLOADNODE>(&s_payloads, keyName, &inserted, s_payloadNodeTag,
        s_payloadBucketTag);
    if (!g_tactKeyDB.HasRecord(static_cast<int32_t>(id)) && !DB2HotfixRequestsAvailable()) {
        node->m_payload = payload;
        return;
    }
    payload.m_keyId = id;
    payload.m_state = ENCRYPTIONPAYLOAD::PENDING;
    node->m_payload = payload;
    RECORDREQUEST request = { RecordCallback, keyName };
    GetRecordWithCallback(&g_tactKeyDB, id, &request);
}

// A TactKey record was patched: its key, under the name TactKeyLookup gives the row.
// ref: FUN_141358e80
void TactKeyPatched(uint32_t table, uint32_t id, uint32_t a3, void* a4, void* record) {
    (void)table;
    (void)a3;
    (void)a4;
    bool local = false;
    auto lookup = static_cast<const uint64_t*>(g_tactKeyLookupDB.GetRecord(id, false, &local));
    if (lookup != nullptr) {
        KeyArrived(*lookup, id, record, 0);
    }
}

// Both tables loaded: fill the store.
// ref: LAB_1413593b0
void TablesLoaded(int32_t event) {
    if (event == 0 && g_tactKeyDB.m_loaded && g_tactKeyLookupDB.m_loaded) {
        TactKeysInitialize();
    }
}

}

// ref: FUN_14135aa50
void TactKeysSetup(void (*handler)(int32_t)) {
    s_handler = handler;
    // FUN_14057d190(LAB_14135aac0, FUN_1413596a0, FUN_141359a10): the DB2 encrypted-section
    // hooks (DAT_142e8e2a8..2b8)
    FROST_UNIMPLEMENTED();
    if (g_tactKeyDB.m_loaded && g_tactKeyLookupDB.m_loaded) {
        TactKeysInitialize();
        return;
    }
    g_tactKeyDB.AddLoadCallback(TablesLoaded);
    g_tactKeyLookupDB.AddLoadCallback(TablesLoaded);
}

// ref: FUN_141359ff0
void TactKeysInitialize() {
    if (s_initialized) {
        return;
    }
    if (WowDB2Lookup* lookup = g_tactKeyLookupDB.m_lookup) {
        DB2LOOKUPITER it;
        WowDB2LookupBegin(lookup, &it);
        while (it.m_index != -2) {
            uint64_t keyName = *static_cast<const uint64_t*>(it.m_record);
            RegisterKey(lookup->Id(&it), keyName);
            lookup->Next(&it);
        }
    }
    // DAT_1430ab778 = FUN_1405d8e10(FUN_141358160), DAT_1430ab780 = FUN_1405db9d0(FUN_141357f10):
    // the async reads' missing-key hooks
    FROST_UNIMPLEMENTED();
    cas::CASSetKeyCallback(TactKeysLookup);
    g_tactKeyDB.AddPatchCallback(TactKeyPatched, nullptr);
    // FUN_1405791e0(TactKeyLookup, LAB_141358ed0), FUN_1413e4d40, FUN_1413a5ca0 and the
    // FUN_140579390 handler for 0xdf2f53cf: the lookup watcher and the hotfix-side hooks
    FROST_UNIMPLEMENTED();
    s_initialized = true;
}

// ref: FUN_14135a840
void TactKeysShutdown() {
    if (!s_initialized) {
        return;
    }
    cas::CASSetKeyCallback(nullptr);
    g_tactKeyDB.RemovePatchCallback(TactKeyPatched);
    blz::chained_destroy<KNOWNNODE>(&s_known, s_knownNodeTag, s_knownBucketTag);
    s_known = blz::chained_hash_header();
    s_initialized = false;
}

void TactKeysProvide(uint64_t keyName, const uint8_t key[16]) {
    bool inserted = false;
    PAYLOADNODE* node = blz::chained_insert<PAYLOADNODE>(&s_payloads, keyName, &inserted, s_payloadNodeTag,
        s_payloadBucketTag);
    if (node->m_payload.m_state == ENCRYPTIONPAYLOAD::KNOWN) {
        return;
    }
    KeyArrived(keyName, node->m_payload.m_keyId, key, 0);
}

// ref: FUN_14135a180
int64_t TactKeysLookup(uint64_t* keyName, void* key, uint32_t keySize) {
    PAYLOADNODE* node = blz::chained_find<PAYLOADNODE>(&s_payloads, *keyName);
    if (node == nullptr) {
        return 0;
    }
    uint32_t state = node->m_payload.m_state;
    if (state == ENCRYPTIONPAYLOAD::KNOWN) {
        if (keySize == 0x10) {
            std::memcpy(key, node->m_payload.m_key, 16);
        }
        return 0x10;
    }
    if (state != ENCRYPTIONPAYLOAD::UNKNOWN && state != ENCRYPTIONPAYLOAD::FAILED) {
        return 0;
    }
    if (DB2HotfixRequestsAvailable()) {
        RECORDREQUEST request = { RecordCallback, *keyName };
        int32_t result = GetRecordWithCallback(&g_tactKeyDB, node->m_payload.m_keyId, &request);
        if (result == 0) {
            // The reference reports the size here without copying the key it just stored.
            return 0x10;
        }
        if (result == 2) {
            node->m_payload.m_state = ENCRYPTIONPAYLOAD::PENDING;
        }
    }
    return 0;
}

// ---- GENERATED by tools/forward_stubs.py: link fallbacks where src/gen is not linked ----
#pragma comment(linker, "/include:GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141357630=GenForwardMissing")
// ---- end of link fallbacks ----

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_1430ab768 = (void*)&s_handler;
// ---- end of generated globals ----
