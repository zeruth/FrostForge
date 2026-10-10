#ifndef MODEL2_M2_INIT_HPP
#define MODEL2_M2_INIT_HPP

// M2Init (Engine/Source/Model2/M2Shared.cpp): turns the file offsets of an M2 read whole into
// pointers, checking each array against the end of the data. The same functions fix up an .anim
// file's keys for one sequence, given the sequence in the context.

#include <cstdint>

#include "model2/M2Data.hpp"

// What the fixups are told (0x48).
struct M2INITCONTEXT {
    // The sequence whose keys are fixed up, -1 for the model's own data.
    uint32_t m_sequence = 0xffffffff;       // 0x00
    // An .anim file's size; 0 when the keys stay offsets (moved by m_offset).
    uint32_t m_size = 0;                    // 0x04
    uint8_t m_pad08[0x10] = {};             // 0x08
    // An .anim file's data (low 32 bits: the offset taken from keys left as offsets).
    uint64_t m_base = 0;                    // 0x18
    uint8_t m_pad20[0x18] = {};             // 0x20
    // The sequences, whose flags say which keys are in this file; null to take all.
    const M2Array<M2Sequence>* m_sequences = nullptr;  // 0x38
    // The model's own arrays are fixed already: only keys are left.
    bool m_skipMain = false;                // 0x40
    // Every sequence's keys are in this file.
    bool m_allSequences = false;            // 0x41
    uint8_t m_pad42[6] = {};
};
static_assert(sizeof(M2INITCONTEXT) == 0x48, "M2INITCONTEXT is 0x48 bytes");

// The whole MD20 header and what hangs off it (FUN_1411ab3a0).
bool M2InitAll(void* base, uint32_t size, M2Data* data, M2INITCONTEXT* context);
// The parent's keys an .anim file or a chunk carries, as a parent track array (the PADC chunk).
bool M2InitKeys32(void* base, uint32_t size, M2Data* data, M2Array<M2Array<uint32_t>>* keys, M2INITCONTEXT* context);
// The EXP2 chunk (FUN_14119ce60).
bool M2InitExtendedParticles(void* base, uint32_t size, M2Data* data, M2Array<M2ExtendedParticle>* array,
    M2INITCONTEXT* context);
// An array a chunk starts with (PGD1, PABC, PSBC), checked against the chunk.
bool M2InitChunkArray(void* base, uint32_t size, void* field, uint32_t elemSize, const char* what);
// The PADC and PEDC chunks' tracks.
bool M2InitParentAnimData(void* base, uint32_t size, M2Data* data, M2Array<M2Track<uint16_t>>* array);
bool M2InitParentEventData(void* base, uint32_t size, M2Data* data, M2Array<M2TrackBase>* array);
// A skin profile (FUN_1411aba40).
bool M2InitSkinProfile(void* base, uint32_t size, M2Data* data, M2SkinProfile* profile);

// Reports a failure to the M2 log (DAT_1430a12c8, FUN_14025b4a0).
void M2InitLog(const char* format, ...);

#endif
