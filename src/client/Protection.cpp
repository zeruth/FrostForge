// The anti-tamper layer's own routines that game code calls (tools/aegis.py). FrostLite runs
// without the protector, so each does what the reference's does when the protection is satisfied
// and nothing else: no key schedule, no code hashing, no timing traps. DIVERGED, by design
// (docs/stubs.md, "Diverged on purpose"). The layer's frame tick is Sub14021C040 in
// StartupClient.cpp.
#include <cstdint>

// The key stub the layer decodes its masked values through, its body still encrypted in the clean
// exe ("bad instruction data"). Game code encodes and decodes a masked value through the same key
// path, so leaving the key as it is keeps the two consistent.
// ref: FUN_1400b1420
void AegisDecodeKey(uint64_t key) {
    (void)key;
}

// The layer's code integrity hash: crc32 over code pages, compared under its spinlock. Its callers
// take false for a failed check (FUN_14029fc00 then wipes the state it was handed and returns -1),
// so the check passes.
// ref: FUN_14029fc80
bool AegisCheckCodeHash() {
    return true;
}

// The layer's timing check under its spinlock (rdtsc against KUSER_SHARED_DATA), reached only through
// the pointer a guard in FUN_1401a9730 takes. Returns nothing.
// ref: FUN_1402a5bc0
void AegisCheckTiming(uint64_t unused, uint32_t a2, uint64_t a3) {
    (void)unused;
    (void)a2;
    (void)a3;
}

// The Lua taint shadow: the reference keeps an obfuscated copy of the current taint in an 8-byte
// block a masked pointer at lua_State +0x18 leads to, and its taint code keeps the copy in step with
// lua_taint. The port's Lua has no shadow (vendor/lua-5.1.4/src/lblizzard.c), so the slot holds no
// masked pointer, and unmasking it gave a wild one: LoadAddOn faulted in the setter
// (Blizzard_FrameEffects and Blizzard_StoreUI never loaded, and CharacterSelect.lua hit nil
// globals, 2026-10-20). The setter writes nothing; the check passes.
// ref: FUN_1402bd260
void* AegisTaintShadowSet(void* L) {
    (void)L;
    return nullptr;
}

// Compares the shadow with lua_taint, and reports a mismatch (FUN_14241f9a0, code 0x322).
// ref: FUN_1402bd150
void AegisTaintShadowCheck(void* L, uint32_t site) {
    (void)L;
    (void)site;
}
