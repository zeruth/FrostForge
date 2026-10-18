#include "base/ObjectAlloc.hpp"

#include <intrin.h>

#include <cstdlib>
#include <cstring>
#include <new>

#include "os/Timer.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"

#define OBJECTALLOC_FILE "Engine/Source/ObjectAlloc/ObjectAlloc.cpp"
#define OBJECTALLOCBASE_FILE "Engine/Source/ObjectAlloc/ObjectAllocBase.cpp"
#define IOBJECTALLOC_FILE "Engine/Source/ObjectAlloc/IObjectAlloc.h"

namespace {

CObjectHeapListArray* s_heapLists;  // 0x1430a1c38
OSLAZYCRITSECT s_heapListLock;      // 0x1430a1c40

// The registry of allocator instances: blz::list<TObjectAllocBase*> (0x18 sentinel).
struct ALLOCNODE {
    ALLOCNODE* m_next;
    ALLOCNODE* m_prev;
    void* m_allocator;
};
ALLOCNODE* s_allocators;            // 0x1430a1ca0
OSLAZYCRITSECT s_allocatorLock;     // 0x1430a1c70

// The granularity rule TSGrowableArray uses: below the limit, the highest set bit of the
// count; at or above it, the limit itself (which is then kept).
uint32_t GrowableGranularity(uint32_t* granularity, uint32_t count, uint32_t limit) {
    uint32_t value = *granularity;
    if (value == 0) {
        if (count < limit) {
            value = count;
            for (uint32_t low = (count - 1) & count; low != 0; low = (low - 1) & low) {
                value = low;
            }
            if (value == 0) {
                value = 1;
            }
        } else {
            *granularity = limit;
            value = limit;
        }
    }
    return value;
}

// TSFixedArray<CObjectHeap, 8>::~TSFixedArray: every block, then the array.
// ref: FUN_14123f8f0
void HeapsFree(CObjectHeapList* list) {
    for (uint32_t i = 0; i < list->m_count; i++) {
        SMemFree(list->m_heaps[i].m_objects, IOBJECTALLOC_FILE, 0x26, 0);
    }
    if (list->m_heaps != nullptr) {
        SMemFree(list->m_heaps, ".?AV?$TSFixedArray@VCObjectHeap@@$07@@", -2, 0);
    }
}

// TSFixedArray<CObjectHeapList, 8>::~TSFixedArray.
// ref: FUN_14123f970
void HeapListsFree(CObjectHeapListArray* array) {
    for (uint32_t i = 0; i < array->m_count; i++) {
        HeapsFree(&array->m_lists[i]);
    }
    if (array->m_lists != nullptr) {
        SMemFree(array->m_lists, ".?AV?$TSFixedArray@VCObjectHeapList@@$07@@", -2, 0);
    }
}

// TSGrowableArray<CObjectHeap, 8>: room for `count` more (the count itself is the caller's).
// ref: FUN_1412408d0
void HeapsReserve(CObjectHeapList* list, uint32_t count, const char* tag) {
    uint32_t total = list->m_count + count;
    if (list->m_capacity >= total) {
        return;
    }
    uint32_t granularity = GrowableGranularity(&list->m_granularity, total, 10);
    if (total % granularity != 0) {
        total += granularity - total % granularity;
    }
    CObjectHeap* old = list->m_heaps;
    for (uint32_t i = total; i < list->m_count; i++) {
        SMemFree(old[i].m_objects, IOBJECTALLOC_FILE, 0x26, 0);
    }
    list->m_capacity = total;
    list->m_heaps = nullptr;
    if (total != 0) {
        list->m_heaps = static_cast<CObjectHeap*>(SMemAlloc(uint64_t(total) * sizeof(CObjectHeap), 8, tag, 0, -7, 0));
    }
    if (old != nullptr) {
        uint32_t keep = list->m_count <= total ? list->m_count : total;
        for (uint32_t i = 0; i < keep; i++) {
            list->m_heaps[i] = old[i];
            old[i].m_objects = nullptr;
            old[i].m_freeIndices = nullptr;
            SMemFree(nullptr, IOBJECTALLOC_FILE, 0x26, 0);
        }
        SMemFree(old, ".?AV?$TSGrowableArray@VCObjectHeap@@$07@@", -2, 0);
    }
}

// TSGrowableArray<CObjectHeapList, 8>: reallocates to `capacity` lists, moving each one.
// ref: FUN_1412406c0
void HeapListsRealloc(CObjectHeapListArray* array, uint32_t capacity, const char* tag, const char* allocTag) {
    CObjectHeapList* old = array->m_lists;
    for (uint32_t i = capacity; i < array->m_count; i++) {
        HeapsFree(&old[i]);
    }
    array->m_capacity = capacity;
    array->m_lists = nullptr;
    if (capacity != 0) {
        array->m_lists = static_cast<CObjectHeapList*>(SMemAlloc(uint64_t(capacity) * sizeof(CObjectHeapList), 8,
            allocTag, 0, -7, 0));
    }
    if (old == nullptr) {
        return;
    }
    uint32_t keep = array->m_count <= capacity ? array->m_count : capacity;
    for (uint32_t i = 0; i < keep; i++) {
        CObjectHeapList* from = &old[i];
        CObjectHeapList* to = &array->m_lists[i];
        to->m_count = 0;
        to->m_heaps = nullptr;
        to->m_capacity = 0;
        uint32_t heaps = from->m_count;
        if (heaps != 0) {
            to->m_capacity = heaps;
            to->m_heaps = static_cast<CObjectHeap*>(SMemReAlloc(nullptr, uint64_t(heaps) * sizeof(CObjectHeap), 8,
                allocTag, 0, -7, 0));
            for (uint32_t h = 0; h < heaps; h++) {
                to->m_heaps[h] = from->m_heaps[h];
                from->m_heaps[h].m_objects = nullptr;
                from->m_heaps[h].m_freeIndices = nullptr;
            }
        }
        to->m_count = heaps;
        to->m_granularity = from->m_granularity;
        to->m_objSize = from->m_objSize;
        to->m_objsPerHeap = from->m_objsPerHeap;
        to->m_fullHeaps = from->m_fullHeaps;
        to->m_frees = from->m_frees;
        to->m_current = from->m_current;
        to->m_align16 = from->m_align16;
        to->m_hasEmpty = from->m_hasEmpty;
        to->m_warned = from->m_warned;
        HeapsFree(from);
    }
    SMemFree(old, tag, -2, 0);
}

void ObjectAllocShutdown();

// The global table, created on first use. Returns with the table's lock held.
// ref: FUN_14123fe30
CObjectHeapListArray* HeapListsLock() {
    OsLazyCritSectEnter(&s_heapListLock);
    if (s_heapLists == nullptr) {
        auto array = static_cast<CObjectHeapListArray*>(SMemAlloc(sizeof(CObjectHeapListArray), 0x1a,
            OBJECTALLOC_FILE, SStrHashFnv1a(OBJECTALLOC_FILE), 0x23f, 0));
        s_heapLists = array;
        array->m_count = 0;
        array->m_lists = nullptr;
        array->m_capacity = 0;
        array->m_granularity = 0;
        array->m_compact = true;
        array->m_unknown24 = 0;
        atexit(ObjectAllocShutdown);
    }
    return s_heapLists;
}

// ref: FUN_14123fee0
void ObjectAllocShutdown() {
    ObjectAllocUsage();
    OsLazyCritSectEnter(&s_heapListLock);
    CObjectHeapListArray* array = s_heapLists;
    if (array != nullptr) {
        HeapListsFree(array);
        array->m_capacity = 0;
        array->m_count = 0;
        array->m_lists = nullptr;
        for (uint32_t i = 0; i < array->m_count; i++) {
            HeapsFree(&array->m_lists[i]);
        }
        if (array->m_lists != nullptr) {
            SMemFree(array->m_lists, ".?AV?$TSFixedArray@VCObjectHeapList@@$07@@", -2, 0);
        }
        operator delete(array, sizeof(CObjectHeapListArray));
        s_heapLists = nullptr;
    }
    OsLazyCritSectLeave(&s_heapListLock);
}

}  // namespace

// ref: FUN_14123f9e0
bool CObjectHeap::Allocate(uint32_t objSize, uint32_t count, int32_t memFlags, bool align16) {
    uint32_t align = align16 ? 1 : 0;
    char* block = static_cast<char*>(SMemAlloc(uint64_t((align * 0xf + objSize) * count) + uint64_t(count) * 4,
        memFlags != 0 ? memFlags : 0x1a, OBJECTALLOC_FILE, SStrHashFnv1a(OBJECTALLOC_FILE), 0x188, 0));
    this->m_objects = block;
    if (block == nullptr) {
        return false;
    }
    this->m_freeIndices = reinterpret_cast<uint32_t*>(block + uint64_t(objSize * count) + uint64_t(align * count * 0xf));
    for (uint32_t i = 0; i < count; i++) {
        this->m_freeIndices[i] = i;
    }
    this->m_used = 0;
    return this->m_objects != nullptr;
}

// ref: FUN_14123fb80
uint32_t CObjectHeapList::CountObjects() const {
    uint32_t total = 0;
    for (uint32_t i = 0; i < this->m_count; i++) {
        total += this->m_heaps[i].m_used;
    }
    return total;
}

// ref: FUN_141240680
uint32_t CObjectHeapList::PercentUsed() const {
    uint32_t capacity = 0;
    uint32_t used = 0;
    for (uint32_t i = 0; i < this->m_count; i++) {
        if (this->m_heaps[i].m_objects != nullptr) {
            capacity += this->m_objsPerHeap;
            used += this->m_heaps[i].m_used;
        }
    }
    if (capacity != 0) {
        return used * 100 / capacity;
    }
    return 0;
}

// ref: FUN_141240a70
uint32_t CObjectHeapList::CountHeaps() const {
    uint32_t heaps = 0;
    for (uint32_t i = 0; i < this->m_count; i++) {
        if (this->m_heaps[i].m_objects != nullptr) {
            heaps++;
        }
    }
    return heaps;
}

// ref: FUN_14123fc10
void CObjectHeapList::Clear() {
    HeapsFree(this);
    this->m_capacity = 0;
    this->m_count = 0;
    this->m_heaps = nullptr;
    this->m_fullHeaps = 0;
    this->m_current = 0;
}

// ref: FUN_14123ffa0
void* CObjectHeapList::Alloc(uint32_t* index, bool zero) {
    void* object = nullptr;
    uint32_t count = this->m_count;
    CObjectHeap* heap;
    if (count <= this->m_current || (heap = &this->m_heaps[this->m_current], heap->m_used == this->m_objsPerHeap)
        || heap->m_objects == nullptr) {
        if (count == this->m_fullHeaps) {
            this->m_current = count;
            if (!this->m_warned && count > 9999) {
                this->m_warned = true;
            }
            HeapsReserve(this, 1, static_cast<const char*>(_ReturnAddress()));
            heap = &this->m_heaps[this->m_count];
            this->m_count++;
            heap->m_objects = nullptr;
            heap->m_freeIndices = nullptr;
            heap->m_used = 0;
            heap->m_emptyTime = 0;
            if (!heap->Allocate(this->m_objSize, this->m_objsPerHeap, 0, this->m_align16)) {
                return nullptr;
            }
        } else {
            heap = nullptr;
            CObjectHeap* candidate = nullptr;
            for (uint32_t i = 0; i < count; i++) {
                CObjectHeap* h = &this->m_heaps[i];
                if (h->m_used != this->m_objsPerHeap) {
                    this->m_current = i;
                    candidate = h;
                    if (h->m_objects != nullptr) {
                        break;
                    }
                }
            }
            heap = candidate;
        }
    }
    uint32_t objSize = this->m_objSize;
    bool align16 = this->m_align16;
    if (heap->m_used < this->m_objsPerHeap) {
        if (heap->m_objects == nullptr) {
            heap->Allocate(objSize, this->m_objsPerHeap, 0, align16);
        }
        uint32_t slot = heap->m_freeIndices[heap->m_used];
        heap->m_used++;
        *index = slot;
        char* p = heap->m_objects + uint64_t(slot * objSize) + uint64_t(uint32_t(align16) * slot * 0xf);
        uintptr_t raw = reinterpret_cast<uintptr_t>(p);
        object = p + (((raw + 0xf) & ~uintptr_t(0xf)) - raw) * uint32_t(align16);
        if (zero) {
            memset(object, 0, objSize);
        }
        if (object != nullptr) {
            *index += this->m_current * this->m_objsPerHeap;
            if (heap->m_used == this->m_objsPerHeap) {
                this->m_fullHeaps++;
            }
        }
    }
    return object;
}

// ref: FUN_14123fc40
void CObjectHeapList::Free(uint32_t index, bool compact) {
    uint32_t per = this->m_objsPerHeap;
    CObjectHeap* heap = &this->m_heaps[index / per];
    if (heap->m_used == per) {
        this->m_fullHeaps--;
    }
    if (heap->m_used != 0 && heap->m_freeIndices != nullptr) {
        heap->m_used--;
        heap->m_freeIndices[heap->m_used] = index % per;
        if (heap->m_used == 0) {
            heap->m_emptyTime = OsGetTickCount();
        }
    }
    if (heap->m_objects != nullptr && heap->m_used == 0) {
        this->m_hasEmpty = true;
    }
    if (compact && this->m_hasEmpty) {
        uint32_t frees = this->m_frees++;
        if ((frees & ((this->m_objsPerHeap - 1) >> 2)) == 0) {
            this->Compact();
        }
    }
}

// Releases empty blocks at the end of the list that have been empty for five seconds, while
// more than one and a half blocks' worth of room is spare; gives up after two seconds.
// ref: FUN_14123fd00
void CObjectHeapList::Compact() {
    uint32_t count = this->m_count;
    if (count != 0) {
        uint32_t capacity = 0;
        int32_t used = 0;
        int32_t empty = 0;
        for (uint32_t i = 0; i < count; i++) {
            CObjectHeap* h = &this->m_heaps[i];
            if (h->m_objects != nullptr) {
                capacity += this->m_objsPerHeap;
                if (h->m_used == 0) {
                    empty++;
                } else {
                    used += h->m_used;
                }
            }
        }
        if (empty != 0) {
            uint32_t keep = (this->m_objsPerHeap * 3 >> 1) + used;
            if (keep < capacity) {
                int32_t now = static_cast<int32_t>(OsGetTickCount());
                while (keep < capacity && empty != 0) {
                    uint32_t last = count - 1;
                    CObjectHeap* h = &this->m_heaps[last];
                    if (h->m_objects != nullptr && h->m_used == 0
                        && static_cast<uint32_t>(now - static_cast<int32_t>(h->m_emptyTime)) > 4999) {
                        SMemFree(h->m_objects, OBJECTALLOC_FILE, 0x1a2, 0);
                        h->m_objects = nullptr;
                        capacity -= this->m_objsPerHeap;
                        empty--;
                        this->m_frees = 0;
                    }
                    if ((count & 0xf) == 0 && static_cast<int32_t>(OsGetTickCount()) + (-2000 - now) > 0) {
                        break;
                    }
                    count = last;
                    if (last == 0) {
                        break;
                    }
                }
            }
            this->m_hasEmpty = empty != 0;
            return;
        }
    }
    this->m_hasEmpty = false;
}

// ref: FUN_1412401e0
uint32_t ObjectAllocAddHeap(uint32_t objSize, uint32_t objsPerHeap, const char* name, int32_t memFlags, bool align16) {
    (void)name;
    (void)memFlags;
    CObjectHeapListArray* array = HeapListsLock();
    uint32_t id = array->m_count;
    uint32_t total = id + 1;
    if (array->m_capacity < total) {
        uint32_t granularity = GrowableGranularity(&array->m_granularity, total, 8);
        if (total % granularity != 0) {
            total += granularity - total % granularity;
        }
        HeapListsRealloc(array, total, ".?AV?$TSGrowableArray@VCObjectHeapList@@$07@@",
            static_cast<const char*>(_ReturnAddress()));
    }
    CObjectHeapList* list = &array->m_lists[array->m_count];
    array->m_count++;
    list->m_align16 = align16;
    list->m_count = 0;
    list->m_heaps = nullptr;
    list->m_capacity = 0;
    list->m_granularity = 0;
    list->m_fullHeaps = 0;
    list->m_frees = 0;
    list->m_current = 0;
    list->m_hasEmpty = false;
    list->m_warned = false;
    list->m_objsPerHeap = objsPerHeap;
    list->m_objSize = (((objSize + 0xf) & ~0xfu) - objSize) * uint32_t(align16) + objSize;
    OsLazyCritSectLeave(&s_heapListLock);
    return id;
}

// ref: FUN_1412402f0
void ObjectAllocDestroyHeap(uint32_t heapId) {
    CObjectHeapListArray* array = HeapListsLock();
    CObjectHeapList* list = &array->m_lists[heapId];
    HeapsFree(list);
    list->m_capacity = 0;
    list->m_count = 0;
    list->m_heaps = nullptr;
    list->m_fullHeaps = 0;
    list->m_current = 0;
    OsLazyCritSectLeave(&s_heapListLock);
}

// ref: FUN_141240170
void* ObjectAlloc(uint32_t heapId, uint32_t* index, bool zero) {
    *index = 0;
    CObjectHeapListArray* array = HeapListsLock();
    uint32_t local;
    void* object = array->m_lists[heapId].Alloc(&local, zero);
    OsLazyCritSectLeave(&s_heapListLock);
    if (object != nullptr) {
        *index = local;
    }
    return object;
}

// ref: FUN_141240330
void ObjectAllocSetCompact(bool compact) {
    CObjectHeapListArray* array = HeapListsLock();
    array->m_compact = compact;
    OsLazyCritSectLeave(&s_heapListLock);
}

// ref: FUN_141240360
void ObjectFree(uint32_t heapId, uint32_t index) {
    CObjectHeapListArray* array = HeapListsLock();
    bool compact = array->m_compact;
    array->m_lists[heapId].Free(index, compact);
    OsLazyCritSectLeave(&s_heapListLock);
}

// The usage report: per heap, its blocks, size and fill, then the totals. The output goes to a
// debug print compiled out of this build (FUN_140255c60), so only the walk remains.
// ref: FUN_141240440
void ObjectAllocUsage() {
    CObjectHeapListArray* array = HeapListsLock();
    for (uint32_t i = 0; i < array->m_count; i++) {
        CObjectHeapList* list = &array->m_lists[i];
        (void)list->PercentUsed();
        (void)list->CountObjects();
        (void)list->CountHeaps();
    }
    OsLazyCritSectLeave(&s_heapListLock);
}

// ref: FUN_141240aa0
CObjectAllocLocked::CObjectAllocLocked() {
    this->m_initialized = false;
    this->m_compact = true;
    this->m_objSize = 0;
    this->m_heapList = nullptr;
    this->m_userData = nullptr;
}

// ref: FUN_141240c70
void CObjectAllocLocked::Init(const char* name, uint32_t objSize, uint32_t alignment, uint32_t objsPerHeap,
    int32_t memFlags, bool align16, void* userData) {
    (void)name;
    uint32_t align = alignment > 3 ? alignment : 4;
    this->m_objSize = (objSize + 3 + align) & (0u - align);
    auto list = static_cast<CObjectHeapList*>(SMemAlloc(sizeof(CObjectHeapList), memFlags, OBJECTALLOCBASE_FILE,
        SStrHashFnv1a(OBJECTALLOCBASE_FILE), 0x11b, 0));
    list->m_count = 0;
    list->m_heaps = nullptr;
    list->m_capacity = 0;
    list->m_granularity = 0;
    list->m_objSize = 0;
    list->m_objsPerHeap = 0x80;
    list->m_fullHeaps = 0;
    list->m_frees = 0;
    list->m_current = 0;
    list->m_align16 = false;
    list->m_hasEmpty = false;
    list->m_warned = false;
    uint32_t size = this->m_objSize;
    this->m_heapList = list;
    list->m_objSize = (((size + 0xf) & ~0xfu) - size) * uint32_t(list->m_align16) + size;
    this->m_heapList->m_objsPerHeap = objsPerHeap;
    CObjectHeapList* heapList = this->m_heapList;
    uint32_t current = heapList->m_objSize;
    heapList->m_align16 = align16;
    heapList->m_objSize = (((current + 0xf) & ~0xfu) - current) * uint32_t(align16) + current;
    this->m_userData = userData;
    OsMutexInit(&this->m_lock);
    this->m_initialized = true;
}

// ref: FUN_141240af0
void CObjectAllocLocked::Destroy() {
    if (!this->m_initialized) {
        return;
    }
    // the usage line goes to a debug print compiled out of this build
    (void)this->m_heapList->PercentUsed();
    (void)this->m_heapList->CountObjects();
    (void)this->m_heapList->CountHeaps();
    this->m_heapList->Clear();
    CObjectHeapList* list = this->m_heapList;
    if (list != nullptr) {
        HeapsFree(list);
        operator delete(list, sizeof(CObjectHeapList));
    }
    this->m_heapList = nullptr;
}

// ref: FUN_141240bb0
void* CObjectAllocLocked::Alloc(bool zero) {
    OsMutexLock(&this->m_lock);
    uint32_t index;
    void* object = this->m_heapList->Alloc(&index, zero);
    OsMutexUnlock(&this->m_lock);
    if (object != nullptr) {
        *reinterpret_cast<uint32_t*>(static_cast<char*>(object) + this->m_objSize - 4) = index;
        return object;
    }
    return nullptr;
}

// ref: FUN_141240c20
void CObjectAllocLocked::Free(void* object) {
    uint32_t index = *reinterpret_cast<uint32_t*>(static_cast<char*>(object) + this->m_objSize - 4);
    OsMutexLock(&this->m_lock);
    this->m_heapList->Free(index, this->m_compact);
    OsMutexUnlock(&this->m_lock);
}

// ref: FUN_141240d80
void ObjectAllocRegister(void* allocator) {
    OsLazyCritSectEnter(&s_allocatorLock);
    ALLOCNODE* list = s_allocators;
    if (list == nullptr) {
        list = static_cast<ALLOCNODE*>(SMemAlloc(sizeof(ALLOCNODE), 0x1a, OBJECTALLOCBASE_FILE,
            SStrHashFnv1a(OBJECTALLOCBASE_FILE), 0x15, 0));
        s_allocators = list;
        list->m_next = list;
        list->m_prev = list;
        list->m_allocator = nullptr;
    }
    const char* tag = ".?AU?$list_node@PEAVTObjectAllocBase@@@blz@@";
    auto node = static_cast<ALLOCNODE*>(SMemAlloc(sizeof(ALLOCNODE), 0xb, tag, SStrHashFnv1a(tag), -6, 0));
    if (node != nullptr) {
        node->m_allocator = allocator;
    }
    // the sentinel's m_allocator slot doubles as the list's size
    ALLOCNODE* tail = list->m_prev;
    node->m_next = tail->m_next;
    node->m_prev = tail;
    tail->m_next = node;
    node->m_next->m_prev = node;
    list->m_allocator = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(list->m_allocator) + 1);
    OsLazyCritSectLeave(&s_allocatorLock);
}

// ref: FUN_141240ea0
void ObjectAllocUnregister(void* allocator) {
    OsLazyCritSectEnter(&s_allocatorLock);
    ALLOCNODE* list = s_allocators;
    const char* tag = ".?AU?$list_node@PEAVTObjectAllocBase@@@blz@@";
    for (ALLOCNODE* node = list->m_prev; node != list;) {
        ALLOCNODE* prev = node->m_prev;
        if (node->m_allocator == allocator) {
            node->m_next->m_prev = prev;
            prev->m_next = node->m_next;
            SMemFree(node, tag, -6, 0);
            list->m_allocator = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(list->m_allocator) - 1);
        }
        node = prev;
    }
    list = s_allocators;
    if (list->m_allocator == nullptr) {
        ALLOCNODE* node = list->m_prev;
        list->m_prev = list;
        list->m_next = list;
        list->m_allocator = nullptr;
        while (node != list) {
            ALLOCNODE* prev = node->m_prev;
            SMemFree(node, tag, -6, 0);
            node = prev;
        }
        operator delete(list, sizeof(ALLOCNODE));
        s_allocators = nullptr;
    }
    OsLazyCritSectLeave(&s_allocatorLock);
}

// ref: FUN_141240d80 (the base constructor, inlined at each typed allocator's creation)
TObjectAllocBase::TObjectAllocBase() {
    ObjectAllocRegister(this);
}

// ref: FUN_1402deb10
TObjectAllocBase::~TObjectAllocBase() {
    ObjectAllocUnregister(this);
}

TObjectAllocHeap::TObjectAllocHeap(const char* name, uint32_t objSize, uint32_t objsPerHeap, int32_t memFlags) {
    this->m_name = name;
    this->m_userData = nullptr;
    this->m_objSize = objSize;
    this->m_heapId = static_cast<int32_t>(ObjectAllocAddHeap(objSize, objsPerHeap, name, memFlags, false));
}

// ref: FUN_1402deab0
TObjectAllocHeap::~TObjectAllocHeap() {
    if (this->m_heapId != -1) {
        ObjectAllocDestroyHeap(static_cast<uint32_t>(this->m_heapId));
    }
}

// The allocation the users inline: the index goes in the object's last 4 bytes.
void* TObjectAllocHeap::Alloc() {
    uint32_t index;
    void* object = ObjectAlloc(static_cast<uint32_t>(this->m_heapId), &index, false);
    if (object == nullptr) {
        return nullptr;
    }
    *reinterpret_cast<uint32_t*>(static_cast<char*>(object) + this->m_objSize - 4) = index;
    _InterlockedIncrement64(&this->m_live);
    _InterlockedIncrement64(&this->m_total);
    return object;
}

void TObjectAllocHeap::Free(void* object) {
    _InterlockedDecrement64(&this->m_live);
    ObjectFree(static_cast<uint32_t>(this->m_heapId),
        *reinterpret_cast<uint32_t*>(static_cast<char*>(object) + this->m_objSize - 4));
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_1430a1c38 = (void*)&s_heapLists;
extern "C" void* const gen_p_1430a1c40 = (void*)&s_heapListLock;
// ---- end of generated globals ----
