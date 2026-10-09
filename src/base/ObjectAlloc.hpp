#ifndef BASE_OBJECT_ALLOC_HPP
#define BASE_OBJECT_ALLOC_HPP

// ObjectAlloc (Engine/Source/ObjectAlloc): fixed-size object heaps. A heap list hands out
// objects from blocks of `objsPerHeap`, each block with a stack of free slot indices; an
// object is named by its global index (block * objsPerHeap + slot). Lists live in one global
// table addressed by heap id, or in a CObjectAllocLocked that owns its list and lock.

#include <cstdint>

#include "os/Sync.hpp"

// One block of objects (0x18).
struct CObjectHeap {
    char* m_objects;                // 0x00
    uint32_t* m_freeIndices;        // 0x08 slot indices, the first m_used are taken
    uint32_t m_used;                // 0x10
    uint32_t m_emptyTime;           // 0x14 when it last became empty (ms)

    bool Allocate(uint32_t objSize, uint32_t count, int32_t memFlags, bool align16);
};
static_assert(sizeof(CObjectHeap) == 0x18, "CObjectHeap is 0x18 bytes");

// TSGrowableArray<CObjectHeap, 8> plus the list's settings (0x38).
struct CObjectHeapList {
    uint32_t m_count;               // 0x00
    uint32_t m_pad04;
    CObjectHeap* m_heaps;           // 0x08
    uint32_t m_capacity;            // 0x10
    uint32_t m_pad14;
    uint32_t m_granularity;         // 0x18
    uint32_t m_pad1C;
    uint32_t m_objSize;             // 0x20 including alignment padding
    uint32_t m_objsPerHeap;         // 0x24
    uint32_t m_fullHeaps;           // 0x28
    uint32_t m_frees;               // 0x2C counts frees between compactions
    uint32_t m_current;             // 0x30 the heap allocations come from
    bool m_align16;                 // 0x34
    bool m_hasEmpty;                // 0x35
    bool m_warned;                  // 0x36 past 9999 heaps

    void* Alloc(uint32_t* index, bool zero);
    void Free(uint32_t index, bool compact);
    void Compact();
    void Clear();
    uint32_t CountObjects() const;
    uint32_t PercentUsed() const;
    uint32_t CountHeaps() const;
};
static_assert(sizeof(CObjectHeapList) == 0x38, "CObjectHeapList is 0x38 bytes");

// The global table of heap lists (0x28).
struct CObjectHeapListArray {
    uint32_t m_count;               // 0x00
    uint32_t m_pad04;
    CObjectHeapList* m_lists;       // 0x08
    uint32_t m_capacity;            // 0x10
    uint32_t m_pad14;
    uint32_t m_granularity;         // 0x18
    uint32_t m_pad1C;
    bool m_compact;                 // 0x20 compact heaps as objects are freed
    uint32_t m_unknown24;           // 0x24
};
static_assert(sizeof(CObjectHeapListArray) == 0x28, "CObjectHeapListArray is 0x28 bytes");

// A new global heap list; returns its id. Objects are `objSize` rounded up to 16 when
// `align16`.
uint32_t ObjectAllocAddHeap(uint32_t objSize, uint32_t objsPerHeap, const char* name, int32_t memFlags, bool align16);
void ObjectAllocDestroyHeap(uint32_t heapId);
void* ObjectAlloc(uint32_t heapId, uint32_t* index, bool zero);
void ObjectFree(uint32_t heapId, uint32_t index);
void ObjectAllocSetCompact(bool compact);
// Prints each heap's usage to the debug output.
void ObjectAllocUsage();

// An allocator with its own heap list and lock. Each object carries its index in its last 4
// bytes (0x28).
class CObjectAllocLocked {
    public:
    uint32_t m_objSize;             // 0x00 the requested size plus the index, aligned
    CObjectHeapList* m_heapList;    // 0x08
    OSMUTEX m_lock;                 // 0x10
    bool m_initialized;             // 0x18
    bool m_compact;                 // 0x19
    void* m_userData;               // 0x20

    CObjectAllocLocked();
    void Init(const char* name, uint32_t objSize, uint32_t alignment, uint32_t objsPerHeap, int32_t memFlags,
        bool align16, void* userData);
    void Destroy();
    void* Alloc(bool zero);
    void Free(void* object);
};
static_assert(sizeof(CObjectAllocLocked) == 0x28, "CObjectAllocLocked is 0x28 bytes");

// The registry of TObjectAlloc instances (for usage dumps).
void ObjectAllocRegister(void* allocator);
void ObjectAllocUnregister(void* allocator);

// The base of the typed allocators (vtable 0x142631f88, 0x28): counted, and listed in the
// registry while it exists.
class TObjectAllocBase {
    public:
    uint32_t m_objSize = 0;         // 0x08 including the index in the last 4 bytes
    int64_t m_live = 0;             // 0x10
    int64_t m_total = 0;            // 0x18
    const char* m_name = nullptr;   // 0x20

    TObjectAllocBase();
    virtual ~TObjectAllocBase();
};
static_assert(sizeof(TObjectAllocBase) == 0x28, "TObjectAllocBase is 0x28 bytes");

// A typed allocator over one global heap (vtable 0x142631f90, 0x38). Each object carries its
// index in its last 4 bytes.
class TObjectAllocHeap : public TObjectAllocBase {
    public:
    int32_t m_heapId = -1;          // 0x28
    void* m_userData = nullptr;     // 0x30

    TObjectAllocHeap(const char* name, uint32_t objSize, uint32_t objsPerHeap, int32_t memFlags);
    ~TObjectAllocHeap() override;
    void* Alloc();
    void Free(void* object);
};
static_assert(sizeof(TObjectAllocHeap) == 0x38, "TObjectAllocHeap is 0x38 bytes");

#endif
