#include "blz/ObjectPool.hpp"
#include <algorithm>

namespace blz {

namespace {

POOLPAGE* ListHead(POOLPAGE** head) {
    return reinterpret_cast<POOLPAGE*>(head);
}

void Unlink(POOLPAGE* page) {
    page->m_prev->m_next = page->m_next;
    page->m_next->m_prev = page->m_prev;
    page->m_prev = page;
    page->m_next = page;
}

void LinkTail(POOLPAGE** head, POOLPAGE* page) {
    POOLPAGE* list = ListHead(head);
    page->m_prev = head[1];
    page->m_next = list;
    head[1] = page;
    page->m_prev->m_next = page;
}

bool IsEmptyList(POOLPAGE** head) {
    return head[0] == ListHead(head);
}

}

// ref: FUN_142364b80
void object_pool::Init(allocator* allocator, uint32_t elementSize, uint32_t alignment, uint32_t elementsPerPage,
    uint32_t maxFreePages) {
    this->m_elementsPerPage = elementsPerPage;
    this->m_partial[0] = this->m_partial[1] = ListHead(this->m_partial);
    this->m_free[0] = this->m_free[1] = ListHead(this->m_free);
    this->m_allocator = allocator;
    this->m_elementSize = elementSize;
    this->m_alignment = alignment;
    for (void*& slot : this->m_recent) {
        slot = nullptr;
    }
    this->m_sorted = nullptr;
    this->m_sortedCount = 0;
    this->m_sortedCapacity = 0;
    this->m_allocated = 0;
    this->m_freeSlots = 0;
    this->m_freePages = 0;
    this->m_maxFreePages = maxFreePages;
}

// Releases every wholly free page and the page array (pages still in use stay allocated).
// ref: FUN_142364bf0
void object_pool::Destroy() {
    this->m_maxFreePages = 0;
    this->Compact();
    if (this->m_sorted) {
        this->m_allocator->deallocate(this->m_sorted);
        this->m_sorted = nullptr;
    }
    this->m_sortedCapacity = 0;
}

// ref: FUN_142365170
POOLPAGE* object_pool::NewPage() {
    uint32_t bytes = this->m_elementSize * this->m_elementsPerPage;
    auto base = static_cast<uint8_t*>(this->m_allocator->allocate(uint64_t(bytes) + sizeof(POOLPAGE), this->m_alignment));
    POOLPAGE* page = nullptr;
    if (base + bytes) {
        page = reinterpret_cast<POOLPAGE*>(base + bytes);
        page->m_base = base;
        page->m_next = page;
        page->m_freeCount = bytes / this->m_elementSize;
        page->m_prev = page;
        page->m_freeList = nullptr;
        page->m_bytes = bytes;
        page->m_elementSize = this->m_elementSize;
        page->m_tracked = 0;
    }
    this->m_freeSlots += static_cast<int32_t>(this->m_elementsPerPage);
    return page;
}

// ref: FUN_1423651f0
void* object_pool::Allocate() {
    POOLPAGE* page = IsEmptyList(this->m_partial) ? nullptr : this->m_partial[0];
    if (!page) {
        if (IsEmptyList(this->m_free)) {
            page = this->NewPage();
        } else {
            page = this->m_free[0];
            Unlink(page);
            this->m_freePages--;
        }
        if (!page->m_tracked) {
            bool cached = false;
            for (void*& slot : this->m_recent) {
                if (!slot) {
                    slot = page->m_base;
                    cached = true;
                    break;
                }
            }
            if (!cached) {
                this->Compact();
                this->m_recent[0] = page->m_base;
            }
        }
        if (page->m_freeCount != 1) {
            LinkTail(this->m_partial, page);
        }
    } else if (page->m_freeCount == 1) {
        Unlink(page);
    }
    this->m_allocated++;
    this->m_freeSlots--;

    auto object = static_cast<void**>(page->m_freeList);
    if (!object) {
        auto p = static_cast<uint8_t*>(page->m_base);
        if (page->m_freeCount != 1) {
            for (uint32_t i = 0; i < page->m_freeCount - 1; i++) {
                *reinterpret_cast<uint8_t**>(p) = p + page->m_elementSize;
                p += page->m_elementSize;
            }
        }
        object = static_cast<void**>(page->m_base);
        page->m_freeList = object;
    }
    page->m_freeCount--;
    page->m_freeList = *object;
    return object;
}

// Puts an object back on its page's free list; a page that becomes wholly free moves to the
// free list (and may trigger a compaction) and gives null.
// ref: FUN_142364c40
POOLPAGE* object_pool::ReturnToPage(void* object) {
    uint32_t bytes = this->m_elementSize * this->m_elementsPerPage;
    POOLPAGE* page = nullptr;
    for (void*& slot : this->m_recent) {
        auto base = static_cast<uint8_t*>(slot);
        if (base && base <= object && object < base + bytes) {
            page = this->PageOf(base);
            if (page->m_freeCount == page->m_bytes / page->m_elementSize - 1) {
                slot = nullptr;
            }
        }
    }
    if (!page) {
        void** first = this->m_sorted;
        void** last = this->m_sorted + this->m_sortedCount;
        void** found = std::lower_bound(first, last, object);
        if (found == last || object < *found || static_cast<uint8_t*>(*found) + bytes <= object) {
            found--;
        }
        page = this->PageOf(*found);
    }

    *static_cast<void**>(object) = page->m_freeList;
    uint32_t total = page->m_bytes;
    uint32_t size = page->m_elementSize;
    page->m_freeCount++;
    page->m_freeList = object;
    if (page->m_freeCount == total / size) {
        Unlink(page);
        LinkTail(this->m_free, page);
        this->m_freePages++;
        if (this->m_maxFreePages < this->m_freePages) {
            this->Compact();
        }
        return nullptr;
    }
    return page;
}

// ref: FUN_142365360
void object_pool::Free(void* object) {
    POOLPAGE* page = this->ReturnToPage(object);
    this->m_freeSlots++;
    this->m_allocated--;
    if (!page) {
        return;
    }
    if (page->m_freeCount != 1) {
        POOLPAGE* head = IsEmptyList(this->m_partial) ? nullptr : this->m_partial[0];
        if (!head || page == head || head->m_freeCount <= page->m_freeCount) {
            return;
        }
    }
    Unlink(page);
    LinkTail(this->m_partial, page);
}

// Folds the recent-page cache into the sorted array, drops wholly free pages from it, and
// releases free pages beyond the limit.
// ref: FUN_142364f00
void object_pool::Compact() {
    uint32_t count = 0;
    for (void*& slot : this->m_recent) {
        if (slot) {
            POOLPAGE* page = this->PageOf(slot);
            if (page->m_freeCount == page->m_bytes / page->m_elementSize) {
                slot = nullptr;
            } else {
                page->m_tracked = 1;
                count++;
            }
        }
    }
    // FUN_1413b36d0, the library introsort, over the 8 cache slots
    std::sort(std::begin(this->m_recent), std::end(this->m_recent));

    uint32_t kept = 0;
    for (uint32_t i = 0; i < this->m_sortedCount; i++) {
        POOLPAGE* page = this->PageOf(this->m_sorted[i]);
        if (page->m_freeCount == page->m_bytes / page->m_elementSize) {
            page->m_tracked = 0;
            this->m_sorted[i] = nullptr;
        } else {
            kept++;
        }
    }
    uint32_t total = count + kept;
    uint32_t capacity = (total + 7) & ~7u;
    void** merged = capacity ? static_cast<void**>(this->m_allocator->allocate(uint64_t(capacity) * 8, 0x10)) : nullptr;

    void** out = merged;
    void** recent = std::begin(this->m_recent);
    void** recentEnd = std::end(this->m_recent);
    while (recent != recentEnd && !*recent) {
        recent++;
    }
    void** sorted = this->m_sorted;
    void** sortedEnd = this->m_sorted + this->m_sortedCount;
    for (;;) {
        while (sorted != sortedEnd && !*sorted) {
            sorted++;
        }
        if (sorted == sortedEnd) {
            for (; recent < recentEnd; recent++) {
                *out++ = *recent;
                *recent = nullptr;
            }
            break;
        }
        if (recent == recentEnd) {
            *out++ = *sorted++;
            continue;
        }
        if (*recent < *sorted) {
            *out++ = *recent;
            *recent = nullptr;
            recent++;
        } else {
            *out++ = *sorted++;
        }
    }

    if (this->m_sorted) {
        this->m_allocator->deallocate(this->m_sorted);
    }
    this->m_sorted = merged;
    this->m_sortedCount = total;
    this->m_sortedCapacity = capacity;
    while (this->m_maxFreePages < this->m_freePages) {
        POOLPAGE* page = IsEmptyList(this->m_free) ? nullptr : this->m_free[0];
        if (page) {
            Unlink(page);
        }
        this->m_freePages--;
        this->m_allocator->deallocate(page->m_base);
        this->m_freeSlots -= static_cast<int32_t>(this->m_elementsPerPage);
    }
}

}
