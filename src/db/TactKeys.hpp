#ifndef DB_TACT_KEYS_HPP
#define DB_TACT_KEYS_HPP

// The client's store of content encryption keys. TactKeyLookup names each key (its 8-byte
// TactID by row id) and TactKey holds the key bytes (16 by the same row id), from the file or
// as hotfixes. The store maps TactID to the row and, once known, the key, and is what CASC's
// key callback asks when a block is encrypted.

#include <cstdint>

// What the store knows of a key (EncryptionPayload, 0x18).
struct ENCRYPTIONPAYLOAD {
    enum : uint32_t {
        UNKNOWN = 0,    // named, the key not asked for
        PENDING = 1,    // the TactKey record has been asked for
        KNOWN = 2,      // m_key holds the key
        FAILED = 3,     // the record could not be had
    };
    uint32_t m_keyId;           // 0x00 the TactKey / TactKeyLookup row
    uint32_t m_state;           // 0x04
    uint8_t m_key[16];          // 0x08
};

// Sets up the store: the handler hears of keys that arrive late, and the store fills once
// TactKey and TactKeyLookup are both loaded (now, or from their load callbacks).
// ref: FUN_14135aa50
void TactKeysSetup(void (*handler)(int32_t));
// Fills the store from TactKeyLookup and asks TactKey for every key, then hands CASC the key
// callback and watches TactKey for patched records.
// ref: FUN_141359ff0
void TactKeysInitialize();
// ref: FUN_14135a840
void TactKeysShutdown();

// A key known by name from outside the tables, stored as a hotfixed TactKey record would be.
// Not a reference function: the client only learns such keys from the server; tools use it to
// supply them from a key list.
void TactKeysProvide(uint64_t keyName, const uint8_t key[16]);

// CASC's key callback: copies the key named `keyName` and returns its size (16), or 0.
// ref: FUN_14135a180
int64_t TactKeysLookup(uint64_t* keyName, void* key, uint32_t keySize);

#endif
